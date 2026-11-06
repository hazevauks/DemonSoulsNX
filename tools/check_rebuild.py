#!/usr/bin/env python3
"""Says what the next game build would compile again because a command line changed.

    python tools/check_rebuild.py [--build build] [--target demonsouls_game_nro] [--ninja ninja]

The lifted chunks take hours to compile on a laptop, and ninja compiles an object again when its
command line changes: a new flag, an include folder a library brings along, a definition. Run this
after changing the build and before building the game: it compares every object's command now
with the one in ninja's log (.ninja_log, hashed as ninja hashes it) and counts what would compile
again, by kind. Objects that are missing or older than their sources are not counted: ninja's own
dry run shows those, once the build files are up to date. Exits with 1 when a lifted chunk would
compile again. Needs the devkitPro shell's ninja, or --ninja with its path.
"""

import argparse
import collections
import pathlib
import re
import subprocess
import sys

MASK = (1 << 64) - 1


def murmur64a(data, seed=0xDECAFBADDECAFBAD):
    """ninja's command hash up to log version 6."""
    m = 0xC6A4A7935BD1E995
    h = (seed ^ (len(data) * m)) & MASK
    n = len(data) // 8 * 8
    for i in range(0, n, 8):
        k = int.from_bytes(data[i:i + 8], "little")
        k = (k * m) & MASK
        k ^= k >> 47
        k = (k * m) & MASK
        h ^= k
        h = (h * m) & MASK
    if data[n:]:
        h ^= int.from_bytes(data[n:], "little")
        h = (h * m) & MASK
    h ^= h >> 47
    h = (h * m) & MASK
    h ^= h >> 47
    return h


SECRET = (0x2D358DCCAA6C78A5, 0x8BB84B93962EACC9, 0x4B33A62ED433D4A3)


def _mix(a, b):
    r = a * b
    return (r & MASK) ^ (r >> 64)


def _r64(p, i):
    return int.from_bytes(p[i:i + 8], "little")


def _r32(p, i):
    return int.from_bytes(p[i:i + 4], "little")


def rapidhash(p, seed=0xBDD89AA982704029):
    """ninja's command hash from log version 7 (ninja 1.12)."""
    s = SECRET
    n = len(p)
    seed ^= _mix(seed ^ s[0], s[1]) ^ n
    if n <= 16:
        if n >= 4:
            last = n - 4
            delta = (n & 24) >> (n >> 3)
            a = (_r32(p, 0) << 32) | _r32(p, last)
            b = (_r32(p, delta) << 32) | _r32(p, last - delta)
        elif n > 0:
            a, b = (p[0] << 56) | (p[n >> 1] << 32) | p[n - 1], 0
        else:
            a = b = 0
    else:
        i, o = n, 0
        if i > 48:
            see1 = see2 = seed
            while i >= 48:
                seed = _mix(_r64(p, o) ^ s[0], _r64(p, o + 8) ^ seed)
                see1 = _mix(_r64(p, o + 16) ^ s[1], _r64(p, o + 24) ^ see1)
                see2 = _mix(_r64(p, o + 32) ^ s[2], _r64(p, o + 40) ^ see2)
                o += 48
                i -= 48
            seed ^= see1 ^ see2
        if i > 16:
            seed = _mix(_r64(p, o) ^ s[2], _r64(p, o + 8) ^ seed ^ s[1])
            if i > 32:
                seed = _mix(_r64(p, o + 16) ^ s[2], _r64(p, o + 24) ^ seed)
        a, b = _r64(p, o + i - 16), _r64(p, o + i - 8)
    a ^= s[1]
    b ^= seed
    r = a * b
    a, b = r & MASK, r >> 64
    return _mix(a ^ s[0] ^ n, b ^ s[1])


def kind(obj):
    if "/work/recomp/" in obj:
        return "lifted PPU chunks"
    if "/work/spu/" in obj:
        return "lifted SPU programs"
    if "/work/libsre/" in obj:
        return "lifted libsre"
    return "the port, the runtime and the host side"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=pathlib.Path, default=pathlib.Path("build"))
    parser.add_argument("--target", default="demonsouls_game_nro")
    parser.add_argument("--ninja", default="ninja")
    args = parser.parse_args()

    log = args.build / ".ninja_log"
    if not log.is_file():
        raise SystemExit("%s was not found: nothing was built in %s yet" % (log, args.build))
    logged = {}
    with open(log, encoding="utf-8") as f:
        version = f.readline().strip()
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 5:
                logged[parts[3]] = int(parts[4], 16)    # the last entry for an output wins
    hash_of = rapidhash if version.endswith(("v7", "v8")) else murmur64a

    try:
        out = subprocess.run([args.ninja, "-C", str(args.build), "-t", "commands", args.target],
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise SystemExit("ninja -t commands failed (%s): run this from the devkitPro shell, or pass --ninja" % e)

    counts = collections.defaultdict(lambda: [0, 0])
    changed = []
    for command in out.splitlines():
        m = re.search(r" -o (\S+\.o)\b", command)
        if not m:
            continue
        obj = m.group(1)
        if obj not in logged:
            continue
        same = logged[obj] == hash_of(command.encode())
        counts[kind(obj)][0 if same else 1] += 1
        if not same:
            changed.append(obj)

    if not counts:
        raise SystemExit("no object of %s is in ninja's log yet: build it once first" % args.target)
    for name, (same, diff) in sorted(counts.items()):
        print("%-42s %5d unchanged, %d would compile again" % (name + ":", same, diff))
    for obj in changed[:20]:
        print("  " + obj)
    if len(changed) > 20:
        print("  ... and %d more" % (len(changed) - 20))
    return 1 if counts["lifted PPU chunks"][1] else 0


if __name__ == "__main__":
    sys.exit(main())
