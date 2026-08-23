#!/usr/bin/env python3
"""lockstep_check.py -- compare our COMPILED code against our own interpreter.

    python3 lockstep_check.py --frames 120
    python3 lockstep_check.py --func --frames 120     # dispatch-segment level

Why this is the right instrument for a recompilation bug
--------------------------------------------------------
Every cross-emulator comparison in this investigation has been fought on three
fronts before it could say anything: aligning frames, matching addresses across
two allocators, and getting both emulators to the same point in the game. Each
of those has produced a confident wrong answer at least once.

Lockstep needs none of them. It runs the same guest code twice inside ONE
process -- once compiled, once interpreted -- and reports the first place they
disagree, with the register or memory value each produced. There is no oracle,
no frame number to align, no address to correspond.

What "found: false" does and does not mean
------------------------------------------
It means no divergence was seen in the blocks that were actually CHECKED, which
is not the same as none existing. The segment-level comparator skips segments it
cannot replay -- interrupts, overflow, unhandled ops, conflicts -- and a run
that skipped everything reports exactly the same "found: false" as a clean one.
So the counters are reported alongside the verdict, and a check with nothing
checked is called out rather than read as a pass.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402

KIND = "psx-lockstep"

# What each divergence kind tells you about where to look.
MEANING = {
    "reg": "a general register held a different value",
    "gpr": "a general register held a different value",
    "hi": "the HI multiply/divide result differed",
    "lo": "the LO multiply/divide result differed",
    "pc": "control flow went somewhere else",
    "write-val": "the same address was written with a different VALUE",
    "read-addr": "a load came from a different ADDRESS",
    "write-addr": "a store went to a different ADDRESS",
    "trace-exhausted": "the compiled path ran out of recorded operations",
    "trace-leftover": "the interpreter had operations the compiled path did not",
    "compiled-extra-ops": "the compiled path performed operations the interpreter did not",
    "path-cap": "the comparison hit its own length limit, not a divergence",
    "unsupported": "this segment shape is not replayable, not a divergence",
}

SKIPS = ("skipped_irq", "skipped_overflow", "skipped_unhandled",
         "skipped_conflict", "skipped_disabled")


def summarise(d, func_mode, out=sys.stdout):
    checked = d.get("segments_checked" if func_mode else "blocks_checked", 0)
    lo, hi = (d.get("window") or [0, 0])[:2]
    unit = "segment" if func_mode else "block"
    print(f"window frames {lo}..{hi}, {checked} {unit}(s) checked", file=out)

    skipped = {k: d.get(k, 0) for k in SKIPS if d.get(k)}
    if skipped:
        print("  skipped: " + ", ".join(f"{k[8:]}={v}" for k, v in skipped.items()),
              file=out)

    if not d.get("found"):
        if not checked:
            print("\nINCONCLUSIVE: nothing was checked. This is NOT a clean "
                  "result — it is the same output a clean run would give. Widen "
                  "the frame window, or confirm the comparator is enabled in "
                  "this build.", file=out)
            return "inconclusive"
        if skipped and sum(skipped.values()) >= checked:
            print(f"\nWEAK: {sum(skipped.values())} {unit}(s) were skipped "
                  f"against {checked} checked. Coverage is thin enough that a "
                  f"divergence could easily sit in what was skipped.", file=out)
            return "weak"
        print(f"\nNo divergence across {checked} {unit}(s). The compiled code "
              f"matched the interpreter everywhere it was compared — so within "
              f"this window, recompilation is not the fault.", file=out)
        return "clean"

    kind = d.get("kind", "?")
    print(f"\nFIRST DIVERGENCE ({kind}): {MEANING.get(kind, 'see the fields below')}",
          file=out)
    print(f"  frame     {d.get('frame')}", file=out)
    print(f"  {unit:<9} {d.get('entry') or d.get('block')}", file=out)
    print(f"  pc        {d.get('pc')}", file=out)
    if d.get("addr", "0x00000000") != "0x00000000":
        print(f"  address   {d.get('addr')}", file=out)
    if d.get("reg", -1) >= 0:
        print(f"  register  ${d.get('reg')}", file=out)
    print(f"  interpreter expected  {d.get('interp_expected')}", file=out)
    print(f"  compiled produced     {d.get('compiled_actual')}", file=out)
    tr = d.get("trace") or []
    if tr:
        print(f"  last {len(tr)} memory op(s) before it:", file=out)
        for t in tr[-12:]:
            print(f"    {t}", file=out)
    if kind in ("path-cap", "unsupported"):
        print("\nNote: this kind is a limit of the comparator, not a bug in the "
              "compiled code.", file=out)
    return "diverged"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--frames", type=int, default=120,
                    help="how many frames ahead to arm the comparison over")
    ap.add_argument("--lo", type=int, default=None,
                    help="explicit window start (default: the current frame)")
    ap.add_argument("--func", action="store_true",
                    help="compare whole dispatch segments instead of blocks")
    ap.add_argument("--wait", type=float, default=0.0,
                    help="seconds to wait after arming before reading back")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    cmd = "lockstep_func" if args.func else "lockstep"
    conn = DebugConn(args.host, args.port, args.timeout)
    doc = {"kind": KIND, "version": 1, "mode": cmd}
    try:
        lo = args.lo if args.lo is not None else conn.frame()
        hi = lo + max(1, args.frames)
        doc["window"] = [lo, hi]
        print(f"arming {cmd} over frames {lo}..{hi}")
        conn.cmd(cmd, lo=lo, hi=hi)
        if args.wait > 0:
            time.sleep(args.wait)
        else:
            # Let the window actually elapse; reading back immediately would
            # report on frames that have not run yet.
            deadline = time.time() + max(2.0, (hi - lo) / 50.0 + 2.0)
            while time.time() < deadline and conn.frame() < hi:
                time.sleep(0.25)
        rep = conn.cmd(cmd)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        doc["error"] = str(e)
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=1)
        return 2

    body = rep.get("lockstep") or rep.get("lockstep_func") or {}
    doc.update(body if isinstance(body, dict) else {})
    doc["verdict"] = summarise(doc, args.func)
    # Carry the plain-English reading of the divergence kind, so the GUI does
    # not have to keep its own copy of this table and drift from it.
    doc["meaning"] = MEANING.get(doc.get("kind", ""), "")
    doc["skipped_total"] = sum(doc.get(k, 0) for k in SKIPS)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        print(f"\nwrote {args.json}")
    return 0 if doc["verdict"] in ("clean", "diverged") else 1


if __name__ == "__main__":
    raise SystemExit(main())
