#include "dirty_ram_interp.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

enum { TEST_TEXT_LO = 0x1000u, TEST_TEXT_LEN = 0x3000u };

static uint8_t s_ram[2u * 1024u * 1024u];
static uint8_t s_reference[TEST_TEXT_LEN];

int dirty_ram_text_native_ok_ranges_from(const uint32_t *lo_len_pairs,
                                         uint32_t count,
                                         uint32_t exec_pc) {
    const uint32_t at = exec_pc & 0x1FFFFFFFu;
    int any = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = lo_len_pairs[i * 2u] & 0x1FFFFFFFu;
        uint32_t len = lo_len_pairs[i * 2u + 1u];
        if (len == 0u || phys < TEST_TEXT_LO ||
            phys >= TEST_TEXT_LO + TEST_TEXT_LEN ||
            len > TEST_TEXT_LO + TEST_TEXT_LEN - phys)
            return 0;
        if (phys + len <= at) continue;
        if (phys < at) {
            len -= at - phys;
            phys = at;
        }
        any = 1;
        if (memcmp(s_ram + phys, s_reference + phys - TEST_TEXT_LO, len) != 0)
            return 0;
    }
    return any;
}

int main(void) {
    uint8_t *ram = s_ram;
    const uint32_t ranges[] = {
        0x1000u, 8u,
        0x2000u, 8u,
    };
    uint64_t suffix_cache[2] = {0, 0};
    uint64_t full_cache[2] = {0, 0};
    uint64_t before[2] = {0, 0};
    uint64_t after[2] = {0, 0};

    for (uint32_t i = 0; i < TEST_TEXT_LEN; i++)
        s_reference[i] = (uint8_t)(i * 37u + 11u);
    memcpy(ram + TEST_TEXT_LO, s_reference, sizeof(s_reference));
    dirty_ram_text_cache_register(TEST_TEXT_LO, sizeof(s_reference));

    assert(dirty_ram_text_native_ok_ranges_from_cached(
        ranges, 2u, 0x2000u, suffix_cache));
    assert(suffix_cache[0] != 0u);
    assert(dirty_ram_text_native_ok_ranges_cached(ranges, 2u, full_cache));
    assert(full_cache[0] == suffix_cache[0]);

    /* A write to the skipped prologue invalidates full-entry admission but not
     * the clipped continuation whose exact fetched bytes begin at 0x2000. */
    dirty_ram_text_note_range_write(0x1000u, 1u);
    ram[0x1000u] ^= 0x5Au;
    assert(dirty_ram_text_native_ok_ranges_from_cached(
        ranges, 2u, 0x2000u, suffix_cache));
    assert(!dirty_ram_text_native_ok_ranges_cached(ranges, 2u, full_cache));
    assert(full_cache[0] == 0u && full_cache[1] == 0u);

    dirty_ram_text_note_range_write(0x1000u, 1u);
    ram[0x1000u] = s_reference[0];
    assert(dirty_ram_text_native_ok_ranges_cached(ranges, 2u, full_cache));

    assert(dirty_ram_text_range_generation_from(
        ranges, 2u, 0x2000u, before));
    /* Page 0x3000 is inside the registered image but outside both exact code
     * ranges. Ordinary data writes there must not churn this function cache. */
    dirty_ram_text_note_range_write(0x3000u, 1u);
    ram[0x3000u] ^= 0xA5u;
    assert(dirty_ram_text_range_generation_from(
        ranges, 2u, 0x2000u, after));
    assert(before[0] == after[0] && before[1] == after[1]);

    /* A watched code-page mutation advances the qualifier and byte validation
     * fails closed. Blessing the intentional live byte into the reference then
     * permits a fresh positive cache entry. */
    dirty_ram_text_note_range_write(0x2000u, 1u);
    ram[0x2000u] ^= 0x3Cu;
    assert(dirty_ram_text_range_generation_from(
        ranges, 2u, 0x2000u, after));
    assert(before[0] == after[0] && before[1] != after[1]);
    assert(!dirty_ram_text_native_ok_ranges_from_cached(
        ranges, 2u, 0x2000u, suffix_cache));
    assert(suffix_cache[0] == 0u && suffix_cache[1] == 0u);
    dirty_ram_text_note_range_write(0x2000u, 1u);
    s_reference[0x2000u - TEST_TEXT_LO] = ram[0x2000u];
    assert(dirty_ram_text_native_ok_ranges_from_cached(
        ranges, 2u, 0x2000u, suffix_cache));

    /* Registering a replacement image rotates the epoch; no generated record
     * may reuse a positive result from the prior image. */
    before[0] = suffix_cache[0];
    memcpy(s_reference, ram + TEST_TEXT_LO, sizeof(s_reference));
    dirty_ram_text_cache_register(TEST_TEXT_LO, sizeof(s_reference));
    assert(dirty_ram_text_native_ok_ranges_from_cached(
        ranges, 2u, 0x2000u, suffix_cache));
    assert(suffix_cache[0] != before[0]);

    return 0;
}
