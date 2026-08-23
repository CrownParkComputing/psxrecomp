#include "psx_cyc.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

uint64_t psx_cycle_count = 0;
uint64_t psx_cpu_retired_cycles = 0;
uint64_t psx_cpu_native_cycles = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
int g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = 0;
int psx_in_device_service = 0;
int g_event_step_conservative = 0;
int g_ls_replay_active = 0;
int g_ls_mode = 0;
volatile int g_ds_recording = 0;
uint8_t *g_psx_ram = 0;
int g_psx_load_delay = 1;
uint32_t g_psx_oc_numerator = 1u;
uint32_t g_psx_oc_denominator = 1u;
uint32_t g_psx_oc_accum = 0;

static int service_count;

void psx_devices_service_to_now(void) {
    service_count++;
    psx_next_service_cycle = psx_cycle_count + 1000u;
}

void psx_advance_cycles_slow(uint32_t cycles) {
    psx_cycle_count += cycles;
}

static void reset_clock(uint64_t deadline) {
    psx_cycle_count = 0;
    psx_next_service_cycle = deadline;
    g_psx_cyc_batch = 0;
    g_psx_cyc_batch_limit = 0;
    g_psx_cyc_bb_defer = 0;
    g_psx_cyc_local_acc = 0;
    psx_cpu_retired_cycles = 0;
    psx_cpu_native_cycles = 0;
    g_psx_oc_numerator = 1u;
    g_psx_oc_denominator = 1u;
    g_psx_oc_accum = 0u;
    service_count = 0;
}

int main(void) {
    reset_clock(5u);
    for (int i = 0; i < 4; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 4u);
    assert(service_count == 0);

    psx_cyc_charge(1u);
    assert(psx_cycle_count == 5u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 1);

    reset_clock(1000u);
    for (int i = 0; i < 63; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 63u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 64u);
    assert(g_psx_cyc_batch == 0u);
    assert(service_count == 0);

    reset_clock(5u);
    psx_cyc_bb_defer_begin();
    for (int i = 0; i < 10; i++) psx_cyc_charge(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_cyc_batch == 10u);
    psx_cyc_bb_defer_flush();
    assert(psx_cycle_count == 10u);
    assert(service_count == 1);
    assert(g_psx_cyc_bb_defer == 1);
    psx_cyc_bb_defer_end();
    assert(g_psx_cyc_bb_defer == 0);

    reset_clock(0u);
    psx_cyc_charge(1u);
    assert(psx_cycle_count == 1u);
    assert(service_count == 1);

    /* Exact 900% conversion: tiny charges retain their carried fraction and
     * are bit-identical to a single combined charge. */
    reset_clock(1000u);
    g_psx_oc_numerator = 1u;
    g_psx_oc_denominator = 9u;
    for (int i = 0; i < 8; i++) psx_advance_cycles(1u);
    assert(psx_cycle_count == 0u);
    assert(g_psx_oc_accum == 8u);
    psx_advance_cycles(1u);
    assert(psx_cycle_count == 1u);
    assert(g_psx_oc_accum == 0u);
    assert(psx_cpu_retired_cycles == 9u);
    assert(psx_cpu_native_cycles == 1u);

    reset_clock(2000000u);
    g_psx_oc_numerator = 1u;
    g_psx_oc_denominator = 9u;
    psx_advance_cycles(9000007u);
    assert(psx_cycle_count == 1000000u);
    assert(g_psx_oc_accum == 7u);
    assert(psx_cpu_retired_cycles == 9000007u);
    assert(psx_cpu_native_cycles == 1000000u);

    /* A native clock read includes both pending tiers exactly, without
     * publishing them or mutating the carried remainder. */
    reset_clock(1000u);
    g_psx_oc_numerator = 1u;
    g_psx_oc_denominator = 9u;
    g_psx_oc_accum = 4u;
    g_psx_cyc_batch = 5u;
    {
        uint32_t acc = 9u;
        g_psx_cyc_local_acc = &acc;
        assert(psx_get_pending_cpu_cycles() == 14u);
        assert(psx_preview_native_cycle_count() == 2u);
        assert(psx_get_cpu_retired_cycle_count() == 14u);
        assert(g_psx_oc_accum == 4u);
        assert(g_psx_cyc_batch == 5u && acc == 9u);
        assert(psx_cpu_cycles_until(37u) == 23u);
        g_psx_cyc_local_acc = 0;
    }

    /* Device-service work is already native time and must never pass through
     * the 9:1 CPU converter. */
    reset_clock(1000u);
    g_psx_oc_numerator = 1u;
    g_psx_oc_denominator = 9u;
    psx_in_device_service = 1;
    psx_advance_cycles(9u);
    psx_in_device_service = 0;
    assert(psx_cycle_count == 9u);
    assert(psx_cpu_retired_cycles == 0u);
    assert(psx_cpu_native_cycles == 0u);
    assert(g_psx_oc_accum == 0u);

    /* GTE/muldiv deadlines are raw CPU-cycle deadlines. The accessing
     * instruction's two already-pending raw cycles count toward the wait; the
     * remaining 13 retire once and convert to native time exactly once. */
    reset_clock(1000u);
    g_psx_oc_numerator = 1u;
    g_psx_oc_denominator = 9u;
    g_psx_cyc_batch = 2u;
    {
        const uint64_t deadline = 15u;
        uint32_t stall = psx_cpu_cycles_until(deadline);
        assert(stall == 13u);
        psx_advance_cycles(stall);
        assert(psx_get_cpu_retired_cycle_count() == deadline);
        assert(psx_cycle_count == 1u);
        assert(g_psx_oc_accum == 6u);
    }

    reset_clock(1000u);
    g_psx_cyc_batch = 2u;
    {
        const uint64_t deadline = 15u;
        psx_advance_cycles(psx_cpu_cycles_until(deadline));
        assert(psx_get_cpu_retired_cycle_count() == deadline);
        assert(psx_cycle_count == deadline);
    }

    /* Emitter-level local accumulator: charges stay off the published clock
     * until local_publish / batch_flush; guest total at the barrier matches. */
    reset_clock(1000u);
    {
        uint32_t acc = 0;
        psx_cyc_local_begin(&acc);
        for (int i = 0; i < 100; i++) psx_cyc_charge(5u);
        assert(psx_cycle_count == 0u);
        assert(g_psx_cyc_batch == 0u);
        assert(acc == 500u);
        psx_cyc_bb_defer_flush();
        assert(psx_cycle_count == 500u);
        assert(acc == 0u);
        psx_cyc_local_end();
        assert(g_psx_cyc_local_acc == 0);
    }
    return 0;
}
