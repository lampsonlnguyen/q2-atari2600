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
//
// It also provides the rest of the DS frontend's surface the core
// links against (frame counters, SaveKey state, the per-cart config
// database, the debug scratch array, the sound buffer pointers and
// the two debug-print hooks), all zeroed — the DS frontend's
// power-on state. Without these the core does not link at all.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

#include "Cart.hxx"
#include "config.h"

// The core reads these before/during Console construction. myCartInfo
// and myGlobalCartInfo are NOT defined here — the core defines them
// itself (Cart.cpp); the shim only fills them in per ROM.
uInt16 mySoundFreq = 20933;
char my_filename[256] = { 0 };

// Direct DS video buffer (see src/compat/nds.h BG_GFX): the TIA writes
// every scanline here instead of the DS's VRAM. The frontend reads it
// back as the DS-path framebuffer.
uInt16 q2_ds_framebuffer[160 * 262] = { 0 };

// DS-frontend globals (declared extern in StellaDS.h / System.hxx /
// TIASound.cpp, defined by the DS frontend in StellaDS.cpp and
// config.cpp). The core reads them during emulation; the Q2 frontend
// owns them here, zeroed.
uInt32 gAtariFrames = 0;
uInt32 gTotalAtariFrames = 0;
uInt8 gSaveKeyEEWritten = 0;
uInt8 gSaveKeyIsDirty = 0;
uInt8 tv_type_requested = 0;
Int32 debug[32] = { 0 };
// No saved per-cart configs yet, so the core falls through to its
// internal cartridge database for every ROM.
AllConfig_t allConfigs;

// Sound buffer pointers (TIASound.cpp declares them extern). The DS
// frontend pointed these at its sound DMA buffer; here they point at
// shim-owned single-sample slots.
static uInt16 q2_sample_l = 0, q2_sample_r = 0;
uInt16 *aptr = &q2_sample_l;
uInt16 *bptr = &q2_sample_r;

extern "C" int q2shim_map_ds_ram(void) {
    // The DS core's sound buffers live at fixed DS VRAM addresses
    // (TIASound.cpp: sampleExtender at 0x068A0000, tia_buf at
    // 0x068A1000). Tia_sound_init() writes both during Console
    // construction, and Tia_process() reads the sample table on every
    // sample, so they must be real RAM before the first Console
    // exists. Map 8KB there once; the window is free in a normal
    // process (the Q2 binary loads at 0x00400000) and under QEMU
    // user-mode. Only these two addresses in the whole core are
    // hardcoded DS hardware addresses.
    static bool mapped = false;
    if (mapped) return 1;
    void *p = mmap((void *)0x068A0000, 0x2000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "q2-atari: cannot map DS sound RAM at 0x068A0000: %s\n",
                strerror(errno));
        return 0;
    }
    mapped = true;
    return 1;
}

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

// The core's other debug-print hooks (declared in StellaDS.h, defined
// by the DS frontend). dsPrintValue is called from the 6502 illegal
// opcode path; dsWarnIncompatibileCart when the cartridge database
// flags a ROM. Both go to stderr here.
void dsPrintValue(int x, int y, unsigned int isSelect, char *pchStr) {
    fprintf(stderr, "q2-atari: dsPrintValue %d,%d sel=%u %s\n", x, y,
            isSelect, pchStr ? pchStr : "");
}

void dsWarnIncompatibileCart(void) {
    fprintf(stderr, "q2-atari: warning: cartridge may be incompatible\n");
}
