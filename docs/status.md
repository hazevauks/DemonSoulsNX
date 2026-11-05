# Status

What has been checked, and how. Nothing here counts as working until a console has shown it.

## Checked

| What | How |
| --- | --- |
| The disc: Demon's Souls, BLUS30443, version 01.00 | `tools/extract_game.py` reads `PARAM.SFO` |
| `EBOOT.BIN` is the 1.00 one | its SHA-256 matches the one Soulbrandt's setup expects |
| Extraction: 12,511 files, 8.45 GB | `tools/extract_game.py`, in pure Python, from the ISO 9660 / Joliet image |
| The files fit the Switch's SD card | `tools/scan_assets.py`: largest file 692 MB, paths up to 74 characters, no case clashes (docs/assets.md) |
| Sources: ps3recomp and Soulbrandt, pinned | third_party/README.md |
| No keys in the vendored sources | the key tables and the code that used them are removed, `tools/check_repo.py` looks for key tables |
| The NRO builds: name, author and icon | `tools/check_nro.py`: Demon's Souls, hazevauks, 256x256 JPEG |
| The repository holds no game data or personal data | `tools/check_repo.py`, also on CI |
| Every compressed file inflates: 3,600 DCX and DCP files, 1,750 MB to 3,067 MB | `tools/check_archives.py` |
| 3,585 raw files are byte for byte their `.dcx` (3.01 GB); 3 differ | `tools/check_archives.py` |
| The 26 encrypted scripts are on the disc unencrypted too, with the bytes decrypting gives | `tools/check_inputs.py`, against Soulbrandt's hashes |
| devkitPro is up to date | `pacman -Syu`: deko3D 0.5.0, libnx 4.12.0, devkitA64 r30, uam 1.1.0, switch-sdl2, switch-openal-soft, switch-ffmpeg 7.1 |

## Checked on a console

The first test of the stage 1 NRO, on firmware 22.5.0 with Atmosphère, started with title
takeover (2026-11-05):

| What | Result |
| --- | --- |
| The NRO starts and writes `SD_LOG/demonsouls_log.txt` | yes |
| Memory for the process, as an application | 3189 MB |
| The game's folder on the SD card | found: BLUS30443 01.00, all 18 `USRDIR` folders |
| `svcMapPhysicalMemory`, the first plan for guest memory | **not available**: hbloader's process has no system resource; its alias region is 6 GB, a 36-bit address space |
| The text on the screen | went into the log instead, so the screen stayed black; fixed for the next test |

The second test, the same day, on the same console, tried guest memory the other way
(docs/architecture.md):

| What | Result |
| --- | --- |
| Address space | 36-bit: code region 0x8000000-0x1000000000, heap and alias regions inside it |
| Process handle, `svcMapProcessCodeMemory`, `svcSetProcessMemoryPermission` | given, allowed, allowed |
| A free 4 GB window on a 4 GB boundary | found, at 0x400000000 |
| Main and RSX memory, 512 MB, mapped and zeroed | 90 ms |
| 512 pieces of 64 KB, mapped one by one | 20 µs each; each stays a block of its own for the kernel |
| Faults across the 4 GB (null page, gaps, stacks, the top edge) | 9 of 9 answered, 28 µs each |
| Everything unmapped and given back before the exit | yes |
| The screen text | no longer in the log |

The stage 2 test, the same day, on the same console and SD card:

| What | Result |
| --- | --- |
| The game's first files by the PS3's paths (`/dev_bdvd`, `/app_home`) | 6 of 6 open and hold what they should |
| `other/envLight.tpf`, the case the game asks for (the disc has `envlight.tpf`) | opens: the SD card ignores case |
| A path out of the drives, and `/dev_flash` | both refused |
| DCX files inflated with zlib on the console | 3 of 3: a shader archive, a texture pack, an animation archive |
| A long read, 1 MB at a time | 75 MB/s |
| A 64 KB read at a random place | 2.4 ms |
| A small file: open, read 4 KB, close (251 files of one level) | 1.9 ms each |
| The cache's way of writing: size set first, 8 MB in 64 KB pieces, flushed, read back, renamed, removed | works, **10 MB/s** |
| Decrypted files on the card | the 26 scripts; `EBOOT.elf` and `libsre.prx` not yet |

## Not checked yet

The game's cache on the SD card. The game copies files it loads into `/dev_hdd1` and reads the
copies on later loads (Soulbrandt's docs/101: 173 copies during the load into 1-1). On the PS3
that saved the disc's seeks; here the copies come from the same SD card, and writing runs at
10 MB/s. Stage 11 measures what it costs a load, and the cache may move to memory.

Which of the two copies the game opens (raw or `.dcx`): the runtime's log of opened files, on the
first boot of the game.

How many separate pieces the kernel allows before it runs out of memory blocks: each piece
mapped on its own stays a block of its own. Stage 6 maps the game's memory in large ranges and
leaves 64 KB pieces for stray touches; it will log how many pieces it maps.

## Roadmap

| Stage | State |
| --- | --- |
| 1. Extraction, asset structure, sources | **done**, both console tests passed |
| 2. Asset packaging: paths, case, the file layer on Horizon, the decrypted inputs | **done**, checked on the PC and the console |
| 3. Engine dependencies: RSX/libgcm to deko3D, PPU/SPU threads to the Switch's cores | next |
| 4. The port's build: ps3recomp's runtime and the lifted game with devkitA64 and CMake | to do |
| 5. Toolchain files: devkitA64, libnx, SDL2, deko3D | to do |
| 6. POSIX layer, alignment, guest memory on the Switch's heap | to do |
| 7. Sound (cellAudio on audren or SDL2) and controls for the Joy-Cons and Pro Controller | to do |
| 8. First boot of the game, with a fallback renderer | to do |
| 9. Test harness: nxlink and the SD card, logs in `SD_LOG` | started: the log exists |
| 10. Crash triage: alignment faults, missing system calls, null pointers | to do |
| 11. Asset loading: archives, materials, shaders, streaming | to do |
| 12. Native deko3D renderer | to do |
| 13. The game's passes: deferred lighting, shadows, post-processing, fog | to do |
| 14. Surfaces, framebuffers, textures, geometry buffers | to do |
| 15. Render pass, lighting, particle and post-processing tuning | to do |
| 16. Frames and timings against targets: Nexus, Boletaria, Stonefang, HUD, phantoms, cutscenes | to do |
| 17-19. 30 fps at 1020/460/1331 MHz, memory budget, everything on hardware | to do |
