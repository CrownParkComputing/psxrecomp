"""Per-game format registry.

One TOML per title in ``games/``, keyed by boot serial. This is what keeps
psxasset general: the engine knows formats, the registry knows which game
uses which. Schema:

    [game]
    serial = "SCES-02845"
    name   = "WipEout 3 Special Edition"

    [[container]]
    glob   = "WIPEOUT3/*.PBP"     # against the ISO path, case-insensitive
    format = "psyg_pb"

    [[skip]]
    glob   = "*.STR"              # rip copies these but doesn't scan them
    reason = "FMV stream"
"""

from __future__ import annotations

import fnmatch
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

GAMES_DIR = Path(__file__).resolve().parent / "games"


@dataclass
class GameRegistry:
    serial: str
    name: str
    containers: list[tuple[str, str]] = field(default_factory=list)  # (glob, format)
    skips: list[tuple[str, str]] = field(default_factory=list)       # (glob, reason)
    source: Path | None = None

    def container_format(self, iso_path: str) -> str | None:
        rel = iso_path.lstrip("/").upper()
        for glob, fmt in self.containers:
            if fnmatch.fnmatch(rel, glob.upper()):
                return fmt
        return None

    def skip_reason(self, iso_path: str) -> str | None:
        rel = iso_path.lstrip("/").upper()
        for glob, reason in self.skips:
            if fnmatch.fnmatch(rel, glob.upper()):
                return reason
        return None


def _load(path: Path) -> GameRegistry:
    doc = tomllib.loads(path.read_text())
    game = doc.get("game", {})
    reg = GameRegistry(serial=game.get("serial", ""), name=game.get("name", ""),
                       source=path)
    for c in doc.get("container", []):
        reg.containers.append((c["glob"], c["format"]))
    for s in doc.get("skip", []):
        reg.skips.append((s["glob"], s.get("reason", "")))
    return reg


def load_all(extra_dir: Path | None = None) -> list[GameRegistry]:
    regs = []
    dirs = [GAMES_DIR] + ([extra_dir] if extra_dir else [])
    for d in dirs:
        if d and d.is_dir():
            for path in sorted(d.glob("*.toml")):
                regs.append(_load(path))
    return regs


def find(serial: str | None, extra_dir: Path | None = None) -> GameRegistry | None:
    if not serial:
        return None
    for reg in load_all(extra_dir):
        if reg.serial.upper() == serial.upper():
            return reg
    return None
