// tweak_sites.h — shared types for the compile-free Tweaks guarded-variant bake
// (TWEAKS_PREBAKE.md Phase 3). Defined once here and included by both the code
// generator (CodeGenConfig holds the site map) and the config loader (which
// parses the bake manifest into it), so there is a single source of truth.
//
// A guarded site is one MIPS word that one or more Tweaks options patch to a
// DIFFERENT instruction (opcode/reg change — a logic tweak, not a value). The
// recompiler bakes ALL options' patched words at that address into a runtime-
// flag-selected if/else-if chain over the vanilla one, so a single superset
// binary serves every selection with no player-side recompile. The tool's
// radio/PreReq exclusivity guarantees at most one case's flag is active at once.

#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace PSXRecomp {

// One patched variant of a guarded instruction: emitted under `if (psx_tweak_on(flag_bit))`.
struct TweakGuardCase {
    int      flag_bit;       // g_tweak_flags bit index for the owning option
    uint32_t patched_word;   // the option's replacement instruction word
};

// All baked variants for one guarded word address, plus the vanilla word used
// as the gen-time drift check and the final `else` fall-through.
struct TweakGuardSite {
    uint32_t                    vanilla_word;
    std::vector<TweakGuardCase> cases;
};

// word_addr -> site. `.find(addr)` per instruction, mirroring the widescreen
// std::set<uint32_t> site-lookup pattern (here a map because we carry a payload).
using TweakGuardMap = std::map<uint32_t, TweakGuardSite>;

// A parameterized value immediate (strategy B): the instruction's 16-bit
// immediate is emitted as a runtime read g_tweak_param[index] instead of a
// literal, defaulting to vanilla_imm (identity when tweaks.state doesn't set it).
// Only opcodes with a real value immediate are parameterized (addiu/slti/sltiu/
// ori-li); masks, load/store offsets, control flow, and register-form (opcode 0)
// words are never param sites. Mirrors the widescreen psx_ws_x_margin() pattern.
struct TweakParamSite {
    int      index;             // g_tweak_param[] slot
    uint16_t vanilla_imm;       // raw 16-bit immediate in the vanilla word (drift check)
    int32_t  vanilla_default;   // seed value (sign/zero-extended per opcode by the
                                // baker) written to g_tweak_param[index] at boot, so
                                // an unset param reads identical to vanilla
};
using TweakParamMap = std::map<uint32_t, TweakParamSite>;

// -------------------------------------------------------------------------
// Function-variant bake (TWEAKS_PREBAKE.md §13 — dual-source).
// An instruction-level guarded site (above) cannot represent a CONTROL-FLOW
// tweak: the block structure is baked from the vanilla CFG, so a patched
// j/jal/branch can't restructure it mid-block. A function-variant instead
// emits the WHOLE function twice — once from vanilla bytes, once from a
// patched-image VIEW (the word overrides below applied) whose CFG is
// re-analyzed — and a `func_XXXX` dispatcher picks the body at ENTRY via
// psx_tweak_on(flag_bit). The flag is immutable at runtime (set at boot,
// relaunch-to-apply), so re-entered CPS continuations resolve to the same
// variant every time. flags-off => the vanilla body runs (byte-behaviour
// identical to a non-tweak build).
// -------------------------------------------------------------------------

// One instruction word this variant patches (drives BOTH the re-analyzed CFG
// and the translation via PS1Executable::set_word_override).
struct TweakWordOverride {
    uint32_t addr;
    uint32_t vanilla_word;   // gen-time drift check vs the image
    uint32_t patched_word;
};

// One patched variant of a function: its flag bit + the words it overrides.
struct TweakFuncVariant {
    int                            flag_bit;
    std::vector<TweakWordOverride> overrides;
};

// All variants for one function entry address.
struct TweakFuncSite {
    std::vector<TweakFuncVariant> variants;
};

// func_start_addr -> its variants.
using TweakFuncMap = std::map<uint32_t, TweakFuncSite>;

// Full parse of a tweaks_bake.toml: guarded-variant sites + parameterized
// sites + control-flow function-variants.
struct TweakBake {
    TweakGuardMap guarded;
    TweakParamMap param;
    TweakFuncMap  func_variants;
};

} // namespace PSXRecomp
