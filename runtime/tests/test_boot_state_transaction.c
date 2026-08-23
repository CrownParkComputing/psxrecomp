#include "boot_state.h"
#include "gpu_vram_dirty.h"
#include "psx_cycles.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_BYTES (2u * 1024u * 1024u)
#define SPAD_BYTES 1024u
#define VRAM_WORDS (1024u * 512u)
#define SPURAM_BYTES 4096u
#define DEVICE_BYTES 16u
#define DIRTY_WORDS 16u
#define BIOS_SUM 0x10203040u
#define ENTRY_PC 0x80010000u

static uint8_t g_ram[RAM_BYTES];
static uint8_t g_spad[SPAD_BYTES];
static uint16_t g_vram[VRAM_WORDS];
static uint8_t g_spuram[SPURAM_BYTES];
static uint8_t g_gpu[DEVICE_BYTES];
static uint8_t g_spu[DEVICE_BYTES];
static uint8_t g_cdrom[DEVICE_BYTES];
static uint8_t g_dma[DEVICE_BYTES];
static uint8_t g_sio[DEVICE_BYTES];
static uint8_t g_mdec[DEVICE_BYTES];
static uint32_t g_dirty[DIRTY_WORDS];
static uint16_t g_timer_counter[3];
static uint32_t g_timer_mode[3];
static uint16_t g_timer_target[3];
static int32_t g_timer_irq_line[3];
static uint32_t g_timer_frac[3];
static uint32_t g_cycles_since_vblank;
static uint32_t g_vblank_fraction;
static uint32_t g_requested_percent = 100u;
static int g_overclock_active;
static uint64_t g_dirty_mask[GPU_VRAM_DIRTY_H / 64u];
static int g_bless_calls;
static int g_overlay_invalidate_calls;
static int g_failures;
static int g_resume_calls;
static int g_canonicalize_calls;
static int g_service_mutate_ram_once;

uint32_t i_stat;
uint32_t i_mask;
uint64_t psx_cycle_count;
uint64_t psx_cpu_retired_cycles;
uint64_t psx_cpu_native_cycles;
uint64_t psx_next_service_cycle;
int psx_in_device_service;
int g_event_step_conservative;
int g_ls_replay_active;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
int g_psx_cyc_bb_defer;
uint32_t* g_psx_cyc_local_acc;
uint32_t g_psx_oc_numerator = 1u;
uint32_t g_psx_oc_denominator = 1u;
uint32_t g_psx_oc_accum;
uint32_t g_psx_icache_tv[1024];
int g_psx_vram_dirty_tracking;

void psx_devices_service_to_now(void) {
    if (g_service_mutate_ram_once) {
        g_service_mutate_ram_once = 0;
        g_ram[0] = 0xa5u; /* model a due device DMA at the save boundary */
    }
}
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }

static uint64_t fnv_bytes(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    while (n--) {
        h ^= *b++;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t machine_digest(const CPUState* cpu) {
    uint64_t h = 1469598103934665603ull;
#define HASH(v) h = fnv_bytes(h, &(v), sizeof(v))
    HASH(*cpu);
    HASH(g_ram);
    HASH(g_spad);
    HASH(g_vram);
    HASH(g_spuram);
    HASH(g_gpu);
    HASH(g_spu);
    HASH(g_cdrom);
    HASH(g_dma);
    HASH(g_sio);
    HASH(g_mdec);
    HASH(g_dirty);
    HASH(g_timer_counter);
    HASH(g_timer_mode);
    HASH(g_timer_target);
    HASH(g_timer_irq_line);
    HASH(g_timer_frac);
    HASH(g_cycles_since_vblank);
    HASH(g_vblank_fraction);
    HASH(i_stat);
    HASH(i_mask);
    HASH(psx_cycle_count);
    HASH(psx_cpu_retired_cycles);
    HASH(psx_cpu_native_cycles);
    HASH(g_psx_oc_accum);
    HASH(g_psx_icache_tv);
#undef HASH
    return h;
}

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void fill_machine(CPUState* cpu, uint8_t seed) {
    memset(cpu, 0, sizeof(*cpu));
    for (size_t i = 0; i < sizeof(cpu->gpr) / sizeof(cpu->gpr[0]); i++)
        cpu->gpr[i] = (uint32_t)seed * 0x01010101u + (uint32_t)i;
    cpu->pc = ENTRY_PC + ((uint32_t)seed << 4);
    cpu->hi = 0x11110000u | seed;
    cpu->lo = 0x22220000u | seed;
    cpu->read_absorb_which = 0x20u;
    cpu->read_fudge = 0x20u;
    cpu->ld_which_t = 0x20u;
    memset(g_ram, seed, sizeof g_ram);
    memset(g_spad, seed + 1u, sizeof g_spad);
    for (size_t i = 0; i < VRAM_WORDS; i++)
        g_vram[i] = (uint16_t)((uint16_t)seed << 8) ^ (uint16_t)i;
    memset(g_spuram, seed + 2u, sizeof g_spuram);
    memset(g_gpu, seed + 3u, sizeof g_gpu);
    memset(g_spu, seed + 4u, sizeof g_spu);
    memset(g_cdrom, seed + 5u, sizeof g_cdrom);
    memset(g_dma, seed + 6u, sizeof g_dma);
    memset(g_sio, seed + 7u, sizeof g_sio);
    memset(g_mdec, seed + 8u, sizeof g_mdec);
    for (uint32_t i = 0; i < DIRTY_WORDS; i++) g_dirty[i] = seed + i;
    for (int i = 0; i < 3; i++) {
        g_timer_counter[i] = (uint16_t)(seed + i);
        g_timer_mode[i] = seed * 100u + (uint32_t)i;
        g_timer_target[i] = (uint16_t)(seed * 2u + i);
        g_timer_irq_line[i] = (int32_t)seed - i;
        g_timer_frac[i] = seed * 3u + (uint32_t)i;
    }
    i_stat = 0x1000u | seed;
    i_mask = 0x2000u | seed;
    g_cycles_since_vblank = 1000u + seed;
    g_vblank_fraction = seed;
    psx_cycle_count = 100000u + seed;
    psx_cpu_retired_cycles = 200000u + seed;
    psx_cpu_native_cycles = 300000u + seed;
    g_psx_oc_accum = 0;
    for (uint32_t i = 0; i < 1024u; i++)
        g_psx_icache_tv[i] = ((uint32_t)seed << 24) | i;
    g_bless_calls = 0;
    g_overlay_invalidate_calls = 0;
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t* find_section(uint8_t* blob, size_t len, uint32_t want_tag,
                             size_t* record_len) {
    uint8_t* p = blob + BOOT_STATE_HEADER_WIRE_BYTES;
    uint8_t* end = blob + len;
    uint32_t count = rd32(blob + 28);
    for (uint32_t i = 0; i < count && (size_t)(end - p) >= 16u; i++) {
        uint64_t payload = rd64(p + 8);
        if (payload > (uint64_t)(end - p - 16u)) return NULL;
        if (rd32(p) == want_tag) {
            if (record_len) *record_len = 16u + (size_t)payload;
            return p;
        }
        p += 16u + (size_t)payload;
    }
    return NULL;
}

static uint8_t* clone_blob(const uint8_t* blob, size_t len) {
    uint8_t* copy = (uint8_t*)malloc(len);
    if (copy) memcpy(copy, blob, len);
    return copy;
}

static int reject_resume_pc(uint32_t pc) {
    (void)pc;
    return 0;
}

static int reject_resume_pc_at_commit(uint32_t pc) {
    (void)pc;
    return ++g_resume_calls == 1;
}

/* ---- boot_state.c runtime seams ---- */

uint8_t* memory_get_ram_ptr(void) { return g_ram; }
uint8_t* memory_get_scratchpad_ptr(void) { return g_spad; }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                         uint16_t target[3], int32_t irq_line[3],
                         uint32_t frac[3]) {
    memcpy(counter, g_timer_counter, sizeof g_timer_counter);
    memcpy(mode, g_timer_mode, sizeof g_timer_mode);
    memcpy(target, g_timer_target, sizeof g_timer_target);
    memcpy(irq_line, g_timer_irq_line, sizeof g_timer_irq_line);
    memcpy(frac, g_timer_frac, sizeof g_timer_frac);
}
void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                         const uint16_t target[3], const int32_t irq_line[3],
                         const uint32_t frac[3]) {
    memcpy(g_timer_counter, counter, sizeof g_timer_counter);
    memcpy(g_timer_mode, mode, sizeof g_timer_mode);
    memcpy(g_timer_target, target, sizeof g_timer_target);
    memcpy(g_timer_irq_line, irq_line, sizeof g_timer_irq_line);
    memcpy(g_timer_frac, frac, sizeof g_timer_frac);
}

#define SNAP_FUNCS(name, storage) \
    uint32_t name##_snapshot_bytes(void) { return DEVICE_BYTES; } \
    void name##_snapshot_write(uint8_t* p) { memcpy(p, storage, DEVICE_BYTES); } \
    int name##_snapshot_read(const uint8_t* p, uint32_t len) { \
        if (len != DEVICE_BYTES) return 0; \
        memcpy(storage, p, DEVICE_BYTES); \
        return 1; \
    }
SNAP_FUNCS(gpu, g_gpu)
SNAP_FUNCS(spu, g_spu)
SNAP_FUNCS(cdrom, g_cdrom)
SNAP_FUNCS(dma, g_dma)
SNAP_FUNCS(sio, g_sio)
#undef SNAP_FUNCS

uint32_t mdec_snapshot_bytes(void) { return DEVICE_BYTES; }
void mdec_snapshot_write(uint8_t* p) { memcpy(p, g_mdec, DEVICE_BYTES); }
int mdec_snapshot_read(const uint8_t* p, uint32_t len) {
    if (len != DEVICE_BYTES) return 0;
    /* Deliberately mutate before rejecting: this models the legacy subsystem
     * parsers and proves the outer load transaction restores their writes. */
    memcpy(g_mdec, p, DEVICE_BYTES);
    return p[0] != 0xeeu;
}

uint8_t* spu_get_ram_ptr(void) { return g_spuram; }
uint32_t spu_get_ram_bytes(void) { return SPURAM_BYTES; }
const uint16_t* gpu_get_vram(void) { return g_vram; }
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t* src) {
    (void)x; (void)y; (void)w; (void)h;
    memcpy(g_vram, src, sizeof g_vram);
}
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t* dst) {
    (void)x; (void)y; (void)w; (void)h;
    memcpy(dst, g_vram, sizeof g_vram);
}

uint32_t dirty_ram_get_bitmap_word_count(void) { return DIRTY_WORDS; }
uint32_t dirty_ram_get_bitmap_word(uint32_t i) {
    return i < DIRTY_WORDS ? g_dirty[i] : 0u;
}
void dirty_ram_set_bitmap_words(const uint32_t* words, uint32_t count) {
    memset(g_dirty, 0, sizeof g_dirty);
    if (count > DIRTY_WORDS) count = DIRTY_WORDS;
    memcpy(g_dirty, words, count * sizeof(*words));
}
void overlay_watch_invalidate_after_ram_restore(void) {
    g_overlay_invalidate_calls++;
}
void psx_kernel_bless_note_range(uint32_t phys, uint32_t len) {
    (void)phys; (void)len;
    g_bless_calls++;
}
uint32_t overlay_loader_active_config_hash(void) { return 0x55667788u; }
void gte_canonicalize_cpu_state(CPUState* cpu) {
    (void)cpu;
    g_canonicalize_calls++;
}

uint32_t interrupts_get_cycles_since_vblank(void) {
    return g_cycles_since_vblank;
}
void interrupts_set_cycles_since_vblank(uint32_t value) {
    g_cycles_since_vblank = value;
}
uint32_t interrupts_get_vblank_fraction(void) { return g_vblank_fraction; }
void interrupts_set_vblank_fraction(uint32_t value) { g_vblank_fraction = value; }
uint32_t interrupts_get_vblank_base_rate(void) { return 50u; }
uint32_t interrupts_get_vblank_multiplier(void) { return 2u; }

uint32_t psx_get_cpu_overclock(void) { return g_requested_percent; }
uint32_t psx_get_effective_cpu_overclock(void) {
    return g_overclock_active ? g_requested_percent : 100u;
}
void psx_clock_domain_snapshot(PSXClockDomainSnapshot* out) {
    out->native_cycle_count = psx_cycle_count;
    out->cpu_retired_cycles = psx_cpu_retired_cycles;
    out->cpu_native_cycles = psx_cpu_native_cycles;
    out->requested_percent = g_requested_percent;
    out->overclock_active = (uint32_t)g_overclock_active;
    out->numerator = g_psx_oc_numerator;
    out->denominator = g_psx_oc_denominator;
    out->remainder = g_psx_oc_accum;
    out->reserved_flags = 0u;
}
int psx_clock_domain_restore(const PSXClockDomainSnapshot* in) {
    if (!in || in->requested_percent != g_requested_percent ||
        in->overclock_active != (uint32_t)g_overclock_active ||
        in->numerator != g_psx_oc_numerator ||
        in->denominator != g_psx_oc_denominator ||
        in->remainder >= in->denominator || in->reserved_flags != 0u)
        return 0;
    psx_cycle_count = in->native_cycle_count;
    psx_cpu_retired_cycles = in->cpu_retired_cycles;
    psx_cpu_native_cycles = in->cpu_native_cycles;
    g_psx_oc_accum = in->remainder;
    return 1;
}

int gpu_vram_dirty_tracking(void) { return g_psx_vram_dirty_tracking != 0; }
void gpu_vram_dirty_clear(void) { memset(g_dirty_mask, 0, sizeof g_dirty_mask); }
void gpu_vram_dirty_mark_row_impl(uint32_t y) {
    g_dirty_mask[y >> 6] |= (uint64_t)1u << (y & 63u);
}
uint32_t gpu_vram_dirty_row_count(void) { return GPU_VRAM_DIRTY_H; }
const uint64_t* gpu_vram_dirty_mask(void) { return g_dirty_mask; }
int gpu_vram_dirty_verify_enabled(void) { return 0; }

static void expect_reject_unchanged(const uint8_t* blob, size_t len,
                                    CPUState* cpu, const char* label) {
    uint64_t before = machine_digest(cpu);
    g_bless_calls = 0;
    g_overlay_invalidate_calls = 0;
    g_canonicalize_calls = 0;
    check(!boot_state_load_buffer(blob, len, BIOS_SUM, ENTRY_PC, cpu), label);
    check(machine_digest(cpu) == before, "rejected load changed machine state");
    check(g_canonicalize_calls == 0,
          "rejected load invalidated live precision state");
    check(g_bless_calls == 0, "rejected load blessed restored RAM");
    check(g_overlay_invalidate_calls == 0,
          "rejected load invalidated overlays");
}

int main(void) {
    CPUState source_cpu, live_cpu;
    uint8_t* valid = NULL;
    size_t valid_len = 0;
    uint8_t* bad;
    uint8_t* section;
    size_t section_len;
    uint64_t source_digest;

    /* A save publication can run due device work, including RAM DMA. That
     * work must happen before RAM is serialized, not later when the clock
     * section is written. */
    fill_machine(&source_cpu, 0x10u);
    g_service_mutate_ram_once = 1;
    check(boot_state_save_buffer_raw(&source_cpu, BIOS_SUM, ENTRY_PC,
                                     &valid, &valid_len),
          "create device-coherent save");
    section = valid ? find_section(valid, valid_len, BS_SEC_RAM, NULL) : NULL;
    check(section != NULL && section[16] == 0xa5u,
          "save serialized RAM before publishing due device DMA");
    free(valid);
    valid = NULL;
    valid_len = 0;

    fill_machine(&source_cpu, 0x11u);
    source_digest = machine_digest(&source_cpu);
    check(boot_state_save_buffer_raw(&source_cpu, BIOS_SUM, ENTRY_PC,
                                     &valid, &valid_len),
          "create valid v6 state");
    if (!valid) return 1;

    /* A late, mutating MDEC semantic failure must roll CPU/RAM and every
     * already-applied device back to the live pre-load machine. */
    fill_machine(&live_cpu, 0x41u);
    bad = clone_blob(valid, valid_len);
    section = find_section(bad, valid_len, BS_SEC_MDEC, NULL);
    check(section != NULL, "find MDEC section");
    if (section) section[16] = 0xeeu;
    expect_reject_unchanged(bad, valid_len, &live_cpu,
                            "late MDEC semantic failure rejects");
    free(bad);

    /* Duplicate tags are rejected during decode, before the first apply. */
    fill_machine(&live_cpu, 0x42u);
    bad = clone_blob(valid, valid_len);
    section = find_section(bad, valid_len, BS_SEC_SPAD, NULL);
    check(section != NULL, "find SPAD section");
    if (section) wr32(section, BS_SEC_RAM);
    expect_reject_unchanged(bad, valid_len, &live_cpu,
                            "duplicate section rejects");
    free(bad);

    /* ICACHE is part of the v6 required set: physically remove its record and
     * decrement section_count so this is a well-framed but incomplete image. */
    fill_machine(&live_cpu, 0x43u);
    bad = clone_blob(valid, valid_len);
    section = find_section(bad, valid_len, BS_SEC_ICACHE, &section_len);
    check(section != NULL, "find ICACHE section");
    if (section) {
        size_t offset = (size_t)(section - bad);
        memmove(section, section + section_len,
                valid_len - offset - section_len);
        wr32(bad + 28, rd32(bad + 28) - 1u);
        expect_reject_unchanged(bad, valid_len - section_len, &live_cpu,
                                "missing ICACHE rejects");
    }
    free(bad);

    /* Dirty-page geometry is part of the machine contract. A short bitmap is
     * not an acceptable partial restore even when it remains word-aligned. */
    fill_machine(&live_cpu, 0x44u);
    bad = clone_blob(valid, valid_len);
    section = find_section(bad, valid_len, BS_SEC_DIRTY, &section_len);
    check(section != NULL && section_len >= 20u, "find DIRTY section");
    if (section && section_len >= 20u) {
        size_t offset = (size_t)(section - bad);
        uint64_t payload_len = rd64(section + 8);
        memmove(section + 16u + payload_len - 4u,
                section + 16u + payload_len,
                valid_len - offset - 16u - (size_t)payload_len);
        /* Raw in-memory saves use no compression, so shortening both the
         * framed payload and file produces a well-formed wrong-size section. */
        wr32(section + 8, (uint32_t)(payload_len - 4u));
        wr32(section + 12, 0u);
        expect_reject_unchanged(bad, valid_len - 4u, &live_cpu,
                                "short dirty bitmap rejects");
    }
    free(bad);

    /* Runtime timing policy identity is configuration. A candidate carrying
     * a different rational ratio is rejected in preflight without mutation. */
    fill_machine(&live_cpu, 0x45u);
    bad = clone_blob(valid, valid_len);
    section = find_section(bad, valid_len, BS_SEC_CLOCK, NULL);
    check(section != NULL, "find CLOCK section");
    if (section) {
        wr32(section + 16 + 24, 200u); /* requested_percent */
        wr32(section + 16 + 28, 1u);   /* active */
        wr32(section + 16 + 32, 1u);   /* numerator */
        wr32(section + 16 + 36, 2u);   /* denominator */
        expect_reject_unchanged(bad, valid_len, &live_cpu,
                                "runtime timing policy mismatch rejects");
    }
    free(bad);

    /* The scheduler's dispatchability gate runs on staged CPU state, before
     * commit, so a resume-PC rejection is transactional too. */
    fill_machine(&live_cpu, 0x46u);
    {
        uint64_t before = machine_digest(&live_cpu);
        g_canonicalize_calls = 0;
        check(!boot_state_load_buffer_checked(valid, valid_len, BIOS_SUM,
                                              ENTRY_PC, &live_cpu,
                                              reject_resume_pc),
              "resume-PC gate rejects");
        check(machine_digest(&live_cpu) == before,
              "resume-PC rejection changed machine state");
        check(g_canonicalize_calls == 0,
              "early resume rejection invalidated precision state");
    }

    /* Also cover state-dependent dispatchability: accept preflight, reject
     * only after all candidate sections were applied, and still roll back. */
    fill_machine(&live_cpu, 0x47u);
    {
        uint64_t before = machine_digest(&live_cpu);
        g_resume_calls = 0;
        g_canonicalize_calls = 0;
        check(!boot_state_load_buffer_checked(valid, valid_len, BIOS_SUM,
                                              ENTRY_PC, &live_cpu,
                                              reject_resume_pc_at_commit),
              "commit-boundary resume-PC gate rejects");
        check(machine_digest(&live_cpu) == before,
              "late resume-PC rejection changed machine state");
        check(g_canonicalize_calls == 0,
              "late resume rejection invalidated precision state");
    }

    fill_machine(&live_cpu, 0x48u);
    g_canonicalize_calls = 0;
    check(boot_state_load_buffer(valid, valid_len, BIOS_SUM, ENTRY_PC,
                                 &live_cpu),
          "valid transactional load succeeds");
    check(machine_digest(&live_cpu) == source_digest,
          "successful load did not restore the serialized machine");
    check(g_bless_calls == 1 && g_overlay_invalidate_calls == 1,
          "successful load did not publish restored RAM exactly once");
    check(g_canonicalize_calls == 1,
          "successful load did not canonicalize precision exactly once");

    free(valid);
    if (g_failures) {
        fprintf(stderr, "%d boot-state transaction checks failed\n", g_failures);
        return 1;
    }
    printf("boot-state transaction checks passed\n");
    return 0;
}
