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
int      psx_enh_scratch_contains(uint32_t phys);

/* Host pointer for a guest DATA block in main RAM or the scratch window, or
 * NULL if it does not lie wholly inside one. For bulk block moves an
 * enhancement makes on its own behalf; guest-visible accesses still go through
 * the psx_read / psx_write accessors, which carry the write-trace fan-out. */
uint8_t *psx_guest_block_ptr(uint32_t addr, uint32_t len);

/* ---- Simultaneous extra player actors (generic, DEFAULT OFF; coop.c) -------
 *
 * For an engine that runs exactly one player actor through a per-frame stage
 * pipeline. Each stage declared in [coop] replay_sites is replayed once per
 * extra actor, with that actor's saved state SWAPPED INTO the storage the game
 * keeps its player in — so every access finds the right actor however it is
 * addressed, including from streamed overlay code the recompiler never saw.
 *
 * Per-actor state the engine keeps outside that struct (an animation object, a
 * pad-word global) is declared with psx_coop_add_swap_region() and swapped
 * along with it, so extending the swap set never needs new generated code. */
int      psx_coop_configure(uint32_t primary_base, uint32_t struct_len,
                            int companions, uint32_t scratch_phys_base);
int      psx_coop_add_swap_region(uint32_t guest_base, uint32_t len);
void     psx_coop_set_gate(int (*gate)(void));
void     psx_coop_set_gate_state(uint32_t mode_addr, uint8_t mode_val,
                                 const uint32_t *zero_addrs, int zero_count);
void     psx_coop_set_input_layout(uint32_t cur_off, uint32_t edge_off, int enabled);
int      psx_coop_spawn(int idx);
void     psx_coop_despawn(int idx);
void     psx_coop_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_RUNTIME_H */
