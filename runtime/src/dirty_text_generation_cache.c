/* Epoch-qualified admission cache for exact generated game-code ranges.
 *
 * Generated dispatch is extremely hot. Its unchanged path must not walk a
 * function's range list or covered 4 KiB pages. Instead, ranges arm a page
 * watch only when they are first validated. A write touching any armed text
 * page advances one process-global mutation epoch, invalidating every positive
 * generated entry in O(1). The next use of an entry re-arms/checks its exact
 * clipped ranges and compares the live bytes before publishing the new epoch.
 *
 * Writes to never-executed data pages do not advance the epoch. That is safe:
 * an entry covering such a page has no positive cache yet and therefore must
 * take the exact validation path before it can execute. Epoch zero is reserved
 * for fail-closed operation after uint64_t exhaustion; it can never equal a
 * positive generated cache entry.
 */

#include "dirty_ram_interp.h"

#include <stdint.h>
#include <string.h>

#define TEXT_RAM_SIZE       (2u * 1024u * 1024u)
#define TEXT_PAGE_SHIFT     12u
#define TEXT_PAGE_COUNT     (TEXT_RAM_SIZE >> TEXT_PAGE_SHIFT)
#define TEXT_BITMAP_WORDS   ((TEXT_PAGE_COUNT + 31u) / 32u)

static uint32_t s_watch_bitmap[TEXT_BITMAP_WORDS];
uint64_t g_dirty_ram_text_mutation_epoch = 1u;
uint64_t g_dirty_ram_text_watched_write_epochs;
uint64_t g_dirty_ram_text_exact_revalidations;
static uint32_t s_image_lo;
static uint32_t s_image_hi;
static int s_image_registered;
static int s_cache_disabled;

#ifdef PSX_DIRTY_TEXT_CACHE_TESTING
static uint64_t s_test_page_visits;
static uint64_t s_test_exact_validations;
#endif

static void dirty_ram_text_advance_epoch(void) {
    if (s_cache_disabled) return;
    if (g_dirty_ram_text_mutation_epoch == UINT64_MAX) {
        /* Never wrap onto an epoch an old generated entry may still hold. */
        g_dirty_ram_text_mutation_epoch = 0u;
        s_cache_disabled = 1;
        return;
    }
    g_dirty_ram_text_mutation_epoch++;
}

void dirty_ram_text_cache_reset(int clear_watches) {
    dirty_ram_text_advance_epoch();
    if (clear_watches)
        memset(s_watch_bitmap, 0, sizeof(s_watch_bitmap));
}

void dirty_ram_text_cache_register(uint32_t phys_lo, uint32_t len) {
    if (phys_lo >= TEXT_RAM_SIZE) {
        s_image_lo = s_image_hi = 0u;
        s_image_registered = 0;
        dirty_ram_text_cache_reset(1);
        return;
    }
    if (len > TEXT_RAM_SIZE - phys_lo) len = TEXT_RAM_SIZE - phys_lo;
    s_image_lo = phys_lo;
    s_image_hi = phys_lo + len;
    s_image_registered = len != 0u;
    dirty_ram_text_cache_reset(1);
}

void dirty_ram_text_note_range_write(uint32_t phys, uint32_t len) {
    uint32_t end;
    uint32_t lo;
    uint32_t hi;
    uint32_t first_page;
    uint32_t last_page;

    if (!s_image_registered || len == 0u || phys >= TEXT_RAM_SIZE) return;
    end = phys + len;
    if (end < phys || end > TEXT_RAM_SIZE) end = TEXT_RAM_SIZE;
    lo = phys < s_image_lo ? s_image_lo : phys;
    hi = end > s_image_hi ? s_image_hi : end;
    if (hi <= lo) return;

    first_page = lo >> TEXT_PAGE_SHIFT;
    last_page = (hi - 1u) >> TEXT_PAGE_SHIFT;
    for (uint32_t page = first_page; page <= last_page; page++) {
        const uint32_t bit = 1u << (page & 31u);
        if (s_watch_bitmap[page >> 5] & bit) {
            /* One writer notification is one mutation, even when it spans
             * multiple watched pages. Monotonicity, not the delta, matters. */
            g_dirty_ram_text_watched_write_epochs++;
            dirty_ram_text_advance_epoch();
            return;
        }
    }
}

/* Validate/clamp the exact range description and arm every page whose bytes
 * the native continuation may fetch. Returns the stable epoch observed after
 * publication, or zero when the range is invalid / caching is disabled. */
static uint64_t dirty_ram_text_watch_ranges_from(
    const uint32_t *lo_len_pairs, uint32_t count, uint32_t exec_pc) {
    uint32_t at;
    int any = 0;

    if (!s_image_registered || !lo_len_pairs || count == 0u ||
        s_cache_disabled || g_dirty_ram_text_mutation_epoch == 0u)
        return 0u;
    at = exec_pc & 0x1FFFFFFFu;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = lo_len_pairs[i * 2u] & 0x1FFFFFFFu;
        uint32_t len = lo_len_pairs[i * 2u + 1u];
        uint32_t first_page;
        uint32_t last_page;

        if (len == 0u || phys < s_image_lo || phys >= s_image_hi ||
            len > s_image_hi - phys)
            return 0u;
        if (phys + len <= at) continue;
        if (phys < at) {
            len -= at - phys;
            phys = at;
        }
        any = 1;
        first_page = phys >> TEXT_PAGE_SHIFT;
        last_page = (phys + len - 1u) >> TEXT_PAGE_SHIFT;
        for (uint32_t page = first_page; page <= last_page; page++) {
            const uint32_t bit = 1u << (page & 31u);
            s_watch_bitmap[page >> 5] |= bit;
#ifdef PSX_DIRTY_TEXT_CACHE_TESTING
            s_test_page_visits++;
#endif
        }
    }
    return any ? g_dirty_ram_text_mutation_epoch : 0u;
}

uint64_t dirty_ram_text_mutation_epoch(void) {
    return g_dirty_ram_text_mutation_epoch;
}

/* Compatibility query retained for diagnostics and older generated sources.
 * The qualifier is now {global mutation epoch, 0}; unlike the old page-sum
 * implementation, this routine is slow and is never used on an unchanged
 * generated-dispatch hit. */
int dirty_ram_text_range_generation_from(const uint32_t *lo_len_pairs,
                                         uint32_t count,
                                         uint32_t exec_pc,
                                         uint64_t out[2]) {
    uint64_t epoch;
    if (out) out[0] = out[1] = 0u;
    if (!out) return 0;
    epoch = dirty_ram_text_watch_ranges_from(lo_len_pairs, count, exec_pc);
    if (epoch == 0u) return 0;
    out[0] = epoch;
    return 1;
}

int dirty_ram_text_native_ok_ranges_from_epoch_cached(
    const uint32_t *lo_len_pairs, uint32_t count, uint32_t exec_pc,
    uint64_t *cached_epoch) {
    uint64_t epoch;

    if (!cached_epoch)
        return dirty_ram_text_native_ok_ranges_from(lo_len_pairs, count,
                                                    exec_pc);

    epoch = g_dirty_ram_text_mutation_epoch;
    if (epoch != 0u && *cached_epoch == epoch) return 1;

    for (int attempt = 0; attempt < 2; attempt++) {
        const uint64_t before = dirty_ram_text_watch_ranges_from(
            lo_len_pairs, count, exec_pc);

        g_dirty_ram_text_exact_revalidations++;
#ifdef PSX_DIRTY_TEXT_CACHE_TESTING
        s_test_exact_validations++;
#endif
        if (!dirty_ram_text_native_ok_ranges_from(lo_len_pairs, count,
                                                  exec_pc)) {
            *cached_epoch = 0u;
            return 0;
        }

        /* A disabled/overflowed cache may still admit byte-identical native
         * code, but it must validate on every dispatch and publish no epoch. */
        if (before == 0u) {
            *cached_epoch = 0u;
            return 1;
        }
        if (before == g_dirty_ram_text_mutation_epoch) {
            *cached_epoch = before;
            return 1;
        }
    }
    *cached_epoch = 0u;
    return 0;
}

int dirty_ram_text_native_ok_ranges_epoch_cached(
    const uint32_t *lo_len_pairs, uint32_t count, uint64_t *cached_epoch) {
    return dirty_ram_text_native_ok_ranges_from_epoch_cached(
        lo_len_pairs, count, 0u, cached_epoch);
}

/* ABI compatibility for already-generated projects. */
int dirty_ram_text_native_ok_ranges_from_cached(
    const uint32_t *lo_len_pairs, uint32_t count, uint32_t exec_pc,
    uint64_t cached_generation[2]) {
    return dirty_ram_text_native_ok_ranges_from_epoch_cached(
        lo_len_pairs, count, exec_pc,
        cached_generation ? &cached_generation[0] : (uint64_t *)0);
}

int dirty_ram_text_native_ok_ranges_cached(const uint32_t *lo_len_pairs,
                                           uint32_t count,
                                           uint64_t cached_generation[2]) {
    return dirty_ram_text_native_ok_ranges_from_cached(
        lo_len_pairs, count, 0u, cached_generation);
}

#ifdef PSX_DIRTY_TEXT_CACHE_TESTING
void dirty_ram_text_cache_test_set_epoch(uint64_t epoch) {
    g_dirty_ram_text_mutation_epoch = epoch;
    s_cache_disabled = epoch == 0u;
}

uint64_t dirty_ram_text_cache_test_page_visits(void) {
    return s_test_page_visits;
}

uint64_t dirty_ram_text_cache_test_exact_validations(void) {
    return s_test_exact_validations;
}

void dirty_ram_text_cache_test_reset_counters(void) {
    s_test_page_visits = 0u;
    s_test_exact_validations = 0u;
}
#endif
