// SPDX-License-Identifier: GPL-2.0-or-later
// Forced prefix header for every translation unit of the Q2 build
// (build.sh and CI pass -include src/compat/q2prefix.h).
//
// The vendored StellaDS core was written against the DS toolchain,
// where <nds.h> transitively provided the standard integer/string
// headers. Some core files use uint8_t / memset without including
// <cstdint> / <cstring> themselves (e.g. M6502Low.hxx uses uint8_t
// but Console.cpp never includes <nds.h> before it, and TIASound.cpp
// uses memset). Rather than patching vendored files that CI clones
// fresh, include the headers here for every TU.
#ifndef Q2ATARI_Q2PREFIX_H
#define Q2ATARI_Q2PREFIX_H

#ifdef __cplusplus
#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#else
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#endif

#endif
