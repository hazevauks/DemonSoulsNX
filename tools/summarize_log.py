#!/usr/bin/env python3
"""Sums up a log of the port: what a run reached, without reading all of it.

    python tools/summarize_log.py [LOG]

LOG is SD_LOG/demonsouls_log.txt by default; an nxlink log (tools/nxlink_run.py) reads the same.
For the game: its settings, the frames, sound, threads and guest memory over time, the buttons
that reached it, its files, any wait that blocked, and how the log ends. For the test NRO: each
check's own lines. And whether the log holds stray bytes, which would mean lines written over
each other.
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PERIODIC = ("frames:", "sound:", "threads:", "guest memory after")
TEST_CHECKS = ("guest probe:", "gpu:", "cpu:", "sdl:", "files:", "libs:", "system:", "memory:", "game:")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=pathlib.Path, nargs="?", default=ROOT / "SD_LOG" / "demonsouls_log.txt")
    args = parser.parse_args()

    data = args.log.read_bytes()
    stray = sum(1 for b in data if not (b in (9, 10, 13) or 32 <= b < 127))
    lines = data.decode("latin-1").splitlines()
    head = next((l for l in lines if l.startswith("DemonSoulsNX ")), lines[0] if lines else "")
    game = "the game" in head
    print("%s: %d lines; %s" % (args.log.name, len(lines), head.strip()))
    print("stray bytes: %d%s" % (stray, "" if not stray else " -- lines were written over each other"))

    def show(title, found, limit=None):
        if not found:
            return
        print("\n%s:" % title)
        for line in (found if limit is None else found[-limit:]):
            print("  " + line.strip()[:200])

    if not game:
        show("checks", [l for l in lines if l.startswith(TEST_CHECKS)])
        show("the end", lines[-5:])
        return 0

    show("settings", [l for l in lines if l.startswith("settings:")])
    stop = [l for l in lines if l.startswith("game: cannot start")]
    show("could not start", stop)
    for prefix in PERIODIC:
        found = [l for l in lines if prefix in l]
        if found:
            print("\n%s (%d times; the first and the last three):" % (prefix.rstrip(":"), len(found)))
            for line in ([found[0]] + found[-3:] if len(found) > 4 else found):
                print("  " + line[line.index(prefix):].strip()[:200])
    show("controls", [l for l in lines if l.startswith("controls:")])

    opens = [l for l in lines if "[fs] open '" in l]
    fails = [l for l in lines if "[fs] open FAIL" in l]
    print("\nfiles: %d opened, %d not found (the game's cache, on a first start, is most of these)"
          % (len(opens), len(fails)))
    show("the last files opened", [re.sub(r" -> fd \d+", "", l) for l in opens], limit=5)

    blocked = [l for l in lines if "EventFlagWait BLOCKED" in l and " 1s " in l]
    show("waits that blocked a second", blocked)
    if any("[spurs-trace]" in l for l in lines):
        print("\nthe SPURS trace is in the log ([spurs-trace] lines)")
    show("save data", [l for l in lines if "[cellSaveData] SAVE complete" in l or "AutoLoad" in l])
    show("movies", [l for l in lines if "[cellSail] CreateDescriptor" in l])
    show("the end", [l for l in lines if not any(p in l for p in PERIODIC)], limit=8)
    return 0


if __name__ == "__main__":
    sys.exit(main())
