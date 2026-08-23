#!/usr/bin/env python3
"""wedge_watch.py -- catch the giant-wedge frame without racing the animation.

The land-creation effect renders on psx-runtime as hard-edged full-screen
wedges instead of a soft glow, but only for a moment, and the packet list at
that exact moment has never been captured. This tool watches the GP0 ring
while you replay the scene and captures automatically the first time a frame
contains what a wedge is made of: semi-transparent Gouraud triangles far
larger than the fine glow mesh ever produces (~10 px there; the wedges span
hundreds).

    python3 tools/wedge_watch.py --out analysis/frames --tag wedge

Nothing is paused, ever. Each poll reads the newest ring frame; because the
ring holds several hundred frames, a strided sweep also covers the frames
between polls, so a brief flash between two polls is still caught. On trigger
it saves <tag>.json / <tag>.summary.json / <tag>.png plus --context frames
before the hit for animation context, then exits (or keeps hunting with
--keep-going).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_NATIVE_PORT, DebugConn, DebugError, capture, save_dump, save_summary,
)

KIND = "psx-wedge-watch"


def prim_span(prim):
    """Longest axis of a primitive's bounding box, in pixels."""
    vs = prim.get("verts") or []
    if len(vs) < 3:
        return 0
    xs = [v[0] for v in vs]
    ys = [v[1] for v in vs]
    return max(max(xs) - min(xs), max(ys) - min(ys))


def wedge_prims(dump, min_span=120):
    """The untextured polygons big enough to be wedges.

    The glow mesh this effect legitimately draws is triangles of ~10 px; the
    failure renders whole-screen fans. Textured polys are excluded (the
    background tiles are legitimately large), but both flat and Gouraud and
    both opaque and semi-transparent are counted: "hard-edged solid polygons"
    could be any of them, and the vignette ring proves a legitimate frame
    holds only a handful of big untextured triangles either way.
    """
    out = []
    for p in dump.get("prims", []):
        if p.get("textured"):
            continue
        if p.get("kind") not in (None, "poly") and not p.get("verts"):
            continue
        if prim_span(p) >= min_span:
            out.append(p)
    return out


def evaluate(dump, min_span=120, min_count=10):
    """Trigger decision for one frame dump.

    `min_count` guards against the triangles a healthy frame legitimately
    draws at this size -- the vignette ring and the B-F pair over the water
    contribute 3-4 of them at 150-200 px (measured in bad.json/good.json);
    a wedge fan is dozens at once.
    """
    hits = wedge_prims(dump, min_span=min_span)
    return {
        "frame": dump.get("frame"),
        "trigger": len(hits) >= min_count,
        "wedge_count": len(hits),
        "max_span": max((prim_span(p) for p in hits), default=0),
        "sample": [
            {"op": p.get("op_name"), "stp": p.get("stp"),
             "verts": p.get("verts"), "colors": p.get("colors")}
            for p in hits[:6]
        ],
    }


def sweep_frames(last_done, span, stride):
    """Frames to inspect this poll: newest first, strided back to last_done."""
    newest, oldest = span["newest"], span["oldest"]
    lo = max(oldest, (last_done + 1) if last_done is not None else newest)
    frames = list(range(newest, lo - 1, -stride))
    if not frames:
        frames = [newest]
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--min-span", type=int, default=120,
                    help="pixels: smallest triangle that counts as a wedge")
    ap.add_argument("--min-count", type=int, default=10,
                    help="how many wedge triangles one frame needs to trigger")
    ap.add_argument("--stride", type=int, default=3,
                    help="inspect every Nth ring frame between polls")
    ap.add_argument("--poll", type=float, default=1.0, help="seconds between polls")
    ap.add_argument("--watch-secs", type=float, default=600.0,
                    help="give up after this long without a trigger")
    ap.add_argument("--context", type=int, default=3,
                    help="also capture this many frames before the hit")
    ap.add_argument("--keep-going", action="store_true",
                    help="do not exit on the first hit; capture each new burst")
    ap.add_argument("--tag", default="wedge")
    ap.add_argument("--out", default="analysis/frames")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    conn = DebugConn(args.host, args.port, timeout=args.timeout)

    doc = {"kind": KIND, "version": 1, "min_span": args.min_span,
           "min_count": args.min_count, "hits": []}
    report_path = os.path.join(args.out, f"{args.tag}_watch.json")

    def flush():
        with open(report_path, "w") as f:
            json.dump(doc, f, indent=1)

    print(f"watching for >= {args.min_count} shaded semi triangles spanning "
          f">= {args.min_span} px; replay the effect now", flush=True)

    last_done = None
    burst_end = None    # frame number after which a new hit counts as a new burst
    deadline = time.monotonic() + args.watch_secs
    while time.monotonic() < deadline:
        try:
            span = conn.ring_span()
        except DebugError as e:
            print(f"  ring unavailable ({e}); retrying", flush=True)
            time.sleep(args.poll)
            continue
        if span["total"] == 0:
            time.sleep(args.poll)
            continue

        for fr in sweep_frames(last_done, span, args.stride):
            if burst_end is not None and fr <= burst_end:
                continue
            try:
                dump = capture(conn, frame=fr, label=args.tag)
            except DebugError:
                continue          # evicted between ring_span and the dump
            ev = evaluate(dump, args.min_span, args.min_count)
            if not ev["trigger"]:
                continue

            n = len(doc["hits"])
            tag = args.tag if n == 0 else f"{args.tag}{n + 1}"
            base = os.path.join(args.out, tag)
            save_dump(dump, base + ".json")
            save_summary(dump, base + ".summary.json",
                         dump_name=os.path.basename(base + ".json"))
            shot = None
            try:
                conn.screenshot(base + ".png")
                shot = base + ".png"
            except DebugError:
                pass
            ctx = []
            for k in range(1, args.context + 1):
                try:
                    cd = capture(conn, frame=fr - k, label=f"{tag}-minus{k}")
                    cb = os.path.join(args.out, f"{tag}-minus{k}")
                    save_dump(cd, cb + ".json")
                    ctx.append(cb + ".json")
                except DebugError:
                    break
            hit = dict(ev)
            hit.update({"dump": base + ".json",
                        "summary": base + ".summary.json",
                        "screenshot": shot,
                        "screenshot_note": "taken at capture time, which is "
                        "after the triggering frame; trust the dump, not the "
                        "png, for what frame " + str(fr) + " drew",
                        "context_dumps": ctx})
            doc["hits"].append(hit)
            flush()
            print(f"  HIT frame {fr}: {ev['wedge_count']} wedge triangles, "
                  f"max span {ev['max_span']} px -> {base}.json", flush=True)
            if not args.keep_going:
                print(f"report: {report_path}")
                return 0
            burst_end = span["newest"]   # don't re-trigger inside this burst
            break

        last_done = span["newest"]
        time.sleep(args.poll)

    doc["timed_out"] = True
    flush()
    print(f"no wedge frame seen in {args.watch_secs:.0f}s; report: {report_path}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
