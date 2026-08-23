#!/usr/bin/env python3
"""effect_palette.py -- does the oracle build the same effect geometry we do?

    python3 tools/effect_palette.py --samples 8

The land-creation effect renders on psx-runtime as hard-edged coloured
wedges. Measured from psx-runtime's own GP0 ring, the additive Gouraud quads
that make them carry a signature that a correctly-rendering frame of the same
game does not:

    frame                  quads  colours  saturated  vertex-y span
    good  (renders right)    144        5          0          155 px
    wedge (the effect)        64      151         68          599 px

Five distinct vertex colours against 151, and geometry spanning 599 lines on
a 240-line screen. The question this tool answers is whether DuckStation,
running the same effect, builds the same thing.

Why a signature and not a diff
------------------------------
Every attempt to compare the two emulators frame-against-frame in this
investigation has foundered on phase: the effect is a fade, so two captures
taken seconds apart differ for reasons that have nothing to do with the bug,
and locking the phase needs a register match that is itself unreliable. A
signature sidesteps that entirely. "How many distinct colours do the additive
quads carry" is a property of the geometry the effect builds, not of where it
is in its fade -- the same way class counts were phase-robust while pixel
diffs were not. 5-vs-151 is not a number that drifts with phase.

psx-runtime is measured from the GP0 ring (retrospective, nothing paused).
The oracle has no ring, so it is parked on the effect's own code and its
ordering table is walked out of guest RAM -- parking on a PC that only
executes during the effect is what puts it inside the animation without
anyone timing anything.
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gpu_display_list import blend_of, walk_side  # noqa: E402
from psx_gpu_frame import (  # noqa: E402
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT, ORACLE_PAUSED_POLL_S,
    DebugConn, DebugError, OracleBreak, capture, oracle_resume,
)

KIND = "psx-effect-palette"

SATURATION = 80     # max-min channel spread that counts as "saturated"


VERT_RE = re.compile(r"\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)")


def parse_verts(v):
    """Vertices as a list of (x, y), from either prim shape.

    The GP0 ring dump carries verts as [[x, y], ...]; gpu_display_list's
    report() formats them as the string "(x,y) (x,y) ...". A tool that reads
    both sides has to accept both, and indexing the string shape as if it
    were the list shape is a crash, not a wrong answer -- which is what it
    did.
    """
    if not v:
        return []
    if isinstance(v, str):
        return [(int(a), int(b)) for a, b in VERT_RE.findall(v)]
    out = []
    for item in v:
        if isinstance(item, (list, tuple)) and len(item) >= 2:
            out.append((int(item[0]), int(item[1])))
    return out


def parse_colors(c):
    """Vertex colours as a list of (r, g, b), from either prim shape."""
    if not c:
        return []
    if isinstance(c, str):
        return [tuple(int(x) for x in m.split(","))
                for m in re.findall(r"\(([^)]*)\)", c)
                if len(m.split(",")) == 3]
    out = []
    for item in c:
        if isinstance(item, (list, tuple)) and len(item) >= 3:
            out.append(tuple(int(x) for x in item[:3]))
    return out


def additive_shaded_quads(prims):
    """The primitive class the wedges are made of.

    Matched by properties rather than by op name, because the ring dump and
    the RAM walk spell the name differently ('PolyG4+semi' vs whatever the
    walker produced) and a class that silently matches nothing would report
    a clean signature for a broken frame.
    """
    out = []
    for p in prims:
        name = p.get("op_name") or p.get("op") or ""
        if "G4" not in name:
            continue
        blend = p.get("blend") or blend_of(p)
        if blend not in ("B+F",):
            continue
        if len(parse_verts(p.get("verts"))) < 3:
            continue
        out.append(p)
    return out


def signature(prims):
    """Phase-robust description of what the effect's quads look like."""
    quads = additive_shaded_quads(prims)
    colours = collections.Counter()
    ys = []
    for q in quads:
        for c in parse_colors(q.get("colors")):
            colours[c] += 1
        for v in parse_verts(q.get("verts")):
            ys.append(v[1])
    sat = [c for c in colours if max(c) - min(c) > SATURATION]
    return {
        "quads": len(quads),
        "distinct_colours": len(colours),
        "saturated_colours": len(sat),
        "y_span": (max(ys) - min(ys)) if ys else 0,
        "top_colours": [list(c) for c, _ in colours.most_common(6)],
    }


def merge(sigs):
    """Combine per-sample signatures by taking maxima.

    Maxima, not means: a sample that caught the effect mid-build has fewer
    quads than one that caught it whole, and averaging those understates the
    frame that actually matters. This is the same reason class_census tracks
    maxima.
    """
    live = [s for s in sigs if s["quads"] > 0]
    if not live:
        return {"quads": 0, "distinct_colours": 0, "saturated_colours": 0,
                "y_span": 0, "samples_with_quads": 0, "samples": len(sigs)}
    return {
        "quads": max(s["quads"] for s in live),
        "distinct_colours": max(s["distinct_colours"] for s in live),
        "saturated_colours": max(s["saturated_colours"] for s in live),
        "y_span": max(s["y_span"] for s in live),
        "samples_with_quads": len(live),
        "samples": len(sigs),
    }


def verdict(nat, orc, colour_ratio=4.0, span_ratio=2.0):
    """Compare the two signatures.

    Ratios, not absolute thresholds: what matters is whether one side builds
    an order of magnitude more colour variety or geometry spread than the
    other, which is scale-free and does not need calibrating against a frame
    nobody has captured.
    """
    if not nat["samples_with_quads"]:
        return ("no-native-samples",
                "psx-runtime's ring held no additive shaded quads -- the "
                "effect did not play inside the scanned window.")
    if not orc["samples_with_quads"]:
        return ("no-oracle-samples",
                "the oracle never parked inside the effect, so nothing is "
                "compared. This is not evidence that its list is clean.")
    dc_n, dc_o = nat["distinct_colours"], orc["distinct_colours"]
    sp_n, sp_o = nat["y_span"], orc["y_span"]
    colour_blowup = dc_o and (dc_n / max(dc_o, 1)) >= colour_ratio
    span_blowup = sp_o and (sp_n / max(sp_o, 1)) >= span_ratio
    if colour_blowup or span_blowup:
        why = []
        if colour_blowup:
            why.append(f"{dc_n} distinct vertex colours against the oracle's "
                       f"{dc_o}")
        if span_blowup:
            why.append(f"geometry spanning {sp_n} lines against the oracle's "
                       f"{sp_o}")
        return ("native-builds-different-geometry",
                "psx-runtime BUILDS the wedges: " + "; ".join(why) +
                ". The display list already differs, so the fault is upstream "
                "of the renderer -- in the code that computes this effect.")
    if dc_o / max(dc_n, 1) >= colour_ratio:
        return ("oracle-builds-more",
                "the ORACLE builds more colour variety than psx-runtime, "
                "which is the reverse of the wedge symptom -- treat the "
                "sampling as suspect before concluding anything.")
    return ("signatures-agree",
            f"both sides build the same kind of geometry ({dc_n} vs {dc_o} "
            f"distinct colours, {sp_n} vs {sp_o} line span). The lists agree, "
            f"so the wedges are produced when this list is RASTERISED.")


def sample_native(conn, args, out=sys.stderr):
    """Signatures from the GP0 ring: retrospective, nothing paused."""
    span = conn.ring_span()
    frames = list(range(span["newest"], max(span["oldest"], span["newest"]
                                            - args.ring_frames) - 1,
                        -args.stride))
    sigs = []
    for fr in frames:
        try:
            d = capture(conn, frame=fr, count=args.count, label="native")
        except DebugError:
            continue
        s = signature(d.get("prims", []))
        if s["quads"]:
            sigs.append(s)
        if len(sigs) >= args.samples:
            break
    print(f"  psx-runtime: {len(sigs)} ring frames carried additive shaded "
          f"quads", file=out)
    return sigs


def sample_oracle(conn, args, out=sys.stderr):
    """Signatures from parked OT walks, parking on the effect's own code."""
    sigs = []
    try:
        with OracleBreak(conn, args.pc) as brk:
            if brk.cleared:
                print(f"  cleared {brk.cleared} stale breakpoint(s) left by an "
                      f"earlier run", file=out)
            for _ in range(args.samples):
                try:
                    conn.raw("pc_hit_clear")
                except DebugError:
                    pass
                oracle_resume(conn)
                time.sleep(ORACLE_PAUSED_POLL_S)
                try:
                    hit = conn.cmd("pc_hit_last")
                except DebugError:
                    continue
                if not hit.get("valid"):
                    continue
                with open(os.devnull, "w") as quiet:
                    rep, _meta = walk_side(conn, "oracle", pause=False,
                                           max_nodes=args.max_nodes, out=quiet)
                if not rep:
                    continue
                sig = signature(rep.get("prims") or [])
                if sig["quads"]:
                    sigs.append(sig)
    except DebugError as e:
        print(f"  oracle: {e}", file=out)
    print(f"  oracle: {len(sigs)} parked walks carried additive shaded quads",
          file=out)
    return sigs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--pc", default="0x8006844C",
                    help="a PC that only executes during the effect; the "
                         "oracle is parked here to land inside the animation")
    ap.add_argument("--samples", type=int, default=8)
    ap.add_argument("--ring-frames", type=int, default=600,
                    help="how far back through psx-runtime's ring to look")
    ap.add_argument("--stride", type=int, default=4)
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--max-nodes", type=int, default=8192)
    ap.add_argument("--out", default="analysis/frames/effect_palette.json")
    args = ap.parse_args()

    doc = {"kind": KIND, "version": 1, "pc": args.pc}
    print("sampling psx-runtime's GP0 ring …", flush=True)
    nat_sigs = sample_native(DebugConn(args.host, args.port, args.timeout),
                             args)
    print("parking the oracle inside the effect …", flush=True)
    orc_sigs = sample_oracle(DebugConn(args.host, args.ds_port, args.timeout),
                             args)

    nat, orc = merge(nat_sigs), merge(orc_sigs)
    doc["native"], doc["oracle"] = nat, orc
    doc["native_samples"], doc["oracle_samples"] = nat_sigs, orc_sigs
    v, why = verdict(nat, orc)
    doc["verdict"], doc["explanation"] = v, why

    print(f"\n{'':<12}{'quads':>7}{'colours':>9}{'saturated':>11}{'y-span':>8}"
          f"{'samples':>9}")
    for label, s in (("psx-runtime", nat), ("oracle", orc)):
        print(f"{label:<12}{s['quads']:>7}{s['distinct_colours']:>9}"
              f"{s['saturated_colours']:>11}{s['y_span']:>8}"
              f"{s['samples_with_quads']:>9}")
    print(f"\nVERDICT: {v}\n{why}")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"\nreport: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
