#!/usr/bin/env python3
"""Reports what is in the game's folder: formats, containers, sizes, and the limits that
matter on the Switch's SD card.

    python tools/scan_assets.py game_data/disc [--json report.json]

It reads each file's first bytes to name its format, and looks inside DCX files to tell
which compression they use. It writes nothing into the game's folder.
"""

import argparse
import collections
import json
import pathlib
import struct
import sys

# the first bytes of each format the game uses
MAGIC = [
    (b"DCX\0", "DCX (compressed wrapper)"),
    (b"DCP\0", "DCP (compressed wrapper, old)"),
    (b"BND3", "BND3 (archive)"),
    (b"BND4", "BND4 (archive)"),
    (b"BHF3", "BHF3 (archive header)"),
    (b"BDF3", "BDF3 (archive data)"),
    (b"TPF\0", "TPF (texture pack)"),
    (b"FLVER\0", "FLVER (model)"),
    (b"MSB ", "MSB (map layout)"),
    (b"DRB\0", "DRB (menu layout)"),
    (b"FSB4", "FSB4 (FMOD sound bank)"),
    (b"FEV1", "FEV1 (FMOD event file)"),
    (b"MOSB", "MOSB (sound bank)"),
    (b"PAMF", "PAMF (movie)"),
    (b"\x00PSF", "SFO (system parameters)"),
    (b"\x89PNG", "PNG"),
    (b"SCE\0", "SCE (signed or encrypted executable)"),
    (b"NPD\0", "NPD (SDATA/EDAT, encrypted)"),
    (b"\x1bLua", "Lua bytecode"),
    (b"fsSL", "ESD (event script)"),
    (b"ESD\0", "ESD (event script)"),
    (b"DDS ", "DDS (texture)"),
    (b"GFX", "Scaleform GFX"),
    (b"FWS", "Flash SWF"),
    (b"CFX", "Scaleform CFX"),
    (b"PARAM", "PARAMDEF"),
    (b"MTD ", "MTD (material)"),
    (b"\0\0\0\x01", "big-endian header (unknown)"),
]

FAT_NAME_LIMIT = 255
HORIZON_PATH_LIMIT = 0x300  # FS_MAX_PATH - 1


def identify(head):
    for magic, name in MAGIC:
        if head.startswith(magic):
            return name
    if head[4:8] == b"MTD ":
        return "MTD (material)"
    if head[:4] == b"\0\0\0\0" and head[4:8] == b"\0\0\0\0":
        return "zeros at the start (raw data)"
    return "other"


def dcx_compression(path, head):
    """Returns the compression named inside a DCX header (DFLT, EDGE, KRAK...)."""
    if not head.startswith(b"DCX\0"):
        return None
    with open(path, "rb") as f:
        data = f.read(0x60)
    for offset in range(0x18, 0x40, 4):
        tag = data[offset:offset + 4]
        if tag in (b"DFLT", b"EDGE", b"KRAK", b"ZSTD"):
            return tag.decode()
    return "?"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path)
    args = parser.parse_args()

    usrdir = args.root / "PS3_GAME" / "USRDIR"
    if not usrdir.is_dir():
        raise SystemExit("%s has no PS3_GAME/USRDIR" % args.root)

    by_top = collections.defaultdict(lambda: [0, 0])
    by_ext = collections.defaultdict(lambda: [0, 0])
    by_format = collections.defaultdict(lambda: [0, 0])
    compression = collections.Counter()
    longest_name, longest_path, upper = "", "", []
    folded = {}
    clashes = []

    files = sorted(p for p in usrdir.rglob("*") if p.is_file())
    for path in files:
        relative = path.relative_to(args.root).as_posix()
        inside = path.relative_to(usrdir).as_posix()
        size = path.stat().st_size
        top = inside.split("/")[0] if "/" in inside else "(USRDIR)"
        suffix = "".join(path.suffixes[-2:]) if path.suffix == ".dcx" else path.suffix
        with open(path, "rb") as f:
            head = f.read(16)
        kind = identify(head)
        for table, key in ((by_top, top), (by_ext, suffix or "(none)"), (by_format, kind)):
            table[key][0] += 1
            table[key][1] += size
        mode = dcx_compression(path, head)
        if mode:
            compression[mode] += 1
        if len(path.name) > len(longest_name):
            longest_name = path.name
        if len(relative) > len(longest_path):
            longest_path = relative
        if inside != inside.lower():
            upper.append(inside)
        other = folded.setdefault(inside.lower(), inside)
        if other != inside:
            clashes.append((other, inside))

    def table(title, rows):
        print("\n%s" % title)
        print("%8s %10s  %s" % ("files", "MB", "name"))
        for key, (count, size) in sorted(rows.items(), key=lambda item: -item[1][1]):
            print("%8d %10.1f  %s" % (count, size / 1e6, key))

    total = sum(c for c, _ in by_top.values()), sum(s for _, s in by_top.values())
    print("USRDIR: %d files, %.2f GB" % (total[0], total[1] / 1e9))
    table("By folder", by_top)
    table("By extension", by_ext)
    table("By format (first bytes)", by_format)
    print("\nDCX compression: %s" % ", ".join("%s %d" % item for item in compression.most_common()))
    print("Longest name: %d characters (FAT limit %d)" % (len(longest_name), FAT_NAME_LIMIT))
    print("Longest path from the game's folder: %d characters (Horizon limit %d)" % (len(longest_path), HORIZON_PATH_LIMIT))
    print("Names with capitals: %d%s" % (len(upper), (", e.g. " + ", ".join(upper[:5])) if upper else ""))
    print("Names that differ only in case: %d" % len(clashes))

    if args.json:
        args.json.write_text(json.dumps({
            "files": total[0], "bytes": total[1],
            "folders": by_top, "extensions": by_ext, "formats": by_format,
            "dcx": compression, "longest_name": len(longest_name), "longest_path": len(longest_path),
            "capitals": upper, "case_clashes": clashes,
        }, indent=1) + "\n")
    return 1 if clashes or len(longest_name) > FAT_NAME_LIMIT else 0


if __name__ == "__main__":
    sys.exit(main())
