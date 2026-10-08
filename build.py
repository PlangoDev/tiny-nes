# Builds tinynes.bin, the program TINY-32 runs.
#   python3 build.py              -> build/tinynes.bin with only the test cartridge (testcart.nes)
#   python3 build.py ROM_FOLDER   -> build/tinynes.bin with the boot screen + a menu of every .nes in that folder
#   python3 build.py --cart GAME.nes -> build/tinynes.bin that starts that one game straight away (no menu)
# Needs LLVM's clang, ld.lld and llvm-objcopy with the RISC-V target (see the README). Set LLVM=<its bin folder> if
# they aren't on your PATH.
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')
RT = os.path.join(HERE, 't32', 'rt')
ARCH = ['--target=riscv32-unknown-elf', '-march=rv32im', '-mabi=ilp32']
CFLAGS = ARCH + ['-O2', '-ffreestanding', '-nostdlibinc', '-I' + os.path.join(RT, 'include'), '-Wall', '-DNES_SCREEN=0xF0010000u']


def tool(name):
    exe = name + ('.exe' if os.name == 'nt' else '')
    places = [os.environ.get('LLVM', ''), '/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin', '/usr/local/opt/llvm/bin',
              '/usr/local/opt/lld/bin', r'C:\Program Files\LLVM\bin']
    for d in places:
        if d and os.path.exists(os.path.join(d, exe)):
            return os.path.join(d, exe)
    found = shutil.which(name)
    if not found:
        sys.exit(f"can't find {name}: install LLVM (see the README) or set LLVM to its bin folder")
    return found


def run(cmd):
    subprocess.run(cmd, check=True)


def main():
    os.makedirs(BUILD, exist_ok=True)
    clang, lld, objcopy = tool('clang'), tool('ld.lld'), tool('llvm-objcopy')
    o = lambda n: os.path.join(BUILD, n + '.o')
    args = sys.argv[1:]
    cart = os.path.join(HERE, 'testcart.nes')
    if args[:1] == ['--cart'] and len(args) == 2:
        cart, args = os.path.abspath(os.path.expanduser(args[1])), []
    games = len(args) > 0
    cflags = CFLAGS[:]
    if games:
        import library
        print('games:')
        library.main(os.path.expanduser(args[0]))
        cflags += ['-DGAMES', '-I' + BUILD]
        run([clang] + ARCH + ['-c', os.path.join(BUILD, 'games.S'), '-o', o('cart')])
    else:
        run([clang] + ARCH + ['-DCART="%s"' % cart.replace('\\', '/'), '-c', os.path.join(HERE, 't32', 'cart.S'), '-o', o('cart')])
    run([clang] + ARCH + ['-c', os.path.join(RT, 'crt0.S'), '-o', o('crt0')])
    for src, name in ((os.path.join(HERE, 't32', 'main.c'), 'main'), (os.path.join(HERE, 'nes.c'), 'nes'),
                      (os.path.join(RT, 'libc.c'), 'libc'), (os.path.join(RT, 'builtins.c'), 'builtins')):
        run([clang] + cflags + ['-c', src, '-o', o(name)])
    elf, out = os.path.join(BUILD, 'tinynes.elf'), os.path.join(BUILD, 'tinynes.bin')
    run([lld, '-T', os.path.join(RT, 'link.ld'), '--gc-sections', '-o', elf] + [o(n) for n in ('crt0', 'main', 'nes', 'cart', 'libc', 'builtins')])
    run([objcopy, '-O', 'binary', elf, out])
    print(f'{out}: {os.path.getsize(out):,} bytes')


if __name__ == '__main__':
    main()
