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
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT,
    DebugConn, DebugError, capture, oracle_clear_breaks, oracle_resume,
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
    peak = max((max(c) for c in colours), default=0)
    return {
        "quads": len(quads),
        "distinct_colours": len(colours),
        "saturated_colours": len(sat),
        "peak_channel": peak,
        "y_span": (max(ys) - min(ys)) if ys else 0,
        "top_colours": [list(c) for c, _ in colours.most_common(6)],
    }


def group_key(sig):
    """Which on-screen object a sample is of.

    Quad count and vertical span identify the object: the effect draws 64
    quads across 599 lines, the land-placement glow draws 144 across 155.
    Mixing them is not a detail -- taking maxima over both compared
    psx-runtime's EFFECT against the oracle's PLACEMENT SCREEN, which is a
    comparison of two different things that happens to produce a number.
    """
    return (sig["quads"], sig["y_span"])


def merge(sigs):
    """Combine per-sample signatures by taking maxima, per object.

    Maxima, not means: a sample that caught the effect mid-build has fewer
    quads than one that caught it whole, and averaging those understates the
    frame that actually matters. This is the same reason class_census tracks
    maxima. Grouping first keeps that from averaging across objects.
    """
    live = [s for s in sigs if s["quads"] > 0]
    groups = {}
    for sig in live:
        k = group_key(sig)
        g = groups.setdefault(k, {"quads": k[0], "y_span": k[1],
                                  "distinct_colours": 0,
                                  "saturated_colours": 0, "samples": 0,
                                  "peak_max": 0, "peak_min": None})
        g["distinct_colours"] = max(g["distinct_colours"],
                                    sig["distinct_colours"])
        g["saturated_colours"] = max(g["saturated_colours"],
                                     sig["saturated_colours"])
        peak = sig.get("peak_channel", 0)
        g["peak_max"] = max(g["peak_max"], peak)
        # The DIMMEST sample is the load-bearing one: the effect is a fade, so
        # the question is not how bright it gets but whether it ever goes out.
        g["peak_min"] = peak if g["peak_min"] is None else min(g["peak_min"],
                                                               peak)
        g["samples"] += 1
    return {"groups": groups, "samples": len(sigs),
            "samples_with_quads": len(live)}


def common_groups(nat, orc):
    """Objects both sides actually saw, largest first.

    Only these can be compared. An object one side never sampled is a gap in
    the evidence, not a difference between the emulators.
    """
    shared = set(nat["groups"]) & set(orc["groups"])
    return sorted(shared, key=lambda k: -(k[0] * max(k[1], 1)))


def verdict(nat, orc, colour_ratio=4.0):
    """Compare the two signatures, object by object.

    Ratios, not absolute thresholds: what matters is whether one side builds
    an order of magnitude more colour variety than the other, which is
    scale-free and does not need calibrating against a frame nobody has
    captured.
    """
    if not nat["samples_with_quads"]:
        return ("no-native-samples",
                "psx-runtime's ring held no additive shaded quads -- the "
                "effect did not play inside the scanned window.", None)
    if not orc["samples_with_quads"]:
        return ("no-oracle-samples",
                "no oracle read caught the effect, so nothing is compared. "
                "This is not evidence that its list is clean.", None)
    # The biggest object either side saw is the effect. If only ONE side
    # sampled it, there is nothing to compare -- falling back to a smaller
    # object and reporting agreement from it is how this tool once answered
    # "signatures-agree" using the placement glow while the effect object was
    # missing from psx-runtime entirely.
    everything = set(nat["groups"]) | set(orc["groups"])
    if everything:
        biggest = max(everything, key=lambda k: k[0] * max(k[1], 1))
        if biggest not in nat["groups"] or biggest not in orc["groups"]:
            missing = "psx-runtime" if biggest not in nat["groups"] else "the oracle"
            return ("effect-object-one-sided",
                    f"the effect object ({biggest[0]} quads spanning "
                    f"{biggest[1]} lines) was sampled only on "
                    f"{'the oracle' if missing == 'psx-runtime' else 'psx-runtime'}"
                    f"; {missing} never saw it, so there is nothing to compare. "
                    f"Replay the effect on BOTH emulators while this runs. "
                    f"Smaller objects present on both sides are NOT a "
                    f"substitute -- they are a different thing.", None)
    shared = common_groups(nat, orc)
    if not shared:
        return ("no-common-object",
                "the two sides never sampled the same object: psx-runtime saw "
                + ", ".join(f"{q} quads/{s} lines" for q, s in nat["groups"])
                + "; the oracle saw "
                + ", ".join(f"{q} quads/{s} lines" for q, s in orc["groups"])
                + ". Replay the effect on BOTH and sample again.", None)
    for k in shared:
        n, o = nat["groups"][k], orc["groups"][k]
        dn, do = n["distinct_colours"], o["distinct_colours"]
        if dn / max(do, 1) >= colour_ratio:
            extra = ""
            if n["peak_min"] is not None and o["peak_min"] is not None:
                extra = (f" Across samples psx-runtime's dimmest frame of this "
                         f"object still peaks at {n['peak_min']} while the "
                         f"oracle's reaches {o['peak_min']}: the fade never "
                         f"goes out here, which is what leaves the mesh "
                         f"visible as hard-edged quads."
                         if n["peak_min"] > 4 * max(o["peak_min"], 1) else "")
            return ("native-builds-different-geometry",
                    f"on the same object ({k[0]} quads spanning {k[1]} lines), "
                    f"psx-runtime builds {dn} distinct vertex colours against "
                    f"the oracle's {do}. The display list already differs, so "
                    f"the fault is upstream of the renderer -- in the code "
                    f"that computes this effect's colours." + extra, k)
        if do / max(dn, 1) >= colour_ratio:
            return ("oracle-builds-more",
                    f"on {k[0]} quads/{k[1]} lines the ORACLE builds {do} "
                    f"distinct colours against psx-runtime's {dn} -- the "
                    f"reverse of the wedge symptom. Treat the sampling as "
                    f"suspect before concluding anything.", k)
    k = shared[0]
    return ("signatures-agree",
            f"on {k[0]} quads/{k[1]} lines both sides build the same colour "
            f"variety ({nat['groups'][k]['distinct_colours']} vs "
            f"{orc['groups'][k]['distinct_colours']}). The lists agree, so the "
            f"wedges are produced when this list is RASTERISED.", k)


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
    """Signatures from RAM walks of a RUNNING oracle. Nothing is paused.

    DuckStation must NOT be paused for this, and every attempt to park it
    here has failed the same way. A paused DuckStation serves its debug
    socket from a Qt idle timer at about 1 Hz, so a walk that needs many
    round trips cannot finish; and a pc_break that re-fires on its next hit
    re-pauses the moment anything resumes it, which wedges the emulator for
    the user as well as for the tool.

    None of it was necessary. The effect lasts seconds and the game rebuilds
    its ordering table every frame, so reading the list repeatedly while it
    runs catches the effect the same way psx-runtime's ring does -- by
    sampling often, not by freezing time.
    """
    # Undo any wedging an earlier run left behind, before anything else.
    oracle_resume(conn)
    try:
        n = oracle_clear_breaks(conn)
        if n:
            print(f"  cleared {n} stale breakpoint(s) left by an earlier run",
                  file=out)
    except DebugError as e:
        print(f"  WARNING: {e}", file=out)
    oracle_resume(conn)

    sigs = []
    empty = 0
    root = None          # once known, read only the span around it
    deadline = time.monotonic() + args.watch_secs
    while time.monotonic() < deadline and len(sigs) < args.samples:
        try:
            with open(os.devnull, "w") as quiet:
                rep, _meta = walk_side(conn, "oracle", pause=False,
                                       addr=root, window=args.window,
                                       max_nodes=args.max_nodes, out=quiet)
        except DebugError as e:
            print(f"  oracle read failed: {e}", file=out)
            time.sleep(args.poll)
            continue
        if rep:
            # Remember where the list lives; the next read is a few KB rather
            # than 2 MB, which is what stops the oracle stuttering.
            root = rep.get("root") or root
            sig = signature(rep.get("prims") or [])
            if sig["quads"]:
                sigs.append(sig)
                print(f"  oracle sample {len(sigs)}/{args.samples}: "
                      f"{sig['quads']} quads, {sig['distinct_colours']} "
                      f"colours, {sig['y_span']} line span", file=out)
            else:
                empty += 1
        time.sleep(args.poll)

    if not sigs:
        print(f"  oracle: {empty} walks read, none held additive shaded "
              f"quads -- the effect was not playing on DuckStation while "
              f"this ran. Replay it there and try again.", file=out)
    return sigs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--samples", type=int, default=8)
    ap.add_argument("--watch-secs", type=float, default=90.0,
                    help="how long to keep reading the running oracle")
    ap.add_argument("--window", type=lambda v: int(v, 0), default=0x20000,
                    help="bytes of RAM to re-read per oracle sample once the "
                         "list is located (0 = always snapshot all 2 MB)")
    ap.add_argument("--poll", type=float, default=0.4,
                    help="seconds between oracle reads")
    ap.add_argument("--ring-frames", type=int, default=600,
                    help="how far back through psx-runtime's ring to look")
    ap.add_argument("--stride", type=int, default=4)
    ap.add_argument("--count", type=int, default=20000)
    ap.add_argument("--max-nodes", type=int, default=8192)
    ap.add_argument("--out", default="analysis/frames/effect_palette.json")
    args = ap.parse_args()

    doc = {"kind": KIND, "version": 1}
    print("sampling psx-runtime's GP0 ring …", flush=True)
    nat_sigs = sample_native(DebugConn(args.host, args.port, args.timeout),
                             args)
    print("reading the running oracle (nothing is paused) …", flush=True)
    orc_sigs = sample_oracle(DebugConn(args.host, args.ds_port, args.timeout),
                             args)

    nat, orc = merge(nat_sigs), merge(orc_sigs)
    doc["native"] = {f"{k[0]}x{k[1]}": v for k, v in nat["groups"].items()}
    doc["oracle"] = {f"{k[0]}x{k[1]}": v for k, v in orc["groups"].items()}
    doc["native_samples"], doc["oracle_samples"] = nat_sigs, orc_sigs
    v, why, which = verdict(nat, orc)
    doc["verdict"], doc["explanation"] = v, why
    if which:
        doc["compared_object"] = {"quads": which[0], "y_span": which[1]}

    print(f"\n{'object':>16}  {'side':<12}{'colours':>9}{'saturated':>11}"
          f"{'dimmest':>9}{'peak':>7}{'samples':>9}")
    seen = sorted(set(nat["groups"]) | set(orc["groups"]),
                  key=lambda k: -(k[0] * max(k[1], 1)))
    for k in seen:
        label = f"{k[0]}q/{k[1]}ln"
        for side, m in (("psx-runtime", nat), ("oracle", orc)):
            g = m["groups"].get(k)
            if not g:
                print(f"{label:>16}  {side:<12}{'-':>9}{'-':>11}{'-':>9}"
                      f"{'-':>7}{0:>9}")
            else:
                print(f"{label:>16}  {side:<12}{g['distinct_colours']:>9}"
                      f"{g['saturated_colours']:>11}{g['peak_min']:>9}"
                      f"{g['peak_max']:>7}{g['samples']:>9}")
    print(f"\nVERDICT: {v}\n{why}")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"\nreport: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
