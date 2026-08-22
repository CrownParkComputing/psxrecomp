#!/usr/bin/env python3
"""packet_writers.py -- find the code that writes a primitive's COLOUR words.

    python3 packet_writers.py --class "PolyG4+semi|B+F"

What this is for
----------------
The display-list comparison narrowed a render bug to vertex colour: geometry is
identical between emulators, flat-coloured primitives agree, shaded ones do
not. The next question is which instruction writes those colour words, and that
cannot be answered by function attribution -- a game that builds an ordering
table and DMAs it reports the same submit PC for every packet on screen.

So it is answered by address. Walk the list, note where each primitive's
colour words actually live, watch those addresses, and see which PCs store to
them. That works even when the writer is overlay code the static analyser never
sees, which is where this game's are.

Why the addresses are derived rather than assumed
-------------------------------------------------
Packet stride is not a constant: it depends on the opcode, and a buffer holds
different opcodes. Assuming one stride and taking (addr - base) % stride
misclassifies every write as soon as the assumption slips -- it reports
confident nonsense rather than failing. Each field's address comes from the
walk itself.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_NATIVE_PORT, DebugConn, DebugError, decode_entries,
    find_display_lists, snapshot_ram, walk_ordering_table,
)


def field_map(prims, want=None):
    """addr -> 'colour' | 'vertex', derived from each packet's real layout.

    A gouraud polygon interleaves colour and vertex words (c,v,c,v,...); a flat
    one has a single leading colour. Textured packets carry a UV word after each
    vertex. The layout is read off the opcode rather than guessed.
    """
    roles = {}
    for p in prims:
        if want and p["op_name"] != want.split("|")[0]:
            continue
        if p["kind"] != "poly" or not p.get("src"):
            continue
        base = int(p["src"], 16) & 0x1FFFFFFF
        # Read the layout off the decoded packet, not off the opcode name.
        gouraud = bool(p.get("gouraud"))
        textured = bool(p.get("textured"))
        n = len(p.get("verts") or [])
        off = 0
        for i in range(n):
            if i == 0 or gouraud:
                roles[base + off * 4] = "colour"
                off += 1
            roles[base + off * 4] = "vertex"
            off += 1
            if textured:
                roles[base + off * 4] = "uv"
                off += 1
    return roles


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--class", dest="want", default="PolyG4+semi|B+F")
    ap.add_argument("--watch", type=float, default=5.0,
                    help="seconds to let the trace collect")
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args(argv)

    conn = DebugConn(args.host, args.port, args.timeout)
    try:
        conn.cmd("pause")
        try:
            ram = snapshot_ram(conn)
        finally:
            conn.cmd("continue")
        cands = find_display_lists(ram)
        if not cands:
            print("no display list found", file=sys.stderr)
            return 1
        prims = decode_entries(walk_ordering_table(ram, cands[0]["root"]))
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    op = args.want.split("|")[0]
    sel = [p for p in prims if p["kind"] == "poly" and p["op_name"] == op]
    if not sel:
        have = Counter(p["op_name"] for p in prims if p["kind"] == "poly")
        print(f"no {op} in the current list. Present: "
              f"{', '.join(f'{k} x{v}' for k, v in have.most_common(6))}",
              file=sys.stderr)
        print("\nThe effect has to be ON SCREEN for this to find its writers.",
              file=sys.stderr)
        return 1

    roles = field_map(sel, args.want)
    addrs = sorted(roles)
    lo, hi = addrs[0], addrs[-1] + 4
    print(f"{len(sel)} {op} packet(s), fields 0x{lo:08X}..0x{hi:08X}")
    print(f"  {sum(1 for v in roles.values() if v == 'colour')} colour words, "
          f"{sum(1 for v in roles.values() if v == 'vertex')} vertex words")

    try:
        conn.cmd("wtrace_reset")
        conn.cmd("wtrace_add", lo=f"0x{lo:08X}", hi=f"0x{hi:08X}")
        time.sleep(args.watch)
        rep = conn.cmd("wtrace_dump", addr_lo=f"0x{lo:08X}",
                       addr_hi=f"0x{hi:08X}", count=16000)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    entries = rep.get("entries", [])
    by_pc = defaultdict(Counter)
    unknown = 0
    for e in entries:
        a = int(e["addr"], 16) & 0x1FFFFFFF
        role = roles.get(a & ~3)
        if role is None:
            unknown += 1
            role = "unmapped"
        by_pc[e["pc"]][role] += 1

    print(f"\n{len(entries)} write(s) recorded"
          + (f", {unknown} to addresses not in the walked layout (the list was "
             f"rebuilt while tracing)" if unknown else ""))
    print(f"\n  {'pc':<12}{'func':<12}{'COLOUR':>8}{'vertex':>8}{'uv':>5}")
    ranked = sorted(by_pc.items(), key=lambda kv: -kv[1]["colour"])
    pcfunc = {e["pc"]: e.get("func", "") for e in entries}
    for pc, k in ranked[:14]:
        print(f"  {pc:<12}{pcfunc.get(pc,''):<12}{k['colour']:>8}"
              f"{k['vertex']:>8}{k['uv']:>5}")
    top = [pc for pc, k in ranked if k["colour"] and not k["vertex"]]
    if top:
        print(f"\nWrites ONLY colour words: {', '.join(top[:6])}")
        print("Disassemble with:  python3 disasm_ram.py --pc " + top[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
