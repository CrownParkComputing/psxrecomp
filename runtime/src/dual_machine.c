/*
 * dual_machine.c -- Stage B dual-console driver. See dual_machine.h.
 *
 * The switch primitive is exactly the proven savestate mechanic
 * (savestate.c savestate_poll): resolve a dispatchable resume PC, save the
 * whole machine with boot_state_save_buffer_raw, load the other machine's
 * blob with boot_state_load_buffer, run the post-restore resyncs, and
 * psx_scheduler_resume_at() -- which longjmps to the scheduler and abandons
 * the suspended CPS frames (safe: every block leader is re-enterable).
 *
 * What deliberately does NOT swap: the SIO1 crossover wire (host-owned
 * channel between the machines -- BS_SEC_SIO1 apply is suppressed) and the
 * two Sio1Device instances themselves (kept live per machine and installed
 * into the sio1 singleton on switch, so wire continuity is byte-exact).
 */
#include "dual_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_state.h"
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_cycles.h"
#include "psx_link.h"
#include "psx_rewind.h"
#include "psx_scheduler.h"
#include "savestate.h"
#include "sio1.h"

int g_psx_dual_active = 0;

/* Coarse slice while the link is idle (guest cycles). Half an NTSC frame:
 * keeps handshake-line latency under ~8 ms while paying only ~4 swaps per
 * frame-pair. PSX_DUAL_SLICE overrides. */
#define DUAL_SLICE_COARSE_DEFAULT 282240u
/* Floor for the fine (link-armed) slice so a not-yet-programmed BAUD of 0
 * (char_cycles ~7) cannot thrash the switcher. */
#define DUAL_SLICE_FINE_FLOOR 1024u

static int       s_requested;
static int       s_active;
static int       s_live;                /* machine currently executing */
static int       s_local;               /* machine that presents A/V */
static uint8_t  *s_blob[2];             /* parked state (live one is stale) */
static size_t    s_blob_len[2];
static uint64_t  s_cycles[2];           /* last-known guest clock per machine */
static Sio1Device      *s_dev[2];
static PsxLinkEndpoint *s_ep[2];
static uint64_t  s_swaps;
static uint32_t  s_slice_coarse = DUAL_SLICE_COARSE_DEFAULT;
static uint32_t  s_bios_checksum;
static uint32_t  s_entry_pc;
static uint64_t  s_defer_notes;

/* main.cpp: re-push current host pad state into sio.c after a switch load
 * (the blob restored the incoming machine's stale pad snapshot, and the
 * present body -- the normal per-frame push -- runs only for the local
 * machine). Stage B routes the same pads to BOTH machines. */
extern void psx_dual_repush_host_pads(void);

void psx_dual_machine_request(int local_machine) {
    s_requested = 1;
    s_local = local_machine ? 1 : 0;
    g_psx_dual_active = 1;
}

int psx_dual_present_gate(void) {
    return !s_active || s_live == s_local;
}

int psx_dual_machine_live(void) { return s_active ? s_live : -1; }
uint64_t psx_dual_machine_swaps(void) { return s_swaps; }
void psx_dual_machine_cycles(uint64_t out[2]) {
    out[0] = s_cycles[0];
    out[1] = s_cycles[1];
}

/* Same candidate ladder as savestate.c savestate_resolve_resume_pc. */
static uint32_t resolve_resume_pc(const struct CPUState *cpu, uint32_t hint) {
    const uint32_t cands[5] = {
        hint,
        cpu ? cpu->pc : 0u,
        psx_compiled_irq_resume_pc(),
        psx_last_irq_check_pc(),
        cpu ? cpu->gpr[31] : 0u,
    };
    for (int i = 0; i < 5; ++i) {
        uint32_t pc = cands[i];
        if (!pc || (pc & 3u) != 0u)
            continue;
        if (pc == 0x80000080u || pc == 0xbfc00180u || pc == 0x80000000u)
            continue;
        if (psx_is_dispatchable(pc))
            return pc;
    }
    return 0;
}

static int link_armed(void) {
    for (int m = 0; m < 2; m++) {
        if (!s_dev[m]) continue;
        if (sio1_device_active(s_dev[m])) return 1;
        if (sio1_device_peek_ctrl(s_dev[m]) & SIO1_CTRL_DTR) return 1;
    }
    return 0;
}

static uint32_t current_slice(void) {
    if (!link_armed())
        return s_slice_coarse;
    {
        uint32_t a = sio1_device_char_cycles(s_dev[0]);
        uint32_t b = sio1_device_char_cycles(s_dev[1]);
        uint32_t fine = a < b ? a : b;
        if (fine < DUAL_SLICE_FINE_FLOOR) fine = DUAL_SLICE_FINE_FLOOR;
        if (fine > s_slice_coarse) fine = s_slice_coarse;
        return fine;
    }
}

static int save_live_blob(struct CPUState *cpu, uint32_t pc) {
    uint8_t *data = NULL;
    size_t len = 0;
    CPUState snap = *cpu;
    snap.pc = pc;                        /* cpu->pc is 0 mid-block */
    if (!boot_state_save_buffer_raw(&snap, s_bios_checksum, s_entry_pc,
                                    &data, &len))
        return 0;
    free(s_blob[s_live]);
    s_blob[s_live] = data;
    s_blob_len[s_live] = len;
    return 1;
}

/* First poll with a safe resume PC: capture the shared t0 state as the
 * OTHER machine's start blob and arm the crossover. Machine 0 keeps
 * executing (no restore needed -- both machines are identical at t0). */
static void try_activate(struct CPUState *cpu, uint32_t hint) {
    uint32_t pc = resolve_resume_pc(cpu, hint);
    if (!pc)
        return;
    savestate_get_integrity(&s_bios_checksum, &s_entry_pc);
    {
        const char *e = getenv("PSX_DUAL_SLICE");
        if (e && e[0]) {
            unsigned long v = strtoul(e, NULL, 0);
            if (v >= DUAL_SLICE_FINE_FLOOR && v <= 33868800ul)
                s_slice_coarse = (uint32_t)v;
        }
    }

    s_live = 0;
    if (!save_live_blob(cpu, pc)) {
        fprintf(stderr, "dual: activation save failed -- dual mode OFF\n");
        s_requested = 0;
        g_psx_dual_active = 0;
        return;
    }
    /* The t0 blob is machine 1's start state. */
    s_blob[1] = (uint8_t *)malloc(s_blob_len[0]);
    if (!s_blob[1]) {
        fprintf(stderr, "dual: activation alloc failed -- dual mode OFF\n");
        s_requested = 0;
        g_psx_dual_active = 0;
        return;
    }
    memcpy(s_blob[1], s_blob[0], s_blob_len[0]);
    s_blob_len[1] = s_blob_len[0];

    /* Cross-wire: machine 0 adopts the existing singleton device; machine 1
     * gets a fresh instance. Wire state lives in the crossover (host-owned)
     * and never swaps -- suppress BS_SEC_SIO1 apply from here on. */
    psx_link_crossover_create(&s_ep[0], &s_ep[1]);
    if (!s_ep[0] || !s_ep[1]) {
        fprintf(stderr, "dual: crossover alloc failed -- dual mode OFF\n");
        s_requested = 0;
        g_psx_dual_active = 0;
        return;
    }
    s_dev[0] = sio1_get_device();
    s_dev[1] = sio1_device_create();
    if (!s_dev[0] || !s_dev[1]) {
        fprintf(stderr, "dual: sio1 device missing -- dual mode OFF\n");
        s_requested = 0;
        g_psx_dual_active = 0;
        return;
    }
    sio1_device_attach(s_dev[0], s_ep[0]);
    sio1_device_attach(s_dev[1], s_ep[1]);
    sio1_device_reset(s_dev[1], psx_get_cycle_count());
    sio1_dual_suppress_snapshot_apply(1);

    /* Rewind's snapshot ring would interleave two different machines. */
    psx_rewind_shutdown();
    /* Per-switch load_timing stderr would be ~100+ lines/s. */
    boot_state_set_quiet_load(1);

    s_cycles[0] = s_cycles[1] = psx_get_cycle_count();
    s_swaps = 0;
    s_active = 1;
    fprintf(stderr,
            "dual: ACTIVE -- two consoles from pc=0x%08X cyc=%llu "
            "(local=%d, coarse slice=%u, blob=%zu bytes)\n",
            (unsigned)pc, (unsigned long long)psx_get_cycle_count(),
            s_local, s_slice_coarse, s_blob_len[0]);
}

static void switch_machines(struct CPUState *cpu, uint32_t pc) {
    if (!save_live_blob(cpu, pc)) {
        /* Failed save: keep running this machine; retry at the next edge. */
        fprintf(stderr, "dual: save failed at switch (machine %d) -- retry\n",
                s_live);
        return;
    }
    s_cycles[s_live] = psx_get_cycle_count();
    s_live ^= 1;
    sio1_dual_install(s_dev[s_live]);
    if (!boot_state_load_buffer(s_blob[s_live], s_blob_len[s_live],
                                s_bios_checksum, s_entry_pc, cpu)) {
        /* Restore failure is fatal for the pair: the live blob was just
         * replaced and the other is unloadable. Freeze dual mode. */
        fprintf(stderr,
                "dual: FATAL load failure switching to machine %d -- "
                "dual mode OFF (staying on machine %d)\n",
                s_live, s_live ^ 1);
        s_live ^= 1;
        sio1_dual_install(s_dev[s_live]);
        s_active = 0;
        s_requested = 0;
        g_psx_dual_active = 0;
        return;
    }
    psx_cycles_resync_after_restore(cpu);
    interrupts_resync_after_restore();
    /* NO cdrom_accelerate_after_savestate (switches resume exact state)
     * and NO psx_frontend_on_savestate_loaded (it re-anchors pacing/FPS/
     * audio -- correct once per user load, harmful at per-switch rate;
     * each machine's own timeline is continuous across suspension). */
    psx_dual_repush_host_pads();
    s_swaps++;
    /* Never returns: longjmp to the scheduler, dispatch the restored PC. */
    psx_scheduler_resume_at(cpu->pc);
}

void psx_dual_machine_poll(struct CPUState *cpu, uint32_t resume_pc) {
    uint64_t now, other;
    uint32_t pc;
    if (!s_requested)
        return;
    if (!s_active) {
        try_activate(cpu, resume_pc);
        return;
    }
    now = psx_get_cycle_count();
    s_cycles[s_live] = now;
    other = s_cycles[s_live ^ 1];
    if (now < other + current_slice())
        return;                          /* keep running this machine */
    pc = resolve_resume_pc(cpu, resume_pc);
    if (!pc) {
        s_defer_notes++;
        return;                          /* no safe boundary; retry */
    }
    switch_machines(cpu, pc);
}
