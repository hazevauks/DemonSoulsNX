#!/usr/bin/env python3
"""Extracts Demon's Souls from your own disc image.

Reads the ISO 9660 file system of a decrypted disc image (the Blu-ray encryption already
removed, as a ".dec.iso" dump has it) and writes the game's folder, the one with
PS3_DISC.SFB and PS3_GAME in it. The disc's system update (PS3_UPDATE) is left out. No other
tools are needed. Files already extracted with the right size are skipped, so a stopped run
can be started again.

    python tools/extract_game.py "<your disc>.iso" game_data/disc

Only the US disc, BLUS30443 version 1.00, is accepted: the port is made for that version.
"""

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys
import time

SECTOR = 2048
TITLE_ID = "BLUS30443"
APP_VERSION = "01.00"
# the retail EBOOT.BIN of BLUS30443 01.00 (still encrypted, as it is on the disc)
EBOOT_SIZE = 28224840
EBOOT_SHA256 = "77375fbf5c654b5a92b870831862a9a09bdc9e6052956d5b84503bda5704f3ac"
SKIP_TOP = {"PS3_UPDATE"}
CHUNK = 8 * 1024 * 1024


def read_at(iso, offset, size):
    iso.seek(offset)
    return iso.read(size)


def volume_root(iso):
    """Returns (root extent, root size, joliet) from the best volume descriptor.

    The Joliet table keeps the names' case, so it wins over the primary one."""
    primary = None
    for index in range(16, 64):
        descriptor = read_at(iso, index * SECTOR, SECTOR)
        if descriptor[1:6] != b"CD001":
            break
        kind = descriptor[0]
        root = descriptor[156:190]
        entry = (struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0])
        if kind == 2 and descriptor[88:90] == b"%/":
            return entry + (True,)
        if kind == 1:
            primary = entry + (False,)
        if kind == 255:
            break
    if primary is None:
        raise SystemExit("not an ISO 9660 image")
    return primary


def directory_records(iso, extent, size):
    data = read_at(iso, extent * SECTOR, size)
    offset = 0
    while offset < len(data):
        length = data[offset]
        if length == 0:
            offset = (offset // SECTOR + 1) * SECTOR
            continue
        yield data[offset:offset + length]
        offset += length


def walk(iso, extent, size, joliet, prefix=""):
    """Yields (path, [(extent, size), ...]) for every file under a directory.

    A file larger than 4 GB is stored as several records with the multi-extent flag; their
    extents are joined in order."""
    pending = None
    for record in directory_records(iso, extent, size):
        name_length = record[32]
        raw = record[33:33 + name_length]
        if name_length == 1 and raw in (b"\x00", b"\x01"):
            continue
        name = raw.decode("utf-16-be" if joliet else "latin-1").split(";")[0].rstrip(".")
        child = (struct.unpack("<I", record[2:6])[0], struct.unpack("<I", record[10:14])[0])
        flags = record[25]
        path = prefix + name
        if flags & 2:
            if not prefix and name.upper() in SKIP_TOP:
                continue
            yield from walk(iso, child[0], child[1], joliet, path + "/")
            continue
        if pending and pending[0] == path:
            pending[1].append(child)
        else:
            if pending:
                yield pending
            pending = (path, [child])
        if not flags & 0x80:
            yield pending
            pending = None
    if pending:
        yield pending


def read_sfo(data):
    """Returns the PARAM.SFO's keys and values."""
    if data[:4] != b"\x00PSF":
        raise SystemExit("PARAM.SFO is not valid")
    key_table, data_table, count = struct.unpack("<III", data[8:20])
    values = {}
    for index in range(count):
        key_offset, fmt, length, _, data_offset = struct.unpack("<HHIII", data[20 + index * 16:36 + index * 16])
        key = data[key_table + key_offset:data.index(b"\0", key_table + key_offset)].decode()
        raw = data[data_table + data_offset:data_table + data_offset + length]
        values[key] = struct.unpack("<I", raw)[0] if fmt == 0x0404 else raw.rstrip(b"\0").decode("utf-8", "replace")
    return values


def copy_file(iso, extents, target):
    digest = hashlib.sha256()
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + ".part")
    with open(temporary, "wb") as out:
        for extent, size in extents:
            iso.seek(extent * SECTOR)
            left = size
            while left:
                block = iso.read(min(CHUNK, left))
                if not block:
                    raise SystemExit("the image ends early: is it complete?")
                digest.update(block)
                out.write(block)
                left -= len(block)
    os.replace(temporary, target)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("iso", type=pathlib.Path, help="your decrypted disc image")
    parser.add_argument("output", type=pathlib.Path, help="folder to write PS3_DISC.SFB and PS3_GAME into")
    parser.add_argument("--list", action="store_true", help="only list the files")
    args = parser.parse_args()

    with open(args.iso, "rb") as iso:
        extent, size, joliet = volume_root(iso)
        files = sorted(walk(iso, extent, size, joliet), key=lambda item: item[0].lower())
        by_name = {path.upper(): extents for path, extents in files}

        sfo_extents = by_name.get("PS3_GAME/PARAM.SFO")
        if not sfo_extents:
            raise SystemExit("no PS3_GAME/PARAM.SFO: this is not a PS3 game disc")
        sfo = read_sfo(read_at(iso, sfo_extents[0][0] * SECTOR, sfo_extents[0][1]))
        title, version = sfo.get("TITLE_ID"), sfo.get("APP_VER") or sfo.get("VERSION")
        print("disc: %s, %s, version %s" % (sfo.get("TITLE", "?"), title, version))
        if title != TITLE_ID or version != APP_VERSION:
            raise SystemExit("this port needs %s version %s" % (TITLE_ID, APP_VERSION))
        eboot = by_name.get("PS3_GAME/USRDIR/EBOOT.BIN")
        if not eboot or sum(s for _, s in eboot) != EBOOT_SIZE:
            raise SystemExit("PS3_GAME/USRDIR/EBOOT.BIN is not the one of version %s" % APP_VERSION)

        total = sum(s for _, extents in files for _, s in extents)
        print("%d files, %.2f GB" % (len(files), total / 1e9))
        if args.list:
            for path, extents in files:
                print("%12d  %s" % (sum(s for _, s in extents), path))
            return 0

        folded = {}
        for path, _ in files:
            other = folded.setdefault(path.lower(), path)
            if other != path:
                raise SystemExit("two files differ only in case: %s and %s" % (other, path))

        done, started, hashes = 0, time.time(), {}
        for path, extents in files:
            target = args.output / path
            length = sum(s for _, s in extents)
            if target.is_file() and target.stat().st_size == length and path.upper() != "PS3_GAME/USRDIR/EBOOT.BIN":
                done += length
                continue
            hashes[path] = copy_file(iso, extents, target)
            done += length
            elapsed = max(time.time() - started, 0.001)
            sys.stdout.write("\r%5.1f%%  %6.1f MB/s  %-70s" % (100.0 * done / total, done / elapsed / 1e6, path[-70:]))
            sys.stdout.flush()
        print()

        eboot_hash = hashes.get("PS3_GAME/USRDIR/EBOOT.BIN")
        if EBOOT_SHA256 and eboot_hash != EBOOT_SHA256:
            raise SystemExit("EBOOT.BIN does not match version %s (SHA-256 %s)" % (APP_VERSION, eboot_hash))
        manifest = {
            "title_id": title,
            "version": version,
            "files": len(files),
            "bytes": total,
            "eboot_sha256": eboot_hash,
        }
        (args.output / "extract_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print("done: %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
