#!/usr/bin/env python3
"""cd_diff.py -- diff the game's CD request streams between the two emulators.

    python3 tools/cd_diff.py            # both emulators at/after the scene

Every layer below the game has measured clean on psx-runtime: requests are
executed exactly (delivered == requested, data heads match the ISO), INT1s are
presented (lost=0), the arithmetic and tables downstream are correct. What
diverges is the game's OWN request stream -- native asks Setloc 125111 then
125113 where the oracle's table proves it asked for 125112. The game computes
those requests from what the emulator tells it, so the first differing command
between the two streams, and the traffic just before it, IS the runtime bug.

Both emulators now keep a flood-immune per-command history (psx-runtime:
cdrom_command_history; oracle: cd_history, added for this). Play the scene on
BOTH, then run this: it aligns the streams on the last common Setloc and
prints the fork.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import (  # noqa: E402
    DEFAULT_DUCKSTATION_PORT, DEFAULT_NATIVE_PORT, DebugConn, DebugError,
)
from cd_verify import CMD_NAMES, bcd  # noqa: E402

KIND = "psx-cd-diff"


def normalise(entries):
    """Common shape: [{cmd, params, lba?, frame}] oldest first."""
    out = []
    for e in entries:
        cmd = e.get("cmd")
        cmd = int(cmd, 16) if isinstance(cmd, str) else int(cmd or 0)
        ps = [int(x, 16) if isinstance(x, str) else int(x)
              for x in (e.get("params") or [])]
        row = {"cmd": CMD_NAMES.get(cmd, f"0x{cmd:02X}"), "params": ps,
               "frame": int(e.get("frame", 0))}
        if cmd == 0x02 and len(ps) >= 3:
            m, s, f = (bcd(x) for x in ps[:3])
            row["lba"] = (m * 60 + s) * 75 + f - 150
        out.append(row)
    return out


def command_key(row):
    return (row["cmd"], row.get("lba", tuple(row["params"])))


def find_fork(a, b, window):
    """Align the two streams on their last long common run inside `window`
    and return the index pair where they first differ afterwards."""
    ka = [command_key(r) for r in a]
    kb = [command_key(r) for r in b]
    best = None
    for i in range(len(ka)):
        for j in range(len(kb)):
            if ka[i] != kb[j]:
                continue
            n = 0
            while (i + n < len(ka) and j + n < len(kb)
                   and ka[i + n] == kb[j + n]):
                n += 1
            if n >= window and (best is None or n > best[2]):
                best = (i, j, n)
    if best is None:
        return None
    i, j, n = best
    return {"a_start": i, "b_start": j, "common": n,
            "a_fork": i + n, "b_fork": j + n}


def in_lba_range(rows, lo, hi):
    return [r for r in rows if lo <= r.get("lba", -1) <= hi
            or True]  # keep all; range used only for reporting focus


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--ds-port", type=int, default=DEFAULT_DUCKSTATION_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--count", type=int, default=2048)
    ap.add_argument("--window", type=int, default=6,
                    help="minimum matching run to anchor the alignment")
    ap.add_argument("--out", default="analysis/frames/cd_diff.json")
    args = ap.parse_args()

    doc = {"kind": KIND, "version": 1}
    try:
        nat = normalise(DebugConn(args.host, args.port, args.timeout)
                        .cmd("cdrom_command_history",
                             count=args.count).get("entries", []))
    except DebugError as e:
        print(f"psx-runtime: {e}", file=sys.stderr)
        return 2
    try:
        orc = normalise(DebugConn(args.host, args.ds_port, args.timeout)
                        .cmd("cd_history", count=args.count)
                        .get("entries", []))
    except DebugError as e:
        print(f"oracle: {e} -- rebuild it (duckstation_oracle.py build + "
              f"install) and replay the scene there", file=sys.stderr)
        return 2

    # psx-runtime's history is newest-first; oracle's is oldest-first.
    if nat and len(nat) > 1 and nat[0]["frame"] > nat[-1]["frame"]:
        nat = nat[::-1]
    if orc and len(orc) > 1 and orc[0]["frame"] > orc[-1]["frame"]:
        orc = orc[::-1]
    doc["native_count"], doc["oracle_count"] = len(nat), len(orc)
    print(f"native: {len(nat)} commands; oracle: {len(orc)} commands")

    fork = find_fork(nat, orc, args.window)
    doc["fork"] = fork
    if not fork:
        print("no common run found to align on -- were both histories "
              "captured over the same scene?")
        _save(doc, args)
        return 1

    ia, ib, n = fork["a_fork"], fork["b_fork"], fork["common"]
    print(f"\naligned on a common run of {n} commands; showing the fork:")

    def show(rows, idx, label):
        print(f"\n  {label}:")
        for k in range(max(0, idx - 6), min(len(rows), idx + 8)):
            r = rows[k]
            lba = f" -> lba {r['lba']}" if "lba" in r else ""
            mark = "  <<< FIRST DIVERGED" if k == idx else ""
            print(f"    f{r['frame']}: {r['cmd']}"
                  f"({' '.join(f'{x:02X}' for x in r['params'])}){lba}{mark}")

    show(nat, ia, "psx-runtime")
    show(orc, ib, "oracle")

    if ia < len(nat) and ib < len(orc):
        a, b = nat[ia], orc[ib]
        print(f"\nFORK: native issued {a['cmd']}"
              f"{'(lba ' + str(a.get('lba')) + ')' if 'lba' in a else ''} "
              f"where the oracle issued {b['cmd']}"
              f"{'(lba ' + str(b.get('lba')) + ')' if 'lba' in b else ''}.")
        print("The game computes its requests from what the emulator tells "
              "it, so the runtime's divergence happened in the traffic just "
              "before this point.")
    _save(doc, args)
    return 0


def _save(doc, args):
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1)
    print(f"\nreport: {args.out}")


if __name__ == "__main__":
    sys.exit(main())
