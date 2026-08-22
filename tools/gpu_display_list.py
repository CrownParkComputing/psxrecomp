#!/usr/bin/env python3
"""gpu_display_list.py -- read a display list out of guest RAM and decode it.

    python3 gpu_display_list.py                            # scan RAM for lists
    python3 gpu_display_list.py --addr 0x0010D05C          # a known buffer
    python3 gpu_display_list.py --port 4371                # the oracle

Why this exists
---------------
A PSX game does not usually poke GP0 directly. It builds a linked list in RAM
and hands it to DMA channel 2, so the display list is a data structure sitting
in memory, and reading it is a different act from watching commands go by.

That matters for three things:

  * The oracle has no GP0 ring, so walking RAM is the ONLY way to see what
    DuckStation was about to draw.
  * For decompilation, seeing what a routine builds tells you what the routine
    IS. Pair this with a write-trace ("code at 0x80061364 fills this buffer")
    and the function names itself -- which works even for overlay code the
    static analyser never sees.
  * For modding, finding the display list that draws a thing is the
    prerequisite for changing it.

The list is walked from a snapshot of RAM rather than node by node: a display
list runs to hundreds of nodes, and a round trip each would be absurd.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_NATIVE_PORT, STP_MODES, DebugConn, DebugError, decode_entries,
    dma_gpu_list_root, read_ram_range, snapshot_ram,
    walk_ordering_table,
)

LIST_KIND = "psx-display-list"


DRAWING = ("poly", "rect", "line", "fill")


def blend_of(p):
    return STP_MODES.get(p.get("stp", 0), "?") if p.get("semi") else "opaque"


def cmax(p):
    """Brightest channel over a primitive's vertices.

    The useful number for an additive effect: what the game asked the blender
    to add. Comparing it across emulators separates "built different colours"
    from "blended the same colours differently".
    """
    return max((v for c in (p.get("colors") or []) for v in c), default=0)


def report(prims, root, port):
    """The same summary the CLI prints, as data. Studio reads this."""
    drawing = [p for p in prims if p["kind"] in DRAWING]
    classes = Counter(f"{p['op_name']}|{blend_of(p)}" for p in drawing)
    return {
        "kind": LIST_KIND, "version": 1,
        "root": f"0x{root:08X}", "port": port,
        "nodes": len(prims), "drawing": len(drawing),
        "classes": [{"key": k, "count": n} for k, n in classes.most_common()],
        "prims": [{
            "op": p["op_name"],
            "blend": blend_of(p),
            "src": p.get("src", ""),
            "cmax": cmax(p),
            "verts": " ".join(f"({x},{y})" for x, y in (p.get("verts") or [])),
            "colors": " ".join(str(tuple(c)) for c in (p.get("colors") or [])),
        } for p in drawing],
    }


def summarise(prims, out=sys.stdout, limit=20):
    drawing = [p for p in prims if p["kind"] in DRAWING]
    print(f"{len(prims)} node(s), {len(drawing)} drawing", file=out)
    if not drawing:
        return
    classes = Counter(f"{p['op_name']}|{blend_of(p)}" for p in drawing)
    print("\nclasses:", file=out)
    for k, n in classes.most_common():
        print(f"  {k:<26} {n:>5}", file=out)

    print(f"\nfirst {min(limit, len(drawing))} primitives:", file=out)
    print(f"  {'#':>4}  {'opcode':<16} {'blend':<10} {'src':<12} vertices / colours",
          file=out)
    for i, p in enumerate(drawing[:limit]):
        mode = blend_of(p)
        verts = " ".join(f"({x},{y})" for x, y in (p.get("verts") or [])[:4])
        cols = " ".join(str(tuple(c)) for c in (p.get("colors") or [])[:4])
        print(f"  {i:>4}  {p['op_name']:<16} {mode:<10} {p['src']:<12} {verts}",
              file=out)
        if cols:
            print(f"  {'':>4}  {'':<16} {'':<10} {'':<12} {cols}", file=out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--addr", default=None, help="root of the list (hex ok)")
    ap.add_argument("--near", default=None,
                    help="prefer a chain covering this address (e.g. a src "
                         "address the GP0 ring reported)")
    ap.add_argument("--from-dma", action="store_true",
                    help="take the root from DMA ch2 MADR (usually wrong — a "
                         "finished transfer leaves the terminator there)")
    ap.add_argument("--candidates", type=int, default=6,
                    help="how many scanned chains to list")
    ap.add_argument("--max-nodes", type=int, default=8192)
    ap.add_argument("--limit", type=int, default=20, help="primitives to print")
    ap.add_argument("--json", default=None)
    ap.add_argument("--pause", action="store_true",
                    help="park the emulator around the snapshot. Safe against "
                         "psx-runtime, which keeps serving while parked. NOT "
                         "safe against the DuckStation oracle: pausing it drops "
                         "its socket to the idle poll timer (1 Hz with no "
                         "gamepad present), and a 2 MB read cannot finish.")
    args = ap.parse_args(argv)

    conn = DebugConn(args.host, args.port, args.timeout)
    paused = False
    try:
        # Pause FIRST, then read the root, then snapshot. The root and the list
        # it points at have to come from the same instant: a game rebuilds its
        # ordering table every frame, so a root read while running can name a
        # list that the snapshot no longer contains -- which walks into whatever
        # replaced it and reports a display list that never existed.
        if args.pause:
            try:
                conn.cmd("pause")
                paused = True
            except DebugError as e:
                print(f"  warning: could not pause: {e}", file=sys.stderr)

        root = int(args.addr, 0) if args.addr else None
        if root is None and args.from_dma:
            root = dma_gpu_list_root(conn)
            if root is None:
                print("  DMA ch2 MADR holds the end-of-list terminator, not a "
                      "root — the transfer already finished. Scanning instead.",
                      file=sys.stderr)
            else:
                print(f"DMA ch2 MADR -> 0x{root:08X}")

        print(f"snapshotting RAM from {args.host}:{args.port} …")
        ram = snapshot_ram(conn)
        print(f"  {len(ram)} bytes")
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    finally:
        if paused:
            try:
                conn.cmd("continue")
            except DebugError:
                pass

    near = int(args.near, 0) if args.near else None
    entries = walk_ordering_table(ram, root, max_nodes=args.max_nodes) if root else []
    if root and not entries:
        print(f"nothing walkable from 0x{root:08X} — the tag there is not an "
              f"ordering-table node. Scanning for one instead.", file=sys.stderr)

    if not entries:
        print("scanning RAM for ordering-table chains …")
        cands = find_display_lists(ram, near=near, limit=args.candidates)
        if not cands:
            print("no display lists found. If the game is on a menu or a load "
                  "screen it may genuinely have none on screen — capture while "
                  "the effect is visible.", file=sys.stderr)
            return 1
        print(f"  {len(cands)} candidate chain(s), best first:")
        for c in cands:
            print(f"    0x{c['root']:06X}  {c['nodes']:>5} nodes  {c['prims']:>5} "
                  f"prims  src 0x{c['lo']:06X}..0x{c['hi']:06X}  "
                  f"{', '.join(c['classes'][:4])}")
        root = cands[0]["root"]
        print(f"  walking the best: 0x{root:06X}")
        entries = walk_ordering_table(ram, root, max_nodes=args.max_nodes)

    prims = decode_entries(entries)
    summarise(prims, limit=args.limit)

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report(prims, root, args.port), f, indent=1)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
