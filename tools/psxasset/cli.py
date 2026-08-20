"""psxasset command line.

    ls        list the ISO9660 filesystem of a disc image
    extract   copy files out of a disc image
    unpack    unpack a container file (registry- or probe-detected)
    scan      signature-scan a file or directory, optionally carving hits
    tim2png   decode TIM(s) in a blob to PNG
    vag2wav   decode a VAG (headered, headerless, or raw+--rate) to WAV
    rip       the whole pipeline: extract -> unpack -> scan -> decode + report
"""

from __future__ import annotations

import argparse
import json
import sys
import wave
from pathlib import Path

from . import containers, registry, scan as scanner
from .formats import tim, vag
from .iso import DiscImage, IsoFs
from .pngout import write_png


def _open_fs(disc_path: Path) -> IsoFs:
    return IsoFs(DiscImage(disc_path))


def _safe_name(iso_path: str) -> Path:
    return Path(*[p for p in iso_path.strip("/").split("/") if p not in ("", "..")])


def _write_wav(path: Path, rate: int, samples: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(vag.pcm_bytes(samples))


def _decode_tims_in(data: bytes, out_dir: Path, stem: str,
                    all_palettes: bool = False,
                    findings: list[dict] | None = None) -> list[dict]:
    written = []
    for f in (findings if findings is not None else scanner.scan(data)):
        if f["type"] != "tim":
            continue
        img = tim.parse(data, f["offset"])
        variants = range(img.palette_count) if (all_palettes and img.palette_count > 1) else [0]
        for pal in variants:
            suffix = f"_p{pal}" if len(list(variants)) > 1 or pal else ""
            name = f"{stem}_{f['offset']:06x}_{img.width}x{img.height}_{img.bpp}bpp{suffix}.png"
            out = out_dir / name
            out.parent.mkdir(parents=True, exist_ok=True)
            write_png(out, img.width, img.height, tim.to_rgba(img, pal))
            written.append({**f, "png": str(out), "palette": pal})
    return written


def _decode_vags_in(data: bytes, out_dir: Path, stem: str,
                    findings: list[dict] | None = None) -> list[dict]:
    written = []
    for f in (findings if findings is not None else scanner.scan(data)):
        if f["type"] not in ("vag", "vag_headerless"):
            continue
        info = vag.probe(data, f["offset"])
        if info is None:
            continue
        samples = vag.decode(data[info.data_off:info.data_off + info.data_size])
        if not samples:
            continue
        label = info.name.replace("/", "_") if info.name else f"{f['offset']:06x}"
        out = out_dir / f"{stem}_{label}_{info.rate}hz.wav"
        _write_wav(out, info.rate, samples)
        written.append({**f, "wav": str(out), "samples": len(samples)})
    return written


# --- subcommands -----------------------------------------------------------

def cmd_ls(args) -> int:
    fs = _open_fs(args.disc)
    serial = fs.serial()
    print(f"volume={fs.volume_id}  serial={serial or '?'}")
    for e in fs.files():
        print(f"  {e.size:>10}  lba={e.lba:<8}  {e.path}")
    return 0


def cmd_extract(args) -> int:
    fs = _open_fs(args.disc)
    wanted = [w.upper().strip("/") for w in args.paths]
    out: Path = args.out
    count = 0
    for e in fs.files():
        rel = e.path.strip("/").upper()
        if wanted and not any(rel == w or rel.startswith(w + "/") for w in wanted):
            continue
        dest = out / _safe_name(e.path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(fs.read(e))
        count += 1
        print(f"  {e.path} -> {dest}")
    print(f"{count} file(s) extracted")
    return 0 if count else 1


def _resolve_format(path: Path, data: bytes, explicit: str | None,
                    reg: registry.GameRegistry | None, iso_path: str | None) -> str | None:
    if explicit:
        return explicit
    if reg and iso_path:
        fmt = reg.container_format(iso_path)
        if fmt:
            return fmt
    return containers.probe_all(data)


def cmd_unpack(args) -> int:
    data = args.file.read_bytes()
    fmt = _resolve_format(args.file, data, args.format, None, None)
    if fmt is None:
        print(f"no container handler recognizes {args.file}", file=sys.stderr)
        return 1
    handler = containers.get_handler(fmt)
    entries = handler.unpack(data)
    out: Path = args.out or args.file.with_suffix(args.file.suffix + ".d")
    out.mkdir(parents=True, exist_ok=True)
    stored = 0
    for e in entries:
        if e.stored:
            (out / f"{e.index:03d}.bin").write_bytes(e.data)
            stored += 1
        elif args.raw:
            (out / f"{e.index:03d}.{fmt}.packed").write_bytes(e.packed_data)
    print(f"{args.file.name}: {fmt}, {len(entries)} entries, "
          f"{stored} stored -> {out}")
    return 0


def cmd_scan(args) -> int:
    targets = ([p for p in args.target.rglob("*") if p.is_file()]
               if args.target.is_dir() else [args.target])
    total = 0
    for path in sorted(targets):
        data = path.read_bytes()
        findings = scanner.scan(data)
        if not findings:
            continue
        total += len(findings)
        print(f"{path}:")
        for f in findings:
            extra = {k: v for k, v in f.items() if k not in ("type", "offset")}
            print(f"  +{f['offset']:#08x}  {f['type']:<14} {extra or ''}")
        if args.extract:
            out = args.out or path.parent
            _decode_tims_in(data, out, path.stem, all_palettes=args.all_palettes)
            _decode_vags_in(data, out, path.stem)
    print(f"{total} finding(s)")
    return 0


def cmd_tim2png(args) -> int:
    data = args.file.read_bytes()
    out = args.out or args.file.parent
    written = _decode_tims_in(data, out, args.file.stem, all_palettes=args.all_palettes)
    for w in written:
        print(f"  {w['png']}")
    if not written:
        print("no valid TIM found", file=sys.stderr)
        return 1
    return 0


def cmd_vag2wav(args) -> int:
    data = args.file.read_bytes()
    info = vag.probe(data)
    if info:
        samples = vag.decode(data[info.data_off:info.data_off + info.data_size])
        rate = args.rate or info.rate
    elif args.rate:
        samples = vag.decode(data)
        rate = args.rate
    else:
        print("no VAG header; pass --rate for raw ADPCM", file=sys.stderr)
        return 1
    out = args.out or args.file.with_suffix(".wav")
    _write_wav(out, rate, samples)
    print(f"  {out}  ({len(samples)} samples @ {rate} Hz)")
    return 0


def cmd_rip(args) -> int:
    fs = _open_fs(args.disc)
    serial = fs.serial()
    reg = registry.find(serial, args.registry_dir)
    out: Path = args.out
    print(f"disc: volume={fs.volume_id} serial={serial or '?'} "
          f"registry={'yes (' + reg.name + ')' if reg else 'none (probe mode)'}")

    report: dict = {"serial": serial, "volume": fs.volume_id,
                    "registry": str(reg.source) if reg else None,
                    "files": [], "decoded": {"tim": 0, "vag": 0},
                    "packed_entries": 0}

    files_dir = out / "files"
    unpacked_dir = out / "unpacked"
    decoded_dir = out / "decoded"

    for e in fs.files():
        frec: dict = {"path": e.path, "size": e.size}
        report["files"].append(frec)
        skip = reg.skip_reason(e.path) if reg else None
        if skip:
            frec["skipped"] = skip
            print(f"  skip  {e.path}  ({skip})")
            continue
        data = fs.read(e)
        dest = files_dir / _safe_name(e.path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)

        fmt = _resolve_format(dest, data, None, reg, e.path)
        stem = "_".join(_safe_name(e.path).parts)
        if fmt:
            handler = containers.get_handler(fmt)
            entries = handler.unpack(data)
            frec["container"] = fmt
            frec["entries"] = len(entries)
            edir = unpacked_dir / _safe_name(e.path)
            edir.mkdir(parents=True, exist_ok=True)
            stored = packed = 0
            for ce in entries:
                if ce.stored:
                    stored += 1
                    blob_name = f"{ce.index:03d}.bin"
                    (edir / blob_name).write_bytes(ce.data)
                    found = scanner.scan(ce.data)
                    tims = _decode_tims_in(ce.data, decoded_dir / "textures",
                                           f"{stem}_{ce.index:03d}",
                                           all_palettes=args.all_palettes,
                                           findings=found)
                    vags = _decode_vags_in(ce.data, decoded_dir / "audio",
                                           f"{stem}_{ce.index:03d}",
                                           findings=found)
                    report["decoded"]["tim"] += len(tims)
                    report["decoded"]["vag"] += len(vags)
                else:
                    packed += 1
                    if args.keep_packed:
                        (edir / f"{ce.index:03d}.packed").write_bytes(ce.packed_data)
            report["packed_entries"] += packed
            frec["stored"] = stored
            frec["packed"] = packed
            print(f"  unpack {e.path}: {len(entries)} entries "
                  f"({stored} stored, {packed} packed)")
        else:
            found = scanner.scan(data)
            tims = _decode_tims_in(data, decoded_dir / "textures", stem,
                                   all_palettes=args.all_palettes,
                                   findings=found)
            vags = _decode_vags_in(data, decoded_dir / "audio", stem,
                                   findings=found)
            report["decoded"]["tim"] += len(tims)
            report["decoded"]["vag"] += len(vags)
            note = ""
            if tims or vags:
                note = f"  [{len(tims)} tim, {len(vags)} vag]"
            print(f"  copy  {e.path}{note}")

    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps(report, indent=2))
    print(f"\ndecoded: {report['decoded']['tim']} textures, "
          f"{report['decoded']['vag']} audio samples; "
          f"{report['packed_entries']} entries still packed (custom codec)")
    print(f"report: {out / 'report.json'}")
    return 0


# --- argument wiring -------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="psxasset", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("ls", help="list the disc filesystem")
    p.add_argument("disc", type=Path)
    p.set_defaults(fn=cmd_ls)

    p = sub.add_parser("extract", help="copy files out of a disc image")
    p.add_argument("disc", type=Path)
    p.add_argument("paths", nargs="*", help="ISO paths/prefixes (default: all)")
    p.add_argument("-o", "--out", type=Path, default=Path("extracted"))
    p.set_defaults(fn=cmd_extract)

    p = sub.add_parser("unpack", help="unpack a container file")
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--out", type=Path)
    p.add_argument("--format", help="container handler (default: probe)")
    p.add_argument("--raw", action="store_true",
                   help="also write compressed entries as .packed blobs")
    p.set_defaults(fn=cmd_unpack)

    p = sub.add_parser("scan", help="signature-scan a file or directory")
    p.add_argument("target", type=Path)
    p.add_argument("--extract", action="store_true", help="decode hits to PNG/WAV")
    p.add_argument("-o", "--out", type=Path)
    p.add_argument("--all-palettes", action="store_true")
    p.set_defaults(fn=cmd_scan)

    p = sub.add_parser("tim2png", help="decode TIM(s) in a blob")
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--out", type=Path)
    p.add_argument("--all-palettes", action="store_true")
    p.set_defaults(fn=cmd_tim2png)

    p = sub.add_parser("vag2wav", help="decode a VAG to WAV")
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--out", type=Path)
    p.add_argument("--rate", type=int, help="sample rate for headerless raw ADPCM")
    p.set_defaults(fn=cmd_vag2wav)

    p = sub.add_parser("rip", help="full pipeline: extract, unpack, scan, decode")
    p.add_argument("disc", type=Path)
    p.add_argument("-o", "--out", type=Path, required=True)
    p.add_argument("--registry-dir", type=Path,
                   help="extra directory of game registry TOMLs")
    p.add_argument("--keep-packed", action="store_true",
                   help="carve compressed entries as .packed blobs")
    p.add_argument("--all-palettes", action="store_true",
                   help="export every CLUT variant of paletted textures")
    p.set_defaults(fn=cmd_rip)

    args = ap.parse_args(argv)
    return args.fn(args)
