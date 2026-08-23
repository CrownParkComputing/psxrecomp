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


def sample_per_frame(conn, pc, n, reg, out=sys.stderr):
    """Sample the register on CONSECUTIVE frames, by stepping.

    This is the measurement that actually answers "does the fade sweep or
    jump", and it needs one emulator rather than two.

    Free-running samples are taken about 1.5s apart -- ninety frames -- so the
    difference between consecutive readings is aliasing, not the animation
    increment. A fade stepping by 2 per frame sampled every ninety frames looks
    like arbitrary jumps, which is exactly what psx-runtime's 48/94/128 could
    be. Stepping one frame between reads removes the confound entirely: the
    deltas ARE the increment.
    """
    vals = []
    leader = None
    try:
        conn.cmd("pause")
    except DebugError as e:
        return vals, f"could not pause: {e}"
    try:
        for _ in range(n):
            pr = probe_registers(conn, pc, want=(reg,), wait=0.8, leader=leader)
            leader = pr.get("block_leader") or leader
            v = pr.get("regs", {}).get(reg)
            if v:
                vals.append(int(v, 16))
            else:
                leader = None
            f0 = conn.frame()
            conn.cmd("step", n=1)
            for _ in range(150):
                st = conn.raw("pause_state")
                if st.get("paused") and conn.frame() > f0:
                    break
                time.sleep(0.02)
    except DebugError as e:
        return vals, str(e)
    finally:
        try:
            conn.cmd("continue")
        except DebugError:
            pass
    return vals, (None if vals else "no samples were captured")


def sample_native(conn, pc, n, gap, reg, out=sys.stderr):
    """Returns (values, reason-it-stopped-or-None)."""
    vals = []
    leader = None
    for i in range(n):
        # Find the block leader once, then reuse it. The sweep is the expensive
        # part, and repeating it per sample capped an earlier run at three —
        # not enough to say anything about a value that animates.
        pr = probe_registers(conn, pc, want=(reg,), wait=(1.2 if leader else 1.5),
                             leader=leader)
        leader = leader or pr.get("block_leader")
        v = pr.get("regs", {}).get(reg)
        if v:
            vals.append(int(v, 16))
        elif pr.get("error"):
            # Drop the cached leader and keep going: one failed sample is not a
            # reason to abandon a series, and the usual cause is the block not
            # being reached on that pass.
            leader = None
            if len(vals) == 0 and i >= 2:
                print(f"  psx-runtime: {pr['error']}", file=out)
                return vals, pr["error"]
        time.sleep(gap)
    return vals, (None if vals else "no samples were captured")


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
    # Granularity, not just range. The oracle stepped 122/124/126/128 -- by 2 --
    # while psx-runtime showed 28 and 128 with nothing between. A fade that
    # moves in fine steps and one that jumps between extremes look identical in
    # min/max and produce very different pictures.
    steps = [b - a for a, b in zip(uniq, uniq[1:])]
    d = {"samples": len(vals), "distinct": len(uniq), "min": min(vals),
         "max": max(vals), "values": uniq[:16],
         "constant": len(uniq) == 1,
         "neutral_only": uniq == [NEUTRAL],
         "median_step": statistics.median(steps) if steps else 0,
         "max_step": max(steps) if steps else 0}
    print(f"  {label:<12} {len(vals):>3} sample(s), {len(uniq)} distinct, "
          f"range {min(vals)}..{max(vals)}"
          + (f", steps {min(steps)}..{max(steps)}" if steps else "")
          + f"  {uniq[:8]}", file=out)
    return d


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--native-port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--pc", required=True)
    ap.add_argument("--reg", default="s6")
    ap.add_argument("--samples", type=int, default=24,
                    help="more is better: comparing HOW a value "
                         "animates needs more than a handful")
    ap.add_argument("--gap", type=float, default=0.5)
    ap.add_argument("--per-frame", action="store_true",
                    help="step ONE frame between psx-runtime samples, so the "
                         "differences are the actual animation increment "
                         "rather than aliasing of a value sampled ~90 frames "
                         "apart")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    pc = int(args.pc, 16)
    doc = {"kind": KIND, "version": 1, "pc": f"0x{pc:08X}", "reg": args.reg}
    print(f"sampling ${args.reg} at 0x{pc:08X} on both emulators …")

    n = DebugConn(args.host, args.native_port, args.timeout)
    o = DebugConn(args.host, args.ds_port, args.timeout)
    if args.per_frame:
        print("  psx-runtime: stepping one frame between reads, so the deltas "
              "are the real per-frame increment")
        nat, nat_why = sample_per_frame(n, pc, args.samples, args.reg)
    else:
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

    # "Never moved" is a claim about a series, and one sample is not a series.
    # Reported on a single sample it produced exactly the conclusion this
    # investigation had already disproved twice -- the most dangerous shape a
    # bug can take, since it agrees with what someone already suspects.
    MIN_FOR_CONSTANT = 5
    thin = [n for n, d in (("psx-runtime", a), ("oracle", b))
            if d["constant"] and d["samples"] < MIN_FOR_CONSTANT]
    if thin:
        doc["verdict"] = "too-few-samples"
        doc["note"] = (
            f"{', '.join(thin)} produced fewer than {MIN_FOR_CONSTANT} samples "
            f"(psx-runtime {a['samples']}, oracle {b['samples']}), so 'never "
            f"moved' cannot be claimed for it. That is a statement about a "
            f"series, and this is not one yet.")
        print(f"\nINCONCLUSIVE: {doc['note']}", file=sys.stderr)
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
        # Both animating does not mean both animating the SAME WAY.
        if a["samples"] >= 8 and b["samples"] >= 8:
            if a["max_step"] >= 4 * max(1, b["max_step"]):
                doc["granularity"] = "native-coarser"
                print(f"\n  But psx-runtime moves in far coarser steps (up to "
                      f"{a['max_step']}) than the oracle (up to {b['max_step']}). "
                      f"A fade that jumps between extremes and one that sweeps "
                      f"smoothly produce very different pictures from identical "
                      f"geometry.")
            elif b["max_step"] >= 4 * max(1, a["max_step"]):
                doc["granularity"] = "oracle-coarser"
                print(f"\n  The ORACLE moves in coarser steps than psx-runtime, "
                      f"which is the opposite of the expected fault.")
            else:
                doc["granularity"] = "similar"
                print(f"\n  Step sizes are comparable "
                      f"(psx-runtime max {a['max_step']}, oracle max "
                      f"{b['max_step']}), so the fade granularity matches too.")
        elif args.per_frame and a["samples"] >= 5:
            doc["granularity"] = "native-measured-per-frame"
            print(f"\n  psx-runtime, sampled on CONSECUTIVE frames, moves in "
                  f"steps of up to {a['max_step']} (median "
                  f"{a['median_step']}). That is its real animation increment. "
                  f"The oracle's series is not frame-adjacent, so its apparent "
                  f"step is an upper bound only — but a per-frame increment "
                  f"this large is a coarse fade regardless of what the oracle "
                  f"does.")
        else:
            doc["granularity"] = "too-few-samples"
            print(f"\n  Too few samples ({a['samples']} vs {b['samples']}) to "
                  f"compare HOW each one moves. Two distinct values out of "
                  f"three is also what sparse sampling of a smooth ramp looks "
                  f"like — raise --samples.")
    return _finish(doc, args, 0)


def _finish(doc, args, rc):
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        print(f"wrote {args.json}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
