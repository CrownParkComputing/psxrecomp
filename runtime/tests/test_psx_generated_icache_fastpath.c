#include "cpu_state.h"
#include "psx_cyc.h"
#include "psx_icache.h"

#include <stdio.h>
#include <string.h>

int g_ls_replay_active = 0;
static uint64_t test_cycles = 0;

void psx_advance_cycles(uint32_t cycles) { test_cycles += cycles; }

static int expect(int condition, const char *message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

int main(void) {
    CPUState cpu;
    memset(&cpu, 0, sizeof(cpu));

    psx_icache_reset();
    g_psx_icache_active = 1;
    cpu.read_absorb_which = 2u;
    cpu.read_absorb[2] = 41u;
    psx_icache_fetch_generated(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u, "generated cold fetch preserves refill cost"))
        return 1;
    if (!expect(cpu.read_absorb_which == 0u && cpu.read_absorb[2] == 0u,
                "generated miss clears selected load give-back"))
        return 1;

    cpu.read_absorb_which = 2u;
    cpu.read_absorb[2] = 29u;
    psx_icache_fetch_generated(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u, "generated steady tag hit is cycle-free"))
        return 1;
    if (!expect(cpu.read_absorb_which == 2u && cpu.read_absorb[2] == 29u,
                "generated tag hit preserves load give-back"))
        return 1;

    psx_icache_reset();
    g_psx_icache_active = 1;
    test_cycles = 0u;
    g_ls_replay_active = 1;
    psx_icache_fetch_generated(&cpu, 0x80010000u);
    g_ls_replay_active = 0;
    if (!expect(test_cycles == 0u && g_psx_icache_tv[0] == 1u,
                "ordinary replay remains cache-transactional"))
        return 1;

    if (!expect(psx_icache_shadow_record_begin(), "record shadow entry")) return 1;
    psx_icache_fetch_generated(&cpu, 0x80010000u);
    if (!expect(test_cycles == 7u, "live cache warms after record")) return 1;
    if (!expect(psx_icache_shadow_replay_begin(), "begin shadow replay")) return 1;
    test_cycles = 0u;
    g_ls_replay_active = 1;
    psx_icache_fetch_generated(&cpu, 0x80010000u);
    g_ls_replay_active = 0;
    if (!expect(test_cycles == 7u,
                "shadow replay evolves its transactional cache view"))
        return 1;
    psx_icache_shadow_replay_end();
    test_cycles = 0u;
    psx_icache_fetch_generated(&cpu, 0x80010000u);
    if (!expect(test_cycles == 0u, "shadow end restores warmed live cache"))
        return 1;

    g_psx_icache_active = 0;
    psx_icache_fetch_generated(&cpu, 0x80010010u);
    if (!expect(test_cycles == 0u, "disabled generated cache gate is free"))
        return 1;

    puts("PASS: generated constant-address I-cache hit gate preserves semantics");
    return 0;
}
