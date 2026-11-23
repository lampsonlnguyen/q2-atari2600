#!/bin/sh
# Build q2-atari for the Shanling Q2 (MIPS32r2, NaN-2008 hard-float).
# Static binary: no Q2 sysroot is needed at link time, because ALSA is
# loaded at runtime with dlopen (see src/platform.c) and the C/C++
# runtimes are linked statically.
#
# Usage:
#   STELLADS=deps/StellaDS sh src/build.sh [output]
#
# STELLADS defaults to deps/StellaDS (clone it first):
#   git clone --depth 1 --branch 8.4a \
#     https://github.com/wavemotion-dave/StellaDS.git deps/StellaDS
# CXX_Q2 defaults to mipsel-linux-gnu-g++ (Ubuntu/Debian
# g++-mipsel-linux-gnu). clang with --target=mipsel-linux-gnu also
# works if you pass it as CXX_Q2 with matching flags.
set -eu

STELLADS=${STELLADS:-deps/StellaDS}
CXX_Q2=${CXX_Q2:-mipsel-linux-gnu-g++}
out=${1:-q2-atari}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

test -d "$STELLADS/arm9/source/emucore" || {
    echo "StellaDS core not found at $STELLADS (see README)" >&2
    exit 1
}

# The core carries __attribute__((section(".dtcm"))) from its DS days.
# Those are orphan sections under the default linker script and link
# fine statically; if a linker ever rejects them, strip them here with
# sed before compiling instead.
"$CXX_Q2" -march=mips32r2 -mabi=32 -mhard-float -mfp64 -mnan=2008 -mabs=2008 \
    -O2 -fno-rtti -fno-exceptions -static -no-pie \
    -include "$root/src/compat/q2prefix.h" \
    -Isrc -I"$root/src/compat" \
    -I"$STELLADS/arm9/source/emucore" -I"$STELLADS/arm9/source" \
    -D_GNU_SOURCE -std=gnu++17 -Wall -Wextra \
    "$STELLADS"/arm9/source/emucore/*.cpp \
    "$root/src/main.cpp" "$root/src/q2shim.cpp" "$root/src/platform.c" \
    -o "$out" -ldl

echo "Built $out"
