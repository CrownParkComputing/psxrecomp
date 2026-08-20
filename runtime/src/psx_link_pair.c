/* psx_link_pair.c -- see psx_link_pair.h for the design contract. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#include "psx_link_pair.h"

#if !defined(PSX_HAS_RECOMP_NET)
/* PSX-Link pairs ride the netplay stack; titles built without recomp-net get
 * inert stubs (and never spawn followers). */
int  psx_link_pair_client_start(const PsxLinkPairClientCfg *cfg) { (void)cfg; return 0; }
int  psx_link_pair_active(void) { return 0; }
int  psx_link_pair_base_seat(void) { return 0; }
void psx_link_pair_stage_row(uint32_t t, int r, const uint8_t b[8]) { (void)t; (void)r; (void)b; }
int  psx_link_pair_on_admit(uint32_t t) { (void)t; return 1; }
void psx_link_pair_on_save(uint32_t t) { (void)t; }
int  psx_link_pair_after_load(uint32_t t) { (void)t; return 1; }
void psx_link_pair_shutdown(void) {}
int  psx_link_pair_failed(void) { return 0; }
int  psx_link_pair_follower_mode(void) { return 0; }
int  psx_link_pair_follower_booted(void) { return 0; }
int  psx_link_pair_follower_boot(struct CPUState *c, uint32_t b, uint32_t e,
                                 uint32_t f, uint32_t i, uint32_t h)
{ (void)c; (void)b; (void)e; (void)f; (void)i; (void)h; return 0; }
int  psx_link_pair_follower_admit(void) { return 0; }
void psx_link_pair_follower_note_finish(void) {}
#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>   /* Sleep — the tick-0 pairing gate's 1 ms nap */
#else
#  include <signal.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "netplay_snap_ring.h"
#include "netplay_state_digest.h"
#include "psx_netplay_rb.h"
#include "cpu_state.h"
#include "sio.h"

extern uint64_t psx_get_cycle_count(void);
extern void psx_scheduler_resume_at(uint32_t pc);

/* psx_netplay.c: decode one 8-byte netplay pad blob onto a local SIO port. */
extern void psx_netplay_apply_pad_blob(int port, const uint8_t row[8]);

/* psx_netplay_rb.c: follower-side snapshot save/apply reusing the exact rb
 * machinery (resume-pc pick, boot_state, restore resync battery). */
extern int psx_netplay_rb_pair_snap_save(void *ring, uint32_t tick,
                                         struct CPUState *cpu, uint32_t bios,
                                         uint32_t entry);
extern uint32_t psx_netplay_rb_pair_snap_load_apply(void *ring, uint32_t tick,
                                                    struct CPUState *cpu,
                                                    uint32_t bios,
                                                    uint32_t entry);

#define PAIR_ERR_NO_SNAP   1u
#define PAIR_ERR_LOAD_FAIL 2u
#define PAIR_ERR_CFG       3u
#define PAIR_ERR_ORDER     4u

static const uint8_t k_neutral_row[8] = {
    0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80, 0x01, 0x01,
};

/* ===== driver =========================================================== */

static struct {
    int      active;
    int      failed;
    int      started;         /* START pushed, epoch anchored */
    int      base_seat;
    uint32_t load_count;      /* LOADs pushed (ack fence counter) */
    long     child_pid;
    uint32_t stage_tick;
    uint8_t  stage_rows[2][8];
    uint8_t  stage_valid;     /* bitmask per rel seat */
    uint8_t  last_rows[2][8];
    int      last_rows_valid;
} g_drv;

static void drv_fail(const char *why) {
    if (!g_drv.failed) {
        fprintf(stderr, "psxrecomp: link pair FAILED — %s\n", why ? why : "?");
        fflush(stderr);
    }
    g_drv.failed = 1;
}

int psx_link_pair_client_start(const PsxLinkPairClientCfg *cfg) {
    PsxLinkPairCfg pc;
    char role;
    if (!cfg || g_drv.active) return 0;
    memset(&g_drv, 0, sizeof(g_drv));
    g_drv.base_seat = cfg->base_seat;

    /* Side = CONSOLE identity (A=0, B=1), not driver/follower. */
    role = (cfg->base_seat >= 2) ? 'b' : 'a';
    if (!psx_link_shm_open(cfg->shm_name, role)) {
        drv_fail("shm open");
        return 0;
    }
    memset(&pc, 0, sizeof(pc));
    pc.session_id = cfg->session_id;
    pc.driver_side = (role == 'b') ? 1u : 0u;
    pc.tick_len_cycles = cfg->tick_len_cycles;
    pc.latency_cycles = cfg->latency_cycles
                            ? cfg->latency_cycles
                            : cfg->tick_len_cycles + cfg->tick_len_cycles / 8u;
    pc.flags = cfg->flags;
    pc.bios_id = cfg->bios_id;
    pc.codegen_hash = cfg->codegen_hash;
    if (!psx_link_shm_pair_init(&pc)) {
        drv_fail("pair init");
        return 0;
    }

#if defined(_WIN32)
    fprintf(stderr, "psxrecomp: link pair spawn not ported to Windows yet\n");
    drv_fail("spawn unsupported");
    return 0;
#else
    {
        pid_t pid = fork();
        if (pid < 0) {
            drv_fail("fork");
            return 0;
        }
        if (pid == 0) {
            if (cfg->child_env) {
                for (char *const *e = cfg->child_env; *e; ++e) {
                    char *dup = strdup(*e);
                    char *eq = dup ? strchr(dup, '=') : NULL;
                    if (dup && eq) {
                        *eq = '\0';
                        setenv(dup, eq + 1, 1);
                    }
                    free(dup);
                }
            }
            execv(cfg->exe_path, (char *const *)cfg->child_argv);
            fprintf(stderr, "psxrecomp: link follower exec failed: %s\n",
                    cfg->exe_path);
            _exit(127);
        }
        g_drv.child_pid = (long)pid;
    }
#endif
    g_drv.active = 1;
    fprintf(stdout,
            "psxrecomp: link pair started (console %c seats %d/%d, follower "
            "pid %ld, shm '%s')\n",
            role == 'b' ? 'B' : 'A', cfg->base_seat, cfg->base_seat + 1,
            g_drv.child_pid, cfg->shm_name ? cfg->shm_name : "?");
    return 1;
}

int psx_link_pair_active(void) { return g_drv.active; }
int psx_link_pair_base_seat(void) { return g_drv.base_seat; }
int psx_link_pair_failed(void) { return g_drv.active && g_drv.failed; }

void psx_link_pair_stage_row(uint32_t tick, int rel, const uint8_t row[8]) {
    if (!g_drv.active || rel < 0 || rel > 1 || !row) return;
    if (g_drv.stage_tick != tick) {
        g_drv.stage_tick = tick;
        g_drv.stage_valid = 0;
    }
    memcpy(g_drv.stage_rows[rel], row, 8);
    g_drv.stage_valid |= (uint8_t)(1u << rel);
    memcpy(g_drv.last_rows[rel], row, 8);
    g_drv.last_rows_valid = 1;
}

int psx_link_pair_on_admit(uint32_t tick) {
    PsxLinkPairCmd c;
    if (!g_drv.active) return 1;
    if (g_drv.failed) return 0;

    if (!g_drv.started) {
        /* Tick-0 gate: the cable must be connected from tick 0 on EVERY
         * machine, or early DSR reads fork. Wait out the follower's process
         * boot here (both processes park pre-guest, so this is launch skew,
         * not guest time). */
        uint32_t paired = 0;
        int spins = 0;
        for (;;) {
            psx_link_shm_stats(NULL, NULL, &paired, NULL);
            if (paired) break;
            psx_link_shm_poll(psx_get_cycle_count());   /* pairs + heartbeats */
            if (++spins > 60000) {                       /* ~60 s of 1 ms naps */
                drv_fail("follower never attached");
                return 0;
            }
#if defined(_WIN32)
            Sleep(1);
#else
            {
                struct timespec ts = { 0, 1000000 };
                nanosleep(&ts, NULL);
            }
#endif
        }
        psx_link_shm_anchor_epoch(psx_get_cycle_count());
        memset(&c, 0, sizeof(c));
        c.kind = PSX_LINK_CMD_START;
        if (!psx_link_shm_cmd_push(&c)) {
            drv_fail("START push");
            return 0;
        }
        g_drv.started = 1;
    }

    /* Pipeline barrier: the determinism proof needs both sides done with
     * T-1 before either runs T (latency >= tick covers the in-tick overlap). */
    if (tick > 0 && !psx_link_shm_wait_follower_tick(tick - 1u)) {
        drv_fail("follower lost at tick barrier");
        return 0;
    }

    memset(&c, 0, sizeof(c));
    c.kind = PSX_LINK_CMD_TICK;
    c.tick = tick;
    if (g_drv.stage_valid == 0x3u && g_drv.stage_tick == tick) {
        memcpy(c.rows[0], g_drv.stage_rows[0], 8);
        memcpy(c.rows[1], g_drv.stage_rows[1], 8);
    } else if (g_drv.last_rows_valid) {
        /* Publish path did not touch the foreign seats this tick (e.g. an
         * early boot tick) — carry the last applied rows, like SIO latches. */
        memcpy(c.rows[0], g_drv.last_rows[0], 8);
        memcpy(c.rows[1], g_drv.last_rows[1], 8);
    } else {
        memcpy(c.rows[0], k_neutral_row, 8);
        memcpy(c.rows[1], k_neutral_row, 8);
    }
    if (!psx_link_shm_cmd_push(&c)) {
        drv_fail("TICK push");
        return 0;
    }
    return 1;
}

void psx_link_pair_on_save(uint32_t tick) {
    PsxLinkPairCmd c;
    if (!g_drv.active || g_drv.failed || !g_drv.started) return;
    memset(&c, 0, sizeof(c));
    c.kind = PSX_LINK_CMD_SAVE;
    c.tick = tick;
    if (!psx_link_shm_cmd_push(&c))
        drv_fail("SAVE push");
}

int psx_link_pair_after_load(uint32_t tick) {
    PsxLinkPairCmd c;
    if (!g_drv.active) return 1;
    if (g_drv.failed) return 0;
    memset(&c, 0, sizeof(c));
    c.kind = PSX_LINK_CMD_LOAD;
    c.tick = tick;
    if (!psx_link_shm_cmd_push(&c)) {
        drv_fail("LOAD push");
        return 0;
    }
    g_drv.load_count++;
    /* Fence: the follower must finish draining + restoring before any driver
     * guest code can consume cable bytes from the abandoned timeline. */
    if (!psx_link_shm_wait_load_ack(g_drv.load_count)) {
        drv_fail("LOAD ack (follower dead or snap miss)");
        return 0;
    }
    /* Staged rows past the restore point belong to the dead timeline. */
    if (g_drv.stage_tick > tick)
        g_drv.stage_valid = 0;
    return 1;
}

void psx_link_pair_shutdown(void) {
    if (!g_drv.active) return;
    {
        PsxLinkPairCmd c;
        memset(&c, 0, sizeof(c));
        c.kind = PSX_LINK_CMD_STOP;
        (void)psx_link_shm_cmd_push(&c);
    }
#if !defined(_WIN32)
    if (g_drv.child_pid > 0) {
        int st = 0;
        int waited = 0;
        for (; waited < 2000; waited += 50) {
            pid_t r = waitpid((pid_t)g_drv.child_pid, &st, WNOHANG);
            if (r == (pid_t)g_drv.child_pid || r < 0) break;
            {
                struct timespec ts = { 0, 50000000 };
                nanosleep(&ts, NULL);
            }
        }
        if (waited >= 2000) {
            fprintf(stderr,
                    "psxrecomp: link follower pid %ld did not stop — killing\n",
                    g_drv.child_pid);
            kill((pid_t)g_drv.child_pid, SIGKILL);
            (void)waitpid((pid_t)g_drv.child_pid, &st, 0);
        }
        g_drv.child_pid = 0;
    }
#endif
    g_drv.active = 0;
    g_drv.started = 0;
    fprintf(stdout, "psxrecomp: link pair shut down\n");
}

/* ===== follower ========================================================= */

static struct {
    int              booted;
    struct CPUState *cpu;
    uint32_t         bios_checksum;
    uint32_t         entry_pc;
    NetplaySnapRing *ring;
    uint32_t         last_ran;
    int              have_ran;
    uint32_t         pending_save_tick;
    int              pending_save;
    int              dig_published;   /* note_finish ran for last_ran */
} g_fol;

int psx_link_pair_follower_booted(void) { return g_fol.booted; }

int psx_link_pair_follower_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("PSX_LINK_FOLLOWER");
        mode = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return mode;
}

int psx_link_pair_follower_boot(struct CPUState *cpu, uint32_t bios_checksum,
                                uint32_t entry_pc, uint32_t own_flags,
                                uint32_t bios_id, uint32_t codegen_hash) {
    PsxLinkPairCfg cfg;
    int spins = 0;
    g_fol.last_ran = 0xFFFFFFFFu;               /* "none ran yet" sentinel */
    if (!psx_link_shm_active()) {
        fprintf(stderr, "psxrecomp: link follower without shm backend\n");
        return 0;
    }
    /* Wait for the driver's cfg publication (it runs pair_init before the
     * spawn, so this is usually immediate). */
    while (!psx_link_shm_pair_cfg(&cfg)) {
        psx_link_shm_poll(psx_get_cycle_count());
        if (++spins > 60000) {
            fprintf(stderr, "psxrecomp: link follower: no pair cfg\n");
            return 0;
        }
#if !defined(_WIN32)
        {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
        }
#endif
    }
    if (cfg.codegen_hash != codegen_hash || cfg.bios_id != bios_id ||
        cfg.flags != own_flags) {
        fprintf(stderr,
                "psxrecomp: link follower CONFIG MISMATCH — "
                "hash %08x/%08x bios %u/%u flags %x/%x — refusing pair\n",
                cfg.codegen_hash, codegen_hash, cfg.bios_id, bios_id,
                cfg.flags, own_flags);
        psx_link_shm_set_follower_err(PAIR_ERR_CFG);
        return 0;
    }
    g_fol.cpu = cpu;
    g_fol.bios_checksum = bios_checksum;
    g_fol.entry_pc = entry_pc;
    sio_netplay_canonicalize_session_pads(2);
    g_fol.ring = netplay_snap_ring_create(NETPLAY_SNAP_RING_DEFAULT_DEPTH);
    if (!g_fol.ring) {
        psx_link_shm_set_follower_err(PAIR_ERR_LOAD_FAIL);
        return 0;
    }
    g_fol.booted = 1;
    fprintf(stdout,
            "psxrecomp: link follower paired (console %c, session %u)\n",
            cfg.driver_side ? 'A' : 'B', cfg.session_id);
    return 1;
}

static void fol_exec_save(uint32_t tick) {
    if (!psx_netplay_rb_pair_snap_save(g_fol.ring, tick, g_fol.cpu,
                                       g_fol.bios_checksum, g_fol.entry_pc)) {
        /* Tolerated until a LOAD references it — the driver's own saves can
         * miss on a non-dispatchable PC too. Loud, not fatal. */
        fprintf(stderr,
                "psxrecomp: link follower snap save MISS tick=%u\n",
                (unsigned)tick);
        fflush(stderr);
    }
}

void psx_link_pair_follower_note_finish(void) {
    /* Same boundary phase as the client's psx_netplay_finish_frame digest. */
    CPUState dig_cpu;
    NetplayCoreParts parts;
    if (!g_fol.booted || !g_fol.have_ran || !g_fol.cpu)
        return;
    psx_netplay_rb_cpu_for_present_digest(&dig_cpu, g_fol.cpu);
    netplay_core_digest_parts(&dig_cpu, &parts);
    if (g_fol.last_ran == 0u && getenv("PSX_LINK_RAMDUMP")) {
        extern uint8_t *g_psx_ram;
        FILE *f = fopen("/tmp/link_ram_follower.bin", "wb");
        if (f) { fwrite(g_psx_ram, 1, 2u << 20, f); fclose(f); }
    }
    if (g_fol.last_ran < 3u) {
        fprintf(stderr,
                "psxrecomp: link follower parts tick=%u core=%08x cpu=%08x "
                "clk=%08x tim=%08x ram=%08x dirty=%08x\n",
                (unsigned)g_fol.last_ran, (unsigned)parts.core,
                (unsigned)parts.cpu, (unsigned)parts.clock_irq,
                (unsigned)parts.timers, (unsigned)parts.ram,
                (unsigned)parts.dirty);
        fflush(stderr);
    }
    psx_link_shm_publish_digest(g_fol.last_ran, parts.core);
    g_fol.dig_published = 1;
}

int psx_link_pair_follower_admit(void) {
    if (!g_fol.booted) return 0;
    if (g_fol.have_ran) {
        /* The digest was published at the finish_frame boundary point
         * (psx_link_pair_follower_note_finish) — the SAME spot in the vblank
         * present body where the client digests, because the body's tail
         * mutates digested clock/timer bookkeeping (measured: client and
         * follower digests of an identical console differed systematically
         * when the follower digested here instead). Late fallback only if a
         * present path skipped the note. */
        if (!g_fol.dig_published) {
            CPUState dig_cpu;
            psx_netplay_rb_cpu_for_present_digest(&dig_cpu, g_fol.cpu);
            psx_link_shm_publish_digest(g_fol.last_ran,
                                        netplay_core_digest(&dig_cpu));
        }
        g_fol.dig_published = 0;
        psx_link_shm_set_done_tick(g_fol.last_ran);
        g_fol.have_ran = 0;
        /* A save keyed to the tick that just ran executes at THIS boundary
         * (post-run-T), mirroring the driver's finish_frame flush phase. */
        if (g_fol.pending_save && g_fol.pending_save_tick == g_fol.last_ran) {
            fol_exec_save(g_fol.pending_save_tick);
            g_fol.pending_save = 0;
        }
    }
    for (;;) {
        PsxLinkPairCmd c;
        if (!psx_link_shm_cmd_wait_pop(&c)) {
            fprintf(stderr, "psxrecomp: link follower: driver gone\n");
            return 0;
        }
        switch (c.kind) {
        case PSX_LINK_CMD_START:
            psx_link_shm_anchor_epoch(psx_get_cycle_count());
            break;
        case PSX_LINK_CMD_SAVE:
            /* Boundary state here == post-run-last_ran. A save keyed to the
             * just-ran tick executes now; a save keyed ahead pends until its
             * TICK has run (fol admit entry). Phase skew vs the driver's own
             * snap point is tolerated by the log model: re-executions rewrite
             * cable bytes bit-identically, so mixed-phase restores reconverge. */
            if (c.tick == g_fol.last_ran) {
                fol_exec_save(c.tick);
                break;
            }
            if (g_fol.pending_save && g_fol.pending_save_tick == c.tick)
                break;                          /* idempotent re-request */
            if (g_fol.pending_save) {
                fprintf(stderr,
                        "psxrecomp: link follower save ORDER (pend=%u new=%u "
                        "ran=%u)\n",
                        (unsigned)g_fol.pending_save_tick, (unsigned)c.tick,
                        (unsigned)g_fol.last_ran);
                psx_link_shm_set_follower_err(PAIR_ERR_ORDER);
                return 0;
            }
            g_fol.pending_save_tick = c.tick;
            g_fol.pending_save = 1;
            break;
        case PSX_LINK_CMD_TICK:
            psx_netplay_apply_pad_blob(0, c.rows[0]);
            psx_netplay_apply_pad_blob(1, c.rows[1]);
            g_fol.last_ran = c.tick;
            g_fol.have_ran = 1;
            return 1;
        case PSX_LINK_CMD_LOAD: {
            uint32_t pc;
            if (!netplay_snap_ring_has(g_fol.ring, c.tick)) {
                fprintf(stderr,
                        "psxrecomp: link follower LOAD MISS tick=%u\n",
                        (unsigned)c.tick);
                psx_link_shm_set_follower_err(PAIR_ERR_NO_SNAP);
                return 0;
            }
            pc = psx_netplay_rb_pair_snap_load_apply(g_fol.ring, c.tick,
                                                     g_fol.cpu,
                                                     g_fol.bios_checksum,
                                                     g_fol.entry_pc);
            if (!pc) {
                fprintf(stderr,
                        "psxrecomp: link follower LOAD FAIL tick=%u\n",
                        (unsigned)c.tick);
                psx_link_shm_set_follower_err(PAIR_ERR_LOAD_FAIL);
                return 0;
            }
            /* Abandoned-timeline bookkeeping BEFORE the ack releases the
             * driver: snaps above the restore are dead; done-count rewinds
             * (post-run-T restored => T executed); staged saves cancel. */
            (void)netplay_snap_ring_drop_after(g_fol.ring, c.tick);
            if (g_fol.pending_save && g_fol.pending_save_tick > c.tick)
                g_fol.pending_save = 0;
            g_fol.last_ran = c.tick;
            g_fol.have_ran = 0;
            psx_link_shm_rewind_done_count(c.tick + 1u);
            psx_link_shm_load_ack_publish();
            psx_scheduler_resume_at(pc);
            return 1; /* unreachable */
        }
        case PSX_LINK_CMD_STOP:
            fprintf(stdout, "psxrecomp: link follower: session over\n");
            return 0;
        default:
            break;
        }
    }
}

#endif /* PSX_HAS_RECOMP_NET */
