#!/usr/bin/env python3
"""Sends an NRO to the console with nxlink, and keeps its log as it comes.

    python tools/nxlink_run.py [--test] [--address IP] [--set NAME=value ...] [--nro FILE]

On the console: start the homebrew menu by title takeover (hold R while starting a game), and
press Y there, for its netloader. The game's NRO goes by default, the test NRO with --test. It is
written where it lives on the SD card (switch/demonsouls/), so the menu starts the new one from
then on, and it starts at once. nxlink looks for the console with a broadcast, which many routers do
not pass between devices ("No response from Switch!"): --address then names it, as the netloader's
screen shows it.

--set gives the game a setting for this run, as a line of settings.txt would (it goes as one of
the NRO's arguments, and wins over the file): --set DESNX_CRASH_TEST=30.

What the NRO prints comes back over the network: shown here, and saved to
SD_LOG/nxlink_<date>_<name>.txt. The run ends when the NRO exits, or with Ctrl+C here. The NRO
also writes its own log to the SD card, as without nxlink, but the game's runtime then prints to
the network only.

nxlink comes with devkitPro (switch-tools): run this from devkitPro's MSYS2 shell, or set
DEVKITPRO. Windows may ask to let nxlink through its firewall: the log needs that.
"""

import argparse
import datetime
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def find_nxlink():
    found = shutil.which("nxlink")
    if found:
        return found
    devkitpro = os.environ.get("DEVKITPRO")
    for base in ([devkitpro] if devkitpro else []) + ["/opt/devkitpro", "C:/devkitPro"]:
        for name in ("nxlink", "nxlink.exe"):
            candidate = pathlib.Path(base) / "tools" / "bin" / name
            if candidate.is_file():
                return str(candidate)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test", action="store_true", help="send the test NRO instead of the game")
    parser.add_argument("--nro", type=pathlib.Path, help="the NRO to send")
    parser.add_argument("--address", "-a", help="the console's IP address, when nxlink does not find it")
    parser.add_argument("--set", action="append", default=[], metavar="NAME=value",
                        help="a setting for this run, as in settings.txt (again for more)")
    args = parser.parse_args()
    for setting in args.set:
        name = setting.split("=", 1)[0]
        if "=" not in setting or not name or not all(c.isupper() or c.isdigit() or c == "_" for c in name) \
                or " " in setting:
            raise SystemExit("--set %s: a setting is NAME=value, the name in capitals, with no spaces" % setting)

    name = "demonsouls.nro" if args.test else "demonsouls_game.nro"
    nro = args.nro or ROOT / "build" / name
    if not nro.is_file():
        raise SystemExit("%s was not found: build it first (docs/build.md)" % nro)
    nxlink = find_nxlink()
    if not nxlink:
        raise SystemExit("nxlink was not found: run this from devkitPro's MSYS2 shell, or set DEVKITPRO")

    log_dir = ROOT / "SD_LOG"
    log_dir.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = log_dir / ("nxlink_%s_%s.txt" % (stamp, nro.stem))

    command = [nxlink, "-s", "-p", "demonsouls/" + name, str(nro)]
    if args.address:
        command[1:1] = ["-a", args.address]
    if args.set:
        command[1:1] = ["--args", " ".join(args.set)]
    print("sending %s (%.1f MB) as switch/demonsouls/%s; the log goes to %s"
          % (nro.name, nro.stat().st_size / 1e6, name, log_path.relative_to(ROOT)), flush=True)

    lines = 0
    with open(log_path, "w", encoding="utf-8", newline="\n") as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            for raw in process.stdout:
                line = raw.decode("utf-8", errors="replace")
                log.write(line)
                log.flush()
                sys.stdout.write(line)
                lines += 1
            process.wait()
        except KeyboardInterrupt:
            process.terminate()
            process.wait()
            print("\nstopped here")
    print("%d lines in %s (nxlink exit code %s)" % (lines, log_path.relative_to(ROOT), process.returncode))
    return 0 if process.returncode in (0, None) else process.returncode


if __name__ == "__main__":
    sys.exit(main())
