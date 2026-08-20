"""Cue/bin disc access and a minimal ISO9660 filesystem walker.

Handles raw MODE2/2352 track images (Redump), cooked 2048-byte images, and
cue sheets (the first ``FILE`` entry is taken as the data track — audio
tracks never carry filesystem data). All the recomp game repos keep discs in
this shape already (see ``prepare_disc.py``).
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

RAW_SEC = 2352
USER = 2048
USER_OFF = 24  # Mode2 Form1: sync(12) + header(4) + subheader(8)
SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])


class DiscError(RuntimeError):
    pass


class DiscImage:
    """Sector-level access to the data track of a disc image."""

    def __init__(self, path: Path):
        path = Path(path)
        if path.suffix.lower() == ".cue":
            bin_path = self._first_file_from_cue(path)
        else:
            bin_path = path
        if not bin_path.is_file():
            raise DiscError(f"data track not found: {bin_path}")
        self.path = bin_path
        self._fh = open(bin_path, "rb")
        self.sector_size, self.user_off = self._detect_layout()
        self.sector_count = bin_path.stat().st_size // self.sector_size

    @staticmethod
    def _first_file_from_cue(cue_path: Path) -> Path:
        text = cue_path.read_text(errors="replace")
        m = re.search(r'FILE\s+"([^"]+)"', text) or re.search(r"FILE\s+(\S+)", text)
        if not m:
            raise DiscError(f"no FILE entry in cue: {cue_path}")
        return cue_path.parent / m.group(1)

    def _detect_layout(self) -> tuple[int, int]:
        self._fh.seek(0)
        head = self._fh.read(RAW_SEC)
        if head[:12] == SYNC:
            return RAW_SEC, USER_OFF
        # Cooked image: PVD magic should appear at LBA 16 with 2048 sectors.
        self._fh.seek(16 * USER)
        if self._fh.read(6)[1:6] == b"CD001":
            return USER, 0
        # Fall back to raw if the size fits; 2448 dumps get trimmed reads.
        size = self.path.stat().st_size
        if size % RAW_SEC == 0:
            return RAW_SEC, USER_OFF
        if size % 2448 == 0:
            return 2448, USER_OFF
        raise DiscError(f"unrecognized sector layout: {self.path}")

    def read_sector(self, lba: int) -> bytes:
        """Return the 2048 user bytes of one sector."""
        self._fh.seek(lba * self.sector_size + self.user_off)
        return self._fh.read(USER)

    def read_span(self, lba: int, size: int) -> bytes:
        n = (size + USER - 1) // USER
        out = bytearray()
        for i in range(n):
            out += self.read_sector(lba + i)
        return bytes(out[:size])

    def close(self) -> None:
        self._fh.close()


@dataclass(frozen=True)
class IsoEntry:
    path: str      # "/WIPEOUT3/TRACKM01.PBP" — version suffix stripped
    lba: int
    size: int
    is_dir: bool


class IsoFs:
    """ISO9660 walker over a :class:`DiscImage`."""

    def __init__(self, disc: DiscImage):
        self.disc = disc
        pvd = disc.read_sector(16)
        if pvd[1:6] != b"CD001":
            raise DiscError("no ISO9660 primary volume descriptor at LBA 16")
        self.volume_id = pvd[40:72].decode("latin1").strip()
        root = pvd[156:156 + 34]
        self._root_lba = struct.unpack("<I", root[2:6])[0]
        self._root_size = struct.unpack("<I", root[10:14])[0]

    def _read_dir(self, lba: int, size: int) -> Iterator[tuple[str, int, int, bool]]:
        data = self.disc.read_span(lba, size)
        off = 0
        while off < len(data):
            rec_len = data[off]
            if rec_len == 0:
                # Records never span sectors; skip to the next one.
                off = (off // USER + 1) * USER
                continue
            rec = data[off:off + rec_len]
            elba = struct.unpack("<I", rec[2:6])[0]
            esize = struct.unpack("<I", rec[10:14])[0]
            flags = rec[25]
            nlen = rec[32]
            raw_name = rec[33:33 + nlen]
            off += rec_len
            if raw_name in (b"\x00", b"\x01"):
                continue
            name = raw_name.decode("latin1").split(";")[0]
            yield name, elba, esize, bool(flags & 2)

    def walk(self) -> Iterator[IsoEntry]:
        stack = [("", self._root_lba, self._root_size)]
        seen = set()
        while stack:
            prefix, lba, size = stack.pop()
            if lba in seen:
                continue
            seen.add(lba)
            for name, elba, esize, is_dir in self._read_dir(lba, size):
                path = f"{prefix}/{name}"
                yield IsoEntry(path, elba, esize, is_dir)
                if is_dir:
                    stack.append((path, elba, esize))

    def files(self) -> list[IsoEntry]:
        return sorted((e for e in self.walk() if not e.is_dir), key=lambda e: e.path)

    def find(self, path: str) -> IsoEntry | None:
        want = "/" + path.strip("/").upper()
        for entry in self.walk():
            if entry.path.upper() == want:
                return entry
        return None

    def read(self, entry: IsoEntry) -> bytes:
        return self.disc.read_span(entry.lba, entry.size)

    def serial(self) -> str | None:
        """Read the boot serial (e.g. ``SCES-02845``) from SYSTEM.CNF."""
        entry = self.find("SYSTEM.CNF")
        if entry is None:
            return None
        text = self.read(entry).decode("latin1", errors="replace")
        m = re.search(r"BOOT\s*=\s*cdrom:\\?([A-Z]{4})_(\d{3})\.(\d{2})", text)
        if not m:
            return None
        return f"{m.group(1)}-{m.group(2)}{m.group(3)}"
