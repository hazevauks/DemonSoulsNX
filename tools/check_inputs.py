#!/usr/bin/env python3
"""Checks the files you decrypted with RPCS3, and puts them where the port reads them.

    python tools/check_inputs.py [FOLDER ...]

Looks in each FOLDER (where RPCS3 wrote what it decrypted) and in game_data/ for the 28 files
Soulbrandt's setup lists (third_party/soulbrandt/tools/setup/des_setup/game_table.json):
EBOOT.elf, the 26 script files (.sdat.unedat) and libsre.prx from PS3 firmware 4.93. A file
counts when its size and SHA-256 match, whatever it is called. The 26 script files need no
RPCS3: the disc has each one unencrypted beside its .sdat, with the same bytes. Each file is
linked (or copied) to where the port reads it:

    game_data/disc/PS3_GAME/USRDIR/EBOOT.elf
    game_data/disc/PS3_GAME/USRDIR/.../<name>.sdat.unedat    beside its .sdat
    game_data/firmware/libsre.prx

tools/make_sd.py takes them to the SD card from there. None of them may go in the repository.
"""

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TABLE = ROOT / "third_party" / "soulbrandt" / "tools" / "setup" / "des_setup" / "game_table.json"
DISC = ROOT / "game_data" / "disc"
FIRMWARE = ROOT / "game_data" / "firmware"


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def place(source, target):
    if target.exists():
        if os.path.samefile(source, target):
            return
        target.unlink()
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def soulbrandt_firmware():
    """Soulbrandt's own check for a libsre.prx from another system software version
    (des_setup/firmware.py): the file differs only in the version written into its code."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("des_firmware", TABLE.parent / "firmware.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("folders", nargs="*", type=pathlib.Path, help="where RPCS3 wrote the decrypted files")
    args = parser.parse_args()

    table = json.loads(TABLE.read_text(encoding="utf-8"))
    wanted = [(e["path"].split("/")[-1], e["size"], e["sha256"], DISC / e["path"]) for e in table["decrypted"]]
    wanted += [(e["name"], e["size"], e["sha256"], FIRMWARE / e["name"]) for e in table["firmware"]]
    by_hash = {(size, digest): (name, target) for name, size, digest, target in wanted}
    sizes = {size for _, size, _, _ in wanted}
    names = {name.lower() for name, _, _, _ in wanted}

    found, wrong = {}, []
    for folder in args.folders + [DISC, FIRMWARE]:
        if not folder.is_dir():
            if folder in args.folders:
                print("ERROR: %s is not a folder" % folder)
            continue
        for path in folder.rglob("*"):
            if not path.is_file():
                continue
            size = path.stat().st_size
            looks_wanted = path.name.lower() in names or path.name.lower().endswith(".unedat")
            if size not in sizes and not looks_wanted:
                continue
            key = (size, sha256(path))
            if key in by_hash:
                found.setdefault(key, path)
            elif looks_wanted:
                wrong.append(path)

    # libsre.prx from another system software version: its version is set back to the one the
    # table's sum is for, and the whole file must then match the sum, as Soulbrandt's setup does
    adapted = []
    firmware = soulbrandt_firmware()
    for entry in table["firmware"]:
        key = (entry["size"], entry["sha256"])
        for path in [p for p in wrong if key not in found and p.name.lower() == entry["name"].lower()]:
            result = firmware.matched(path, entry)
            if result:
                fixed, own = result
                target = FIRMWARE / entry["name"]
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists():
                    target.unlink()
                target.write_bytes(fixed)
                found[key] = target
                wrong.remove(path)
                adapted.append((entry["name"], firmware.name(own), firmware.name(int(entry["version"], 16)),
                                len(firmware.places(fixed))))

    plain = 0
    for name, size, digest, target in wanted:
        source = found.get((size, digest))
        if source:
            # the disc has each script unencrypted beside its .sdat, byte for byte the same
            plain += source.parent == target.parent and source.name + ".sdat.unedat" == target.name
            place(source, target)
    have = sum(1 for _, size, digest, _ in wanted if (size, digest) in found)

    print("%d of %d decrypted files found and in place" % (have, len(wanted)))
    if plain:
        print("  %d script files needed no decryption: the disc has each one unencrypted beside its .sdat" % plain)
    for name, own, want, count in adapted:
        print("  %s is from system software %s: with its version set to %s's (%d places in its code), "
              "it is the file the port is tested with" % (name, own, want, count))
    for name, size, digest, target in wanted:
        if (size, digest) not in found:
            print("  missing: %s" % target.relative_to(ROOT).as_posix())
    for path in wrong:
        print("  not the right file (another version, or damaged): %s" % path)
    if have < len(wanted):
        print("Decrypt them with your own RPCS3: Utilities > Decrypt PS3 Binaries (docs/assets.md).")
    return 0 if have == len(wanted) else 1


if __name__ == "__main__":
    sys.exit(main())
