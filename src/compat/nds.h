// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal stand-in for the Nintendo DS SDK header <nds.h>.
//
// The StellaDS core carries `#include <nds.h>` from its DS days
// (TIA.cpp and possibly others). The Q2 build has no DS SDK, and the
// core does not need any real DS service — the include only has to
// resolve. This stub provides the pieces of the libnds surface the
// core actually uses, so it compiles without the DS SDK. If a CI
// build fails on a missing nds symbol, add the smallest possible
// definition here and note it; do not pull in the real libnds.
//
// What the core needs (found by compiling the vendored 8.4a core):
//   - libnds integer types: u8/u16/u32/s8/s16/s32 plus the plain
//     uint8/uint16/uint32/int8/int16/int32 aliases (StellaDS.h uses
//     `uint8`), and <stdint.h> for uint8_t (M6502Low.hxx uses it).
//   - ITCM_CODE: libnds marks hot code for the DS's fast ITCM;
//     on the Q2 there is no ITCM, so it is empty.
//   - isDSiMode(): libnds reports whether the host is a DSi. The Q2
//     is not one, so this returns false — the DS-Lite code path
//     (flicker-free off by default, MODE_NO frame mode).
//   - BG_GFX / dmaCopyWordsAsynch(): the TIA writes every scanline to
//     the DS video frame through these (TIA.cpp sets
//     myDSFramePointer = BG_GFX on reset and each frame, then per
//     scanline either blends into it or dmaCopyWordsAsynch()s the
//     indexed scanline there). On the DS, BG_GFX is VRAM at
//     0x06000000; on the Q2 that address is unmapped, so the core
//     would segfault on the first frame. BG_GFX is therefore a real
//     buffer owned by the Q2 shim (q2_ds_framebuffer, defined in
//     q2shim.cpp), and the DMA copy is a plain memcpy — libnds's size
//     is in 32-bit words. The frontend reads the buffer back as the
//     DS-path framebuffer (see main.cpp pick_frame()).
#ifndef Q2ATARI_COMPAT_NDS_H
#define Q2ATARI_COMPAT_NDS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;

#define ITCM_CODE

static inline bool isDSiMode(void) { return false; }

// 160x262 u16: the TIA advances myDSFramePointer 128 u16 per scanline
// and writes up to 160 words per scanline, so 192 visible lines need
// at most 192*128 + 160*2 u16 — well inside this buffer.
extern uint16_t q2_ds_framebuffer[160 * 262];
#define BG_GFX (q2_ds_framebuffer)

static inline void dmaCopyWordsAsynch(int channel, const void *src,
                                      void *dest, int size) {
    (void)channel;
    memcpy(dest, src, (size_t)size * 4);
}

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif
