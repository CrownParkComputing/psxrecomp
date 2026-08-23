/* Compact generated-game R3000A timing runs (v6).
 *
 * Keep these helpers in one runtime translation unit. A header-local noinline
 * body is still eligible for GCC IPA constant-propagation clones keyed by each
 * generated mask array, recreating the text explosion this path exists to
 * remove. The shared external boundary is deliberate. */
#include "psx_cyc.h"

#if defined(__GNUC__) && !defined(__clang__)
#define PSX_CYC_RUN_NOINLINE __attribute__((noinline, noclone))
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_CYC_RUN_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PSX_CYC_RUN_NOINLINE __declspec(noinline)
#else
#define PSX_CYC_RUN_NOINLINE
#endif

PSX_CYC_RUN_NOINLINE void psx_cyc_step_slow(
    CPUState* cpu, uint32_t reg_mask) {
    psx_cyc_step(cpu, reg_mask);
}

PSX_CYC_RUN_NOINLINE int psx_cyc_step_run_fast(
    CPUState* cpu, const uint32_t* reg_masks, uint32_t count) {
    uint32_t charges = 0u;
    uint32_t i;

    if (count == 0u) return 1;
    if (!g_psx_cyc_inline_fast ||
        count > UINT32_MAX - g_psx_cyc_batch)
        return 0;

    for (i = 0u; i < count; ++i) {
        uint8_t w = cpu->read_absorb_which;
        if (cpu->read_absorb[w]) cpu->read_absorb[w]--;
        else                     charges++;
        psx_cyc_deps(cpu, reg_masks[i]);
        psx_cyc_lds(cpu);
    }
    g_psx_cyc_batch += charges;
    return 1;
}
