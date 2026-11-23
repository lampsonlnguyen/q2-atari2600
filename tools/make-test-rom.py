#!/usr/bin/env python3
"""Generate a minimal 4K Atari 2600 test ROM (no copyrighted code).

The program runs a proper NTSC frame: 3 VSYNC lines, 37 VBLANK lines,
192 visible lines with COLUBK set to $1E, 30 overscan lines, then
repeats. Under a working emulator it produces a solid-colour frame,
which is all the CI smoke test needs to prove the core runs.

Usage: python3 tools/make-test-rom.py [output.a26]
"""
import sys

# Hand-assembled 6502, loaded at $F000 (file offset 0).
PROGRAM = bytes([
    0x78,              # SEI
    0xD8,              # CLD
    0xA2, 0xFF,        # LDX #$FF
    0x9A,              # TXS
    0xA9, 0x02,        # LDA #$02
    0x85, 0x00,        # STA $00      ; VSYNC on
    0x85, 0x02,        # STA $02      ; WSYNC
    0x85, 0x02,        # STA $02
    0x85, 0x02,        # STA $02
    0xA9, 0x00,        # LDA #$00
    0x85, 0x00,        # STA $00      ; VSYNC off
    0xA9, 0x02,        # LDA #$02
    0x85, 0x01,        # STA $01      ; VBLANK on
    0xA0, 0x25,        # LDY #37
    0x85, 0x02,        # loop: STA $02 ; WSYNC
    0x88,              # DEY
    0xD0, 0xFB,        # BNE loop (-5)
    0xA9, 0x00,        # LDA #$00
    0x85, 0x01,        # STA $01      ; VBLANK off
    0xA9, 0x1E,        # LDA #$1E     ; COLUBK
    0x85, 0x09,        # STA $09
    0xA2, 0xC0,        # LDX #192
    0x85, 0x02,        # loop: STA $02 ; WSYNC
    0xCA,              # DEX
    0xD0, 0xFB,        # BNE loop (-5)
    0xA9, 0x02,        # LDA #$02
    0x85, 0x01,        # STA $01      ; VBLANK on (overscan)
    0xA0, 0x1E,        # LDY #30
    0x85, 0x02,        # loop: STA $02 ; WSYNC
    0x88,              # DEY
    0xD0, 0xFB,        # BNE loop (-5)
    0x4C, 0x00, 0xF0,  # JMP $F000
])

# Vectors at $FFFA (NMI), $FFFC (RESET), $FFFE (IRQ): all point at $F000.
VECTORS = {0xFFA: 0x00, 0xFFB: 0xF0,
           0xFFC: 0x00, 0xFFD: 0xF0,
           0xFFE: 0x00, 0xFFF: 0xF0}


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "test.a26"
    rom = bytearray([0xEA] * 4096)
    rom[0:len(PROGRAM)] = PROGRAM
    for offset, value in VECTORS.items():
        rom[offset] = value
    with open(out, "wb") as f:
        f.write(rom)
    print(f"Wrote {out} ({len(rom)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
