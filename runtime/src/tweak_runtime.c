/* tweak_runtime.c — see tweak_runtime.h.
 *
 * Phase 2 of the compile-free Tweaks feature (TWEAKS_PREBAKE.md). Loads the
 * launcher-written `tweaks.state` file and holds the runtime tweak state:
 *   flags   -> g_tweak_flags        (Phase-3 guarded code variants read this)
 *   param   -> g_tweak_param[index] (Phase-3 parameterized immediates read this)
 *   poke    -> written into guest RAM at game start (data-section values)
 *
 * State-file grammar (one directive per line; '#' comments; anything else ignored):
 *   format_version=1
 *   flags=0x0000000000000003
 *   param <index> <value>            ; decimal or 0x, stored int32 at g_tweak_param[index]
 *   poke  <addr_hex> <hexbytes>      ; bytes in ADDRESS order, e.g. poke 0x800A1234 63
 *
 * Bounded + validated like game_options.c; never blocks boot.
 */
#include "tweak_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void    psx_write_byte(uint32_t addr, uint8_t val);  /* memory.c */
extern uint8_t psx_read_byte(uint32_t addr);                /* memory.c */

#define TWEAK_MAX_POKE     1024      /* large data tables chunk into many poke lines */
#define TWEAK_POKE_BYTES     64      /* max bytes per poke line (emitter chunks to 32) */
#define TWEAK_FORMAT_VERSION 1

uint64_t g_tweak_flags = 0;
int32_t  g_tweak_param[TWEAK_MAX_PARAM];

typedef struct { uint32_t addr; uint16_t nbytes; uint8_t bytes[TWEAK_POKE_BYTES]; } Poke;
static Poke g_poke[TWEAK_MAX_POKE];
static int  g_npoke = 0;
static int  g_nparam_set = 0;
static int  g_applied = 0;
static int  g_have = 0;
static int  g_ver_bad = 0;
static char g_path[1024];

static void reset(void) {
    g_tweak_flags = 0;
    g_npoke = g_nparam_set = g_applied = g_have = 0;
    memset(g_tweak_param, 0, sizeof(g_tweak_param));
}

static int parse_hexbytes(const char *s, uint8_t *out, int cap) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    if (len <= 0 || (len & 1) || (len / 2) > cap) return 0;
    for (int b = 0; b < len / 2; b++) {
        char t[3] = { s[2 * b], s[2 * b + 1], 0 };
        char *end = NULL;
        long v = strtol(t, &end, 16);
        if (end != t + 2) return 0;
        out[b] = (uint8_t)v;
    }
    return len / 2;
}

void tweak_runtime_configure(const char *state_path) {
    reset();
    g_ver_bad = 0;
    g_path[0] = '\0';
    if (!state_path) return;
    snprintf(g_path, sizeof(g_path), "%s", state_path);
    FILE *f = fopen(state_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (strncmp(line, "format_version=", 15) == 0) {
            if (strtol(line + 15, NULL, 0) > TWEAK_FORMAT_VERSION) {
                g_ver_bad = 1; fclose(f); reset(); return;   /* newer format -> drop */
            }
            continue;
        }
        if (strncmp(line, "flags=", 6) == 0) {
            g_tweak_flags = (uint64_t)strtoull(line + 6, NULL, 0);
            continue;
        }
        if (strncmp(line, "param ", 6) == 0) {
            int idx; long val;
            if (sscanf(line + 6, "%d %li", &idx, &val) == 2 &&
                idx >= 0 && idx < TWEAK_MAX_PARAM) {
                g_tweak_param[idx] = (int32_t)val;
                g_nparam_set++;
            }
            continue;
        }
        if (strncmp(line, "poke ", 5) == 0) {
            char astr[32], hstr[2 * TWEAK_POKE_BYTES + 8];
            if (sscanf(line + 5, "%31s %135s", astr, hstr) == 2 && g_npoke < TWEAK_MAX_POKE) {
                uint8_t bytes[TWEAK_POKE_BYTES];
                int nb = parse_hexbytes(hstr, bytes, TWEAK_POKE_BYTES);
                if (nb > 0) {
                    g_poke[g_npoke].addr = (uint32_t)strtoul(astr, NULL, 0);
                    g_poke[g_npoke].nbytes = (uint16_t)nb;
                    memcpy(g_poke[g_npoke].bytes, bytes, nb);
                    g_npoke++;
                }
            }
            continue;
        }
    }
    fclose(f);
    g_have = 1;
}

void tweak_runtime_apply(void) {
    if (g_applied) return;
    g_applied = 1;
    for (int i = 0; i < g_npoke; i++)
        for (int b = 0; b < g_poke[i].nbytes; b++)
            psx_write_byte(g_poke[i].addr + (uint32_t)b, g_poke[i].bytes[b]);
}

int tweak_runtime_debug_json(char *out, int cap) {
    char p[1024];
    snprintf(p, sizeof(p), "%s", g_path);
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    int n = snprintf(out, cap,
        "{\"have\":%d,\"version_rejected\":%d,\"flags\":\"0x%016llX\",\"n_param\":%d,"
        "\"n_poke\":%d,\"applied\":%d,\"path\":\"%s\",\"pokes\":[",
        g_have, g_ver_bad, (unsigned long long)g_tweak_flags, g_nparam_set,
        g_npoke, g_applied, p);
    for (int i = 0; i < g_npoke && n < cap; i++) {
        /* readback: current guest RAM at the poke address (proves it landed) */
        uint32_t cur = 0;
        for (int b = 0; b < g_poke[i].nbytes; b++)
            cur |= (uint32_t)psx_read_byte(g_poke[i].addr + (uint32_t)b) << (8 * b);
        n += snprintf(out + n, (cap - n) > 0 ? cap - n : 0,
            "%s{\"addr\":\"0x%08X\",\"nbytes\":%d,\"cur\":%u}",
            i ? "," : "", g_poke[i].addr, g_poke[i].nbytes, (unsigned)cur);
    }
    n += snprintf(out + n, (cap - n) > 0 ? cap - n : 0, "]}");
    return n;
}
