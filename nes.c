// TINY-NES: an NES (6502 + picture chip + pad) in plain C, for TINY-32 (or a PC, through host.c).
// Official 6502 instructions with their cycle counts; the picture chip draws a whole scanline at a time with the
// real scroll registers (so mid-frame scroll splits work), sprites 8x8 / 8x16, sprite 0 hit, vblank NMI.
// Cartridges: mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), 7 (AxROM), 9 (MMC2). Sound: nes_sound().
#include "nes.h"

static uint8_t ram[0x800], vram[0x800], pal[32], oam[256];
// ---- the cartridge: program in 8K slots, pictures in 1K slots, and how its chip switches them
static const uint8_t *prg;
static uint8_t *chrmem;            // the cartridge's picture memory (ROM, or 8K of RAM)
static uint8_t chrram[0x2000], wram[0x2000];
static uint32_t nprg, nchr;        // sizes in bytes
static int chr_is_ram;
static const uint8_t *prgmap[4];   // $8000 $A000 $C000 $E000
static uint8_t *chrmap[8];         // $0000 ... $1C00
static int mirroring;              // 0 horizontal, 1 vertical, 2 one screen low, 3 one screen high
static int mapper;
static uint8_t mreg[16];           // the mapper chip's registers
static int irq_line;               // MMC3's scanline counter wants an interrupt
static uint8_t irq_latch, irq_count, irq_on, irq_reload;

// ---- picture chip registers
static uint8_t ctrl, mask, status, oamaddr, readbuf, w_toggle, finex;
static uint16_t v, t;
static int line;       // scanline 0..261
static uint8_t pad_state, pad_shift, pad_strobe;
#ifdef NES_SCREEN  // (TINY-32: the picture is drawn straight into the screen's memory)
#define nes_frame ((uint8_t *)NES_SCREEN)
#else
uint8_t nes_frame[NES_W * NES_H] __attribute__((aligned(4)));  // palette numbers 0-63
#endif

// ---- 6502
static uint8_t A, X, Y, S, P;
static uint16_t PC;
static int nmi_line;
static void a12(int high);  // (MMC3's line counter, below)
static void apu_write(uint16_t a, uint8_t d);  // (sound, below)

static uint8_t *ntaddr(uint16_t a) {
    a &= 0xFFF;
    int table = a >> 10;
    int page = mirroring == 1 ? (table & 1) : mirroring == 0 ? (table >> 1) : mirroring - 2;
    return &vram[page << 10 | (a & 0x3FF)];
}

static uint8_t ppu_read(uint16_t a) {
    a &= 0x3FFF;
    if (a < 0x2000) return chrmap[a >> 10][a & 0x3FF];
    if (a < 0x3F00) return *ntaddr(a);
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= ~0x10;
    return pal[a];
}

static void ppu_write(uint16_t a, uint8_t d) {
    a &= 0x3FFF;
    if (a < 0x2000) { if (chr_is_ram) chrmap[a >> 10][a & 0x3FF] = d; return; }
    if (a < 0x3F00) { *ntaddr(a) = d; return; }
    a &= 0x1F;
    if ((a & 0x13) == 0x10) a &= ~0x10;
    pal[a] = d & 0x3F;
}

static uint8_t rd_io(uint16_t a) {
    if (a < 0x4000) {
        switch (a & 7) {
            case 2: { uint8_t s = status; status &= 0x7F; w_toggle = 0; return s; }
            case 4: return oam[oamaddr];
            case 7: {
                uint8_t d = readbuf;
                readbuf = ppu_read(v);
                if ((v & 0x3FFF) >= 0x3F00) d = readbuf;
                v += ctrl & 4 ? 32 : 1;
                if (mapper == 4) a12(v & 0x1000);
                return d;
            }
        }
        return 0;
    }
    if (a >= 0x6000) return wram[a & 0x1FFF];
    if (a == 0x4016) {
        uint8_t b = pad_shift & 1;
        if (!pad_strobe) pad_shift = pad_shift >> 1 | 0x80;
        return 0x40 | b;
    }
    return 0;
}

static inline __attribute__((always_inline)) uint8_t rd(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7FF];
    if (a >= 0x8000) return prgmap[(a >> 13) & 3][a & 0x1FFF];
    return rd_io(a);
}

static void prg16(int slot, int bank) {  // a 16K bank into slots slot, slot+1
    unsigned n = nprg / 16384;
    const uint8_t *b = prg + ((unsigned)bank % n) * 16384;
    prgmap[slot] = b; prgmap[slot + 1] = b + 8192;
}
static void prg8(int slot, int bank) { prgmap[slot] = prg + ((unsigned)bank % (nprg / 8192)) * 8192; }
static void chr1(int slot, int bank) { chrmap[slot] = chrmem + ((unsigned)bank % (nchr / 1024)) * 1024; }
static void chrk(int slot, int bank, int kb) { for (int i = 0; i < kb; i++) chr1(slot + i, bank * kb + i); }

static void map_update(void) {
    switch (mapper) {
        case 0:
            prg16(0, 0); prg16(2, nprg > 16384 ? 1 : 0); chrk(0, 0, 8); break;
        case 1: {  // MMC1: 0 control, 1 CHR 0, 2 CHR 1, 3 PRG
            int ctl = mreg[0];
            mirroring = (ctl & 3) == 0 ? 2 : (ctl & 3) == 1 ? 3 : (ctl & 3) == 2 ? 1 : 0;
            int pm = (ctl >> 2) & 3, pb = mreg[3] & 15;
            if (pm < 2) { prg16(0, pb & ~1); prg16(2, pb | 1); }
            else if (pm == 2) { prg16(0, 0); prg16(2, pb); }
            else { prg16(0, pb); prg16(2, -1 + (int)(nprg / 16384)); }
            if (ctl & 0x10) { chrk(0, mreg[1], 4); chrk(4, mreg[2], 4); }
            else chrk(0, mreg[1] >> 1, 8);
            break;
        }
        case 2: prg16(0, mreg[0]); prg16(2, (int)(nprg / 16384) - 1); chrk(0, 0, 8); break;
        case 3: prg16(0, 0); prg16(2, nprg > 16384 ? 1 : 0); chrk(0, mreg[0], 8); break;
        case 7:
            prg16(0, (mreg[0] & 7) * 2); prg16(2, (mreg[0] & 7) * 2 + 1);
            mirroring = mreg[0] & 0x10 ? 3 : 2; chrk(0, 0, 8); break;
        case 9: {  // MMC2: 0 PRG, 1-2 CHR $0000 for latch FD / FE, 3-4 CHR $1000, 5-6 the two latches
            int last = (int)(nprg / 8192) - 1;
            prg8(0, mreg[0]); prg8(1, last - 2); prg8(2, last - 1); prg8(3, last);
            chrk(0, mreg[5] ? mreg[2] : mreg[1], 4); chrk(4, mreg[6] ? mreg[4] : mreg[3], 4);
            break;
        }
        case 4: {  // MMC3: 8 bank registers in mreg[0..7], mreg[8] = bank select
            int last = (int)(nprg / 8192) - 1, sel = mreg[8];
            prg8(sel & 0x40 ? 2 : 0, mreg[6]); prg8(1, mreg[7]);
            prg8(sel & 0x40 ? 0 : 2, last - 1); prg8(3, last);
            int x = sel & 0x80 ? 4 : 0;  // (which half gets the two 2K banks)
            chr1(x + 0, mreg[0] & 0xFE); chr1(x + 1, mreg[0] | 1);
            chr1(x + 2, mreg[1] & 0xFE); chr1(x + 3, mreg[1] | 1);
            chr1(4 - x, mreg[2]); chr1(5 - x, mreg[3]); chr1(6 - x, mreg[4]); chr1(7 - x, mreg[5]);
            break;
        }
    }
}

static void map_write(uint16_t a, uint8_t d) {
    switch (mapper) {
        case 1:
            if (d & 0x80) { mreg[9] = 0; mreg[10] = 0; mreg[0] |= 0x0C; map_update(); break; }
            mreg[9] |= (uint8_t)((d & 1) << mreg[10]);
            if (++mreg[10] == 5) {
                mreg[(a >> 13) & 3] = mreg[9];
                mreg[9] = mreg[10] = 0;
                map_update();
            }
            break;
        case 2: case 3: case 7: mreg[0] = d; map_update(); break;
        case 9:
            switch (a & 0xF000) {
                case 0xA000: mreg[0] = d & 15; break; case 0xB000: mreg[1] = d & 31; break; case 0xC000: mreg[2] = d & 31; break;
                case 0xD000: mreg[3] = d & 31; break; case 0xE000: mreg[4] = d & 31; break;
                case 0xF000: mirroring = d & 1 ? 0 : 1; break;
            }
            map_update();
            break;
        case 4:
            switch (a & 0xE001) {
                case 0x8000: mreg[8] = d; map_update(); break;
                case 0x8001: mreg[mreg[8] & 7] = d; map_update(); break;
                case 0xA000: mirroring = d & 1 ? 0 : 1; break;
                case 0xC000: irq_latch = d; break;
                case 0xC001: irq_count = 0; irq_reload = 1; break;
                case 0xE000: irq_on = 0; irq_line = 0; break;
                case 0xE001: irq_on = 1; break;
            }
            break;
    }
}

// MMC3 counts rising edges of the picture chip's address line A12 (low for a little while first): the pattern
// fetches make one a line while drawing, and the game's own $2006 / $2007 accesses can make them too.
static int a12_last;
static long a12_low_since;
static long cycles;
static void scanline_clock(void);
static void a12(int high) {
    if (high && !a12_last && cycles - a12_low_since >= 3) scanline_clock();
    if (!high && a12_last) a12_low_since = cycles;
    a12_last = high;
}
static void scanline_clock(void) {
    if (mapper != 4) return;
    if (irq_count == 0 || irq_reload) { irq_count = irq_latch; irq_reload = 0; }
    else irq_count--;
    if (irq_count == 0 && irq_on) irq_line = 1;
}

static void wr(uint16_t a, uint8_t d) {
    if (a >= 0x8000) { map_write(a, d); return; }
    if (a >= 0x6000) { wram[a & 0x1FFF] = d; return; }
    if (a < 0x2000) { ram[a & 0x7FF] = d; return; }
    if (a < 0x4000) {
        switch (a & 7) {
            case 0:
                if (!(ctrl & 0x80) && (d & 0x80) && (status & 0x80)) nmi_line = 1;
                ctrl = d; t = (t & 0xF3FF) | (d & 3) << 10; break;
            case 1: mask = d; break;
            case 3: oamaddr = d; break;
            case 4: oam[oamaddr++] = d; break;
            case 5:
                if (!w_toggle) { t = (t & 0xFFE0) | d >> 3; finex = d & 7; }
                else t = (t & 0x8C1F) | (d & 0xF8) << 2 | (d & 7) << 12;
                w_toggle ^= 1; break;
            case 6:
                if (!w_toggle) t = (t & 0x00FF) | (d & 0x3F) << 8;
                else { t = (t & 0xFF00) | d; v = t; if (mapper == 4) a12(v & 0x1000); }
                w_toggle ^= 1; break;
            case 7: ppu_write(v, d); v += ctrl & 4 ? 32 : 1; if (mapper == 4) a12(v & 0x1000); break;
        }
        return;
    }
    if (a == 0x4014) {
        for (int i = 0; i < 256; i++) oam[(oamaddr + i) & 0xFF] = rd((uint16_t)(d << 8 | i));
        cycles += 513;
        return;
    }
    if (a < 0x4018 && a != 0x4016) { apu_write(a, d); return; }
    if (a == 0x4016) {
        pad_strobe = d & 1;
        if (pad_strobe) pad_shift = pad_state;
        return;
    }
}


// ---- sound: the NES's sound unit, kept as its registers say (lengths, envelopes, sweeps, the triangle's counter),
// then handed out as the game's sound chip wants it: a note (MIDI), a volume 0-15 and on/off for each of 4 voices.
static const uint16_t NOTE_PERIOD[128] = {13681,12913,12188,11504,10858,10249,9674,9131,8618,8134,7678,7247,6840,6456,6094,5752,5429,5124,4836,4565,4309,4067,3838,3623,3419,3228,3046,2875,2714,2561,2418,2282,2154,2033,1919,1811,1709,1613,1523,1437,1356,1280,1208,1140,1076,1016,959,905,854,806,761,718,678,640,604,570,538,507,479,452,427,403,380,359,338,319,301,284,268,253,239,225,213,201,189,179,169,159,150,142,134,126,119,112,106,100,94,89,84,79,75,70,66,63,59,56,52,49,47,44,41,39,37,35,33,31,29,27,26,24,23,21,20,19,18,17,16,15,14,13,12,12,11,10,10,9,8,8};  // pulse timer for each note
static const uint8_t LENGTHS[32] = {10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30};
struct voice {
    uint8_t reg[4], length, env, env_div, env_start, lin, lin_reload, sweep_div, sweep_reload, mute;
    uint16_t period;
};
static struct voice snd[4];  // pulse 1, pulse 2, triangle, noise
static uint8_t snd_on;       // $4015's channel enables

static void apu_write(uint16_t a, uint8_t d) {
    if (a == 0x4015) {
        snd_on = d & 0x0F;
        for (int k = 0; k < 4; k++) if (!(d >> k & 1)) snd[k].length = 0;
        return;
    }
    int k = (a - 0x4000) >> 2, r = a & 3;
    if (k > 3) return;
    struct voice *c = &snd[k];
    c->reg[r] = d;
    if (k < 3 && r == 2) c->period = (uint16_t)((c->period & 0x700) | d);
    if (k < 3 && r == 3) c->period = (uint16_t)((c->period & 0xFF) | (d & 7) << 8);
    if (r == 3) {
        if (snd_on >> k & 1) c->length = LENGTHS[d >> 3];
        c->env_start = 1;
        if (k == 2) c->lin_reload = 1;
    }
    if (k < 2 && r == 1) c->sweep_reload = 1;
}

static void apu_quarter(void) {  // envelopes and the triangle's counter, 240 times a second
    for (int k = 0; k < 4; k++) {
        struct voice *c = &snd[k];
        if (k == 2) {
            if (c->lin_reload) c->lin = c->reg[0] & 0x7F;
            else if (c->lin) c->lin--;
            if (!(c->reg[0] & 0x80)) c->lin_reload = 0;
            continue;
        }
        if (c->env_start) { c->env_start = 0; c->env = 15; c->env_div = c->reg[0] & 15; }
        else if (c->env_div) c->env_div--;
        else {
            c->env_div = c->reg[0] & 15;
            if (c->env) c->env--;
            else if (c->reg[0] & 0x20) c->env = 15;  // (looping)
        }
    }
}

static void apu_half(void) {  // lengths and sweeps, 120 times a second
    for (int k = 0; k < 4; k++) {
        struct voice *c = &snd[k];
        const int halt = k == 2 ? c->reg[0] & 0x80 : c->reg[0] & 0x20;
        if (c->length && !halt) c->length--;
        if (k < 2) {
            const uint8_t sw = c->reg[1];
            int change = c->period >> (sw & 7);
            int target = sw & 8 ? c->period - change - (k == 0) : c->period + change;
            c->mute = c->period < 8 || target > 0x7FF;
            if (!c->sweep_div && (sw & 0x80) && (sw & 7) && !c->mute) c->period = (uint16_t)target;
            if (!c->sweep_div || c->sweep_reload) { c->sweep_div = (sw >> 4) & 7; c->sweep_reload = 0; }
            else c->sweep_div--;
        }
    }
}

static int note_of(unsigned period) {  // the nearest note for a pulse timer value
    int lo = 0, hi = 127;
    while (lo < hi) { int m = (lo + hi) / 2; if (NOTE_PERIOD[m] > period) lo = m + 1; else hi = m; }
    if (lo > 0 && NOTE_PERIOD[lo - 1] - period < period - NOTE_PERIOD[lo]) lo--;
    return lo;
}

void nes_sound(uint8_t out[16]) {
    for (int k = 0; k < 4; k++) {
        const struct voice *c = &snd[k];
        int note = 0, vol = 0, on = (snd_on >> k & 1) && c->length;
        if (k < 2) {
            note = note_of(c->period);
            vol = c->reg[0] & 0x10 ? c->reg[0] & 15 : c->env;
            on = on && !c->mute;
        } else if (k == 2) {
            note = note_of(c->period) - 12;  // (the triangle sounds an octave below a pulse with the same timer)
            vol = 15;
            on = on && c->lin && c->period >= 2;
        } else {
            note = 100 - 4 * (c->reg[2] & 15);  // (noise: higher for shorter periods)
            vol = c->reg[0] & 0x10 ? c->reg[0] & 15 : c->env;
        }
        out[4 * k] = (uint8_t)(note < 0 ? 0 : note);
        out[4 * k + 1] = (uint8_t)vol;
        out[4 * k + 2] = (uint8_t)on;
        out[4 * k + 3] = 0;
    }
}

// ---- one scanline of picture
// Four dots at a time: a tile row's two pattern bytes become byte masks (spread4: a nibble -> 0xFF per set bit,
// leftmost dot in the lowest byte), and each colour is picked with masks, a whole 32-bit word at once.
static uint32_t spread4[16];
static uint32_t lb[66], lm[66];  // the line from the start of its first tile (fine scroll not taken off yet), and where it's opaque

static uint8_t sprline[NES_H];  // lines with a sprite on them, worked out at the top of each frame

static void find_sprites(void) {
    int h = ctrl & 0x20 ? 16 : 8;
    for (int y = 0; y < NES_H; y++) sprline[y] = 0;
    for (int i = 0; i < 64; i++)
        for (int y = oam[i * 4] + 1, k = 0; k < h && y < NES_H; k++, y++) sprline[y] = 1;
}

// MMC2 swaps a picture bank when the picture chip reads tile $FD or $FE's second plane (after that read)
static void mmc2_latch(unsigned a) {
    int was5 = mreg[5], was6 = mreg[6];
    if (a == 0x0FD8) mreg[5] = 0; else if (a == 0x0FE8) mreg[5] = 1;
    else if ((a & 0xFFF8) == 0x1FD8) mreg[6] = 0; else if ((a & 0xFFF8) == 0x1FE8) mreg[6] = 1;
    if (mreg[5] != was5 || mreg[6] != was6) map_update();
}

static void render_line(int y) {
    uint8_t *out = &nes_frame[y * NES_W];
    uint32_t *out32 = (uint32_t *)out;
    uint8_t bgpix[NES_W] __attribute__((aligned(4)));  // nonzero where the background is opaque (sprite priority, sprite 0); bit 7: a sprite drew here
    const int spr = (mask & 0x10) && sprline[y];
    const uint32_t back = pal[0] * 0x01010101u;
    if (!spread4[15])
        for (int n = 0; n < 16; n++)
            for (int j = 0; j < 4; j++)
                if (n & (8 >> j)) spread4[n] |= 0xFFu << (8 * j);
    if (mask & 0x08) {
        uint16_t vv = v;
        const int ptbase = (ctrl & 0x10) << 8 | (vv >> 12);
        const uint8_t *ntp[4];  // the four nametables as the mirroring lays them out
        for (int k = 0; k < 4; k++) ntp[k] = ntaddr((uint16_t)(k << 10));
        for (int t = 0; t < 33; t++) {
            const uint8_t *nt = ntp[(vv >> 10) & 3];
            uint8_t attr = nt[0x3C0 | ((vv >> 4) & 0x38) | ((vv >> 2) & 7)];
            int palhi = ((attr >> (((vv >> 4) & 4) | (vv & 2))) & 3) << 2;
            const int pa = ptbase | nt[vv & 0x3FF] << 4;
            const uint8_t *row = chrmap[pa >> 10] + (pa & 0x3FF);
            unsigned lo = row[0], hi = row[8];
            if (mapper == 9) mmc2_latch((unsigned)(pa | 8) & 0x1FF8);
            // colour per dot = L ? (H ? c3 : c1) : (H ? c2 : back), picked with masks
            const uint32_t c2 = pal[palhi | 2] * 0x01010101u;
            const uint32_t d1 = back ^ pal[palhi | 1] * 0x01010101u, d2 = c2 ^ pal[palhi | 3] * 0x01010101u;
            for (int half = 0; half < 2; half++) {
                const uint32_t L = spread4[half ? lo & 15 : lo >> 4], H = spread4[half ? hi & 15 : hi >> 4];
                const uint32_t a = back ^ (d1 & L), b = c2 ^ (d2 & L);
                lb[2 * t + half] = a ^ ((a ^ b) & H);
                if (spr) lm[2 * t + half] = L | H;
            }
            if ((vv & 0x1F) == 31) { vv &= ~0x1F; vv ^= 0x400; } else vv++;
        }
        const int w = finex >> 2, sh = 8 * (finex & 3);  // (fine scroll: whole words, then bytes)
        for (int i = 0; i < NES_W / 4; i++) {
            out32[i] = sh ? lb[w + i] >> sh | lb[w + i + 1] << (32 - sh) : lb[w + i];
            if (spr) ((uint32_t *)bgpix)[i] = sh ? lm[w + i] >> sh | lm[w + i + 1] << (32 - sh) : lm[w + i];
        }
        if (!(mask & 0x02)) {  // the leftmost 8 dots hidden
            out32[0] = out32[1] = back;
            if (spr) ((uint32_t *)bgpix)[0] = ((uint32_t *)bgpix)[1] = 0;
        }
    } else {
        for (int i = 0; i < NES_W / 4; i++) out32[i] = back;
        if (spr) for (int x = 0; x < NES_W; x++) bgpix[x] = 0;
    }
    if (!spr) return;
    for (int x = 0; x < NES_W; x++) bgpix[x] &= 3;
    if (!(mask & 0x10)) return;
    int h = ctrl & 0x20 ? 16 : 8, found = 0;
    for (int i = 0; i < 64 && found < 8; i++) {
        const uint8_t *s = &oam[i * 4];
        int row = y - (s[0] + 1);
        if (row < 0 || row >= h) continue;
        found++;
        uint8_t at = s[2];
        if (at & 0x80) row = h - 1 - row;
        uint16_t pa;
        if (h == 16) pa = (uint16_t)((s[1] & 1) << 12 | (s[1] & 0xFE) << 4 | (row & 8) << 1 | (row & 7));
        else pa = (uint16_t)((ctrl & 0x08) << 9 | s[1] << 4 | row);
        uint8_t lo = ppu_read(pa), hi = ppu_read(pa + 8);
        if (mapper == 9) mmc2_latch((unsigned)(pa + 8) & 0x1FF8);
        for (int k = 0; k < 8; k++) {
            int x = s[3] + k;
            if (x >= NES_W) break;
            int bit = at & 0x40 ? k : 7 - k;
            int c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
            if (!c || (x < 8 && !(mask & 0x04))) continue;
            if (i == 0 && (bgpix[x] & 3) && x != 255) status |= 0x40;
            // an earlier sprite already drew here: it wins (marked with bit 7 of bgpix)
            if (bgpix[x] & 0x80) continue;
            if (!(at & 0x20) || !(bgpix[x] & 3)) out[x] = pal[0x10 | (at & 3) << 2 | c];
            bgpix[x] |= 0x80;
        }
    }
}

// ---- the 6502
static const uint8_t CYC[256] = {
    7,6,0,0,0,3,5,0,3,2,2,0,0,4,6,0, 2,5,0,0,0,4,6,0,2,4,0,0,0,4,7,0,
    6,6,0,0,3,3,5,0,4,2,2,0,4,4,6,0, 2,5,0,0,0,4,6,0,2,4,0,0,0,4,7,0,
    6,6,0,0,0,3,5,0,3,2,2,0,3,4,6,0, 2,5,0,0,0,4,6,0,2,4,0,0,0,4,7,0,
    6,6,0,0,0,3,5,0,4,2,2,0,5,4,6,0, 2,5,0,0,0,4,6,0,2,4,0,0,0,4,7,0,
    0,6,0,0,3,3,3,0,2,0,2,0,4,4,4,0, 2,6,0,0,4,4,4,0,2,5,2,0,0,5,0,0,
    2,6,2,0,3,3,3,0,2,2,2,0,4,4,4,0, 2,5,0,0,4,4,4,0,2,4,2,0,4,4,4,0,
    2,6,0,0,3,3,5,0,2,2,2,0,4,4,6,0, 2,5,0,0,0,4,6,0,2,4,0,0,0,4,7,0,
    2,6,0,0,3,3,5,0,2,2,2,0,4,4,6,0, 2,5,0,0,0,4,6,0,2,4,0,0,0,4,7,0,
};

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

static void push(uint8_t d) { ram[0x100 | S] = d; S--; }
static uint8_t pull(void) { S++; return ram[0x100 | S]; }
static inline __attribute__((always_inline)) uint8_t nz(uint8_t d) { P = (uint8_t)((P & ~(FN | FZ)) | (d & FN) | (d ? 0 : FZ)); return d; }
static inline __attribute__((always_inline)) uint16_t rd16(uint16_t a) { return (uint16_t)(rd(a) | rd((uint16_t)(a + 1)) << 8); }
static void interrupt(uint16_t vec, int brk) {
    push(PC >> 8); push(PC & 0xFF); push((uint8_t)(P | FU | (brk ? FB : 0)));
    P |= FI; PC = rd16(vec); cycles += 7;
}

// operand address for each addressing mode (pc moves past the operand)
static inline __attribute__((always_inline)) uint16_t a_imm(void) { return PC++; }
static inline __attribute__((always_inline)) uint16_t a_zp(void) { return rd(PC++); }
static inline __attribute__((always_inline)) uint16_t a_zpx(void) { return (uint8_t)(rd(PC++) + X); }
static inline __attribute__((always_inline)) uint16_t a_zpy(void) { return (uint8_t)(rd(PC++) + Y); }
static inline __attribute__((always_inline)) uint16_t a_abs(void) { uint16_t w = rd16(PC); PC += 2; return w; }
static inline __attribute__((always_inline)) uint16_t a_abx(void) { uint16_t w = rd16(PC); PC += 2; uint16_t e = w + X; if ((e ^ w) & 0xFF00) cycles++; return e; }
static inline __attribute__((always_inline)) uint16_t a_aby(void) { uint16_t w = rd16(PC); PC += 2; uint16_t e = w + Y; if ((e ^ w) & 0xFF00) cycles++; return e; }
static inline __attribute__((always_inline)) uint16_t a_izx(void) { uint8_t z = (uint8_t)(rd(PC++) + X); return (uint16_t)(ram[z] | ram[(uint8_t)(z + 1)] << 8); }
static inline __attribute__((always_inline)) uint16_t a_izy(void) {
    uint8_t z = rd(PC++);
    uint16_t w = (uint16_t)(ram[z] | ram[(uint8_t)(z + 1)] << 8), e = w + Y;
    if ((e ^ w) & 0xFF00) cycles++;
    return e;
}
// stores and read-modify-writes never take the page-crossing cycle (it's already in their count)
static inline __attribute__((always_inline)) uint16_t s_abx(void) { uint16_t w = rd16(PC); PC += 2; return w + X; }
static inline __attribute__((always_inline)) uint16_t s_aby(void) { uint16_t w = rd16(PC); PC += 2; return w + Y; }
static inline __attribute__((always_inline)) uint16_t s_izy(void) { uint8_t z = rd(PC++); return (uint16_t)((ram[z] | ram[(uint8_t)(z + 1)] << 8) + Y); }

static inline __attribute__((always_inline)) void adc(uint8_t m) {
    unsigned s = A + m + (P & FC);
    P = (uint8_t)((P & ~(FV | FC)) | ((~(A ^ m) & (A ^ s) & 0x80) ? FV : 0) | (s > 0xFF ? FC : 0));
    A = nz((uint8_t)s);
}
static inline __attribute__((always_inline)) void cmp(uint8_t r, uint8_t m) { P = (uint8_t)((P & ~FC) | (r >= m ? FC : 0)); nz((uint8_t)(r - m)); }
static inline __attribute__((always_inline)) void branch(int take) {
    int8_t off = (int8_t)rd(PC++);
    if (take) { uint16_t n = (uint16_t)(PC + off); cycles += ((n ^ PC) & 0xFF00) ? 2 : 1; PC = n; }
}
static inline __attribute__((always_inline)) uint8_t asl(uint8_t d) { P = (uint8_t)((P & ~FC) | (d >> 7)); return nz((uint8_t)(d << 1)); }
static inline __attribute__((always_inline)) uint8_t lsr(uint8_t d) { P = (uint8_t)((P & ~FC) | (d & 1)); return nz(d >> 1); }
static inline __attribute__((always_inline)) uint8_t rol(uint8_t d) { uint8_t c = P & FC; P = (uint8_t)((P & ~FC) | (d >> 7)); return nz((uint8_t)(d << 1 | c)); }
static inline __attribute__((always_inline)) uint8_t ror(uint8_t d) { uint8_t c = P & FC; P = (uint8_t)((P & ~FC) | (d & 1)); return nz((uint8_t)(d >> 1 | c << 7)); }

#define RMW(f, mode) { uint16_t e = mode(); wr(e, f(rd(e))); } break
#define LOGIC(op, mode) A = nz(A op rd(mode())); break

static inline __attribute__((always_inline)) int step(void) {
    if (nmi_line) { nmi_line = 0; interrupt(0xFFFA, 0); return 7; }
    if (irq_line && !(P & FI)) { interrupt(0xFFFE, 0); return 7; }
    long before = cycles;
    uint8_t op = rd(PC++);
    cycles += CYC[op];
    switch (op) {
        case 0x69: adc(rd(a_imm())); break; case 0x65: adc(rd(a_zp())); break; case 0x75: adc(rd(a_zpx())); break;
        case 0x6D: adc(rd(a_abs())); break; case 0x7D: adc(rd(a_abx())); break; case 0x79: adc(rd(a_aby())); break;
        case 0x61: adc(rd(a_izx())); break; case 0x71: adc(rd(a_izy())); break;
        case 0xE9: adc(~rd(a_imm())); break; case 0xE5: adc(~rd(a_zp())); break; case 0xF5: adc(~rd(a_zpx())); break;
        case 0xED: adc(~rd(a_abs())); break; case 0xFD: adc(~rd(a_abx())); break; case 0xF9: adc(~rd(a_aby())); break;
        case 0xE1: adc(~rd(a_izx())); break; case 0xF1: adc(~rd(a_izy())); break;
        case 0x29: LOGIC(&, a_imm); case 0x25: LOGIC(&, a_zp); case 0x35: LOGIC(&, a_zpx); case 0x2D: LOGIC(&, a_abs);
        case 0x3D: LOGIC(&, a_abx); case 0x39: LOGIC(&, a_aby); case 0x21: LOGIC(&, a_izx); case 0x31: LOGIC(&, a_izy);
        case 0x09: LOGIC(|, a_imm); case 0x05: LOGIC(|, a_zp); case 0x15: LOGIC(|, a_zpx); case 0x0D: LOGIC(|, a_abs);
        case 0x1D: LOGIC(|, a_abx); case 0x19: LOGIC(|, a_aby); case 0x01: LOGIC(|, a_izx); case 0x11: LOGIC(|, a_izy);
        case 0x49: LOGIC(^, a_imm); case 0x45: LOGIC(^, a_zp); case 0x55: LOGIC(^, a_zpx); case 0x4D: LOGIC(^, a_abs);
        case 0x5D: LOGIC(^, a_abx); case 0x59: LOGIC(^, a_aby); case 0x41: LOGIC(^, a_izx); case 0x51: LOGIC(^, a_izy);
        case 0x0A: A = asl(A); break; case 0x06: RMW(asl, a_zp); case 0x16: RMW(asl, a_zpx); case 0x0E: RMW(asl, a_abs); case 0x1E: RMW(asl, s_abx);
        case 0x4A: A = lsr(A); break; case 0x46: RMW(lsr, a_zp); case 0x56: RMW(lsr, a_zpx); case 0x4E: RMW(lsr, a_abs); case 0x5E: RMW(lsr, s_abx);
        case 0x2A: A = rol(A); break; case 0x26: RMW(rol, a_zp); case 0x36: RMW(rol, a_zpx); case 0x2E: RMW(rol, a_abs); case 0x3E: RMW(rol, s_abx);
        case 0x6A: A = ror(A); break; case 0x66: RMW(ror, a_zp); case 0x76: RMW(ror, a_zpx); case 0x6E: RMW(ror, a_abs); case 0x7E: RMW(ror, s_abx);
        case 0x10: branch(!(P & FN)); break; case 0x30: branch(P & FN); break;
        case 0x50: branch(!(P & FV)); break; case 0x70: branch(P & FV); break;
        case 0x90: branch(!(P & FC)); break; case 0xB0: branch(P & FC); break;
        case 0xD0: branch(!(P & FZ)); break; case 0xF0: branch(P & FZ); break;
        case 0x24: case 0x2C: {
            uint8_t m = rd(op == 0x24 ? a_zp() : a_abs());
            P = (uint8_t)((P & ~(FN | FV | FZ)) | (m & (FN | FV)) | ((m & A) ? 0 : FZ));
            break;
        }
        case 0x00: PC++; interrupt(0xFFFE, 1); cycles -= 7; break;
        case 0x18: P &= ~FC; break; case 0x38: P |= FC; break;
        case 0x58: P &= ~FI; break; case 0x78: P |= FI; break;
        case 0xD8: P &= ~FD; break; case 0xF8: P |= FD; break;
        case 0xB8: P &= ~FV; break;
        case 0xC9: cmp(A, rd(a_imm())); break; case 0xC5: cmp(A, rd(a_zp())); break; case 0xD5: cmp(A, rd(a_zpx())); break;
        case 0xCD: cmp(A, rd(a_abs())); break; case 0xDD: cmp(A, rd(a_abx())); break; case 0xD9: cmp(A, rd(a_aby())); break;
        case 0xC1: cmp(A, rd(a_izx())); break; case 0xD1: cmp(A, rd(a_izy())); break;
        case 0xE0: cmp(X, rd(a_imm())); break; case 0xE4: cmp(X, rd(a_zp())); break; case 0xEC: cmp(X, rd(a_abs())); break;
        case 0xC0: cmp(Y, rd(a_imm())); break; case 0xC4: cmp(Y, rd(a_zp())); break; case 0xCC: cmp(Y, rd(a_abs())); break;
        case 0xC6: { uint16_t e = a_zp(); wr(e, nz(rd(e) - 1)); } break;
        case 0xD6: { uint16_t e = a_zpx(); wr(e, nz(rd(e) - 1)); } break;
        case 0xCE: { uint16_t e = a_abs(); wr(e, nz(rd(e) - 1)); } break;
        case 0xDE: { uint16_t e = s_abx(); wr(e, nz(rd(e) - 1)); } break;
        case 0xE6: { uint16_t e = a_zp(); wr(e, nz(rd(e) + 1)); } break;
        case 0xF6: { uint16_t e = a_zpx(); wr(e, nz(rd(e) + 1)); } break;
        case 0xEE: { uint16_t e = a_abs(); wr(e, nz(rd(e) + 1)); } break;
        case 0xFE: { uint16_t e = s_abx(); wr(e, nz(rd(e) + 1)); } break;
        case 0xCA: X = nz(X - 1); break; case 0x88: Y = nz(Y - 1); break;
        case 0xE8: X = nz(X + 1); break; case 0xC8: Y = nz(Y + 1); break;
        case 0x4C: PC = a_abs(); break;
        case 0x6C: { uint16_t w = a_abs(); PC = (uint16_t)(rd(w) | rd((uint16_t)((w & 0xFF00) | ((w + 1) & 0xFF))) << 8); } break;
        case 0x20: { uint16_t e = a_abs(); uint16_t r = PC - 1; push(r >> 8); push(r & 0xFF); PC = e; } break;
        case 0xA9: A = nz(rd(a_imm())); break; case 0xA5: A = nz(rd(a_zp())); break; case 0xB5: A = nz(rd(a_zpx())); break;
        case 0xAD: A = nz(rd(a_abs())); break; case 0xBD: A = nz(rd(a_abx())); break; case 0xB9: A = nz(rd(a_aby())); break;
        case 0xA1: A = nz(rd(a_izx())); break; case 0xB1: A = nz(rd(a_izy())); break;
        case 0xA2: X = nz(rd(a_imm())); break; case 0xA6: X = nz(rd(a_zp())); break; case 0xB6: X = nz(rd(a_zpy())); break;
        case 0xAE: X = nz(rd(a_abs())); break; case 0xBE: X = nz(rd(a_aby())); break;
        case 0xA0: Y = nz(rd(a_imm())); break; case 0xA4: Y = nz(rd(a_zp())); break; case 0xB4: Y = nz(rd(a_zpx())); break;
        case 0xAC: Y = nz(rd(a_abs())); break; case 0xBC: Y = nz(rd(a_abx())); break;
        case 0xEA: break;
        case 0x48: push(A); break; case 0x08: push(P | FB | FU); break;
        case 0x68: A = nz(pull()); break; case 0x28: P = (uint8_t)((pull() & ~FB) | FU); break;
        case 0x40: P = (uint8_t)((pull() & ~FB) | FU); PC = pull(); PC |= (uint16_t)(pull() << 8); break;
        case 0x60: PC = pull(); PC |= (uint16_t)(pull() << 8); PC++; break;
        case 0x85: wr(a_zp(), A); break; case 0x95: wr(a_zpx(), A); break; case 0x8D: wr(a_abs(), A); break;
        case 0x9D: wr(s_abx(), A); break; case 0x99: wr(s_aby(), A); break; case 0x81: wr(a_izx(), A); break;
        case 0x91: wr(s_izy(), A); break;
        case 0x86: wr(a_zp(), X); break; case 0x96: wr(a_zpy(), X); break; case 0x8E: wr(a_abs(), X); break;
        case 0x84: wr(a_zp(), Y); break; case 0x94: wr(a_zpx(), Y); break; case 0x8C: wr(a_abs(), Y); break;
        case 0xAA: X = nz(A); break; case 0xA8: Y = nz(A); break; case 0xBA: X = nz(S); break;
        case 0x8A: A = nz(X); break; case 0x9A: S = X; break; case 0x98: A = nz(Y); break;
        default: PC--; return -1;  // an instruction games don't use: stop
    }
    return (int)(cycles - before);
}

int nes_step(void) { return step(); }

void nes_regs(struct nes_regs *r) { r->pc = PC; r->a = A; r->x = X; r->y = Y; r->p = P; r->s = S; }
void nes_set_pc(uint16_t pc) {
    PC = pc;
}

int nes_load(const uint8_t *rom, unsigned len) {
    if (len < 16 || rom[0] != 'N' || rom[1] != 'E' || rom[2] != 'S' || rom[3] != 0x1A) return -1;
    mapper = rom[6] >> 4 | (rom[7] & 0xF0);
    if (mapper != 0 && mapper != 1 && mapper != 2 && mapper != 3 && mapper != 4 && mapper != 7 && mapper != 9) return -2;
    // (a fresh machine: loading another game must not see the last one's memories or registers)
    for (int i = 0; i < 0x800; i++) ram[i] = vram[i] = 0;
    for (int i = 0; i < 0x2000; i++) chrram[i] = wram[i] = 0;
    for (int i = 0; i < 256; i++) oam[i] = 0;
    for (int i = 0; i < 32; i++) pal[i] = 0;
    ctrl = mask = status = oamaddr = readbuf = w_toggle = finex = 0;
    v = t = 0;
    pad_shift = pad_strobe = 0;
    nmi_line = 0;
    a12_last = 0; a12_low_since = 0;
    nprg = rom[4] * 16384u; nchr = rom[5] * 8192u;
    unsigned skip = rom[6] & 4 ? 512 : 0;  // (a trainer before the program)
    if (len < 16 + skip + nprg + nchr) return -3;
    prg = rom + 16 + skip;
    chr_is_ram = nchr == 0;
    if (chr_is_ram) { chrmem = chrram; nchr = 0x2000; }
    else chrmem = (uint8_t *)(rom + 16 + skip + nprg);
    mirroring = rom[6] & 1;
    for (int i = 0; i < 16; i++) mreg[i] = 0;
    mreg[0] = mapper == 1 ? 0x0C : 0;
    if (mapper == 4) { mreg[6] = 0; mreg[7] = 1; }
    irq_line = irq_on = irq_reload = irq_count = irq_latch = 0;
    for (int k = 0; k < 4; k++) snd[k] = (struct voice){0};
    snd_on = 0;
    map_update();
    A = X = Y = 0; S = 0xFD; P = FU | FI;
    PC = rd16(0xFFFC);
    line = 0; cycles = 0;
    return 0;
}

void nes_pad(uint8_t buttons) { pad_state = buttons; }

// one whole frame: 262 scanlines of 341 picture dots = 113.67 CPU cycles each. Inside a line: the picture is drawn
// at dot 256 (with the scroll as it stands then), the sprite fetches start at 257 (A12 = the sprites' table), the
// next line's first tiles at 321 (A12 = the background's table).
static int run_to(long target) {
    while (cycles < target)
        if (step() < 0) return -1;
    return 0;
}

int nes_frame_run(void) {
    for (line = 0; line < 262; line++) {
        const long start = cycles;
        const long len = line % 3 == 2 ? 113 : 114;  // (113.67 on average)
        if (line == 241) {  // (the NMI comes a few cycles after the flag: a game polling $2002 can see it first)
            status |= 0x80;
            if (run_to(start + 4) < 0) return -1;
            if (ctrl & 0x80) nmi_line = 1;
        }
        if (line == 261) status &= 0x1F;  // pre-render line: vblank, sprite 0, overflow cleared
        if (line % 65 == 0 && line) { apu_quarter(); if (line % 130 == 0) apu_half(); }
        if (run_to(start + 85) < 0) return -1;
        const int rendering = mask & 0x18, drawn = line < 240 || line == 261;
        if (line < 240) {
            if (line == 0) find_sprites();
            render_line(line);
        }
        if (rendering && drawn) {
            if (line < 240) {  // down one row
                if ((v & 0x7000) != 0x7000) v += 0x1000;
                else {
                    v &= ~0x7000;
                    int cy = (v & 0x03E0) >> 5;
                    if (cy == 29) { cy = 0; v ^= 0x0800; }
                    else if (cy == 31) cy = 0;
                    else cy++;
                    v = (v & ~0x03E0) | cy << 5;
                }
            }
            v = (v & ~0x041F) | (t & 0x041F);  // dot 257: the horizontal position back from t
            if (line == 261) v = t;            // (dots 280-304: the frame starts again from the scroll the game set)
            if (mapper == 4) a12(ctrl & 0x20 ? 1 : (ctrl >> 3) & 1);
        }
        if (run_to(start + 107) < 0) return -1;
        if (rendering && drawn && mapper == 4) a12((ctrl >> 4) & 1);
        if (run_to(start + len) < 0) return -1;
    }
    return 0;
}

const uint8_t *nes_vram(void) { return vram; }
const uint8_t *nes_wram(void) { return wram; }
long nes_cycles(void) { return cycles; }
