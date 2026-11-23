#!/bin/sh
# Local MIPS build for q2-atari2600 using the Q2 firmware sysroot.
#
# The stock Ubuntu 24.04 mipsel cross toolchain cannot build for the Q2:
# its C runtime is legacy-NaN, so -mnan=2008 fails at link ("linking
# -mnan=2008 module with previous -mnan=legacy modules") — the CI
# workflow's ABI probe fails the same way. This script uses the proven
# q2-pico8 recipe instead:
#   - C library headers + shared libs from the Q2 firmware sysroot
#     ($HOME/.cache/q2/sysroot, glibc 2.28, NaN-2008), so the binary
#     links against the libc the device actually runs;
#   - C++ headers + static libstdc++ from a Debian Buster GCC 8 root
#     (Q2_GCC8_ROOT), whose runtime is compatible with glibc 2.28
#     (Ubuntu Noble's GCC 11/12 libstdc++ needs glibc 2.34+ symbols).
#   - The binary is DYNAMIC (interpreter /lib/ld-linux-mipsn8.so.1),
#     like q2-pico8's: the firmware's loader runs it, and ALSA is
#     resolved at runtime with dlopen as usual.
#
# The one deliberate ABI bridge: Ubuntu's GCC startup objects (crt1.o
# etc.) are legacy-NaN, so the link needs --no-warn-mismatch and the
# ELF e_flags are fixed afterwards with tools/mark-mips-nan2008.py.
# Those objects contain no floating-point code; the C library is the
# firmware's NaN-2008 one. The link also needs ld.so on the line
# (-l:ld-linux-mipsn8.so.1) for the loader's GLIBC_PRIVATE symbols,
# but it is removed again with patchelf right after: a DT_NEEDED on
# ld.so is what crashed q2-pico8 in the emulator (its dl-minimal
# malloc interposes ahead of libc's). The loader is already present at
# runtime as the PT_INTERP, so the removal is safe. The script asserts
# afterwards that the ELF is nan2008 and that ld-linux is NOT a
# DT_NEEDED entry.
#
# Usage:
#   Q2_GCC8_ROOT=<path to the Buster gcc8 root> sh tools/build-mips-sysroot.sh [output]
#   STELLADS=deps/StellaDS Q2_SYSROOT=$HOME/.cache/q2/sysroot sh tools/build-mips-sysroot.sh
#
# Q2_GCC8_ROOT defaults to the copy built for q2-pico8; pass it
# explicitly if that moves. To build one, see
# q2-pico8-fix-test/gcc-build/mksysroot-v136.sh (Debian Buster
# libstdc++8 deb unpacked into a root).
set -eu

STELLADS=${STELLADS:-deps/StellaDS}
SYSROOT=${Q2_SYSROOT:-$HOME/.cache/q2/sysroot}
GCC8=${Q2_GCC8_ROOT:-$HOME/workspace/q2-pico8-fix-test/gcc-build/gcc8}
CXX_Q2=${CXX_Q2:-mipsel-linux-gnu-g++}
out=${1:-build/q2-atari}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

test -d "$STELLADS/arm9/source/emucore" || {
    echo "StellaDS core not found at $STELLADS (see README)" >&2
    exit 1
}
for f in "$SYSROOT/lib/libc.so.6" "$SYSROOT/lib/ld-linux-mipsn8.so.1"; do
    test -f "$f" || { echo "Missing $f. Set Q2_SYSROOT to the Q2 firmware sysroot." >&2; exit 1; }
done
test -f "$GCC8/usr/lib/gcc/mipsel-linux-gnu/8/libstdc++.a" || {
    echo "Missing $GCC8/usr/lib/gcc/mipsel-linux-gnu/8/libstdc++.a. Set Q2_GCC8_ROOT." >&2
    exit 1
}

mkdir -p "$(dirname -- "$out")"
gcc_include="$("$CXX_Q2" -print-file-name=include)"
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT INT TERM

# Compile every TU with -mnan=2008 against the firmware's headers.
# Order matters: the GCC 8 C++ dirs come BEFORE the C library dirs,
# because libstdc++ headers use #include_next to reach the C headers
# (same order as q2-pico8's build-fake08.sh).
flags="-O2 -march=mips32r2 -mabi=32 -mhard-float -mfp64 -mnan=2008 -mabs=2008 \
    -fno-rtti -fno-exceptions --sysroot=$SYSROOT -nostdinc \
    -nostdinc++ \
    -isystem $GCC8/usr/include/c++/8 \
    -isystem $GCC8/usr/include/mipsel-linux-gnu/c++/8 \
    -isystem $gcc_include \
    -isystem $SYSROOT/usr/include \
    -isystem $SYSROOT/usr/include/mipsel-linux-gnu \
    -include $root/src/compat/q2prefix.h \
    -Isrc -I$root/src/compat \
    -I$STELLADS/arm9/source/emucore -I$STELLADS/arm9/source \
    -D_GNU_SOURCE -std=gnu++17 -Wall -Wextra"

n=0
objects=""
# shellcheck disable=SC2086
for src in "$STELLADS"/arm9/source/emucore/*.cpp "$root/src/main.cpp" \
           "$root/src/q2shim.cpp" "$root/src/platform.c"; do
    n=$((n + 1))
    obj="$build_dir/$n.o"
    # shellcheck disable=SC2086
    $CXX_Q2 $flags -c "$src" -o "$obj"
    objects="$objects $obj"
done

# Dynamic link against the firmware's glibc 2.28. -static-libstdc++
# pulls the GCC 8 static runtime; libgcc_s comes from the sysroot.
# --sysroot is needed on the link too, or the driver mixes Ubuntu's
# legacy-NaN libc with the sysroot's libs. The firmware's libc and
# libpthread reference the loader's GLIBC_PRIVATE symbols, so ld.so
# must be on the link line (-l:ld-linux-mipsn8.so.1); it is removed
# again with patchelf right after, because a DT_NEEDED on ld.so is
# what crashed q2-pico8 in the emulator (its dl-minimal malloc
# interposes ahead of libc's). The loader is already present at
# runtime as the PT_INTERP, so the removal is safe.
# --no-warn-mismatch is only for the legacy-NaN startup objects (see
# the header comment); the ELF flag is repaired below.
# shellcheck disable=SC2086
$CXX_Q2 -march=mips32r2 -mabi=32 -mhard-float -mfp64 -mnan=2008 -mabs=2008 \
    --sysroot=$SYSROOT \
    -L"$GCC8/usr/lib/gcc/mipsel-linux-gnu/8" -static-libstdc++ \
    -Wl,--as-needed -Wl,--no-warn-mismatch \
    -Wl,--dynamic-linker=/lib/ld-linux-mipsn8.so.1 \
    -L"$SYSROOT/lib" -o "$out" $objects -lm -lpthread -ldl \
    -Wl,--no-as-needed -l:ld-linux-mipsn8.so.1

patchelf --remove-needed ld-linux-mipsn8.so.1 "$out"
python3 "$root/tools/mark-mips-nan2008.py" "$out"

# Verify the result: MIPS32r2, NaN-2008, dynamic with the firmware's
# loader, and no DT_NEEDED on ld.so (the q2-pico8 crash pattern).
file "$out"
readelf=$(command -v mipsel-linux-gnu-readelf || echo readelf)
"$readelf" -h "$out" | grep -E 'Machine|Flags'
if ! "$readelf" -h "$out" | grep -q nan2008; then
    echo "error: ELF is not marked nan2008" >&2; exit 1
fi
if "$readelf" -l "$out" | grep -q INTERP; then
    "$readelf" -l "$out" | grep INTERP
else
    echo "error: expected PT_INTERP /lib/ld-linux-mipsn8.so.1" >&2; exit 1
fi
if "$readelf" -d "$out" | grep -q 'ld-linux'; then
    echo "error: ld-linux must not be a DT_NEEDED entry" >&2; exit 1
fi
echo "Built $out"
