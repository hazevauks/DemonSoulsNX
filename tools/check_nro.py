#!/usr/bin/env python3
"""Reads an NRO's name, author, version and icon back from the file, and fails if they are
not what the port ships with.

    python tools/check_nro.py build/demonsouls.nro
"""

import argparse
import pathlib
import struct
import sys

EXPECTED_NAME = "Demon's Souls"
EXPECTED_AUTHOR = "hazevauks"
ICON_SIZE = (256, 256)


def jpeg_size(data):
    """Returns (width, height) from a JPEG's frame header, or None."""
    if data[:2] != b"\xff\xd8":
        return None
    offset = 2
    while offset + 9 < len(data):
        if data[offset] != 0xFF:
            return None
        marker = data[offset + 1]
        length = struct.unpack(">H", data[offset + 2:offset + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            height, width = struct.unpack(">HH", data[offset + 5:offset + 9])
            return width, height
        offset += 2 + length
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("nro", type=pathlib.Path)
    args = parser.parse_args()

    data = args.nro.read_bytes()
    if data[0x10:0x14] != b"NRO0":
        raise SystemExit("%s is not an NRO" % args.nro)
    assets = struct.unpack("<I", data[0x18:0x1C])[0]
    if data[assets:assets + 4] != b"ASET":
        raise SystemExit("the NRO has no icon or NACP")
    icon_offset, icon_size, nacp_offset, nacp_size = struct.unpack("<QQQQ", data[assets + 8:assets + 40])
    icon = data[assets + icon_offset:assets + icon_offset + icon_size]
    nacp = data[assets + nacp_offset:assets + nacp_offset + nacp_size]

    name = nacp[0:0x200].split(b"\0")[0].decode("utf-8")
    author = nacp[0x200:0x300].split(b"\0")[0].decode("utf-8")
    version = nacp[0x3060:0x3070].split(b"\0")[0].decode("utf-8")
    size = jpeg_size(icon)
    print("%s: %s, by %s, version %s, icon %s, %.1f MB" % (
        args.nro.name, name, author, version, "%dx%d JPEG" % size if size else "not a JPEG", len(data) / 1e6))

    problems = []
    if name != EXPECTED_NAME:
        problems.append("the name is %r, not %r" % (name, EXPECTED_NAME))
    if author != EXPECTED_AUTHOR:
        problems.append("the author is %r, not %r" % (author, EXPECTED_AUTHOR))
    if size != ICON_SIZE:
        problems.append("the icon is not a %dx%d JPEG" % ICON_SIZE)
    for problem in problems:
        print("ERROR: " + problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
