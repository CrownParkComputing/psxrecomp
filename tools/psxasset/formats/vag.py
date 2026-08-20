"""VAG / SPU-ADPCM decoder.

Handles standard ``VAGp`` headers, the headerless 48-byte variant seen inside
Psygnosis archives (magic zeroed, big-endian size/rate fields still valid),
and raw block streams with a caller-supplied rate.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

HEADER = 48
_FILTERS = ((0.0, 0.0), (60 / 64, 0.0), (115 / 64, -52 / 64),
            (98 / 64, -55 / 64), (122 / 64, -60 / 64))
PLAUSIBLE_RATES = frozenset(
    {4000, 8000, 11025, 12000, 16000, 18900, 22050, 24000, 32000, 37800, 44100, 48000})


@dataclass(frozen=True)
class VagInfo:
    rate: int
    data_off: int
    data_size: int
    name: str        # embedded source filename, often survives from the build
    headered: bool


def probe(data: bytes, offset: int = 0) -> VagInfo | None:
    """Recognize a headered or headerless-but-structured VAG at ``offset``."""
    d = data[offset:offset + HEADER]
    if len(d) < HEADER:
        return None
    magic = d[:4]
    size, rate = struct.unpack(">II", d[12:20])
    name = d[32:48].split(b"\x00")[0].decode("latin1", errors="replace")
    if magic == b"VAGp":
        if size == 0 or size > len(data) - offset:
            return None
        return VagInfo(rate, offset + HEADER, size, name, True)
    if magic == b"\x00\x00\x00\x00" and rate in PLAUSIBLE_RATES:
        if 16 <= size <= len(data) - offset and size % 16 == 0:
            return VagInfo(rate, offset + HEADER, size, name, False)
    return None


def decode(data: bytes) -> list[int]:
    """Decode a raw SPU-ADPCM block stream to signed 16-bit samples."""
    out: list[int] = []
    s1 = s2 = 0.0
    for i in range(0, len(data) - 15, 16):
        blk = data[i:i + 16]
        shift = blk[0] & 0xF
        pred = min(blk[0] >> 4, 4)
        flags = blk[1]
        if flags == 7:      # end marker block, not audio
            break
        f0, f1 = _FILTERS[pred]
        for j in range(2, 16):
            byte = blk[j]
            for nib in (byte & 0xF, byte >> 4):
                s = nib << 12
                if s & 0x8000:
                    s -= 0x10000
                s >>= shift
                v = s + s1 * f0 + s2 * f1
                s2, s1 = s1, v
                out.append(max(-32768, min(32767, int(round(v)))))
        if flags & 1:       # loop-end: sample stops here
            break
    return out


def pcm_bytes(samples: list[int]) -> bytes:
    return struct.pack(f"<{len(samples)}h", *samples)
