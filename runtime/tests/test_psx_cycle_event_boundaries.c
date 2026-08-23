/*
 * Pin cross-device causality at a scheduler deadline.
 *
 * Build/run: ctest -R psx_cycle_event_boundaries_test
 */

#include "psx_cycles.h"

#include <stdint.h>
#include <stdio.h>

int g_ls_replay_active = 0;
int g_ls_mode = 0;
int g_precise_mode = 0;
int g_psx_call_bail = 0;
uint32_t i_mask = 0;
uint32_t i_stat = 0;
uint64_t g_guest_store_count = 0;
uint64_t g_mmio_access_count = 0;

static uint32_t s_cd_cycles_remaining = 5;
static int s_cd_ready = 0;
static uint32_t s_dma_ready_cycles = 0;
static int s_mdec_recent = 0;

void sio_advance(uint32_t cycles) { (void)cycles; }

void cdrom_advance(uint32_t cycles) {
    if (s_cd_ready) return;
    if (cycles >= s_cd_cycles_remaining) {
        s_cd_cycles_remaining = 0;
        s_cd_ready = 1;
    } else {
        s_cd_cycles_remaining -= cycles;
    }
}

void dma_advance(uint32_t cycles) {
    if (s_cd_ready) s_dma_ready_cycles += cycles;
}

void timers_advance(uint32_t cycles) { (void)cycles; }
void interrupts_advance_cycles(uint32_t cycles) { (void)cycles; }
void interrupts_service_scheduled_events(void) {}

uint32_t interrupts_cycles_to_vblank(void) { return UINT32_MAX; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) {
    (void)mask;
    return s_cd_ready ? UINT32_MAX : s_cd_cycles_remaining;
}
uint32_t dma_cycles_to_internal_event(void) { return UINT32_MAX; }
uint32_t dma_cycles_to_deliverable_irq(uint32_t mask) {
    (void)mask;
    return UINT32_MAX;
}
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return UINT32_MAX; }
int psx_get_in_exception(void) { return 0; }
int mdec_recently_active(uint32_t within_frames) {
    (void)within_frames;
    return s_mdec_recent;
}

void starvation_watchdog_check(void) {}
void starvation_ring_pc_sample(void) {}

int  psx_netplay_active(void) { return 0; }
int  psx_selfcheck_enabled(void) { return 0; }
void dirty_ram_ld_delay_discard(void) {}
void dirty_ram_irq_ambient_resync_after_restore(void) {}

int main(void) {
    /* NULL cpu: this test pins the scheduler boundary, not the CPU-state
     * rewind. psx_cycles_resync_after_restore guards that block on `if (cpu)`. */
    psx_cycles_resync_after_restore(NULL);
    psx_advance_cycles(5);

    if (psx_get_cycle_count() != 5) {
        fprintf(stderr, "FAIL cycle count: expected 5 got %llu\n",
                (unsigned long long)psx_get_cycle_count());
        return 1;
    }
    if (!s_cd_ready) {
        fprintf(stderr, "FAIL CD event did not fire at cycle 5\n");
        return 1;
    }
    if (s_dma_ready_cycles != 1) {
        fprintf(stderr,
                "FAIL retroactive DMA credit: expected 1 boundary cycle got %u\n",
                s_dma_ready_cycles);
        return 1;
    }

    /* Snapshotting must publish the scheduler's intentionally deferred quiet
     * device interval. A clock at cycle 2 paired with CD state from cycle 0 is
     * not a coherent save and cannot be repaired after load. */
    {
        PSXClockDomainSnapshot coherent;
        s_cd_cycles_remaining = 5u;
        s_cd_ready = 0;
        s_dma_ready_cycles = 0u;
        psx_cycles_reset_for_boot();
        psx_set_cpu_overclock(100u);
        psx_devices_service_to_now(); /* establish deadline at native cycle 0 */
        psx_advance_cycles(2u);       /* clock advances; CD remains deferred */
        if (s_cd_cycles_remaining != 5u) {
            fprintf(stderr, "FAIL test did not establish deferred device gap\n");
            return 1;
        }
        psx_clock_domain_snapshot(&coherent);
        if (coherent.native_cycle_count != 2u ||
            s_cd_cycles_remaining != 3u || s_cd_ready) {
            fprintf(stderr,
                    "FAIL snapshot was not device-coherent: cycle=%llu cd=%u ready=%d\n",
                    (unsigned long long)coherent.native_cycle_count,
                    s_cd_cycles_remaining, s_cd_ready);
            return 1;
        }
    }

    /* A 900% save/load must restore the CPU/native converter carry. Losing an
     * 8/9 remainder changes the timestamp of the very next retired cycle. */
    {
        PSXClockDomainSnapshot saved;
        psx_cycles_reset_for_boot();
        psx_set_cpu_overclock(900u);
        psx_advance_cycles(8u);
        if (psx_cycle_count != 0u || g_psx_oc_accum != 8u) {
            fprintf(stderr, "FAIL 900%% pre-snapshot carry\n");
            return 1;
        }
        psx_clock_domain_snapshot(&saved);
        psx_advance_cycles(1u);
        if (psx_cycle_count != 1u || g_psx_oc_accum != 0u) {
            fprintf(stderr, "FAIL 900%% carry consumption\n");
            return 1;
        }
        psx_set_cpu_overclock(100u);
        if (psx_clock_domain_restore(&saved)) {
            fprintf(stderr, "FAIL mismatched clock policy accepted state\n");
            return 1;
        }
        psx_set_cpu_overclock(900u);
        if (!psx_clock_domain_restore(&saved)) {
            fprintf(stderr, "FAIL clock-domain snapshot rejected\n");
            return 1;
        }
        psx_cycles_resync_after_restore(NULL);
        if (psx_cycle_count != 0u || psx_cpu_retired_cycles != 8u ||
            g_psx_oc_accum != 8u) {
            fprintf(stderr, "FAIL clock-domain snapshot state changed\n");
            return 1;
        }
        psx_advance_cycles(1u);
        if (psx_cycle_count != 1u || psx_cpu_retired_cycles != 9u ||
            psx_cpu_native_cycles != 1u || g_psx_oc_accum != 0u) {
            fprintf(stderr, "FAIL clock-domain snapshot continuation\n");
            return 1;
        }
    }

    /* The title countdown compactor must measure the next event from the
     * current published native clock, not from the older device-sync point.
     * Train a nine-raw-cycle loop while CD is synced at native cycle 1 with an
     * event due at absolute cycle 5. At native cycle 3 only ONE synthetic loop
     * is safe: it lands at 4 and leaves the real iteration to reach/observe 5.
     * The stale device-relative distance is still 4 and would wrongly skip
     * three iterations through the event. */
    {
        uint32_t skipped;
        s_cd_cycles_remaining = 5u;
        s_cd_ready = 0;
        s_dma_ready_cycles = 0u;
        i_stat = 0u;
        i_mask = 0u;
        /* The exact WipEout VBlank poll remains safe while an FMV is decoding;
         * MDEC must not disable the title compactor and halve PALx2 playback. */
        s_mdec_recent = 1;
        g_idle_skip_enabled = 1;
        psx_cycles_reset_for_boot();
        psx_set_cpu_overclock(900u);

        psx_advance_cycles(9u); /* native 1; sync devices, CD now 4 away */
        if (psx_idle_batch_countdown(100u) != 0u) {
            fprintf(stderr, "FAIL countdown compacted before training\n");
            return 1;
        }
        psx_advance_cycles(9u); /* native 2 */
        if (psx_idle_batch_countdown(99u) != 0u) {
            fprintf(stderr, "FAIL countdown compacted after one sample\n");
            return 1;
        }
        psx_advance_cycles(9u); /* native 3 */
        skipped = psx_idle_batch_countdown(98u);
        if (skipped != 1u || psx_cycle_count != 4u || s_cd_ready) {
            fprintf(stderr,
                    "FAIL countdown crossed event: skipped=%u cycle=%llu ready=%d\n",
                    skipped, (unsigned long long)psx_cycle_count, s_cd_ready);
            return 1;
        }
        psx_advance_cycles(9u); /* the retained real iteration reaches cycle 5 */
        if (psx_cycle_count != 5u || !s_cd_ready) {
            fprintf(stderr,
                    "FAIL retained countdown iteration missed event: cycle=%llu ready=%d\n",
                    (unsigned long long)psx_cycle_count, s_cd_ready);
            return 1;
        }
        s_mdec_recent = 0;
    }

    fprintf(stderr,
            "PASS event boundary, 900%% clock state, and countdown deadline\n");
    return 0;
}
