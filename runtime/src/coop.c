/* coop.c — simultaneous extra player actors (ENHANCEMENT, default OFF).
 *
 * WHAT THIS IS
 * Some 2D action games run exactly one player actor, dispatched once per frame
 * through a state table. Given that shape, extra simultaneously-controllable
 * characters need only two things:
 *
 *   1. somewhere for each extra actor's state to live that the game can never
 *      overwrite  -> psx_enh_scratch_* (memory.c), a guest window above the
 *      main-RAM mirror. NOT a "free" hole inside main RAM: on MMX6 the
 *      documented free heap->stack gap turned out to be the CD-DMA stage-load
 *      arena and was overwritten wholesale on the first stage load.
 *   2. the per-frame player update to run once per actor, against each actor's
 *      own state -> the stage-replay hook below.
 *
 * HOW AN EXTRA ACTOR GETS ITS OWN STATE: CONTEXT SWAP
 * The actor's state stays at the address the game keeps it at. Before an extra
 * actor's pass, its saved bytes are swapped INTO the game's own actor storage;
 * after the pass, the evolved bytes are saved back and the game's own actor is
 * restored. The game therefore always finds its player exactly where it left
 * it, and every access reaches the right actor no matter how it is addressed —
 * through a pointer, through absolute addressing, through $gp, from a streamed
 * overlay the recompiler never saw, or through a pointer some global cached
 * frames ago.
 *
 * The rejected alternative was redirecting the pointer at each instruction that
 * forms it. That needs every such instruction enumerated per game, silently
 * misses every access that does not go through one (measured on MMX6: damage
 * landed on player 1 because one function formed the pointer somewhere the list
 * did not cover), cannot reach overlay code at all, and leaves any cached
 * pointer aimed at the wrong actor. Swapping is both smaller and total.
 *
 * PER-ACTOR STATE OUTSIDE THE STRUCT
 * Engines commonly keep some of a player's state outside its struct — an
 * animation object, a sprite handle, the processed pad word in a fixed global.
 * Any such region can be declared with psx_coop_add_swap_region() and is
 * swapped along with the actor struct, so extending the swap set never needs
 * new generated code. Which regions those are is a MEASUREMENT, not a guess:
 * every recorded write is tagged with the actor context that made it
 * (g_psx_enh_actor_ctx, consumed by the debug server's write rings), so
 * "written during an extra actor's pass but outside its own regions" names the
 * shared state directly.
 *
 * THE ONE GEN-TIME HOOK (emitted from [coop] in game.toml; omit those keys and
 * none of it is generated at all):
 *
 *   psx_coop_stage_link()  sits in the delay slot of each per-actor stage's
 *                          jal and rewrites the link so that stage replays once
 *                          per extra actor, then once for the game's own actor.
 *
 * plus psx_coop_companion_active(), which guards global side effects only the
 * game's own actor is entitled to cause (a write to a stage-fail flag, say —
 * swapping cannot help there, because the target is game state, not actor
 * state).
 *
 * Deliberately hooked at the LIVE caller only. Attract/demo modes replay
 * recorded inputs against simulation state, so an extra actor there desyncs the
 * recording; leaving the demo caller unhooked makes attract vanilla by
 * construction instead of relying on a runtime predicate.
 *
 * Everything here is inert unless a game calls psx_coop_configure().
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"

extern int      psx_enh_scratch_configure(uint32_t phys_base, uint32_t len);
extern uint8_t *psx_enh_scratch_ptr(uint32_t *out_base, uint32_t *out_len);
extern uint8_t *psx_guest_block_ptr(uint32_t addr, uint32_t len);
extern uint8_t  psx_read_byte(uint32_t addr);
extern uint16_t sio_get_pad_buttons_slot(int slot);

#define COOP_MAX_ACTORS  4
/* Sized for a whole alternate CHARACTER, not just an actor struct. On MMX6 the
 * complete X-vs-Zero difference is 830 KB spread over 84 disjoint runs (the
 * X-vs-X control differs by 866 bytes, so that is signal, not noise), and all
 * of it is DATA -- no overlay covers it and the dirty-RAM interpreter never
 * executes there. A host can memcpy that per pass without noticing; this is a
 * PC enhancement, and the PlayStation is the porting baseline, not the budget. */
#define COOP_MAX_REGIONS 128
#define COOP_MAX_REGION_LEN 0x00100000u

/* Actor context for the observability rings: 0 = the game's own actor,
 * N = extra actor N-1. Read by debug_server.c on every recorded write. */
unsigned char g_psx_enh_actor_ctx = 0;

typedef struct {
    uint32_t guest_base;   /* where the game keeps this state */
    uint32_t len;
} CoopRegion;

typedef struct {
    uint8_t *store;        /* this actor's saved copy of every region, packed */
    int      active;
} CoopActor;

static int        s_coop_enabled = 0;   /* master switch; 0 => every hook inert */
static CoopRegion s_region[COOP_MAX_REGIONS];
static int        s_region_count = 0;
static uint32_t   s_region_bytes = 0;   /* total bytes per actor */
static CoopActor  s_actors[COOP_MAX_ACTORS];
static int        s_actor_count  = 0;   /* extra actors only */
static int        s_current      = -1;  /* -1 => the game's own actor */
static uint8_t   *s_shadow       = NULL; /* the game's own actor, while swapped out */
static uint64_t   s_ticks        = 0;
static uint64_t   s_swaps        = 0;

/* Gate: "is it valid to run extra actors right now?" (in-scene, not paused, not
 * a scripted lock). Either a callback, or the declarative form below so a game
 * needs no framework code at all. NULL/unset => never run. */
static int (*s_gate)(void) = NULL;

static void coop_swap_out(void);

/* Declarative gate: one byte that must EQUAL a value (the game-mode/scene
 * state), plus up to two bytes that must be ZERO (pause, scripted lock).
 * Parameterised rather than game-specific: any title with a mode byte fits. */
#define COOP_MAX_ZERO_ADDRS 2
static uint32_t s_gate_mode_addr = 0;
static uint8_t  s_gate_mode_val  = 0;
static uint32_t s_gate_zero_addr[COOP_MAX_ZERO_ADDRS];
static int      s_gate_zero_count = 0;

void psx_coop_set_gate(int (*gate)(void)) { s_gate = gate; }

void psx_coop_set_gate_state(uint32_t mode_addr, uint8_t mode_val,
                             const uint32_t *zero_addrs, int zero_count)
{
    s_gate_mode_addr = mode_addr;
    s_gate_mode_val  = mode_val;
    s_gate_zero_count = 0;
    if (zero_addrs && zero_count > 0) {
        if (zero_count > COOP_MAX_ZERO_ADDRS) zero_count = COOP_MAX_ZERO_ADDRS;
        for (int i = 0; i < zero_count; i++) s_gate_zero_addr[i] = zero_addrs[i];
        s_gate_zero_count = zero_count;
    }
}

static int coop_gate_open(void)
{
    if (s_gate) return s_gate();
    if (!s_gate_mode_addr) return 0;          /* unconfigured => never */
    if (psx_read_byte(s_gate_mode_addr) != s_gate_mode_val) return 0;
    for (int i = 0; i < s_gate_zero_count; i++)
        if (psx_read_byte(s_gate_zero_addr[i]) != 0) return 0;
    return 1;
}

int psx_coop_gate_open(void) { return coop_gate_open(); }

/* True while an EXTRA actor (not the game's own) is the one being run. Guards
 * global side effects only the game's own actor is entitled to cause. Always
 * false when co-op is off, so guarded sites behave exactly as they always did. */
int psx_coop_companion_active(void) { return s_coop_enabled && s_current >= 0; }

/* ---- region table ---------------------------------------------------------
 * Region 0 is always the actor struct itself (installed by psx_coop_configure).
 * Extra regions are per-actor state the engine keeps elsewhere. */
int psx_coop_add_swap_region(uint32_t guest_base, uint32_t len)
{
    if (s_region_count >= COOP_MAX_REGIONS) return 0;
    if (len == 0 || len > COOP_MAX_REGION_LEN) return 0;
    if (!psx_guest_block_ptr(guest_base, len)) return 0;   /* must be real memory */
    if (s_coop_enabled) return 0;      /* layout is fixed once actors exist */
    /* Overlapping regions would be gathered and scattered twice, so the second
     * copy would silently win and the context would not round-trip. */
    for (int i = 0; i < s_region_count; i++) {
        const uint32_t b = s_region[i].guest_base, e = b + s_region[i].len;
        if (guest_base < e && b < (guest_base + len)) return 0;
    }
    s_region[s_region_count].guest_base = guest_base;
    s_region[s_region_count].len        = len;
    s_region_count++;
    s_region_bytes += len;
    return 1;
}

int psx_coop_swap_region_count(void) { return s_region_count; }

int psx_coop_swap_region(int i, uint32_t *base, uint32_t *len)
{
    if (i < 0 || i >= s_region_count) return 0;
    if (base) *base = s_region[i].guest_base;
    if (len)  *len  = s_region[i].len;
    return 1;
}

/* Configure. primary_base = the game's own actor struct; struct_len = its size;
 * companions = how many extra actors. Their saved contexts live in the
 * enhancement scratch window, which is allocated here. Any swap regions beyond
 * the struct must be added BEFORE this call (they change the per-actor size).
 * Returns 1 on success. Passing companions == 0 disables the feature. */
int psx_coop_configure(uint32_t primary_base, uint32_t struct_len,
                       int companions, uint32_t scratch_phys_base)
{
    /* Put the game's own actor back FIRST. Reconfiguring while an extra actor
     * is swapped in would free the scratch the shadow copy lives in and leave
     * the game running the extra actor's state as its player, permanently. */
    coop_swap_out();
    s_coop_enabled = 0;
    s_actor_count  = 0;
    s_current      = -1;
    s_shadow       = NULL;
    g_psx_enh_actor_ctx = 0;
    memset(s_actors, 0, sizeof(s_actors));

    if (companions <= 0) {
        s_region_count = 0; s_region_bytes = 0;
        psx_enh_scratch_configure(0, 0);
        return 1;
    }
    if (companions > COOP_MAX_ACTORS) return 0;
    if (struct_len == 0 || struct_len > COOP_MAX_REGION_LEN) return 0;

    /* Region 0 = the actor struct. Anything already declared follows it. */
    if (s_region_count == 0 || s_region[0].guest_base != primary_base ||
        s_region[0].len != struct_len) {
        CoopRegion extra[COOP_MAX_REGIONS];
        int n = 0;
        for (int i = 0; i < s_region_count; i++)
            if (s_region[i].guest_base != primary_base) extra[n++] = s_region[i];
        s_region[0].guest_base = primary_base;
        s_region[0].len        = struct_len;
        for (int i = 0; i < n && (i + 1) < COOP_MAX_REGIONS; i++)
            s_region[i + 1] = extra[i];
        s_region_count = (n + 1 > COOP_MAX_REGIONS) ? COOP_MAX_REGIONS : n + 1;
        s_region_bytes = 0;
        for (int i = 0; i < s_region_count; i++) s_region_bytes += s_region[i].len;
    }

    /* One packed context per actor, plus one shadow slot for the game's own
     * actor while an extra actor is swapped in. Rounded up for readable dumps. */
    const uint32_t slot = (s_region_bytes + 0xFFu) & ~0xFFu;
    const uint32_t need = slot * (uint32_t)(companions + 1);
    if (!psx_enh_scratch_configure(scratch_phys_base, need)) return 0;

    uint32_t sbase = 0, slen = 0;
    uint8_t *scratch = psx_enh_scratch_ptr(&sbase, &slen);
    if (!scratch) return 0;

    for (int i = 0; i < companions; i++) {
        s_actors[i].store  = scratch + slot * (uint32_t)i;
        s_actors[i].active = 0;
    }
    s_shadow       = scratch + slot * (uint32_t)companions;
    s_actor_count  = companions;
    s_coop_enabled = 1;
    /* No logging here by design (framework rule 3: no printf debugging, ever).
     * State is observable through the debug server's `coop` command. */
    return 1;
}

void psx_coop_reset(void)
{
    coop_swap_out();               /* never tear down mid-swap -- see configure */
    s_coop_enabled = 0;
    s_actor_count = 0;
    s_current = -1;
    s_region_count = 0;
    s_region_bytes = 0;
    s_shadow = NULL;
    g_psx_enh_actor_ctx = 0;
    psx_enh_scratch_configure(0, 0);
}

int psx_coop_enabled(void) { return s_coop_enabled; }

/* ---- the swap ------------------------------------------------------------ */

/* Copy every region between guest memory and a packed per-actor context. */
static void coop_gather(uint8_t *dst)
{
    uint32_t off = 0;
    for (int i = 0; i < s_region_count; i++) {
        uint8_t *p = psx_guest_block_ptr(s_region[i].guest_base, s_region[i].len);
        if (p) memcpy(dst + off, p, s_region[i].len);
        off += s_region[i].len;
    }
}

static void coop_scatter(const uint8_t *src)
{
    uint32_t off = 0;
    for (int i = 0; i < s_region_count; i++) {
        uint8_t *p = psx_guest_block_ptr(s_region[i].guest_base, s_region[i].len);
        if (p) memcpy(p, src + off, s_region[i].len);
        off += s_region[i].len;
    }
}

/* Swap extra actor `idx` in. The game's own actor is parked in the shadow slot
 * until coop_swap_out() puts it back. */
static void coop_swap_in(int idx)
{
    coop_gather(s_shadow);
    coop_scatter(s_actors[idx].store);
    s_current = idx;
    g_psx_enh_actor_ctx = (unsigned char)(idx + 1);
    s_swaps++;
}

/* Persist whatever the extra actor's pass produced, then restore the game's
 * own actor. Idempotent: safe to call when nothing is swapped in. */
static void coop_swap_out(void)
{
    if (s_current < 0) { g_psx_enh_actor_ctx = 0; return; }
    coop_gather(s_actors[s_current].store);
    coop_scatter(s_shadow);
    s_current = -1;
    g_psx_enh_actor_ctx = 0;
}

/* ---- portable context blobs -----------------------------------------------
 * Capture a set of guest regions to a file and re-install them later, into a
 * DIFFERENT run of the game.
 *
 * This is what makes "the extra actor is a different CHARACTER" tractable. On
 * MMX6 an alternate playable character is not a flag: setting the character
 * byte alone wedges the machine into an exception storm, because the character
 * is really the DATA SET the loader installs at stage entry. But that data set
 * is exactly measurable -- run the same stage as each character and diff --
 * and on MMX6 it is 830 KB of pure data. So capture it once from a run of the
 * character you want, and re-install it whenever you want that character.
 *
 * Deliberately independent of the actor machinery above: the same facility
 * proves the blob by installing it over the LIVE player (does X become Zero?)
 * before any of it is wired into a second actor.
 *
 * The region list travels IN the file, so a load does not depend on the caller
 * having declared the same layout that the save used.
 */
#define CTX_MAGIC   0x58544350u        /* "PCTX" */
#define CTX_VERSION 1u

static CoopRegion s_ctx_region[COOP_MAX_REGIONS];
static int        s_ctx_region_count = 0;

void psx_ctx_region_clear(void) { s_ctx_region_count = 0; }
int  psx_ctx_region_count(void) { return s_ctx_region_count; }

int psx_ctx_region_add(uint32_t base, uint32_t len)
{
    if (s_ctx_region_count >= COOP_MAX_REGIONS) return 0;
    if (len == 0 || len > COOP_MAX_REGION_LEN) return 0;
    if (!psx_guest_block_ptr(base, len)) return 0;
    s_ctx_region[s_ctx_region_count].guest_base = base;
    s_ctx_region[s_ctx_region_count].len        = len;
    s_ctx_region_count++;
    return 1;
}

/* Write every declared region to `path`. Returns bytes written, or 0. */
uint32_t psx_ctx_save(const char *path)
{
    if (s_ctx_region_count <= 0) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t hdr[2] = { CTX_MAGIC, CTX_VERSION };
    uint32_t n = (uint32_t)s_ctx_region_count;
    uint32_t total = 0;
    if (fwrite(hdr, 4, 2, f) != 2 || fwrite(&n, 4, 1, f) != 1) { fclose(f); return 0; }
    for (int i = 0; i < s_ctx_region_count; i++) {
        if (fwrite(&s_ctx_region[i].guest_base, 4, 1, f) != 1 ||
            fwrite(&s_ctx_region[i].len, 4, 1, f) != 1) { fclose(f); return 0; }
    }
    for (int i = 0; i < s_ctx_region_count; i++) {
        const uint8_t *p = psx_guest_block_ptr(s_ctx_region[i].guest_base,
                                               s_ctx_region[i].len);
        if (!p) { fclose(f); return 0; }
        if (fwrite(p, 1, s_ctx_region[i].len, f) != s_ctx_region[i].len) {
            fclose(f); return 0;
        }
        total += s_ctx_region[i].len;
    }
    fclose(f);
    return total;
}

/* Read a blob and scatter it. dst == NULL installs into LIVE guest memory;
 * otherwise the bytes are packed into `dst` in the file's region order (used to
 * seed an actor's saved context). Returns bytes installed, or 0.
 * When seeding an actor, the file's layout must match that actor's swap-region
 * layout exactly -- otherwise the packed offsets would not line up and the
 * actor would be assembled from the wrong bytes. That is checked, not assumed. */
uint32_t psx_ctx_load(const char *path, uint8_t *dst)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint32_t hdr[2] = {0, 0}, n = 0;
    if (fread(hdr, 4, 2, f) != 2 || hdr[0] != CTX_MAGIC || hdr[1] != CTX_VERSION ||
        fread(&n, 4, 1, f) != 1 || n == 0 || n > COOP_MAX_REGIONS) {
        fclose(f); return 0;
    }
    CoopRegion r[COOP_MAX_REGIONS];
    for (uint32_t i = 0; i < n; i++) {
        if (fread(&r[i].guest_base, 4, 1, f) != 1 ||
            fread(&r[i].len, 4, 1, f) != 1 ||
            r[i].len == 0 || r[i].len > COOP_MAX_REGION_LEN) { fclose(f); return 0; }
    }
    if (dst) {
        if ((int)n != s_region_count) { fclose(f); return 0; }
        for (uint32_t i = 0; i < n; i++)
            if (r[i].guest_base != s_region[i].guest_base ||
                r[i].len != s_region[i].len) { fclose(f); return 0; }
    }
    uint32_t total = 0, off = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = dst ? (dst + off)
                         : psx_guest_block_ptr(r[i].guest_base, r[i].len);
        if (!p) { fclose(f); return 0; }
        if (fread(p, 1, r[i].len, f) != r[i].len) { fclose(f); return 0; }
        off   += r[i].len;
        total += r[i].len;
    }
    fclose(f);
    return total;
}

/* Seed an extra actor's saved context from a blob instead of cloning the game's
 * own actor -- i.e. make that actor a different character. */
int psx_coop_seed_from_blob(int idx, const char *path)
{
    if (!s_coop_enabled || idx < 0 || idx >= s_actor_count) return 0;
    if (s_current >= 0) return 0;            /* never seed a swapped-in context */
    if (!psx_ctx_load(path, s_actors[idx].store)) return 0;
    s_actors[idx].active = 1;
    return 1;
}

/* ---- observation ----------------------------------------------------------
 * Resolve a guest address to the bytes that BELONG to its owner, whichever
 * actor happens to be swapped in at the instant a tool asks.
 *
 * Without this, every tool reading the actor struct races the swap: the debug
 * server runs on its own thread, an extra actor is swapped in for a large part
 * of each frame, and a read of the game's player lands on whichever actor is
 * currently there. That does not merely add noise -- it manufactures false
 * conclusions ("both players moved together", "the extra actor teleported"),
 * which is worse than having no reading at all.
 *
 * Returns a host pointer for `len` bytes, or NULL when the ordinary memory path
 * is already correct (co-op off, or nothing swapped in). */
uint8_t *psx_coop_observe_ptr(uint32_t addr, uint32_t len)
{
    if (!s_coop_enabled || s_current < 0 || !s_shadow) return NULL;

    /* An address in a swap region names the GAME'S OWN actor, which is parked
     * in the shadow while an extra actor is swapped in. */
    uint32_t off = 0;
    for (int i = 0; i < s_region_count; i++) {
        const uint32_t base = s_region[i].guest_base, rl = s_region[i].len;
        if (addr >= base && (addr + len) <= (base + rl))
            return s_shadow + off + (addr - base);
        off += rl;
    }

    /* An address in the swapped-in actor's saved context names bytes that are
     * live at the game's own addresses right now. */
    uint32_t sbase = 0, slen = 0;
    uint8_t *scratch = psx_enh_scratch_ptr(&sbase, &slen);
    if (!scratch) return NULL;
    uint8_t *store = s_actors[s_current].store;
    const uint32_t sguest = 0x80000000u | (sbase + (uint32_t)(store - scratch));
    if (addr < sguest || (addr - sguest) >= s_region_bytes) return NULL;
    uint32_t d = addr - sguest;
    off = 0;
    for (int i = 0; i < s_region_count; i++) {
        if (d < off + s_region[i].len) {
            const uint32_t inner = d - off;
            if (inner + len > s_region[i].len) return NULL;   /* straddles */
            return psx_guest_block_ptr(s_region[i].guest_base + inner, len);
        }
        off += s_region[i].len;
    }
    return NULL;
}

/* Seed an extra actor by cloning the game's own context, so it starts as a
 * valid actor rather than zeroes. Zeroes would be actively dangerous: a state
 * index read with a SIGNED load would index backwards off a dispatch table.
 * Cloning a known-good live actor avoids inventing any field values. */
int psx_coop_spawn(int idx)
{
    if (!s_coop_enabled || idx < 0 || idx >= s_actor_count) return 0;
    if (s_current >= 0) return 0;          /* never clone a swapped-in context */
    coop_gather(s_actors[idx].store);
    s_actors[idx].active = 1;
    return 1;
}

void psx_coop_despawn(int idx)
{
    if (idx >= 0 && idx < s_actor_count) s_actors[idx].active = 0;
}

int psx_coop_actor_active(int idx)
{
    return (idx >= 0 && idx < s_actor_count) ? s_actors[idx].active : 0;
}

/* Guest address of an extra actor's saved struct (region 0 inside its packed
 * context), so tools can read and poke it exactly like main RAM. */
uint32_t psx_coop_actor_guest_base(int idx)
{
    uint32_t sbase = 0, slen = 0;
    uint8_t *scratch = psx_enh_scratch_ptr(&sbase, &slen);
    if (!scratch || idx < 0 || idx >= s_actor_count || !s_actors[idx].store) return 0;
    return 0x80000000u | (sbase + (uint32_t)(s_actors[idx].store - scratch));
}

void psx_coop_stats(uint64_t *ticks, uint64_t *swaps)
{
    if (ticks) *ticks = s_ticks;
    if (swaps) *swaps = s_swaps;
}

/* ---- input ---------------------------------------------------------------
 * Feed the actor currently swapped in its own controller.
 *
 * The engine keeps the processed pad word inside the actor struct, so writing
 * it there — at the address the game reads, because the extra actor's context
 * is swapped in — is enough for every read, including ones that never go
 * through the actor pointer.
 *
 * Two conversions matter:
 *  - SIO reports ACTIVE-LOW (0 = pressed); the engine stores ACTIVE-HIGH. That
 *    is not a guess: the demo replay computes its newly-pressed mask as
 *    `cur & (cur ^ prev)`, which only means "newly pressed" for active-high.
 *  - The engine keeps that edge mask in the next field, so maintain it here per
 *    actor rather than letting it go stale.
 *
 * Offsets are supplied by the game (psx_coop_set_input_layout); zero disables
 * the feed entirely, keeping this inert for titles that store input elsewhere.
 *
 * An engine that ALSO reads the pad word from a fixed global is handled by
 * declaring that global as a swap region — not by a bespoke swap here. That
 * was the old mechanism and it was a hazard: it wrote player 1's global behind
 * its back and left it reading zero while a direction was held. */
static uint32_t s_in_cur_off  = 0;
static uint32_t s_in_edge_off = 0;
static int      s_in_enabled  = 0;
static uint16_t s_prev_pad[COOP_MAX_ACTORS];

/* RAW pad delivery -- strongly preferred over the processed-word feed above.
 *
 * `raw_addr` is where the game's own controller poll leaves port 1's RAW
 * (ACTIVE-LOW) button halfword. Writing the extra actor's raw pad there and
 * REPLAYING the engine's own pad-processing stage makes the engine derive that
 * actor's processed input itself -- held mask, edge mask, whatever else it
 * keeps -- with no reimplementation here.
 *
 * That matters more than it sounds. MMX6 does not store the pad in SIO bit
 * order: it remaps every button (SIO 0x0080 LEFT -> 0x0002, SIO 0x4000 CROSS ->
 * 0x0080, ...) and derives three fields, not one. Feeding the processed word
 * from outside means transcribing that table and re-deriving those fields, and
 * getting it subtly wrong is invisible -- an extra actor fed SIO-order bits
 * reads "LEFT" as "JUMP" and simply behaves oddly rather than failing.
 *
 * The address must lie inside a declared swap region, so the game's own port-1
 * value is saved and restored around the pass rather than being clobbered. That
 * is enforced, not documented: an earlier attempt at this wrote a shared input
 * global behind player 1's back and left player 1 reading zero while a
 * direction was held. */
static uint32_t s_raw_pad_addr   = 0;
static uint32_t s_raw_pad_words  = 1;   /* mirrored halfwords at that address */
static int      s_raw_pad_wire   = 0;   /* 1 = pad WIRE order, 0 = SIO word */

void psx_coop_set_input_layout(uint32_t cur_off, uint32_t edge_off, int enabled)
{
    s_in_cur_off  = cur_off;
    s_in_edge_off = edge_off;
    s_in_enabled  = enabled ? 1 : 0;
    for (int i = 0; i < COOP_MAX_ACTORS; i++) s_prev_pad[i] = 0;
}

/* `words` consecutive halfwords at `addr` all receive the actor's pad. Engines
 * commonly keep more than one copy (MMX6 keeps two, both "currently held"), and
 * updating only the first leaves the rest stale from whichever actor wrote them
 * last.
 *
 * `wire` selects the byte order the engine's globals are in:
 *   0 - the SIO word as this runtime reports it (ACTIVE-LOW).
 *   1 - PAD WIRE ORDER: the controller's two button bytes in transfer order,
 *       held-active-high, i.e. byteswap(~sio). This is what a game gets by
 *       reading its pad buffer's two button bytes as a big-endian halfword, so
 *       it is a property of the CONSOLE's pad protocol, not of any one game.
 *       Confirmed on MMX6 for all 14 buttons against its 0x800C456C globals.
 *
 * Returns 1 if accepted. addr == 0 disables raw delivery. */
int psx_coop_set_raw_pad(uint32_t addr, uint32_t words, int wire)
{
    if (addr == 0) { s_raw_pad_addr = 0; return 1; }
    if (words < 1 || words > 8) return 0;
    int covered = 0;
    for (int i = 0; i < s_region_count; i++) {
        if (addr >= s_region[i].guest_base &&
            (addr + 2u * words) <= (s_region[i].guest_base + s_region[i].len)) { covered = 1; break; }
    }
    if (!covered) return 0;
    s_raw_pad_addr  = addr;
    s_raw_pad_words = words;
    s_raw_pad_wire  = wire ? 1 : 0;
    return 1;
}

uint32_t psx_coop_raw_pad_addr(void) { return s_raw_pad_addr; }
uint32_t psx_coop_raw_pad_words(void) { return s_raw_pad_words; }
int      psx_coop_raw_pad_wire(void) { return s_raw_pad_wire; }

static void coop_write16_live(uint32_t addr, uint16_t v)
{
    uint8_t *p = psx_guest_block_ptr(addr, 2);
    if (!p) return;
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void coop_feed_input(int idx)
{
    /* Extra actor N takes SIO slot N+1: the game's own player keeps port 1. */
    const int slot = idx + 1;
    if (slot > 1) return;                     /* only two physical ports exist */
    const uint16_t raw = sio_get_pad_buttons_slot(slot);   /* ACTIVE-LOW */

    if (s_raw_pad_addr) {
        /* Preferred path: hand the engine this actor's raw pad and let its own
         * replayed pad stage decode it. */
        uint16_t v = raw;
        if (s_raw_pad_wire) {
            const uint16_t held = (uint16_t)(~raw & 0xFFFFu);
            v = (uint16_t)((held << 8) | (held >> 8));
        }
        for (uint32_t w = 0; w < s_raw_pad_words; w++)
            coop_write16_live(s_raw_pad_addr + 2u * w, v);
        return;
    }
    if (!s_in_enabled) return;

    /* Fallback for engines whose pad stage cannot be replayed: write the
     * processed word straight into the actor. SIO reports ACTIVE-LOW (0 =
     * pressed) and engines store ACTIVE-HIGH; the edge mask is maintained here
     * because the engine's own derivation is being skipped. This does NOT know
     * about any per-game button remap -- prefer psx_coop_set_raw_pad. */
    const uint16_t active_high = (uint16_t)(~raw & 0xFFFFu);
    const uint16_t edge = (uint16_t)(active_high & (active_high ^ s_prev_pad[idx]));
    s_prev_pad[idx] = active_high;

    /* The actor is swapped in, so its input goes where the game looks. */
    const uint32_t base = s_region[0].guest_base;
    coop_write16_live(base + s_in_cur_off, active_high);
    if (s_in_edge_off) coop_write16_live(base + s_in_edge_off, edge);
}

/* ---- gen-time hook: per-actor stage replay --------------------------------
 * Runs in the delay slot of ONE per-actor stage's jal, after the CPS emitter
 * has stored the natural return address. Returns the link that stage's callee
 * should come back to:
 *
 *   - an extra actor still owes this stage a pass -> swap it in and return
 *     `jal_addr`, so control lands back on the same jal and the stage repeats
 *     for that actor;
 *   - list exhausted -> swap back to the game's own actor and return the
 *     natural link, so it gets the final pass and the frame moves on.
 *
 * The game's own actor takes the LAST pass deliberately: anything a stage
 * leaves in shared state is then left by the actor the rest of the frame's
 * shared stages are about, which is the vanilla value.
 *
 * Each declared stage keeps its OWN cursor, keyed by jal address: stages are
 * independent, and a shared cursor would let one stage's progress silently
 * skip another's passes. Cursors are found linearly over a tiny table — there
 * are a handful of stages, and this runs once per stage per pass.
 *
 * Why per-stage rather than one contiguous span: the frame pipeline interleaves
 * per-actor and shared stages, and replaying a span that included a shared
 * stage ran it twice per frame and corrupted the game (duplicated spawn sprites,
 * physics stepping twice, the player dropping through the floor). Only stages
 * proven per-actor may be declared.
 *
 * Why not call the stage from native code: the recompiled bodies are CPS, so a
 * native psx_dispatch_call does not return to its caller — any restore step is
 * skipped and the actor selection latches on. Here every extra pass is an
 * ordinary guest return and the selection is recomputed on each entry.
 */
#define COOP_MAX_STAGES 16
static uint32_t s_stage_addr[COOP_MAX_STAGES];
static int      s_stage_cursor[COOP_MAX_STAGES];
static int      s_stage_count = 0;

static int coop_stage_slot(uint32_t jal_addr)
{
    for (int i = 0; i < s_stage_count; i++)
        if (s_stage_addr[i] == jal_addr) return i;
    if (s_stage_count >= COOP_MAX_STAGES) return -1;
    const int i = s_stage_count++;
    s_stage_addr[i] = jal_addr;
    s_stage_cursor[i] = -1;
    return i;
}

uint32_t psx_coop_stage_link(CPUState* cpu, uint32_t natural_link, uint32_t jal_addr)
{
    if (!s_coop_enabled || s_actor_count == 0) { coop_swap_out(); return natural_link; }
    if (!coop_gate_open())                     { coop_swap_out(); return natural_link; }

    const int slot = coop_stage_slot(jal_addr);
    if (slot < 0) { coop_swap_out(); return natural_link; }

    const int from = s_stage_cursor[slot];
    coop_swap_out();              /* persist the pass that just finished */

    for (int i = from + 1; i < s_actor_count; i++) {
        if (!s_actors[i].active) continue;
        s_stage_cursor[slot] = i;
        s_ticks++;
        coop_swap_in(i);
        coop_feed_input(i);       /* the actor reads its own pad */
        /* Stages that take the actor POINTER AS AN ARGUMENT (sprite submission,
         * for example) receive it in a0 from the pipeline driver, which runs
         * only once per frame. With the context swapped that pointer is already
         * correct, but re-point a0 anyway so a stage cannot be handed a pointer
         * the pipeline computed for a different purpose. Stages that build the
         * pointer internally overwrite a0 themselves, so this is harmless. */
        if (cpu) cpu->gpr[4] = s_region[0].guest_base;
        return jal_addr;          /* replay this stage for that actor */
    }
    s_stage_cursor[slot] = -1;    /* round complete for this stage */
    return natural_link;          /* the game's own actor gets the final pass */
}
