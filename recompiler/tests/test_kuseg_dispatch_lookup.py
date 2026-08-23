#!/usr/bin/env python3
"""Exhaustively verify generated O(1) game dispatch and PS1 aliases.

The generated map is two-level and bounded: a 512- or 2048-element 4 KiB page table,
then one 1024-element instruction-word table for each populated page. Every
lookup therefore uses a constant number of masks, bounds checks and loads; it
does not compare/search as the number of compiled entries grows.

By default this synthesizes a small KUSEG PS-X EXE and drives psxrecomp-game.
``--dispatch-source`` instead verifies every entry in an existing generated
dispatcher; the WipEout acceptance run exercises all 23,336 records.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

LOAD = 0x00010000
PHYS_MASK = 0x1FFFFFFF
SLOTS_PER_PAGE = 4096 // 4


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, entry)
    struct.pack_into("<I", header, 0x18, load)
    struct.pack_into("<I", header, 0x1C, len(data))
    struct.pack_into("<I", header, 0x30, 0x801FFFF0)
    return bytes(header) + data


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def build_exe():
    body = bytearray(w([
        0x27BDFFF8, jal(LOAD + 0x20), 0x00000000,
        0x27BD0008, 0x03E00008, 0x00000000,
    ]))
    body += b"\x00" * (0x20 - len(body))
    body += w([0x03E00008, 0x00000000])
    return make_psxexe(LOAD, LOAD, bytes(body))


def generate_dispatch(recompiler, tmp):
    psx = os.path.join(tmp, "t.psx")
    seeds = os.path.join(tmp, "seeds.txt")
    out = os.path.join(tmp, "out")
    os.makedirs(out, exist_ok=True)
    Path(psx).write_bytes(build_exe())
    Path(seeds).write_text(
        "0x%08X\n0x%08X\n" % (LOAD, LOAD + 0x20), encoding="utf-8")
    result = subprocess.run(
        [recompiler, psx, "--seeds", seeds, "--out-dir", out],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("recompiler failed:\n" +
                         (result.stderr or result.stdout))
    dispatches = list(Path(out).glob("*_dispatch.c"))
    if len(dispatches) != 1:
        raise SystemExit(f"expected one _dispatch.c in {out}, got {len(dispatches)}")
    return dispatches[0].read_text(encoding="utf-8")


def parse_dispatch(source, ram_size):
    table = re.search(
        r"static const PsxGameDispatchEntry k_psx_game_dispatch\[\] = \{"
        r"(.*?)\n\};", source, re.DOTALL)
    if not table:
        raise AssertionError("dispatch record table not found")
    keys = [
        int(value, 16) & PHYS_MASK
        for value in re.findall(r"\{0x([0-9A-Fa-f]{8})u,", table.group(1))
    ]

    pages = {}
    page_pattern = re.compile(
        r"static const PsxGameDispatchIndex "
        r"k_psx_game_dispatch_page_([0-9A-Fa-f]{3})\[1024\] = \{"
        r"(.*?)\n\};", re.DOTALL)
    for match in page_pattern.finditer(source):
        page = int(match.group(1), 16)
        values = [int(value) for value in re.findall(r"(\d+)u", match.group(2))]
        if len(values) != SLOTS_PER_PAGE:
            raise AssertionError(
                f"page {page:03X} has {len(values)} slots, expected 1024")
        if page in pages:
            raise AssertionError(f"duplicate generated page {page:03X}")
        pages[page] = values

    page_table = re.search(
        r"k_psx_game_dispatch_pages\[\] = \{(.*?)\n\};",
        source, re.DOTALL)
    if not page_table:
        raise AssertionError("dispatch page-pointer table not found")
    pointers = re.findall(r"k_psx_game_dispatch_page_([0-9A-Fa-f]{3})|\b0\b",
                          page_table.group(1))
    # Alternation returns an empty capture for zero; retain positional count.
    raw_tokens = re.findall(r"k_psx_game_dispatch_page_[0-9A-Fa-f]{3}|\b0\b",
                            page_table.group(1))
    page_count = ram_size // 4096
    if len(raw_tokens) != page_count:
        raise AssertionError(
            f"page table has {len(raw_tokens)} entries, expected {page_count}")
    pointer_pages = {
        index: int(token.rsplit("_", 1)[1], 16)
        for index, token in enumerate(raw_tokens) if token != "0"
    }
    if any(index != page for index, page in pointer_pages.items()):
        raise AssertionError("page-pointer table points at the wrong physical page")
    if set(pointer_pages) != set(pages):
        raise AssertionError("page-pointer table and emitted page arrays disagree")
    return keys, pages


def mapped_index(pages, addr, ram_size):
    phys = addr & PHYS_MASK
    if phys >= ram_size or (phys & 3):
        return None
    page = pages.get(phys >> 12)
    if page is None:
        return None
    encoded = page[(phys & 0xFFF) >> 2]
    return None if encoded == 0 else encoded - 1


def verify(source):
    find = re.search(
        r"static const PsxGameDispatchEntry\* psx_game_find_entry"
        r"\(uint32_t addr\) \{(.*?)\n\}", source, re.DOTALL)
    if not find:
        raise AssertionError("psx_game_find_entry not found")
    body = find.group(1)
    ram_guard = re.search(r"phys >= 0x([0-9A-Fa-f]+)u", body)
    if not ram_guard:
        raise AssertionError("direct lookup is missing a concrete RAM bound")
    ram_size = int(ram_guard.group(1), 16)
    if ram_size not in (2 * 1024 * 1024, 8 * 1024 * 1024):
        raise AssertionError(f"unsupported generated RAM size 0x{ram_size:X}")
    required = (
        "addr & 0x1FFFFFFFu",
        "(phys & 3u) != 0u",
        "k_psx_game_dispatch_pages[phys >> 12]",
        "page[(phys & 0xFFFu) >> 2]",
        "encoded == 0u",
        "encoded > PSX_GAME_DISPATCH_COUNT",
        "entry->addr & 0x1FFFFFFFu",
    )
    for fragment in required:
        if fragment not in body:
            raise AssertionError(f"direct lookup missing guard: {fragment}")
    if "while (" in body or "k_psx_game_dispatch[mid]" in body:
        raise AssertionError("dispatch lookup regressed to entry-count search")

    # The direct hit must also avoid the former per-entry range/page scan. The
    # generated scalar fast gate reads the process-global mutation epoch; only
    # a mismatch calls the exact clipped-range slow validator.
    epoch_fragments = (
        "uint64_t suffix_epoch;",
        "uint64_t full_epoch;",
        "const uint64_t epoch = g_dirty_ram_text_mutation_epoch;",
        "validity->suffix_epoch == epoch",
        "validity->full_epoch == epoch",
        "dirty_ram_text_native_ok_ranges_from_epoch_cached(",
        "dirty_ram_text_native_ok_ranges_epoch_cached(",
    )
    for fragment in epoch_fragments:
        if fragment not in source:
            raise AssertionError(f"epoch-qualified direct dispatch missing: {fragment}")

    keys, pages = parse_dispatch(source, ram_size)
    if len(keys) < 2:
        raise AssertionError(f"expected at least two dispatch entries, got {len(keys)}")
    if len(keys) != len(set(keys)):
        raise AssertionError("dispatch table contains colliding physical entries")

    # Every record must occupy exactly its own direct slot, and each legal PS1
    # segment alias must resolve to the identical record.
    for index, key in enumerate(keys):
        for alias in (key, key | 0x80000000, key | 0xA0000000):
            actual = mapped_index(pages, alias, ram_size)
            if actual != index:
                raise AssertionError(
                    f"entry {index} key 0x{key:08X} alias 0x{alias:08X} "
                    f"mapped to {actual}")

    # Conversely, every nonzero generated slot must name the unique record
    # whose physical key is that slot. This proves there are no phantom hits.
    nonzero = 0
    for page_number, slots in pages.items():
        for slot, encoded in enumerate(slots):
            phys = (page_number << 12) | (slot << 2)
            if encoded == 0:
                if mapped_index(pages, phys, ram_size) is not None:
                    raise AssertionError(f"empty slot 0x{phys:08X} produced a hit")
                continue
            nonzero += 1
            if encoded > len(keys) or keys[encoded - 1] != phys:
                raise AssertionError(
                    f"slot 0x{phys:08X} has invalid record {encoded - 1}")
    if nonzero != len(keys):
        raise AssertionError(
            f"map covers {nonzero} records, dispatch table has {len(keys)}")

    # Exhaust the complete active instruction address space, including all
    # unpopulated pages: every aligned word is either the
    # unique expected record or a fail-closed miss.
    expected_by_phys = {key: index for index, key in enumerate(keys)}
    misses = 0
    for phys in range(0, ram_size, 4):
        actual = mapped_index(pages, phys, ram_size)
        expected = expected_by_phys.get(phys)
        if actual != expected:
            raise AssertionError(
                f"exhaustive word 0x{phys:08X}: expected {expected}, got {actual}")
        if expected is None:
            misses += 1
    if mapped_index(pages, keys[0] + 1, ram_size) is not None:
        raise AssertionError("unaligned address did not fail closed")
    if mapped_index(pages, ram_size, ram_size) is not None:
        raise AssertionError("out-of-RAM address did not fail closed")

    return len(keys), len(pages), misses


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--recompiler",
        default=str((here / ".." / "build" / "psxrecomp-game").resolve()))
    parser.add_argument("--dispatch-source", type=Path)
    args = parser.parse_args()

    if args.dispatch_source:
        source = args.dispatch_source.read_text(encoding="utf-8")
    else:
        if not os.path.isfile(args.recompiler):
            raise SystemExit("recompiler not found: " + args.recompiler)
        with tempfile.TemporaryDirectory() as tmp:
            source = generate_dispatch(args.recompiler, tmp)

    try:
        entries, pages, misses = verify(source)
    except AssertionError as exc:
        raise SystemExit(f"FAIL: {exc}") from exc
    print(
        f"direct dispatch lookup: ok ({entries} entries, {pages} pages, "
        f"{misses} full-RAM misses; 3 aliases/entry)")


if __name__ == "__main__":
    main()
