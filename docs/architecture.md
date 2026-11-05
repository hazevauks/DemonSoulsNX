# How the port is put together

Demon's Souls has no source code. [Soulbrandt](https://github.com/JonathanDC64/Soulbrandt) turns
the game's own PS3 code into C with [ps3recomp](https://github.com/JonathanDC64/ps3recomp), a
static recompiler, and runs it on PC against a rewrite of the PS3's system libraries. DemonSoulsNX
takes the same code to the Switch: the recompiled game, ps3recomp's runtime built for ARM64 and
Horizon, and a native deko3D renderer in place of Direct3D 12 and Vulkan.

## From your disc to the NRO

```
your disc (.iso) ──tools/extract_game.py──> game_data/disc/ (PS3_GAME)
                                               │
                        your RPCS3 decrypts ──>│ EBOOT.elf, libsre.prx
               tools/check_inputs.py checks ──>│ them, and links the scripts' plain copies
                                               │
              ps3recomp (on the PC) lifts ───> work/recomp/  (437 MB of C++, never shared)
                                               │
       devkitA64 compiles, with the runtime ──>│ demonsouls.nro (holds code made from your game)
                                               │
                     tools/make_sd.py ──────── SD_CARD/switch/demonsouls/
```

Because the NRO holds code made from the game, it can never be published or shared, not even in
this private repository's releases. Releases can only hold the tools and the runtime; the game is
built on your PC from your copy, as Soulbrandt does.

## What goes where

| Part | Source | On the Switch |
| --- | --- | --- |
| The game's PPU code | lifted from `EBOOT.elf` by ps3recomp | compiled by devkitA64; big-endian loads and stores, AltiVec through sse2neon |
| The game's SPU programs (decompression, skinning, culling) | lifted by ps3recomp from the EBOOT | run on host threads, SPURS through ps3recomp's HLE |
| PS3 system libraries (cellFs, cellGcm, cellPad, cellAudio, SPURS, sysutil, network) | ps3recomp's runtime | the same code, with a Horizon platform layer under it |
| The RSX (the PS3's GPU) | ps3recomp's RSX layer: it reads the game's command buffer | a deko3D backend; RSX programs translated to GLSL and compiled with `uam` |
| Game-specific fixes (timing, HUD, heap, routes) | Soulbrandt's `port/` | kept, minus what is PC-only (the launcher, D3D UI, mouse) |
| Logs, crash reports, memory, threads | `src/platform/` | ours |

## The hard parts

These decide whether the port works, and each has its own stage.

1. **Guest memory.** The PS3 code uses 32-bit addresses into one 4 GB space: the game at
   0x00010000, user memory at 0x40000000 (the game asks for 164 MB of it, all at boot), the RSX's
   memory at 0xC0000000, stacks above. ps3recomp adds a base pointer to every address, and turns
   host pointers back into guest addresses by keeping their low 32 bits, so the base sits on a
   4 GB boundary. Soulbrandt reserves the 4 GB on Windows and commits 64 KB wherever the game
   first touches it.

   The first plan was `svcMapPhysicalMemory` into the alias region. The first console test showed
   it is not available: hbloader's process has no system resource, and its address space is
   36-bit (alias region 6 GB). The way now built (`src/platform/nx_guest.c`) uses what hbloader
   does give: the process's own handle. With it, `svcMapProcessCodeMemory` shows heap memory at
   any free address of the code region, and `svcSetProcessMemoryPermission` makes it read/write.
   The NRO keeps a free 4 GB window on a 4 GB boundary, maps heap memory where the game's memory
   is, and answers a fault anywhere else in the window by mapping a 64 KB piece there
   (`src/platform/nx_guest_fault.s` takes libnx's exception entry). On the console this works:
   512 MB mapped and zeroed in 90 ms, a 64 KB piece in 20 µs, a fault answered in 28 µs. Each
   piece stays a block of its own for the kernel, whose supply of blocks is limited, so the
   runtime maps the game's memory in large ranges and leaves pieces for stray touches.
2. **Cores** ([engine.md](engine.md)). The PS3 runs the game on a two-thread PPU and six SPUs. The Switch gives a game three
   1 GHz Cortex-A57 cores (a fourth in part). The SPU programs become host tasks, and the game's
   own threads share the cores. Soulbrandt's notes show HighGraphics waiting in libsre's queue for
   73-75% of its time, so the threads' waits matter more than raw speed.
3. **Size of the code.** The lift is 437 MB of C++ in 94,017 functions (Soulbrandt's docs/53), and
   its Windows build has 272 MB of machine code, of which only about 12 MB is hot. An NRO of that
   size fits in the 3.2 GB an application gets, but it is loaded whole, and the build takes hours.
4. **The renderer** ([engine.md](engine.md)). Soulbrandt draws through Direct3D 12 or Vulkan from shaders it translates from
   the RSX's programs to HLSL. The Switch needs the same programs as GLSL for `uam`, compiled ahead of
   time on the PC from the programs the game uses, and a deko3D backend for ps3recomp's RSX layer.
5. **30 fps at 1020/460/1331 MHz.** Not known yet. Recompiled code pays for byte swaps and the
   PS3's calling conventions; the Cortex-A57 is slower than the PCs Soulbrandt is tested on. The
   game ran at 30 fps on a 3.2 GHz in-order PPU, which helps: much of the work was on the SPUs.
6. **Online play.** Soulbrandt has its own server. The Switch can reach it through the same
   network layer once the rest works; it is the last stage.

## The repository

| Folder | What |
| --- | --- |
| `src/` | our own code: the Switch platform layer, `src/hos` (what ps3recomp's runtime needs from Horizon) and, for now, the test NRO |
| `third_party/ps3recomp/` | the recompiler and runtime, as Soulbrandt pins it, without its keys (third_party/README.md) |
| `third_party/soulbrandt/` | the PC port: lift scripts, game-specific code, documentation |
| `tools/` | extraction, checks, the SD card |
| `docs/` | how it works and what is tested |
| `SD_CARD/`, `SD_LOG/` | the test build to copy, and the logs copied back (filled locally, not in git) |
| `game_data/`, `work/` | your game and what is made from it (not in git) |
