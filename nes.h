// TINY-NES's NES (nes.c): load a cartridge, set the pad, run frames, read the picture (palette numbers 0-63).
#pragma once
#include <stdint.h>
#define NES_W 256
#define NES_H 240
struct nes_regs { uint16_t pc; uint8_t a, x, y, p, s; };
int nes_load(const uint8_t *rom, unsigned len);
void nes_pad(uint8_t buttons);  // bit 0 A, 1 B, 2 select, 3 start, 4 up, 5 down, 6 left, 7 right
int nes_frame_run(void);
int nes_step(void);
void nes_regs(struct nes_regs *r);
void nes_set_pc(uint16_t pc);
long nes_cycles(void);
void nes_sound(uint8_t out[16]);  // the sound chip's 16 registers: voice k = note, volume, on, 0 at 4k..4k+3
const uint8_t *nes_wram(void);  // the cartridge's RAM at $6000 (test ROMs report there)
#ifndef NES_SCREEN
extern uint8_t nes_frame[NES_W * NES_H];
#endif
