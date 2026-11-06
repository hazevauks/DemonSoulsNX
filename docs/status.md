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
| ps3recomp's runtime builds for Horizon: 307 sources | `./build.sh`, docs/build.md |
| Your decrypted inputs: `EBOOT.elf` as Soulbrandt's hash, libsre 4.93 as 4.86 once its version is set back | `tools/check_inputs.py` |
| The lift: 94,017 PPU functions in 57 chunks, 12 SPU images and 2 jobs by fingerprint, libsre's 756 functions with its TOC | ps3recomp's and Soulbrandt's tools, docs/build.md |
| The host side builds for Horizon: the boot scaffold, the HLE NID table (1,261 handlers), Soulbrandt's game code; linked whole with the runtime, only the lift's own symbols are missing | `./build.sh`, docs/build.md |
| The game builds for Horizon: the 57 lifted chunks, the SPU programs and libsre compile with devkitA64 and link with the host side, the runtime, the platform layer and the libraries, with no symbol missing, into a 173 MB NRO (161 MB of code); it starts at stage 6 | `cmake --build build --target demonsouls_game_nro`, docs/build.md |
| The game's NRO holds the HLE NID table (its 1,261 handlers registered) and all of Soulbrandt's host files: the host side links as objects, as `des_boot` does | `nm`: `ppu_hle_register_all` is the table's, `cellPadGetData` and `cellAudioInit` are in |
| The libraries are declared in one place and their versions checked at configure time: libnx 4.12.0, deko3D 0.5.0, SDL2 2.28.5 (with Mesa 20.1.0, never started), zlib 1.3.1 | `cmake/desnx_libraries.cmake`, docs/build.md |
| The game's sound and controls through the Switch's SDL2: cellAudio's device opened with 1024-frame buffers, cellPad given only the slots with a controller in them, and both logged. cellPad's reads go through the wrappers | the disassembly of cellPad's `pad_sdl_read`, `tools/check_rebuild.py`, docs/build.md |
| The game's start on the Switch: libnx's start code calls `__wrap_main`, which sets up the log, the drives, guest memory and the EBOOT's path, then the boot's own `main`; host stacks above 16 MB get 16 MB. Link options only: no lifted chunk compiles again | `nm` and the start code's branch, `tools/check_rebuild.py` (57 of 57 unchanged), docs/build.md |
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

The stage 3 test, the same day, on the same console:

| What | Result |
| --- | --- |
| deko3D: device, queue, a 1280x720 swapchain, two shader programs uam compiled at build time | all load; the triangle is on the screen |
| 120 frames of it | 16.3 ms each, the slowest 17.1 ms: the display's 60 Hz |
| Cores the process may use | 0, 1 and 2 (mask 0x7); core 3 is the system's |
| Thread priorities it may use | 28 to 59 (hbloader's main thread is at 44) |
| The same work on each core alone | 117 ms on each of the three |
| One thread waking another through address arbitration (`svcWaitForAddress`) | 0.5 µs across cores, 2.2 µs on one core |
| The same through a condition variable (what pthreads use) | 4.1 µs across cores: eight times slower |
| `svcSleepThread` for 1 ms | 1.002 ms on average, the longest 1.005 ms |
| A yield | 0.34 µs |
| zlib inflating the game's DCX blocks on one core | 107 MB/s |

The stage 5 tests, 2026-11-06, on the same console, with a Switch controller (the second run
timed the sound at larger buffers):

| What | Result |
| --- | --- |
| SDL2 2.28.5 and zlib 1.3.1 in the NRO, the versions it was built against | yes |
| SDL2's game controllers and sound, started as cellPad and cellAudio start them | 0.8 ms |
| deko3D after SDL2: the triangle | 120 frames at 16.4 ms, as before |
| The controllers SDL2 lists | 8: switch-sdl2 lists one for each of the console's eight controller slots, connected or not; the first is a "Switch Controller" |
| Every button reaches SDL2's game controller API | all 15 pressed: A, B, X, Y, L, R, ZL and ZR (as triggers), the d-pad, both sticks pressed in, minus; plus exits the test, and HOME is the system's |
| Which button is SDL2's `a` | the Switch's **B**, the bottom one: switch-sdl2 maps by position (its mapping string is `a:b1,b:b0,x:b3,y:b2`, with button 0 the Switch's A), so cellPad's Cross is the bottom button, where the PS3 has it |
| The sound device, opened as cellAudio opens it: 48 kHz, float, stereo, 256 frames a buffer | opens; the driver turns the float into 16-bit for the audio renderer |
| Frames a second the device takes from the queue at 256 frames a buffer | **25,600, not 48,000**: one buffer every 10 ms, so the rest of each 10 ms is silence. switch-sdl2's driver keeps two buffers in the audio renderer and waits for each to start before it takes the next; a 5.3 ms buffer is shorter than that wait. cellAudio paces the game's audio clock on this rate, so it would run at about half speed |
| The same at 512, 1024 and 2048 frames a buffer | 40,690, **48,091** and 47,866 a second: from 1024 frames (21.3 ms) a buffer the device keeps up, within the measurement's half a percent. Stage 7 opens SDL2 with 1024 frames on the Switch; cellAudio already allows a device buffer larger than its blocks |
| A tone at 256 frames a buffer, then at 1024 | the first sounds rougher, as the silence between its buffers would make it |

The stage 6 test, the same day: the test NRO with guest memory's new calls, then the game's NRO
for the first time, on the same console:

| What | Result |
| --- | --- |
| The test NRO's guest memory, as the game maps it: 511 MB committed, 64 pieces of 1 MB, 9 faults | 95 ms; 203 µs a piece; 209 µs a fault; 75 mappings, 584 MB, all given back |
| The game's start: the log, the drives, the checks, guest memory at 0x200000000, the RSX's 256 MB mapped | yes |
| The boot: `EBOOT.elf`'s two segments, libsre (756 functions, 267 exports), the HLE table and the system calls, the entry dispatched | yes |
| The game's own threads | 22 PPU threads, the set Soulbrandt's docs/24 lists for a boot that reaches the engine: HighGraphics, HG_Watchdog, the four `Core.Res.*`, Havok, the sound threads, NexusRevolution, DsSaveLoadMan, SLSession |
| Guest memory after 30 seconds | 467 MB in 61 mappings, 43 faults answered, none refused |
| Host stacks | 95 set so far (SPURS tasksets end and start again, and their threads with them), 622 MB asked for in all, 33 of them cut from 256 MB to 16 MB; no thread failed to start |
| The game's files | 24 opened by the PS3's paths: shader bundles, sounds, `other/`, its cache in `/dev_hdd1` (empty at a first start) |
| Where it stops | about 20 seconds in, a thread waits on SPURS event flag 0x01B93E00, which SPU image 2's job chain sets when HighGraphics starts (Soulbrandt's docs/24); 24 seconds later it was still waiting, and the log ends there. Stage 8 |
| The log | lines of different threads wrote over each other, with leftover bytes from the SD card in the gaps: stdout, stderr and the log were three streams on one descriptor. They are one stream now, not run yet |

The stage 7 tests, the same day, two runs of the game's NRO in hand (the Joy-Cons attached):

| What | Result |
| --- | --- |
| cellAudio's device | 1024 frames a buffer, for the 256 it asks; 48 kHz, two channels |
| Frames a second the game's mixer queued, 10 seconds at a time | 48,074, 48,034, 47,987, 43,817 (once), 47,989; then 48,096, 48,017, 47,986, 47,987, 47,979: the game's audio clock runs at the PS3's rate |
| Waiting to be played | 768 to 1,792 frames in the queue (16 to 37 ms), and the device's two buffers (42 ms) |
| The game's sound | silence so far, the loudest sample 0.000: it stops before it plays anything |
| The controllers SDL2 gives cellPad | one: the Joy-Cons in handheld mode, in slot 1; the seven empty slots are no longer pads for the game |
| The buttons, through cellPad into the game | the second run: left, up, right and plus (Start), read as the game asked for its pad. The rest were pressed before it first asked, a few seconds before its wait; cellPad reads all of them in the same call, and stage 5 had each by name |
| Where it stops | the same wait on SPURS event flag 0x01B93E00, sooner: under 10 seconds in, where the first run got about 20 |
| The log | the first run had gaps again: the runtime makes stdout unbuffered as the PPU starts, and newlib writes to an unbuffered stream outside its lock. With the log's stream kept line-buffered, the second run's 1,259 lines have none |

## Not checked yet

How late the sound is with 1024-frame buffers: two of them in the audio renderer (43 ms) and
cellAudio's queue on top. Stage 7 measures it with the game's own sound; if it is too much,
cellAudio gets its own audio renderer voice with more, smaller buffers in the queue.

Which controller slots are connected: SDL2 lists all eight, and stage 7 has cellPad report only
the ones in use.

The game's cache on the SD card. The game copies files it loads into `/dev_hdd1` and reads the
copies on later loads (Soulbrandt's docs/101: 173 copies during the load into 1-1). On the PS3
that saved the disc's seeks; here the copies come from the same SD card, and writing runs at
10 MB/s. Stage 11 measures what it costs a load, and the cache may move to memory.

Which of the two copies the game opens (raw or `.dcx`): the runtime's log of opened files, on the
first boot of the game.

How many separate pieces the kernel allows before it runs out of memory blocks: each piece
mapped on its own stays a block of its own. The game maps what the runtime commits in whole
ranges, and 1 MB pieces for stray touches: 61 mappings 30 seconds into the first run. Its log
counts them every 10 seconds.

The game's own sound, and how late it comes: the mixer runs at the right rate, but the game has
played nothing yet. Up to about 80 ms wait between the mixer and the speaker (the queue and the
device's two buffers). A Pro Controller, detached Joy-Cons, and a controller connected or taken
away while the game runs.

Whether 16 MB is stack enough for every thread of the game, in a level. None ran short in the
first run's 45 seconds.

The game's exit. `sys_process_exit` ends the process from whichever thread calls it; libnx's exit
would go back to hbloader on that thread while the others go on, and guest memory given back
while they run is a crash. Stage 8, when the game first gets that far.

## Roadmap

| Stage | State |
| --- | --- |
| 1. Extraction, asset structure, sources | **done**, both console tests passed |
| 2. Asset packaging: paths, case, the file layer on Horizon, the decrypted inputs | **done**, checked on the PC and the console |
| 3. Engine dependencies: RSX/libgcm to deko3D, PPU/SPU threads to the Switch's cores | **done** (docs/engine.md), checked on the console |
| 4. The port's build: ps3recomp's runtime and the lifted game with devkitA64 and CMake | **done** (docs/build.md): the runtime, the host side and the lifted game build and link into the game's NRO, in 134 minutes on a 2-core laptop; it can start once stage 6 gives it guest memory |
| 5. Toolchain files: devkitA64, libnx, SDL2, deko3D | **done**: the libraries are declared and checked, the game links with all of them; on the console SDL2 and deko3D work together, every button reaches SDL2, and its sound keeps up from 1024 frames a buffer (cellAudio's 256 play at half rate) |
| 6. POSIX layer, alignment, guest memory on the Switch's heap | **done**: the game's NRO starts, its boot runs on guest memory (467 MB, 61 mappings, nothing refused) and creates the game's 22 PPU threads on capped host stacks |
| 7. Sound (cellAudio on audren or SDL2) and controls for the Joy-Cons and Pro Controller | **done**: the game's mixer runs at 48,000 frames a second on SDL2 with 1024-frame buffers; cellPad gets only the slots with a controller, and the buttons reach the game in the PS3's positions. Checked in handheld mode; a Pro Controller and detached Joy-Cons take the same way and have not been on the console |
| 8. First boot of the game, with a fallback renderer | **in progress**: the wait on HighGraphics' SPURS event flag, at the same point of the log in each run. The game uploads its textures through one job chain, run again as soon as its last job sets the flag. A run that came while the chain was still being walked was dropped; it now walks the chain again, but that never happened in the run after, which stopped the same way. The runtime now keeps its last SPURS events and prints the chains' and the flag's when a wait blocks a second |
| 9. Test harness: nxlink and the SD card, logs in `SD_LOG` | started: the log exists |
| 10. Crash triage: alignment faults, missing system calls, null pointers | to do |
| 11. Asset loading: archives, materials, shaders, streaming | to do |
| 12. Native deko3D renderer | to do |
| 13. The game's passes: deferred lighting, shadows, post-processing, fog | to do |
| 14. Surfaces, framebuffers, textures, geometry buffers | to do |
| 15. Render pass, lighting, particle and post-processing tuning | to do |
| 16. Frames and timings against targets: Nexus, Boletaria, Stonefang, HUD, phantoms, cutscenes | to do |
| 17-19. 30 fps at 1020/460/1331 MHz, memory budget, everything on hardware | to do |
