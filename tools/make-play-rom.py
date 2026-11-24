#!/usr/bin/env python3
"""Generate PLAYTEST, a small original 4K Atari 2600 game ROM for
q2-atari2600 — written for this port, no copyrighted code, no
commercial ROM.

The game: a yellow ship (Player 0) that moves with the joystick,
fires a missile with Centre or Play (held), against a red enemy
(Player 1) that sweeps left-right on its own. A missile hit scores,
flashes the background white, respawns the enemy and beeps. A short
tap of Play is Console Select: it toggles the enemy speed (1x/2x)
and the background colour (blue/green). Sound is a TIA tone while
firing / on a hit, silence otherwise.

Why this ROM exists: the v0.1.0 test ROM only draws a solid colour
field, which proves the TIA framebuffer path and nothing else.
PLAYTEST proves, in the emulator, that a sprite moves with the
joystick, fire does something, the game animates on its own, and
the audio path produces centred samples.

Registers used (TIA write $00-$2F, read $00-$0D; RIOT $280-$297):
  VSYNC $00, VBLANK $01, WSYNC $02, COLUBK $09, CTRLPF $0A,
  COLUP0 $06, COLUP1 $07, GRP0 $1B, GRP1 $1C, ENAM0 $1D,
  HMP0 $20, HMP1 $21, HMM0 $22, HMOVE $2A,
  RESP0 $10, RESP1 $11, RESM0 $12,
  AUDC0 $15, AUDF0 $17, AUDV0 $19,
  SWCHA $280 (joystick, active low: $10 up $20 down $40 left $80 right),
  SWCHB $282 (console: bit1 Select, active low),
  INPT4 $0C (fire, bit7: 0 = pressed),
  TIM64T $296, INTIM $284.

Usage: python3 tools/make-play-rom.py [output.a26]
"""
import sys

# ---------------------------------------------------------------------------
# Minimal 6502 assembler (two passes, labels, the opcodes this game uses).
# ---------------------------------------------------------------------------
class Asm:
    def __init__(self, org, consts=None):
        self.org = org
        self.consts = consts or {}
        self.labels = {}
        self.items = []  # (addr, kind, data)
        self.pc = org

    def label(self, name):
        self.labels[name] = self.pc

    def byte(self, *vals):
        for v in vals:
            self.items.append((self.pc, 'b', v))
            self.pc += 1

    def word(self, val):
        self.items.append((self.pc, 'w', val))
        self.pc += 2

    def emit(self, opcode, operand=None, mode=None):
        size = 1 if mode in ('imp', 'acc') else (2 if mode in ('imm', 'zp', 'zpx', 'zpy', 'izx', 'izy', 'rel') else 3)
        self.items.append((self.pc, 'op', (opcode, operand, mode, size)))
        self.pc += size

    def assemble(self, src):
        lines = []
        for raw in src.splitlines():
            line = raw.split(';', 1)[0].strip()
            if not line:
                continue
            lines.append(line)
        # Pass 1: sizes + labels.
        for line in lines:
            if line.endswith(':'):
                self.label(line[:-1].strip())
                continue
            self._parse(line, emit=False)
        # Pass 2: emit.
        self.pc = self.org
        self.items = []
        for line in lines:
            if line.endswith(':'):
                continue
            self._parse(line, emit=True)
        rom = bytearray([0xEA] * 4096)
        for addr, kind, data in self.items:
            off = addr - self.org
            if kind == 'b':
                rom[off] = self._val(data) & 0xFF
            elif kind == 'w':
                v = self._val(data) & 0xFFFF
                rom[off] = v & 0xFF
                rom[off + 1] = (v >> 8) & 0xFF
            else:
                opcode, operand, mode, size = data
                rom[off] = opcode
                if size == 2:
                    if mode == 'rel':
                        target = self._val(operand)
                        rom[off + 1] = (target - (addr + 2)) & 0xFF
                    else:
                        rom[off + 1] = self._val(operand) & 0xFF
                elif size == 3:
                    v = self._val(operand) & 0xFFFF
                    rom[off + 1] = v & 0xFF
                    rom[off + 2] = (v >> 8) & 0xFF
        return rom

    def _val(self, v):
        if isinstance(v, int):
            return v
        if isinstance(v, str):
            v = v.strip()
            if v.startswith('$'):
                return int(v[1:], 16)
            if v.startswith('%'):
                return int(v[1:], 2)
            if v.lstrip('-').isdigit():
                return int(v)
            if v in self.labels:
                return self.labels[v]
            if v in self.consts:
                return self.consts[v]
            raise ValueError(f'unknown symbol {v!r}')
        raise ValueError(f'bad value {v!r}')

    # opcode tables: mnemonic -> (mode -> opcode)
    OPS = {
        'LDA': {'imm': 0xA9, 'zp': 0xA5, 'zpx': 0xB5, 'abs': 0xAD, 'abx': 0xBD, 'aby': 0xB9, 'izx': 0xA1, 'izy': 0xB1},
        'LDX': {'imm': 0xA2, 'zp': 0xA6, 'zpy': 0xB6, 'abs': 0xAE, 'aby': 0xBE},
        'LDY': {'imm': 0xA0, 'zp': 0xA4, 'zpx': 0xB4, 'abs': 0xAC, 'abx': 0xBC},
        'STA': {'zp': 0x85, 'zpx': 0x95, 'abs': 0x8D, 'abx': 0x9D, 'aby': 0x99, 'izx': 0x81, 'izy': 0x91},
        'STX': {'zp': 0x86, 'zpy': 0x96, 'abs': 0x8E},
        'STY': {'zp': 0x84, 'zpx': 0x94, 'abs': 0x8C},
        'ADC': {'imm': 0x69, 'zp': 0x65, 'abs': 0x6D},
        'SBC': {'imm': 0xE9, 'zp': 0xE5, 'abs': 0xED},
        'AND': {'imm': 0x29, 'zp': 0x25, 'abs': 0x2D},
        'ORA': {'imm': 0x09, 'zp': 0x05, 'abs': 0x0D},
        'EOR': {'imm': 0x49, 'zp': 0x45, 'abs': 0x4D},
        'CMP': {'imm': 0xC9, 'zp': 0xC5, 'abs': 0xCD},
        'CPX': {'imm': 0xE0, 'zp': 0xE4, 'abs': 0xEC},
        'CPY': {'imm': 0xC0, 'zp': 0xC4, 'abs': 0xCC},
        'ASL': {'acc': 0x0A, 'zp': 0x06, 'abs': 0x0E},
        'LSR': {'acc': 0x4A, 'zp': 0x46, 'abs': 0x4E},
        'ROL': {'acc': 0x2A, 'zp': 0x26, 'abs': 0x2E},
        'ROR': {'acc': 0x6A, 'zp': 0x66, 'abs': 0x6E},
        'INC': {'zp': 0xE6, 'abs': 0xEE},
        'DEC': {'zp': 0xC6, 'abs': 0xCE},
        'BIT': {'zp': 0x24, 'abs': 0x2C},
        'JMP': {'abs': 0x4C, 'ind': 0x6C},
        'JSR': {'abs': 0x20},
        'BNE': {'rel': 0xD0}, 'BEQ': {'rel': 0xF0}, 'BCC': {'rel': 0x90},
        'BCS': {'rel': 0xB0}, 'BPL': {'rel': 0x10}, 'BMI': {'rel': 0x30},
        'BVC': {'rel': 0x50}, 'BVS': {'rel': 0x70},
    }
    IMPL = {'TAX': 0xAA, 'TAY': 0xA8, 'TXA': 0x8A, 'TYA': 0x98, 'TSX': 0xBA,
            'TXS': 0x9A, 'INX': 0xE8, 'INY': 0xC8, 'DEX': 0xCA, 'DEY': 0x88,
            'CLC': 0x18, 'SEC': 0x38, 'CLD': 0xD8, 'SED': 0xF8, 'CLI': 0x58,
            'SEI': 0x78, 'NOP': 0xEA, 'RTS': 0x60, 'RTI': 0x40, 'BRK': 0x00,
            'PHA': 0x48, 'PLA': 0x68, 'PHP': 0x08, 'PLP': 0x28}

    def _parse(self, line, emit):
        # Directives: .byte / .word
        if line.startswith('.byte'):
            vals = [v.strip() for v in line[5:].split(',')]
            if emit:
                for v in vals:
                    self.byte(v)
            else:
                self.pc += len(vals)
            return
        if line.startswith('.word'):
            vals = [v.strip() for v in line[5:].split(',')]
            if emit:
                for v in vals:
                    self.word(v)
            else:
                self.pc += 2 * len(vals)
            return
        parts = line.split(None, 1)
        mn = parts[0].upper()
        if mn in self.IMPL:
            if emit:
                self.items.append((self.pc, 'op', (self.IMPL[mn], None, 'imp', 1)))
            self.pc += 1
            return
        if mn in ('ASL', 'LSR', 'ROL', 'ROR') and len(parts) == 1:
            # Bare shift = accumulator mode.
            if emit:
                self.emit(self.OPS[mn]['acc'], None, 'acc')
            else:
                self.pc += 1
            return
        if len(parts) < 2:
            raise ValueError(f'bad line: {line!r}')
        arg = parts[1].strip()
        if mn in ('BNE', 'BEQ', 'BCC', 'BCS', 'BPL', 'BMI', 'BVC', 'BVS'):
            if emit:
                self.emit(self.OPS[mn]['rel'], arg, 'rel')
            else:
                self.pc += 2
            return
        # Addressing mode (allow a space after the comma: "$80, X").
        arg = arg.replace(', ', ',').replace(' ,', ',')
        if arg.startswith('#'):
            mode, operand = 'imm', arg[1:]
        elif arg.startswith('(') and arg.endswith(',X)'):
            mode, operand = 'izx', arg[1:-3]
        elif arg.startswith('(') and arg.endswith('),Y'):
            mode, operand = 'izy', arg[1:-3]
        elif arg.endswith(',X'):
            operand = arg[:-2]
            mode = 'zpx' if self._is_zp(operand) else 'abx'
        elif arg.endswith(',Y'):
            operand = arg[:-2]
            mode = 'zpy' if self._is_zp(operand) else 'aby'
        elif arg == 'A':
            mode, operand = 'acc', None
        else:
            operand = arg
            mode = 'zp' if self._is_zp(operand) else 'abs'
        opcode = self.OPS[mn][mode]
        if emit:
            self.emit(opcode, operand, mode)
        else:
            self.pc += 1 if mode in ('imp', 'acc') else (2 if mode in ('imm', 'zp', 'zpx', 'zpy', 'izx', 'izy') else 3)

    def _is_zp(self, operand):
        if isinstance(operand, str) and operand.startswith('$'):
            try:
                return int(operand[1:], 16) < 0x100
            except ValueError:
                return False
        return False


# RAM map ($80-$8F)
PX, PY, EX, EY, MX, MY, MACT, SCORE = 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
FRAME, SNDT, ESPEED, EDIR, HITFL, BGMODE = 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D
SELPREV, SNDPITCH = 0x8E, 0x8F

SRC = r"""
; --- reset / init ---
Reset:
    SEI
    CLD
    LDX #$FF
    TXS
    LDA #$00
    STA $00
    STA $01
    LDX #$00
ClearRam:
    STA $80, X
    INX
    CPX #$80
    BNE ClearRam
    LDA #$4C
    STA PX
    LDA #$78
    STA PY
    LDA #$28
    STA EX
    LDA #$30
    STA EY
    LDA #$00
    STA MX
    STA MY
    STA MACT
    STA SCORE
    STA FRAME
    STA SNDT
    LDA #$01
    STA ESPEED
    LDA #$00
    STA EDIR
    STA HITFL
    STA BGMODE
    STA SELPREV
    LDA #$02
    STA $01

; --- frame ---
Frame:
    LDA #$02
    STA $00
    STA $02
    STA $02
    STA $02
    LDA #$00
    STA $00
    LDA #$2C
    STA $296
    LDA #$02
    STA $01

    ; ---- game logic (runs inside VBLANK, timer counts the lines) ----
    INC FRAME
    LDA HITFL
    BEQ NoFlash
    DEC HITFL
NoFlash:
    ; enemy sweeps left-right on its own
    LDA EDIR
    BEQ EnemyRight
    LDA EX
    SEC
    SBC ESPEED
    STA EX
    CMP #$08
    BCS EnemyDone
    LDA #$00
    STA EDIR
    JMP EnemyDone
EnemyRight:
    LDA EX
    CLC
    ADC ESPEED
    STA EX
    CMP #$8C
    BCC EnemyDone
    LDA #$01
    STA EDIR
EnemyDone:
    ; joystick: SWCHA $280, active low ($10 up $20 down $40 left $80 right)
    LDA $280
    STA $90
    AND #$10
    BEQ JoyUp
    LDA $90
    AND #$20
    BEQ JoyDown
    JMP JoyVertDone
JoyUp:
    LDA PY
    SEC
    SBC #$02
    CMP #$10
    BCC JoyVertDone
    STA PY
    JMP JoyVertDone
JoyDown:
    LDA PY
    CLC
    ADC #$02
    CMP #$A0
    BCS JoyVertDone
    STA PY
JoyVertDone:
    LDA $90
    AND #$40
    BEQ JoyLeft
    LDA $90
    AND #$80
    BEQ JoyRight
    JMP JoyHorzDone
JoyLeft:
    LDA PX
    SEC
    SBC #$02
    CMP #$04
    BCC JoyHorzDone
    STA PX
    JMP JoyHorzDone
JoyRight:
    LDA PX
    CLC
    ADC #$02
    CMP #$94
    BCS JoyHorzDone
    STA PX
JoyHorzDone:
    ; fire: INPT4 $0C bit7, 0 = pressed
    LDA $0C
    AND #$80
    BNE NoFire
    LDA MACT
    BNE NoFire
    LDA #$01
    STA MACT
    LDA PX
    CLC
    ADC #$03
    STA MX
    LDA PY
    STA MY
    LDA #$08
    STA SNDT
    LDA #$14
    STA SNDPITCH
NoFire:
    ; missile flies up, 4 px/frame
    LDA MACT
    BEQ MissileDone
    LDA MY
    SEC
    SBC #$04
    STA MY
    CMP #$08
    BCS MissileHit
    LDA #$00
    STA MACT
    JMP MissileDone
MissileHit:
    ; hit if missile point is inside the enemy 8x8 box
    LDA MY
    SEC
    SBC EY
    CMP #$08
    BCS MissileDone
    LDA MX
    SEC
    SBC EX
    CMP #$08
    BCS MissileDone
    INC SCORE
    LDA #$00
    STA MACT
    LDA #$1E
    STA HITFL
    LDA #$0A
    STA SNDT
    LDA #$0C
    STA SNDPITCH
    LDA FRAME
    ASL
    ASL
    ASL
    AND #$7F
    CLC
    ADC #$08
    STA EX
MissileDone:
    ; sound timer: tone while SNDT > 0, else silent
    LDA SNDT
    BEQ SoundOff
    DEC SNDT
    LDA #$0C
    STA $15
    LDA SNDPITCH
    STA $17
    LDA #$08
    STA $19
    JMP SoundDone
SoundOff:
    LDA #$00
    STA $19
    STA $15
SoundDone:
    ; Select: SWCHB $282 bit1, active low; edge-triggered toggle
    LDA $282
    AND #$02
    STA $91
    LDA SELPREV
    BEQ SelWasUp
    LDA $91
    BNE SelDone
    JMP SelDone
SelWasUp:
    LDA $91
    BNE SelDone
    LDA BGMODE
    EOR #$01
    STA BGMODE
    LDA ESPEED
    EOR #$03
    STA ESPEED
SelDone:
    LDA $91
    EOR #$02
    LSR
    STA SELPREV

    ; ---- position the players (still in VBLANK: RESP strobes cost
    ; ---- 3 scanlines, the timer covers them) ----
    LDA PX
    JSR PosP0
    LDA EX
    JSR PosP1
    LDA MACT
    BEQ MissileOffPos
    LDA MX
    JSR PosM0
    JMP PosDone
MissileOffPos:
    LDA #$A0
    JSR PosM0
PosDone:
    ; wait out VBLANK on the RIOT timer, then visible
WaitVBlank:
    LDA $284
    BNE WaitVBlank
    LDA #$00
    STA $01
    LDA #$84
    STA $09
    LDA BGMODE
    BEQ BgBlue
    LDA #$C4
    STA $09
BgBlue:
    LDA HITFL
    BEQ BgSet
    LDA #$0E
    STA $09
BgSet:
    LDA #$1E
    STA $06
    LDA #$46
    STA $07
    LDA #$00
    STA $0A
    STA $0B
    STA $0C
    STA $0D
    LDA #$10
    STA $04
    LDA #$00
    STA $05
    STA $1E
    STA $1F
    ; NOTE: do NOT clear $20-$24 (HMP0/HMP1/HMM0/...) here — the Pos
    ; routines just set them, and HMOVE below applies them. Clearing
    ; them was the v0.2.0 bug that quantised every sprite to 15 px
    ; steps (fine position never applied).
    ; Display Y = game Y - 6 (P0) / - 5 (P1, M0): the HMOVE line, the
    ; kernel's first WSYNC and the GRP write lag put the sprite
    ; ~5-6 scanlines below the line the kernel compares against
    ; (measured with TIM64T=44: py=120 -> rows 131..144 with the
    ; old +5 pre-add, i.e. rows = dispY + 6 for P0, dispY + 5 for
    ; P1). Pre-subtract it here, in VBLANK, so the kernel costs no
    ; extra cycles. (TIM64T=88 was tried: it moved the whole frame
    ; down ~21 rows without changing the 210-line height, so the
    ; height is a StellaDS detection quirk, not the ROM's timing;
    ; the standard 44 stays.)
    LDA PY
    SEC
    SBC #$06
    STA $92
    LDA EY
    SEC
    SBC #$05
    STA $93
    LDA MY
    SEC
    SBC #$05
    STA $94
    STA $02
    STA $2A
    LDY #$00
    ; ---- visible kernel: 191 lines (1 HMOVE line + 191 = 192) ----
    ; 2-line kernel: even lines draw Player 0, odd lines draw Player 1
    ; + Missile 0. A 1-line kernel needs P0+P1+missile on one line
    ; (~79 cycles worst case) and overruns the 76-cycle scanline,
    ; adding a scanline per sprite row (measured frame height 210).
    ; Unrolled, the worst line is ~64 cycles. Sprites are stored
    ; 16 rows tall (each row doubled), so the row compare is against
    ; 16 and the sprite is 16 scanlines tall. Display Y comes from
    ; $92/$93/$94 (game Y + 5, set in VBLANK).
    ; NOTE: the "default 0" must be loaded on the not-in-range path,
    ; NOT before TYA — TYA clobbers A, so a pre-loaded 0 becomes
    ; (Y - dispY) on out-of-range lines and the sprite smears over
    ; the whole frame (that was the v0.2.0 smear bug).
Kernel:
    STA $02
    TYA
    SEC
    SBC $92
    CMP #$10
    BCS NoP0
    TAX
    LDA SpriteP0, X
    JMP GotP0
NoP0:
    LDA #$00
GotP0:
    STA $1B
    INY
    CPY #$BF
    BEQ KernelDone
    STA $02
    TYA
    SEC
    SBC $93
    CMP #$10
    BCS NoP1
    TAX
    LDA SpriteP1, X
    JMP GotP1
NoP1:
    LDA #$00
GotP1:
    STA $1C
    LDX MACT
    BEQ NoMissile
    TYA
    SEC
    SBC $94
    CMP #$04
    BCS NoMissile
    LDA #$02
    JMP GotMissile
NoMissile:
    LDA #$00
GotMissile:
    STA $1D
    INY
    CPY #$BF
    BNE Kernel
KernelDone:
    ; ---- overscan: 30 lines ----
    LDA #$02
    STA $01
    LDY #$1E
Over:
    STA $02
    DEY
    BNE Over
    JMP Frame

; Position a player/missile at X in A (0-159). Standard divide-by-15
; routine: RESP strobe sets the coarse position, HMPx the fine one.
; Each call costs one scanline (STA WSYNC).
; The RESP strobe in this core lands 9 colour clocks early (measured:
; px=40/80/120/150 -> sprite at 31/71/111/141, a constant -9), so add
; 9 to X on entry — the routine then positions exactly at X.
PosP0:
    CLC
    ADC #$09
    SEC
    STA $02
PosP0Div:
    SBC #$0F
    BCS PosP0Div
    EOR #$07
    ASL
    ASL
    ASL
    ASL
    STA $20
    STA $10
    RTS
PosP1:
    CLC
    ADC #$09
    SEC
    STA $02
PosP1Div:
    SBC #$0F
    BCS PosP1Div
    EOR #$07
    ASL
    ASL
    ASL
    ASL
    STA $21
    STA $11
    RTS
PosM0:
    CLC
    ADC #$09
    SEC
    STA $02
PosM0Div:
    SBC #$0F
    BCS PosM0Div
    EOR #$07
    ASL
    ASL
    ASL
    ASL
    STA $22
    STA $12
    RTS

SpriteP0:
    .byte $18, $18, $3C, $3C, $7E, $7E, $FF, $FF, $7E, $7E, $3C, $3C, $18, $18, $00, $00
SpriteP1:
    .byte $3C, $3C, $7E, $7E, $FF, $FF, $A5, $A5, $FF, $FF, $7E, $7E, $3C, $3C, $00, $00
"""

# Vectors: NMI/RESET/IRQ all point at Reset ($F000).
VECTORS = {0xFFA: 0x00, 0xFFB: 0xF0,
           0xFFC: 0x00, 0xFFD: 0xF0,
           0xFFE: 0x00, 0xFFF: 0xF0}


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "playtest.a26"
    asm = Asm(0xF000, {'PX': PX, 'PY': PY, 'EX': EX, 'EY': EY, 'MX': MX,
                       'MY': MY, 'MACT': MACT, 'SCORE': SCORE, 'FRAME': FRAME,
                       'SNDT': SNDT, 'ESPEED': ESPEED, 'EDIR': EDIR,
                       'HITFL': HITFL, 'BGMODE': BGMODE, 'SELPREV': SELPREV,
                       'SNDPITCH': SNDPITCH})
    rom = asm.assemble(SRC)
    for offset, value in VECTORS.items():
        rom[offset] = value
    with open(out, "wb") as f:
        f.write(rom)
    print(f"Wrote {out} ({len(rom)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
