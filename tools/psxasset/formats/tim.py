"""TIM texture parser and RGBA conversion.

Alpha tiers follow the pack-authoring convention used elsewhere in this repo
(``tools/textures/rescale_blocky.py``): fully transparent 0, semi-transparent
(STP bit) 127, opaque 255. Black-with-STP is the PSX "opaque black" case and
exports as opaque.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = b"\x10\x00\x00\x00"
_BPP = {0: 4, 1: 8, 2: 16, 3: 24}


class TimError(ValueError):
    pass


@dataclass
class TimImage:
    pmode: int                    # 0=4bpp 1=8bpp 2=16bpp 3=24bpp
    vram_x: int
    vram_y: int
    width: int                    # in pixels, not VRAM halfwords
    height: int
    pixel_data: bytes             # raw pixel block payload (no header)
    clut_x: int = 0
    clut_y: int = 0
    palettes: list[bytes] = field(default_factory=list)  # raw u16le rows
    byte_size: int = 0            # total bytes consumed from the source blob

    @property
    def bpp(self) -> int:
        return _BPP[self.pmode]

    @property
    def palette_count(self) -> int:
        return len(self.palettes)


def _rgba(c: int) -> tuple[int, int, int, int]:
    if c == 0:
        return 0, 0, 0, 0
    r = (c & 31) << 3
    g = ((c >> 5) & 31) << 3
    b = ((c >> 10) & 31) << 3
    r |= r >> 5
    g |= g >> 5
    b |= b >> 5
    a = 127 if (c >> 15) and (c & 0x7FFF) else 255
    return r, g, b, a


def parse(data: bytes, offset: int = 0) -> TimImage:
    """Parse one TIM at ``offset``. Raises :class:`TimError` on anything
    structurally wrong, so it doubles as a validator for signature scanning."""
    d = data[offset:]
    if len(d) < 20 or d[:4] != MAGIC:
        raise TimError("bad magic")
    flags = struct.unpack("<I", d[4:8])[0]
    if flags & ~0x0B:
        raise TimError(f"bad flags {flags:#x}")
    pmode = flags & 3
    pos = 8

    clut_x = clut_y = 0
    palettes: list[bytes] = []
    if flags & 8:
        if pos + 12 > len(d):
            raise TimError("truncated CLUT header")
        blk_len, cx, cy, cw, ch = struct.unpack("<IHHHH", d[pos:pos + 12])
        if blk_len < 12 or pos + blk_len > len(d) or cw == 0 or ch == 0:
            raise TimError("bad CLUT block")
        if blk_len != 12 + cw * ch * 2:
            raise TimError("CLUT size mismatch")
        clut_x, clut_y = cx, cy
        body = d[pos + 12:pos + blk_len]
        row = cw * 2
        palettes = [body[i * row:(i + 1) * row] for i in range(ch)]
        pos += blk_len

    if pos + 12 > len(d):
        raise TimError("truncated pixel header")
    blk_len, px, py, pw, ph = struct.unpack("<IHHHH", d[pos:pos + 12])
    if blk_len < 12 or pos + blk_len > len(d) or pw == 0 or ph == 0:
        raise TimError("bad pixel block")
    if blk_len != 12 + pw * ph * 2:
        raise TimError("pixel size mismatch")
    if pw > 1024 or ph > 512:
        raise TimError("pixel rect exceeds VRAM")
    body = d[pos + 12:pos + blk_len]
    pos += blk_len

    if pmode == 0:
        width = pw * 4
    elif pmode == 1:
        width = pw * 2
    elif pmode == 2:
        width = pw
    else:
        if (pw * 2) % 3:
            raise TimError("24bpp width not divisible")
        width = pw * 2 // 3

    return TimImage(pmode=pmode, vram_x=px, vram_y=py, width=width, height=ph,
                    pixel_data=body, clut_x=clut_x, clut_y=clut_y,
                    palettes=palettes, byte_size=pos)


def to_rgba(tim: TimImage, palette: int = 0) -> bytes:
    """Decode to an RGBA8 buffer of ``tim.width * tim.height`` pixels."""
    w, h = tim.width, tim.height
    out = bytearray(w * h * 4)
    src = tim.pixel_data

    if tim.pmode in (0, 1):
        if not tim.palettes:
            # CLUT-less paletted TIM: fall back to greyscale indices.
            pal16 = [(i * 17,) * 3 + (255,) for i in range(16)]
            pal256 = [(i,) * 3 + (255,) for i in range(256)]
            pal = pal16 if tim.pmode == 0 else pal256
        else:
            raw = tim.palettes[min(palette, len(tim.palettes) - 1)]
            colors = struct.unpack(f"<{len(raw) // 2}H", raw)
            pal = [_rgba(c) for c in colors]
            need = 16 if tim.pmode == 0 else 256
            if len(pal) < need:
                pal = list(pal) + [(0, 0, 0, 0)] * (need - len(pal))
        i = 0
        if tim.pmode == 0:
            for byte in src:
                for idx in (byte & 0xF, byte >> 4):
                    if i >= w * h:
                        break
                    out[i * 4:i * 4 + 4] = bytes(pal[idx])
                    i += 1
        else:
            for byte in src:
                if i >= w * h:
                    break
                out[i * 4:i * 4 + 4] = bytes(pal[byte])
                i += 1
    elif tim.pmode == 2:
        colors = struct.unpack(f"<{w * h}H", src[:w * h * 2])
        for i, c in enumerate(colors):
            out[i * 4:i * 4 + 4] = bytes(_rgba(c))
    else:  # 24bpp
        for i in range(w * h):
            r, g, b = src[i * 3:i * 3 + 3]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    return bytes(out)
