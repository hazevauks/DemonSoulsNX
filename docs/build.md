# Building the port

Stage 4 of the roadmap: the port's build, with devkitA64 and CMake. One `./build.sh` makes two
things today:

| What | From | State |
| --- | --- | --- |
| `build/demonsouls.nro` | `src/` | the test NRO of stages 1 to 3 |
| `build/third_party/ps3recomp/libps3recomp_runtime.a` | `third_party/ps3recomp`, built by its own CMake | builds for Horizon: all 307 sources |

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
vendored file: the game build has to provide it, or leave that file out.

## The host side (next)

Soulbrandt builds the game as `des_boot` (its `port/CMakeLists.txt`): the lifted code, plus the boot
scaffold (`runtime/ppu`: `ppu_loader`, `ppu_hle`, `ppu_fs`, `ppu_sysprx`, `tests/boot_main.cpp`),
the NID table (`hle_nid_table.cpp`, made by ps3recomp's `tools/gen_hle_nids.py --all` from the
runtime itself, not from the game), and its own game-specific code (`port/des_*.cpp`,
`missing_stubs.cpp`). Its CI builds all of that with no lift (`DES_HOST_ONLY`). The Switch's next
step is the same: everything but the lifted code, built for Horizon and linked into the NRO, with
`boot_main.cpp`'s memory and exception handling going through `src/platform/nx_guest.c`.

## The game's code

The lift runs on the PC, with ps3recomp's Python lifters, as Soulbrandt's README and its docs/52 lay
it out. It needs `EBOOT.elf` and `libsre.prx`, decrypted with your own RPCS3 and put in place by
`tools/check_inputs.py`:

| Step | Tool | Writes |
| --- | --- | --- |
| analyse the EBOOT | `ppu_loader.py EBOOT.elf` | `work/loader/` |
| lift the PPU code | `ppu_lifter.py`, with Soulbrandt's flags (`scripts/lift_ppu.json`: four overridden functions, `port/insn_hooks.txt`, a shadow stack, 150,000-line chunks) | `work/recomp/ppu_recomp_*.cpp`, about 437 MB |
| boot patches | `scripts/apply_boot_patches.py` | `work/recomp/` |
| lift the SPU programs | `scripts/build_spu.py` (`spu_lifter.py`) | `work_spu/*/spu_recomp.c` |
| lift libsre's SPURS kernel | `scripts/build_libsre.sh libsre.prx` | `work/libsre/` |

Then devkitA64 compiles all of it into the NRO. `work/` is in `.gitignore`, and
`tools/check_repo.py` refuses it. The NRO that comes out holds code made from your game: it stays on
your PC and your console.
