#!/usr/bin/env python3
"""Playable test for q2-atari2600 in the Q2 Pod 1.9.0 emulator.

Usage (run as root, from anywhere):
    python3 tools/test-playable.py <sdcard_dir> <shot_prefix>

Steps: boot -> home -> Library -> Games -> Atari 2600 -> ROM picker
-> launch playtest.a26 -> baseline shot -> 3 s animation shot ->
wheel clockwise (player down) -> wheel anticlockwise (player up)
-> hold Centre (fire) -> tap Play (Select: bg green, enemy 2x)
-> 3 s animation shot -> Return to picker -> launch test.a26
(second ROM) -> Return to picker.

Screenshots land in ~/workspace/q2emu-work-190/<prefix>-*.png and
the firmware log in ~/workspace/q2emu-work-190/work/demo.log
(check it for the "q2-atari: audio samples=..." line after exit).

Modelled on tools/test-emulator.py (same q2emu API, same /dev
bind mounts in the same exec as the run).
"""
import pathlib, subprocess, sys, time

sys.path.insert(0, "/home/hatch/workspace/q2-emulation/emulator")
import q2emu

WORK = pathlib.Path("/home/hatch/workspace/q2emu-work-190/work")
OUT = pathlib.Path("/home/hatch/workspace/q2emu-work-190")
SD = pathlib.Path(sys.argv[1])
PREFIX = sys.argv[2]

for n in ("null", "zero", "random", "urandom", "tty"):
    subprocess.run(["mount", "--bind", f"/dev/{n}", str(WORK / "rootfs/dev" / n)],
                   capture_output=True)
root = WORK / "rootfs"
q2emu._patch_serial_check(root / "release/bin/demo")
q2emu.build_shim(root, WORK)
q2emu.prepare(root, SD, False, "")
log = open(WORK / "demo.log", "wb")
q2 = q2emu.Q2(root, log)


def shot(name):
    q2.frame().save(str(OUT / f"{PREFIX}-{name}.png"))
    print("shot", name, flush=True)


for _ in range(150):
    if q2.started():
        break
    time.sleep(1)
time.sleep(10)
shot("home")

q2.tap(55, 100)          # LIBRARY
time.sleep(2)
shot("library")
q2.wheel(13)             # Games (14th row)
time.sleep(2)
shot("library-games-row")
q2.key("centre")         # open Games
time.sleep(2)
shot("games-list")

# Games list order (1.9.0): Sudoku, Vortex, Solitaire, Snake,
# Atari 2600 (5th row).
q2.wheel(4)              # Atari 2600
time.sleep(2)
shot("games-atari-row")
q2.key("centre")         # open Atari 2600 -> ROM picker
time.sleep(3)
shot("picker")

# Picker lists ROMs alphabetically: playtest.a26 first, test.a26
# second. The first row is selected by default; launch it.
q2.key("centre")         # launch playtest.a26
time.sleep(4)
shot("game-t0")

# Animation: enemy sweeps on its own, 3 s apart.
time.sleep(3)
shot("game-t3")

# Wheel: q2emu wheel(+n) is clockwise (x falls), which the firmware
# reports as negative travel, and the fixed mapping turns that into
# Down; wheel(-n) is anticlockwise -> Up. (v0.1.0 had this inverted.)
q2.wheel(15)             # clockwise -> player down
time.sleep(2)
shot("game-wheel-down")
q2.wheel(-30)            # anticlockwise -> player up, past baseline
time.sleep(2)
shot("game-wheel-up")

# Fire: hold Centre (~1 s). A missile should be visible above the
# ship. (Play held is also fire; a short Play tap is Select.)
q2.press("centre", True)
time.sleep(1)
shot("game-fire")
q2.press("centre", False)
time.sleep(1)

# Select: short Play tap (<300 ms) toggles enemy speed and the
# background blue -> green.
q2.key("play", ms=150)
time.sleep(2)
shot("game-select")

# More animation, 3 s after the Select shot.
time.sleep(3)
shot("game-t3-after")

# Back to the picker, then launch the second ROM (test.a26).
# Picker controls (q2-boy model): Play = next ROM, wheel = browse
# (21 units/tick, q2-boy's read_wheel).
q2.key("return")
time.sleep(2)
shot("picker-after-return")
q2.key("play")           # next ROM -> test.a26 (second row)
time.sleep(1)
shot("picker-play-next")
q2.key("play")           # already last row: stays on test.a26
time.sleep(1)
q2.wheel(-1)             # anticlockwise -> up -> playtest.a26
time.sleep(1)
shot("picker-wheel-up")
q2.wheel(1)              # clockwise -> down -> test.a26
time.sleep(1)
shot("picker-test-row")
q2.key("centre")         # launch test.a26
time.sleep(4)
shot("game-test-rom")
q2.key("return")         # back to the picker
time.sleep(2)
shot("picker-final")
q2.stop()
print("done", flush=True)
