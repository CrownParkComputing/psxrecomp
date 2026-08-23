#!/usr/bin/env python3
"""Guard PSX display enhancements as trusted-mod-only features."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)
RUI_ROOT = ROOT.parent / "recomp-ui"
RUI_HEADER = (RUI_ROOT / "src" / "recomp_launcher.h").read_text(encoding="utf-8")
RUI_MODEL = (RUI_ROOT / "src" / "common" / "launcher_model.c").read_text(
    encoding="utf-8"
)
RUI_IMGUI = (
    RUI_ROOT / "src" / "common" / "backends" / "imgui" / "launcher_imgui.cpp"
).read_text(encoding="utf-8")

for declaration in (
    "constexpr bool ws_offered = false;",
    "constexpr bool ws_ultrawide_offered = false;",
    "constexpr bool frame_interpolation_offered = false;",
    "constexpr bool skip_fmv_offered = false;",
):
    assert declaration in MAIN, f"PSX launcher capability must default off: {declaration}"

for legacy_route in (
    "ws_offered = gc.ws_offered;",
    "ws_ultrawide_offered = gc.ws_ultrawide_offered;",
    "frame_interpolation_offered =\n                gc.runtime.video_offer_frame_interpolation;",
    "skip_fmv_offered = gc.runtime.video_offer_skip_fmv;",
):
    assert legacy_route not in MAIN, f"legacy offer flag still controls UI: {legacy_route}"

for hidden_capability in (
    "gi->widescreen_supported = 0;",
    "gi->aspect_mask = 0;",
):
    assert hidden_capability in MAIN

for trusted_api in (
    "psx_mod_set_fixed_display_aspect",
    "psx_mod_set_adaptive_display_aspect",
    "psx_mod_set_frame_interpolation",
    "psx_mod_set_auto_skip_fmv",
):
    assert trusted_api in HEADER, f"missing trusted mod API: {trusted_api}"

assert MAIN.index("g_auto_skip_fmv = 0;") < MAIN.index("mod_runtime_activate_plugins();")

# SCES-02845 is the one explicit title-facing exception. Generic PSX profiles
# remain hidden above; WipEout uses the same trusted fixed-aspect API.
assert 'game_id == "SCES-02845"' in MAIN
for label in ('"4:3 (Native)"', '"16:9"', '"21:9"', '"32:9"'):
    assert label in MAIN
assert "kWipeoutResolutionLabels" in MAIN
assert '"2560 x 1440 (16:9)"' in MAIN
assert '"3840 x 2160 (16:9, 4K)"' in MAIN
assert '"7680 x 4320 (16:9, 8K)"' in MAIN
assert "kWipeoutRefreshHz[] = { 60, 100 }" in MAIN
assert "psx_apply_requested_exclusive_display_mode" in MAIN


def int_array(name: str) -> list[int]:
    body = re.search(
        rf"static const int {name}\[\] = \{{(.*?)\}};", MAIN, re.S
    )
    assert body, f"missing catalog array: {name}"
    return [int(v) for v in re.findall(r"\d+", body.group(1))]


widths = int_array("kWipeoutResolutionWidths")
heights = int_array("kWipeoutResolutionHeights")
max_hz = int_array("kWipeoutResolutionMaxRefreshHz")
assert len(widths) == len(heights) == len(max_hz) == 24
catalog = {(w, h): hz for w, h, hz in zip(widths, heights, max_hz)}
for mode in ((2560, 1440), (3840, 2160), (3440, 1440), (5120, 1440)):
    assert catalog[mode] == 100, f"100 Hz target missing: {mode}"
for mode in ((5760, 4320), (7680, 4320), (7680, 3200), (7680, 2160)):
    assert catalog[mode] == 60, f"8K-class mode must cap at 60 Hz: {mode}"

for abi_field in (
    "window_height",
    "display_refresh_hz",
    "output_resolution_labels",
    "output_resolution_max_refresh_hz",
    "output_refresh_hz",
):
    assert abi_field in RUI_HEADER, f"missing output selector ABI: {abi_field}"
assert "m->s.display_refresh_hz >" in RUI_MODEL, "8K refresh cap not enforced"
assert 'row_label("Output refresh"' in RUI_IMGUI
assert '"Host display-mode preference only.' in RUI_IMGUI

# The display-mode helper must remain presentation-only. Pin its body so a
# future refactor cannot accidentally make 100 Hz alter emulated time.
mode_helper = MAIN.split(
    "static void psx_apply_requested_exclusive_display_mode", 1
)[1].split("}\n", 1)[0]
for forbidden in (
    "g_frame_period_ms",
    "psx_mod_set_native_vblank_rate",
    "cpu_overclock",
    "cdrom",
    "spu",
):
    assert forbidden not in mode_helper, f"output refresh leaked into timing: {forbidden}"

print("mod-owned PSX display controls guard passed")
