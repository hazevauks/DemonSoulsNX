# Third-party sources

Copies of the projects the port is built on, at one exact commit each, so a change of theirs never
breaks the build and every change of ours is a plain diff. Their git history is not included.

| Folder | Project | Commit | License |
| --- | --- | --- | --- |
| `ps3recomp/` | [JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp), Soulbrandt's fork of [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp) | `63a7197a2c129a3af7567a5f811ce2b624df0796` (tag `des-0.14.0-3`, the one `soulbrandt/ps3recomp.pin` names) | MIT |
| `soulbrandt/` | [JonathanDC64/Soulbrandt](https://github.com/JonathanDC64/Soulbrandt) | `1741c0a6bd3f4d056b63db73a708b7282f1ea438` (0.1.0 and four fixes) | MIT |
| `uam/` | [devkitPro/uam](https://github.com/devkitPro/uam), deko3D's shader compiler, with a selection of Mesa's sources | `5a5afc2bae8b55409ab36ba45be63fcb73f68993` (1.1.0) | zlib-style for its own files, MIT for Mesa's (its `LICENSE`) |

Each folder keeps its own `LICENSE` and `README.md`.

Fetched at configure time, not copied, at the pins ps3recomp's `cmake/gpu_deps.cmake` names (Vulkan
SDK 1.4.363.0), for the game's shaders on the console (`cmake/desnx_shader_compilers.cmake`):
[SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) (MIT),
[SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) (Apache-2.0),
[glslang](https://github.com/KhronosGroup/glslang) (BSD-3-Clause and others) and
[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (Apache-2.0).

## What was left out or changed

No keys and no firmware are in this repository. Upstream ps3recomp carries the published
SDATA/EDAT and PKG keys, and its history once held decrypted firmware; neither is copied here.

| Change | Why |
| --- | --- |
| `ps3recomp/libs/filesystem/edat.c` rewritten without keys or decryption: it opens your own decrypted copies (`<name>.unedat`) | no keys in the repository |
| `ps3recomp/runtime/ppu/sdata_decrypt.h` reduced to a stub that decrypts nothing | the same |
| `ps3recomp/libs/filesystem/edat.h`: the comment on keys | the same |
| removed `ps3recomp/tools/pkg_extract.py`, `extract_jobmods.py`, `test_pkg_extract.py`, `triage_protos.py` and `libs/filesystem/tests/test_edat_paths.c` | they hold or need the keys |
| removed `ps3recomp/scratch/`, `ps3recomp/.github/`, `soulbrandt/.github/`, `soulbrandt/.claude/`, `soulbrandt/.trail/` | notes and CI of the original projects, with paths from their authors' machines |
| `ps3recomp/runtime/platform/win32_compat.c`: on `__SWITCH__`, `SuspendThread` fails instead of parking the thread with a signal, and vectored exception handlers are registered without installing signal handlers | Horizon has no signals (docs/build.md) |
| `ps3recomp/runtime/ppu/ppu_loader.cpp`: on `__SWITCH__`, the stray guest pointer trap arms nothing | the same |
| `ps3recomp/libs/spurs/cellSpurs.c`: a job chain's Run or Kick that arrives while the chain is being walked makes the walker walk it again, instead of being dropped, and Shutdown retires the walk in progress | Demon's Souls runs its texture chain again as soon as the last job's flag is set; a run that found the walker still finishing was lost (docs/status.md) |
| `ps3recomp/libs/video/rsxcore/rsxc_null.c` and `rsx_out.h`: the portable renderer's core and its null route are built on `__SWITCH__` too | stage 12: the core runs the game's frames on Horizon before the deko3D backend draws them |
| `ps3recomp/libs/video/rsx_out.c` and `rsx_out.h`: on `__SWITCH__` every route is the new path's, with no legacy D3D12 renderer to name or fall back on; the `deko3d` route (the port's `src/gpu/rsxc_dk.c`) is the default, `null` the other | the Switch has no D3D12 and no legacy renderer |
| `ps3recomp/runtime/ppu/tests/boot_main.cpp`: on `__SWITCH__` with `PS3RECOMP_RSX_HAL`, the boot draws through the route switch (`rsx_out`) instead of the old null backend | the same |
| `uam/`: added its parsers and lexers (bison 3.8.2 and flex 2.6.4 on `glsl_parser.yy`, `glsl_lexer.ll`, `glcpp-parse.y`, `glcpp-lex.l`, without line directives) and the three `ir_expression_operation*.h` (Mesa's `ir_expression_operation.py`, with mako 1.4.3), beside their inputs; added `CMakeLists.txt`, a library build of its `meson.build`'s sources (and let it past uam's own `.gitignore`, which leaves out `*.txt`) | the build then runs neither bison, flex nor Python for it, on the PC or on CI |
| `uam/source/compiler_iface.*`: `OutputDkshToMemory`, the DKSH `OutputDksh` writes, into memory; `uam/source/glsl_frontend.cpp`: with `DESNX_UAM_LIBRARY`, the GLSL front end's context, types and built-ins kept from one compile to the next; `SetOptimizeConservatively` (Mesa's `GLSLOptimizeConservatively`) and the two halves' times of a compile; `DekoCompiler` with default visibility | uam linked into the game (stage 12c), where each shader is compiled on the console |
| `ps3recomp/libs/system/cellSaveData.c`: on `__SWITCH__`, a save file's replace moves the old file to a hidden name before renaming the new one over it, and the log names a failed `fsync` or rename | Horizon's file system renames onto no file that exists, so every save after a folder's first failed at its first file and the game wrote none of its data |
| `ps3recomp/libs/spurs/cellSpurs.c`: a ring of the last 4,096 SPURS events (chain runs, walks, jobs, shutdowns, joins; event flag sets, clears, waits and the SPU's writes to a flag), printed once, for the chains and that flag, when a PPU wait on a flag blocks a second | the game's boot blocks on an event flag, and the runtime's own logs of these stop after their first few lines |

Changes made for the Switch are marked `DemonSoulsNX` in the code. Everything else the runtime needs
from Horizon comes from `src/hos/`, outside these folders.
