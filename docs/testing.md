# Testing on the console

## The SD card

```bash
python tools/make_sd.py
```

fills `SD_CARD/` with the NRO and your game's folder (linked, so it takes no extra space on the
PC). Copy what is inside `SD_CARD/` to the root of the SD card:

```
switch/demonsouls/
  demonsouls.nro   the test NRO: checks the console and the game's files
  demonsouls_game.nro  the game, once you have built it (docs/build.md): for your card only
  game/            your disc's folder: PS3_DISC.SFB and PS3_GAME (the game's /dev_bdvd),
                   with EBOOT.elf decrypted beside EBOOT.BIN's place
  firmware/        libsre_image.bin, made from your firmware's libsre.prx
  hdd0/            the PS3's drive (/dev_hdd0): saves; made by the port
  hdd1/            the game's cache (/dev_hdd1); made by the port
  SD_LOG/          the logs, written by the port
```

To update only the NRO, run `python tools/make_sd.py --no-game` and copy `demonsouls.nro`.

Start it from the homebrew menu while holding **R** on a game (title takeover) or through a
forwarder: as an applet (from the album) the process gets far less memory, and the log says so.

## Logs

The port writes to `sd:/switch/demonsouls/SD_LOG/`:

| File | What it is |
| --- | --- |
| `demonsouls_log.txt` | everything the port printed; `demonsouls_log_previous.txt` is the run before |
| `demonsouls_crash.txt` | a crash: the address, the call stack, the registers (from stage 10) |
| `demonsouls_fatal.txt` | a fatal error's message |

Copy them into this repository's `SD_LOG/` to look at them; git ignores them.

## nxlink

For a live log, with the console on the same network and the homebrew menu open:

```bash
nxlink -s build/demonsouls.nro
```

The log then goes to the PC and to the SD card at the same time.

## What the test NRO shows

The launch mode, the memory the process gets, whether guest memory works, and whether the game's
folder is found. The log adds the firmware version, the address space and its regions, and each
step of the guest memory test with its timing: stage 6 is built on these figures.

The guest memory test touches memory nothing has mapped yet, on purpose. If the log stops at
`touching unmapped guest memory`, faults do not reach the NRO and the console shows an error; the
log up to there is what is needed.

Stage 3 starts with two seconds of a triangle on a dark screen: that is deko3D, before the text
screen. Its `gpu:` lines give the frames and their timing, and the `cpu:` lines the cores, how
fast threads wake each other, and how accurate a short sleep is.

Stage 5 starts SDL2 first, the way the runtime's pad and sound modules do. The sound device is
opened four times, with buffers of 256, 512, 1024 and 2048 frames, and timed for half a second
each in silence; then two short, quiet tones play just before the triangle, the first with 256
frames a buffer (what the sound module asks for), the second with the smallest size that kept up.
If the first sounds rougher than the second, say so. The screen's `Controls:` line gives the
controller SDL2 sees, and `Sound:` the frames each buffer size played a second (48,000 is right).
The `Buttons:` line at the bottom shows, live, what SDL2 reads from the controller. Press every
button once, the sticks and ZL and ZR too: the log's `sdl:` lines note each button by SDL2's name
the first time, which is how the console's buttons reach the game's pad module (stage 7 maps
them).

Stage 2 adds the `files:` lines: the game's first files opened by the PS3's paths (one in the
mixed case the game uses), three DCX files inflated, the SD card's speed for a long read, a seek
and a small file, the game's way of writing its cache (in `hdd1/`, removed again), and which of the
files decrypted on the PC are on the card. It reads about 80 MB and takes a few seconds.

## What the game NRO shows

`demonsouls_game.nro` is the game itself; in the homebrew menu it is the "Demon's Souls" whose
version reads "0.0.1 game". Start it the same way, with title takeover. If something it needs is
missing (the game's folder, `EBOOT.elf`, the libsre image, enough memory), a text screen says what,
and + exits. Otherwise the screen stays black: the game draws nothing on the Switch yet (stage 8).

Stage 6 is the guest memory under the game. The log's `guest memory` lines give the window and
what is mapped at the start, then, every 10 seconds while it changes, how much the game has
mapped, in how many pieces, how many faults were answered, and the threads' host stacks. Between
them come the boot's own lines (`[boot]`, `[boot-dbg]`, `[SYS]` and the like), as far as the game
gets. Give it a minute; if it does not end by itself, close it from the HOME menu, and copy the
log.

Stage 7 adds the sound and the controls. The game starts reading its pad before the point where it
stops for now (about 20 seconds in), so press every button once as soon as the screen goes black: B, A, Y, X, L, R, ZL, ZR, minus, both sticks pressed in and moved, the d-pad, and plus last
(the game's Start). Each `controls:` line names a button the first time it reached the game, with
the PS3 button the game got, and which controller is in which slot. The `sound:` lines, every 10
seconds, give the frames a second the game's mixer queued (48,000 is right), its loudest sample
(above 0 means the game played something), and how much was waiting.
