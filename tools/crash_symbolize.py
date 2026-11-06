#!/usr/bin/env python3
"""Names the addresses of a crash report, from the NRO's ELF.

    python tools/crash_symbolize.py [REPORT] [--elf FILE]

REPORT is SD_LOG/demonsouls_crash.txt by default (src/platform/nx_crash.c writes it on the
console). Each "nro+0x..." in it is an address in the NRO's ELF; this prints the report again with
the function each one falls in after it. The ELF is build/demonsouls_game.elf for the game's
report and build/demonsouls.elf for the test NRO's, which must be the build the console ran.
In the game, a lifted function is named func_<its PS3 address>, so "func_00B5D8D8+0x16c" is the
PS3 code at 0x00B5D8D8, the HLE and runtime functions by their own names.

nm comes with devkitPro: run this from its MSYS2 shell, or set DEVKITPRO.
"""

import argparse
import bisect
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OFFSET = re.compile(r"nro\+0x([0-9a-fA-F]+)")


def find_nm():
    for name in ("aarch64-none-elf-nm", "aarch64-none-elf-nm.exe"):
        found = shutil.which(name)
        if found:
            return found
    devkitpro = os.environ.get("DEVKITPRO")
    for base in ([devkitpro] if devkitpro else []) + ["/opt/devkitpro", "C:/devkitPro"]:
        for name in ("aarch64-none-elf-nm", "aarch64-none-elf-nm.exe"):
            candidate = pathlib.Path(base) / "devkitA64" / "bin" / name
            if candidate.is_file():
                return str(candidate)
    return None


def load_symbols(nm, elf):
    """The ELF's code symbols, sorted by address: (addresses, names)."""
    out = subprocess.run([nm, "-n", "--defined-only", str(elf)], capture_output=True, text=True, check=True).stdout
    addresses, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TtWw":
            addresses.append(int(parts[0], 16))
            names.append(parts[2])
    return addresses, names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=pathlib.Path, nargs="?", default=ROOT / "SD_LOG" / "demonsouls_crash.txt")
    parser.add_argument("--elf", type=pathlib.Path)
    args = parser.parse_args()

    text = args.report.read_text(encoding="latin-1")
    elf = args.elf or ROOT / "build" / ("demonsouls_game.elf" if "the game" in text.splitlines()[0] else "demonsouls.elf")
    if not elf.is_file():
        raise SystemExit("%s was not found: it must be the build the console ran" % elf)
    nm = find_nm()
    if not nm:
        raise SystemExit("nm was not found: run this from devkitPro's MSYS2 shell, or set DEVKITPRO")
    addresses, names = load_symbols(nm, elf)
    print("%s, named from %s (%d functions)\n" % (args.report.name, elf.relative_to(ROOT) if elf.is_relative_to(ROOT) else elf,
                                                 len(addresses)))

    def name_of(offset):
        i = bisect.bisect_right(addresses, offset) - 1
        if i < 0:
            return "?"
        return "%s+0x%x" % (names[i], offset - addresses[i])

    for line in text.splitlines():
        found = OFFSET.findall(line)
        if found:
            line += "  " + ", ".join(name_of(int(f, 16)) for f in found)
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
