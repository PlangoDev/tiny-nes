# TINY-NES TEST CART: our own NES cartridge that checks the machine.
#   python3 testcart.py            -> testcart.nes (and the model's verdict on it)
# What it checks (each line on screen says PASS or FAIL):
#   13 CPU tests (loads + flags, ADC, SBC, logic, shifts, compares, inc/dec, branches, stack + JSR/RTS,
#   indexed modes with their wraps, indirect modes + JMP ($xxFF), BIT + transfers, BRK/RTI),
#   the picture chip: text from the nametable, 4 palettes through the attribute table, a sprite moved by the pad,
#   sprite 0 hit (it says PASS once it has fired), and a mid-frame scroll split (the checker field below the split
#   slides sideways while the text above stays put), the pad (each button shows its letter while held), a frame counter.
import os
import sys

from asm6502 import assemble, ines

HERE = os.path.dirname(os.path.abspath(__file__))
# the letters (ASCII 32-95), 8 rows each as hex (bit 7 = left): the 5x7 font of Tiny Logic, centred in 8x8
GLYPHS = (
    '0000000000000000 1010101010001000 2828000000000000 28287C287C282800 103C503814781000 60640810204C0C00 3048502054483400 1010000000000000 '
    '0810202020100800 2010080808102000 0010543854100000 0010107C10100000 0000000000081000 0000003C00000000 0000000000001000 0404081020404000 '
    '38444C5464443800 1030101010103800 3844040810207C00 7C08100804443800 081828487C080800 7C40780404443800 1820407844443800 7C04081020202000 '
    '3844443844443800 3844443C04083000 0000100000100000 0000080000081000 0408102010080400 00003C003C000000 2010080408102000 3844040810001000 '
    '38445C545C403800 3844447C44444400 7844447844447800 3844404040443800 7844444444447800 7C40407840407C00 7C40407840404000 3844405C44443C00 '
    '4444447C44444400 3810101010103800 1C08080808483000 4448506050484400 4040404040407C00 446C545444444400 444464544C444400 3844444444443800 '
    '7844447840404000 3844444454483400 7844447850484400 3C40403804047800 7C10101010101000 4444444444443800 4444444444281000 4444445454542800 '
    '4444281028444400 4444281010101000 7C04081020407C00 3820202020203800 4040201008040400 3808080808083800 1028440000000000 0000000000007C00 '
).split()


def chr_rom():
    """8 KB of tiles: ASCII 32-95 are letters (tile number = character), 1 solid, 2 checker, 3 a smiley"""
    tiles = [[[0] * 8 for _ in range(8)] for _ in range(512)]
    for code in range(32, 96):
        g = bytes.fromhex(GLYPHS[code - 32])
        tiles[code] = [[3 if g[y] >> (7 - x) & 1 else 0 for x in range(8)] for y in range(8)]
    tiles[1] = [[3] * 8 for _ in range(8)]
    tiles[2] = [[1 if (x // 4 + y // 4) % 2 else 2 for x in range(8)] for y in range(8)]
    face = ['..3333..', '.333333.', '33.33.33', '33333333', '3.3333.3', '33....33', '.333333.', '..3333..']
    tiles[3] = [[3 if c == '3' else 0 for c in row] for row in face]
    out = bytearray()
    for t in tiles:
        lo = bytes(sum(((t[y][x] & 1) << (7 - x)) for x in range(8)) for y in range(8))
        hi = bytes(sum(((t[y][x] >> 1) << (7 - x)) for x in range(8)) for y in range(8))
        out += lo + hi
    return bytes(out)


TESTS = []  # (name, body): a body falls through to pass, or jumps to the label FAIL


def test(name, body):
    TESTS.append((name, body))


test('LOAD FLAGS', """
    lda #$80
    bpl FAIL
    beq FAIL
    lda #$00
    bne FAIL
    bmi FAIL
    ldx #$7F
    bmi FAIL
    ldy #$FF
    bpl FAIL
""")
test('ADC', """
    clc
    lda #$50
    adc #$50
    bvc FAIL
    bcs FAIL
    cmp #$A0
    bne FAIL
    clc
    lda #$FF
    adc #$01
    bne FAIL
    bcc FAIL
    sec
    lda #$01
    adc #$01
    cmp #$03
    bne FAIL
""")
test('SBC', """
    sec
    lda #$50
    sbc #$F0
    bcs FAIL
    bvs FAIL
    cmp #$60
    bne FAIL
    sec
    lda #$80
    sbc #$01
    bvc FAIL
    bcc FAIL
    cmp #$7F
    bne FAIL
    clc
    lda #$05
    sbc #$02
    cmp #$02
    bne FAIL
""")
test('LOGIC', """
    lda #$F0
    and #$3C
    cmp #$30
    bne FAIL
    lda #$0F
    ora #$30
    cmp #$3F
    bne FAIL
    lda #$FF
    eor #$0F
    cmp #$F0
    bne FAIL
""")
test('SHIFTS', """
    lda #$81
    asl a
    bcc FAIL
    cmp #$02
    bne FAIL
    lsr a
    bcs FAIL
    cmp #$01
    bne FAIL
    sec
    lda #$40
    rol a
    bcs FAIL
    cmp #$81
    bne FAIL
    clc
    ror a
    bcc FAIL
    cmp #$40
    bne FAIL
    lda #$01
    sta tmp
    asl tmp
    asl tmp
    lda tmp
    cmp #$04
    bne FAIL
""")
test('COMPARES', """
    lda #$40
    cmp #$40
    bne FAIL
    bcc FAIL
    cmp #$41
    bcs FAIL
    beq FAIL
    ldx #$10
    cpx #$08
    bcc FAIL
    beq FAIL
    ldy #$00
    cpy #$01
    bcs FAIL
    bpl FAIL
""")
test('INC DEC', """
    lda #$FF
    sta tmp
    inc tmp
    bne FAIL
    dec tmp
    bpl FAIL
    lda tmp
    cmp #$FF
    bne FAIL
    ldx #$FF
    inx
    bne FAIL
    ldy #$00
    dey
    cpy #$FF
    bne FAIL
""")
test('BRANCHES', """
    clc
    bcs FAIL
    sec
    bcc FAIL
    clv
    bvs FAIL
    lda #$01
    beq FAIL
    bmi FAIL
    lda #$80
    bpl FAIL
    bne @far
    jmp FAIL
@back:
    jmp @done
@far:
    bmi @back
    jmp FAIL
@done:
""")
test('STACK JSR', """
    tsx
    stx tmp
    lda #$5A
    pha
    lda #$00
    pla
    cmp #$5A
    bne FAIL
    sec
    php
    clc
    plp
    bcc FAIL
    jsr @sub
    cpy #$77
    bne FAIL
    tsx
    cpx tmp
    bne FAIL
    jmp @done
@sub:
    ldy #$77
    rts
@done:
""")
test('INDEXED', """
    lda #$11
    sta $01
    ldx #$02
    lda $FF,x
    cmp #$11
    bne FAIL
    lda #$22
    sta $0310
    ldy #$20
    lda $02F0,y
    cmp #$22
    bne FAIL
    ldx #$10
    lda $0300,x
    cmp #$22
    bne FAIL
""")
test('INDIRECT', """
    lda #$10
    sta $40
    lda #$03
    sta $41
    lda #$33
    sta $0310
    ldx #$04
    lda ($3C,x)
    cmp #$33
    bne FAIL
    ldy #$10
    lda #$00
    sta $40
    lda ($40),y
    cmp #$33
    bne FAIL
    lda #<@ok
    sta $02FF
    lda #>@ok
    sta $0200
    lda #$FF
    sta $0300
    jmp ($02FF)
@ok:
""")
test('BIT TRANSFER', """
    lda #$C0
    sta tmp
    lda #$01
    bit tmp
    bne FAIL
    bpl FAIL
    bvc FAIL
    lda #$42
    tax
    txa
    tay
    tya
    cmp #$42
    bne FAIL
    tsx
    txs
""")
test('BRK RTI', """
    lda #$00
    sta brkhit
    brk
    .byte $EA
    lda brkhit
    cmp #$01
    bne FAIL
""")


def source():
    tests = []
    for i, (name, body) in enumerate(TESTS):
        b = body.replace('FAIL', f't{i}_fail').replace('@', f't{i}_')
        tests.append(f"""
test{i}:
{b}
    lda #$00
    rts
t{i}_fail:
    lda #$01
    rts
""")
    calls = '\n'.join(f'    jsr test{i}\n    sta results+{i}' for i in range(len(TESTS)))
    names = '\n'.join(f'name{i}: .byte "{n}", 0' for i, (n, _) in enumerate(TESTS))
    ntab = ', '.join(f'name{i}' for i in range(len(TESTS)))
    return f"""
tmp = $00
ptr = $02
nmiflag = $04
pad = $05
brkhit = $06
frame = $07
sprx = $09
spry = $0A
s0ok = $0B
split = $0C
row = $0D
step = $0E
mtimer = $0F
results = $20
oambuf = $0700
NTESTS = {len(TESTS)}
.org $8000

reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$00
    sta $2000
    sta $2001
@w1: bit $2002
    bpl @w1
@w2: bit $2002
    bpl @w2
    lda #$00
    tax
@clr:
    sta $0000,x
    sta $0300,x
    sta $0400,x
    sta $0500,x
    sta $0600,x
    inx
    bne @clr
    lda #$FF
    ldx #$00
@hide:
    sta oambuf,x
    inx
    bne @hide
{calls}
    ldx #$FF
    txs
    jsr draw
    lda #120
    sta sprx
    lda #200
    sta spry
    lda #$0F
    sta $4015
    lda #$80
    sta $2000
    lda #$1E
    sta $2001
main:
    lda nmiflag
    beq main
    lda #$00
    sta nmiflag
    jsr waits0
    jsr readpad
    jsr move
    jsr frameupdate
    jsr music
    jmp main

; ---- sound: a 16-step tune (pulse 1 melody, triangle bass, noise tick), a beep on pulse 2 while A is held
music:
    dec mtimer
    bpl @beep
    lda #7
    sta mtimer
    ldx step
    lda melody,x
    beq @rest
    sta $4002
    lda #$BF            ; duty 50%, length halted, constant volume 15
    sta $4000
    lda #$08            ; length index 1 (254), timer high 0
    sta $4003
@rest:
    lda bass,x
    sta $400A
    lda #$FF
    sta $4008
    lda #$09            ; timer high 1
    sta $400B
    txa
    and #$01
    bne @nonoise
    lda #$34            ; noise: constant volume 4
    sta $400C
    lda #$03
    sta $400E
    lda #$18
    sta $400F
@nonoise:
    inx
    txa
    and #$0F
    sta step
@beep:
    lda pad
    and #$01
    beq @quiet
    lda #$7F
    sta $4004
    lda #$70            ; a high note
    sta $4006
    lda #$08
    sta $4007
    rts
@quiet:
    lda #$30            ; constant volume 0
    sta $4004
    rts


; ---- vblank: sprites, the pad's letters, the counter, then the split under sprite 0
nmi:
    pha
    txa
    pha
    tya
    pha
    lda #$00
    sta $2003
    lda #>oambuf
    sta $4014
    jsr padrow
    jsr counter
    lda s0ok
    beq @nos0
    lda #$22
    sta $2006
    lda #$30
    sta $2006
    lda #'P'
    sta $2007
    lda #'A'
    sta $2007
    lda #'S'
    sta $2007
    sta $2007
@nos0:
    lda #$00
    sta $2005
    sta $2005
    lda #$80
    sta $2000
@s0done:
    lda #$01
    sta nmiflag
    pla
    tay
    pla
    tax
    pla
    rti

; the split: wait for last frame's sprite 0 flag to clear, then for it to fire, then scroll what's below.
; (gives up at the next vblank, so a frame without sprite 0 can't hang the game)
waits0:
    lda oambuf
    cmp #$F0
    bcs @out
@clear:
    bit $2002
    bvs @clear
@set:
    bit $2002
    bmi @out
    bvc @set
    lda #$01
    sta s0ok
    lda split
    sta $2005
    lda #$00
    sta $2005
@out:
    rts

irq:
    lda #$01
    sta brkhit
    rti

readpad:
    lda #$01
    sta $4016
    lda #$00
    sta $4016
    ldx #$08
@b: lda $4016
    lsr a
    ror pad
    dex
    bne @b
    rts

move:
    lda pad
    and #$80
    beq @l
    inc sprx
@l: lda pad
    and #$40
    beq @u
    dec sprx
@u: lda pad
    and #$10
    beq @d
    dec spry
@d: lda pad
    and #$20
    beq @s
    inc spry
@s: inc split
    rts

frameupdate:
    inc frame
    bne @nc
    inc frame+1
@nc:
    lda #175
    sta oambuf+0
    lda #$01
    sta oambuf+1
    lda #$00
    sta oambuf+2
    lda #16
    sta oambuf+3
    lda spry
    sta oambuf+4
    lda #$03
    sta oambuf+5
    lda #$01
    sta oambuf+6
    lda sprx
    sta oambuf+7
    rts

; the pad's 8 buttons on row 17: a letter while held, a dash when not
padrow:
    lda #$22
    sta $2006
    lda #$4A
    sta $2006
    ldx #$00
    lda pad
    sta tmp
@p: lsr tmp
    bcc @off
    lda padnames,x
    jmp @put
@off:
    lda #'-'
@put:
    sta $2007
    lda #' '
    sta $2007
    inx
    cpx #$08
    bne @p
    rts

counter:
    lda #$22
    sta $2006
    lda #$68
    sta $2006
    lda frame+1
    jsr hex
    lda frame
    jsr hex
    rts

hex:
    pha
    lsr a
    lsr a
    lsr a
    lsr a
    tax
    lda hexdigits,x
    sta $2007
    pla
    and #$0F
    tax
    lda hexdigits,x
    sta $2007
    rts

; ---- the screen, once, with the picture chip off
draw:
    lda #$3F
    sta $2006
    lda #$00
    sta $2006
    ldx #$00
@pal:
    lda palette,x
    sta $2007
    inx
    cpx #$20
    bne @pal
    lda #$20
    sta $2006
    lda #$00
    sta $2006
    ldx #$00
    ldy #$04
    lda #' '
@cls:
    sta $2007
    inx
    bne @cls
    dey
    bne @cls
    lda #$23
    sta $2006
    lda #$C0
    sta $2006
    ldx #$40
    lda #$00
@clat:
    sta $2007
    dex
    bne @clat
    lda #$20
    ldx #$47
    jsr at
    lda #<title
    ldx #>title
    jsr print
    lda #$00
    sta row
@line:
    ; nametable address $2082 + row * 32
    lda row
    lsr a
    lsr a
    lsr a
    clc
    adc #$20
    sta ptr+1
    lda row
    asl a
    asl a
    asl a
    asl a
    asl a
    clc
    adc #$62
    sta ptr
    bcc @nc2
    inc ptr+1
@nc2:
    lda ptr+1
    sta $2006
    lda ptr
    sta $2006
    ldx row
    lda results,x
    pha
    txa
    asl a
    tax
    lda nametab,x
    pha
    lda nametab+1,x
    tax
    pla
    jsr print
    lda #' '
    sta $2007
    pla
    bne @fail
    lda #<passtxt
    ldx #>passtxt
    jmp @say
@fail:
    lda #<failtxt
    ldx #>failtxt
@say:
    jsr print
    inc row
    lda row
    cmp #NTESTS
    bne @line
    lda #$22
    ldx #$22
    jsr at
    lda #<s0txt
    ldx #>s0txt
    jsr print
    lda #$22
    ldx #$42
    jsr at
    lda #<padtxt
    ldx #>padtxt
    jsr print
    lda #$22
    ldx #$62
    jsr at
    lda #<frametxt
    ldx #>frametxt
    jsr print
    ; the split line (row 15): a solid bar that sprite 0 sits on
    lda #$22
    ldx #$C0
    jsr at
    lda #$01
    ldx #$20
@bar:
    sta $2007
    dex
    bne @bar
    ; colour bars, rows 19-20, one palette per 16 pixels via the attribute table
    lda #$22
    ldx #$80
    jsr at
    ldx #$40
    lda #$01
@cb:
    sta $2007
    dex
    bne @cb
    lda #$23
    ldx #$E8
    jsr at
    ldx #$00
@attr:
    lda attrbars,x
    sta $2007
    inx
    cpx #$08
    bne @attr
    ; the checker field, rows 22-27 (it slides under the split)
    lda #$22
    ldx #$E0
    jsr at
    ldx #$C0
    lda #$02
@ck:
    sta $2007
    dex
    bne @ck
    rts

at:
    sta $2006
    stx $2006
    rts

print:
    sta ptr
    stx ptr+1
    ldy #$00
@c: lda (ptr),y
    beq @e
    sta $2007
    iny
    bne @c
@e: rts

palette:
    .byte $0F,$30,$21,$30, $0F,$16,$27,$16, $0F,$1A,$2A,$1A, $0F,$12,$22,$12
    .byte $0F,$30,$30,$30, $0F,$28,$18,$38, $0F,$30,$30,$30, $0F,$30,$30,$30
attrbars:
    .byte $00,$55,$AA,$FF,$00,$55,$AA,$FF
padnames: .byte "ABSTUDLR"
melody: .byte $FD,$C9,$A9,$7E,$A9,$C9,$FD,0, $E1,$B3,$96,$70,$96,$B3,$E1,0
bass: .byte $FB,$FB,$FB,$FB,$A9,$A9,$A9,$A9, $DF,$DF,$DF,$DF,$BC,$BC,$BC,$BC
hexdigits: .byte "0123456789ABCDEF"
title: .byte "TINY-NES TEST CART", 0
passtxt: .byte "PASS", 0
failtxt: .byte "FAIL", 0
s0txt: .byte "SPRITE 0 HIT  ....", 0
padtxt: .byte "PAD", 0
frametxt: .byte "FRAME", 0
nametab: .word {ntab}
{names}
{''.join(tests)}
.org $FFFA
.word nmi, reset, irq
"""


def build():
    prg, syms = assemble(source(), 0x8000)
    assert len(prg) == 0x8000, len(prg)
    path = os.path.join(HERE, 'testcart.nes')
    open(path, 'wb').write(ines(prg, chr_rom()))
    return path, syms


def model_verdict(path, frames=8):
    """runs the cart in the Python CPU model (no picture chip: vblank faked) and reads its results"""
    from m6502 import CPU
    rom = open(path, 'rb').read()
    prg = rom[16:16 + 32768]

    class Bus:
        def __init__(s):
            s.ram = bytearray(0x800); s.reads = 0
        def read(s, a):
            if a < 0x2000: return s.ram[a & 0x7FF]
            if a == 0x2002:
                s.reads += 1
                return 0xC0 if s.reads % 2 else 0x80   # vblank, and sprite 0 set
            if a >= 0x8000: return prg[a & 0x7FFF]
            return 0
        def write(s, a, v):
            if a < 0x2000: s.ram[a & 0x7FF] = v
    bus = Bus()
    cpu = CPU(bus)
    cpu.reset()
    for _ in range(200000):
        cpu.step()
        if cpu.pc == _main:
            break
    return list(bus.ram[0x20:0x20 + len(TESTS)])


if __name__ == '__main__':
    path, syms = build()
    _main = syms['main']
    res = model_verdict(path)
    for (name, _), r in zip(TESTS, res):
        print(f'  {name:14s} {"PASS" if r == 0 else "FAIL"}')
    print(path, 'reset', hex(syms['reset']), 'main', hex(syms['main']))
    sys.exit(0 if not any(res) else 1)
