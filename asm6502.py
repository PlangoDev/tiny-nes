# A small two-pass 6502 assembler for TINY-NES's own cartridges.
#   label:          NAME = expr        .org $C000      .byte 1,2,"TEXT"      .word label      .res n
#   lda #<label     sta $2006          lda table,x     lda (ptr),y     jmp (vec)      ; comments
# Expressions: numbers ($hex %bin dec 'c'), labels, + - * / & | >> <<, < (low byte), > (high byte), parentheses.
import re

from m6502 import OPS, MODES

BYOP = {}
for _op, (_name, _mode) in OPS.items():
    BYOP[(_name, _mode)] = _op


class AsmError(Exception):
    pass


def _value(expr, syms, final):
    expr = expr.strip()
    if not expr:
        raise AsmError('missing value')
    if expr[0] == '<':
        return _value(expr[1:], syms, final) & 0xFF
    if expr[0] == '>':
        return (_value(expr[1:], syms, final) >> 8) & 0xFF

    def num(m):
        t = m.group(0)
        if t.startswith('$'): return str(int(t[1:], 16))
        if t.startswith('%'): return str(int(t[1:], 2))
        if t.startswith("'"): return str(ord(t[1]))
        if t[0].isdigit(): return t
        if t in syms: return str(syms[t])
        if final: raise AsmError(f'unknown name {t}')
        return '65535'  # (not known yet: a later label, so never zero page)
    py = re.sub(r"\$[0-9A-Fa-f]+|%[01]+|'.'|[A-Za-z_.@][A-Za-z0-9_.@]*|\d+", num, expr)
    if not re.fullmatch(r'[0-9+\-*/&|<>() ]*', py):
        raise AsmError(f'bad expression {expr}')
    return int(eval(py.replace('/', '//')))


def _split_args(s):
    out, cur, q = [], '', False
    for ch in s:
        if ch == '"': q = not q
        if ch == ',' and not q:
            out.append(cur); cur = ''
        else:
            cur += ch
    out.append(cur)
    return [a.strip() for a in out]


def _mode(name, arg, syms, final):
    """(mode, value expression)"""
    a = arg.strip()
    if not a:
        return ('acc' if (name, 'acc') in BYOP else 'imp'), None
    if a.upper() == 'A' and (name, 'acc') in BYOP:
        return 'acc', None
    if a.startswith('#'):
        return 'imm', a[1:]
    if (name, 'rel') in BYOP:
        return 'rel', a
    m = re.fullmatch(r'\((.*),\s*[xX]\)', a)
    if m: return 'izx', m.group(1)
    m = re.fullmatch(r'\((.*)\),\s*[yY]', a)
    if m: return 'izy', m.group(1)
    m = re.fullmatch(r'\((.*)\)', a)
    if m and (name, 'ind') in BYOP: return 'ind', m.group(1)
    idx = None
    m = re.fullmatch(r'(.*),\s*([xXyY])', a)
    if m:
        a, idx = m.group(1), m.group(2).lower()
    v = _value(a, syms, final)
    zp = 0 <= v < 0x100  # (names used before they're defined count as big: define zero-page names first)
    base = {None: ('zp', 'abs'), 'x': ('zpx', 'abx'), 'y': ('zpy', 'aby')}[idx]
    if zp and (name, base[0]) in BYOP:
        return base[0], a
    return base[1], a


def assemble(src, origin=0x8000):
    """-> (bytes from origin, symbols)"""
    lines = []
    for n, raw in enumerate(src.splitlines(), 1):
        t = raw.split(';', 1)[0].rstrip()
        if t.strip():
            lines.append((n, t))
    syms, sizes = {}, {}
    for final in (False, True):
        pc, out, start = origin, bytearray(), None
        for n, t in lines:
            try:
                s = t.strip()
                m = re.match(r'([A-Za-z_.@][A-Za-z0-9_.@]*):\s*(.*)', s)
                if m:
                    syms[m.group(1)] = pc
                    s = m.group(2)
                    if not s: continue
                m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)', s)
                if m:
                    v = _value(m.group(2), syms, final)
                    syms[m.group(1)] = v
                    continue
                parts = s.split(None, 1)
                name, arg = parts[0].upper(), parts[1] if len(parts) > 1 else ''
                if name == '.ORG':
                    target = _value(arg, syms, final)
                    if target < pc: raise AsmError('.org goes backwards')
                    out += bytes(target - pc); pc = target
                elif name == '.BYTE':
                    for a in _split_args(arg):
                        if a.startswith('"'):
                            b = a.strip('"').encode()
                        else:
                            b = bytes([_value(a, syms, final) & 0xFF])
                        out += b; pc += len(b)
                elif name == '.WORD':
                    for a in _split_args(arg):
                        v = _value(a, syms, final)
                        out += bytes([v & 0xFF, v >> 8 & 0xFF]); pc += 2
                elif name == '.RES':
                    k = _value(arg, syms, final); out += bytes(k); pc += k
                else:
                    mode, e = _mode(name, arg, syms, final)
                    if (name, mode) not in BYOP:
                        raise AsmError(f'{name} has no {mode} form')
                    op = BYOP[(name, mode)]
                    nb = MODES[mode]
                    v = _value(e, syms, final) if e is not None else 0
                    if mode == 'rel':
                        d = v - (pc + 2)
                        if final and not -128 <= d <= 127: raise AsmError(f'branch too far ({d})')
                        v = d & 0xFF
                    if final and nb == 1 and not 0 <= v <= 0xFF and mode != 'rel':
                        raise AsmError(f'{v} does not fit in a byte')
                    out.append(op)
                    out += bytes([v & 0xFF, v >> 8 & 0xFF][:nb])
                    pc += 1 + nb
            except AsmError as err:
                raise AsmError(f'line {n}: {err}: {t.strip()}')
    return bytes(out), syms


def ines(prg, chr_, vertical=False):
    """an NROM cartridge file (16 or 32 KB of program, 8 KB of tiles)"""
    assert len(prg) in (16384, 32768) and len(chr_) == 8192
    head = b'NES\x1a' + bytes([len(prg) // 16384, 1, 1 if vertical else 0, 0]) + bytes(8)
    return head + prg + chr_
