#!/usr/bin/env python3
"""gpu_colour_parity.py -- compare vertex-colour distributions between emulators.

    python3 gpu_colour_parity.py --class "PolyG4+semi|B+F" --samples 40

Why a distribution and not a diff
---------------------------------
Comparing one frame against one frame requires the two emulators to be at the
same instant, and nothing short of deterministic replay gets them there. An
animating effect sampled at two different phases differs everywhere, which says
nothing -- a single-frame byte diff of a live effect is a trap, and reading one
is how an earlier attempt at this went wrong.

A distribution does not care about phase. If one emulator's additive quads peak
at colour 199 and the other's at 40, that is a real difference no matter which
frame each was on. If both peak in the same place, the colours are fine and the
divergence is elsewhere.

Each side is sampled the way it can be: psx-runtime through its GP0 ring, the
oracle by walking the ordering table out of RAM, since it has no ring.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT, STP_MODES, DebugConn,
    DebugError, capture, decode_entries, dma_gpu_list_root, snapshot_ram,
    walk_ordering_table,
)

PARITY_KIND = "psx-colour-parity"


def prim_class(p):
    mode = STP_MODES.get(p.get("stp", 0), "?") if p.get("semi") else "opaque"
    return f"{p['op_name']}|{mode}"


def colours_of(prims, want):
    out = []
    for p in prims:
        if p["kind"] not in ("poly", "rect", "line", "fill"):
            continue
        if want and prim_class(p) != want:
            continue
        for c in p.get("colors") or []:
            out.append(c)
    return out


def sample_native(conn, want, samples, gap):
    """psx-runtime: the GP0 ring already holds decoded packets per frame."""
    cols, frames = [], []
    for _ in range(samples):
        try:
            span = conn.ring_span()
            d = capture(conn, frame=span["newest"], count=30000, verify_ring=False)
        except DebugError:
            time.sleep(gap)
            continue
        frames.append(d["frame"])
        cols += colours_of(d["prims"], want)
        time.sleep(gap)
    return cols, frames


def sample_oracle(conn, want, samples, gap, addr=None):
    """The oracle has no ring, so walk the ordering table out of RAM.

    Each sample is taken with the emulator parked. The root pointer and the
    list it points at must come from the same instant -- a game rebuilds its
    ordering table every frame, and a walk that straddles a rebuild yields
    colours from a display list that never existed. Those would land in the
    distribution looking exactly like real data.
    """
    cols, frames = [], []
    misses = 0
    for _ in range(samples):
        paused = False
        try:
            try:
                conn.cmd("pause")
                paused = True
            except DebugError:
                pass          # unpaused sampling is worse, not useless
            root = addr if addr is not None else dma_gpu_list_root(conn)
            if root is None:
                break
            ram = snapshot_ram(conn)
            prims = decode_entries(walk_ordering_table(ram, root))
            frames.append(conn.frame())
            cols += colours_of(prims, want)
        except DebugError:
            misses += 1
        finally:
            if paused:
                try:
                    conn.resume()
                except DebugError:
                    pass
        time.sleep(gap)
    if misses:
        print(f"  ({misses} oracle sample(s) failed to read)", file=sys.stderr)
    return cols, frames


def describe(cols, label, out=sys.stdout):
    if not cols:
        print(f"  {label}: no matching primitives sampled", file=out)
        return None
    flat = [v for c in cols for v in c]
    peak = max(flat)
    mean = statistics.mean(flat)
    srt = sorted(flat)
    p50 = srt[len(srt) // 2]
    p90 = srt[int(len(srt) * 0.9)]
    print(f"  {label:<14} {len(cols):>6} vertices   peak {peak:>3}   "
          f"mean {mean:>6.1f}   p50 {p50:>3}   p90 {p90:>3}", file=out)
    return {"vertices": len(cols), "peak": peak, "mean": round(mean, 2),
            "p50": p50, "p90": p90}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--native-port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--class", dest="want", default=None,
                    help='primitive class, e.g. "PolyG4+semi|B+F" (default: all)')
    ap.add_argument("--samples", type=int, default=20)
    ap.add_argument("--gap", type=float, default=0.25, help="seconds between samples")
    ap.add_argument("--oracle-addr", default=None,
                    help="ordering-table root on the oracle, if DMA cannot be read")
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    native = DebugConn(args.host, args.native_port, args.timeout)
    oracle = DebugConn(args.host, args.ds_port, args.timeout)
    try:
        native.frame()
    except DebugError as e:
        print(f"error: psx-runtime unreachable: {e}", file=sys.stderr)
        return 2
    try:
        oracle.frame()
    except DebugError as e:
        print(f"error: oracle unreachable: {e}", file=sys.stderr)
        return 2

    want = args.want
    print(f"sampling {args.samples}x from each"
          + (f", class {want}" if want else ", all classes"))
    ncols, nframes = sample_native(native, want, args.samples, args.gap)
    addr = int(args.oracle_addr, 0) if args.oracle_addr else None
    ocols, oframes = sample_oracle(oracle, want, args.samples, args.gap, addr)

    print(f"\nframes covered — psx-runtime {min(nframes) if nframes else '-'}.."
          f"{max(nframes) if nframes else '-'}   oracle "
          f"{min(oframes) if oframes else '-'}..{max(oframes) if oframes else '-'}")
    print("\ncolour distribution (all channels, 0..255):")
    a = describe(ncols, "psx-runtime")
    b = describe(ocols, "oracle")

    verdict = None
    if a and b:
        ratio = a["peak"] / max(1, b["peak"])
        print()
        if 0.8 <= ratio <= 1.25:
            verdict = "match"
            print("VERDICT: the colour distributions match. The game builds the "
                  "same colours on both — a visible difference is NOT coming from "
                  "vertex colour, so look at rasterisation or blending.")
        else:
            verdict = "differ"
            print(f"VERDICT: the distributions differ (peak ratio {ratio:.2f}). "
                  f"The two emulators are building different colours, so the "
                  f"divergence is upstream of the renderer.")
        print("\nNote: this is phase-independent — it does not require the two to "
              "be on the same frame — but both must be showing the same EFFECT.")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"kind": PARITY_KIND, "version": 1, "class": want,
                       "native": a, "oracle": b, "verdict": verdict}, f, indent=1)
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
