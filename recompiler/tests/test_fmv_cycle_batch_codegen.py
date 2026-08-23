#!/usr/bin/env python3
"""Verify generated FMV cycle batching has portable scope and IRQ flushes."""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


LOAD = 0x80010000


def generate_source(recompiler, transformed=False):
    header = bytearray(2048)
    header[0:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD)
    struct.pack_into("<I", header, 0x18, LOAD)
    words = (
        0x28620010,  # slti v0,v1,16   -- transparent unless config rewrites it
        0x00421821,  # addu v1,v0,v0   -- same straight-line timing run
        0x00622025,  # or a0,v1,v0     -- same straight-line timing run
        0x1480FFFC,  # bne a0,zero,loop -- timing-sensitive boundary
        0x00000000,
        0x03E00008,
        0x00000000,
    )
    body = b"".join(struct.pack("<I", word) for word in words)
    struct.pack_into("<I", header, 0x1C, len(body))

    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "cycle.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        config = os.path.join(tmp, "game.toml")
        with open(exe, "wb") as stream:
            stream.write(header + body)
        with open(seeds, "w", encoding="utf-8") as stream:
            stream.write(f"0x{LOAD:08X}\n")
        args = [recompiler, exe, "--seeds", seeds, "--out-dir", out]
        if transformed:
            with open(config, "w", encoding="utf-8") as stream:
                stream.write(f'''[game]
name = "Cycle run transform test"
exe = "cycle.psx"
load_address = "0x{LOAD:08X}"
entry_pc = "0x{LOAD:08X}"
text_size = "0x{len(body):X}"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "out"

[widescreen.cull]
slti_sites = ["0x{LOAD:08X}"]
''')
            project_root = os.path.abspath(os.path.join(
                os.path.dirname(recompiler), "..", ".."))
            args = [recompiler, "--config", config,
                    "--project-root", project_root]
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
        full = sorted(
            name
            for name in os.listdir(out)
            if "_full" in name and name.endswith(".c") and "_dispatch" not in name
        )
        if not full:
            raise RuntimeError("no generated _full*.c source found")
        sources = []
        for name in full:
            with open(os.path.join(out, name), encoding="utf-8") as stream:
                sources.append(stream.read())
        return "\n".join(sources)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--recompiler", required=True)
    args = parser.parse_args()
    try:
        source = generate_source(args.recompiler)
        transformed_source = generate_source(args.recompiler, transformed=True)
    except (OSError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    guard = (
        "#if defined(PSX_ENABLE_BLOCK_CYCLES) && "
        "(defined(__GNUC__) || defined(__clang__))\n"
    )
    guarded_begin = (
        guard
        + "    __attribute__((cleanup(psx_cyc_bb_defer_cleanup))) "
        "int _psx_cyc_bb_guard = 1;\n"
        "    psx_cyc_bb_defer_begin();\n"
        "#endif\n"
    )
    failures = []
    if guarded_begin not in source:
        failures.append("basic-block defer is not wholly GCC/Clang guarded")
    flush = "psx_cyc_bb_defer_flush();\n#endif\n"
    if flush not in source or "psx_check_interrupts_at(cpu," not in source:
        failures.append("generated interrupt edges do not flush deferred cycles")
    run_decl = "static const uint32_t _psx_cyc_run_80010000[] = {"
    run_call = (
        "psx_cyc_step_run_fast(cpu, _psx_cyc_run_80010000, 3u);"
    )
    if run_decl not in source or run_call not in source:
        failures.append("transparent straight-line instructions were not aggregated")
    if "psx_ws_cull_slti(" not in transformed_source:
        failures.append("configured SLTI fixture was not transformed")
    if "psx_cyc_step_run_fast(cpu, _psx_cyc_run_80010000, 3u);" in transformed_source:
        failures.append("configured helper body was incorrectly crossed by a timing run")
    if "psx_cyc_step_run_fast(cpu, _psx_cyc_run_80010004, 2u);" not in transformed_source:
        failures.append("timing run did not resume after configured helper barrier")
    if "!defined(PSX_COSIM)" not in source:
        failures.append("COSIM did not retain instruction-site timing steps")
    if "defined(PSX_GAME_GENERATED_TIMING_RUNS)" not in source:
        failures.append("timing-run A/B gate was not emitted")
    if "psx_cyc_step_slow(cpu," not in source:
        failures.append("exceptional modes lost instruction-boundary fallback")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: generated FMV cycle batching is scoped and flushed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
