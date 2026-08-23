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
    const uint32_t ranges[] = {
        0x1000u, 8u,
        0x2000u, 8u,
    };
    const uint32_t invalid_ranges[] = {0x0FFCu, 8u};
    uint64_t suffix_epoch = 0u;
    uint64_t full_epoch = 0u;
    uint64_t compat_cache[2] = {0u, 0u};
    uint64_t first_epoch;
    uint64_t page_visits;
    uint64_t validations;
    uint64_t watched_writes;

    for (uint32_t i = 0; i < TEST_TEXT_LEN; i++)
        s_reference[i] = (uint8_t)(i * 37u + 11u);
    memcpy(s_ram + TEST_TEXT_LO, s_reference, sizeof(s_reference));
    dirty_ram_text_cache_register(TEST_TEXT_LO, sizeof(s_reference));
    first_epoch = dirty_ram_text_mutation_epoch();
    assert(first_epoch != 0u);

    /* The first suffix check validates exact bytes and arms only page 0x2000. */
    dirty_ram_text_cache_test_reset_counters();
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(suffix_epoch == first_epoch);
    assert(dirty_ram_text_cache_test_page_visits() == 1u);
    assert(dirty_ram_text_cache_test_exact_validations() == 1u);

    /* Core performance contract: an unchanged positive entry is a single
     * epoch compare. It performs neither a range/page walk nor byte compare. */
    page_visits = dirty_ram_text_cache_test_page_visits();
    validations = dirty_ram_text_cache_test_exact_validations();
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(dirty_ram_text_cache_test_page_visits() == page_visits);
    assert(dirty_ram_text_cache_test_exact_validations() == validations);

    /* A never-watched data page does not churn any positive entry. */
    dirty_ram_text_note_range_write(0x3000u, 1u);
    s_ram[0x3000u] ^= 0xA5u;
    assert(dirty_ram_text_mutation_epoch() == first_epoch);
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(dirty_ram_text_cache_test_page_visits() == page_visits);
    assert(g_dirty_ram_text_watched_write_epochs == 0u);

    /* Full admission arms both exact code pages. A watched write rotates the
     * global epoch, so every entry revalidates; clipped suffix bytes can still
     * pass while full-entry bytes fail closed. */
    assert(dirty_ram_text_native_ok_ranges_epoch_cached(
        ranges, 2u, &full_epoch));
    watched_writes = g_dirty_ram_text_watched_write_epochs;
    dirty_ram_text_note_range_write(0x1000u, 0x1008u);
    assert(g_dirty_ram_text_watched_write_epochs == watched_writes + 1u);
    assert(dirty_ram_text_native_ok_ranges_epoch_cached(
        ranges, 2u, &full_epoch));
    first_epoch = dirty_ram_text_mutation_epoch();
    watched_writes = g_dirty_ram_text_watched_write_epochs;
    dirty_ram_text_note_range_write(0x1000u, 1u);
    s_ram[0x1000u] ^= 0x5Au;
    assert(dirty_ram_text_mutation_epoch() != first_epoch);
    assert(g_dirty_ram_text_watched_write_epochs == watched_writes + 1u);
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(!dirty_ram_text_native_ok_ranges_epoch_cached(
        ranges, 2u, &full_epoch));
    assert(full_epoch == 0u);

    dirty_ram_text_note_range_write(0x1000u, 1u);
    s_ram[0x1000u] = s_reference[0];
    assert(dirty_ram_text_native_ok_ranges_epoch_cached(
        ranges, 2u, &full_epoch));

    /* A watched continuation mutation is byte-checked and rejected, then can
     * be admitted only after both live and reference bytes agree again. */
    dirty_ram_text_note_range_write(0x2000u, 1u);
    s_ram[0x2000u] ^= 0x3Cu;
    assert(!dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(suffix_epoch == 0u);
    dirty_ram_text_note_range_write(0x2000u, 1u);
    s_reference[0x2000u - TEST_TEXT_LO] = s_ram[0x2000u];
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));

    /* Restore/image lifecycle changes cannot reuse prior positive admission. */
    first_epoch = suffix_epoch;
    dirty_ram_text_cache_reset(0);
    assert(dirty_ram_text_mutation_epoch() != first_epoch);
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(suffix_epoch == dirty_ram_text_mutation_epoch());

    first_epoch = suffix_epoch;
    memcpy(s_reference, s_ram + TEST_TEXT_LO, sizeof(s_reference));
    dirty_ram_text_cache_register(TEST_TEXT_LO, sizeof(s_reference));
    assert(dirty_ram_text_mutation_epoch() != first_epoch);
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));

    /* Invalid range metadata never publishes an epoch. */
    suffix_epoch = 0u;
    assert(!dirty_ram_text_native_ok_ranges_from_epoch_cached(
        invalid_ranges, 1u, 0u, &suffix_epoch));
    assert(suffix_epoch == 0u);

    /* Existing generated dispatch sources retain their two-word ABI. */
    assert(dirty_ram_text_native_ok_ranges_from_cached(
        ranges, 2u, 0x2000u, compat_cache));
    assert(compat_cache[0] == dirty_ram_text_mutation_epoch());

    /* Exhaustion is permanent and fail-closed: zero can match no positive
     * cache. Byte-identical code remains runnable, but exact validation repeats
     * on every dispatch and no cache epoch is published. */
    suffix_epoch = dirty_ram_text_mutation_epoch();
    dirty_ram_text_cache_test_set_epoch(UINT64_MAX);
    watched_writes = g_dirty_ram_text_watched_write_epochs;
    dirty_ram_text_note_range_write(0x2000u, 1u);
    assert(dirty_ram_text_mutation_epoch() == 0u);
    assert(g_dirty_ram_text_watched_write_epochs == watched_writes + 1u);
    dirty_ram_text_cache_test_reset_counters();
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(suffix_epoch == 0u);
    assert(dirty_ram_text_native_ok_ranges_from_epoch_cached(
        ranges, 2u, 0x2000u, &suffix_epoch));
    assert(dirty_ram_text_cache_test_exact_validations() == 2u);
    dirty_ram_text_cache_reset(1);
    assert(dirty_ram_text_mutation_epoch() == 0u);

    return 0;
}
