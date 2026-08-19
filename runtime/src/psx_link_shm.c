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
#define SHM_VERSION 1u
#define SHM_DEPTH   64u           /* chars per direction, like PSX_LINK_RING_DEPTH */

/* SPSC ring: tail is written only by the producer (peer), head only by the
 * consumer (us). due[] is ordered by the release-store of tail. */
typedef struct ShmRing {
    _Atomic uint32_t tail;
    _Atomic uint32_t head;
    uint8_t          byte[SHM_DEPTH];
    uint64_t         due[SHM_DEPTH];       /* link-time cycles */
} ShmRing;

typedef struct ShmSeg {
    uint32_t         magic;
    uint32_t         version;
    _Atomic uint32_t present[2];
    _Atomic uint32_t paired;
    _Atomic uint64_t link_now[2];          /* published link-time per side */
    _Atomic uint64_t heartbeat_ms[2];      /* CLOCK_MONOTONIC ms, liveness only */
    _Atomic uint32_t out_lines[2];         /* my DTR/RTS -> peer DSR/CTS */
    uint32_t         latency;              /* wire delay, cycles */
    uint32_t         lookahead;            /* barrier bound, cycles */
    ShmRing          ring[2];              /* [0] = A->B, [1] = B->A */
} ShmSeg;

/* ===== process-side state =============================================== */

typedef struct ShmEnd {
    ShmSeg  *seg;
    int      side;                 /* 0 = A, 1 = B */
    int      epoch_latched;
    uint64_t epoch;                /* local cycle at pairing */
    uint64_t waits_ms;             /* total barrier block time (diag) */
    PsxLinkEndpoint ep;
#if defined(_WIN32)
    HANDLE   map;
#else
    char     shm_name[128];
#endif
} ShmEnd;

static ShmEnd *g_shm;              /* one link per process */

static uint64_t mono_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static uint64_t link_now(const ShmEnd *e, uint64_t cycle_now) {
    return e->epoch_latched ? (cycle_now - e->epoch) : 0u;
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
    uint32_t tail, head, slot;
    if (!shm_paired(e) || !e->epoch_latched) return 0;   /* no cable yet */
    tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail - head >= SHM_DEPTH) return 0;              /* wire overflow: drop */
    slot = tail % SHM_DEPTH;
    r->byte[slot] = b;
    r->due[slot]  = link_now(e, c) + e->seg->latency;
    atomic_store_explicit(&r->tail, tail + 1u, memory_order_release);
    return 1;
}

static int shm_rx(void *s, uint8_t *o, uint64_t c) {
    ShmEnd *e = (ShmEnd *)s;
    ShmRing *r = in_ring(e);
    uint32_t head, tail, slot;
    if (!e->epoch_latched) return 0;
    head = atomic_load_explicit(&r->head, memory_order_relaxed);
    tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head == tail) return 0;
    slot = head % SHM_DEPTH;
    if (r->due[slot] > link_now(e, c)) return 0;
    *o = r->byte[slot];
    atomic_store_explicit(&r->head, head + 1u, memory_order_release);
    return 1;
}

static int shm_rx_peek(void *s, uint64_t *d) {
    ShmEnd *e = (ShmEnd *)s;
    ShmRing *r = in_ring(e);
    uint32_t head, tail;
    if (!e->epoch_latched) return 0;
    head = atomic_load_explicit(&r->head, memory_order_relaxed);
    tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head == tail) return 0;
    /* Translate the stored link-time due back to the CALLER's local cycle
     * timeline: due_local = due_link + epoch. sio1 compares against local. */
    *d = r->due[head % SHM_DEPTH] + e->epoch;
    return 1;
}

static void shm_set_lines(void *s, uint32_t l) {
    ShmEnd *e = (ShmEnd *)s;
    atomic_store_explicit(&e->seg->out_lines[e->side], l, memory_order_relaxed);
}

static uint32_t shm_get_lines(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    if (!shm_paired(e) || !peer_alive(e)) return 0;      /* cable out: DSR low */
    return atomic_load_explicit(&e->seg->out_lines[e->side ^ 1],
                                memory_order_relaxed);
}

static int shm_connected(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    return shm_paired(e) && peer_alive(e);
}

static void shm_reset(void *s) {
    ShmEnd *e = (ShmEnd *)s;
    ShmRing *r = out_ring(e);
    /* Drop own in-flight chars + own lines, like the crossover. The ring is
     * drained producer-side: advance tail is peer-owned reads -- instead mark
     * every queued entry immediately-due-and-consumed by moving OUR view: we
     * cannot touch head (consumer-owned), so just leave queued bytes; a block
     * reset mid-handshake retransmits anyway. Lines drop for real. */
    (void)r;
    atomic_store_explicit(&e->seg->out_lines[e->side], 0, memory_order_relaxed);
}

/* Savestate during shm link play: serialize as an EMPTY inbound ring + own
 * lines (crossover wire format). A char in flight at save time is lost --
 * link handshakes retransmit; a full cross-process co-checkpoint is future
 * work and needs both processes to agree on the save cycle anyway. */
static uint32_t shm_snap_bytes(void *s) { (void)s; return 4u + 4u; }
static void shm_snap_write(void *s, uint8_t *p, uint64_t now) {
    ShmEnd *e = (ShmEnd *)s;
    uint32_t lines = atomic_load_explicit(&e->seg->out_lines[e->side],
                                          memory_order_relaxed);
    (void)now;
    p[0] = (uint8_t)lines; p[1] = (uint8_t)(lines >> 8);
    p[2] = (uint8_t)(lines >> 16); p[3] = (uint8_t)(lines >> 24);
    p[4] = p[5] = p[6] = p[7] = 0;                        /* ring count = 0 */
}
static int shm_snap_read(void *s, const uint8_t *p, uint32_t len, uint64_t now) {
    ShmEnd *e = (ShmEnd *)s;
    uint32_t lines;
    (void)now;
    if (len < 8u) return 0;
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
         * the nap dominates. A quarter-frame bound means one potential nap
         * per ~7.7 ms of guest: >95% duty. Bytes then arrive up to one bound
         * LATE on the receiver's clock, which is exactly how the fiber dual
         * behaved at its coarse 282240-cycle slice -- and the libcomb
         * handshake and race traffic tolerated that, measured. Wire latency
         * stays at the fiber-proven small value; late-but-ordered is the
         * contract, not early. */
        /* One PAL frame plus margin. Each side's wall-clock pacer freezes
         * its guest clock for up to ~8 ms every frame; a bound below one
         * frame turns that ordinary idle into peer stalls (measured 0.57x at
         * a quarter frame). At one frame the barrier only fires on REAL
         * divergence -- content dips, loads -- where lockstep is supposed to
         * wait. Pair speed is min(A, B) by design. */
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

    /* Claim a side. */
    if (role == 'a') side = 0;
    else if (role == 'b') side = 1;
    else side = created ? 0 : 1;
    {
        uint32_t expect = 0;
        if (!atomic_compare_exchange_strong(&seg->present[side], &expect, 1u)) {
            int other = side ^ 1;
            expect = 0;
            if (role != 0 ||        /* explicit role taken: fail loudly */
                !atomic_compare_exchange_strong(&seg->present[other], &expect, 1u)) {
                fprintf(stderr, "psx_link_shm: side %c of %s already claimed\n",
                        side ? 'B' : 'A', name);
#if defined(_WIN32)
                UnmapViewOfFile(seg); CloseHandle(map);
#else
                munmap(seg, sizeof(ShmSeg));
#endif
                return NULL;
            }
            side = other;
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

    if (!e->epoch_latched) {
        /* Pairing: latch the epoch the first time both sides are present.
         * Both sides latch within one poll of each other -- microseconds of
         * skew against a multi-thousand-cycle lookahead. */
        if (atomic_load_explicit(&e->seg->present[0], memory_order_relaxed) &&
            atomic_load_explicit(&e->seg->present[1], memory_order_relaxed)) {
            atomic_store_explicit(&e->seg->paired, 1u, memory_order_release);
            e->epoch = cycle_now;
            e->epoch_latched = 1;
            atomic_store_explicit(&e->seg->link_now[e->side], 0u,
                                  memory_order_relaxed);
            fprintf(stdout, "psx_link_shm: PAIRED (side %c epoch=%llu)\n",
                    e->side ? 'B' : 'A', (unsigned long long)e->epoch);
        }
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
                uint32_t occ = atomic_load(&ri->tail) - atomic_load(&ri->head);
                fprintf(stderr,
                        "[SHMLINK] side=%c mine=%llu peer=%llu skew=%+lld "
                        "waits=%llums inq=%u lines_peer=%u\n",
                        e->side ? 'B' : 'A',
                        (unsigned long long)mine, (unsigned long long)peer,
                        (long long)(mine - peer), (unsigned long long)e->waits_ms,
                        occ,
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
            struct timespec nap = { 0, 100000 };   /* 100 us */
            for (;;) {
                uint64_t peer;
#if defined(_WIN32)
                Sleep(0);
#else
                nanosleep(&nap, NULL);
#endif
                peer = atomic_load_explicit(&e->seg->link_now[e->side ^ 1],
                                            memory_order_relaxed);
                if (mine <= peer + e->seg->lookahead) break;
                if (!peer_alive(e)) break;        /* cable unplugged */
                if ((mono_ms() - t0) > SHM_DEAD_MS) break;
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
