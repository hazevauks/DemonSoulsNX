# Building the port

Stage 4 of the roadmap: the port's build, with devkitA64 and CMake. One `./build.sh` makes three
things today:

| What | From | State |
| --- | --- | --- |
| `build/demonsouls.nro` | `src/` | the test NRO of stages 1 to 3 |
| `build/third_party/ps3recomp/libps3recomp_runtime.a` | `third_party/ps3recomp`, built by its own CMake | builds for Horizon: all 307 sources |
| `desnx_host`, objects in `build/CMakeFiles/desnx_host.dir` | ps3recomp's boot scaffold, the HLE NID table, Soulbrandt's game code | builds for Horizon: everything Soulbrandt's `des_boot` holds but the lifted code |

The game itself joins in two more steps, below. Nothing built from the game ever goes in this
repository, or in a release.

## The toolchain and the libraries

Stage 5. `build.sh` configures with devkitPro's own CMake wrapper (`aarch64-none-elf-cmake`), and
the top `CMakeLists.txt` takes devkitPro's toolchain file (`$DEVKITPRO/cmake/Switch.cmake`) when
none is given: devkitA64's GCC, the Switch's CPU flags, libnx, and portlibs on the search path.
Everything the port links against is declared in one place, `cmake/desnx_libraries.cmake`, as
`desnx::libraries`, for the test NRO and the game alike:

| Library | Package, as tested | For |
| --- | --- | --- |
| libnx | libnx 4.12.0 | the system: services, threads, memory, the window, the start code |
| deko3D | deko3d 0.5.0, with uam 1.1.0 for shaders | the GPU: the renderer (stage 12) |
| SDL2 | switch-sdl2 2.28.5 | game controllers and sound, for the runtime's cellPad and cellAudio |
| Mesa | switch-mesa 20.1.0 | nothing: SDL2's video brings it in, and it is never started, as deko3D owns the window. 5 MB of the game's NRO |
| zlib | switch-zlib 1.3.1 | the game's DCX and DCP archives, and the runtime |
| | devkitA64 r30, switch-tools 1.13.1 | the compiler; `elf2nro` and `nacptool` |

OpenAL Soft is installed but not linked: the runtime's cellAudio plays through SDL2 (stage 7).

At configure time each package's version is read from devkitPro's package database, the MSYS2
one on Windows or dkp-pacman's on Linux, and printed (`devkitPro packages: ...`). One older than the
table is a warning, as it may lack what the port uses; a newer one is noted, as it has not been on a
console yet.

The libraries' headers stay the platform layer's own (`PRIVATE`), and only their linking reaches the
game. Their include folders would otherwise join every lifted chunk's command line, and ninja
compiles an object again when its command line changes: hours for the chunks. Before building the
game after a change to the build, `tools/check_rebuild.py`, from the devkitPro shell, says what would
compile again:

```bash
python tools/check_rebuild.py
```

## ps3recomp's runtime on Horizon

The top `CMakeLists.txt` adds `third_party/ps3recomp` as it is, with `DESNX_RUNTIME` on (the
default). ps3recomp's CMake finds the devkitPro toolchain, SDL2 (switch-sdl2) and pthreads, and
takes Horizon for a POSIX host. What Horizon's C library does not have comes from `src/hos/`, so the
vendored files stay as they are:

| `src/hos/` | For | What it does |
| --- | --- | --- |
| `include/sys/mman.h` | the memory manager, the file mapper, the Win32 layer | `mmap` gives heap memory (zeroed) and read-only copies of files. It cannot reserve address space, or map 4 GB or more (ENOMEM), or place memory at an address (ENOSYS), and `mprotect` is accepted but not enforced. In the guest's window, `mprotect` making memory readable or writable is how the runtime commits it, and `src/platform/nx_guest.c` maps it there (`hos_guest.h`) |
| `include/dlfcn.h` | crash reports, the split build | nothing is loaded: the port is one NRO |
| `include/ucontext.h` | cellFiber | fails with ENOSYS; Demon's Souls does not use fibers |
| `include/ifaddrs.h` | cellNetCtl, which includes it without calling it | fails with ENOSYS |
| `include/sys/syscall.h` | the Win32 layer, which only calls `syscall` on Linux | empty |
| `hos_compat.h`, forced into every source | the network modules, the Win32 layer | the socket headers glibc's `netdb.h` brings in, `PTHREAD_STACK_MIN`, `timegm` and `pthread_mutex_timedlock` |
| `hos_compat.c` | all of the above | their code, and `sysconf` and `posix_memalign`, which newlib declares and libnx does not provide |
| `hos_mbedtls_config.h` | mbedTLS (the online server's TLS) | entropy from the kernel's random source, milliseconds from the system tick, no network or timing module |

One vendored file needed a change, guarded by `__SWITCH__` (third_party/README.md):
`runtime/platform/win32_compat.c` parks threads and catches faults with signals, which Horizon does
not have. There `SuspendThread` fails, the failure Win32 callers already check, and vectored
exception handlers are registered but not called. Faults on Horizon reach the process's exception
entry (`src/platform/nx_guest_fault.s`), which maps guest memory. Nothing the game needs registers
a handler: the runtime's own are debugging aids (a guard page on a watched word), and a crash
report, which would use the list, is stage 10's.

GCC 16 gives 126 warnings in ps3recomp's own code (misleading indentation, sign comparisons,
format truncation and the like); none come from `src/hos/`.

### What the runtime needs from the rest of the build

Linked whole (`--whole-archive`, no section garbage collection) into an empty program with libnx,
SDL2, zlib and deko3D, the runtime leaves 34 symbols undefined. 33 are defined by the PPU boot
scaffold, which ps3recomp compiles with each game rather than into the library:

| From | Symbols |
| --- | --- |
| `runtime/ppu/ppu_loader.cpp` | the reservation and watch state (`g_resv_store_active`, `g_ww_hi`, `g_ww_lo`, `ppu_resv_*`), guard pages, guest stack dumps, `vm_write8/16`, `vm_read8` |
| `runtime/ppu/ppu_fs.cpp` | `ppu_vfs_root`, `ppu_vm_size`, `ps3_hle_register_ctx`, `vm_write32`, `ppu_guest_caller` |
| `runtime/ppu/ppu_hle.cpp` | `ppu_guest_call`, `ps3_exit_on`, `vm_read32` |
| `runtime/ppu/ppu_sysprx.cpp` | the SPU image tables (`g_spu_image_*`, `ps3_spu_image_source_ea`) |
| `runtime/ppu/game_host.c` | `vm_base`, `ps3_sjlj_thread_reset` |

The 34th, `spurs_rt_alloc_queue`, is declared by `libs/spurs/spurs_runtime.c` and defined by no
vendored file. Nothing calls that file's functions, so an ordinary link leaves it out and never
needs the symbol.

## The host side

Soulbrandt builds the game as `des_boot` (its `port/CMakeLists.txt`): the lifted code, plus the boot
scaffold (`runtime/ppu`: `ppu_loader`, `ppu_hle`, `ppu_fs`, `ppu_sysprx`, `tests/boot_main.cpp`),
the NID table, and its own game-specific code (`port/des_*.cpp`, `missing_stubs.cpp`). Its CI builds
all of that with no lift (`DES_HOST_ONLY`), and so does this build, as `desnx_host`, with the flags
Soulbrandt uses (C17 and C++20, `-fwrapv -fno-strict-aliasing -ffp-contract=off`, no shadow stack,
no developer cheats):

- **The NID table** (`build/host/hle_nid_table.cpp`) is generated at build time by ps3recomp's
  `tools/gen_hle_nids.py --all`, from the runtime's own modules, not from the game: 1,261 handlers
  in 93 modules.
- **No launcher UI.** Soulbrandt's PC launcher (`port/ui`) is not built; its own stand-in for a
  build without it, `des_ui_stub.cpp`, is. Three calls into PC-only parts get the Switch's answers
  in `src/platform/nx_port_glue.c`: the shader cache's folder (`sdmc:/switch/demonsouls/shader_cache`),
  the problem report's per-frame hook (nothing: the Switch writes to `SD_LOG`) and the launcher's
  icon mode (not available).
- **One more guarded change** (third_party/README.md): `runtime/ppu/ppu_loader.cpp` arms a signal
  trap for stray guest pointers; on Horizon there is nothing to arm, as faults reach the port's own
  exception entry.
- `pipe()` joins `src/hos/hos_compat.c`, failing with ENOSYS: its one caller, a check before a guest
  memory dump, does without.

The host side is a set of objects, not an archive, as Soulbrandt compiles these files into
`des_boot` itself. From an archive the linker takes only what something asks for by name, and two
of the files would be left out: `ppu_hle.cpp` has an empty, weak `ppu_hle_register_all`, which
answers for the NID table's real one, so no system call would reach its handler; and
`missing_stubs.cpp` sets its hooks from a static constructor. The first game build, from the
archive, had neither.

Linked whole with the runtime, as before, the host side leaves exactly what the lift provides: the
lifted code's `function_table` and `function_table_count`, and 60 lifted functions
(`func_<address>`) that Soulbrandt's game code calls by name. Plus `spurs_rt_alloc_queue`, which an
ordinary link never asks for.

### The game's start

`boot_main.cpp` defines `main`, and it stays as it is. The game links with `-Wl,--wrap=main`, so
libnx's start code calls `__wrap_main` in `src/platform/nx_game_main.c` (a host-side file), which
gives the boot what a PC gives it and then calls the boot's `main`:

| What the boot expects | On the Switch |
| --- | --- |
| a console, or a log | the log in `SD_LOG`, with stdout too: the game has no text screen |
| the game's folder | the PS3's drives on the SD card (`nx_vfs`), and a check of the folder, `EBOOT.elf` and the libsre image, with a screen saying what is missing |
| 4 GB of address space on a 4 GB boundary | `nx_guest`'s window as `vm_base`. Off Windows the boot maps 8 GB and keeps an aligned 4 GB inside it; the shim refuses a mapping that large, and the boot goes on with the `vm_base` it finds. The RSX's 256 MB is mapped at the start |
| the EBOOT's path on its command line | `game/PS3_GAME/USRDIR/EBOOT.elf` |
| a main thread with a large stack | a thread of its own with 16 MB: the lifted code nests a host call for each guest call |

The runtime gives each PPU thread a 256 MB host stack, a reservation a PC fills only as the stack
grows. libnx takes a thread's whole stack from the heap at once, so twenty threads would take the
console's memory. With `-Wl,--wrap=pthread_attr_setstacksize`, a host stack above 16 MB gets 16 MB,
what the runtime gives SPU threads anyway; the log says how many threads and how much memory.

The game's sound and controls go through SDL2 as on a PC, with two differences the Switch's SDL2
needs, in `src/platform/nx_game_sdl.c`, again by `--wrap`:

- `SDL_OpenAudioDevice`: cellAudio asks for 256 frames a buffer, the PS3's block. switch-sdl2's
  driver plays that at 25,600 frames a second, and cellAudio paces the game's audio clock on what
  the device takes; from 1024 frames it keeps up (stage 5), so the device gets 1024.
  `SDL_QueueAudio` is counted on the way: the log gives, every 10 seconds, the frames a second the
  game's mixer queues, its loudest sample, and what is waiting.
- `SDL_IsGameController`, `SDL_GameControllerGetAttached`: switch-sdl2 lists a controller for each
  of the console's eight slots, filled or not, and cellPad would report each to the game as a
  connected pad. Only a slot with a controller in it counts. The first is player 1 together with
  the Joy-Cons in handheld mode, so the console in hand, a Pro Controller or a pair of Joy-Cons is
  the game's port 0. `SDL_GameControllerOpen`, `GetButton` and `GetAxis` log which controller is in
  which slot, and each button the first time it reaches the game.

The buttons are by position, as cellPad maps them and as the PS3's are: B is Cross, A Circle,
Y Square, X Triangle, L and R are L1 and R1, ZL and ZR are L2 and R2, minus and plus are Select and
Start. The runtime's `PAD_MAP` could swap them.

libnx's threads cannot be detached. newlib's `pthread_create` keeps only the stack's address and
size from its attributes, so every thread is joinable, and `pthread_detach` fails with ENOSYS, as
libnx leaves out the `__syscall_thread_detach` it calls. A thread gives back its kernel handle, its
stack and its control block only when another thread joins it. The runtime's `CreateThread` makes
every thread detached and joins none, and SPURS starts one for each walk of a job chain, so the
game's boot ran out of threads about a hundred in. `src/platform/nx_game_threads.c` puts a thread
created detached (`--wrap=pthread_create`) or detached later (its own `__syscall_thread_detach`)
on a list, and a reaper thread joins each one, every 100 ms, once it has ended. The log counts
them every 10 seconds (`threads:`).

A crash, a fault that is not the guest's memory, goes through the same exception entry
(`src/platform/nx_guest_fault.s`, which now saves x19 to x29 as well, for the call stack) to
`src/platform/nx_crash.c`, which writes `SD_LOG/demonsouls_crash.txt` before the kernel ends the
process: through the file system service and a buffer of its own, as the faulting thread may hold
stdio's lock or malloc's. `--wrap=abort` and `--wrap=__assert_func` write one from where they were
called. The game gives it a hook for the PS3 code of the faulting thread (the runtime's
`g_active_ctx`).

None of this needs a change to a vendored file, and all of it is link options: nothing compiles
again.

## The game's code

The lift runs on the PC, with ps3recomp's Python lifters, as Soulbrandt's README and its docs/52 lay
it out. It needs `EBOOT.elf` and `libsre.prx`, decrypted with your own RPCS3 and put in place by
`tools/check_inputs.py`. Any system software version's libsre will do: it differs from the 4.86
copy Soulbrandt is tested with only in the version written into its code, 86 times, so
`tools/check_inputs.py` sets that back (Soulbrandt's own `des_setup/firmware.py`) and the file must
then match Soulbrandt's hash exactly.

The lifters need ps3recomp's Python packages (`third_party/ps3recomp/requirements.txt`), kept in a
virtual environment of their own:

```bash
python -m venv work/venv
work/venv/Scripts/python -m pip install -r third_party/ps3recomp/requirements.txt
```

Then, from the repository's folder (`work/venv/Scripts/python` for `python`):

| Step | Command | Writes | On a 2-core laptop (i5-3210M) |
| --- | --- | --- | --- |
| analyse the EBOOT | `third_party/ps3recomp/tools/ppu_loader.py game_data/disc/PS3_GAME/USRDIR/EBOOT.elf -o work/loader` | `work/loader/` | 16 s |
| lift the PPU code | `third_party/ps3recomp/tools/ppu_lifter.py` with Soulbrandt's flags (its README, `scripts/lift_ppu.json`), `--insn-hooks @third_party/soulbrandt/port/insn_hooks.txt --output work/recomp -j 4` | `work/recomp/`: 94,017 functions in 57 chunks, 470 MB | 5.4 min |
| boot patches | `third_party/soulbrandt/scripts/apply_boot_patches.py work/recomp` | 1 patch | seconds |
| lift the SPU programs | `third_party/soulbrandt/scripts/build_spu.py --eboot <EBOOT.elf> --elf-dir work/spu_images --job-dir work/spu_jobs --ps3recomp third_party/ps3recomp --out-dir work/spu -j 4` | `work/spu/`: 12 images and 2 SPURS jobs, each checked by its fingerprint | 1.1 min |
| lift libsre | `third_party/soulbrandt/scripts/build_libsre.py game_data/firmware/libsre.prx --ps3recomp third_party/ps3recomp --work work/libsre --glue work/libsre/libsre_glue.c --runtime-image sdmc:/switch/demonsouls/firmware/libsre_image.bin` | `work/libsre/`: 756 functions, TOC 0x30039AB0 | 34 s |

The counts match Soulbrandt's (94,017 PPU functions, 756 in libsre, the same TOC).

With a lift in `work/recomp`, CMake defines the game's target. It is never part of an ordinary build:

```bash
cmake --build build --target demonsouls_game_nro -j 2
```

It compiles the 57 chunks, the SPU images and libsre with Soulbrandt's flags (`-O2 -fwrapv
-fno-strict-aliasing -ffp-contract=off`, without its x86 `-msse4.1`: `ps3emu/ps3_sse.h` gives the same
operations on ARM), and links them with the host side, the runtime and the platform layer into
`build/demonsouls_game.nro`: 166 MB, 156 MB of it ARM code, and 37 MB of zeroed data on top when it
runs. Only what changes is compiled again: the chunks only when the lift is made anew. A change to
the port's own code links again in about a minute.

The lifted code builds without debug information (`-g0`), although devkitPro's Release flags carry
`-g`. The first whole build had it: it took 203 minutes, its objects 2.1 GB and its ELF 1 GB (866 MB of
debug sections). Most of the time went to one chunk: 039 holds 12 functions of 10,000 to 12,600 lines,
on which GCC's variable tracking for the debugger took about two hours, the last 50 minutes alone.
Without it, measured two at a time:

| Chunk | With `-g` | With `-g0` | Object | Peak memory |
| --- | --- | --- | --- | --- |
| 000 | 6.6 min (alone) | 7.4 min | 39.9 MB → 7.4 MB | 1.3 GB |
| 039 | about 2 hours | 14.6 min | 36.0 MB → 6.8 MB | 1.4 GB |

The whole build without it took 134 minutes with two jobs, against 203 with three: objects 355 MB, the
ELF 188 MB. The code is the same (`.text` is 156,235,352 bytes in both, and so is the NRO), and so are
the compiler's 2,671 warnings: 2,668 from ps3recomp's `runtime/spu/spu_helpers.h`, inlined into the
lifted SPU code, where helpers fill a 128-bit value one lane at a time and GCC takes the whole value
for uninitialized. The ELF keeps every function's name in its symbol table, which is what a crash report needs;
the port's own code and the runtime keep their debug information.

On the laptop above, with the browser and other big programs closed, three chunks at a time fit in its
12 GB: up to 4.6 GB between them with `-g`, never under 2.4 GB free, no paging (measured over ten
minutes). It has two cores with four threads, so the third job adds less than a core. But Windows
Update can take memory of its own: during the second build its service held 7 GB, free memory fell to
0.2 GB with three jobs and committed memory reached 18.0 of 19.4 GB, so that build ran with two. Two is
the safe choice; four would leave too little memory for the largest chunks (14 MB of C++).

`work/` is in `.gitignore`, and `tools/check_repo.py` refuses it. The NRO that comes out holds code
made from your game: it stays on your PC and your console. `tools/make_sd.py` puts it beside the
test NRO as `demonsouls_game.nro` (the homebrew menu shows it as version "0.0.1 game"), and takes the
libsre image to `firmware/` on the SD card, where its glue looks (`PS3_LIBSRE_IMAGE`).
