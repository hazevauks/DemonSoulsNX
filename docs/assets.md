# The game's files

What is on the disc, measured from a BLUS30443 01.00 dump with `tools/extract_game.py` and
`tools/scan_assets.py`. Only names, counts and sizes are listed here: nothing of the game itself.

## The disc

| What | Value |
| --- | --- |
| Image | ISO 9660 with a Joliet table (the Joliet names keep their case), plus UDF |
| Title | Demon's Souls, BLUS30443, version 01.00 |
| `PS3_GAME/USRDIR/EBOOT.BIN` | 28,224,840 bytes, SHA-256 `77375fbf…3ac`, a retail SELF (encrypted) |
| Files | 12,512 on the disc; 12,511 without `PS3_UPDATE/PS3UPDAT.PUP` (a 2009 system update, not needed) |
| Size | 8.45 GB extracted |

`tools/extract_game.py` checks the title, the version and the EBOOT's size and hash, and leaves
`PS3_UPDATE` out. Its result is the disc's folder as RPCS3 and Soulbrandt use it:
`PS3_DISC.SFB` and `PS3_GAME/`.

## USRDIR, by folder

| Folder | Files | MB | What it holds |
| --- | ---: | ---: | --- |
| `movie` | 6 | 2,768 | the cutscenes and the intro, as PAMF (H.264 video, ATRAC3plus sound) |
| `map` | 6,803 | 2,234 | the levels: layouts (`.msb`), models, textures, collision (`.hkx`), navigation (`.nvm`) |
| `chr` | 942 | 1,334 | characters: models, animations, event scripts |
| `obj` | 1,558 | 1,279 | objects |
| `parts` | 1,286 | 439 | armour and weapons |
| `sound` | 424 | 128 | sound banks (`.mowb`, `.mosb`, `.moib`) |
| `remo` | 104 | 67 | in-engine cutscenes |
| `menu` | 180 | 59 | menus, HUD |
| `sfx` | 24 | 36 | effects (`.ffxbnd`) |
| `font` | 20 | 23 | fonts |
| `script` | 373 | 13 | Lua and event scripts; 26 of the 151 `.esd` scripts are in `.sdat` files |
| `param`, `paramdef` | 292 | 9 | the game's tables |
| `shader` | 10 | 6 | the RSX shaders (`.shaderbnd`, one `.vpo` and one `.fpo`) |
| `msg`, `mtd`, `facegen`, `item`, `other`, `testdata`, `tropdir` | 461 | 16 | text, materials, faces, trophies, test lists |

## Formats

| Format (first bytes) | Files | MB |
| --- | ---: | ---: |
| PAMF movies | 6 | 2,768 |
| BND3 archives (`*bnd`) | 1,958 | 1,876 |
| DCX compressed files (`*.dcx`) | 3,365 | 1,707 |
| TPF texture packs | 356 | 1,196 |
| FLVER models | 1,776 | 622 |
| DCP compressed files (older wrapper, `.flver.dcx` and `.chrbnd.dcx`) | 235 | 43 |
| NPD (SDATA, encrypted) `.sdat` | 26 | 1.4 |
| Havok, navigation, Lua, params and others | 4,789 | 234 |

- **Compression.** Every DCX file uses `EDGE`: the PS3's Edge zlib, 64 KB blocks of raw deflate
  that the game inflates on the SPUs. The 235 DCP files, an older wrapper, hold one zlib stream
  each (`DFLT`). On the Switch this runs as the game's own SPU code, recompiled
  (docs/architecture.md). `tools/check_archives.py` inflates all 3,600 on the PC: 1,750 MB become
  3,067 MB, every block the size its header gives. Inside are 1,781 BND3 archives (listing 17,869
  files; 6 unused ones list none), 1,635 FLVER models and 177 TPF texture packs.
- **Two copies of many files.** 3,588 files are on the disc both raw and as `.dcx`: mostly
  `.flver`, `.objbnd`, `.partsbnd`, `.tpf`, `.anibnd` and `.chrbnd`. For 3,585 of them (3.01 GB)
  the inflated `.dcx` is byte for byte the raw file (`tools/check_archives.py`); three objects
  (`o0052`, `o2195`, `o4219`) differ. Soulbrandt's logs show the game opening the `.dcx` copies, and
  the game's load lists name files without `.dcx`, so the game adds it. Until the runtime's log of
  opened files confirms that on the first boot, the SD card keeps both.
- **Encrypted files.** `EBOOT.BIN` (the game's code) and the 26 `.sdat` files (event scripts) are
  encrypted. The port has no keys. The scripts need none: the disc has each one unencrypted beside
  its `.sdat`, and those plain copies are byte for byte what decrypting the `.sdat` gives (checked
  against the hashes in Soulbrandt's table). Only `EBOOT.BIN` is decrypted, once, with your own RPCS3.
- **Byte order.** Everything is big-endian, as the PS3 reads it. The recompiled code reads it as it
  is, so no file is converted.

## What it means on the Switch

| Limit | Disc | Switch |
| --- | --- | --- |
| Largest file | 692 MB (`movie/ds_opening.pam`) | FAT32 allows 4 GB: any card works |
| Longest name | 36 characters | 255 |
| Longest path from the game's folder | 74 characters | 768 (`FS_MAX_PATH`), with room for `sdmc:/switch/demonsouls/game/` |
| Names that differ only in case | none | FAT32 and exFAT ignore case |
| Names with capitals | 1 on the disc (`EBOOT.BIN`) | the game asks for some (`other/envLight.tpf` in a load list, `envlight.tpf` on the disc); FAT32 and exFAT ignore case, as the PS3's disc does |
| Space | 8.45 GB (8.4 GB without `EBOOT.BIN`) | about 5.5 GB if the duplicates go |

So the disc's folder can be copied as it is, and the port's file layer (`src/platform/nx_vfs.c`)
only has to map the PS3's drives onto folders of the SD card:

| The game asks for | On the SD card |
| --- | --- |
| `/dev_bdvd/...`, `/app_home/...` (the disc) | `sdmc:/switch/demonsouls/game/...` |
| `/dev_hdd0/...` (the PS3's drive: saves) | `sdmc:/switch/demonsouls/hdd0/...` |
| `/dev_hdd1/...` (the game's cache, emptied at every start) | in memory, up to `DESNX_CACHE_MEMORY_MB` (512), then `sdmc:/switch/demonsouls/hdd1_spill/`; with 0, `sdmc:/switch/demonsouls/hdd1/...` |
| `/dev_flash`, `/dev_usb`, a path out of a drive | refused |

## What has to be decrypted on the PC

Soulbrandt asks for 28 files ([its docs/51](../third_party/soulbrandt/docs/51-legal-decryption.md)).
Two of them need your own RPCS3 (*Utilities > Decrypt PS3 Binaries*); the other 26 are on the disc:

| File | What for | From |
| --- | --- | --- |
| `PS3_GAME/USRDIR/EBOOT.BIN` → `EBOOT.elf` | the game's code: the recompiler reads it on the PC, and the runtime loads its data | RPCS3 |
| `libsre.sprx` from PS3 firmware 4.93 → `libsre.prx` | eight SPURS queue functions Soulbrandt still runs as Sony's code | RPCS3 |
| the 26 `.sdat` files → `.sdat.unedat` | the event scripts | the disc's plain copy beside each `.sdat` |

`tools/check_inputs.py [the folder RPCS3 wrote to]` checks them all against the sizes and hashes in
`third_party/soulbrandt/tools/setup/des_setup/game_table.json`, whatever they are called, and links
them where the port reads them: `EBOOT.elf` and the `.unedat` files beside their originals in
`game_data/disc/`, `libsre.prx` in `game_data/firmware/`. `tools/make_sd.py` takes them to the SD card.
