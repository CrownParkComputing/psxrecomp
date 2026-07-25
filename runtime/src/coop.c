/* coop.c — simultaneous extra player actors (ENHANCEMENT, default OFF).
 *
 * WHAT THIS IS
 * Some 2D action games run exactly one player actor, dispatched once per frame
 * through a state table, with the actor pointer formed by a single instruction.
 * Given that shape, a second simultaneously-controllable character needs only
 * two things:
 *
 * The per-frame player update is treated as a SET OF PER-ACTOR STAGES inside
 * the frame pipeline, each replayed once per actor -- see config_loader.h. It is
 * deliberately NOT a contiguous span: the pipeline interleaves per-actor and
 * shared stages, and replaying a shared stage corrupts the game.
 *
 *   1. somewhere for the extra actor's struct to live that the game can never
 *      overwrite  -> psx_enh_scratch_* (memory.c), a guest window above the
 *      main-RAM mirror. NOT a "free" hole inside main RAM: on MMX6 the
 *      documented free heap->stack gap turned out to be the CD-DMA stage-load
 *      arena and was overwritten wholesale on the first stage load.
 *   2. the per-frame dispatch to run once per actor, against each actor's own
 *      struct -> the two gen-time hooks below.
 *
 * HOW THE HOOKS FIT (both emitted by the recompiler from [coop] in game.toml;
 * omit those keys and none of this is generated at all):
 *
 *   psx_coop_actor_base()      wraps the one instruction that forms the actor
 *                              pointer. Returns the struct for whichever actor
 *                              is currently being ticked. IDENTITY when off.
 *   psx_coop_stage_link()      sits in the delay slot of each per-actor stage's
 *                              jal and rewrites the link so that stage replays
 *                              once per companion, then once for player 1.
 *
 * Deliberately hooked at the LIVE caller only. Attract/demo modes replay
 * recorded inputs against simulation state, so an extra actor there desyncs the
 * recording; leaving the demo caller unhooked makes attract vanilla by
 * construction instead of relying on a runtime predicate.
 *
 * Everything here is inert unless a game calls psx_coop_configure().
 */
#include <stdint.h>
#include <string.h>

#include "cpu_state.h"

extern int      psx_enh_scratch_configure(uint32_t phys_base, uint32_t len);
extern uint8_t *psx_enh_scratch_ptr(uint32_t *out_base, uint32_t *out_len);
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_byte(uint32_t addr, uint8_t val);
extern uint8_t  psx_read_byte(uint32_t addr);
extern uint16_t sio_get_pad_buttons_slot(int slot);

#define COOP_MAX_ACTORS 4

typedef struct {
    uint32_t guest_base;   /* guest address of this actor's struct */
    int      active;
} CoopActor;

static int      s_coop_enabled   = 0;   /* master switch; 0 => every hook inert */
static uint32_t s_primary_base   = 0;   /* the game's own (vanilla) actor struct */
static uint32_t s_struct_len     = 0;
static CoopActor s_actors[COOP_MAX_ACTORS];
static int      s_actor_count    = 0;   /* companions only (excludes primary) */
static int      s_current        = -1;  /* -1 => primary, else companion index */
static uint64_t s_ticks          = 0;
static uint64_t s_redirects      = 0;

/* Gate: "is it valid to run companions right now?" (in-stage, not paused, not a
 * scripted lock, not a demo). Either a callback, or the declarative form below
 * so a game needs no framework code at all. NULL/unset => never run. */
static int (*s_gate)(void) = NULL;

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

/* True while a COMPANION (not player 1) is the actor currently being run.
 * Guards global side effects that only player 1 is entitled to cause -- see the
 * suppress_sites hook. Always false when co-op is off, so guarded sites behave
 * exactly as they always did. */
int psx_coop_companion_active(void) { return s_coop_enabled && s_current >= 0; }

/* Configure. primary_base = the game's vanilla actor struct; struct_len = its
 * size; companions = how many extra actors. Their structs are carved out of the
 * enhancement scratch window, which is allocated here.
 * Returns 1 on success. Passing companions == 0 disables the feature. */
int psx_coop_configure(uint32_t primary_base, uint32_t struct_len,
                       int companions, uint32_t scratch_phys_base)
{
    s_coop_enabled = 0;
    s_actor_count  = 0;
    s_current      = -1;
    memset(s_actors, 0, sizeof(s_actors));

    if (companions <= 0) { psx_enh_scratch_configure(0, 0); return 1; }
    if (companions > COOP_MAX_ACTORS) return 0;
    if (struct_len == 0 || struct_len > 0x4000u) return 0;

    /* One page-ish slot per companion, rounded up for readability in dumps. */
    const uint32_t slot = (struct_len + 0xFFu) & ~0xFFu;
    const uint32_t need = slot * (uint32_t)companions;
    if (!psx_enh_scratch_configure(scratch_phys_base, need)) return 0;

    for (int i = 0; i < companions; i++) {
        s_actors[i].guest_base = 0x80000000u | (scratch_phys_base + slot * (uint32_t)i);
        s_actors[i].active = 0;
    }
    s_primary_base = primary_base;
    s_struct_len   = struct_len;
    s_actor_count  = companions;
    s_coop_enabled = 1;
    /* No logging here by design (framework rule 3: no printf debugging, ever).
     * State is observable through the debug server's `coop` command, which
     * reports enabled/gate/actor bases and the tick + redirect counters. */
    return 1;
}

void psx_coop_reset(void)
{
    s_coop_enabled = 0;
    s_actor_count = 0;
    s_current = -1;
    psx_enh_scratch_configure(0, 0);
}

int psx_coop_enabled(void) { return s_coop_enabled; }

/* Seed a companion by cloning the primary's current struct, so it starts as a
 * valid actor rather than zeroes. Zeroes would be actively dangerous: the state
 * index is read with a SIGNED load, so a stray negative byte would index
 * backwards off the dispatch table. Cloning a known-good live actor avoids
 * inventing any field values. */
int psx_coop_spawn(int idx)
{
    if (!s_coop_enabled || idx < 0 || idx >= s_actor_count) return 0;
    for (uint32_t off = 0; off < s_struct_len; off++)
        psx_write_byte(s_actors[idx].guest_base + off,
                       psx_read_byte(s_primary_base + off));
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

uint32_t psx_coop_actor_guest_base(int idx)
{
    return (idx >= 0 && idx < s_actor_count) ? s_actors[idx].guest_base : 0;
}

void psx_coop_stats(uint64_t *ticks, uint64_t *redirects)
{
    if (ticks)     *ticks = s_ticks;
    if (redirects) *redirects = s_redirects;
}

/* Feed a companion its own controller.
 *
 * The engine keeps the processed pad word inside the actor struct, so writing
 * it into the companion's own copy is enough for every read that goes through
 * the redirected pointer -- no extra hook needed.
 *
 * Two conversions matter:
 *  - SIO reports ACTIVE-LOW (0 = pressed); the engine stores ACTIVE-HIGH. That
 *    is not a guess: the demo replay computes its newly-pressed mask as
 *    `cur & (cur ^ prev)`, which only means "newly pressed" for active-high.
 *  - The engine keeps that edge mask in the next field, so we maintain it here
 *    per companion rather than letting it go stale.
 *
 * Offsets are supplied by the game (psx_coop_set_input_layout); zero disables
 * the feed entirely, keeping this inert for titles that store input elsewhere. */
static uint32_t s_in_cur_off  = 0;
static uint32_t s_in_edge_off = 0;
static int      s_in_enabled  = 0;
static uint16_t s_prev_pad[COOP_MAX_ACTORS];

/* Engines commonly ALSO read the processed pad word from a fixed global rather
 * than through the actor pointer (measured on MMX6: feeding only the struct
 * field left the companion motionless while player 1 moved). When that global
 * is declared, swap it to the companion's input for the duration of that
 * companion's dispatch and restore player 1's before player 1 ticks. The swap
 * is safe because each dispatch is synchronous and this hook runs exactly once
 * per dispatch, so there is no window where the wrong owner's input is live. */
static uint32_t s_in_global      = 0;
static uint16_t s_saved_global   = 0;
static int      s_global_swapped = 0;

void psx_coop_set_input_layout(uint32_t cur_off, uint32_t edge_off,
                               uint32_t input_global, int enabled)
{
    s_in_cur_off  = cur_off;
    s_in_edge_off = edge_off;
    s_in_global   = input_global;
    s_in_enabled  = enabled ? 1 : 0;
    s_global_swapped = 0;
    for (int i = 0; i < COOP_MAX_ACTORS; i++) s_prev_pad[i] = 0;
}

static void coop_write16(uint32_t addr, uint16_t v)
{
    psx_write_byte(addr,     (uint8_t)(v & 0xFF));
    psx_write_byte(addr + 1, (uint8_t)(v >> 8));
}

static uint16_t coop_read16(uint32_t addr)
{
    return (uint16_t)((uint16_t)psx_read_byte(addr) |
                      ((uint16_t)psx_read_byte(addr + 1) << 8));
}

/* Restore player 1's input global if we swapped it for a companion. */
static void coop_unswap_global(void)
{
    if (!s_global_swapped) return;
    coop_write16(s_in_global, s_saved_global);
    s_global_swapped = 0;
}

static void coop_feed_input(int idx)
{
    if (!s_in_enabled) return;
    /* Companion N takes SIO slot N+1: player 1 keeps port 1. */
    const int slot = idx + 1;
    if (slot > 1) return;                     /* only two physical ports exist */
    const uint16_t active_high = (uint16_t)(~sio_get_pad_buttons_slot(slot) & 0xFFFFu);
    const uint16_t edge = (uint16_t)(active_high & (active_high ^ s_prev_pad[idx]));
    s_prev_pad[idx] = active_high;

    const uint32_t base = s_actors[idx].guest_base;
    coop_write16(base + s_in_cur_off, active_high);
    if (s_in_edge_off) coop_write16(base + s_in_edge_off, edge);

    /* And the fixed global, for the reads that bypass the actor pointer. */
    if (s_in_global) {
        if (!s_global_swapped) {
            s_saved_global = coop_read16(s_in_global);
            s_global_swapped = 1;
        }
        coop_write16(s_in_global, active_high);
    }
}

/* ---- gen-time hook 1: actor-pointer redirect -------------------------------
 * Identity unless co-op is on AND we are mid-companion-tick. Note it does NOT
 * consult the gate: by the time a companion tick is running the gate has
 * already been checked once, and re-checking mid-tick could redirect only part
 * of a dispatch. */
uint32_t psx_coop_actor_base(uint32_t vanilla_base)
{
    if (!s_coop_enabled || s_current < 0) return vanilla_base;
    /* Only redirect the struct this feature owns; any other pointer formed by
     * the same instruction (different base register value) passes through. */
    if (vanilla_base != s_primary_base) return vanilla_base;
    s_redirects++;
    return s_actors[s_current].guest_base;
}

/* ---- gen-time hook 2: per-actor stage replay ------------------------------
 * Runs in the delay slot of ONE per-actor stage's jal, after the CPS emitter
 * has stored the natural return address. Returns the link that stage's callee
 * should come back to:
 *
 *   - a companion still owes this stage a pass -> select it and return
 *     `jal_addr`, so control lands back on the same jal and the stage repeats
 *     for that actor;
 *   - list exhausted -> deselect and return the natural link, so player 1 gets
 *     the final pass and the frame moves on to the next stage.
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
 * ordinary guest return and the selection is recomputed on each entry. */
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
    if (!s_coop_enabled || s_actor_count == 0) { s_current = -1; coop_unswap_global(); return natural_link; }
    if (!coop_gate_open())                     { s_current = -1; coop_unswap_global(); return natural_link; }

    const int slot = coop_stage_slot(jal_addr);
    if (slot < 0) { s_current = -1; return natural_link; }

    for (int i = s_stage_cursor[slot] + 1; i < s_actor_count; i++) {
        if (!s_actors[i].active) continue;
        s_stage_cursor[slot] = i;
        s_current = i;
        s_ticks++;
        coop_feed_input(i);       /* companion reads its own pad, not player 1's */
        /* Stages that take the actor POINTER AS AN ARGUMENT (sprite submission,
         * for example) receive it in a0 from the pipeline driver, which runs
         * only once per frame -- replaying the stage alone would re-run it with
         * a0 still aimed at player 1, which is exactly why a companion ticked
         * but never appeared on screen. Point a0 at the selected actor. Stages
         * that build the pointer internally overwrite a0 themselves, so this is
         * harmless to them. */
        if (cpu) cpu->gpr[4] = s_actors[i].guest_base;
        return jal_addr;          /* replay this stage for that companion */
    }
    s_stage_cursor[slot] = -1;    /* round complete for this stage */
    s_current = -1;               /* player 1 gets the final pass */
    coop_unswap_global();
    return natural_link;
}
