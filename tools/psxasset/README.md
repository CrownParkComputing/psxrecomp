# psxasset — game-agnostic PSX disc asset extraction

Framework tool. Stdlib-only Python (3.11+), like everything else in `tools/`.
Decodes what it can, carves what it can't, and never needs to know which game
it is looking at unless a registry entry teaches it.

```
cd tools
python3 psxasset ls      <disc.cue|bin>              # list the ISO9660 filesystem
python3 psxasset extract <disc> [paths...] -o DIR    # copy files off the disc
python3 psxasset unpack  <file> [--raw]              # unpack a container (probe or --format)
python3 psxasset scan    <file|dir> [--extract]      # signature-scan, optionally decode hits
python3 psxasset tim2png <file> [--all-palettes]     # TIMs in any blob -> PNG
python3 psxasset vag2wav <file> [--rate N]           # VAG (headered/headerless/raw) -> WAV
python3 psxasset rip     <disc> -o DIR               # the whole pipeline + report.json
```

`rip` output layout:

```
DIR/
  files/      every non-skipped disc file, extracted
  unpacked/   per-container entry blobs (NNN.bin stored, NNN.packed with --keep-packed)
  decoded/
    textures/ <src>_<offset>_<WxH>_<bpp>bpp[_pN].png
    audio/    <src>_<name-or-offset>_<rate>hz.wav
  report.json
```

## What it understands

- **ISO9660** over raw MODE2/2352 (Redump cue/bin), cooked 2048 images, and
  2448 dumps. Boot serial read from SYSTEM.CNF.
- **TIM** — 4/8/16/24bpp, CLUTs, multi-palette (`--all-palettes` exports each
  row). Alpha tiers follow the pack-authoring convention: transparent 0,
  STP 127, opaque 255. `formats/tim.py:parse` is strict enough to double as a
  scanner validator.
- **VAG** — headered `VAGp`, the headerless 48-byte Psygnosis variant, and
  raw SPU-ADPCM with `--rate`. Embedded build-machine filenames are kept in
  output names (`crowd.aif22`, `rocket`, ...).
- **Containers** — `containers/` handlers with `probe()`/`unpack()`.
  Currently `psyg_pb` (Psygnosis .PB/.PBP, WipEout 3 era). Compressed entries
  whose codec is unknown are carved as `.packed` blobs rather than dropped.
- **Signatures** — validated TIM/VAG plus SEQ (`pQES`), VAB (`pBAV`),
  PS-X EXE, and low-confidence TMD candidates (`tmd?`).

## Per-game registry

`games/<title>.toml`, matched by boot serial. This is the piece that keeps
the engine general — the registry knows which game uses which container and
what to skip:

```toml
[game]
serial = "SCES-02845"
name = "WipEout 3 Special Edition"

[[container]]
glob = "WIPEOUT3/*.PBP"   # ISO path, case-insensitive fnmatch
format = "psyg_pb"

[[skip]]
glob = "*.STR"
reason = "FMV stream (MDEC)"
```

No registry entry is required: `rip` falls back to probing every file against
every container handler and signature-scanning whatever isn't a container.
First run against an unknown title is exactly how you discover what its
registry entry should say.

## Measured results (2026-08-20)

| Disc | Registry | Outcome |
|---|---|---|
| WipEout 3 SE (SCES-02845) | yes | 90 PB/PBP archives unpacked, 146 VAG samples + 4 EXE TIMs decoded, 3090 compressed entries carved |
| Klonoa (SLUS-00585) | none (probe) | 3424 TIMs decoded from FILE.BIN |

## Known limits / next steps

- WipEout 3's compressed PB entries use a bespoke Psygnosis codec (stock LZSS
  exhaustively ruled out). The plan of record is to recover the game's own
  decompressor through the recomp and call it from host code, not to reverse
  the bitstream.
- No TMD/model decoding yet — `tmd?` scanner hits are the starting inventory.
- No XA/STR audio demux; `.STR` files are copied but not parsed.
- TIM decode is pure Python and dominates `rip` wall time on TIM-heavy discs
  (Klonoa: ~4 min). Fine for a batch tool; revisit if it becomes interactive.
