// SPDX-License-Identifier: GPL-2.0-or-later
// Q2 frontend globals for the StellaDS core.
//
// The core (a stripped Stella 2.8.4) expects its frontend — on the DS,
// StellaDS.cpp/config.cpp — to provide a few globals: the per-cart
// config myCartInfo, the global config myGlobalCartInfo, the sound
// frequency mySoundFreq, the loaded ROM's name my_filename, and a
// dsPrintCartType() hook the Console constructor calls to report the
// detected cartridge type. This file provides Q2 defaults for all of
// them. v1 keeps it simple: joystick controllers, 20 kHz sound, NTSC,
// difficulty A. Per-game overrides (paddles, PAL, sound quality) are
// follow-ups; the core reads these values when a Console is created,
// so the frontend sets them before constructing one.
#include <cstdio>
#include <cstring>

#include "Cart.hxx"

// The core reads these before/during Console construction.
CartInfo myCartInfo;
GlobalCartInfo myGlobalCartInfo;
uInt16 mySoundFreq = 20933;
char my_filename[256] = { 0 };

extern "C" void q2shim_defaults_for_rom(const char *filename) {
    memset(&myCartInfo, 0, sizeof myCartInfo);
    memset(&myGlobalCartInfo, 0, sizeof myGlobalCartInfo);
    myCartInfo.controllerType = CTR_LJOY;
    myCartInfo.soundQuality = SOUND_20KHZ;
    myCartInfo.tv_type = NTSC;
    myCartInfo.left_difficulty = DIFF_A;
    myCartInfo.right_difficulty = DIFF_A;
    myCartInfo.analogSensitivity = ANA1_0;
    myCartInfo.yScale = 100;
    mySoundFreq = 20933;
    if (filename) {
        snprintf(my_filename, sizeof my_filename, "%s", filename);
    } else {
        my_filename[0] = 0;
    }
}

// Called by the Console constructor to report the detected cartridge
// type. On the DS this printed to the debug screen; here it goes to
// stderr (visible in the emulator's demo.log). Plain C++ linkage: the
// core declares it as a C++ function.
void dsPrintCartType(char *type, int size) {
    fprintf(stderr, "q2-atari: cart type %s, %d bytes\n",
            type ? type : "?", size);
}
