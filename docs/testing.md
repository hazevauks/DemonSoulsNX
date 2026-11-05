# Testing on the console

## The SD card

```bash
python tools/make_sd.py
```

fills `SD_CARD/` with the NRO and your game's folder (linked, so it takes no extra space on the
PC). Copy what is inside `SD_CARD/` to the root of the SD card:

```
switch/demonsouls/
  demonsouls.nro   the port
  game/            your disc's folder: PS3_DISC.SFB and PS3_GAME
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

## What the stage 1 NRO shows

The launch mode, the memory the process gets, whether guest memory works, and whether the game's
folder is found. The log adds the firmware version, the address space and its regions, and each
step of the guest memory test with its timing: stage 6 is built on these figures.

The guest memory test touches memory nothing has mapped yet, on purpose. If the log stops at
`touching unmapped guest memory`, faults do not reach the NRO and the console shows an error; the
log up to there is what is needed.
