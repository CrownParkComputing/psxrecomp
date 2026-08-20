"""Minimal RGBA8 PNG writer (stdlib zlib only)."""

from __future__ import annotations

import struct
import zlib
from pathlib import Path


def _chunk(tag: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload)))


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    if len(rgba) != width * height * 4:
        raise ValueError("rgba buffer does not match dimensions")
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter: none
        raw += rgba[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    data = (b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + _chunk(b"IEND", b""))
    Path(path).write_bytes(data)
