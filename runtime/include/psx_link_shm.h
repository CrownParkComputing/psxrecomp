/*
 * psx_link_shm.h -- shared-memory serial link between TWO PROCESSES.
 *
 * The performance route to full-speed link play: each console is a complete
 * single-console runtime in its own process (own core, own window, own pad),
 * and the SIO1 "cable" is a pair of SPSC rings in a shared-memory segment,
 * paced by the same bounded-skew barrier the in-process dual mode validated
 * (psx_dual_barrier_reached). In-process dual halves the frame rate by
 * construction -- two guests on one core -- and the alternative (threads in
 * one process) requires making ~590 device-module statics and 66 config
 * setters per-machine; two processes get each console a core for free.
 *
 * TIMELINE. The two processes boot at different wall times, so their
 * psx_cycle_count values are unrelated. The link runs on LINK-TIME: local
 * cycles minus an epoch latched when the pair forms (both sides present).
 * tx stamps due = sender_link_now + latency; rx releases when
 * receiver_link_now >= due. The barrier keeps |A - B| <= lookahead and
 * latency >= lookahead, so a byte's due cycle is never already in the
 * receiver's past -- the same causality rule as the fiber dual mode.
 *
 * LIVENESS. Ops stay pure (determinism contract in psx_link.h); the wall
 * clock is used ONLY by the barrier poll for peer-death detection. A dead
 * peer (stale heartbeat) reads as an unplugged cable: connected()=0,
 * DSR/CTS low, barrier disengaged.
 */
#ifndef PSX_LINK_SHM_H
#define PSX_LINK_SHM_H

#include <stdint.h>
#include "psx_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create-or-attach the segment and return this process's endpoint, or NULL.
 * Role: 'a' creates/claims side A, 'b' side B, 0 = first free side. */
PsxLinkEndpoint *psx_link_shm_open(const char *name, char role);
void             psx_link_shm_close(PsxLinkEndpoint *ep);
int              psx_link_shm_is(const PsxLinkEndpoint *ep);

/* Bounded-skew barrier + pairing + heartbeat. Called from the emu thread's
 * interrupt-poll site; blocks (short sleeps) while this side is more than
 * `lookahead` link-cycles ahead of the peer. Cheap no-op when inactive. */
void psx_link_shm_poll(uint64_t cycle_now);
int  psx_link_shm_active(void);

/* Diagnostics for the [DUAL]-style status line. */
void psx_link_shm_stats(uint64_t *my_link_now, uint64_t *peer_link_now,
                        uint32_t *paired, uint64_t *barrier_waits_ms);

#ifdef __cplusplus
}
#endif
#endif /* PSX_LINK_SHM_H */
