/* tweak_runtime.h — runtime layer for the compile-free Tweaks feature
 * (see the game repo's TWEAKS_PREBAKE.md, Phase 2).
 *
 * Reads a launcher-written state file next to game.toml (`tweaks.state`) and:
 *   - holds g_tweak_flags (which baked guarded CODE variants are active) and
 *     g_tweak_param[] (values for parameterized instruction immediates). Both are
 *     read by recompiler-emitted code (Phase 3); until that lands they are simply
 *     loaded and inert, so a build with this module is behaviourally unchanged.
 *   - applies [[tweak.poke]] byte writes into guest RAM once the game EXE has
 *     loaded and started — for SLUS data-section values the game only READS
 *     (no recompile needed). Values the game re-initialises each boot need the
 *     game_options-style store intercept instead; that is a separate follow-up.
 *
 * Runtime-only, no debug server, works in release. Safe/bounded and validated
 * like game_options.c: a missing/short/over-version file just disables the
 * feature and never blocks boot.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TWEAK_MAX_PARAM 128

/* Read by recompiler-emitted guarded variants (g_tweak_flags bits) and
 * parameterized immediate reads (g_tweak_param[index]) — Phase 3. */
extern uint64_t g_tweak_flags;
extern int32_t  g_tweak_param[TWEAK_MAX_PARAM];

/* Parse the launcher-written state file. A NULL path, missing file, or a file
 * declaring a newer format_version disables the feature. Copies what it needs;
 * call once at startup. */
void tweak_runtime_configure(const char *state_path);

/* Apply the poke RAM writes. Call once the game EXE has loaded and started
 * (fntrace_is_game_started); idempotent (a self-guard makes repeat calls no-ops). */
void tweak_runtime_apply(void);

/* Diagnostics JSON for the debug server (configured flags/params/pokes + whether
 * applied + current RAM readback). Returns bytes written. */
int tweak_runtime_debug_json(char *out, int cap);

#ifdef __cplusplus
}
#endif
