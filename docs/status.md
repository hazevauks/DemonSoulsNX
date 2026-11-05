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
| devkitPro is up to date | `pacman -Syu`: deko3D 0.5.0, libnx 4.12.0, devkitA64 r30, uam 1.1.0, switch-sdl2, switch-openal-soft, switch-ffmpeg 7.1 |

## Not checked yet

Everything that needs the console. The first test is the stage 1 NRO: it should start, show the
game's folder as found, and write `SD_LOG/demonsouls_log.txt` with the console's memory figures.

## Roadmap

| Stage | State |
| --- | --- |
| 1. Extraction, asset structure, sources | **done**, waiting on the first console test |
| 2. Asset packaging: paths, case, the file layer on Horizon, the decrypted inputs | next |
| 3. Engine dependencies: RSX/libgcm to deko3D, PPU/SPU threads to the Switch's cores | to do |
| 4. The port's build: ps3recomp's runtime and the lifted game with devkitA64 and CMake | to do |
| 5. Toolchain files: devkitA64, libnx, SDL2, deko3D | to do |
| 6. POSIX layer, alignment, guest memory on the Switch's heap | to do |
| 7. Sound (cellAudio on audren or SDL2) and controls for the Joy-Cons and Pro Controller | to do |
| 8. First boot of the game, with a fallback renderer | to do |
| 9. Test harness: nxlink and the SD card, logs in `SD_LOG` | started: the log exists |
| 10. Crash triage: alignment faults, missing system calls, null pointers | to do |
| 11. Asset loading: archives, materials, shaders, streaming | to do |
| 12. Native deko3D renderer | to do |
| 13. The game's passes: deferred lighting, shadows, post-processing, fog | to do |
| 14. Surfaces, framebuffers, textures, geometry buffers | to do |
| 15. Render pass, lighting, particle and post-processing tuning | to do |
| 16. Frames and timings against targets: Nexus, Boletaria, Stonefang, HUD, phantoms, cutscenes | to do |
| 17-19. 30 fps at 1020/460/1331 MHz, memory budget, everything on hardware | to do |
