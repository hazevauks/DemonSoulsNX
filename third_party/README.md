# Third-party sources

Copies of the projects the port is built on, at one exact commit each, so a change of theirs never
breaks the build and every change of ours is a plain diff. Their git history is not included.

| Folder | Project | Commit | License |
| --- | --- | --- | --- |
| `ps3recomp/` | [JonathanDC64/ps3recomp](https://github.com/JonathanDC64/ps3recomp), Soulbrandt's fork of [sp00nznet/ps3recomp](https://github.com/sp00nznet/ps3recomp) | `63a7197a2c129a3af7567a5f811ce2b624df0796` (tag `des-0.14.0-3`, the one `soulbrandt/ps3recomp.pin` names) | MIT |
| `soulbrandt/` | [JonathanDC64/Soulbrandt](https://github.com/JonathanDC64/Soulbrandt) | `1741c0a6bd3f4d056b63db73a708b7282f1ea438` (0.1.0 and four fixes) | MIT |

Each folder keeps its own `LICENSE` and `README.md`.

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
| `ps3recomp/libs/spurs/cellSpurs.c`: a ring of the last 4,096 SPURS events (chain runs, walks, jobs, shutdowns, joins; event flag sets, clears, waits and the SPU's writes to a flag), printed once, for the chains and that flag, when a PPU wait on a flag blocks a second | the game's boot blocks on an event flag, and the runtime's own logs of these stop after their first few lines |

Changes made for the Switch are marked `DemonSoulsNX` in the code. Everything else the runtime needs
from Horizon comes from `src/hos/`, outside these folders.
