#!/usr/bin/env python3
"""Guard exact high-RAM static-overlay memo invalidation."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[2]


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"\b(?:static\s+)?(?:inline\s+)?(?:int|void|uint32_t)\s+"
        rf"{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.S,
    )
    if not match:
        raise AssertionError(f"missing function definition: {name}")
    start = match.end()
    depth = 1
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos]
    raise AssertionError(f"unterminated function definition: {name}")


def main() -> int:
    memory = (ROOT / "runtime/src/memory.c").read_text(encoding="utf-8")
    loader = (ROOT / "runtime/src/overlay_loader.c").read_text(encoding="utf-8")

    reset = function_body(memory, "dirty_ram_reset_for_boot")
    arm = reset.find("overlay_watch_set_range(0x00780000u, 0x2004u);")
    clear = reset.find("memset(overlay_watch_bitmap, 0, sizeof(overlay_watch_bitmap));")
    epoch = reset.find("g_dirty_ram_code_gen++;")
    if arm < 0:
        raise AssertionError("pinned high-RAM static image is not generation-watched at boot")
    if clear < 0 or arm < clear or epoch < 0 or arm > epoch:
        raise AssertionError("high-RAM watch arm is not inside the boot generation reset")
    if "0x00800000u" in reset[max(0, arm - 180):arm + 120]:
        raise AssertionError("high-RAM invalidation watch widened beyond the pinned image")

    note = function_body(memory, "overlay_watch_note_write")
    watched = note.find("if (watched && overlay_code_watch_intersects(phys, size))")
    bump = note.find("overlay_page_gen[p]++;")
    if watched < 0 or bump < 0 or watched > bump:
        raise AssertionError("writes in the pinned high-RAM image do not bump its page generation")

    matcher = function_body(loader, "psx_overlay_static_code_matches")
    pagegen = matcher.find("overlay_watch_pagegen_sum(lo, len);")
    fast = matcher.find("if (entry && entry->ranges && entry->gen_sum == gen_sum)")
    crc = matcher.find("uint32_t crc = 0xFFFFFFFFu;")
    rehash = matcher.find("s_static_match_rehashes++;")
    save = matcher.find("entry->gen_sum = gen_sum;")
    if min(pagegen, fast, crc, rehash, save) < 0 or not pagegen < fast < crc < rehash < save:
        raise AssertionError("static memo can bypass rehash after a watched-page generation change")

    print("test_perf_round3_guards: all checks passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}")
        sys.exit(1)
