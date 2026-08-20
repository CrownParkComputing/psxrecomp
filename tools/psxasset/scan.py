"""Signature scanner for unknown blobs.

Signature candidates are located with ``bytes.find`` (C speed — a naive
per-byte Python loop takes minutes on a 300 MB file), then structurally
validated, then overlap-filtered so a hit inside an already-claimed span is
dropped. Findings are plain dicts so they serialize straight into a report.
"""

from __future__ import annotations

import struct

from .formats import tim, vag


def _find_all(data: bytes, needle: bytes, align: int = 1):
    pos = data.find(needle)
    while pos != -1:
        if pos % align == 0:
            yield pos
        pos = data.find(needle, pos + 1)


def scan(data: bytes) -> list[dict]:
    candidates: list[dict] = []

    for pos in _find_all(data, tim.MAGIC):
        try:
            img = tim.parse(data, pos)
        except tim.TimError:
            continue
        candidates.append({
            "type": "tim", "offset": pos, "size": img.byte_size,
            "width": img.width, "height": img.height, "bpp": img.bpp,
            "palettes": img.palette_count,
        })

    for pos in _find_all(data, b"VAGp"):
        info = vag.probe(data, pos)
        if info:
            candidates.append({
                "type": "vag", "offset": pos, "size": 48 + info.data_size,
                "rate": info.rate, "name": info.name,
            })

    for pos in _find_all(data, b"pQES"):
        candidates.append({"type": "seq", "offset": pos, "size": 4})
    for pos in _find_all(data, b"pBAV"):
        candidates.append({"type": "vab_header", "offset": pos, "size": 4})
    for pos in _find_all(data, b"PS-X EXE"):
        candidates.append({"type": "psx_exe", "offset": pos, "size": 8})

    # TMD candidate: version 0x41, flags 0/1, sane object count. Low
    # confidence — flagged with a trailing '?' and given no claimed span.
    for pos in _find_all(data, b"\x41\x00\x00\x00", align=4):
        if pos + 12 > len(data):
            continue
        flags, nobj = struct.unpack("<II", data[pos + 4:pos + 12])
        if flags <= 1 and 0 < nobj <= 5000:
            candidates.append({"type": "tmd?", "offset": pos, "size": 0,
                               "objects": nobj})

    candidates.sort(key=lambda f: (f["offset"], -f.get("size", 0)))
    findings: list[dict] = []
    claimed_end = 0
    for f in candidates:
        if f["offset"] < claimed_end:
            continue
        findings.append(f)
        claimed_end = max(claimed_end, f["offset"] + f.get("size", 0))

    # Headerless VAG (Psygnosis variant) at blob start only — the heuristic
    # is too loose to run at every offset.
    if not findings:
        info = vag.probe(data, 0)
        if info:
            findings.append({
                "type": "vag_headerless", "offset": 0,
                "size": 48 + info.data_size, "rate": info.rate,
            })
    return findings
