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

Learned from q2-boy (Pocket Boy v0.4.2, `lampsonlnguyen/q2-boy`) — see
"Controls: learned from q2-boy" below for the audit. In a game:

| Q2 input | Atari 2600 |
| --- | --- |
| Wheel up / down | Joystick up / down |
| Previous / Next buttons | Joystick left / right |
| Centre (hold) | Fire |
| Play (hold) | Fire |
| Play (short tap, <300 ms) | Select (console switch) |
| Touch bottom strip, left to right | Select, Reset, Color/BW, Difficulty |
| Return | Back to the ROM picker (exits the game) |

In the ROM picker, the wheel browses, Centre launches, **Play moves to
the next ROM** (q2-boy's browser), Return quits the app.

A playable 6502 test ROM is included in the repo
(`tools/make-play-rom.py`, an original 4K game: move the ship with
the wheel, fire with Centre, Select toggles the background and the
enemy speed). It is verified in the Q2 Pod 1.9.0 emulator
(`tools/test-playable.py`): wheel direction, fire, Select, animation
and a second ROM launch all pass; the emulator log shows the audio
centred with no clipping (220832 samples, min −8220, max 16320,
mean 12 for the playtest run).

Paddle, driving-controller, keypad and Genesis-controller games load, but
their special controls are not mapped yet. See "Known gaps".

## How the port works

The StellaDS core is a stripped Stella 2.8.4 core whose frontend was the
Nintendo DS. The Q2 port keeps only `arm9/source/emucore/` and replaces the
rest:

- `src/main.cpp` — ROM picker, frame loop, scaling blit, audio pump, input.
  It drives the core through its public surface: `Console(image, size,
  filename)` + `Console::update()` once per frame, `myStellaEvent.set(...)`
  for joystick/console-switch input, the TIA's indexed framebuffer +
  palette for video, and TIASound's sample buffer for audio.
- `src/platform.c` / `src/platform.h` — Q2 platform layer: `/dev/fb0`
  framebuffer (375×320), ALSA audio (loaded at runtime with `dlopen`, so
  the binary stays static-friendly), and input: the firmware's input
  socket, with a GPIO fallback on old firmware (see Input below).
- `src/q2shim.cpp` — the handful of globals the core expects its frontend
  to provide: `mySoundFreq`, `my_filename`, the frame counters
  (`gAtariFrames`, `gTotalAtariFrames`), SaveKey state, `tv_type_requested`,
  `allConfigs` (zeroed: no saved per-cart configs, so the core falls
  through to its internal database), the sound sample slots `aptr`/`bptr`,
  the `debug` array, `dsPrintValue()` / `dsWarnIncompatibileCart()` stubs,
  the DS framebuffer buffer `q2_ds_framebuffer`, and
  `q2shim_map_ds_ram()`, which maps RAM at the DS VRAM addresses the
  TIA sound code writes to. (`myCartInfo` / `myGlobalCartInfo` are defined
  by the core itself, in `Cart.cpp`; the shim only fills them in per ROM.)
- `src/compat/nds.h` — a minimal stub for the `<nds.h>` include the core
  carries from its DS days. It provides the libnds typedefs
  (`uint8`/`uint16`/`uint32`…), `ITCM_CODE` (empty), `isDSiMode()` (false:
  DS-Lite path), and redirects the DS video hardware the TIA writes
  through — `BG_GFX` becomes the shim's `q2_ds_framebuffer` buffer and
  `dmaCopyWordsAsynch` becomes a `memcpy` — so the core never touches
  unmapped DS VRAM addresses.
- `src/compat/q2prefix.h` — forced via `-include` on every translation
  unit: `<cstdint>`, `<cstring>` and `<sys/stat.h>`, which the core relied
  on `<nds.h>` to pull in.
- `src/compat/fat.h` — an empty stub for the libfat include in
  `SaveKey.cpp` (it uses no libfat symbol).
- `src/build.sh` — local cross build (no Q2 sysroot needed; see below).
- `.github/workflows/build.yml` — the real build: Ubuntu 24.04 +
  `gcc-mipsel-linux-gnu`, NaN-2008 ABI probe, static MIPS ELF, QEMU smoke
  test, card-app ZIP artifact.

### Video

The TIA renders 160 pixels wide into a palette-indexed framebuffer. The
frontend converts it through the TIA palette to the 375×320 canvas, scaled
to fit and letterboxed (about 267×320 for a 160×192 NTSC frame).

### Audio

On the DS, `Tia_process()` ran as a TIMER2 interrupt at the sample rate.
The Q2 frame loop drives it instead: one call makes exactly one sample,
so a frame runs it `rate/fps` times. Non-WAVE samples are read from
`*aptr`/`*bptr` after each call; WAVE carts (some DPC+ and others) fill
the `tia_buf` ring, which the frame loop drains (and advances
`tia_out_idx` on, so the producer never sees a full ring). Samples are
converted from unsigned to signed stereo and written to ALSA each frame.
TIASound's samples are unipolar (0..~30720), so the `^ 0x8000` conversion
leaves a DC offset — if the audio sounds wrong on device, that
conversion is the first thing to revisit.

### Input

Input comes from the firmware's datagram socket
(`/tmp/q2-atari.input.sock`), the same protocol q2-pico8 uses. The
firmware sends:

- `k d CODE` / `k u CODE` — a key down/up, AWTK codes: Return 170,
  Centre 218, Play 171, Prev/Next side buttons 222/223 (172/173 on
  older firmware). Centre is fire (held), the side buttons are
  joystick left/right (held), Play is fire while held and pulses
  Console Select on a short tap (<300 ms), Return exits to the picker
  (on the press, before the firmware TERMs the game for holding
  Return). In the picker, Centre launches, Play moves to the next ROM,
  Return quits, the side buttons move the selection.
- `w N` — the wheel's raw travel since the last firmware UI loop pass
  (200 per turn, rising anticlockwise). One tick is 21 units, each
  event's delta is clamped to ±84, and the accumulator resets after
  300 ms idle — q2-boy's `read_wheel`. The frontend emits one
  one-frame joystick pulse per tick (up/down in a game, one row per
  tick in the picker).
- `t d X Y` / `t m X Y` / `t u X Y` — touch down/move/up in the 375×320
  landscape view. Only the down acts: in a game, the bottom strip
  (y ≥ 280) holds four console-switch zones — Select, Reset, Color/BW,
  Difficulty A/B. In the picker, a tap selects a row; tapping the
  selected row launches it.

On firmware too old to forward keys, the game grabs the `md-gpio-keys`
device itself instead, exactly like q2-boy: `main()` reads
`/tmp/q2-addon.caps` and only grabs when it lacks the word `gameview`
(q2-boy's `main.c:3588`). The grabbed device delivers `g` events with
the Linux key codes (Return 103, Centre 116, Play 108, Previous 165,
Next 163), which are translated to the matching firmware codes above,
and an `r` (EV_SYN/SYN_DROPPED) event clears all held buttons. While
GPIO is grabbed, socket `k` events are ignored, like q2-boy's
`decode_key`. An unconditional EVIOCGRAB must never be done: it stops
the firmware seeing the presses, so it never sends the datagrams
(the v0.1.0 bug).

### Controls: learned from q2-boy

The control scheme was audited against q2-boy (Pocket Boy v0.4.2,
`src/platform.c` + `src/main.c`) and aligned where it applies:

| Behaviour | q2-boy | Atari |
| --- | --- | --- |
| Key codes (socket) | Return 170, Play 171, Prev 222, Next 223, Centre 218 | Same |
| Key codes (GPIO) | Return 103, Play 108, Prev 165, Next 163, Centre 116 | Same (fallback only) |
| Centre | A / fire (held) | Fire (held) |
| Prev / Next | Left / Right (held) | Left / Right (held) |
| Wheel tick | 21 units, clamp ±84, 300 ms idle expiry | Same (was 28, no clamp, no expiry) |
| Wheel in game | Ignored | Drives Up/Down — **kept deliberately**: the wheel is the Q2's signature input and the playtest game is built around it |
| Return in game | D-pad Up (held); exit via touch Menu or Hold Return+Centre | Exits to picker — **kept deliberately**: Atari has no menu to exit through |
| Play in game | D-pad Down (held) | Fire (hold) + Select (short tap) — **kept**: what the playtest game expects |
| Picker Play | Next ROM (+1) | Same (was: launch) |
| Picker Return | Go back | Quits the app (no parent screen) |
| Console Select/Reset | Touch buttons only | Touch strip + Play-tap = Select |
| GPIO fallback | Grab `md-gpio-keys` unless `gameview` cap | Same |
| `r` (SYN_DROPPED) | Clears all held buttons | Same |

Not adopted: q2-boy's user-remappable `controls.cfg` / Physical
settings screen (Atari's mapping is hardcoded) — a future step, not
part of this change.

### Frame pacing

NTSC games run at 60 Hz, PAL at 50 Hz (from the cart's TV type). The
frontend paces `Console::update()` on the monotonic clock.

## Building

### In CI (the way releases are made)

Push to `feature/stellads-port` (or run the workflow manually). The
`build` workflow vendors StellaDS 8.4a, builds the static MIPS binary,
runs the QEMU smoke test with a generated minimal test ROM, checks the ELF
with `readelf`, and uploads `q2-atari2600-alpha.zip`.

**CI cannot currently link:** Ubuntu 24.04's mipsel C runtime is
legacy-NaN, so the workflow's NaN-2008 ABI probe fails at the link step
(`linking -mnan=2008 module with previous -mnan=legacy modules`). The
probe fails loudly on purpose — do not suppress it with
`--no-warn-mismatch`. Until CI gains a real NaN-2008 sysroot/toolchain,
use the local sysroot build below; it is the verified route.

### Locally (sysroot build — the verified route)

The Q2 firmware sysroot (`~/.cache/q2/sysroot`, glibc 2.28 NaN-2008)
plus a Debian Buster GCC 8 root for the C++ headers/static libstdc++
gives a dynamic MIPS binary that links against the libc the device
actually runs. This is the q2-pico8 recipe:

```sh
git clone --depth 1 --branch 8.4a \
  https://github.com/wavemotion-dave/StellaDS.git deps/StellaDS
STELLADS=deps/StellaDS sh tools/build-mips-sysroot.sh build/q2-atari
```

`tools/build-mips-sysroot.sh` needs `Q2_SYSROOT` (default
`~/.cache/q2/sysroot`) and `Q2_GCC8_ROOT` (the Buster GCC 8 root; see
the script header). It asserts the result is a nan2008 MIPS32r2 ELF
with the firmware's loader as PT_INTERP and no DT_NEEDED on ld.so
(the q2-pico8 loader-crash pattern). Run the QEMU smoke test with:

```sh
python3 tools/make-test-rom.py build/test.a26
qemu-mipsel -L "$Q2_SYSROOT" build/q2-atari --smoke \
  build/test.a26 build/smoke 70
```

### Locally (static build — Ubuntu toolchain, currently unlinkable)

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

- **Framebuffer source is proven.** The core fills both video paths: the
  host smoke test on the generated test ROM gives max luminance 243 on
  both indexed buffers and 146 on the DS pointer buffer, so the indexed
  path is primary and the DS pointer is a working fallback. (Before the
  fix, the TIA wrote the DS path through the DS VRAM address `0x06000000`,
  which is unmapped on the Q2 — see `src/compat/nds.h`.)
- **Paddles / driving / keypad / Genesis pad** are not mapped to Q2 input.
  Paddle games will load with the paddle parked mid-range.
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
