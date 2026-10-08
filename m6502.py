# The 6502 as TINY-NES runs it: the reference the circuits are checked against.
# Official instructions only (what real games use); decimal mode is ignored, as on the NES's 2A03.
# OPS maps an opcode to (name, mode); the same table later writes the CPU's control memory.

MODES = {  # mode -> operand bytes
    'imp': 0, 'acc': 0, 'imm': 1, 'zp': 1, 'zpx': 1, 'zpy': 1, 'rel': 1,
    'abs': 2, 'abx': 2, 'aby': 2, 'ind': 2, 'izx': 1, 'izy': 1,
}

_T = """
ADC 69 imm 65 zp 75 zpx 6D abs 7D abx 79 aby 61 izx 71 izy
AND 29 imm 25 zp 35 zpx 2D abs 3D abx 39 aby 21 izx 31 izy
ASL 0A acc 06 zp 16 zpx 0E abs 1E abx
BCC 90 rel
BCS B0 rel
BEQ F0 rel
BIT 24 zp 2C abs
BMI 30 rel
BNE D0 rel
BPL 10 rel
BRK 00 imp
BVC 50 rel
BVS 70 rel
CLC 18 imp
CLD D8 imp
CLI 58 imp
CLV B8 imp
CMP C9 imm C5 zp D5 zpx CD abs DD abx D9 aby C1 izx D1 izy
CPX E0 imm E4 zp EC abs
CPY C0 imm C4 zp CC abs
DEC C6 zp D6 zpx CE abs DE abx
DEX CA imp
DEY 88 imp
EOR 49 imm 45 zp 55 zpx 4D abs 5D abx 59 aby 41 izx 51 izy
INC E6 zp F6 zpx EE abs FE abx
INX E8 imp
INY C8 imp
JMP 4C abs 6C ind
JSR 20 abs
LDA A9 imm A5 zp B5 zpx AD abs BD abx B9 aby A1 izx B1 izy
LDX A2 imm A6 zp B6 zpy AE abs BE aby
LDY A0 imm A4 zp B4 zpx AC abs BC abx
LSR 4A acc 46 zp 56 zpx 4E abs 5E abx
NOP EA imp
ORA 09 imm 05 zp 15 zpx 0D abs 1D abx 19 aby 01 izx 11 izy
PHA 48 imp
PHP 08 imp
PLA 68 imp
PLP 28 imp
ROL 2A acc 26 zp 36 zpx 2E abs 3E abx
ROR 6A acc 66 zp 76 zpx 6E abs 7E abx
RTI 40 imp
RTS 60 imp
SBC E9 imm E5 zp F5 zpx ED abs FD abx F9 aby E1 izx F1 izy
SEC 38 imp
SED F8 imp
SEI 78 imp
STA 85 zp 95 zpx 8D abs 9D abx 99 aby 81 izx 91 izy
STX 86 zp 96 zpy 8E abs
STY 84 zp 94 zpx 8C abs
TAX AA imp
TAY A8 imp
TSX BA imp
TXA 8A imp
TXS 9A imp
TYA 98 imp
"""
OPS = {}
for _line in _T.strip().splitlines():
    _w = _line.split()
    for _i in range(1, len(_w), 2):
        OPS[int(_w[_i], 16)] = (_w[0], _w[_i + 1])

C, Z, I, D, B, U, V, N = (1 << i for i in range(8))


class CPU:
    def __init__(self, bus):
        self.bus = bus  # bus.read(addr) / bus.write(addr, v)
        self.a = self.x = self.y = 0
        self.sp = 0xFD
        self.p = 0x24
        self.pc = 0
        self.nmi_pending = False

    def reset(self):
        self.pc = self.rd(0xFFFC) | self.rd(0xFFFD) << 8
        self.sp, self.p = 0xFD, 0x24

    def rd(self, a):
        return self.bus.read(a & 0xFFFF)

    def wr(self, a, v):
        self.bus.write(a & 0xFFFF, v & 0xFF)

    def push(self, v):
        self.wr(0x100 | self.sp, v)
        self.sp = (self.sp - 1) & 0xFF

    def pull(self):
        self.sp = (self.sp + 1) & 0xFF
        return self.rd(0x100 | self.sp)

    def nz(self, v):
        v &= 0xFF
        self.p = (self.p & ~(N | Z)) | (v & N) | (0 if v else Z)
        return v

    def flag(self, f, on):
        self.p = self.p | f if on else self.p & ~f

    def addr(self, mode):
        """the operand's address (None: no memory operand); pc is past the instruction afterwards"""
        pc = self.pc
        if mode in ('imp', 'acc'):
            return None
        if mode == 'imm':
            self.pc += 1
            return pc
        b = self.rd(pc)
        if MODES[mode] == 2:
            w = b | self.rd(pc + 1) << 8
            self.pc += 2
            if mode == 'abs': return w
            if mode == 'abx': return (w + self.x) & 0xFFFF
            if mode == 'aby': return (w + self.y) & 0xFFFF
            # JMP (ind): the high byte comes from the same page (the 6502's famous wrap)
            return self.rd(w) | self.rd((w & 0xFF00) | ((w + 1) & 0xFF)) << 8
        self.pc += 1
        if mode == 'zp': return b
        if mode == 'zpx': return (b + self.x) & 0xFF
        if mode == 'zpy': return (b + self.y) & 0xFF
        if mode == 'rel': return (self.pc + (b - 256 if b & 0x80 else b)) & 0xFFFF
        if mode == 'izx':
            z = (b + self.x) & 0xFF
            return self.rd(z) | self.rd((z + 1) & 0xFF) << 8
        if mode == 'izy':
            return ((self.rd(b) | self.rd((b + 1) & 0xFF) << 8) + self.y) & 0xFFFF

    def interrupt(self, vector, brk=False):
        self.push(self.pc >> 8)
        self.push(self.pc & 0xFF)
        self.push(self.p | U | (B if brk else 0))
        self.p |= I
        self.pc = self.rd(vector) | self.rd(vector + 1) << 8

    def step(self):
        if self.nmi_pending:
            self.nmi_pending = False
            self.interrupt(0xFFFA)
            return
        op = self.rd(self.pc)
        self.pc = (self.pc + 1) & 0xFFFF
        name, mode = OPS[op]
        ea = self.addr(mode)
        self.pc &= 0xFFFF
        m = (lambda: self.rd(ea))
        if name in ('ADC', 'SBC'):
            v = m() ^ (0xFF if name == 'SBC' else 0)
            s = self.a + v + (self.p & C)
            self.flag(V, (~(self.a ^ v) & (self.a ^ s)) & 0x80)
            self.flag(C, s > 0xFF)
            self.a = self.nz(s)
        elif name == 'AND': self.a = self.nz(self.a & m())
        elif name == 'ORA': self.a = self.nz(self.a | m())
        elif name == 'EOR': self.a = self.nz(self.a ^ m())
        elif name in ('ASL', 'LSR', 'ROL', 'ROR'):
            v = self.a if mode == 'acc' else m()
            c = self.p & C
            if name == 'ASL': r, nc = v << 1, v & 0x80
            elif name == 'LSR': r, nc = v >> 1, v & 1
            elif name == 'ROL': r, nc = v << 1 | c, v & 0x80
            else: r, nc = v >> 1 | c << 7, v & 1
            self.flag(C, nc)
            r = self.nz(r)
            if mode == 'acc': self.a = r
            else: self.wr(ea, r)
        elif name[0] == 'B' and mode == 'rel':
            f = {'PL': (N, 0), 'MI': (N, 1), 'VC': (V, 0), 'VS': (V, 1), 'CC': (C, 0), 'CS': (C, 1), 'NE': (Z, 0), 'EQ': (Z, 1)}[name[1:]]
            if bool(self.p & f[0]) == bool(f[1]):
                self.pc = ea
        elif name == 'BIT':
            v = m()
            self.p = (self.p & ~(N | V | Z)) | (v & (N | V)) | (0 if v & self.a else Z)
        elif name == 'BRK':
            self.pc = (self.pc + 1) & 0xFFFF
            self.interrupt(0xFFFE, brk=True)
        elif name in ('CLC', 'SEC'): self.flag(C, name == 'SEC')
        elif name in ('CLI', 'SEI'): self.flag(I, name == 'SEI')
        elif name in ('CLD', 'SED'): self.flag(D, name == 'SED')
        elif name == 'CLV': self.flag(V, 0)
        elif name in ('CMP', 'CPX', 'CPY'):
            r = {'CMP': self.a, 'CPX': self.x, 'CPY': self.y}[name]
            v = m()
            self.flag(C, r >= v)
            self.nz(r - v)
        elif name in ('DEC', 'INC'):
            self.wr(ea, self.nz(m() + (1 if name == 'INC' else -1)))
        elif name == 'DEX': self.x = self.nz(self.x - 1)
        elif name == 'DEY': self.y = self.nz(self.y - 1)
        elif name == 'INX': self.x = self.nz(self.x + 1)
        elif name == 'INY': self.y = self.nz(self.y + 1)
        elif name == 'JMP': self.pc = ea
        elif name == 'JSR':
            ret = (self.pc - 1) & 0xFFFF
            self.push(ret >> 8)
            self.push(ret & 0xFF)
            self.pc = ea
        elif name == 'LDA': self.a = self.nz(m())
        elif name == 'LDX': self.x = self.nz(m())
        elif name == 'LDY': self.y = self.nz(m())
        elif name == 'NOP': pass
        elif name == 'PHA': self.push(self.a)
        elif name == 'PHP': self.push(self.p | B | U)
        elif name == 'PLA': self.a = self.nz(self.pull())
        elif name == 'PLP': self.p = (self.pull() & ~B) | U
        elif name == 'RTI':
            self.p = (self.pull() & ~B) | U
            self.pc = self.pull() | self.pull() << 8
        elif name == 'RTS':
            self.pc = ((self.pull() | self.pull() << 8) + 1) & 0xFFFF
        elif name == 'STA': self.wr(ea, self.a)
        elif name == 'STX': self.wr(ea, self.x)
        elif name == 'STY': self.wr(ea, self.y)
        elif name == 'TAX': self.x = self.nz(self.a)
        elif name == 'TAY': self.y = self.nz(self.a)
        elif name == 'TSX': self.x = self.nz(self.sp)
        elif name == 'TXA': self.a = self.nz(self.x)
        elif name == 'TXS': self.sp = self.x
        elif name == 'TYA': self.a = self.nz(self.y)
        else:
            raise ValueError(f'{name} not handled')
