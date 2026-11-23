#!/usr/bin/env python3
"""Drive the Q2 Pod 1.9.0 emulator to the Games list and screenshot it.

Usage (run as root, from anywhere):
    python3 tools/test-emulator.py <sdcard_dir> <shot_prefix> [step]

Steps: boot -> home -> Library -> Games list. Screenshots land in
~/workspace/q2emu-work-190/<prefix>-*.png and the firmware log in
~/workspace/q2emu-work-190/work/demo.log.

Modelled on q2-pico8-fix-test/test_play.py (same q2emu API, same
/dev bind mounts in the same exec as the run).
"""
import pathlib, subprocess, sys, time

sys.path.insert(0, "/home/hatch/workspace/q2-emulation/emulator")
import q2emu

WORK = pathlib.Path("/home/hatch/workspace/q2emu-work-190/work")
OUT = pathlib.Path("/home/hatch/workspace/q2emu-work-190")
SD = pathlib.Path(sys.argv[1])
PREFIX = sys.argv[2]
STEP = sys.argv[3] if len(sys.argv) > 3 else "games-list"

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

if STEP == "home":
    q2.stop()
    print("done", flush=True)
    sys.exit(0)

q2.tap(55, 100)          # LIBRARY
time.sleep(2)
shot("library")
q2.wheel(13)             # Games (14th row)
time.sleep(2)
shot("library-games-row")
q2.key("centre")         # open Games
time.sleep(2)
shot("games-list")

if STEP == "games-list":
    q2.stop()
    print("done", flush=True)
    sys.exit(0)

# Games list order (1.9.0): Sudoku, Vortex, Solitaire, Snake,
# Atari 2600 (5th row). PICO-8 is not listed in this work dir.
q2.wheel(4)              # Atari 2600
time.sleep(2)
shot("games-atari-row")
q2.key("centre")         # open Atari 2600 -> ROM picker
time.sleep(3)
shot("picker")
q2.key("centre")         # launch the selected ROM (test.a26)
time.sleep(4)
shot("game")
q2.key("return")         # back to the picker
time.sleep(2)
shot("picker-after-return")
q2.stop()
print("done", flush=True)
