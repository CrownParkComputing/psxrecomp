/* Generation-qualified admission cache for exact generated game-code ranges.
 *
 * The PS-X EXE image mixes instructions with hot mutable data, so a single
 * image generation would invalidate virtually every dispatch. Pages become
 * watched only when an emitted instruction range queries them. Each generated
 * dispatch record caches {reference-image epoch, sum(watched page generations)}
 * for its fixed page set. Any relevant write increases that sum; unrelated
 * data writes leave it unchanged.
 */

#include "dirty_ram_interp.h"

#include <stdint.h>
#include <string.h>

#define TEXT_RAM_SIZE       (2u * 1024u * 1024u)
#define TEXT_PAGE_SHIFT     12u
#define TEXT_PAGE_COUNT     (TEXT_RAM_SIZE >> TEXT_PAGE_SHIFT)
#define TEXT_BITMAP_WORDS   ((TEXT_PAGE_COUNT + 31u) / 32u)

static uint32_t s_watch_bitmap[TEXT_BITMAP_WORDS];
static uint64_t s_page_generation[TEXT_PAGE_COUNT];
static uint64_t s_image_epoch = 1u;
static uint32_t s_image_lo;
static uint32_t s_image_hi;
static int s_image_registered;
static int s_cache_disabled;

void dirty_ram_text_cache_reset(int clear_watches) {
    if (s_image_epoch == UINT64_MAX) {
        s_cache_disabled = 1;
    } else {
        s_image_epoch++;
        s_cache_disabled = 0;
    }
    memset(s_page_generation, 0, sizeof(s_page_generation));
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
    s_image_registered = len != 0u && phys_lo < TEXT_RAM_SIZE;
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
        if (!(s_watch_bitmap[page >> 5] & bit)) continue;
        if (s_page_generation[page] == UINT64_MAX) {
            s_cache_disabled = 1;
        } else {
            s_page_generation[page]++;
        }
    }
}

int dirty_ram_text_range_generation_from(const uint32_t *lo_len_pairs,
                                         uint32_t count,
                                         uint32_t exec_pc,
                                         uint64_t out[2]) {
    uint32_t at;
    uint64_t sum = 0;
    int any = 0;
    int representable;

    if (out) out[0] = out[1] = 0;
    if (!s_image_registered || !lo_len_pairs || count == 0u || !out) return 0;
    at = exec_pc & 0x1FFFFFFFu;
    representable = !s_cache_disabled;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = lo_len_pairs[i * 2u] & 0x1FFFFFFFu;
        uint32_t len = lo_len_pairs[i * 2u + 1u];
        uint32_t first_page;
        uint32_t last_page;

        if (len == 0u || phys < s_image_lo || phys >= s_image_hi ||
            len > s_image_hi - phys)
            return 0;
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
            uint32_t *watch_word = &s_watch_bitmap[page >> 5];
            if (!(*watch_word & bit)) *watch_word |= bit;
            if (UINT64_MAX - sum < s_page_generation[page]) {
                representable = 0;
            } else {
                sum += s_page_generation[page];
            }
        }
    }
    if (!any || !representable) return 0;
    out[0] = s_image_epoch;
    out[1] = sum;
    return 1;
}

int dirty_ram_text_native_ok_ranges_from_cached(
    const uint32_t *lo_len_pairs, uint32_t count, uint32_t exec_pc,
    uint64_t cached_generation[2]) {
    if (!cached_generation) {
        return dirty_ram_text_native_ok_ranges_from(lo_len_pairs, count,
                                                    exec_pc);
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t before[2] = {0, 0};
        const int cacheable = dirty_ram_text_range_generation_from(
            lo_len_pairs, count, exec_pc, before);
        if (cacheable && cached_generation[0] == before[0] &&
            cached_generation[1] == before[1]) {
            return 1;
        }

        if (!dirty_ram_text_native_ok_ranges_from(lo_len_pairs, count,
                                                  exec_pc)) {
            cached_generation[0] = cached_generation[1] = 0;
            return 0;
        }
        if (!cacheable) {
            cached_generation[0] = cached_generation[1] = 0;
            return 1;
        }

        {
            uint64_t after[2] = {0, 0};
            if (dirty_ram_text_range_generation_from(
                    lo_len_pairs, count, exec_pc, after) &&
                before[0] == after[0] && before[1] == after[1]) {
                cached_generation[0] = after[0];
                cached_generation[1] = after[1];
                return 1;
            }
        }
    }
    cached_generation[0] = cached_generation[1] = 0;
    return 0;
}

int dirty_ram_text_native_ok_ranges_cached(const uint32_t *lo_len_pairs,
                                           uint32_t count,
                                           uint64_t cached_generation[2]) {
    return dirty_ram_text_native_ok_ranges_from_cached(
        lo_len_pairs, count, 0u, cached_generation);
}
