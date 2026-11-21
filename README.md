# q2-atari2600

An Atari 2600 (VCS) emulator for the Shanling Q2 running Q2-Pod, built on the
**StellaDS** core — the Stella 2.8.4-era core tuned for handhelds weaker than
the Q2. It appears as **Atari 2600** in **Library → Games**.

> Status: work in progress. The StellaDS core port, Q2 frontend, build scripts
> and CI workflow are being put together. No release yet.

## What it is

- **Core:** StellaDS 8.4a (Stella 2.8.4-era 6507/TIA/RIOT emulation, 40
  bankswitch schemes, joysticks, paddles, driving controller, save states).
  The DS frontend is discarded; the Q2 frontend below drives the core.
- **Frontend:** a small native Q2 program modeled on
  [q2-boy](https://github.com/lampsonlnguyen/q2-boy)'s platform layer —
  375×320 framebuffer output, ALSA audio, wheel/buttons/touch input, and a
  simple ROM picker. One game at a time, no Cover Flow in v1.
- **Package:** a card app — a hidden `.atari` folder on the SD card root,
  launched from Library → Games. Nothing is flashed; Q2-Pod is untouched.

## Install on the Q2

1. Download the latest `q2-atari2600-alpha.zip` from
   [Releases](https://github.com/lampsonlnguyen/q2-atari2600/releases) (or
   the `q2-atari2600-alpha` artifact from a successful
   [build workflow run](https://github.com/lampsonlnguyen/q2-atari2600/actions/workflows/build.yml)).
   Extract it and copy the packaged `.atari` directory to the root of the SD
   card. Enable hidden files if your file manager hides the directory.
2. Copy your ROMs (`.a26` / `.bin`) into `atari2600/ROMs` on the SD card.
3. Open **Library → Games → Atari 2600**, pick a game with the wheel,
   press Centre to launch.

The archive includes no commercial ROMs.

## Controls (v1)

| Q2 input | Atari 2600 |
| --- | --- |
| Wheel up / down | Joystick up / down |
| Previous / Next buttons | Joystick left / right |
| Centre | Fire |
| Play | Select (console switch) |
| Touch bottom strip, left to right | Select, Reset, Color/BW, Difficulty |
| Return | Back to the ROM picker (exits the game) |

Paddle, driving-controller, keypad and Genesis-controller games load, but
their special controls are not mapped yet. See "Known gaps".

## How the port works

The StellaDS core is a stripped Stella 2.8.4 core whose frontend was the
Nintendo DS. The Q2 port keeps only `arm9/source/emucore/` and replaces the
rest:

- `src/main.c` — ROM picker, frame loop, scaling blit, audio pump, input.
  It drives the core through its public surface: `Console(image, size,
  filename)` + `Console::update()` once per frame, `myStellaEvent.set(...)`
  for joystick/console-switch input, the TIA's indexed framebuffer +
  palette for video, and TIASound's sample buffer for audio.
- `src/platform.c` / `src/platform.h` — Q2 platform layer: `/dev/fb0`
  framebuffer (375×320), ALSA audio (loaded at runtime with `dlopen`, so
  the binary stays static-friendly), GPIO buttons, and the firmware's input
  socket.
- `src/q2shim.cpp` — the handful of globals the core expects its frontend
  to provide: `myCartInfo` / `myGlobalCartInfo` (per-cart config, defaulted
  to joystick + 20 kHz sound), `mySoundFreq`, `my_filename`, and a
  `dsPrintCartType()` stub.
- `src/compat/nds.h` — a minimal stub for the `<nds.h>` include the core
  carries from its DS days. It provides nothing DS-specific; it only lets
  the core compile without the DS SDK.
- `src/build.sh` — local cross build (needs a Q2 sysroot; see below).
- `.github/workflows/build.yml` — the real build: Ubuntu 24.04 +
  `gcc-mipsel-linux-gnu`, NaN-2008 ABI probe, static MIPS ELF, QEMU smoke
  test, card-app ZIP artifact.

### Video

The TIA renders 160 pixels wide into a palette-indexed framebuffer. The
frontend converts it through the TIA palette to the 375×320 canvas, scaled
to fit and letterboxed (about 267×320 for a 160×192 NTSC frame).

### Audio

TIASound produces samples at ~20,933 Hz into its sample buffer; the
frontend drains it into ALSA each frame.

### Frame pacing

NTSC games run at 60 Hz, PAL at 50 Hz (from the cart's TV type). The
frontend paces `Console::update()` on the monotonic clock.

## Building

### In CI (the way releases are made)

Push to `feature/stellads-port` (or run the workflow manually). The
`build` workflow vendors StellaDS 8.4a, builds the static MIPS binary,
runs the QEMU smoke test with a generated minimal test ROM, checks the ELF
with `readelf`, and uploads `q2-atari2600-alpha.zip`.

### Locally

You need `mipsel-linux-gnu-g++` (Ubuntu/Debian
`g++-mipsel-linux-gnu`). No Q2 sysroot is needed: the binary is
statically linked, and ALSA is loaded at runtime with `dlopen` (see
`src/platform.c`). Then:

```sh
git clone --depth 1 --branch 8.4a \
  https://github.com/wavemotion-dave/StellaDS.git deps/StellaDS
sh src/build.sh dist/.atari/q2-atari
```

`src/build.sh` builds the vendored core + frontend into the given output
path. To build a distributable ZIP, copy `package/.atari/q2app` next to the
binary and zip the `.atari` folder at the archive root.

## Testing

- `tools/make-test-rom.py` generates a minimal 4K homebrew ROM (solid
  colour frame, proper VSYNC/VBLANK timing) for the smoke test. It contains
  no copyrighted code.
- CI runs the MIPS binary under `qemu-mipsel` in `--smoke` mode: it loads
  the test ROM, runs 70 frames, and writes the finished frame as a PGM.
  The job fails if the frame is blank.
- On-device testing happens in
  [q2-emulation](https://github.com/lampsonlnguyen/q2-emulation) with the
  card app on the emulated SD card.

## Known gaps (v1)

- **Framebuffer source is not yet proven.** The core has two video paths:
  the indexed framebuffer (`myCurrentFrameBuffer`) + palette, and a direct
  16-bit DS framebuffer pointer (`myDSFramePointer`). The frontend reads the
  indexed path; `--smoke` mode dumps both paths so the first CI run shows
  which one the core actually fills. If the indexed path is empty, the
  frontend switches to the DS pointer.
- **Paddles / driving / keypad / Genesis pad** are not mapped to Q2 input.
  Paddle games will load but the paddle sits at one extreme.
- **Save states** are not wired up yet (the core supports them; the Q2 UI
  for save/load slots is a follow-up).
- **SaveKey / EEPROM** games: the core's SaveKey controller is installed
  for Genesis-pad games, but EEPROM persistence to the SD card is not
  implemented.
- **No fast-forward, no per-game settings UI.** Difficulty switches and
  Color/BW are touch-only for now.
- **DPC+ / CDF ARM-assisted carts** need the core's Thumbulator (ARM
  emulator); it is compiled in, but untested on MIPS.
- **PAL timing** is implemented (50 Hz pacing) but not verified on device.

## Credits and license

- StellaDS core: Copyright (c) 1995–2024 Bradford W. Mott, Stephen Anthony
  and the Stella Team; modifications by Dave Bernazzani (wavemotion-dave)
  for the DS/DSi, original DS port by Alekmaul. GPL-2.0 — see
  `licenses/StellaDS-LICENSE` in the release artifact (vendored from
  upstream at build time).
- TIA sound: Copyright (c) 1997 Ron Fries, LGPL-2.0.
- Q2 frontend, build scripts and packaging in this repo: GPL-2.0-or-later.
- The q2-boy project (same author) is the model for the Q2 platform layer
  and card-app packaging.

This project is not affiliated with Atari, Shanling, StellaDS, or the
Stella team.
