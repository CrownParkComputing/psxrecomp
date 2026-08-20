"""Container format registry.

A handler module exposes ``probe(data) -> bool`` and
``unpack(data) -> list[ContainerEntry]``. Entries whose codec is unknown come
back with ``data=None`` and their packed payload in ``packed_data`` so they
can still be carved to disk for later work.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class ContainerEntry:
    index: int
    offset: int
    unpacked_size: int
    packed_size: int
    stored: bool                  # True: data is the final payload
    data: bytes | None            # decoded payload, if available
    packed_data: bytes | None = None  # raw compressed payload otherwise


def get_handler(name: str):
    if name == "psyg_pb":
        from . import psyg_pb
        return psyg_pb
    raise KeyError(f"unknown container format: {name}")


def probe_all(data: bytes) -> str | None:
    """Return the name of the first handler whose probe accepts ``data``."""
    for name in ("psyg_pb",):
        if get_handler(name).probe(data):
            return name
    return None
