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

The stage 8 runs, the same day, after the job chain fix and with the SPURS trace:

| What | Result |
| --- | --- |
| A run or kick of a job chain while it was being walked | none: that was not the wait's cause |
| The texture chain, cycle after cycle | create, run, walk: the copy job (EBOOT 0x0177B480), then the notifier (0x0177B600), whose SPU write sets bit 0x0002 of flag 0x01B93E00; HighGraphics wakes, shuts the chain down, joins it, clears the flag, creates and runs it again: about one cycle every 1 to 5 ms |
| The last cycle | created and run (walk 111), but no walk: the chain's state reads not running, which is how the runtime's CreateThread failing leaves it. HighGraphics then waits on the flag for good |
| Why the thread could not be made | libnx's pthreads cannot be detached: newlib's pthread_create keeps no detach state, and its pthread_detach fails, as libnx leaves out its hook. A thread no one joins keeps its kernel handle and its stack. The runtime's CreateThread makes detached threads and joins none, one for each walk of a chain among them |
| With detached threads given back | no wait: 1,279 detached threads ended and given back in the first 10 seconds, 1,997 in 90, never more than 93 at once |
| How far the boot got | past HighGraphics: the in-game load list (`other/ingamestay.loadlist`), the loading screens (`menu/nowloading*.drb`), `other/lensflare.tpf`, the character parts (`parts/*.partsbnd.dcx`), `facegen/facegen.fgbnd`, and indexed draws, about 35 seconds in. Then the log falls nearly silent, while the game goes on making a thread now and then: perhaps waiting for the title screen's Start, which nothing on the screen shows yet |
| The sound | stopped 15 seconds in: the mixer's queue stayed at 1,792 frames, so the device took no more. Most likely SDL2's audio thread, at the same priority as the game's thousand threads, went without the CPU until its voice ran dry, and switch-sdl2's driver waits on a stopped voice for good. Threads asking SDL2 for high priority now get 40, above the game's 59 |

The next run, 150 seconds, with Start pressed at about 40 seconds and Cross after:

| What | Result |
| --- | --- |
| The game's frames, 10 seconds at a time | 63, 97, then 108 to 110 to the end: the game's loop runs, at about 11 frames a second on the null renderer |
| The sound | 47,794 to 48,156 frames a second for all 150 seconds, the device playing: SDL2's audio thread at priority 40 holds |
| Start (plus) | reached the game at about 38 seconds; the game then loaded the player character (`chr/c0000`, its animations and scripts), `menu/menu.tpf.dcx` and the face generator |
| Cross (B) | reached the game at about 55 seconds: it started its trophy manager (38 trophies, from the disc's TROPHY.TRP), found no save, made one (`BLUS30443DEMONSS005`: `USER.DAT` and its copies, the icons) and played a menu sound (the loudest sample 0.188) |
| Then | the title screen's movies: `movie/ds_advertise.pam`, then `movie/ds_title.pam`, each ending at once, as the runtime plays movies through FFmpeg, which this build has not got |
| Guest memory and threads | 469 MB in 63 mappings, 44 faults, none refused; 2,004 detached threads given back, never more than 93 at once |

The stage 9 test, the same day: the game's NRO sent with `tools/nxlink_run.py`, with
`DES_SKIP_INTRO=1` in `settings.txt`:

| What | Result |
| --- | --- |
| nxlink finding the console by itself | no: the router does not pass its broadcast. With the address the netloader shows (`--address`), yes |
| The NRO sent | 173 MB as 56 MB over Wi-Fi, written to `switch/demonsouls/demonsouls_game.nro`, and started |
| The live log | 3,619 lines, the runtime's with them, saved on the PC (`SD_LOG/nxlink_*.txt`); no stray bytes |
| `settings.txt` | `settings: DES_SKIP_INTRO=1`, then `[des] DES_SKIP_INTRO=1: boot logos skipped` |
| The game | the save of the run before found; at the title screen Start, ZL and Cross reached it, and its sound then played on, the loudest sample 0.30 to 0.38 in every 10 seconds to the end. 96 to 115 frames every 10 seconds, the log going to the network |

The stage 10 test, the same day: the game sent with `--set DESNX_CRASH_TEST=30`:

| What | Result |
| --- | --- |
| The setting, as one of the NRO's arguments | `settings, from the NRO's arguments: DESNX_CRASH_TEST=30` |
| The crash, 30 seconds in | the log's last line of ours is the crash test's; the console then shows the system's error and ends the game |
| `SD_LOG/demonsouls_crash.txt` | written: "a data access fault, translation fault: nothing is mapped there, writing; address 0x8, near zero: a null pointer", ESR 0x92000046, all the registers, four frames of call stack, and "the PS3 code: none on this thread" (the log's own thread) |
| Its addresses, named on the PC | pc `watch_thread+0x1c4`, the write itself, lr `watch_thread+0x1b4`, then libnx's `__thread_entry` and `_EntryWrap`: the whole stack, right |

The stage 11 tests, 2026-11-06 and 07: two runs of the game's NRO to the title screen, Start and
Cross, the first with the game's cache on the SD card, the second with it in memory
(`nx_cache_mem.c`):

| What | The cache on the SD card | The cache in memory |
| --- | --- | --- |
| Read from the disc, to the title screen | 103 MB in 5.5 s (19 MB/s, 1,889 reads, 314 files) | 103 MB in 5.0 s (21 MB/s, 1,889 reads, 314 files) |
| Copies written to the cache | 39 MB in 10.4 s (4 MB/s, 850 writes, 139 files) | 39 MB in 32 ms (850 writes, 139 files) |
| All 139 copies made | by the log's line at 40 seconds | by its line at 30 seconds |
| Memory the cache holds | | 49 MB of its 512 at the title screen, 52 MB after the menu |
| Read back from the cache | nothing yet: the title screen loads each file once | the same |
| The end | 87 seconds in, the system's blue screen (below) | closed from the HOME menu after 130 seconds |

The blue screen came from the system, not the game: Atmosphère's crash report names `hid`, the
service that reads the controllers, with an undefined instruction where its code holds a valid one
(`stur x11, [x8, #-26]`). The processor ran something other than what memory held, which points at
the hardware under load (the game keeps three cores busy). That console had an overclocking module
loaded, at the stock clocks. The game's own crash report was not written, since the game did not
crash. The same build had run 90 seconds before without it, and the next run 130.

The stage 12a test, 2026-11-07: the game with the portable renderer's core, on the GPU layer's
null backend (it records and checks every command and draws nothing), to the title screen, Start,
Cross and Circle:

| What | Result |
| --- | --- |
| The route | `[rsx_out] route null (the new path on the null backend: checks every command, draws nothing)` |
| The GPU layer's checks | 3,685 frames presented, each `0 failed checks` |
| The game's frames, 10 seconds at a time | 116, 251, then 294 to 301 to the end: 30 a second, the PS3's rate. The old null backend gave 108 to 110: it drew every triangle in software |
| The rest | as before: the save loaded, the cache in memory (139 files, 49 MB), guest memory 469 MB in 63 mappings, nothing refused; closed from the HOME menu after about 125 seconds |

The stage 12b tests, the same day: the game on the deko3D backend (`src/gpu`, docs/deko3d.md),
three runs to the title screen, Start and Cross:

| What | Result |
| --- | --- |
| The route and the device | `[rsx_out] route deko3d`; the device, a swap chain of two 1280x720 images on the default window, the core's targets and the present blit's pipeline from its GLSL shaders |
| The first run | the GPU's work went on, but every frame logged "the device is lost": the core asks for a fault report after every submit, and the backend gave one even with nothing lost. Fixed. The core's texture pool (263,200 descriptors) did not fit the 65,536 made; now 524,288 |
| `DESNX_DK_TRACE=4000`: the first 4,000 commands each sent and waited for on its own | none left the queue in its error state: clears, depth clears, copies between textures, a copy back to memory, passes, the blit's pipeline and draw |
| `DESNX_DK_TINT=1`: a clear to black as a colour that goes round the hues | the screen's colour changed slowly, as it should: the clears, the copy into the display target, the blit and the presents reach the screen |
| The rest | 30 frames a second; 412 MB of GPU memory in 10 blocks, steady; every fence passed (6,601 of 6,601); Start and Cross reached the game. The game's own draws are left out: their programs have no shaders yet (`no DKSH for vp yet`), which is stage 12c |

The stage 12c tests, the same day: the game's shaders compiled on the console (HLSL to SPIR-V,
GLSL and DKSH, `src/gpu/gpu_dk_sc.cpp`, docs/deko3d.md), cached on the SD card:

| What | Result |
| --- | --- |
| The first run | the boot stood still with a dark blue screen (the renderer's own, with no frame from the game): with the spv target in the runtime, Soulbrandt's shader pass at the start compiled every program of the game's archives for Vulkan's cache. It is off on the Switch now (`DES_PRECOMPILE=0`) |
| The second run, on the screen | **the game draws**: the disclaimer, the title menu, New Game, and on to character creation, which was dark |
| The compiles | the first, 154 ms; then a vertex program of 7 instructions held the frames for about 50 seconds and one of 67 for about 90: the game draws on the thread that compiles |
| Where the time went (`DESNX_DK_SHADER_TIMES=1`) | almost every program: glslang and SPIRV-Tools 11 to 260 ms, SPIRV-Cross under 20, uam under 300. The slow ones are the vertex programs that read their own vertices (`guest_vp2`): 3.7 KB of HLSL became 122,217 SPIR-V words and 444 KB of GLSL, and uam took 66 seconds; 10.5 KB, 138,040 words, 481 KB and 89 seconds. Their fetch unrolled 16 attributes times 4 components times every format's loads |
| Their fetch, only the attributes read, its components' loop a loop (`rsxc_dk.c`) | the same programs: 10,216 words, 31 KB of GLSL, uam 3 seconds; 32,796 words, 90 KB, 16.5 seconds; 36,485 words, 102 KB, 22 seconds. Of uam's time, Mesa's GLSL front end takes about 70% and nouveau the rest; Mesa's optimization loops run once each past 20 KB of GLSL (`GLSLOptimizeConservatively`), which took a fifth off |
| On the screen | fewer stops; character creation dark, and the game deaf to the buttons there while it compiled its three longest programs, now on the SD card |
| The next run, those programs on the SD card | character creation compiled four more of its vertex programs (11 to 16 seconds each), then ran at 30 frames a second, took the d-pad and Circle, and **was drawn perfectly**: it lightened after a moment and the models came, with their textures |
| The sound | the mixer at 48,000 frames a second, its queue never empty; on the console a strong hiss in the game's sound, not in the numbers. It was there before stage 12 too |

The stage 13 test, the same day: a new character, named, into the first level:

| What | Result |
| --- | --- |
| The name at character creation | the Switch's keyboard (`src/platform/nx_osk.c`, on ps3recomp's on-screen keyboard host): "joy" typed and taken. Before it, the PS3's keyboard answered at once with an empty name |
| The first level | **the tutorial played**: the knight in the Boletarian palace's canal, its light, fog and water, the HUD, the first enemy killed; the loading screen too. About 10 to 16 frames a second there, and the first entry's compiles held the frames for some minutes in all (17 programs compiled, 34 from the SD card) |
| What is wrong on the screen | thin white lines down the whole picture at the same columns, in the level and on the loading screen, and small white squares that flash down it for an instant |
| Other lines in the log | `Multistream: atrac failed to load` (a sound stream); a save could not write its icon (`ICON0.PNG`) and kept the old one |

The stage 13 hunt for the lines and squares, the next day, in the same canal. The user's pictures
show two faults: in the water, each column one colour from top to bottom (the white lines of the
first report); and for a frame now and then, a regular grid of short cyan dashes over the near
part of the picture, up to a straight line across the floor:

| What | Result |
| --- | --- |
| A barrier between passes on different targets, and before copies and resolves (`DESNX_DK_PASS_BARRIER`) | kept; both faults stayed |
| New targets cleared to zero on the GPU, as a D3D12 committed resource is (`DESNX_DK_CLEAR_NEW`) | kept; no change |
| Handles with a generation (gpu.h: a handle names one object for the device's life) | the backend gave a texture made in a slot just freed the old texture's handle, and the core's table memo, which compares an entry by its description, kept the old image in the entry. Fixed; no change on the screen |
| Targets without the hardware's compression (`DESNX_DK_COMPRESS=0`) | no change |
| `SCREEN_AS_TEX=0` and `RSX_DEPTH_AS_COLOR=0` | the whole picture became the water's streaks, the HUD right; `RSX_DEPTH_AS_COLOR=0` alone left the picture as it was. A display-sized texture read from guest memory, where the core puts the last frame's copy, is streaked the same way |
| The census (`DESNX_DK_CENSUS=1`) | 38 targets in the session, nearly all made once: 1280x720 colour and depth, a 2048x2048 shadow map, reductions of 640x360, 320x180, 160x90, 64x64 and 16x16; each is read back to guest memory, and one is copied to a texture |
| `FP_IDCOLOR=h`: each of the scene's programs a flat colour | the game's exposure took the scene to white; the squares came in the same cyan, so no scene program draws them |
| Read in the code and ruled out | deko3D's copies between buffers and images (its source), the uniform buffers' sizes and alignment, the vertex inputs' locations (the spv target's E1), the texture decode, `gl_FragCoord`'s origin (uam takes it as upper left), the depth read as colour (`rsx_d2c` gives no blue) |
| The saves | every save after its folder's first had failed: ps3recomp writes a file to `.<name>.tmp` and renames it over the old one, and Horizon's file system renames onto no file that exists, so the game's icon failed first and the game wrote none of its data. Now the old file goes to a hidden name first (`cellSaveData.c`, third_party/README.md). Two autosaves in the level then completed, and the next boot loaded the files they wrote |

The stage 14 tests, the same day, in the tutorial: surfaces, textures and the geometry the GPU
reads, with two new tools, the targets of a frame written to the SD card (`DESNX_DK_RT_DUMP`) and
a description of each draw into a small target (`DESNX_DK_DRAW_PROBE`):

| What | Result |
| --- | --- |
| uam's integer division | Maxwell has none, and uam makes one from a float reciprocal with no correction: an exact multiple can come out one short. The vertex fetch's frequency divider used it; it now divides exactly (`rsxc_dk.c`), and uam's "loss of precision" warning is gone from the log. The faults stayed |
| The targets of a frame (four dumps by the canal) | every colour target smaller than the screen has all its rows the same: the depth of field's 640x360 pair, the bloom's 320x180 and 160x90, the exposure's 64x64 and 16x16 (Soulbrandt's docs/37 names this chain). The 1280x720 targets and the 128x128 one are right. The row they hold is the scene's near its top, and a 1280x720 target that the water reads is one of them blown up, so the streaks are the water and the sky reading the chain |
| The chain's draws (the probe) | right: two triangles over the whole target, positions in its pixels, texture coordinates 0 to 1, the viewport the target's size, the vertex constants one texel of the scene, the texture scale 1, unit 0 the 1280x720 scene as a plain 2D view. They come through the input layout (the CPU's vertices, 16 attributes); the draws that read their own vertices, the 128x128 target's, come out right |
| The components' loop of the vertex fetch unrolled (`DESNX_DK_FETCH_LOOP=0`) | no change |
| Every draw through the input layout (`VP_GPU_FETCH=0`) | the scene as before, the faults as before: the input layout loses nothing in general |
| Formats in the level's draws (the census and the probe) | RGBA8, BC1, R32F and depth (D24S8); B5G6R5, B5G5R5A1 and B4G4R4A4 still not checked |

The stage 15 measurements, the same day, walking the tutorial at stock clocks (1020 MHz CPU),
with `PERF=1`, `RSX_GPU_TIMES=1` (the backend's timestamps, new) and `DESNX_CPU_REPORT=1` (each
thread's time from the kernel, new):

| What | Result |
| --- | --- |
| The frame | 80 to 95 ms: 10.5 to 13 frames a second (`frames:` 102 to 133 in 10 s) |
| The GPU | 27 to 34 ms a frame: it would keep up with about 30 frames a second |
| The backend's commands (`[dk] backend`) | 1.4 to 1.6 ms a frame of the render thread |
| The core (`[PERF]`) | of the render thread's time, about half in `render_frame` (recording some 2,500 draws a frame) and half in `guest` (waiting on the game) |
| The CPU | 252% to 296% of a core in all, of the three the game has: it is full. The SPU that runs the game's heavy jobs (`spu_exec_thread_proc`, Soulbrandt's "image 2") 71% to 83%; the main SPURS thread (`spu_async_thread`) 30% to 45%, and each job chain walk's 10% to 15%; a runtime thread (`thread_trampoline`) about 20%; HighGraphics (the game's drawing thread) about 17%; the game's main thread about 15%; ChrInsManThread about 15% |

So the frame rate is the CPU's: the SPUs' emulation and the game's own threads on three A57
cores, not the renderer. The title screen keeps 30 frames a second.

## Not checked yet

The tutorial's two faults (stages 13 and 14, left for later at the user's choice): the streaks in
the canal's water and in the sky, and the cyan squares that flash over part of the picture,
sometimes all of it. Neither stops the game. Both come from the post-processing chain: every pass
that reads the full-size scene into a smaller target samples one row for the whole target, though
its vertices, constants, texture and view are right (the stage 13 and 14 tables). Left to look at:
the GLSL and DKSH of those passes' programs (the vertex programs without a fetch of their own,
and the one-sample downsample `0xB41801`), which only the console's compile shows.

How late the sound is with 1024-frame buffers: two of them in the audio renderer (43 ms) and
cellAudio's queue on top. Stage 7 measures it with the game's own sound; if it is too much,
cellAudio gets its own audio renderer voice with more, smaller buffers in the queue.

Which controller slots are connected: SDL2 lists all eight, and stage 7 has cellPad report only
the ones in use.

The game's cache in a level. The game copies files it loads into `/dev_hdd1` and reads the
copies on later loads (Soulbrandt's docs/101: 173 copies during the load into 1-1, about 1,000
files and 340 MB in a long session). It is now in memory, up to 512 MB, and files past that go to
the SD card; the title screen fills 52 MB and reads none back. A load into the Nexus and a level,
with the cache's reads and the memory it takes, has not been on the console yet: it needs a
character, made in the game's menus, so it comes after stage 12 puts them on the screen.

The system's blue screen in `hid` (the stage 11 tests): once so far, on a console with an
overclocking module loaded. If it comes back, the next run is without the module.

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
while they run is a crash. So far every run has been closed from the HOME menu.

The game's movies: the runtime plays them through FFmpeg, loaded at run time on a PC. The Switch
has no loading at run time, and devkitPro's switch-ffmpeg 7.1 would have to be linked in.

The game's speed in a level. At the title screen it now keeps the PS3's 30 frames a second, on
the portable renderer's null backend (the stage 12a test); the 11 before were the old null
backend's, which drew every triangle in software. What drawing for real costs, and a level,
are stages 16 and 17.

Two of the game's imports have no HLE handler and get a faked CELL_OK: NIDs 0xAF505DEF (called
from 0x00E491C4) and 0x36D0C2C5 (from 0x00E1240C), which neither ps3recomp's NID database nor
Soulbrandt's notes name. The game goes on to its title screen past both.

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
| 8. First boot of the game, with a fallback renderer | **done**: on the null renderer the game boots to its title screen and runs its loop at about 11 frames a second, with its sound; Start and Cross reach it, it makes its save and plays its menu sounds. Nothing is drawn on the screen until the deko3D renderer (stage 12), and the movies need FFmpeg |
| 9. Test harness: nxlink and the SD card, logs in `SD_LOG` | **done**: the logs in `SD_LOG`, whole and readable; `tools/nxlink_run.py` sends an NRO to its place on the SD card and keeps its live log on the PC; `settings.txt` sets the runtime's options; `tools/summarize_log.py` sums a log up |
| 10. Crash triage: alignment faults, missing system calls, null pointers | **done**: a crash writes `SD_LOG/demonsouls_crash.txt` (the fault decoded, registers, call stack, the PS3 code), `abort` and `assert` too, and `tools/crash_symbolize.py` names its addresses; the log summary lists the runtime's missing functions and system calls, unlifted calls and the guest's null pointers. Checked on the console with a crash on purpose; no alignment fault has happened yet |
| 11. Asset loading: archives, materials, shaders, streaming | **done** to the title screen: where both forms of a file are on the disc, the game opens the `.dcx` (114 of 114, none of the 3 GB of raw twins). The game's file reads and writes are counted by the disc, the cache and the saves: the cache's copies took 10.4 s of writing on the SD card, so the cache is now in memory (32 ms, 52 MB). A load into the Nexus, with the cache read back and a level streaming in, comes after stage 12, when the screen shows where the game is |
| 12. Native deko3D renderer | **done**, in four steps (docs/engine.md): the portable renderer's core on the Switch (12a), the deko3D backend `src/gpu` (12b), the game's shaders compiled on the console and kept on the SD card (12c), its draws, vertex input and textures (12d). The disclaimer, the title menu, New Game and character creation are drawn, the last perfectly, at 30 frames a second. A program's first compile holds the frames, once: most under half a second, the longest vertex programs 11 to 22 seconds |
| 13. The game's passes: deferred lighting, shadows, post-processing, fog | **done**: the tutorial level is drawn with its light, shadows, fog, water and post-processing, and played; the game's saves now write on the Switch and load back. Two faults are left for later (Not checked yet): streaks in the canal's water, and cyan squares that flash for a frame |
| 14. Surfaces, framebuffers, textures, geometry buffers | **done**: the level's targets, textures (RGBA8, BC1, R32F, depth) and both vertex paths checked on the console; the vertex fetch divides exactly, and a frame's targets can be written to the SD card. The post-processing chain's one-row targets, which make the streaks and squares, are left open (Not checked yet) |
| 15. Render pass, lighting, particle and post-processing tuning | **in progress**: measured in the tutorial: the GPU takes 27 to 34 ms a frame, but the CPU is full (the SPUs' emulation first), so the game runs at 10.5 to 13 frames a second |
| 16. Frames and timings against targets: Nexus, Boletaria, Stonefang, HUD, phantoms, cutscenes | to do |
| 17-19. 30 fps at 1020/460/1331 MHz, memory budget, everything on hardware | to do |
