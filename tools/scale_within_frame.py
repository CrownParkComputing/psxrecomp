#!/usr/bin/env python3
"""scale_within_frame.py -- is the fade scale constant across ONE frame?

    python3 tools/scale_within_frame.py

The effect's colours are built by, per vertex:

    lbu $v0,64($sp)        ; source byte
    mult $v0,$s6           ; * fade scale
    mflo $t6
    sra  $v0,$t6,7         ; >> 7

$s6 is the fade level. It is one value for the whole frame -- the effect
fades as a unit -- so every vertex in a frame must see the SAME $s6, and the
64 additive quads should carry only a handful of distinct colours. psx-runtime
carries about 153 where DuckStation carries 3.

If $s6 changes WITHIN a frame on psx-runtime, that alone explains it: same
source colours, same arithmetic, a different scale per vertex. And unlike
every other comparison in this investigation, it needs no oracle and no phase
matching -- "constant within a frame" is a property of one emulator, checkable
against itself.

$s6 is callee-saved and stays live across the GTE ops in this routine
(swc2 / NCLIP at 0x800684C4..0x800684D8), which is exactly where a
recompiled routine could lose it.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from probe_regs import search_windows  # noqa: E402
from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402

KIND = "psx-scale-within-frame"


def distinct_scales(samples, reg="s6"):
    """Distinct values of `reg` across samples taken inside one frame."""
    vals = collections.Counter()
    for s in samples:
        v = (s.get("regs") or {}).get(reg)
        if v:
            vals[int(v, 16)] += 1
    return vals


def verdict_of(per_frame, reg="s6"):
    """per_frame: list of Counters, one per frame sampled."""
    live = [c for c in per_frame if c]
    if not live:
        return ("no-samples",
                f"${reg} was never captured -- the probe did not fire inside "
                f"the effect. Replay it while this runs.")
    worst = max(live, key=len)
    if len(worst) == 1:
        return ("constant-within-frame",
                f"${reg} held a single value in every frame sampled "
                f"({len(live)} frame(s)). The scale is not the source of the "
                f"extra colour variety; look at the source bytes instead.")
    return ("varies-within-frame",
            f"${reg} took {len(worst)} DIFFERENT values inside one frame "
            f"({sorted(worst)[:8]}{' …' if len(worst) > 8 else ''}). The fade "
            f"is one level per frame, so every vertex should see the same "
            f"value. A per-vertex scale multiplies the same source colours by "
            f"different amounts -- which is exactly the extra colour variety "
            f"psx-runtime shows and DuckStation does not.")


def sample_one_frame(conn, pc, n, leader=None):
    """Arm at `pc`, advance exactly one frame, return the probe samples."""
    windows = search_windows(pc, 0x400)
    cands = [leader] if leader else windows[0]
    conn.cmd("pc_probe_clear")
    conn.cmd("pc_probe_arm", n=n,
             pcs=",".join(f"0x{c:08X}" for c in cands))
    f0 = conn.frame()
    conn.cmd("step", n=1)
    for _ in range(200):
        st = conn.raw("pause_state")
        if st.get("paused") and conn.frame() > f0:
            break
        time.sleep(0.02)
    rep = conn.cmd("pc_probe_dump")
    hits = [s for s in rep.get("samples", []) if s.get("regs")]
    return hits, rep


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--pc", default="0x80068474",
                    help="the multiply that scales a source byte by $s6")
    ap.add_argument("--reg", default="s6")
    ap.add_argument("--n", type=int, default=64,
                    help="probe slots per frame (64 quads x 4 verts = 256)")
    ap.add_argument("--frames", type=int, default=6,
                    help="frames to sample; each is checked on its own")
    ap.add_argument("--wait-secs", type=float, default=120.0)
    ap.add_argument("--out", default="analysis/frames/scale_within_frame.json")
    args = ap.parse_args()

    pc = int(args.pc, 0)
    conn = DebugConn(args.host, args.port, args.timeout)
    print(f"sampling ${args.reg} at {args.pc} — trigger the effect now",
          flush=True)

    per_frame = []
    raw = []
    deadline = time.monotonic() + args.wait_secs
    try:
        conn.cmd("pause")
        while len(per_frame) < args.frames and time.monotonic() < deadline:
            try:
                hits, rep = sample_one_frame(conn, pc, args.n)
            except DebugError as e:
                print(f"  probe failed: {e}", file=sys.stderr)
                break
            vals = distinct_scales(hits, args.reg)
            if not vals:
                continue
            per_frame.append(vals)
            raw.append({"distinct": len(vals),
                        "values": {str(k): v for k, v in vals.items()}})
            print(f"  frame {len(per_frame)}: {len(hits)} samples, "
                  f"{len(vals)} distinct ${args.reg} "
                  f"{sorted(vals)[:8]}", flush=True)
    finally:
        try:
            conn.cmd("continue")
        except DebugError:
            pass

    v, why = verdict_of(per_frame, args.reg)
    doc = {"kind": KIND, "version": 1, "pc": args.pc, "reg": args.reg,
           "frames": raw, "verdict": v, "explanation": why}
    print(f"\nVERDICT: {v}\n{why}")
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"\nreport: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
