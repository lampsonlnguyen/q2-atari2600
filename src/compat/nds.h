// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal stand-in for the Nintendo DS SDK header <nds.h>.
//
// The StellaDS core carries `#include <nds.h>` from its DS days
// (TIA.cpp and possibly others). The Q2 build has no DS SDK, and the
// core does not need any real DS service — the include only has to
// resolve. This stub provides the basic integer types the DS headers
// define, so core files that mention them still compile. If a CI
// build fails on a missing nds symbol, add the smallest possible
// definition here and note it; do not pull in the real libnds.
#ifndef Q2ATARI_COMPAT_NDS_H
#define Q2ATARI_COMPAT_NDS_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif
