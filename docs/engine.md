# The engine's dependencies on the Switch

Stage 3 of the roadmap: what the recompiled game needs from the machine under it, what each piece
becomes on the Switch, and what was checked on the console. Figures marked *Soulbrandt* come from
its docs (third_party/soulbrandt/docs), measured on PCs; the others from this repository's tests.

## Rendering: the RSX to deko3D

The game draws through libgcm: it writes the RSX's command buffer in guest memory, and
ps3recomp's RSX layer (`libs/video`: `sys_rsx`, `cellGcmSys`, `rsx_commands`, `rsx_dispatch`)
reads it. Soulbrandt draws those commands through its *portable renderer*
([its docs/120](../third_party/soulbrandt/docs/120-portable-renderer.md)):

| Layer | Where | On the Switch |
| --- | --- | --- |
| The core: draw records, vertex fetch, the texture cache, render targets, write-back, occlusion counts, pipeline keys | `libs/video/rsxcore` | unchanged |
| The GPU layer (HAL): devices, command recorders, buffers, textures, views, pipelines, descriptor tables, in D3D12's numbering | `libs/video/gpu/gpu.h` | unchanged |
| A backend per graphics API | `gpu_d3d12*`, `gpu_vk*`, `gpu_null*` | a new one, `gpu_dk*`, on deko3D |
| The route, chosen at start | `rsx_out.c`, `RSX_BACKEND` | `deko3d` |

So the Switch needs one backend, not a new renderer. Vulkan's (`gpu_vk*`, about 6,900 lines) is the
model: like deko3D it has explicit memory, command buffers and descriptor sets. What each part of
the HAL becomes:

| HAL | deko3D |
| --- | --- |
| device, queue | `DkDevice`, `DkQueue` |
| command recorder | `DkCmdBuf` with its command memory |
| buffer (vertices, constants, uploads, read-backs) | a range of a `DkMemBlock`, by GPU address |
| texture, view | `DkImage` with its layout, `DkImageView`, image descriptors |
| descriptor table, sampler tables | image and sampler descriptor sets |
| pipeline | bound shaders and state objects: deko3D has no pipeline objects to build |
| occlusion counts (ZPASS) | `dkCmdBufReportCounter` with the samples-passed counter |
| fences, the frame's end | `DkFence`, `dkQueueWaitFence` |
| swapchain | `DkSwapchain` on the default window |
| write-back to guest memory | CPU-cached memory blocks the guest window can see |

Stage 12 builds it in four steps, each checked on the console before the next:

| Step | What | State |
| --- | --- | --- |
| 12a | The core on Horizon, on the GPU layer's null backend: it records and checks every command and draws nothing. The route switch has no legacy renderer here, so every route is the new path's | done: 0 failed checks in 3,685 frames, 30 frames a second at the title screen |
| 12b | `gpu_dk*`: device, queue, memory, command buffers, the swap chain; clears, copies and the present, with the core's built-in passes compiled by uam at build time | done: `src/gpu` (docs/deko3d.md); 4,000 commands with no fault, the frames on the screen at 30 a second |
| 12c | The game's shaders: the core's HLSL to DKSH (below), and where that runs | next |
| 12d | Draws: vertex and index buffers, textures and samplers, the title screen on the screen | to do |

Because deko3D has no pipeline objects, the stutter Soulbrandt fights by predicting pipelines
([its docs/49](../third_party/soulbrandt/docs/49-pso-prediction-design.md)) does not exist here.
Shader compiles remain.

### Shaders

The core turns each RSX program into HLSL text (`rsx_vp_decompiler`, `rsx_fp_decompiler`). The
Vulkan route compiles that HLSL to SPIR-V with glslang (`gpu_shader_spv`). deko3D's compiler, uam,
takes GLSL and writes DKSH. So the Switch's path is:

```
RSX program ─ core ─> HLSL ─ glslang ─> SPIR-V ─ SPIRV-Cross ─> GLSL ─ uam ─> DKSH
```

It runs on the PC, ahead of time. Soulbrandt's precompile pass (`rsx_shader_precompile`) already
walks every Cg program in the game's shader archives, with the variants earlier runs recorded, and
fills a cache through a sink, one target a pass. A DKSH target fills a cache that goes on the SD
card. That cache is made from the game's own shaders, so it is built on the player's PC and never
shared. A draw whose program is not in the cache is logged and listed for the next pass; whether
uam's compiler can also run on the console, for those, is a question for stage 12. devkitPro's
switch-mesa already runs Mesa's GLSL compiler and nouveau's code generator on the console (for
OpenGL), and uam is built from the same two, so a build of uam for the Switch is one way (12c).

## Threads: the PPU and the SPUs on three cores

The PS3 runs the game on a two-thread PPU at 3.2 GHz and six SPUs. The game starts 22 PPU threads
(*Soulbrandt*, docs/23): the main thread, HighGraphics (the renderer's), the resource system's
FileLoader, Decompressor and CacheDeviceHandler, sound streaming and mixing, trophies, network.
ps3recomp runs them as host threads, and runs SPU work there too: each SPURS task on a host thread
of its own, the SPURS kernel's handler and event threads as host threads, the sound synthesizer as
a raw SPU thread.

In a level, on a PC, that keeps about 3.8 cores busy (*Soulbrandt*, docs/28): an SPU executor at
96% of a core, an SPU async thread at 71%, HighGraphics at 68%, the vblank ticker at 49%, sound
streaming at 28% (inside short sleeps), the mixer at 18% (waiting on a condition variable). Some
threads spin: CacheDeviceHandler sits at 99% of a core in lock and semaphore try-waits.

The Switch gives an application three Cortex-A57 cores at 1,020 MHz, each several times slower than
those PC cores. What follows from that for the runtime:

1. **No spinning.** A spinning thread costs a PC a spare core; here it takes one of three. Every
   wait that spins or polls (try-lock loops, the spin tail of short sleeps, mailbox polls) has to
   block instead.
2. **A fixed pool for SPU work.** SPU tasks run on a few worker threads, not one thread each.
3. **Placement by measurement.** Which thread goes on which core is set from the stage 17
   measurements, not guessed now.

### What the runtime asks of the system

ps3recomp's runtime is written against Win32. Its POSIX layer (`runtime/platform/win32_compat.c`)
builds the same calls on pthreads, and devkitA64's C library has pthreads on top of libnx, so it
builds as it is. Where it can go straight to Horizon:

| The runtime calls (Win32) | On POSIX today | On Horizon |
| --- | --- | --- |
| `CreateThread`, `_beginthreadex` | `pthread_create` | libnx `threadCreate`, with a core and a priority |
| `SetThreadAffinityMask`, `SetThreadPriority` | `pthread_setaffinity_np`, `setpriority` | `svcSetThreadCoreMask`, `svcSetThreadPriority` |
| `WaitOnAddress`, `WakeByAddress*` | a table of mutexes and condition variables | `svcWaitForAddress`, `svcSignalToAddress`: the kernel's own futex |
| SRW locks, critical sections | pthread locks | libnx `Mutex`, `RwLock` |
| condition variables | `pthread_cond` | libnx `CondVar` |
| events, semaphores | built on the above | the same |
| `QueryPerformanceCounter` | `clock_gettime` | `armGetSystemTick` (19.2 MHz) |
| `Sleep`, `SwitchToThread` | `nanosleep`, `sched_yield` | `svcSleepThread` |
| `YieldProcessor`, `_mm_pause` | `yield` | `yield` |

## The rest

| PS3 library | ps3recomp | On the Switch | Stage |
| --- | --- | --- | --- |
| cellAudio (the game's own mixer runs on top) | `libs/audio` | SDL2's audio (switch-sdl2 over audren), with 1024-frame device buffers (`src/platform/nx_game_sdl.c`) | 7 |
| cellPad | `libs/input` | SDL2's game controllers (switch-sdl2 over libnx HID): handheld, Joy-Cons, Pro Controller; only the slots with a controller in them | 7 |
| cellPamf, cellSail (the movies: H.264 and ATRAC3plus) | `libs/codec` | FFmpeg 7.1 from devkitPro's portlibs | 11 |
| cellFs, sys_fs | `runtime/ppu/ppu_fs.cpp` | `src/platform/nx_vfs.c` (stage 2) | done |
| guest memory | `vm_base` | `src/platform/nx_guest.c` (stage 1), the game's commits and faults (stage 6) | done |
| network (NP, sockets) | `libs/network` | libnx sockets | last |

## Checked on the console

The stage 3 test NRO, before its text screen, draws two seconds of a triangle through deko3D with two
shader programs uam compiled at build time (`src/shaders`, `src/platform/nx_gpu_probe.c`). Then it
measures the cores (`src/platform/nx_cpu_probe.c`). On firmware 22.5.0 (the full table is in
docs/status.md):

- **deko3D works:** device, swapchain and uam's shaders load, and 120 frames keep the display's 60 Hz.
- **Three cores, all alike:** cores 0 to 2 (core 3 is the system's) run the same work in 117 ms
  each. Priorities 28 to 59 are the game's.
- **Address arbitration is the way to wait:** a wake across cores takes 0.5 µs through
  `svcWaitForAddress`, 4.1 µs through a condition variable. The runtime's `WaitOnAddress` goes
  onto it, and so can its events and semaphores.
- **Short sleeps are exact:** 1 ms takes 1.002 ms, the longest 1.005 ms. The spin tail Soulbrandt's
  `sys_timer_usleep` adds on Windows (its docs/28) is not needed here, and would only cost a core.
- **Inflating:** zlib does 107 MB/s on one core, a first idea of what the game's decompressor costs.
