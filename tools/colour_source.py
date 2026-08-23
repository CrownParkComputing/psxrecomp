#!/usr/bin/env python3
"""colour_source.py -- find and inspect the table the effect's colours come from.

    python3 tools/colour_source.py

Where this sits
---------------
The effect's colours are built at 0x8006844C and friends by:

    lwl $t6,-9($s4) / lwr $t6,-12($s4)   ; source colour word
    swl/swr -> 64($sp)                   ; staged in scratchpad
    lbu 64/65/66($sp) ; mult by $s6 ; sra >>7 ; sb   ; scaled
    beq $s6,$t3 -> skip the multiply     ; at neutral scale, copied verbatim

$s6 has been measured constant within each frame and ramping smoothly
(128,124,120,...), so the scale is not the source of psx-runtime's ~153
distinct vertex colours where DuckStation has 3. At neutral scale the branch
copies the source word straight through, so those 153 values ARE the source
table.

This locates that table by probing $s4, reads it, counts what is actually in
it, and traces which code wrote it. Comparing the same table on the oracle
(--port 4371, read-only) answers whether the data or the code filling it is
at fault.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_NATIVE_PORT, DebugConn, DebugError, read_ram_range,
)
from scale_within_frame import sample_one_frame  # noqa: E402

KIND = "psx-colour-source"

# The source pointer is read as $s4-12 .. $s4-1 (lwr -12, lwl -9 for the first
# of three words), so the table starts before the lowest $s4 seen.
SRC_BACK = 16


def pointer_span(samples, reg="s4", back=SRC_BACK):
    """Address range the source pointer covered, widened to what it reads."""
    vals = []
    for s in samples:
        v = (s.get("regs") or {}).get(reg)
        if v:
            vals.append(int(v, 16))
    if not vals:
        return None
    return (min(vals) - back, max(vals)), sorted(set(vals))


def colours_in(blob, base, lo, hi):
    """Decode a span of RAM as 24-bit colour words."""
    out = collections.Counter()
    for a in range(lo & ~3, (hi & ~3) + 4, 4):
        off = a - base
        if off < 0 or off + 4 > len(blob):
            continue
        w = int.from_bytes(blob[off:off + 4], "little")
        out[(w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF)] += 1
    return out


def verdict_of(n_colours, expect_max=8):
    if n_colours == 0:
        return ("no-source",
                "the source span could not be read, so nothing is decided.")
    if n_colours <= expect_max:
        return ("source-is-uniform",
                f"the source table holds {n_colours} distinct colour word(s) "
                f"-- consistent with DuckStation's 3. If the packets still "
                f"carry ~153, the fault is between this table and the packet, "
                f"not in the table.")
    return ("source-is-varied",
            f"the source table holds {n_colours} distinct colour words. That "
            f"matches what psx-runtime writes into its packets, so the wrong "
            f"data is already in the table before the colour routine reads "
            f"it -- the fault is in whatever FILLS this table.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--pc", default="0x80068450",
                    help="the lwl that loads a source colour word")
    ap.add_argument("--reg", default="s4")
    ap.add_argument("--n", type=int, default=64)
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--trace", action="store_true",
                    help="also trace which code writes the table (native only)")
    ap.add_argument("--wait-secs", type=float, default=120.0)
    ap.add_argument("--out", default="analysis/frames/colour_source.json")
    args = ap.parse_args()

    pc = int(args.pc, 0)
    conn = DebugConn(args.host, args.port, args.timeout)
    doc = {"kind": KIND, "version": 1, "pc": args.pc, "port": args.port}
    print(f"probing ${args.reg} at {args.pc} — trigger the effect now",
          flush=True)

    span = None
    ptrs = []
    deadline = time.monotonic() + args.wait_secs
    try:
        conn.cmd("pause")
        for _ in range(args.frames):
            if time.monotonic() > deadline:
                break
            try:
                hits, _ = sample_one_frame(conn, pc, args.n)
            except DebugError as e:
                print(f"  probe failed: {e}", file=sys.stderr)
                break
            got = pointer_span(hits, args.reg)
            if not got:
                continue
            (lo, hi), vals = got
            ptrs = vals
            span = (lo, hi) if span is None else (min(span[0], lo),
                                                  max(span[1], hi))
            print(f"  ${args.reg} covered 0x{lo:08X}..0x{hi:08X} "
                  f"({len(vals)} distinct pointer values)", flush=True)

        if span is None:
            print(f"\n${args.reg} was never captured -- the probe did not "
                  f"fire inside the effect. Replay it while this runs.")
            return 1

        lo, hi = span
        doc["span"] = [f"0x{lo:08X}", f"0x{hi:08X}"]
        doc["pointer_values"] = [f"0x{v:08X}" for v in ptrs[:32]]
        blob = read_ram_range(conn, lo & ~3, ((hi - lo) & ~3) + 64)
        cols = colours_in(blob, lo & ~3, lo, hi)
        doc["distinct_colours"] = len(cols)
        doc["top_colours"] = [[list(c), n] for c, n in cols.most_common(12)]
        print(f"\nsource span 0x{lo:08X}..0x{hi:08X}: {len(cols)} distinct "
              f"colour word(s)")
        for c, n in cols.most_common(10):
            print(f"    {c}  x{n}")

        v, why = verdict_of(len(cols))
        doc["verdict"], doc["explanation"] = v, why
        print(f"\nVERDICT: {v}\n{why}")

        if args.trace and v == "source-is-varied":
            print(f"\ntracing writes to the table …", flush=True)
            try:
                conn.cmd("wtrace_reset")
                conn.cmd("wtrace_add", lo=f"0x{lo:08X}", hi=f"0x{hi:08X}")
                f0 = conn.frame()
                conn.cmd("step", n=2)
                for _ in range(200):
                    st = conn.raw("pause_state")
                    if st.get("paused") and conn.frame() > f0:
                        break
                    time.sleep(0.02)
                rep = conn.cmd("wtrace_dump", addr_lo=f"0x{lo:08X}",
                               addr_hi=f"0x{hi:08X}", count=8000)
                by_pc = collections.Counter(e["pc"] for e in
                                            rep.get("entries", []))
                doc["writers"] = [[p, n] for p, n in by_pc.most_common(12)]
                if by_pc:
                    print("  writers:")
                    for p, n in by_pc.most_common(12):
                        print(f"    {p}  x{n}")
                else:
                    print("  no writes in the traced window -- the table may "
                          "be filled less often than every frame, or copied "
                          "in by DMA rather than by CPU stores.")
            except DebugError as e:
                print(f"  trace failed: {e}", file=sys.stderr)
    finally:
        try:
            conn.cmd("continue")
        except DebugError:
            pass

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"\nreport: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
