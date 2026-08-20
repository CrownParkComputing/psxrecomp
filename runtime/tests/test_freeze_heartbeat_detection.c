/*
 * Regression coverage for the freeze heartbeat's spin-freeze signature.
 *
 * The production source is included in its dependency-free unit-test mode so
 * this exercises the actual window predicate rather than a copied model.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FREEZE_HEARTBEAT_UNIT_TEST 1
#include "../src/freeze_heartbeat.c"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", message); \
        failures++; \
    } else { \
        printf("ok:   %s\n", message); \
    } \
} while (0)

static void fill_pinned(HbRingEntry *ring) {
    memset(ring, 0, sizeof(HbRingEntry) * RING_CAP);
    for (uint32_t i = 0; i < RING_CAP; i++) {
        ring[i].current_func = 0x00000F40u;
        ring[i].last_store_pc = 0x8015FAB0u;
        ring[i].dirty_ram_insns = 24u;
    }
}

int main(void) {
    HbRingEntry ring[RING_CAP];
    fill_pinned(ring);

    /* A real guest wedge stays pinned across the complete 20-tick window. */
    CHECK(hb_logic_pinned_window(ring, 7u, 26u) == 1,
          "fully pinned guest window remains a spin-freeze candidate");

    /* The captured false-positive shape: endpoints match, but a normal paced
     * frame changed the store PC inside the window. Endpoint-only logic would
     * return true here and re-arm/dump again when the coincidence recurs. */
    ring[16].last_store_pc = 0x8016292Cu;
    CHECK(ring[7].last_store_pc == ring[26].last_store_pc,
          "host-pacing fixture keeps matching endpoint values");
    CHECK(hb_logic_pinned_window(ring, 7u, 26u) == 0,
          "interior host-pacing change suppresses spin-freeze");

    /* The heartbeat ring wraps; an interior change must still be observed. */
    fill_pinned(ring);
    ring[1].current_func = 0x000025ACu;
    CHECK(hb_logic_pinned_window(ring, 58u, 13u) == 0,
          "wrapped window checks every sample, not just its endpoints");

    fill_pinned(ring);
    ring[1].dirty_ram_insns++;
    CHECK(hb_logic_pinned_window(ring, 58u, 13u) == 0,
          "wrapped dirty-instruction progress suppresses spin-freeze");

    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
