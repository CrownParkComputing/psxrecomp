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
            row = {"lba": int(e["lba"]), "dest": dest, "size": size}
            if "delivered_lba" in e:
                row["delivered_lba"] = int(e["delivered_lba"])
            if "first_words" in e:
                row["first_words"] = e["first_words"]
            out.append(row)
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


CMD_NAMES = {
    0x01: "Getstat", 0x02: "Setloc", 0x03: "Play", 0x06: "ReadN",
    0x08: "Stop", 0x09: "Pause", 0x0A: "Init", 0x0B: "Mute",
    0x0C: "Demute", 0x0D: "Setfilter", 0x0E: "Setmode", 0x0F: "Getparam",
    0x10: "GetlocL", 0x11: "GetlocP", 0x13: "GetTN", 0x14: "GetTD",
    0x15: "SeekL", 0x16: "SeekP", 0x19: "Test", 0x1A: "GetID",
    0x1B: "ReadS", 0x1E: "ReadTOC",
}


def bcd(v):
    return (v >> 4) * 10 + (v & 0x0F)


def transcript(entries):
    """The game's CD conversation, reconstructed from the register trace.

    'W' to 0x1F801802 while gathering = param bytes; 'C' = command issue with
    the code in val; 'R' from 0x1F801801 afterwards = the response bytes the
    game actually READ. This is the ground truth for what the game asked and
    what the runtime told it -- the thing that decides whether the game's
    SetLoc(125113) was computed from a number we gave it.
    """
    out = []
    params = []
    cur = None
    for e in entries:
        k = e.get("kind")
        addr = int(e.get("addr", "0x0"), 16) if isinstance(e.get("addr"), str) \
            else int(e.get("addr", 0))
        val = int(e.get("val", "0x0"), 16) if isinstance(e.get("val"), str) \
            else int(e.get("val", 0))
        if k == "write" and (addr & 0xF) == 2:
            params.append(val & 0xFF)
        elif k == "cmd":
            cmd = val & 0xFF
            cur = {"cmd": CMD_NAMES.get(cmd, f"0x{cmd:02X}"),
                   "params": params[-8:], "resp": [],
                   "frame": e.get("frame")}
            if cmd == 0x02 and len(params) >= 3:
                m, s_, f = (bcd(x) for x in params[-3:])
                cur["lba"] = (m * 60 + s_) * 75 + f - 150
            params = []
            out.append(cur)
        elif k == "read" and (addr & 0xF) == 1 and cur is not None:
            cur["resp"].append(val & 0xFF)
        elif k == "sector":
            out.append({"sector_lba": val, "frame": e.get("frame")})
    return out


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

    print("armed. Play into the scene AND START THE ANIMATION (the X press); "
          "the palette load happens at the animation itself, not when the "
          "map appears. Watching for a load from LBA "
          f"{args.lba_lo}..{args.lba_hi} …", flush=True)
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
        # Trigger on the CRITICAL load -- the one from the palette's own LBA
        # neighbourhood. Earlier scene loads also touch these addresses (the
        # region is a general-purpose buffer); stopping at the first of those
        # is how a previous run of this tool analysed the wrong load and
        # called a zero-filled table 'CORRECT'.
        if any(args.lba_lo <= l["lba"] <= args.lba_hi for l in loads):
            break
        time.sleep(1.0)
    else:
        seen = sorted({l["lba"] for l in loads})
        print(f"no load from LBA {args.lba_lo}..{args.lba_hi} appeared. "
              f"Loads that did touch the region came from LBAs {seen} -- if "
              f"the animation was played, the palette came from one of "
              f"those; rerun with --lba-lo/--lba-hi around it.")
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
        # delivered_lba and first_words exist once psx-runtime is rebuilt with
        # the extended log; older binaries just omit them.
        src = next((e for e in fresh
                    if (int(e["dest"], 16) & 0x1FFFFF) == l["dest"]
                    and int(e["size"]) == l["size"]
                    and int(e["lba"]) == l["lba"]), {})
        dlv = src.get("delivered_lba")
        fw = src.get("first_words")
        extra = ""
        if dlv is not None:
            mark = "" if dlv == l["lba"] else "  <-- continuation/desync"
            extra = f"  delivered={dlv}{mark}"
        if fw:
            extra += f"  data={fw[0]},{fw[1]}"
        print(f"  LBA {l['lba']:>7} -> 0x{l['dest']:06X}  {l['size']} bytes"
              f"{extra}")

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

    # The command transcript around the load: what the game asked, and the
    # response bytes it read back. This decides whether SetLoc(125113) was
    # computed from a position WE reported.
    try:
        tr = conn.cmd("cdrom_trace_dump", count=20000)
        conv = transcript(tr.get("entries", []))
        doc["transcript"] = conv[-400:]
        interesting = [c for c in conv if "lba" in c or
                       c.get("cmd") in ("GetlocL", "GetlocP", "Pause",
                                        "ReadN", "ReadS", "SeekL")]
        print(f"\ncommand transcript (position-relevant, last 30):")
        for c in interesting[-30:]:
            if "sector_lba" in c:
                continue
            lba = f" lba={c['lba']}" if "lba" in c else ""
            resp = ""
            if c.get("resp") and c.get("cmd") in ("GetlocL", "GetlocP"):
                b = c["resp"]
                resp = " resp=" + " ".join(f"{x:02X}" for x in b[:10])
                if c["cmd"] == "GetlocL" and len(b) >= 4:
                    resp += (f" -> lba {(bcd(b[1])*60 + bcd(b[2]))*75 + bcd(b[3]) - 150}")
                if c["cmd"] == "GetlocP" and len(b) >= 8:
                    resp += (f" -> abs lba {(bcd(b[6])*60 + bcd(b[7]))*75 + bcd(b[8]) - 150}"
                             if len(b) >= 9 else "")
            print(f"  f{c.get('frame')}: {c['cmd']}"
                  f"({' '.join(f'{x:02X}' for x in c.get('params', []))})"
                  f"{lba}{resp}")
    except DebugError as e:
        print(f"  trace dump unavailable ({e})", file=sys.stderr)

    blob = read_ram_range(conn, 0x80000000 + TABLE_LO,
                          ((TABLE_HI - TABLE_LO) & ~3) + 4)
    n = distinct_colours(blob)
    doc["table_distinct_colours"] = n
    words = {int.from_bytes(blob[i:i + 4], "little") & 0xFFFFFF
             for i in range(0, len(blob) - 3, 4)}
    # The palette's own bright words, from the ISO's sector 125112. Presence
    # decides -- a low count alone also matches a zero-filled buffer that has
    # not been loaded yet, which a previous run miscalled 'CORRECT'.
    has_palette = 0x0888F8 in words and 0xB0F8F8 in words
    if has_palette and n <= 8:
        state = "CORRECT: the 5-colour palette is in place"
    elif n > 32:
        state = "STILL CORRUPT: raw file data"
    else:
        state = ("NOT LOADED YET: neither the palette nor the corruption -- "
                 "the reading happened at the wrong moment")
    doc["table_state"] = state
    print(f"\ntable region now holds {n} distinct colour word(s) -- {state}")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"report: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
