// SPDX-License-Identifier: GPL-2.0-or-later
// Q2 frontend defaults for the StellaDS core (see q2shim.cpp).
#ifndef Q2ATARI_Q2SHIM_H
#define Q2ATARI_Q2SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

// Zero the per-cart config and fill in the v1 Q2 defaults (joystick,
// 20 kHz sound, NTSC, difficulty A). Call before constructing a
// Console for the given ROM filename.
void q2shim_defaults_for_rom(const char *filename);

#ifdef __cplusplus
}
#endif

#endif
