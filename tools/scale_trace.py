#!/usr/bin/env python3
"""scale_trace.py -- does the colour scale ANIMATE on each emulator?

    python3 scale_trace.py --pc 0x8006844C --samples 20

The question
------------
Every vertex colour in this effect is computed as source_rgb * $s6 >> 7. So
$s6 is the fade: 128 leaves the colour unchanged (x*128>>7 == x), and smaller
values darken it. An effect that fades from bright centre to dark edge is that
register sweeping.

Across every run so far the oracle's scale has come back different each time --
52, 64, 68, 76, 96, 126, 128 -- while psx-runtime's has read 128. If that
holds, psx-runtime is not modulating at all, and bright unfaded polygons are
exactly what the code would produce.

Why this measurement and not another
------------------------------------
It compares VARIATION, not values, so it does not need the two emulators
aligned to the same frame, the same buffer half, or the same animation phase.
Those three have each produced a confident wrong answer in this investigation.
A register that sweeps on one side and sits still on the other is a difference
no amount of misalignment can manufacture.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from probe_regs import probe_registers  # noqa: E402
from psx_gpu_frame import (  # noqa: E402
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT, DebugConn, DebugError,
    ORACLE_PAUSED_POLL_S, oracle_resume,
)

KIND = "psx-scale-trace"
NEUTRAL = 128


def sample_native(conn, pc, n, gap, reg, out=sys.stderr):
    """Returns (values, reason-it-stopped-or-None)."""
    vals = []
    for _ in range(n):
        pr = probe_registers(conn, pc, want=(reg,), wait=1.2)
        v = pr.get("regs", {}).get(reg)
        if v:
            vals.append(int(v, 16))
        elif pr.get("error"):
            print(f"  psx-runtime: {pr['error']}", file=out)
            return vals, pr["error"]
        time.sleep(gap)
    return vals, None


def sample_oracle(conn, pc, n, reg, out=sys.stderr):
    """Returns (values, reason-it-stopped-or-None).

    A side that produced nothing used to report as a bare null, which says
    "no data" without saying whether the emulator was unreachable, never
    reached the PC, or refused the breakpoint. Those need different responses,
    and the reason is known here and nowhere else.
    """
    vals = []
    oracle_resume(conn)
    for _ in range(n):
        try:
            conn.cmd("pc_hit_clear")
            conn.cmd("pc_break", addr=pc)
        except DebugError as e:
            print(f"  oracle: {e}", file=out)
            return vals, str(e)
        got = None
        for _ in range(20):
            time.sleep(ORACLE_PAUSED_POLL_S)
            try:
                rep = conn.cmd("pc_hit_last")
            except DebugError:
                continue
            if rep.get("valid"):
                got = rep.get("regs", {}).get(reg)
                break
        try:
            conn.cmd("pc_unbreak", addr=pc)
        except DebugError:
            pass
        oracle_resume(conn)
        if got is None:
            reason = (f"the oracle did not reach 0x{pc:08X} while sampling. "
                      f"That PC is overlay code, so it only exists while the "
                      f"right overlay is resident and the oracle has to be at "
                      f"the same point in the game.")
            print(f"  oracle: {reason}", file=out)
            return vals, reason
        vals.append(int(got, 16))
    return vals, None


def describe(vals, label, out=sys.stdout):
    if not vals:
        print(f"  {label:<12} no samples", file=out)
        return None
    uniq = sorted(set(vals))
    d = {"samples": len(vals), "distinct": len(uniq), "min": min(vals),
         "max": max(vals), "values": uniq[:12],
         "constant": len(uniq) == 1,
         "neutral_only": uniq == [NEUTRAL]}
    print(f"  {label:<12} {len(vals):>3} sample(s), {len(uniq)} distinct, "
          f"range {min(vals)}..{max(vals)}  {uniq[:8]}", file=out)
    return d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--native-port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--pc", required=True)
    ap.add_argument("--reg", default="s6")
    ap.add_argument("--samples", type=int, default=12)
    ap.add_argument("--gap", type=float, default=0.5)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    pc = int(args.pc, 16)
    doc = {"kind": KIND, "version": 1, "pc": f"0x{pc:08X}", "reg": args.reg}
    print(f"sampling ${args.reg} at 0x{pc:08X} on both emulators …")

    n = DebugConn(args.host, args.native_port, args.timeout)
    o = DebugConn(args.host, args.ds_port, args.timeout)
    nat, nat_why = sample_native(n, pc, args.samples, args.gap, args.reg)
    orc, orc_why = sample_oracle(o, pc, max(3, args.samples // 3), args.reg)

    print()
    a = describe(nat, "psx-runtime")
    b = describe(orc, "oracle")
    doc["native"], doc["oracle"] = a, b
    if nat_why:
        doc["native_error"] = nat_why
    if orc_why:
        doc["oracle_error"] = orc_why

    if not a or not b:
        doc["verdict"] = "incomplete"
        missing = "the oracle" if not b else "psx-runtime"
        why = (orc_why if not b else nat_why) or "no reason was reported"
        print(f"\nINCOMPLETE: {missing} produced no samples — {why}", file=sys.stderr)
        # A one-sided result is not a comparison, but the side that DID answer
        # is still worth stating: it is what disproves or supports a hypothesis
        # about that emulator on its own.
        got = a or b
        if got:
            who = "psx-runtime" if a else "the oracle"
            if got["constant"]:
                print(f"\nStill worth recording: {who}'s ${args.reg} did not "
                      f"move from {got['min']} across {got['samples']} samples.",
                      file=sys.stderr)
            else:
                print(f"\nStill worth recording: {who}'s ${args.reg} DOES vary "
                      f"({got['min']}..{got['max']}, values {got['values']}), so "
                      f"it is not pinned.", file=sys.stderr)
        return _finish(doc, args, 1)

    if a["constant"] and not b["constant"]:
        doc["verdict"] = "native-not-animating"
        note = (f"psx-runtime's ${args.reg} never moved from {a['min']} across "
                f"{a['samples']} samples, while the oracle's took "
                f"{b['distinct']} different values ({b['min']}..{b['max']}).")
        if a["neutral_only"]:
            note += (f" And {NEUTRAL} is the NEUTRAL scale: x*{NEUTRAL}>>7 "
                     f"leaves x unchanged, so psx-runtime is applying no fade "
                     f"at all. Bright unfaded polygons are what this code would "
                     f"then produce.")
        print(f"\nVERDICT: {note}")
        doc["note"] = note
    elif b["constant"] and not a["constant"]:
        doc["verdict"] = "oracle-not-animating"
        print(f"\nVERDICT: the oracle's ${args.reg} is constant while "
              f"psx-runtime's varies — the opposite of the expected fault, "
              f"and worth understanding before going further.")
    elif a["constant"] and b["constant"]:
        doc["verdict"] = "both-constant"
        print(f"\nVERDICT: neither side varies ({a['min']} vs {b['min']}). "
              f"Either the effect is not animating right now on either, or "
              f"${args.reg} is not what drives it.")
    else:
        doc["verdict"] = "both-animate"
        print(f"\nVERDICT: both vary — psx-runtime {a['min']}..{a['max']}, "
              f"oracle {b['min']}..{b['max']}. The scale is animating on both, "
              f"so the fade is not simply missing.")
    return _finish(doc, args, 0)


def _finish(doc, args, rc):
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        print(f"wrote {args.json}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
