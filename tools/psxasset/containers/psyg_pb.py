"""Psygnosis .PB / .PBP archive container (WipEout 3 era).

Layout (verified against SCES-02845 — every entry chains exactly):

    0x00  80 bytes  zero / reserved
    0x50  u32       entry count
    0x54  u32       0
    0x58  count x { u32 offset, u32 unpacked_size, u32 packed_size }
          payload follows; entries with packed == unpacked are stored
          uncompressed (36-54% of bytes per archive on WipEout 3)

Compressed entries use a bespoke Psygnosis codec that is NOT a stock LZSS
(exhaustively ruled out); they are surfaced as packed blobs until the game's
own decompressor is recovered via the recomp (see docs/MOD_PACKAGES.md for
the plugin route).
"""

from __future__ import annotations

import struct

from . import ContainerEntry

_HDR = 0x50
_TAB = 0x58


def probe(data: bytes) -> bool:
    if len(data) < _TAB + 12 or data[:_HDR] != b"\x00" * _HDR:
        return False
    count, zero = struct.unpack("<II", data[_HDR:_TAB])
    if zero != 0 or not 0 < count < 8192:
        return False
    end = _TAB + count * 12
    if end > len(data):
        return False
    prev_end = end
    for i in range(count):
        off, unp, pkd = struct.unpack("<3I", data[_TAB + i * 12:_TAB + i * 12 + 12])
        if off < prev_end - 8 or off + pkd > len(data) or pkd == 0 or pkd > unp:
            return False
        prev_end = off + pkd
    return True


def unpack(data: bytes) -> list[ContainerEntry]:
    count = struct.unpack("<I", data[_HDR:_HDR + 4])[0]
    entries: list[ContainerEntry] = []
    for i in range(count):
        off, unp, pkd = struct.unpack("<3I", data[_TAB + i * 12:_TAB + i * 12 + 12])
        blob = data[off:off + pkd]
        if pkd == unp:
            entries.append(ContainerEntry(i, off, unp, pkd, True, blob))
        else:
            entries.append(ContainerEntry(i, off, unp, pkd, False, None, blob))
    return entries
