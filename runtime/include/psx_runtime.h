#ifndef PSXRECOMP_PSX_RUNTIME_H
#define PSXRECOMP_PSX_RUNTIME_H

#include "cpu_state.h"
#include "debug_server.h"
#include "interrupts.h"
#include "psx_cycles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fix B exception-return escape state + helpers are declared in cpu_state.h
 * (the lowest common header the generated BIOS/game both include). */

static inline void call_by_address(CPUState* cpu, uint32_t addr) {
    psx_dispatch_call(cpu, addr, cpu->gpr[31]);
}

/* ---- Enhancement scratch RAM (generic, DEFAULT OFF; see runtime/src/memory.c)
 *
 * Guest-addressable memory that the game can never touch, for enhancements
 * that need somewhere to park state. Mapped ABOVE the 4x main-RAM mirror
 * (physical >= 0x00800000), a window that is unmapped on real hardware and
 * open-bus here — so unlike a "free" hole inside main RAM it cannot be
 * overwritten by the game (notably not by CD-DMA stage loads, which is exactly
 * what invalidated a hand-picked reservation inside MMX6's heap->stack gap).
 *
 * Disabled until a game opts in, and the checks live off the main-RAM fast
 * path, so a game that never calls configure() is byte-identical.
 *
 * Guest address = 0x80000000 | phys_base (KSEG0) for the configured window. */
int      psx_enh_scratch_configure(uint32_t phys_base, uint32_t len);
void     psx_enh_scratch_reset(void);
uint8_t *psx_enh_scratch_ptr(uint32_t *out_base, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_RUNTIME_H */
