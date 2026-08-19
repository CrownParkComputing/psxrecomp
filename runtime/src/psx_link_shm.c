/* psx_link_shm.c -- see psx_link_shm.h for the design contract. */
#include "psx_link_shm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

extern uint64_t psx_get_cycle_count(void);

/* ===== shared segment layout ============================================ */

#define SHM_MAGIC   0x50314C4Bu   /* "P1LK" */
#define SHM_VERSION 2u
/* Cable ring is a LOG in pair mode: entries are retained until the writer
 * laps the reader by a full depth, so rollback can rewind the read cursor.
 * Worst-case retention = rollback window (~40 snaps) x max per-tick burst
 * (~66 chars at 529 kbps) ~= 2.6 KiB; 16 Ki entries is deep slack. */
#define SHM_DEPTH   16384u
#define SHM_CMD_DEPTH 1024u

/* SPSC log: write cursor owned by the producer (peer), read cursor by the
 * consumer (us); both MONOTONIC u64 (never wrapped) so snapshots can carry
 * and restore absolute positions. byte[]/due[] are ordered by the
 * release-store of wr. */
typedef struct ShmRing {
    _Atomic uint64_t wr;
    _Atomic uint64_t rd;
    uint8_t          byte[SHM_DEPTH];
    uint64_t         due[SHM_DEPTH];       /* link-time cycles */
} ShmRing;

typedef struct ShmCmdRing {
    _Atomic uint64_t pushed;
    _Atomic uint64_t consumed;
    PsxLinkPairCmd   cmd[SHM_CMD_DEPTH];
} ShmCmdRing;

typedef struct ShmSeg {
    uint32_t         magic;
    uint32_t         version;
    _Atomic uint32_t present[2];
    _Atomic uint32_t paired;
    /* Bumped whenever a claimer starts a fresh session (no live peer) or
     * reclaims a dead side. Each process latches its epoch when it observes
     * paired==1 with a generation it has not latched yet -- so a rejoining
     * peer gets a fresh epoch pair instead of comparing link-times across
     * unrelated boots. (WALL mode only; PAIR mode anchors explicitly.) */
    _Atomic uint32_t pair_gen;
    _Atomic uint64_t link_now[2];          /* published link-time per side */
    _Atomic uint64_t heartbeat_ms[2];      /* CLOCK_MONOTONIC ms, liveness only */
    _Atomic uint32_t out_lines[2];         /* my DTR/RTS -> peer DSR/CTS */
    uint32_t         latency;              /* wire delay, cycles */
    uint32_t         lookahead;            /* barrier bound, cycles (WALL) */
    /* ---- PAIR mode ---- */
    _Atomic uint32_t pair_mode;            /* 1 once driver published cfg */
    PsxLinkPairCfg   pair_cfg;             /* driver-written, follower-read */
    _Atomic uint32_t done_tick;            /* follower executed-count (t+1) */
    /* Follower post-run core digests, keyed tick & (SHM_DIG_DEPTH-1). Written
     * BEFORE the done_tick release-store; the driver reads a slot only after
     * wait_follower_tick(t), so the release/acquire pair orders them. */
    uint32_t         fol_dig[256];
    _Atomic uint32_t load_ack;             /* completed LOAD commands */
    _Atomic uint32_t follower_err;         /* nonzero: follower fatal */
    ShmCmdRing       cmds;                 /* driver -> follower */
    ShmRing          ring[2];              /* [0] = A->B, [1] = B->A */
} ShmSeg;

/* ===== process-side state =============================================== */

typedef struct ShmEnd {
    ShmSeg  *seg;
    int      side;                 /* 0 = A, 1 = B */
    uint32_t latched_gen;          /* pair_gen the epoch below belongs to */
    int      epoch_armed;          /* PAIR mode: anchor_epoch() ran */
    uint64_t epoch;                /* local cycle at pairing / tick-0 anchor */
    uint64_t waits_ms;             /* total barrier block time (diag) */
    PsxLinkEndpoint ep;
#if defined(_WIN32)
    HANDLE   map;
#else
    char     shm_name[128];
#endif
} ShmEnd;

static ShmEnd *g_shm;              /* one link per process */

/* Clean-quit release: mark our side free so the next launch claims it
 * without waiting out the heartbeat. Crashes skip this; the dead-side
 * reclaim in psx_link_shm_open covers them. */
static void psx_link_shm_release_at_exit(void) {
    ShmEnd *e = g_shm;
    if (!e) return;
    atomic_store(&e->seg->present[e->side], 0u);
    atomic_store(&e->seg->heartbeat_ms[e->side], 0u);
}

static uint64_t mono_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void shm_nap(void) {
#if defined(_WIN32)
    Sleep(0);
#else
    struct timespec nap = { 0, 100000 };   /* 100 us */
    nanosleep(&nap, NULL);
#endif
}

static int pair_mode(const ShmEnd *e) {
    return atomic_load_explicit(&e->seg->pair_mode, memory_order_acquire) != 0;
}

static int epoch_valid(const ShmEnd *e) {
    if (pair_mode(e)) return e->epoch_armed;
    return e->latched_gen != 0 &&
           e->latched_gen == atomic_load_explicit(&e->seg->pair_gen,
                                                  memory_order_relaxed);
}

static uint64_t link_now(const ShmEnd *e, uint64_t cycle_now) {
    return epoch_valid(e) ? (cycle_now - e->epoch) : 0u;
}

/* Peer alive = heartbeat within 3 s. Generous: a peer sitting in a pause
 * menu still heartbeats from its own poll site. */
#define SHM_DEAD_MS 3000u

static int peer_alive(const ShmEnd *e) {
    uint64_t hb = atomic_load_explicit(&e->seg->heartbeat_ms[e->side ^ 1],
                                       memory_order_relaxed);
    return hb != 0 && (mono_ms() - hb) < SHM_DEAD_MS;
}

static int shm_paired(const ShmEnd *e) {
    return atomic_load_explicit(&e->seg->paired, memory_order_acquire) != 0;
}

/* ===== PsxLinkOps ======================================================= */

static ShmRing *out_ring(ShmEnd *e) { return &e->seg->ring[e->side]; }
static ShmRing *in_ring(ShmEnd *e)  { return &e->seg->ring[e->side ^ 1]; }

static int shm_tx(void *s, uint8_t b, uint64_t c) {
    ShmEnd *e = (ShmEnd *)s;
    ShmRing *r = out_ring(e);
    uint64_t wr, rd;
    uint32_t slot;
    if (!shm_paired(e) || !epoch_valid(e)) return 0;     /* no cable yet */
    wr = atomic_load_explicit(&r->wr, memory_order_relaxed);
    rd = atomic_load_explicit(&r->rd, memory_order_acquire);
    if (wr - rd >= SHM_DEPTH) {
        if (!pair_mode(e)) return 0;         /* wire overflow: drop (WALL) */
        /* PAIR: a drop would be nondeterministic (depends on peer timing).
         * Block until the reader frees a slot -- the guest cycle argument is
         * pinned by the caller, so due stays a pure function of the guest
         * timeline. Peer death degrades to the unplugged-cable drop. */
        for (;;) {
            shm_nap();
            rd = atomic_load_explicit(&r->rd, memory_order_acquire);
            if (wr - rd < SHM_DEPTH) break;
            if (!peer_alive(e)) return 0;
        }
    }
    slot = (uint32_t)(wr % SHM_DEPTH);
    r->byte[slot] = b;
    r->due[slot]  = link_now(e, c) + e->seg->latency;
    atomic_store_explicit(&r->wr, wr + 1u, memory_order_release);
    return 1;
}

static int shm_rx(void *s, uint8_t *o, uint64_t c) {
    ShmEnd *e = (ShmEnd *)s;
    ShmRing *r = in_ring(e);
    uint64_t rd, wr;
    uint32_t slot;
    if (!epoch_valid(e)) return 0;
    rd = atomic_load_explicit(&r->rd, memory_order_relaxed);
    wr = atomic_load_explicit(&r->wr, memory_order_acquire);
    if (rd >= wr) return 0;
    slot = (uint32_t)(rd % SHM_DEPTH);
    if (r->due[slot] > link_now(e, c)) return 0;
    *o = r->byte[slot];
    atomic_store_explicit(&r->rd, rd + 1u, memory_order_release);
    return 1;
}

static int shm_rx_peek(void *s, uint64_t *d) {
    ShmEnd *e = (ShmEnd *)s;
    ShmRing *r = in_ring(e);
    uint64_t rd, wr;
    if (!epoch_valid(e)) return 0;
    rd = atomic_load_explicit(&r->rd, memory_order_relaxed);
    wr = atomic_load_explicit(&r->wr, memory_order_acquire);
    if (rd >= wr) return 0;
    /* Translate the stored link-time due back to the CALLER's local cycle
     * timeline: due_local = due_link + epoch. sio1 compares against local. */
    *d = r->due[rd % SHM_DEPTH] + e->epoch;
    return 1;
}

static void shm_set_lines(void *s, uint32_t l) {
    ShmEnd *e = (ShmEnd *)s;
    atomic_store_explicit(&e->seg->out_lines[e->side], l, memory_order_relaxed);
}

static uint32_t shm_get_lines(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    if (!shm_paired(e)) return 0;                        /* cable out: DSR low */
    /* WALL mode treats a dead peer as an unplugged cable. PAIR mode must
     * NOT: get_lines is on the deterministic guest path, and a wall-clock
     * liveness read here would fork machines (one machine's follower stalls
     * 3s on a disc seek and its DSR drops while the others' stay high).
     * Pair-mode death is detected at the command-wait layer instead, where
     * it aborts the whole session. */
    if (!pair_mode(e) && !peer_alive(e)) return 0;
    return atomic_load_explicit(&e->seg->out_lines[e->side ^ 1],
                                memory_order_relaxed);
}

static int shm_connected(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    if (!shm_paired(e)) return 0;
    return pair_mode(e) ? 1 : peer_alive(e);
}

static void shm_reset(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    /* Drop own lines, like the crossover; queued bytes stay (a block reset
     * mid-handshake retransmits anyway, and in PAIR mode a cursor mutation
     * outside snapshot restore would fork determinism). */
    atomic_store_explicit(&e->seg->out_lines[e->side], 0, memory_order_relaxed);
}

/* Savestate / rollback snapshot of the wire, via BS_SEC_SIO1.
 *
 * WALL mode: serialize as an EMPTY inbound ring + own lines (crossover wire
 * format). A char in flight at save time is lost -- link handshakes
 * retransmit; the two processes' clocks are wall-paired anyway.
 *
 * PAIR mode: the cable is a truncatable log; serialize MY cursors -- the
 * read position into the peer's ring and the write position of my own.
 * Restore rewinds the read cursor and truncates my speculative writes. The
 * driver's LOAD fence (push LOAD, wait drained, THEN restore locally)
 * guarantees the peer is parked/level so both mutations are race-free, and
 * causality (due >= send + latency, latency >= tick) guarantees the peer's
 * cursors never point past a truncation. */
#define SHM_SNAP_PAIR_TAG 0x504C524Bu     /* "KRLP" */

static uint32_t shm_snap_bytes(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    return pair_mode(e) ? (4u + 4u + 8u + 8u) : (4u + 4u);
}
static void shm_snap_write(void *s, uint8_t *p, uint64_t now) {
    ShmEnd *e = (ShmEnd *)s;
    uint32_t lines = atomic_load_explicit(&e->seg->out_lines[e->side],
                                          memory_order_relaxed);
    (void)now;
    if (!pair_mode(e)) {
        p[0] = (uint8_t)lines; p[1] = (uint8_t)(lines >> 8);
        p[2] = (uint8_t)(lines >> 16); p[3] = (uint8_t)(lines >> 24);
        p[4] = p[5] = p[6] = p[7] = 0;                    /* ring count = 0 */
        return;
    }
    {
        uint64_t in_rd = atomic_load_explicit(&in_ring(e)->rd,
                                              memory_order_relaxed);
        uint64_t out_wr = atomic_load_explicit(&out_ring(e)->wr,
                                               memory_order_relaxed);
        uint32_t tag = SHM_SNAP_PAIR_TAG;
        memcpy(p + 0, &tag, 4);
        memcpy(p + 4, &lines, 4);
        memcpy(p + 8, &in_rd, 8);
        memcpy(p + 16, &out_wr, 8);
    }
}
static int shm_snap_read(void *s, const uint8_t *p, uint32_t len, uint64_t now) {
    ShmEnd *e = (ShmEnd *)s;
    uint32_t lines;
    (void)now;
    if (len < 8u) return 0;
    if (pair_mode(e)) {
        uint32_t tag;
        uint64_t in_rd, out_wr;
        if (len < 24u) return 0;
        memcpy(&tag, p + 0, 4);
        if (tag != SHM_SNAP_PAIR_TAG) return 0;
        memcpy(&lines, p + 4, 4);
        memcpy(&in_rd, p + 8, 8);
        memcpy(&out_wr, p + 16, 8);
        atomic_store_explicit(&e->seg->out_lines[e->side], lines,
                              memory_order_relaxed);
        /* Rewind my read cursor; truncate my speculative writes. */
        atomic_store_explicit(&in_ring(e)->rd, in_rd, memory_order_release);
        atomic_store_explicit(&out_ring(e)->wr, out_wr, memory_order_release);
        return 1;
    }
    lines = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    atomic_store_explicit(&e->seg->out_lines[e->side], lines,
                          memory_order_relaxed);
    return 1;
}

static const PsxLinkOps s_shm_ops = {
    shm_tx, shm_rx, shm_rx_peek, shm_set_lines, shm_get_lines,
    shm_connected, shm_reset, shm_snap_bytes, shm_snap_write, shm_snap_read,
};

/* ===== open/close ======================================================= */

PsxLinkEndpoint *psx_link_shm_open(const char *name, char role) {
    ShmEnd *e;
    ShmSeg *seg = NULL;
    int side = -1;
    int created = 0;
    if (g_shm) return &g_shm->ep;             /* one per process */
    if (!name || !name[0]) name = "psxrecomp-link";

#if defined(_WIN32)
    char full[160];
    snprintf(full, sizeof full, "Local\\%s", name);
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, (DWORD)sizeof(ShmSeg), full);
    if (!map) return NULL;
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    seg = (ShmSeg *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmSeg));
    if (!seg) { CloseHandle(map); return NULL; }
#else
    char full[160];
    snprintf(full, sizeof full, "/%s", name);
    int fd = shm_open(full, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd >= 0) {
        created = 1;
        if (ftruncate(fd, (off_t)sizeof(ShmSeg)) != 0) {
            close(fd); shm_unlink(full); return NULL;
        }
    } else {
        fd = shm_open(full, O_RDWR, 0600);
        if (fd < 0) return NULL;
    }
    seg = (ShmSeg *)mmap(NULL, sizeof(ShmSeg), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
    close(fd);
    if (seg == MAP_FAILED) return NULL;
#endif

    if (created) {
        memset(seg, 0, sizeof(*seg));
        seg->magic = SHM_MAGIC;
        seg->version = SHM_VERSION;
        /* Lookahead sets the BLOCK CADENCE, and each block costs a ~100 us+
         * jitter nap. At 8192 cycles (242 us of guest) the two processes
         * degenerate into strict run-nap alternation and crawl at ~0.3x --
         * the nap dominates. One PAL frame plus margin: each side's
         * wall-clock pacer freezes its guest clock for up to ~8 ms every
         * frame; a bound below one frame turns that ordinary idle into peer
         * stalls (measured 0.57x at a quarter frame). At one frame the
         * barrier only fires on REAL divergence -- content dips, loads --
         * where lockstep is supposed to wait. Pair speed is min(A, B) by
         * design. Bytes arrive up to one bound LATE on the receiver's clock,
         * exactly how the fiber dual behaved at its coarse slice -- and the
         * libcomb handshake and race traffic tolerated that, measured. */
        seg->lookahead = 700000u;
        seg->latency   = 1024u;
        {
            const char *l = getenv("PSX_LINK_SHM_LOOKAHEAD");
            if (l && l[0]) {
                unsigned long v = strtoul(l, NULL, 0);
                if (v >= 512ul && v <= 2000000ul)
                    seg->lookahead = (uint32_t)v;
            }
            l = getenv("PSX_LINK_SHM_LATENCY");
            if (l && l[0]) {
                unsigned long v = strtoul(l, NULL, 0);
                if (v >= 64ul && v <= 1000000ul)
                    seg->latency = (uint32_t)v;
            }
        }
    } else if (seg->magic != SHM_MAGIC || seg->version != SHM_VERSION) {
        fprintf(stderr, "psx_link_shm: segment %s exists with wrong magic\n", name);
#if defined(_WIN32)
        UnmapViewOfFile(seg); CloseHandle(map);
#else
        munmap(seg, sizeof(ShmSeg));
#endif
        return NULL;
    }

    /* Claim a side. A side is only genuinely taken if its owner is ALIVE
     * (fresh heartbeat): crashed or killed processes never release their
     * claim, and a stale flag must not brick the wire (observed: two clean
     * launches both refused because earlier test runs left both sides
     * marked present). A claimed-but-dead side is a corpse -- take it over
     * and bump the pair generation so any surviving peer re-latches its
     * epoch against us instead of the dead process's timeline. */
    if (role == 'a') side = 0;
    else if (role == 'b') side = 1;
    else side = created ? 0 : 1;
    {
        int claimed = 0;
        for (int attempt = 0; attempt < 2 && !claimed; attempt++) {
            int try_side = (attempt == 0) ? side : (side ^ 1);
            uint32_t expect = 0;
            if (attempt == 1 && role != 0)
                break;              /* explicit role: no fallback side */
            if (atomic_compare_exchange_strong(&seg->present[try_side],
                                               &expect, 1u)) {
                side = try_side; claimed = 1; break;
            }
            /* Occupied: live owner, or corpse? */
            {
                uint64_t hb = atomic_load(&seg->heartbeat_ms[try_side]);
                if (hb == 0 || (mono_ms() - hb) >= SHM_DEAD_MS) {
                    atomic_store(&seg->present[try_side], 1u);
                    atomic_store(&seg->paired, 0u);
                    atomic_fetch_add(&seg->pair_gen, 1u);
                    fprintf(stdout,
                            "psx_link_shm: reclaimed dead side %c\n",
                            try_side ? 'B' : 'A');
                    side = try_side; claimed = 1; break;
                }
            }
        }
        if (!claimed) {
            fprintf(stderr, "psx_link_shm: side %c of %s already claimed "
                    "by a LIVE process\n", side ? 'B' : 'A', name);
#if defined(_WIN32)
            UnmapViewOfFile(seg); CloseHandle(map);
#else
            munmap(seg, sizeof(ShmSeg));
#endif
            return NULL;
        }
    }
    /* No live peer => this claim starts a fresh session: reset the wire so
     * the arriving peer sees clean rings and a fresh pairing generation. */
    {
        uint64_t hb = atomic_load(&seg->heartbeat_ms[side ^ 1]);
        int peer_live = atomic_load(&seg->present[side ^ 1]) &&
                        hb != 0 && (mono_ms() - hb) < SHM_DEAD_MS;
        if (!peer_live) {
            atomic_store(&seg->paired, 0u);
            atomic_fetch_add(&seg->pair_gen, 1u);
            for (int r = 0; r < 2; r++) {
                atomic_store(&seg->ring[r].wr, 0u);
                atomic_store(&seg->ring[r].rd, 0u);
            }
            atomic_store(&seg->out_lines[0], 0u);
            atomic_store(&seg->out_lines[1], 0u);
            atomic_store(&seg->pair_mode, 0u);
            atomic_store(&seg->done_tick, 0u);
            atomic_store(&seg->load_ack, 0u);
            atomic_store(&seg->follower_err, 0u);
            atomic_store(&seg->cmds.pushed, 0u);
            atomic_store(&seg->cmds.consumed, 0u);
            atomic_store(&seg->present[side ^ 1], 0u);
        }
    }

    e = (ShmEnd *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->seg = seg;
    e->side = side;
    e->ep.ops = &s_shm_ops;
    e->ep.self = e;
#if defined(_WIN32)
    e->map = map;
#else
    snprintf(e->shm_name, sizeof e->shm_name, "%s", full);
#endif
    atomic_store(&seg->heartbeat_ms[side], mono_ms());
    g_shm = e;
    atexit(psx_link_shm_release_at_exit);
    fprintf(stdout, "psx_link_shm: attached '%s' as side %c (%s, lookahead=%u)\n",
            name, side ? 'B' : 'A', created ? "created" : "joined",
            seg->lookahead);
    return &e->ep;
}

void psx_link_shm_close(PsxLinkEndpoint *ep) {
    ShmEnd *e;
    if (!ep || ep->ops != &s_shm_ops) return;
    e = (ShmEnd *)ep->self;
    atomic_store(&e->seg->present[e->side], 0u);
    atomic_store(&e->seg->heartbeat_ms[e->side], 0u);
#if defined(_WIN32)
    UnmapViewOfFile(e->seg); CloseHandle(e->map);
#else
    munmap(e->seg, sizeof(ShmSeg));
    /* Last one out removes the name so a fresh pair starts clean. */
    if (!atomic_load(&e->seg->present[e->side ^ 1]))
        shm_unlink(e->shm_name);
#endif
    if (g_shm == e) g_shm = NULL;
    free(e);
}

int psx_link_shm_is(const PsxLinkEndpoint *ep) {
    return ep && ep->ops == &s_shm_ops;
}

int psx_link_shm_active(void) { return g_shm != NULL; }

/* ===== barrier poll ===================================================== */

void psx_link_shm_poll(uint64_t cycle_now) {
    ShmEnd *e = g_shm;
    static uint32_t s_hb_div;
    if (!e) return;

    /* Heartbeat + publish, throttled: mono_ms() per call would be waste. */
    if ((++s_hb_div & 0x3FFu) == 0)
        atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                              memory_order_relaxed);

    if (!shm_paired(e)) {
        /* Pairing: raise the flag the first time both sides are present.
         * Idempotent from either side. */
        if (atomic_load_explicit(&e->seg->present[0], memory_order_relaxed) &&
            atomic_load_explicit(&e->seg->present[1], memory_order_relaxed))
            atomic_store_explicit(&e->seg->paired, 1u, memory_order_release);
        else
            return;
    }
    if (pair_mode(e)) {
        /* PAIR: the command channel paces (tick-ack barrier); this site only
         * heartbeats and publishes link-time for the [SHMLINK] diag. */
        if (epoch_valid(e))
            atomic_store_explicit(&e->seg->link_now[e->side],
                                  link_now(e, cycle_now),
                                  memory_order_relaxed);
        return;
    }
    if (!epoch_valid(e)) {
        /* First poll of a new pairing generation: latch the epoch. Both
         * sides latch within one poll of each other -- microseconds of skew
         * against the frame-scale lookahead. */
        e->epoch = cycle_now;
        e->latched_gen = atomic_load_explicit(&e->seg->pair_gen,
                                              memory_order_relaxed);
        atomic_store_explicit(&e->seg->link_now[e->side], 0u,
                              memory_order_relaxed);
        fprintf(stdout, "psx_link_shm: PAIRED gen=%u (side %c epoch=%llu)\n",
                e->latched_gen, e->side ? 'B' : 'A',
                (unsigned long long)e->epoch);
        return;
    }

    /* PSX_DUAL_DIAG=1: one line per second -- link-time skew, barrier time,
     * ring occupancy. The counterpart of the fiber mode's [DUAL] line. */
    {
        static int diag = -1;
        static uint64_t last_ms;
        if (diag < 0) {
            const char *d = getenv("PSX_DUAL_DIAG");
            diag = (d && d[0] && d[0] != '0') ? 1 : 0;
        }
        if (diag) {
            uint64_t nowm = mono_ms();
            if (nowm - last_ms >= 1000u) {
                uint64_t mine = cycle_now - e->epoch;
                uint64_t peer = atomic_load_explicit(
                    &e->seg->link_now[e->side ^ 1], memory_order_relaxed);
                ShmRing *ri = in_ring(e);
                uint64_t occ = atomic_load(&ri->wr) - atomic_load(&ri->rd);
                fprintf(stderr,
                        "[SHMLINK] side=%c mine=%llu peer=%llu skew=%+lld "
                        "waits=%llums inq=%llu lines_peer=%u\n",
                        e->side ? 'B' : 'A',
                        (unsigned long long)mine, (unsigned long long)peer,
                        (long long)(mine - peer), (unsigned long long)e->waits_ms,
                        (unsigned long long)occ,
                        (unsigned)atomic_load_explicit(
                            &e->seg->out_lines[e->side ^ 1],
                            memory_order_relaxed));
                last_ms = nowm;
            }
        }
    }

    {
        uint64_t mine = cycle_now - e->epoch;
        uint64_t ahead_limit;
        atomic_store_explicit(&e->seg->link_now[e->side], mine,
                              memory_order_relaxed);
        ahead_limit = atomic_load_explicit(&e->seg->link_now[e->side ^ 1],
                                           memory_order_relaxed)
                      + e->seg->lookahead;
        if (mine <= ahead_limit) return;

        /* Ahead of the peer's window: block until it catches up. Short naps,
         * not futexes -- waits are a few hundred microseconds in steady state.
         * A dead peer unblocks as a disconnect. */
        {
            uint64_t t0 = mono_ms();
            for (;;) {
                uint64_t peer;
                shm_nap();
                peer = atomic_load_explicit(&e->seg->link_now[e->side ^ 1],
                                            memory_order_relaxed);
                if (mine <= peer + e->seg->lookahead) break;
                if (!peer_alive(e)) break;        /* cable unplugged */
                /* NO wall-clock escape while the peer is alive: a peer whose
                 * guest clock is stalled (disc load, pause menu, savestate
                 * dialog) is exactly when lockstep must WAIT -- an early
                 * give-up here let one side run 4M+ cycles past the bound.
                 * Peer death is the only exit besides catching up. */
                atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                                      memory_order_relaxed);
            }
            e->waits_ms += mono_ms() - t0;
        }
    }
}

void psx_link_shm_stats(uint64_t *my_link_now, uint64_t *peer_link_now,
                        uint32_t *paired, uint64_t *barrier_waits_ms) {
    ShmEnd *e = g_shm;
    if (!e) {
        if (my_link_now) *my_link_now = 0;
        if (peer_link_now) *peer_link_now = 0;
        if (paired) *paired = 0;
        if (barrier_waits_ms) *barrier_waits_ms = 0;
        return;
    }
    if (my_link_now)
        *my_link_now = atomic_load(&e->seg->link_now[e->side]);
    if (peer_link_now)
        *peer_link_now = atomic_load(&e->seg->link_now[e->side ^ 1]);
    if (paired) *paired = shm_paired(e);
    if (barrier_waits_ms) *barrier_waits_ms = e->waits_ms;
}

/* ===== PAIR mode ======================================================== */

int psx_link_shm_pair_init(const PsxLinkPairCfg *cfg) {
    ShmEnd *e = g_shm;
    if (!e || !cfg) return 0;
    if (cfg->latency_cycles < cfg->tick_len_cycles) {
        fprintf(stderr,
                "psx_link_shm: pair latency %u < tick %u breaks the "
                "determinism proof -- refusing\n",
                cfg->latency_cycles, cfg->tick_len_cycles);
        return 0;
    }
    e->seg->pair_cfg = *cfg;
    e->seg->latency = cfg->latency_cycles;
    atomic_store(&e->seg->done_tick, 0u);
    atomic_store(&e->seg->load_ack, 0u);
    atomic_store(&e->seg->follower_err, 0u);
    atomic_store(&e->seg->cmds.pushed, 0u);
    atomic_store(&e->seg->cmds.consumed, 0u);
    atomic_store_explicit(&e->seg->pair_mode, 1u, memory_order_release);
    fprintf(stdout,
            "psx_link_shm: PAIR mode (driver side %c, session %u, tick %u, "
            "latency %u, flags 0x%x)\n",
            cfg->driver_side ? 'B' : 'A', cfg->session_id,
            cfg->tick_len_cycles, cfg->latency_cycles, cfg->flags);
    return 1;
}

int psx_link_shm_pair_cfg(PsxLinkPairCfg *out) {
    ShmEnd *e = g_shm;
    if (!e || !out) return 0;
    if (!pair_mode(e)) return 0;
    *out = e->seg->pair_cfg;
    return 1;
}

int psx_link_shm_pair_mode(void) {
    ShmEnd *e = g_shm;
    return e ? pair_mode(e) : 0;
}

void psx_link_shm_anchor_epoch(uint64_t cycle_now) {
    ShmEnd *e = g_shm;
    if (!e) return;
    e->epoch = cycle_now;
    e->epoch_armed = 1;
    atomic_store_explicit(&e->seg->link_now[e->side], 0u, memory_order_relaxed);
    fprintf(stdout, "psx_link_shm: pair epoch anchored (side %c cycle=%llu)\n",
            e->side ? 'B' : 'A', (unsigned long long)cycle_now);
}

int psx_link_shm_cmd_push(const PsxLinkPairCmd *cmd) {
    ShmEnd *e = g_shm;
    ShmCmdRing *r;
    uint64_t pushed, consumed;
    if (!e || !cmd) return 0;
    r = &e->seg->cmds;
    pushed = atomic_load_explicit(&r->pushed, memory_order_relaxed);
    consumed = atomic_load_explicit(&r->consumed, memory_order_acquire);
    while (pushed - consumed >= SHM_CMD_DEPTH) {
        shm_nap();
        if (!peer_alive(e)) return 0;
        consumed = atomic_load_explicit(&r->consumed, memory_order_acquire);
    }
    r->cmd[pushed % SHM_CMD_DEPTH] = *cmd;
    atomic_store_explicit(&r->pushed, pushed + 1u, memory_order_release);
    atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                          memory_order_relaxed);
    return 1;
}

/* done_tick stores EXECUTED-COUNT (tick + 1), so 0 = nothing executed yet
 * and wait(tick) is exact for tick 0. Sessions cap far below u32 wrap. */
int psx_link_shm_wait_follower_tick(uint32_t tick) {
    ShmEnd *e = g_shm;
    if (!e) return 0;
    for (;;) {
        uint32_t done = atomic_load_explicit(&e->seg->done_tick,
                                             memory_order_acquire);
        if (done >= tick + 1u) return 1;
        if (atomic_load_explicit(&e->seg->follower_err,
                                 memory_order_relaxed) != 0)
            return 0;
        if (!peer_alive(e)) return 0;
        shm_nap();
        atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                              memory_order_relaxed);
    }
}

int psx_link_shm_wait_cmds_drained(void) {
    ShmEnd *e = g_shm;
    if (!e) return 0;
    for (;;) {
        uint64_t pushed = atomic_load_explicit(&e->seg->cmds.pushed,
                                               memory_order_relaxed);
        uint64_t consumed = atomic_load_explicit(&e->seg->cmds.consumed,
                                                 memory_order_acquire);
        if (consumed >= pushed) return 1;
        if (!peer_alive(e)) return 0;
        shm_nap();
        atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                              memory_order_relaxed);
    }
}

int psx_link_shm_cmd_wait_pop(PsxLinkPairCmd *out) {
    ShmEnd *e = g_shm;
    ShmCmdRing *r;
    uint64_t consumed, pushed;
    static uint32_t s_hb_div;
    if (!e || !out) return 0;
    r = &e->seg->cmds;
    consumed = atomic_load_explicit(&r->consumed, memory_order_relaxed);
    for (;;) {
        pushed = atomic_load_explicit(&r->pushed, memory_order_acquire);
        if (consumed < pushed) break;
        if (!peer_alive(e)) return 0;
        shm_nap();
        if ((++s_hb_div & 0xFu) == 0)
            atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                                  memory_order_relaxed);
    }
    *out = r->cmd[consumed % SHM_CMD_DEPTH];
    atomic_store_explicit(&r->consumed, consumed + 1u, memory_order_release);
    atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                          memory_order_relaxed);
    return 1;
}

/* LOAD rewind: after a snapshot restore the follower's executed-count moves
 * BACKWARD; a stale-high done_tick would let the driver run ahead of the
 * re-execution and break cable determinism during resim. */
void psx_link_shm_rewind_done_count(uint32_t executed_count) {
    ShmEnd *e = g_shm;
    if (!e) return;
    atomic_store_explicit(&e->seg->done_tick, executed_count,
                          memory_order_release);
}

void psx_link_shm_publish_digest(uint32_t tick, uint32_t core) {
    ShmEnd *e = g_shm;
    if (!e) return;
    e->seg->fol_dig[tick & 255u] = core;
}

int psx_link_shm_read_digest(uint32_t tick, uint32_t *out) {
    ShmEnd *e = g_shm;
    if (!e || !out) return 0;
    /* Caller must have observed done_tick >= tick+1 (acquire) first. */
    *out = e->seg->fol_dig[tick & 255u];
    return 1;
}

void psx_link_shm_set_done_tick(uint32_t tick) {
    ShmEnd *e = g_shm;
    if (!e) return;
    /* Executed-count encoding: see psx_link_shm_wait_follower_tick. */
    atomic_store_explicit(&e->seg->done_tick, tick + 1u, memory_order_release);
}

void psx_link_shm_load_ack_publish(void) {
    ShmEnd *e = g_shm;
    if (!e) return;
    atomic_fetch_add_explicit(&e->seg->load_ack, 1u, memory_order_release);
}

int psx_link_shm_wait_load_ack(uint32_t count) {
    ShmEnd *e = g_shm;
    if (!e) return 0;
    for (;;) {
        if (atomic_load_explicit(&e->seg->load_ack, memory_order_acquire) >=
            count)
            return 1;
        if (atomic_load_explicit(&e->seg->follower_err,
                                 memory_order_relaxed) != 0)
            return 0;
        if (!peer_alive(e)) return 0;
        shm_nap();
        atomic_store_explicit(&e->seg->heartbeat_ms[e->side], mono_ms(),
                              memory_order_relaxed);
    }
}

void psx_link_shm_set_follower_err(uint32_t code) {
    ShmEnd *e = g_shm;
    if (!e) return;
    atomic_store_explicit(&e->seg->follower_err, code, memory_order_release);
}

uint32_t psx_link_shm_follower_err(void) {
    ShmEnd *e = g_shm;
    return e ? atomic_load_explicit(&e->seg->follower_err,
                                    memory_order_relaxed)
             : 0u;
}

void psx_link_shm_log_cursors(uint64_t *in_read, uint64_t *in_write,
                              uint64_t *out_read, uint64_t *out_write) {
    ShmEnd *e = g_shm;
    if (!e) {
        if (in_read) *in_read = 0;
        if (in_write) *in_write = 0;
        if (out_read) *out_read = 0;
        if (out_write) *out_write = 0;
        return;
    }
    if (in_read)   *in_read = atomic_load(&in_ring(e)->rd);
    if (in_write)  *in_write = atomic_load(&in_ring(e)->wr);
    if (out_read)  *out_read = atomic_load(&out_ring(e)->rd);
    if (out_write) *out_write = atomic_load(&out_ring(e)->wr);
}
