#!/usr/bin/env python3
"""colour_inputs.py -- compare the INPUTS to a packet's colour computation.

    python3 colour_inputs.py --pc 0x8006844C

What this settles
-----------------
The display-list comparison showed the two emulators build identical geometry
and different vertex colours. packet_writers then found the code, and it
computes every colour the same way:

    lwl/lwr  $t6, ($s4-12)     load the source RGB
    mult     $v0, $s6          scale it
    sra      $v0, 7            >> 7
    sw       $v0, n($a3)       store into the packet

So a wrong colour has exactly two possible causes: the SOURCE RGB differs, or
the SCALE differs. This reads both and says which.

How, given only one side can break on a PC
------------------------------------------
The DuckStation oracle supports pc_break and captures all 32 GPRs on hit;
psx-runtime has no equivalent (its pc_probe fires at basic-block leaders with
a fixed set of registers, and adding a real one means emitting per-instruction
hooks from the recompiler).

That asymmetry does not matter here, because the register we need from the
oracle is a POINTER. Once it reports $s4, the source table's address is known
-- and the same address can be read on BOTH emulators with read_ram, which
both support. The comparison that matters is of memory, not registers.

What the answer means:
  * source bytes differ  -> the divergence is upstream, in whatever fills that
                            table; the colour code is faithfully scaling bad
                            input.
  * source bytes match   -> the input is fine, so the scale $s6 or the
                            arithmetic differs, and the fault is in this code.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT, ORACLE_PAUSED_POLL_S,
    DebugConn, DebugError, oracle_resume, read_ram_range,
)

KIND = "psx-colour-inputs"
SRC_OFFSET = -12          # lwr $t6,-12($s4): the source word's base
DEFAULT_SPAN = 48         # enough to see the table around it


def wait_for_hit(conn, pc, timeout, poll=ORACLE_PAUSED_POLL_S):
    """Arm a PC breakpoint on the oracle and wait for it to fire.

    Paced for a PAUSED oracle, not a running one. The breakpoint pauses
    DuckStation when it fires, which drops its debug socket to the Qt idle
    timer -- about 1 Hz with no gamepad attached. Polling every 100 ms then
    stacks up connections it cannot accept, and the failure surfaces as
    "server closed without replying", which does not read like a pacing
    problem at all.

    Resumes first, because a previous run that parked the oracle and exited
    without resuming leaves it crippled for every attempt after it.
    """
    oracle_resume(conn)
    for cmd in ("pc_hit_clear",):
        try:
            conn.cmd(cmd)
        except DebugError:
            pass                       # not fatal; the arm below is what matters
    conn.cmd("pc_break", addr=pc)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            rep = conn.cmd("pc_hit_last")
        except DebugError:
            # A hit parks the oracle mid-exchange, so one dropped reply here is
            # expected rather than fatal.
            time.sleep(poll)
            continue
        if rep.get("valid"):
            return rep
        time.sleep(poll)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--native-port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--pc", required=True,
                    help="a colour-writing PC, from the Shading pane")
    ap.add_argument("--ptr-reg", default="s4",
                    help="register holding the source pointer")
    ap.add_argument("--scale-reg", default="s6")
    ap.add_argument("--span", type=int, default=DEFAULT_SPAN)
    ap.add_argument("--wait", type=float, default=45.0,
                    help="seconds to wait for the PC to be reached. "
                         "A paused oracle answers about once a second, so this needs headroom.")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args(argv)

    pc = int(args.pc, 16)
    doc = {"kind": KIND, "version": 1, "pc": f"0x{pc:08X}",
           "ptr_reg": args.ptr_reg, "scale_reg": args.scale_reg}

    ds = DebugConn(args.host, args.ds_port, args.timeout)
    native = DebugConn(args.host, args.native_port, args.timeout)
    try:
        print(f"arming the oracle at 0x{pc:08X} …")
        hit = wait_for_hit(ds, pc, args.wait)
        if not hit:
            msg = (f"the oracle never reached 0x{pc:08X} within {args.wait:.0f}s. "
                   f"That PC is OVERLAY code — it only exists while the right "
                   f"overlay is resident, so the oracle has to be at the same "
                   f"point in the game.")
            print(f"error: {msg}", file=sys.stderr)
            doc["error"] = msg
            raise SystemExit(_finish(doc, args, 1))
        regs = hit.get("regs", {})
        ptr = int(regs.get(args.ptr_reg, "0x0"), 16)
        scale = int(regs.get(args.scale_reg, "0x0"), 16)
        doc["oracle_regs"] = {k: regs.get(k) for k in
                              (args.ptr_reg, args.scale_reg, "sp", "a3")}
        doc["oracle_scale"] = scale
        print(f"  hit at frame-time: ${args.ptr_reg}=0x{ptr:08X}  "
              f"${args.scale_reg}={scale} (scale/128 = {scale / 128:.3f})")

        src = (ptr + SRC_OFFSET) & 0x1FFFFFFF
        doc["source_addr"] = f"0x{src:08X}"
        if src == 0 or src >= 0x200000:
            msg = (f"${args.ptr_reg} does not point into RAM "
                   f"(0x{ptr:08X}); nothing to compare.")
            print(f"error: {msg}", file=sys.stderr)
            doc["error"] = msg
            raise SystemExit(_finish(doc, args, 1))

        print(f"reading {args.span} bytes at 0x{src:08X} from both …")
        a = read_ram_range(native, 0x80000000 | src, args.span)
        b = read_ram_range(ds, 0x80000000 | src, args.span)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        doc["error"] = str(e)
        return _finish(doc, args, 2)
    finally:
        # Clear the breakpoint AND resume. Leaving the oracle parked was the
        # actual defect here: it exits at 1 Hz and every later run then fails
        # in a way that points at the network rather than at this tool.
        try:
            ds.cmd("pc_unbreak", addr=pc)
        except DebugError:
            pass
        if not oracle_resume(ds):
            print("warning: could not resume the oracle — it may still be "
                  "parked, which will make later runs time out. Stop and "
                  "restart it from the Oracle tab.", file=sys.stderr)

    doc["native_bytes"] = a.hex()
    doc["oracle_bytes"] = b.hex()
    same = a == b
    doc["source_identical"] = same

    print(f"\n  psx-runtime: {a[:16].hex(' ')}")
    print(f"  oracle     : {b[:16].hex(' ')}")
    if same:
        doc["verdict"] = "source-matches"
        print("\nVERDICT: the source RGB is IDENTICAL on both. The input to the "
              "colour computation is fine, so the divergence is the scale "
              f"(${args.scale_reg}) or the arithmetic in this routine.")
    else:
        n = sum(1 for x, y in zip(a, b) if x != y)
        doc["verdict"] = "source-differs"
        doc["differing_bytes"] = n
        first = next(i for i, (x, y) in enumerate(zip(a, b)) if x != y)
        doc["first_difference"] = f"0x{src + first:08X}"
        print(f"\nVERDICT: the source RGB DIFFERS ({n}/{len(a)} bytes, first at "
              f"0x{src + first:08X}). The colour code is faithfully scaling bad "
              f"input — the fault is upstream, in whatever fills this table.")
    return _finish(doc, args, 0)


def _finish(doc, args, rc):
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
        print(f"wrote {args.json}")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
