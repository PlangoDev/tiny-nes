// TINY-NES on a PC: `host nestest ROM LOG` checks every official instruction against nestest's log;
// `host frames ROM N DIR [pads]` runs N frames and writes each as a PPM (pads: one hex byte per frame, optional).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes.h"

static const uint8_t RGB[64][3] = {
    {84,84,84},{0,30,116},{8,16,144},{48,0,136},{68,0,100},{92,0,48},{84,4,0},{60,24,0},{32,42,0},{8,58,0},{0,64,0},{0,60,0},{0,50,60},{0,0,0},{0,0,0},{0,0,0},
    {152,150,152},{8,76,196},{48,50,236},{92,30,228},{136,20,176},{160,20,100},{152,34,32},{120,60,0},{84,90,0},{40,114,0},{8,124,0},{0,118,40},{0,102,120},{0,0,0},{0,0,0},{0,0,0},
    {236,238,236},{76,154,236},{120,124,236},{176,98,236},{228,84,236},{236,88,180},{236,106,100},{212,136,32},{160,170,0},{116,196,0},{76,208,32},{56,204,108},{56,180,204},{60,60,60},{0,0,0},{0,0,0},
    {236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},{204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{0,0,0},{0,0,0}};

static uint8_t *slurp(const char *path, unsigned *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = (unsigned)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len);
    if (fread(d, 1, *len, f) != *len) exit(1);
    fclose(f);
    return d;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "host nestest ROM LOG | host frames ROM N DIR [pads]\n"); return 2; }
    unsigned len;
    uint8_t *rom = slurp(argv[2], &len);
    int r = nes_load(rom, len);
    if (r) { fprintf(stderr, "can't load: %d\n", r); return 1; }
    if (!strcmp(argv[1], "nestest")) {
        FILE *f = fopen(argv[3], "r");
        char ln[256];
        int n = 0;
        nes_set_pc(0xC000);
        // (nestest starts with S = FD and P = 24, as after a reset)
        while (fgets(ln, sizeof ln, f)) {
            unsigned pc, a, x, y, p, s, op;
            sscanf(ln, "%4x %2x", &pc, &op);
            if (strchr(ln, '*')) break;
            long cyc = atol(strstr(ln, "CYC:") + 4);
            if (cyc != nes_cycles() + 7) { printf("line %d: cycle %ld, want %ld\n", n + 1, nes_cycles() + 7, cyc); return 1; }
            char *q = strstr(ln, "A:");
            sscanf(q, "A:%x X:%x Y:%x P:%x SP:%x", &a, &x, &y, &p, &s);
            struct nes_regs g;
            nes_regs(&g);
            if (n == 0) g.p = 0x24;
            if (g.pc != pc || g.a != a || g.x != x || g.y != y || (n && g.p != p) || g.s != s) {
                printf("line %d: want %04X A%02X X%02X Y%02X P%02X S%02X got %04X A%02X X%02X Y%02X P%02X S%02X\n",
                       n + 1, pc, a, x, y, p, s, g.pc, g.a, g.x, g.y, g.p, g.s);
                return 1;
            }
            if (nes_step() < 0) break;
            n++;
        }
        printf("%d instructions match nestest.log, up to the first unofficial one\n", n);
        return 0;
    }
    if (!strcmp(argv[1], "blargg")) {  // blargg's tests: $6001-3 = DE B0 61 once running, $6000 = 80 while running, then the result
        const uint8_t *w = nes_wram();
        for (int f = 0; f < 60 * 120; f++) {
            if (nes_frame_run() < 0) { struct nes_regs g; nes_regs(&g); printf("stopped at %04X\n", g.pc); return 1; }
            if (w[1] == 0xDE && w[2] == 0xB0 && w[3] == 0x61 && w[0] < 0x80) {
                printf("result %d after %d frames: %s\n", w[0], f, (const char *)w + 4);
                return w[0];
            }
        }
        printf("no result (status %02X)\n", w[0]);
        return 1;
    }
    if (!strcmp(argv[1], "sound")) {  // the sound chip's registers each frame: note/volume/on for the 4 voices
        int n = atoi(argv[3]);
        for (int f = 0; f < n; f++) {
            nes_pad(f >= n / 2 ? 1 : 0);  // (A held for the second half)
            nes_frame_run();
            uint8_t r[16];
            nes_sound(r);
            printf("%3d", f);
            for (int k = 0; k < 4; k++) printf("  %3d/%2d/%d", r[4 * k], r[4 * k + 1], r[4 * k + 2]);
            printf("\n");
        }
        return 0;
    }
    if (!strcmp(argv[1], "icon")) {  // the picture after N frames (palette numbers, 256x240 bytes): a game's icon
        for (int f = 0, n = atoi(argv[3]); f < n; f++) {
            nes_pad(argc > 5 && f >= atoi(argv[5]) && f < atoi(argv[5]) + 4 ? 8 : 0);  // (a Start press at that frame)
            if (nes_frame_run() < 0) return 1;
        }
        FILE *o = fopen(argv[4], "wb");
        if (!o) { perror(argv[4]); return 1; }
        fwrite(nes_frame, 1, NES_W * NES_H, o);
        fclose(o);
        return 0;
    }
    int frames = atoi(argv[3]);
    const char *pads = argc > 5 ? argv[5] : "";
    for (int i = 0; i < frames; i++) {
        unsigned b = 0;
        if (strlen(pads) >= (size_t)(i * 2 + 2)) sscanf(pads + i * 2, "%2x", &b);
        nes_pad((uint8_t)b);
        if (nes_frame_run() < 0) { struct nes_regs g; nes_regs(&g); fprintf(stderr, "stopped at %04X\n", g.pc); return 1; }
        char path[512];
        snprintf(path, sizeof path, "%s/f%04d.ppm", argv[4], i);
        FILE *o = fopen(path, "wb");
        if (!o) { perror(path); return 1; }
        fprintf(o, "P6\n%d %d\n255\n", NES_W, NES_H);
        for (int k = 0; k < NES_W * NES_H; k++) fwrite(RGB[nes_frame[k] & 63], 1, 3, o);
        fclose(o);
    }
    printf("%d frames, %ld cycles\n", frames, nes_cycles());
    return 0;
}
