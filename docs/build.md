# Building the port

Stage 4 of the roadmap: the port's build, with devkitA64 and CMake. One `./build.sh` makes three
things today:

| What | From | State |
| --- | --- | --- |
| `build/demonsouls.nro` | `src/` | the test NRO of stages 1 to 3 |
| `build/third_party/ps3recomp/libps3recomp_runtime.a` | `third_party/ps3recomp`, built by its own CMake | builds for Horizon: all 307 sources |
| `build/libdesnx_host.a` | ps3recomp's boot scaffold, the HLE NID table, Soulbrandt's game code | builds for Horizon: everything Soulbrandt's `des_boot` holds but the lifted code |

The game itself joins in two more steps, below. Nothing built from the game ever goes in this
repository, or in a release.

## ps3recomp's runtime on Horizon

The top `CMakeLists.txt` adds `third_party/ps3recomp` as it is, with `DESNX_RUNTIME` on (the
default). ps3recomp's CMake finds the devkitPro toolchain, SDL2 (switch-sdl2) and pthreads, and
takes Horizon for a POSIX host. What Horizon's C library does not have comes from `src/hos/`, so the
vendored files stay as they are:

| `src/hos/` | For | What it does |
| --- | --- | --- |
| `include/sys/mman.h` | the memory manager, the file mapper, the Win32 layer | `mmap` gives heap memory (zeroed) and read-only copies of files. It cannot reserve address space (ENOMEM) or place memory at an address (ENOSYS), and `mprotect` is accepted but not enforced: guest memory and its pages go through `src/platform/nx_guest.c` instead, from stage 6 |
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
exception handlers are registered but not yet called. Faults on Horizon reach the process's
exception entry (`src/platform/nx_guest_fault.s`); handing them to this list is stage 6's.

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

Linked whole with the runtime, as before, the host side leaves exactly what the lift provides: the
lifted code's `function_table` and `function_table_count`, and 60 lifted functions
(`func_<address>`) that Soulbrandt's game code calls by name. Plus `spurs_rt_alloc_queue`, which an
ordinary link never asks for. `boot_main.cpp` defines `main`, so in the game's NRO it is the entry
point; its memory and exception handling still have to go through `src/platform/nx_guest.c`, which
is stage 6.

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
cmake --build build --target demonsouls_game_nro -j 3
```

It compiles the 57 chunks, the SPU images and libsre with Soulbrandt's flags (`-O2 -fwrapv
-fno-strict-aliasing -ffp-contract=off`, without its x86 `-msse4.1`: `ps3emu/ps3_sse.h` gives the same
operations on ARM), and links them with the host side, the runtime and the platform layer into
`build/demonsouls_game.nro`. On the laptop above, one chunk alone takes about 6.6 minutes and 1.6 GB.
With the browser and other big programs closed, three at a time fit in its 12 GB: up to 4.6 GB between
them, never under 2.4 GB free, no paging (measured over ten minutes). It has two cores with four
threads, so the third job adds less than a core: about two and a half hours in all, against three
with two jobs. Four would leave too little memory for the largest chunks (14 MB of C++). Only what
changes is compiled again: the chunks only when the lift is made anew.

`work/` is in `.gitignore`, and `tools/check_repo.py` refuses it. The NRO that comes out holds code
made from your game: it stays on your PC and your console. `tools/make_sd.py` takes the libsre image
to `firmware/` on the SD card, where its glue looks (`PS3_LIBSRE_IMAGE`).
