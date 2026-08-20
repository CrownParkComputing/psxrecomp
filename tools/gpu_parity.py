#!/usr/bin/env python3
"""gpu_parity.py -- frame-locked image parity between psx-runtime and DuckStation.

    python3 gpu_parity.py --frame 41230 --out analysis/frames/parity-41230

Drives both emulators to the SAME guest frame, screenshots both, and reports how
far apart the presented images are. That answers the first question any render
bug has to answer:

  images match      -> the game code executed the same on both; a divergence
                       you can see on screen is in the renderer
  images differ     -> the guest ran differently; the GP0 stream is the place
                       to look, and tools/cosim.py will bracket where

Scope, stated honestly: this is IMAGE parity, not GP0-stream parity. The
DuckStation oracle patch (tools/duckstation/psxrecomp_oracle.patch) implements
screenshot / read_vram / gpu_state / run_to_frame / step / pause, but not
gpu_frame_dump, so there is no packet stream to compare on that side. Adding one
to the patch is the obvious next step if image parity keeps saying "the streams
must differ" without saying where.

Both instances must already be running with their debug servers up, on the same
disc, from the same starting state -- typically both loaded from the same
savestate, or both run from boot with the same input script. Parity between two
runs that were not driven identically means nothing.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT, DebugConn, DebugError,
)

GPU_STATE_FIELDS = ("display_area_x", "display_area_y", "disp_w", "disp_h",
                    "da_x", "da_y", "display_enable")


def compare_images(pa, pb, out_path):
    a = Image.open(pa).convert("RGB")
    b = Image.open(pb).convert("RGB")
    note = None
    if a.size != b.size:
        note = f"size mismatch: {a.size} vs {b.size}; compared over the common region"
        w, h = min(a.width, b.width), min(a.height, b.height)
        a, b = a.crop((0, 0, w, h)), b.crop((0, 0, w, h))
    na = np.asarray(a, dtype=np.int16)
    nb = np.asarray(b, dtype=np.int16)
    d = np.abs(na - nb)
    per_px = d.max(axis=2)
    differing = int((per_px > 0).sum())
    total = int(per_px.size)

    # The PSX writes 5 bits per channel; the runtime and DuckStation both
    # expand to 8 by shifting left 3, so an exact match is achievable and a
    # 1-2 level difference is still a real difference, not rounding.
    vis = np.zeros((*per_px.shape, 3), dtype=np.uint8)
    vis[..., 0] = np.clip(per_px * 4, 0, 255)
    vis[..., 1] = np.clip(per_px, 0, 255) // 2
    Image.fromarray(vis, "RGB").save(out_path)

    ys, xs = np.nonzero(per_px)
    bbox = [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())] if differing else None
    return {
        "width": a.width, "height": a.height,
        "differing_pixels": differing,
        "total_pixels": total,
        "differing_pct": round(100.0 * differing / max(1, total), 4),
        "max_channel_delta": int(d.max()),
        "mean_abs_delta": round(float(d.mean()), 4),
        "diff_bbox": bbox,
        "note": note,
        "diff_image": out_path,
    }


def gpu_state_delta(a, b):
    out = {}
    for k in GPU_STATE_FIELDS:
        if k in a or k in b:
            av, bv = a.get(k), b.get(k)
            if av != bv:
                out[k] = {"native": av, "duckstation": bv}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--native-port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--frame", type=int, default=None,
                    help="drive BOTH to this guest frame before capturing")
    ap.add_argument("--out", default="analysis/frames/parity")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--dump-native", action="store_true",
                    help="also capture the native GP0 stream (no DuckStation counterpart)")
    args = ap.parse_args(argv)

    outdir = os.path.abspath(args.out)
    os.makedirs(outdir, exist_ok=True)
    result = {"kind": "psx-gpu-parity", "version": 1, "frame": args.frame}

    try:
        with DebugConn(args.host, args.native_port, args.timeout) as native, \
             DebugConn(args.host, args.ds_port, args.timeout) as ds:
            if args.frame is not None:
                native.run_to_frame(args.frame)
                ds.run_to_frame(args.frame)
            fa, fb = native.frame(), ds.frame()
            result["native_frame"] = fa
            result["duckstation_frame"] = fb
            if fa != fb:
                result["frame_mismatch"] = True
                print(f"warning: frames differ (native {fa}, duckstation {fb}); "
                      f"the comparison below is between DIFFERENT frames",
                      file=sys.stderr)

            pa = os.path.join(outdir, "native.png")
            pb = os.path.join(outdir, "duckstation.png")
            native.screenshot(pa)
            ds.screenshot(pb)
            result["native_image"] = pa
            result["duckstation_image"] = pb

            try:
                result["gpu_state"] = gpu_state_delta(native.cmd("gpu_state"),
                                                      ds.cmd("gpu_state"))
            except DebugError as e:
                result["gpu_state_error"] = str(e)

            if args.dump_native:
                from psx_gpu_frame import capture, save_dump
                d = capture(native, frame=fa, label="native")
                p = os.path.join(outdir, "native-gp0.json")
                save_dump(d, p)
                result["native_gp0"] = p
                result["native_gp0_note"] = (
                    "no DuckStation counterpart: the oracle patch does not "
                    "implement gpu_frame_dump")
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    result["image"] = compare_images(pa, pb, os.path.join(outdir, "diff.png"))
    with open(os.path.join(outdir, "parity.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)

    img = result["image"]
    print(f"native frame {result['native_frame']}  duckstation frame "
          f"{result['duckstation_frame']}")
    if img["note"]:
        print(f"  {img['note']}")
    print(f"  {img['differing_pixels']}/{img['total_pixels']} pixels differ "
          f"({img['differing_pct']}%), max channel delta {img['max_channel_delta']}")
    if img["diff_bbox"]:
        print(f"  differences confined to bbox {img['diff_bbox']}")
    if result.get("gpu_state"):
        print("  display state differs:")
        for k, v in result["gpu_state"].items():
            print(f"    {k}: native={v['native']} duckstation={v['duckstation']}")
    print()
    if img["differing_pixels"] == 0:
        print("VERDICT: images identical -- the guest ran the same on both. A visible "
              "render bug here is in the renderer, not the game code.")
    else:
        print("VERDICT: images differ -- the guest state or the GP0 stream diverged. "
              "Capture both sides' frames and run tools/cosim.py to bracket where.")
    print(f"wrote {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
