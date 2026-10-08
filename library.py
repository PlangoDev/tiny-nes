# TINY-NES's game library: every .nes in a folder (your own dumps: they stay on your computer, never in a repo), each
# with a 128x120 icon (its picture after a few seconds on the PC harness, halved), linked into the TINY-32 program for
# the menu (t32/main.c). The name shown is the file's name.
#   python3 library.py ROM_FOLDER     -> build/games.S, build/games.h (build.py runs this for you)
import glob
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')
ICON_FRAMES = 300  # (5 seconds in: most games show their title by then)


def host():
    """the PC harness (host.c + nes.c), built with the computer's own compiler"""
    exe = os.path.join(BUILD, 'host.exe' if os.name == 'nt' else 'host')
    if not os.path.exists(exe):
        os.makedirs(BUILD, exist_ok=True)
        cc = os.environ.get('CC') or 'cc'
        subprocess.run([cc, '-O2', '-w', '-o', exe, 'host.c', 'nes.c'], cwd=HERE, check=True)
    return exe


def icon(rom, frames):
    fd, out = tempfile.mkstemp(suffix='.raw')
    os.close(fd)
    subprocess.run([host(), 'icon', rom, str(frames), out], check=True)
    d = open(out, 'rb').read()
    os.remove(out)
    px = bytearray()
    for y in range(0, 240, 2):   # each 2x2 block: its most common colour (ties: top-left)
        for x in range(0, 256, 2):
            b = [d[y * 256 + x], d[y * 256 + x + 1], d[(y + 1) * 256 + x], d[(y + 1) * 256 + x + 1]]
            px.append(max(b, key=lambda c: (b.count(c), c == b[0])))
    return bytes(px)


def label(path):
    """the menu's name for a game: its file name, without anything in brackets, in capitals (the menu's font)"""
    name = os.path.splitext(os.path.basename(path))[0]
    for c in '([':
        name = name.split(c)[0]
    name = ''.join(ch for ch in name.strip().upper() if 32 <= ord(ch) < 96 and ch not in '"\\')
    return name[:24] or 'GAME'


def asm_path(p):
    return os.path.abspath(p).replace('\\', '/')


def main(folder):
    roms = sorted(glob.glob(os.path.join(folder, '**', '*.nes'), recursive=True))
    if not roms:
        sys.exit(f'no .nes files in {folder}')
    os.makedirs(BUILD, exist_ok=True)
    with open(os.path.join(BUILD, 'games.S'), 'w') as s, open(os.path.join(BUILD, 'games.h'), 'w') as h:
        s.write('    .section .rodata\n')
        h.write('// made by library.py\n')
        for i, rom in enumerate(roms):
            ic = os.path.join(BUILD, f'icon{i}.bin')
            open(ic, 'wb').write(icon(rom, ICON_FRAMES))
            s.write(f'    .globl rom{i}, rom{i}_end, icon{i}\n    .balign 4\nrom{i}:\n    .incbin "{asm_path(rom)}"\nrom{i}_end:\n'
                    f'    .balign 4\nicon{i}:\n    .incbin "{asm_path(ic)}"\n')
            h.write(f'extern const unsigned char rom{i}[], rom{i}_end[], icon{i}[];\n')
        h.write(f'#define GAME_COUNT {len(roms)}\nstatic const struct game {{ const char *name, *info; const unsigned char *rom, *end, *icon; }} GAME_LIST[] = {{\n')
        for i, rom in enumerate(roms):
            h.write(f'    {{"{label(rom)}", "", rom{i}, rom{i}_end, icon{i}}},\n')
        h.write('};\n')
    for rom in roms:
        print(f'  {label(rom):24s} {os.path.basename(rom)}')
    return len(roms)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('python3 library.py ROM_FOLDER')
    main(os.path.expanduser(sys.argv[1]))
