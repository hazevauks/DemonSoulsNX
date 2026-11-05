#!/usr/bin/env python3
"""Opens every compressed file on the disc the way the game does, and checks it.

Each DCX file is EDGE compressed: 64 KB blocks of raw deflate, listed in an EgdT table. The
older DCP files hold one zlib stream. This inflates them all, checks the sizes the headers give,
names what comes out (BND3, TPF, FLVER...), and checks that BND3 archives' headers fit their files.

The disc has 3,362 files both raw and compressed. For each pair this also compares the inflated
copy with the raw one, byte for byte: if they are the same, the SD card needs only one of them.

Usage: python tools/check_archives.py [game_data/disc] [--json report.json]
"""

import argparse
import collections
import hashlib
import json
import pathlib
import struct
import sys
import zlib

BLOCK = 0x10000


class Bad(Exception):
    pass


def u32(data, at):
    return struct.unpack_from(">I", data, at)[0]


def expect(data, at, value, what):
    if isinstance(value, bytes):
        if data[at:at + len(value)] != value:
            raise Bad("%s: %r at %#x" % (what, data[at:at + len(value)], at))
    elif u32(data, at) != value:
        raise Bad("%s: %#x at %#x, not %#x" % (what, u32(data, at), at, value))


def edge_blocks(data, table, count, base, size, last):
    """Inflates `count` EgdT entries (16 bytes each from `table`), whose data starts at `base`."""
    out = bytearray()
    for i in range(count):
        entry = table + i * 16
        expect(data, entry, 0, "block %d" % i)
        offset, length, packed = u32(data, entry + 4), u32(data, entry + 8), u32(data, entry + 12)
        if packed not in (0, 1):
            raise Bad("block %d: compressed flag %d" % (i, packed))
        chunk = data[base + offset:base + offset + length]
        if len(chunk) != length:
            raise Bad("block %d: past the end of the file" % i)
        block = zlib.decompress(chunk, -15) if packed else chunk
        want = last if i == count - 1 else BLOCK
        if len(block) != want:
            raise Bad("block %d: %d bytes, not %d" % (i, len(block), want))
        out += block
    if len(out) != size:
        raise Bad("%d bytes, not %d" % (len(out), size))
    return bytes(out)


def inflate_dcx(data):
    """DCX: header, DCS sizes, DCP/EDGE parameters, then DCA with the EgdT block table."""
    expect(data, 0x00, b"DCX\0", "magic")
    expect(data, 0x04, 0x10000, "version")
    expect(data, 0x24, b"DCP\0EDGE", "compression")
    size = u32(data, 0x1C)
    expect(data, 0x44, b"DCA\0", "DCA")
    expect(data, 0x4C, b"EgdT", "EgdT")
    expect(data, 0x5C, BLOCK, "block size")
    last, count = u32(data, 0x60), u32(data, 0x68)
    expect(data, 0x64, 0x24 + count * 0x10, "table size")
    return edge_blocks(data, 0x70, count, 0x44 + u32(data, 0x48), size, last)


def inflate_dcp(data):
    """DCP: the older wrapper. On this disc it holds one zlib stream (DFLT), not EDGE blocks."""
    expect(data, 0x00, b"DCP\0DFLT", "magic")
    expect(data, 0x20, b"DCS\0", "DCS")
    size, packed = u32(data, 0x24), u32(data, 0x28)
    expect(data, 0x2C + packed, b"DCA\0", "DCA")
    out = zlib.decompress(data[0x2C:0x2C + packed])
    if len(out) != size:
        raise Bad("%d bytes, not %d" % (len(out), size))
    return out


def check_bnd3(data):
    """BND3: the file count and the end of the file headers must fit the file."""
    big = data[0x0D] != 0 or data[0x0C] & 1 == 0
    count, end = struct.unpack_from(">II" if big else "<II", data, 0x10)
    if count == 0 and end == 0:
        return 0    # a header that lists nothing: a few unused archives on the disc
    if not 0 < count < 100000 or not 0x20 <= end <= len(data):
        raise Bad("BND3 header: %d files, headers end at %#x of %#x" % (count, end, len(data)))
    return count


KINDS = [(b"BND3", "BND3"), (b"TPF\0", "TPF"), (b"FLVER\0", "FLVER"), (b"DCX\0", "DCX"),
         (b"\x1bLua", "Lua"), (b"PARAM", "param"), (b"\x57\xe0\xe0\x57", "Havok")]


def kind(data):
    for magic, name in KINDS:
        if data.startswith(magic):
            return name
    return "other"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("disc", nargs="?", default="game_data/disc")
    parser.add_argument("--json", help="write the counts to this file")
    args = parser.parse_args()

    usrdir = pathlib.Path(args.disc) / "PS3_GAME" / "USRDIR"
    if not usrdir.is_dir():
        sys.exit("no %s: extract the disc first (tools/extract_game.py)" % usrdir)

    kinds, bad, different = collections.Counter(), [], []
    archives = bnd3_files = bnd3_empty = packed_bytes = inflated_bytes = 0
    same = differ = same_bytes = 0
    for path in sorted(p for p in usrdir.rglob("*") if p.is_file()):
        with open(path, "rb") as f:
            head = f.read(8)
        if not (head.startswith(b"DCX\0") or head.startswith(b"DCP\0")):
            continue
        data = path.read_bytes()
        name = path.relative_to(usrdir).as_posix()
        try:
            out = inflate_dcx(data) if head.startswith(b"DCX\0") else inflate_dcp(data)
            what = kind(out)
            if what == "BND3":
                listed = check_bnd3(out)
                bnd3_files += listed
                bnd3_empty += listed == 0
        except (Bad, zlib.error, struct.error) as error:
            bad.append("%s: %s" % (name, error))
            continue
        archives += 1
        packed_bytes += len(data)
        inflated_bytes += len(out)
        kinds[what] += 1

        raw = path.with_suffix("") if path.suffix == ".dcx" else None
        if raw and raw.is_file():
            if raw.stat().st_size == len(out) and hashlib.sha1(raw.read_bytes()).digest() == hashlib.sha1(out).digest():
                same += 1
                same_bytes += len(out)
            else:
                differ += 1
                different.append(name)

    print("%d compressed files inflated: %.0f MB to %.0f MB" % (archives, packed_bytes / 1e6, inflated_bytes / 1e6))
    print("inside: " + ", ".join("%s %d" % item for item in kinds.most_common()))
    print("BND3 archives list %d files; %d archives list none" % (bnd3_files, bnd3_empty))
    print("raw copies: %d the same as their compressed file (%.2f GB), %d different" % (same, same_bytes / 1e9, differ))
    for name in different:
        print("  different: %s and its raw copy" % name)
    for line in bad[:20]:
        print("ERROR: " + line)
    if len(bad) > 20:
        print("... and %d more" % (len(bad) - 20))
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps({
            "inflated": archives, "packed_bytes": packed_bytes, "inflated_bytes": inflated_bytes,
            "kinds": kinds, "bnd3_files": bnd3_files, "bnd3_empty": bnd3_empty, "raw_same": same,
            "raw_same_bytes": same_bytes, "raw_different": different, "errors": bad}, indent=2))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
