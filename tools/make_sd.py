#!/usr/bin/env python3
"""Fills SD_CARD/ with a test build: copy its contents to the root of the SD card.

    python tools/make_sd.py [--nro build/demonsouls.nro] [--game game_data/disc] [--no-game]

build/demonsouls_game.nro, the game, comes along as demonsouls_game.nro when it has been built.

The game's files are linked, not copied, when they are on the same drive, so the folder
costs no extra space. The disc's EBOOT.BIN is left out: it is encrypted, and the console
never runs it (the game's code is recompiled into the NRO). What tools/check_inputs.py put in
place comes along: EBOOT.elf and the .sdat.unedat files inside the game's folder, and
game_data/firmware/libsre.prx.
"""

import argparse
import os
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
APP = pathlib.Path("switch") / "demonsouls"
LEAVE_OUT = {"PS3_GAME/USRDIR/EBOOT.BIN", "extract_manifest.json"}


def place(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        if target.stat().st_size == source.stat().st_size and target.stat().st_mtime >= source.stat().st_mtime:
            return False
        target.unlink()
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nro", type=pathlib.Path, default=ROOT / "build" / "demonsouls.nro")
    parser.add_argument("--game", type=pathlib.Path, default=ROOT / "game_data" / "disc",
                        help="the folder tools/extract_game.py wrote")
    parser.add_argument("--no-game", action="store_true", help="only put the NRO in place")
    args = parser.parse_args()

    app = ROOT / "SD_CARD" / APP
    if not args.nro.is_file():
        raise SystemExit("%s was not found: build it first" % args.nro)
    (app / "SD_LOG").mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.nro, app / "demonsouls.nro")
    print("NRO: %s" % (APP / "demonsouls.nro").as_posix())
    # the game's own NRO, once it is built (docs/build.md): code made from your EBOOT, for your
    # SD card only
    game_nro = ROOT / "build" / "demonsouls_game.nro"
    if game_nro.is_file():
        place(game_nro, app / "demonsouls_game.nro")
        print("game NRO: %s" % (APP / "demonsouls_game.nro").as_posix())
    # the game's options (src/platform/nx_settings.c): the commented template, once, so the card's
    # own settings are never overwritten
    settings = app / "settings.txt"
    if not settings.exists():
        shutil.copy2(ROOT / "assets" / "settings.txt", settings)
        print("settings: %s (all commented out)" % (APP / "settings.txt").as_posix())

    if args.no_game:
        return 0
    if not (args.game / "PS3_GAME" / "PARAM.SFO").is_file():
        raise SystemExit("%s has no PS3_GAME/PARAM.SFO: run tools/extract_game.py first" % args.game)

    placed = total = 0
    for source in sorted(args.game.rglob("*")):
        relative = source.relative_to(args.game).as_posix()
        if not source.is_file() or relative in LEAVE_OUT:
            continue
        total += 1
        placed += place(source, app / "game" / relative)
    print("game: %d files in %s (%d new or changed)" % (total, (APP / "game").as_posix(), placed))

    # what the lifted libsre's glue loads at run time (PS3_LIBSRE_IMAGE, src/platform/nx_vfs.c):
    # the image scripts/build_libsre.py made from your libsre.prx (docs/build.md)
    image = ROOT / "work" / "libsre" / "libsre_image.bin"
    if image.is_file():
        place(image, app / "firmware" / "libsre_image.bin")
        print("firmware: %s" % (APP / "firmware" / "libsre_image.bin").as_posix())
    else:
        print("firmware: no libsre image yet (the libsre lift, docs/build.md)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
