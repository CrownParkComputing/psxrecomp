/* test_pgxp.cpp — PGXP value-propagation engine unit tests (ENHANCEMENTS.md
 * G1.2/G1.3). White-box over runtime/src/pgxp.cpp with the gte.cpp fallback
 * cache stubbed, exercising exactly the properties the engine's safety rests
 * on: provenance roundtrips, validate-on-read, half-word semantics, the
 * repack arithmetic, the suppression bracket, and the GPU-side safeguards. */

#include "pgxp.h"
#include "pgxp_hooks.h"
#include "psx_memory.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- gte.cpp fallback-cache stub ----------------------------------------- */

static uint32_t g_fb_packed = 0;
static int32_t  g_fb_x16 = 0, g_fb_y16 = 0;
static int      g_fb_valid = 0;

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16) {
    if (!g_fb_valid || packed != g_fb_packed) return 0;
    if (x16) *x16 = g_fb_x16;
    if (y16) *y16 = g_fb_y16;
    return 1;
}

/* ---- MIPS encodings ------------------------------------------------------ */

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh,
                      uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
static uint32_t enc_cop2(uint32_t sub, uint32_t rt, uint32_t rd) {
    return (0x12u << 26) | (sub << 21) | (rt << 16) | (rd << 11);
}

#define LW(rs, rt)   enc_i(0x23, rs, rt, 0)
#define SW(rs, rt)   enc_i(0x2B, rs, rt, 0)
#define LH(rs, rt)   enc_i(0x21, rs, rt, 0)
#define LHU(rs, rt)  enc_i(0x25, rs, rt, 0)
#define SH(rs, rt)   enc_i(0x29, rs, rt, 0)
#define SB(rs, rt)   enc_i(0x28, rs, rt, 0)
#define LWC2(rt)     enc_i(0x32, 1, rt, 0)
#define SWC2(rt)     enc_i(0x3A, 1, rt, 0)
#define MFC2(rt, rd) enc_cop2(0x00, rt, rd)
#define MTC2(rt, rd) enc_cop2(0x04, rt, rd)
#define ADDIU(rs, rt, imm) enc_i(0x09, rs, rt, (uint16_t)(imm))
#define LUI(rt, imm) enc_i(0x0F, 0, rt, (uint16_t)(imm))
#define SLL(rt, rd, sh) enc_r(0, rt, rd, sh, 0x00)
#define SRA(rt, rd, sh) enc_r(0, rt, rd, sh, 0x03)
#define AND(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x24)
#define OR(rs, rt, rd)  enc_r(rs, rt, rd, 0, 0x25)
#define ADDU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x21)

/* One projected vertex: x = 160.5, y = 80.25 -> packed integer word. */
static const uint32_t PACKED  = (80u << 16) | 160u;
static const int32_t  X16     = (160 << 16) | 0x8000;   /* 160.5  */
static const int32_t  Y16     = (80 << 16)  | 0x4000;   /* 80.25  */
static const uint16_t SZ3     = 100;

static const uint32_t ADDR_A  = 0x80100000u;   /* packet slot A (KSEG0)  */
static const uint32_t ADDR_B  = 0x00100040u;   /* packet slot B (KUSEG)  */
static const uint32_t ADDR_C  = 0x00100044u;   /* adjacent packet word    */

static void produce_custom(uint32_t addr, uint32_t packed,
                           int32_t x16, int32_t y16, uint16_t z) {
    pgxp_gte_push_sxy(x16, y16, z, packed);
    psx_pgxp_cop2(nullptr, SWC2(14), packed, addr);
}

static void produce_at(uint32_t addr) {
    produce_custom(addr, PACKED, X16, Y16, SZ3);
}

static int lookup(uint32_t addr, uint32_t word, int32_t ix, int32_t iy,
                  int32_t *x, int32_t *y, uint16_t *z) {
    int32_t lx, ly; uint16_t lz;
    int r = pgxp_get_precise_vertex(addr, word, ix, iy, &lx, &ly, &lz);
    if (x) *x = lx;
    if (y) *y = ly;
    if (z) *z = lz;
    return r;
}

int main(void) {
    pgxp_set_enabled(1);
    CHECK(pgxp_tolerance() < 0.0f);
    CHECK(!pgxp_vertex_cache());
    pgxp_set_tolerance(-1.0f);
    pgxp_set_cpu_mode(0);

    /* --- negative screen coordinates: fixed conversion is UB-free -------- */
    {
        const uint32_t packed = ((uint32_t)(uint16_t)-80 << 16) |
                                (uint16_t)-160;
        const int32_t x16 = -160 * 65536 + 0x8000; /* -159.5, floor = -160 */
        const int32_t y16 = -80 * 65536 + 0x4000;  /* -79.75, floor = -80 */
        int32_t x, y; uint16_t z;
        produce_custom(ADDR_A, packed, x16, y16, SZ3);
        pgxp_set_tolerance(0.25f);
        CHECK(lookup(ADDR_A, packed, -160, -80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
        CHECK(x == -160 * 65536 && y == -80 * 65536 && z == 0);
        pgxp_set_tolerance(0.75f);
        CHECK(lookup(ADDR_A, packed, -160, -80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == x16 && y == y16 && z == SZ3);
        pgxp_set_tolerance(-1.0f);
    }

    /* A precise coordinate below its packet integer produces a negative
     * delta. It must hit the tolerance gate symmetrically, before the later
     * structural truncation safeguard. */
    {
        PGXPStats before, after;
        const int32_t below_x16 = 159 * 65536 + 0x8000; /* 159.5 vs 160 */
        produce_custom(ADDR_A, PACKED, below_x16, Y16, SZ3);
        pgxp_set_tolerance(0.25f);
        pgxp_get_stats(&before);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        pgxp_get_stats(&after);
        CHECK(after.tolerance_reject == before.tolerance_reject + 1);
        CHECK(after.trunc_reject == before.trunc_reject);
        pgxp_set_tolerance(-1.0f);
    }

    /* --- SWC2 produce -> GPU consume (the perspective-texturing spine) --- */
    produce_at(ADDR_A);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* mirrors resolve to the same shadow word */
        CHECK(lookup(0xA0100000u, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
    }

    /* --- DMA/untracked overwrite: value validation rejects the shadow --- */
    {
        int32_t x, y; uint16_t z;
        uint32_t other = (81u << 16) | 161u;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(x == (161 << 16) && y == (81 << 16) && z == 0);
    }

    /* --- LW/SW roundtrip: packet copied by the CPU keeps provenance --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
    }

    /* --- stale GPR: register changed between load and store --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, 0xDEADBEEFu);   /* r8 mutated */
    CHECK(lookup(ADDR_B, 0xDEADBEEFu, 0, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- same-value overwrite: value validation cannot detect provenance --- */
    {
        const uint32_t dst = ADDR_B + 0x40u;
        produce_at(ADDR_A);
        psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
        /* A synthetic/COP0/link write can reproduce the exact packed word but
         * is still a new origin. It must not inherit the loaded sub-pixel XY. */
        psx_pgxp_gpr_written(nullptr, 8, PACKED);
        psx_pgxp_store(nullptr, SW(1, 8), dst, PACKED);
        CHECK(lookup(dst, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
    }

    /* --- external same-value writes invalidate by provenance, not value --- */
    {
        int32_t x, y; uint16_t z;
        produce_at(ADDR_A);
        produce_at(ADDR_C);
        pgxp_external_write(ADDR_A + 1u, 1u);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
        CHECK(lookup(ADDR_C, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);

        /* KSEG aliases invalidate the canonical shadow slot too. */
        produce_at(ADDR_A);
        pgxp_external_write(0x00100000u, 4u);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);

        /* Speculative/rollback writes are deliberately suppressed. */
        produce_at(ADDR_A);
        pgxp_suppress_begin();
        pgxp_external_write(ADDR_A, 4u);
        pgxp_suppress_end();
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
    }

    /* --- target RAM geometry controls provenance aliasing ---------------- */
    {
        const uint32_t low = 0x00000000u;
        const uint32_t high = 0x00600000u;
        int32_t x, y; uint16_t z;
        pgxp_invalidate_all();
        produce_at(high);
#if PSX_MAIN_RAM_BYTES == PSX_MAIN_RAM_EXPANDED_BYTES
        CHECK(lookup(high, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
        CHECK(lookup(low, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
        pgxp_external_write(high, 4u);
        CHECK(lookup(high, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
#else
        CHECK(lookup(low, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
        pgxp_external_write(low, 4u);
        CHECK(lookup(high, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
#endif
    }

    /* --- destructive ALU alias: equal integer result still replaces shadow --- */
    {
        const uint32_t dst = ADDR_B + 0x80u;
        produce_at(ADDR_A);
        psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
        /* and r8,r8,r9 with r9=-1 leaves the architectural word unchanged.
         * The hook receives pre-write sources, then clears r8 provenance. */
        psx_pgxp_alu(nullptr, AND(8, 9, 8), PACKED, PACKED, 0xFFFFFFFFu);
        psx_pgxp_store(nullptr, SW(1, 8), dst, PACKED);
        CHECK(lookup(dst, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
    }

    /* --- MOVE idiom (memory mode, no cpu_mode needed) --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDU(8, 0, 10), PACKED, PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- MFC2 -> SW (register transfer path) --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(9, 14), PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- LH/SH: halves travel independently, depth does not survive --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B + 2u, PACKED >> 16);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A, PACKED & 0xFFFFu);     /* X   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B, PACKED & 0xFFFFu);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
        CHECK(z == 0);                       /* SH killed the vertex depth   */
    }

    /* --- SB destroys the touched half only --- */
    produce_at(ADDR_B);
    psx_pgxp_store(nullptr, SB(1, 8), ADDR_B, PACKED & 0xFFu);  /* same byte */
    {
        int32_t x, y; uint16_t z;
        /* low half invalidated -> not a full XY hit anymore */
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) != PGXP_SRC_DATAFLOW);
    }

    /* --- cpu-mode repack: lhu / sll 16 / or (the classic vertex build) --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_load(nullptr, LHU(1, 10), ADDR_A, PACKED & 0xFFFFu);    /* X   */
    psx_pgxp_alu(nullptr, OR(9, 10, 11), PACKED,
                 (PACKED >> 16) << 16, PACKED & 0xFFFFu);
    psx_pgxp_store(nullptr, SW(1, 11), ADDR_B, PACKED);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
    }

    /* --- cpu-mode addiu: fraction rides an integer offset (incl. -N) --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED + 4u);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
    }
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, (uint16_t)-4), PACKED - 4u, PACKED,
                 (uint32_t)(int32_t)-4);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED - 4u);
    {
        int32_t x;
        CHECK(lookup(ADDR_B, PACKED - 4u, 156, 80, &x, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 - (4 << 16));
    }
    pgxp_set_cpu_mode(0);

    /* --- cpu-mode OFF: the same repack must degrade to native, cleanly --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, (PACKED >> 16) << 16);
    CHECK(lookup(ADDR_B, (PACKED >> 16) << 16, 0, 80, nullptr, nullptr,
                 nullptr) == PGXP_SRC_NATIVE);

    /* --- truncation agreement: integer part must match the native parse --- */
    produce_at(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 161, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- tolerance clamp --- */
    produce_at(ADDR_A);
    pgxp_set_tolerance(0.25f);                 /* fraction is 0.5 -> reject  */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_set_tolerance(0.75f);                 /* 0.5 <= 0.75 -> accept      */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_set_tolerance(-1.0f);

    /* --- fallback cache: explicit opt-in, never carries depth ------------ */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_set_vertex_cache(1);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_FALLBACK);
        CHECK(x == X16 && y == Y16 && z == 0);
    }
    pgxp_set_vertex_cache(0);
    g_fb_valid = 0;

    /* --- suppression bracket: nothing records inside it --- */
    pgxp_invalidate_all();
    pgxp_suppress_begin();
    produce_at(ADDR_A);
    pgxp_suppress_end();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- deferred invalidate inside the bracket --- */
    produce_at(ADDR_A);
    pgxp_suppress_begin();
    pgxp_invalidate_all();                     /* deferred                   */
    pgxp_suppress_end();                       /* applies here               */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- invalidate-all + generation wrap --- */
    produce_at(ADDR_A);
    pgxp_invalidate_all();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_test_set_generation(0xFFFFFFFFu);
    pgxp_invalidate_all();
    CHECK(pgxp_test_generation() == 1u);

    /* --- test accessors mirror the SXY FIFO shadows --- */
    pgxp_test_seed_gte_sxy(2, PACKED, X16, Y16, SZ3, 1);
    {
        uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(valid && packed == PACKED && x == X16 && y == Y16 && z == SZ3);
        pgxp_test_seed_gte_sxy(2, 0, 0, 0, 0, 0);
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(!valid);
    }

    /* --- guest SXYP writes never leave stale FIFO precision -------------- */
    {
        const uint32_t old0 = (10u << 16) | 1u;
        const uint32_t old1 = (20u << 16) | 2u;
        const uint32_t old2 = (30u << 16) | 3u;
        pgxp_test_seed_gte_sxy(0, old0, 1 * 65536, 10 * 65536, 10, 1);
        pgxp_test_seed_gte_sxy(1, old1, 2 * 65536, 20 * 65536, 20, 1);
        pgxp_test_seed_gte_sxy(2, old2, 3 * 65536, 30 * 65536, 30, 1);
        pgxp_test_seed_gte_sxy(3, old2, 3 * 65536, 30 * 65536, 30, 1);
        pgxp_gte_reg_written(15, PACKED);
        for (uint32_t i = 0; i < 4; ++i) {
            uint8_t valid = 1;
            pgxp_test_get_gte_sxy(i, nullptr, nullptr, nullptr, nullptr,
                                  &valid);
            CHECK(!valid);
        }

        /* The COP2 hook has the instruction context needed to consume the
         * pre-write snapshot and perform the exact hardware FIFO shift. */
        produce_at(ADDR_A);
        psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
        pgxp_test_seed_gte_sxy(0, old0, 1 * 65536, 10 * 65536, 10, 1);
        pgxp_test_seed_gte_sxy(1, old1, 2 * 65536, 20 * 65536, 20, 1);
        pgxp_test_seed_gte_sxy(2, old2, 3 * 65536, 30 * 65536, 30, 1);
        pgxp_test_seed_gte_sxy(3, old2, 3 * 65536, 30 * 65536, 30, 1);
        pgxp_gte_reg_written(14, PACKED);
        pgxp_gte_reg_written(15, PACKED);
        psx_pgxp_cop2(nullptr, MTC2(8, 15), PACKED, 0);
        {
            uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
            pgxp_test_get_gte_sxy(0, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == old1 && x == 2 * 65536 &&
                  y == 20 * 65536 && z == 20);
            pgxp_test_get_gte_sxy(1, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == old2 && x == 3 * 65536 &&
                  y == 30 * 65536 && z == 30);
        }
        for (uint32_t i = 2; i < 4; ++i) {
            uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
            pgxp_test_get_gte_sxy(i, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == PACKED && x == X16 && y == Y16 &&
                  z == SZ3);
        }

        /* Direct SXY2 writes keep SXY0/SXY1 and mirror new provenance into
         * SXYP (gte_write_data reports 14+15 for this path too). */
        pgxp_gte_reg_written(14, PACKED);
        pgxp_gte_reg_written(15, PACKED);
        psx_pgxp_cop2(nullptr, MTC2(8, 14), PACKED, 0);
        {
            uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
            pgxp_test_get_gte_sxy(0, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == old1 && x == 2 * 65536 &&
                  y == 20 * 65536 && z == 20);
            pgxp_test_get_gte_sxy(1, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == old2 && x == 3 * 65536 &&
                  y == 30 * 65536 && z == 30);
        }
        for (uint32_t i = 2; i < 4; ++i) {
            uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
            pgxp_test_get_gte_sxy(i, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == PACKED && x == X16 && y == Y16 &&
                  z == SZ3);
        }

        /* LWC2 to SXYP consumes the same exact FIFO snapshot contract. */
        pgxp_gte_reg_written(14, PACKED);
        pgxp_gte_reg_written(15, PACKED);
        psx_pgxp_cop2(nullptr, LWC2(15), PACKED, ADDR_A);
        {
            uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
            pgxp_test_get_gte_sxy(0, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == old2 && x == 3 * 65536 &&
                  y == 30 * 65536 && z == 30);
            pgxp_test_get_gte_sxy(1, &packed, &x, &y, &z, &valid);
            CHECK(valid && packed == PACKED && x == X16 && y == Y16 &&
                  z == SZ3);
        }
    }

    /* --- stats sanity: dataflow hits were counted --- */
    {
        PGXPStats st;
        pgxp_get_stats(&st);
        CHECK(st.lookups > 0);
        CHECK(st.dataflow_hit > 0);
        CHECK(st.native > 0);
        CHECK(st.fallback_hit > 0);
        CHECK(st.value_mismatch > 0);
        CHECK(st.external_ranges >= 2);
        CHECK(st.external_words >= 2);
    }

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp: all checks passed\n");
    return 0;
}
