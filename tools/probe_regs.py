#!/usr/bin/env python3
"""probe_regs.py -- capture psx-runtime's registers near a PC.

    python3 probe_regs.py --pc 0x8006844C --want s4,s6

Why this is not simply a breakpoint
-----------------------------------
psx-runtime has no PC breakpoint. Its probe fires at BASIC-BLOCK LEADERS, which
the recompiler emits observers for, and an arbitrary mid-block address such as
a colour store never matches one. Adding a real breakpoint would mean emitting
per-instruction hooks from the recompiler -- a much larger change.

So the block containing the address is found by arming a SPREAD of candidates
around it and seeing which ones actually fire. Whatever fires is a real block
leader; the nearest one at or below the target encloses it.

What that costs in accuracy, stated plainly
-------------------------------------------
The registers are read at block ENTRY, not at the target instruction. For
callee-saved registers ($s0-$s7) set outside the loop -- which is what $s4 and
$s6 are in the colour routine -- those are the same value. For a register the
block itself computes ($v0, $t6, $a0 there) they are NOT, and this tool will
report a value that is real but taken earlier than asked for. It flags which
class each requested register falls into rather than leaving that to be
remembered.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402

KIND = "psx-probe-regs"
MAX_PCS = 16          # PC_PROBE_MAX_PCS in debug_server.c
SAVED = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "sp", "fp", "gp", "ra"}


def candidates(pc, back, step, limit=MAX_PCS):
    """Addresses to arm: the target, then backwards in `step` increments.

    Backwards because a block leader is at or BEFORE the instruction it
    contains. Arming forward addresses would find the next block, whose
    registers describe what happens after the one being asked about.
    """
    out = [pc]
    a = pc - step
    while len(out) < limit and a > pc - back:
        out.append(a)
        a -= step
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--pc", required=True, help="the address of interest")
    ap.add_argument("--want", default="s4,s6",
                    help="comma-separated register names to report")
    ap.add_argument("--back", type=lambda v: int(v, 0), default=0x100,
                    help="how far back to look for the block leader")
    ap.add_argument("--step", type=lambda v: int(v, 0), default=0x10)
    ap.add_argument("--samples", type=int, default=32)
    ap.add_argument("--wait", type=float, default=6.0)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    pc = int(args.pc, 16)
    want = [w.strip() for w in args.want.split(",") if w.strip()]
    cands = candidates(pc, args.back, args.step)
    doc = {"kind": KIND, "version": 1, "pc": f"0x{pc:08X}", "want": want,
           "candidates": [f"0x{c:08X}" for c in cands]}

    conn = DebugConn(args.host, args.port, args.timeout)
    try:
        conn.cmd("pc_probe_clear")
        conn.cmd("pc_probe_arm", n=args.samples,
                 pcs=",".join(f"0x{c:08X}" for c in cands))
        print(f"armed {len(cands)} candidate block leader(s) around 0x{pc:08X}; "
              f"running {args.wait:.0f}s …")
        time.sleep(args.wait)
        rep = conn.cmd("pc_probe_dump")
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        doc["error"] = str(e)
        return _finish(doc, args, 2)
    finally:
        try:
            conn.cmd("pc_probe_clear")
        except DebugError:
            pass

    slots = [s for s in rep.get("slots", []) if int(s.get("count", 0)) > 0]
    doc["fired"] = [{"pc": s["pc"], "count": int(s["count"])} for s in slots]
    if not slots:
        msg = (f"none of the {len(cands)} candidates fired. They are all "
               f"mid-block, or this code did not run in the window. Widen "
               f"--back, or make sure the effect is on screen.")
        print(f"error: {msg}", file=sys.stderr)
        doc["error"] = msg
        return _finish(doc, args, 1)

    print(f"\n{len(slots)} candidate(s) are real block leaders:")
    for s in slots:
        print(f"  {s['pc']}  hit {s['count']} time(s)")

    # The enclosing block is the firing leader closest to the target from below.
    below = [s for s in slots if (int(s["pc"], 16) & 0x1FFFFFFF) <= (pc & 0x1FFFFFFF)]
    chosen = max(below, key=lambda s: int(s["pc"], 16)) if below else slots[0]
    doc["block_leader"] = chosen["pc"]
    if not below:
        print("\nwarning: every firing leader is AFTER the target, so none of "
              "them encloses it. The values below are from the wrong side of "
              "the instruction.", file=sys.stderr)
        doc["leader_after_target"] = True

    samples = [s for s in rep.get("samples", [])
               if s.get("pc") == chosen["pc"] and s.get("regs")]
    doc["samples_seen"] = len(samples)
    if not samples:
        msg = (f"{chosen['pc']} fired but no register sample was captured. "
               f"Raise --samples.")
        print(f"error: {msg}", file=sys.stderr)
        doc["error"] = msg
        return _finish(doc, args, 1)

    last = samples[-1]["regs"]
    doc["regs"] = {w: last.get(w) for w in want}
    doc["frame"] = samples[-1].get("frame")
    print(f"\nblock leader {chosen['pc']}, {len(samples)} sample(s), "
          f"frame {doc['frame']}")
    for w in want:
        note = ("" if w in SAVED else
                "   <- computed inside the block; this is its value at ENTRY, "
                "not at the target instruction")
        print(f"  ${w:<4} = {last.get(w, '?')}{note}")
    doc["all_regs"] = last
    return _finish(doc, args, 0)


def _finish(doc, args, rc):
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        print(f"wrote {args.json}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
