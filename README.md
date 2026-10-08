# TINY-NES

An NES emulator written in C that runs on **TINY-32**, a RISC-V computer built entirely from logic gates inside
[Tiny Logic](https://whoismuzzy.itch.io/tinylogic).

TINY-32's CPU (RV32IM) was built in the game, starting from a single NAND gate made of two transistors: 93,338
transistors in all. It passes the official [riscv-tests](https://github.com/riscv-software-src/riscv-tests) for
RV32IM. TINY-NES is an ordinary C program compiled for it, so every NES instruction you see is being worked out by that
gate-built CPU, inside the game's circuit simulator.

## What's implemented

- **6502 CPU**: every official instruction, with cycle counts. It passes all 5,003 official steps of nestest, and
  all 16 tests of blargg's `instr_test-v5` (official_only).
- **Picture chip (PPU)**: drawn a scanline at a time with the real scroll registers (so mid-frame scroll splits work),
  8x8 and 8x16 sprites, sprite 0 hit, the vblank NMI.
- **Mappers**: 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3, with its A12 scanline counter), 7 (AxROM),
  9 (MMC2). Cartridge RAM at $6000.
- **Sound (APU)**: the length counters, envelopes, sweeps, the triangle's linear counter and noise, played through
  the board's sound chip. The program hands the chip one register change each millisecond.
- **Input**: the board's gamepad (any controller SDL knows, Xbox over Bluetooth included) or your keyboard.
- **A games menu**: a boot screen, then a carousel of your games, each with an icon taken from its own picture.

## How fast it is

These are RISC-V instructions TINY-32 runs for each NES frame, measured with an instruction-counting RV32 simulator
over 900 frames (15 seconds) of each cartridge's title screen and demo:

| cartridge | RISC-V instructions per NES frame |
|---|---|
| our test cartridge (mapper 0) | 1.66 M |
| mapper 0 game | 1.59 M |
| mapper 1 game | 2.10 M |
| mapper 3 games | 1.67 M and 1.72 M |
| mapper 4 games | 1.68 M, 1.75 M and 1.94 M |
| mapper 9 game | 1.73 M |

Mappers 2 and 7 work the same way but weren't measured (no cartridge here uses them).

In Tiny Logic on an M2 MacBook Air, TINY-NES runs at the NES's full **60 frames a second**: about 100 million RISC-V
instructions a second from the gate-built CPU. The program holds itself to 60 frames a second using the board's
clock.

## Try it (no games needed)

`prebuilt/tinynes.bin` holds only our own test cartridge (`testcart.nes`, made by `testcart.py`). It runs 13 CPU
checks, shows palettes, a sprite you move with the pad, sprite 0 hit, a scroll split, the buttons you press and a
frame counter.

1. Open `board/TINY-NES.tlc` in Tiny Logic (it's TINY-32 with a 256x240 screen, a gamepad and a sound chip; its
   memories are empty).
2. Drag `prebuilt/tinynes.bin` onto the **PROGRAM** memory, then drag it onto the **RAM** too.
3. Turn on **POWER** (the toggle under the screen).
4. Press **K** to hand your keyboard to the circuit (Esc gives it back).

## Using your own games

**Tiny Logic doesn't include any games. Use ROMs dumped from cartridges you own.**

1. Put your `.nes` files in a folder, anywhere on your computer (not inside this repository).
2. Build the program with them baked in, plus the menu:

   ```
   python3 build.py path/to/your/folder
   ```

   This makes `build/tinynes.bin`. Each game's menu name comes from its file name. To start one game straight away
   with no menu, use `python3 build.py --cart path/to/game.nes`.
3. In Tiny Logic, open `board/TINY-NES.tlc` and drag `build/tinynes.bin` onto **PROGRAM**, then onto **RAM**.
4. Turn on **POWER**. TINY-NES drops in with a chime, then shows your games.
5. Press **K** to hand your keyboard to the circuit, or just use a controller.

Don't share a `tinynes.bin` that has games in it: it contains the ROMs.

### Controls

| NES | keyboard (after K) | controller |
|---|---|---|
| D-pad | arrow keys | D-pad |
| A | Z | A |
| B | X | B |
| START | Enter | Menu |
| SELECT | Tab | View |
| pick a game | Left / Right, then START or A | same |
| back to the menu | hold SELECT + START for 1.5 s | same |

Esc takes the keyboard back from the circuit.

## Building

You need Python 3 and LLVM's `clang`, `ld.lld` and `llvm-objcopy` with the RISC-V target (the usual LLVM builds
include it). Building with games also compiles a small PC copy of the emulator, to take each game's icon, so it
needs a compiler for your own computer as well (`cc`, or set `CC`).

- **macOS**: `brew install llvm lld`. `build.py` finds them in Homebrew's folders. Xcode's command line tools give
  you `cc`.
- **Windows**: install LLVM from [the LLVM releases](https://github.com/llvm/llvm-project/releases) (the
  `LLVM-*-win64.exe` installer includes `ld.lld`), and Python from python.org. Run `set CC=clang` before building
  with games.
- **Linux**: `sudo apt install clang lld llvm` (Debian / Ubuntu) or `sudo dnf install clang lld llvm` (Fedora).

If the tools aren't on your `PATH`, set `LLVM` to their folder, for example `LLVM=/usr/lib/llvm-18/bin`.
`t32/Makefile` does the same build for people who prefer `make`.

## What's in here

| | |
|---|---|
| `nes.c`, `nes.h` | the NES: 6502, picture chip, sound, mappers |
| `t32/main.c` | TINY-32's side: screen, gamepad, sound chip, 60 frames a second, the boot screen and menu |
| `t32/cart.S` | links one cartridge into the program |
| `t32/rt/` | the start-up code, linker script and a small C library for TINY-32 |
| `build.py`, `t32/Makefile` | the build |
| `library.py` | turns a folder of your games into the menu (icons + names) |
| `testcart.py`, `asm6502.py` | our test cartridge and the small 6502 assembler it's written with |
| `m6502.py` | a Python 6502 model `testcart.py` checks the cartridge with |
| `host.c` | TINY-NES on a PC: frames to pictures, and the test ROM checks below |
| `board/TINY-NES.tlc` | the Tiny Logic board, memories empty |
| `prebuilt/tinynes.bin` | the program with only the test cartridge |

### Test ROMs

The test ROMs aren't included. Get them from their authors:
[nestest](https://www.qmtpro.com/~nes/misc/nestest.nes) (with its [log](https://www.qmtpro.com/~nes/misc/nestest.log))
and blargg's tests from [christopherpow/nes-test-roms](https://github.com/christopherpow/nes-test-roms). Then:

```
cc -O2 -o host host.c nes.c
./host nestest nestest.nes nestest.log
./host blargg official_only.nes
```

## License

MIT (see `LICENSE`). Tiny Logic and TINY-32 are by Mazell Smack-Dubose.

TINY-NES is not affiliated with or endorsed by Nintendo. NES and Nintendo Entertainment System are trademarks of
Nintendo.
