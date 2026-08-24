#!/usr/bin/env python3
"""gap_anatomy.py -- split the 2.5M-cycle gap between sectors into its parts.

    python3 tools/gap_anatomy.py      # after playing the palette load

Measured: sectors the game individually Setlocs arrive ~2,500,000 guest cycles
(4.4 frames) after the previous one, while continuations arrive 225,792
(0.40 frames, the exact 2x cadence). due == buffer on every record, so the
drive is never late -- the READ IS ARMED late. DuckStation moves the same 14
sectors in 2 guest frames.

That gap has three possible tenants, needing three different fixes:

  1. guest silence   -- the sector arrived and the guest issued nothing for a
                        long time. A guest-side stall (or it never saw the
                        notification).
  2. queued command  -- the guest DID issue Setloc/ReadN promptly, but it sat
                        queued because try_execute_queued_command() only runs
                        when irq_flag == 0. Then the ack latency is the cost,
                        and that is ours, not the game's.
  3. read start      -- initial_read_delay_cycles(), 451584 at 2x.

The command history now carries a guest cycle stamp, so this prints, for each
individually-requested sector, how many cycles fell into each bucket.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402

KIND = "psx-gap-anatomy"
CYCLES_PER_FRAME = 33868800 / 60.0
READ_START_2X = 451584


def bcd(v):
    return (v >> 4) * 10 + (v & 0x0F)


def setloc_cycles(entries):
    """LBA -> guest cycle at which its Setloc was recorded (latest wins)."""
    out = {}
    for e in entries:
        cmd = e.get("cmd")
        cmd = int(cmd, 16) if isinstance(cmd, str) else int(cmd or 0)
        if cmd != 0x02:
            continue
        ps = [int(x, 16) if isinstance(x, str) else int(x)
              for x in (e.get("params") or [])]
        if len(ps) < 3:
            continue
        m, s, f = (bcd(x) for x in ps[:3])
        out[(m * 60 + s) * 75 + f - 150] = int(e.get("cycle", 0))
    return out


def sector_cycles(entries):
    """LBA -> cycle the sector landed in the buffer (latest wins)."""
    out = {}
    for e in entries:
        lba = int(e.get("lba", -1))
        if lba >= 0:
            out[lba] = int(e.get("buffer", 0))
    return out


def anatomy(sectors, setlocs, lo, hi):
    """Per requested sector: silence / queue+start, from the previous sector."""
    rows = []
    for lba in sorted(sectors):
        if not (lo <= lba <= hi) or lba not in setlocs:
            continue
        prev = max((v for k, v in sectors.items()
                    if k < lba and sectors[k] < sectors[lba]), default=None)
        if prev is None:
            continue
        arrive = sectors[lba]
        issued = setlocs[lba]
        rows.append({
            "lba": lba,
            "gap": arrive - prev,
            "silence": max(0, issued - prev),
            "after_issue": max(0, arrive - issued),
        })
    return rows


def verdict_of(rows):
    if not rows:
        return ("no-data", "no individually-requested sector had both a "
                          "Setloc stamp and a delivery record.")
    tot = sum(r["gap"] for r in rows)
    sil = sum(r["silence"] for r in rows)
    aft = sum(r["after_issue"] for r in rows)
    start = READ_START_2X * len(rows)
    queue = max(0, aft - start)
    if sil > aft:
        return ("guest-silence",
                f"{sil/tot:.0%} of the gap is the guest issuing nothing after "
                f"the previous sector arrived ({sil/len(rows):,.0f} cycles "
                f"each). The drive is idle and waiting on the game, so the "
                f"cost is guest-side -- either it is doing real work, or it "
                f"never acted on the notification.")
    return ("emulator-latency",
            f"{aft/tot:.0%} of the gap falls AFTER the guest asked "
            f"({aft/len(rows):,.0f} cycles each): about "
            f"{start/len(rows):,.0f} of read-start delay and "
            f"{queue/len(rows):,.0f} of command queueing. That part is ours, "
            f"not the game's -- a queued Setloc/ReadN waits for irq_flag to "
            f"clear before it can even run.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--lba-lo", type=int, default=125104)
    ap.add_argument("--lba-hi", type=int, default=125117)
    ap.add_argument("--out", default="analysis/frames/gap_anatomy.json")
    args = ap.parse_args()

    conn = DebugConn(args.host, args.port, args.timeout)
    try:
        tim = conn.cmd("cdrom_timing_dump", tail=4096,
                       lba_lo=args.lba_lo - 4, lba_hi=args.lba_hi)
        hist = conn.cmd("cdrom_command_history", count=4096)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    sectors = sector_cycles(tim.get("entries", []))
    setlocs = setloc_cycles(hist.get("entries", []))
    if hist.get("entries") and "cycle" not in hist["entries"][0]:
        print("this psx-runtime predates the command-history cycle stamp -- "
              "rebuild and restart it.", file=sys.stderr)
        return 1

    rows = anatomy(sectors, setlocs, args.lba_lo, args.lba_hi)
    if not rows:
        print(f"no requested sector in {args.lba_lo}..{args.lba_hi} has both "
              f"a Setloc and a delivery record -- replay the load and rerun.")
        return 1

    print(f"{'LBA':>7} {'gap':>10} {'silence':>10} {'after ask':>10}"
          f"   {'silence':>8} {'after':>8}  (frames)")
    for r in rows:
        print(f"{r['lba']:>7} {r['gap']:>10,} {r['silence']:>10,} "
              f"{r['after_issue']:>10,}   {r['silence']/CYCLES_PER_FRAME:>8.2f} "
              f"{r['after_issue']/CYCLES_PER_FRAME:>8.2f}")

    v, why = verdict_of(rows)
    doc = {"kind": KIND, "version": 1, "rows": rows,
           "verdict": v, "explanation": why}
    print(f"\nVERDICT: {v}\n{why}")
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"\nreport: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
