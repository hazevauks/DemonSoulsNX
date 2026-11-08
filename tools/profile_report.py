#!/usr/bin/env python3
"""The sampling profiler's windows (SD_LOG/profile/<n>.bin, DESNX_PROFILE=1) as a report.

Each sample is a thread's pc and lr, read every 2 ms from the six busiest threads. The game's ELF
names them: its symbols from nm, the NRO's addresses being the ELF's (nro+ offsets).

    python tools/profile_report.py build/demonsouls_game.elf SD_LOG/profile/3.bin [4.bin ...]
    python tools/profile_report.py --nm <path to aarch64-none-elf-nm> ...

The report: for all windows given, the functions the samples fall in (self), the functions their
lr falls in (who called), and each thread's own top functions, with its start routine's name.
"""

import argparse
import bisect
import collections
import os
import shutil
import struct
import subprocess
import sys

HEAD = struct.Struct("<8s3Q4I")
THREAD = struct.Struct("<IIQ")
SAMPLE = struct.Struct("<IIQQ")


def find_nm(given):
    if given:
        return given
    for name in ("aarch64-none-elf-nm", "aarch64-none-elf-nm.exe"):
        p = shutil.which(name)
        if p:
            return p
    for root in (os.environ.get("DEVKITPRO", ""), "/opt/devkitpro"):
        p = os.path.join(root, "devkitA64", "bin", "aarch64-none-elf-nm.exe")
        if root and os.path.exists(p):
            return p
        p = os.path.join(root, "devkitA64", "bin", "aarch64-none-elf-nm")
        if root and os.path.exists(p):
            return p
    sys.exit("aarch64-none-elf-nm not found: give it with --nm")


def symbols(nm, elf):
    out = subprocess.run([nm, "-n", "-C", elf], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def name_of(addrs, names, off):
    i = bisect.bisect_right(addrs, off) - 1
    return names[i] if i >= 0 else "?"


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, base, end, ns, window, nthreads, nsamples, _ = HEAD.unpack_from(data, 0)
    if magic != b"DSNXPRF1":
        sys.exit(f"{path}: not a profile window")
    pos = HEAD.size
    threads = []
    for _ in range(nthreads):
        tid, _, start = THREAD.unpack_from(data, pos)
        threads.append((tid, start))
        pos += THREAD.size
    samples = []
    for _ in range(nsamples):
        slot, _, pc, lr = SAMPLE.unpack_from(data, pos)
        samples.append((slot, pc, lr))
        pos += SAMPLE.size
    return base, end, ns, threads, samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("elf")
    ap.add_argument("windows", nargs="+")
    a = ap.parse_args()
    addrs, names = symbols(find_nm(a.nm), a.elf)

    def where(base, end, pc):
        return name_of(addrs, names, pc - base) if base <= pc < end else "(outside the NRO: the kernel or libraries)"

    total = 0
    seconds = 0.0
    self_all = collections.Counter()
    caller_all = collections.Counter()
    per_thread = collections.defaultdict(collections.Counter)
    thread_name = {}
    for path in a.windows:
        base, end, ns, threads, samples = read(path)
        seconds += ns / 1e9
        for slot, pc, lr in samples:
            tid, start = threads[slot] if slot < len(threads) else (0, 0)
            key = (tid, start)
            thread_name[key] = where(base, end, start)
            f = where(base, end, pc)
            self_all[f] += 1
            caller_all[where(base, end, lr)] += 1
            per_thread[key][f] += 1
            total += 1
    if not total:
        sys.exit("no samples")
    print(f"{total} samples over {seconds:.0f} s in {len(a.windows)} windows\n")
    print("Where the samples are (self):")
    for f, n in self_all.most_common(a.top):
        print(f"  {100 * n / total:5.1f}%  {f}")
    print("\nWho the samples return to (lr):")
    for f, n in caller_all.most_common(a.top):
        print(f"  {100 * n / total:5.1f}%  {f}")
    print("\nBy thread:")
    for key, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        n = sum(c.values())
        print(f"\n  thread {key[0]} ({thread_name[key]}): {100 * n / total:.1f}% of the samples")
        for f, m in c.most_common(10):
            print(f"    {100 * m / n:5.1f}%  {f}")


if __name__ == "__main__":
    main()
