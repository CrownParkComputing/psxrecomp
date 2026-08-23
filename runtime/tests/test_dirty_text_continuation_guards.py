#!/usr/bin/env python3
"""Keep dirty-text continuation handoff behavior from regressing."""

from pathlib import Path
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    memory = (root / "runtime/src/memory.c").read_text(encoding="utf-8")
    cache = (root / "runtime/src/dirty_text_generation_cache.c").read_text(
        encoding="utf-8")
    cosim = (root / "runtime/src/cosim_state.c").read_text(encoding="utf-8")
    interp = (root / "runtime/src/dirty_ram_interp.c").read_text(encoding="utf-8")
    compat = (root / "runtime/src/game_dispatch_compat.c").read_text(encoding="utf-8")
    runtime_cmake = (root / "runtime/runtime.cmake").read_text(encoding="utf-8")

    start = memory.index("int dirty_ram_text_native_ok_ranges_from(")
    end = memory.index("\n/* Preserve the generated-code ABI", start)
    range_guard = memory[start:end]

    required_range_fragments = (
        "uint32_t exec_pc",
        "if (phys + len <= at) continue;",
        "len -= at - phys;",
        "if (!any)",
    )
    for fragment in required_range_fragments:
        if fragment not in range_guard:
            raise AssertionError(f"missing continuation range guard: {fragment}")
    if "text_diverged_bitmap" in range_guard:
        raise AssertionError("exact-range mismatch still sticky-poisons a whole page")
    if "dirty_ram_text_native_ok_ranges_from(lo_len_pairs, count, 0u)" not in memory:
        raise AssertionError("legacy generated-code ABI is not preserved")

    # Generated dispatch validity is page-qualified, not image-global: the
    # PS-X EXE image also contains hot mutable data. Exact queried pages become
    # watched before the qualifier is captured, and only overlapping writes
    # advance their monotonic generations.
    for fragment in (
        "s_watch_bitmap[TEXT_BITMAP_WORDS]",
        "s_page_generation[TEXT_PAGE_COUNT]",
        "uint64_t s_image_epoch",
        "dirty_ram_text_range_generation_from(",
        "uint32_t *watch_word = &s_watch_bitmap[page >> 5];",
        "if (!(*watch_word & bit)) *watch_word |= bit;",
        "sum += s_page_generation[page];",
    ):
        if fragment not in cache:
            raise AssertionError(f"missing exact-range generation cache guard: {fragment}")
    if "g_dirty_ram_code_gen" in cache:
        raise AssertionError("exact text cache regressed to the global code epoch")
    generation_start = cache.index("int dirty_ram_text_range_generation_from(")
    generation_end = cache.index(
        "int dirty_ram_text_native_ok_ranges_from_cached(", generation_start)
    generation = cache[generation_start:generation_end]
    for fragment in (
        "if (phys + len <= at) continue;",
        "len -= at - phys;",
        "phys = at;",
        "first_page = phys >> TEXT_PAGE_SHIFT;",
        "last_page = (phys + len - 1u) >> TEXT_PAGE_SHIFT;",
    ):
        if fragment not in generation:
            raise AssertionError(
                f"page qualifier does not mirror continuation clipping: {fragment}")

    note_start = cache.index("void dirty_ram_text_note_range_write(")
    note_end = cache.index("int dirty_ram_text_range_generation_from(", note_start)
    note = cache[note_start:note_end]
    if "s_page_generation[page]++" not in note:
        raise AssertionError("overlapping text write does not advance its page")
    if "s_image_epoch++" in note:
        raise AssertionError("ordinary text write still invalidates every cached entry")
    if memory.count("text_guard_note_write(phys,") != 3:
        raise AssertionError("one of the byte/half/word RAM store paths bypasses text guard")

    cached_start = cache.index(
        "int dirty_ram_text_native_ok_ranges_from_cached(")
    cached_end = cache.index(
        "int dirty_ram_text_native_ok_ranges_cached(", cached_start)
    cached = cache[cached_start:cached_end]
    for fragment in (
        "dirty_ram_text_range_generation_from(",
        "cached_generation[0] == before[0]",
        "dirty_ram_text_native_ok_ranges_from(",
        "before[0] == after[0] && before[1] == after[1]",
        "cached_generation[0] = cached_generation[1] = 0;",
    ):
        if fragment not in cached:
            raise AssertionError(f"missing qualified validation ordering: {fragment}")

    text_write = memory[
        memory.index("static inline void text_guard_note_write("):
        memory.index("int dirty_ram_text_native_ok(")]
    if text_write.index("dirty_ram_text_note_range_write(") > text_write.index(
            "memcmp(ref, buf"):
        raise AssertionError("CPU text write invalidates only after inspecting bytes")
    mark = memory[
        memory.index("void dirty_ram_mark_executable_range("):
        memory.index("/* Force-interp mode", memory.index(
            "void dirty_ram_mark_executable_range("))]
    if "dirty_ram_text_note_range_write(phys, len);" not in mark:
        raise AssertionError("bulk executable writes bypass exact-range generations")
    bless = memory[
        memory.index("void dirty_ram_text_bless("):
        memory.index("uint64_t dirty_ram_text_native_blocked", memory.index(
            "void dirty_ram_text_bless("))]
    if bless.index("dirty_ram_text_note_range_write(lo, hi - lo);") > bless.index(
            "memcpy(ref, src"):
        raise AssertionError("reference bless publishes bytes before cache invalidation")
    restore = memory[
        memory.index("void dirty_ram_text_guard_resync_after_restore("):
        memory.index("void overlay_watch_invalidate_after_ram_restore(")]
    if "dirty_ram_text_cache_reset(0);" not in restore:
        raise AssertionError("whole-RAM restore does not rotate text validity")
    register = memory[
        memory.index("void dirty_ram_register_text_image("):
        memory.index("int dirty_ram_text_image_registered(")]
    if "dirty_ram_text_cache_register(phys_lo, len);" not in register:
        raise AssertionError("new reference image can reuse stale validity")
    reset = memory[
        memory.index("void dirty_ram_reset_for_boot("):
        memory.index("void overlay_watch_set_range(")]
    if "dirty_ram_text_cache_reset(1);" not in reset:
        raise AssertionError("boot reset can reuse stale validity")
    low_clear = memory[
        memory.index("void memory_clear_low_boot_scratch("):
        memory.index("/* ---- Dirty-page tracking")]
    if "dirty_ram_text_note_range_write(0u, 0x10u);" not in low_clear:
        raise AssertionError("direct low-RAM memset bypasses text validity")
    cosim_inject = cosim[
        cosim.index("if (s_inj_ram_phys >= 0"):
        cosim.index("s_inj_ram_phys = -1;", cosim.index(
            "if (s_inj_ram_phys >= 0"))]
    if cosim_inject.index(
            "dirty_ram_text_note_range_write((uint32_t)s_inj_ram_phys, 1u);") > \
            cosim_inject.index("ram[s_inj_ram_phys] ^="):
        raise AssertionError("cosim RAM injection mutates bytes before cache invalidation")

    handoff = "clean_game_text_miss && interp_enter_compiled(cpu, "
    if interp.count(handoff) != 1:
        raise AssertionError("expected one suffix-validated transfer handoff")
    if "interp_enter_compiled(cpu, target)" not in interp:
        raise AssertionError("missing transfer-boundary continuation handoff")
    if "psx_game_text_native_ok_full(pc) &&" not in interp:
        raise AssertionError("straight-line handoff lacks full-range validation")
    if "interp_enter_compiled(cpu, pc)" not in interp:
        raise AssertionError("missing call-return continuation handoff")
    if "PSX_GAME_DISPATCH_HAS_NATIVE_OK_FULL" not in compat:
        raise AssertionError("older generated dispatchers lost full-guard compatibility")
    if compat.count("int psx_game_text_native_ok_full(uint32_t addr)") != 3:
        raise AssertionError("full-guard compatibility branches are incomplete")
    if compat.count("int psx_game_text_native_ok_full(uint32_t addr)\n{\n    (void)addr;\n    return 0;\n}") != 3:
        raise AssertionError("a dispatcher without the full ABI may accept an unsafe handoff")
    if "has_game_dispatch_native_ok_full" not in runtime_cmake:
        raise AssertionError("runtime does not detect the generated full-range ABI")
    if "runtime/src/dirty_text_generation_cache.c" not in runtime_cmake:
        raise AssertionError("generation cache implementation is not linked into runtime")

    print("dirty-text continuation guards: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
