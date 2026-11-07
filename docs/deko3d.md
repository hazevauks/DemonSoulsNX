# The deko3D backend

Stage 12's GPU layer backend for the Switch (docs/engine.md): `gpu_dk*`, under the portable
renderer's contract (`third_party/ps3recomp/libs/video/gpu/gpu.h`, `gpu_cmd_ops.h`). The core
(`libs/video/rsxcore`) is compiled for it once more, as `src/gpu/rsxc_dk.c`, beside its null
instance. Its files are this project's own, in `src/gpu`; nothing of ps3recomp's changes for it
but the route switch (`rsx_out.c`), which gains the `deko3d` route.

## What each part of the contract becomes

| gpu.h | deko3D |
| --- | --- |
| device | one `DkDevice` (depth 0 to 1, origin upper left, y up: D3D12's conventions) and one `DkQueue` (graphics) |
| memory | `DkMemBlock`s of 32 MB or more, cut by a first-fit allocator, one list per kind: GPU (GPU-cached, CPU-uncached), upload (CPU-uncached), readback (CPU-cached), images, shader code |
| handles | a slot + 1 and the slot's generation (gpu.h); buffers, textures and views count theirs up when freed, so a table entry compared by its description never takes a new texture for an old one |
| buffer | a range of a block; `gpu_addr` is its GPU address (`DkGpuAddr`), so a bind needs no lookup |
| texture | a `DkImage` in image memory, from its `DkImageLayout`; a placed texture (`heap_create`) is put at its offset in the heap's own block |
| view, sampler | a `DkImageView` kept with its texture; a `DkSampler` description, kept by its bytes |
| tables | one image descriptor array and one sampler descriptor array for the device, bound in every recorder; a table pool is a run of one of them, a table its first entry |
| shader | DKSH code copied into code memory, a `DkShader` |
| layout | the slots' bindings (below); deko3D has no layout object |
| pipeline | the bound state: shaders, rasterizer, colour, colour-write, blend, depth-stencil and vertex states, kept as one record and bound together |
| recorder | a `DkCmdBuf` with its command memory, per slot |
| fences | a ring of `DkFence`s on the queue, one per timeline point asked for |
| swap chain | `DkSwapchain` on the default window, with its images' memory |
| barriers | `dkCmdBufBarrier`, gathered until the next draw, clear or copy |
| occlusion counts | `dkCmdBufResetCounter` and `dkCmdBufReportCounter` with the samples-passed counter |

## Bindings

deko3D binds per stage, in four spaces: uniform buffers, storage buffers, textures (an image and a
sampler together, by handle) and images. A layout's slots, which are D3D12 registers, go there by
these rules, and every shader made for the backend (the built-in passes' GLSL here, the game's
programs at stage 12c) uses the same:

| Slot | deko3D |
| --- | --- |
| CB `b<n>` | uniform buffer `binding = n` |
| CONST32 `b<n>` | uniform buffer `binding = n`, written into the command stream (`dkCmdBufPushConstants`) |
| RAW `t<n>` | storage buffer `binding = n - 16` from `t16` (the guest's `t20` is 4), else `n` |
| TABLE_TEX `t<j>` | texture `binding = j`, with the sampler at `s<j>` (a sampler table's entry, or a static sampler at that register); a layout with one sampler only gives it to every texture |
| vertex attribute `k` | `location = k` |

The guest pipelines' layout (`gpu.h`) is then: the vertex stage's uniform buffers 0 (`b0`) and 2
(`b2`, the push constant) and storage buffer 4 (`t20`); the pixel stage's uniform buffer 1 (`b1`)
and textures 0 to 15, each with its own sampler, as the RSX's texture units have.

## The game's shaders (12c)

Compiled on the console, the first time each is drawn with, and kept on the SD card in
`switch/demonsouls/shader_cache/dk/<key>.dksh`, the key over the chain's version, the spv
target's tag (its compilers, edits and layout), the stage, the entry point and the text:

1. the core's HLSL to SPIR-V: ps3recomp's spv target (`gpu_shader_spv*.cpp`: its text edits,
   glslang, SPIRV-Tools' legalization), in the Vulkan backend's binding model, with
   NoContraction, the invariant position and the vertex fetch clamped to t20;
2. SPIR-V to GLSL 4.60: SPIRV-Cross (`src/gpu/gpu_dk_sc.cpp`), each texture combined with its
   sampler at the texture's register, every binding moved to the rules above, the push
   constants as a uniform buffer, and y flipped back: the spv target flips it for Vulkan;
3. GLSL to DKSH: uam (`third_party/uam`), linked into the game.

glslang, SPIRV-Tools and SPIRV-Cross are fetched at ps3recomp's pins and built for Horizon
(`cmake/desnx_shader_compilers.cmake`). uam brings a copy of Mesa's GLSL compiler, as
switch-mesa's EGL does (SDL2 links it): uam goes into one object with every name it defines but
`DekoCompiler` prefixed `uam_`, so the two never meet. Soulbrandt's shader pass at the start,
which compiles every program in the game's archives for the spv target's own cache, is off
(`DES_PRECOMPILE=0`): Vulkan's cache is of no use here.

## Built-in passes

The core makes a few shaders of its own, by name (`sc_shader_for`): the present's blit
(`blit_vs`, `blit_ps`), the write-back's downscale (`down_*`), the output filter, FXAA, SMAA, HDR.
Their HLSL is fixed, so each one the backend needs is written once in GLSL in `src/gpu/shaders`,
compiled by uam at build time and embedded in the NRO; the backend's compile hook hands back the
one that has the name asked for. A pass without one fails to compile, as on a device without its
feature, and the core goes on without it.

## Steps

12b makes the device, its memory and objects, the recorders, the swap chain, clears, copies and
the present blit. The game's own programs have no shaders yet, so their draws fail to get a
pipeline and the core skips them: the screen shows what the frame's clears and copies leave.
Checked on the console (docs/status.md): 4,000 commands with no fault, and the frames on the
screen at 30 a second.

## Settings for testing it

| Setting (`settings.txt`, or `--set` with nxlink) | What it does |
| --- | --- |
| `RSX_BACKEND=null` | the null backend instead (it records and checks every command, and draws nothing) |
| `DESNX_DK_TRACE=<n>` | the first n commands that give the GPU work are each sent and waited for on their own, and the first after which the queue is in its error state is named in the log with its arguments. Slow |
| `DESNX_DK_TINT=1` | a clear to black clears to a colour going round the hues instead, so the screen shows the clears, copies, blit and presents working while the game's draws have no shaders |
| `DESNX_DK_SHADER_TIMES=1` | each compile's steps and their times in the log (without it, only compiles over half a second) |
| `DESNX_DK_SHADER_DUMP=1` | the GLSL of a program uam refuses, in the log |
| `DESNX_DK_CACHE=0` | every program compiled again, not read from the SD card's cache (still written there) |
| `DESNX_DK_CONSERVATIVE=0` or `1` | Mesa's optimization loops run to their end, or once each, for every program (default: once past 20 KB of GLSL) |
| `DESNX_DK_UAM_OPT_BIG=<0-3>` | nouveau's optimization level for a program past 20 KB of GLSL (default 3) |
| `DES_PRECOMPILE=1` | Soulbrandt's shader pass at the start, for the spv target's cache (off by default here) |
| `DESNX_DK_PASS_BARRIER=0` | no barrier of the backend's own between a pass and one on other targets, or before a copy or resolve: only the core's |
| `DESNX_DK_CLEAR_NEW=0` | new colour and depth targets left as their memory was, instead of cleared to zero before their first use |
| `DESNX_DK_COMPRESS=0` | targets made without the hardware's compression |
| `DESNX_DK_CENSUS=1` | each kind of target, copy, read-back and view that reads a texture as another format named in the log once, when it first comes, with the targets made in the present lines |
| `DESNX_DK_HLSL_DUMP=1` | each program's HLSL, as the core hands it over, to `shader_cache/hlsl` on the SD card, named by stage, length (`FP_LIST`'s) and hash. Made from the game's programs: it stays off the repository |

## Not done yet

- Compiles on the thread that draws: a long one holds the frames, once (it is then on the SD
  card). The longest so far, vertex programs that read their own vertices, take 11 to 22
  seconds, after their fetch is cut down to the attributes they read (`rsxc_dk.c`); most
  programs take under half a second. Compiling on a worker, or ahead of the game, needs the
  core to draw without a program for a while and try it again later.
- Formats checked on the screen so far: the menus' and character creation's (RGBA8, BC, depth);
  B5G6R5, B5G5R5A1 and B4G4R4A4 are mapped from their names and not yet seen.
- Barriers are coarse: any gathered barrier waits for all the GPU's work before it.
- New colour and depth targets are cleared to zero on the GPU before their first use (a list of
  their own, sent ahead of the next submit); other textures, placed or not, are not zeroed.
- No GPU timings (`gpu_ns`), no present statistics, no pipeline cache (deko3D has no pipeline
  objects to keep), and `buffer_import_host` is not offered: the guest window is not memory nvmap
  takes.
