// SPDX-License-Identifier: GPL-2.0-or-later
// Q2 frontend defaults for the StellaDS core (see q2shim.cpp).
#ifndef Q2ATARI_Q2SHIM_H
#define Q2ATARI_Q2SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Zero the per-cart config and fill in the v1 Q2 defaults (joystick,
// 20 kHz sound, NTSC, difficulty A). Call before constructing a
// Console for the given ROM filename.
void q2shim_defaults_for_rom(const char *filename);

// Map the DS VRAM window the core's sound buffers live at
// (TIASound.cpp: sampleExtender at 0x068A0000, tia_buf at 0x068A1000).
// Tia_sound_init() writes both during Console construction, so call
// this before the first Console exists. Returns 1 on success, 0 if
// the mapping failed (do not construct a Console then — it would
// segfault writing to unmapped DS addresses).
int q2shim_map_ds_ram(void);

#ifdef __cplusplus
}
#endif

// Direct DS video buffer owned by the shim (defined in q2shim.cpp).
// The TIA writes every scanline here through BG_GFX; the frontend
// reads it back as the DS-path framebuffer. uint16_t is the same type
// as the core's uInt16 (unsigned short).
#ifdef __cplusplus
extern uint16_t q2_ds_framebuffer[160 * 262];

// Frame counters the core reads (TIA.hxx / StellaDS.h). The DS
// frontend bumped these once per emulated frame; the Q2 frame loop
// does the same, so the flicker-free blend alternates as intended.
extern uint32_t gAtariFrames;
extern uint32_t gTotalAtariFrames;

// Sound buffer pointers (TIASound.cpp declares them extern). The DS
// frontend pointed these at its sound DMA buffer; here they point at
// shim-owned single-sample slots the frame loop reads after each
// Tia_process() call.
extern uint16_t *aptr;
extern uint16_t *bptr;
#endif

#endif
