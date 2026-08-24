#!/usr/bin/env python3
"""cd_verify.py -- watch the table load happen and say exactly what went wrong.

    python3 tools/cd_verify.py          # then play into the scene

One run, three answers, all from the same load:

1. REQUEST STREAM: the cd_read_log entries that fill the effect's colour
   table, with their SetLoc LBAs. The failure signature is the game itself
   requesting 125111 then 125113 -- its own bookkeeping skipped a sector.
2. PHYSICAL DELIVERY: the per-sector timing ring around LBA 125112. Was the
   sector read into the buffer (data=1)? Was its INT1 pended? LOST? With
   cycle timestamps, the guest's ack latency around the loss is visible
   directly -- at authentic 1x/2x cadence the guest has a full sector period,
   so a loss here means something upstream held the guest or the INT.
3. OUTCOME: the distinct-colour count of the table region afterwards.
   5 = the load was right this time; ~216 = still broken.

Timing is authentic in this configuration (game.toml sets no disc_speed), so
the accelerated-read flow control added earlier is not in play; this tool
exists to see what IS.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_NATIVE_PORT, DebugConn, DebugError, read_ram_range,
)
from table_watch import distinct_colours  # noqa: E402

KIND = "psx-cd-verify"

TABLE_LO = 0x0E25F0
TABLE_HI = 0x0E2C60
SECTOR = 2048


def table_loads(entries, lo=TABLE_LO, hi=TABLE_HI):
    out = []
    for e in entries:
        dest = int(e["dest"], 16) & 0x1FFFFF
        size = int(e["size"])
        if dest < hi and dest + size > lo:
            out.append({"lba": int(e["lba"]), "dest": dest, "size": size})
    return out


def request_gaps(loads):
    """Consecutive-destination loads whose LBAs do not advance in step."""
    gaps = []
    by_dest = sorted(loads, key=lambda x: x["dest"])
    for a, b in zip(by_dest, by_dest[1:]):
        dest_step = b["dest"] - a["dest"]
        lba_step = b["lba"] - a["lba"]
        expect = dest_step // SECTOR
        if dest_step > 0 and lba_step != expect:
            gaps.append({"from": a, "to": b,
                         "dest_sectors": expect, "lba_sectors": lba_step})
    return gaps


def analyse_records(entries):
    """Delivery-side view: which LBAs were read, pended, lost."""
    seen = {}
    for e in entries:
        lba = int(e["lba"])
        r = seen.setdefault(lba, {"data": 0, "dma": 0, "pended": 0,
                                  "lost": 0, "records": 0})
        r["records"] += 1
        for k in ("data", "dma", "pended", "lost"):
            r[k] += int(e.get(k, 0))
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--lba-lo", type=int, default=125100)
    ap.add_argument("--lba-hi", type=int, default=125125)
    ap.add_argument("--wait-secs", type=float, default=300.0)
    ap.add_argument("--out", default="analysis/frames/cd_verify.json")
    args = ap.parse_args()

    conn = DebugConn(args.host, args.port, args.timeout)
    doc = {"kind": KIND, "version": 1}

    try:
        base = conn.cmd("cd_read_log", tail=1)
        base_total = int(base.get("total", 0))
        conn.cmd("cdrom_timing", reset=1)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print("armed. Play into the scene now (through the moment the map "
          "appears); watching for the table load …", flush=True)
    deadline = time.monotonic() + args.wait_secs
    loads = []
    while time.monotonic() < deadline:
        try:
            rep = conn.cmd("cd_read_log", tail=4096)
        except DebugError:
            time.sleep(1.0)
            continue
        entries = rep.get("entries", [])
        total = int(rep.get("total", 0))
        fresh = entries[max(0, len(entries) - (total - base_total)):] \
            if total > base_total else []
        loads = table_loads(fresh)
        if any(l["dest"] >= 0x0E2718 for l in loads):
            break
        time.sleep(1.0)
    else:
        print("the table load never appeared in the CD log -- was the scene "
              "entered?")
        return 1

    time.sleep(2.0)     # let the tail of the load finish
    rep = conn.cmd("cd_read_log", tail=4096)
    entries = rep.get("entries", [])
    total = int(rep.get("total", 0))
    fresh = entries[max(0, len(entries) - (total - base_total)):]
    loads = table_loads(fresh)
    doc["loads"] = loads
    print(f"\n{len(loads)} load(s) into the table region this run:")
    for l in loads:
        print(f"  LBA {l['lba']:>7} -> 0x{l['dest']:06X}  {l['size']} bytes")

    gaps = request_gaps(loads)
    doc["request_gaps"] = gaps
    for g in gaps:
        print(f"\nREQUEST GAP: dest advanced {g['dest_sectors']} sector(s) "
              f"but LBA advanced {g['lba_sectors']} "
              f"({g['from']['lba']} -> {g['to']['lba']})")

    try:
        tim = conn.cmd("cdrom_timing_dump", tail=4096,
                       lba_lo=args.lba_lo, lba_hi=args.lba_hi)
        doc["timing_records"] = tim.get("entries", [])
        seen = analyse_records(tim.get("entries", []))
        doc["delivery"] = {str(k): v for k, v in sorted(seen.items())}
        print(f"\ndelivery records for LBA {args.lba_lo}..{args.lba_hi}:")
        for lba in sorted(seen):
            r = seen[lba]
            marks = "".join(m for m, f in (("D", r["data"]), ("M", r["dma"]),
                                           ("P", r["pended"]), ("L", r["lost"]))
                            if f)
            print(f"  LBA {lba}: {r['records']} record(s) [{marks or '-'}]")
        missing = [l for l in range(125111, 125114) if l not in seen]
        if missing:
            print(f"  never read by the drive at all: {missing}")
    except DebugError as e:
        print(f"  timing dump unavailable ({e}) -- rebuild psx-runtime to "
              f"get per-sector records", file=sys.stderr)
        doc["timing_error"] = str(e)

    try:
        agg = conn.cmd("cdrom_timing")
        doc["timing_stats"] = {k: v for k, v in agg.items()
                               if k not in ("id", "ok")}
        print(f"\naggregates since arm: lost={agg.get('int1_lost', agg.get('lost'))} "
              f"pended={agg.get('pended')} over_sector={agg.get('exposure_over_sector', agg.get('over_sector'))}")
    except DebugError:
        pass

    blob = read_ram_range(conn, 0x80000000 + TABLE_LO,
                          ((TABLE_HI - TABLE_LO) & ~3) + 4)
    n = distinct_colours(blob)
    doc["table_distinct_colours"] = n
    print(f"\ntable region now holds {n} distinct colour word(s) "
          f"({'CORRECT' if n <= 8 else 'still corrupt'})")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"report: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
