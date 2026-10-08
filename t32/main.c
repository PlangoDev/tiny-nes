// TINY-NES on TINY-32: the pad from the board's gamepad (KEYS' low byte), pictures straight into the 256x240
// palette screen, held to 60 frames a second by TICKS, sound through the sound chip.
// With GAMES (library.py's games.h): a boot screen, then a carousel of the games to pick from; holding SELECT +
// START for a second and a half in a game comes back to it. Without: the one cartridge cart.S links in.
#include <stdint.h>
#include "../nes.h"
#define PORT(at) (*(volatile uint32_t *)(at))
#define TICKS PORT(0xF0000004u)
#define KEYS PORT(0xF0000008u)
#define FRAME PORT(0xF000000Cu)
#define PALETTE ((volatile uint32_t *)0xF0001000u)
#define SOUND PORT(0xF0000000u)  // (TINY-NES: the sound chip, where TINY-32 has its console: register << 8 | data)
#define SCREEN ((volatile uint32_t *)NES_SCREEN)

static const uint32_t NES_RGB[64] = {
    0x545454,0x001E74,0x081090,0x300088,0x440064,0x5C0030,0x540400,0x3C1800,0x202A00,0x083A00,0x004000,0x003C00,0x00323C,0,0,0,
    0x989698,0x084CC4,0x3032EC,0x5C1EE4,0x8814B0,0xA01464,0x982220,0x783C00,0x545A00,0x287200,0x087C00,0x007628,0x006678,0,0,0,
    0xECEEEC,0x4C9AEC,0x787CEC,0xB062EC,0xE454EC,0xEC58B4,0xEC6A64,0xD48820,0xA0AA00,0x74C400,0x4CD020,0x38CC6C,0x38B4CC,0x3C3C3C,0,0,
    0xECEEEC,0xA8CCEC,0xBCBCEC,0xD4B2EC,0xECAEEC,0xECAED4,0xECB4B0,0xE4C490,0xCCD278,0xB4DE78,0xA8E290,0x98E2B4,0xA0D6E4,0xA0A2A0,0,0};
enum { PAD_A = 1, PAD_B = 2, PAD_SELECT = 4, PAD_START = 8, PAD_UP = 16, PAD_DOWN = 32, PAD_LEFT = 64, PAD_RIGHT = 128 };

// ---- time and sound: one frame every 50/3 ms; the sound chip takes one changed register a millisecond
static uint8_t snd_want[16], snd_was[16];
static uint32_t t_start, t_wrote, t_frame;

static void frame_reset(void) {
    t_start = TICKS; t_wrote = TICKS - 1; t_frame = 0;
    for (int i = 0; i < 16; i++) snd_was[i] = 0xFF;
}

static void frame_end(void) {
    FRAME = ++t_frame;
    if (TICKS - t_start > (t_frame + 2) * 50 / 3) t_start = TICKS - t_frame * 50 / 3;  // (more than two behind: let it go)
    do {
        if (TICKS == t_wrote) continue;
        for (int i = 0; i < 16; i++)
            if (snd_want[i] != snd_was[i]) { SOUND = (uint32_t)(i << 8 | snd_want[i]); snd_was[i] = snd_want[i]; t_wrote = TICKS; break; }
    } while (TICKS - t_start < t_frame * 50 / 3);
}

static void run_game(const uint8_t *rom, unsigned len, int menu) {
    if (nes_load(rom, len)) return;
    frame_reset();
    for (int held = 0;;) {
        const uint8_t pad = (uint8_t)KEYS;
        nes_pad(pad);
        if (nes_frame_run() < 0) return;
        nes_sound(snd_want);
        if (menu) {
            held = (pad & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START) ? held + 1 : 0;
            if (held >= 90) break;
        }
        frame_end();
    }
    for (int i = 0; i < 16; i++) snd_want[i] = 0;
    frame_end();
}

#ifndef GAMES
extern const uint8_t cart[], cart_end[];
int main(void) {
    for (int i = 0; i < 64; i++) PALETTE[i] = NES_RGB[i];
    run_game(cart, (unsigned)(cart_end - cart), 0);
    return 2;
}
#else
#include "games.h"

// ---- drawing: a 256x240 picture in RAM (NES colours 0-63), copied to the screen a word at a time each frame
static uint8_t fb[256 * 240] __attribute__((aligned(4)));

static void show(void) {
    const uint32_t *s = (const uint32_t *)fb;
    for (int i = 0; i < 256 * 240 / 4; i++) SCREEN[i] = s[i];
}

static inline void dot(int x, int y, uint8_t c) { if ((unsigned)x < 256 && (unsigned)y < 240) fb[y * 256 + x] = c; }

static void fill(int x, int y, int w, int h, uint8_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < 240; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < 256; i++) fb[j * 256 + i] = c;
}

static uint8_t darker(uint8_t c, int rows) {   // (the NES palette's rows: each 0x10 down is a step darker)
    while (rows-- > 0) c = c >= 0x10 ? c - 0x10 : 0x0F;
    return c;
}

// 5x7 letters, a row a byte (bit 4 = leftmost)
static const uint8_t *glyph(char ch) {
    static const struct { char c; uint8_t r[7]; } F[] = {
        {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
        {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
        {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
        {'J', {7, 2, 2, 2, 2, 18, 12}}, {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
        {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
        {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
        {'S', {15, 16, 16, 14, 1, 1, 30}}, {'T', {31, 4, 4, 4, 4, 4, 4}}, {'U', {17, 17, 17, 17, 17, 17, 14}},
        {'V', {17, 17, 17, 17, 17, 10, 4}}, {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
        {'Y', {17, 17, 10, 4, 4, 4, 4}}, {'Z', {31, 1, 2, 4, 8, 16, 31}}, {'0', {14, 17, 19, 21, 25, 17, 14}},
        {'1', {4, 12, 4, 4, 4, 4, 14}}, {'2', {14, 17, 1, 2, 4, 8, 31}}, {'3', {31, 2, 4, 2, 1, 17, 14}},
        {'4', {2, 6, 10, 18, 31, 2, 2}}, {'5', {31, 16, 30, 1, 1, 17, 14}}, {'6', {6, 8, 16, 30, 17, 17, 14}},
        {'7', {31, 1, 2, 4, 8, 8, 8}}, {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
        {'-', {0, 0, 0, 31, 0, 0, 0}}, {'.', {0, 0, 0, 0, 0, 12, 12}}, {'!', {4, 4, 4, 4, 4, 0, 4}},
        {'+', {0, 4, 4, 31, 4, 4, 0}}, {'<', {2, 4, 8, 16, 8, 4, 2}}, {'>', {8, 4, 2, 1, 2, 4, 8}},
        {':', {0, 12, 12, 0, 12, 12, 0}}, {'/', {1, 1, 2, 4, 8, 16, 16}}, {'\'', {4, 4, 8, 0, 0, 0, 0}}, {',', {0, 0, 0, 0, 12, 4, 8}}};
    static const uint8_t SPACE[7];
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
        if (F[i].c == ch) return F[i].r;
    return SPACE;
}

static int text_w(const char *s, int k) { int n = 0; while (s[n]) n++; return n ? n * 6 * k - k : 0; }

// text at size k; `ramp` (if set) colours each row of the letters from a list (a gradient), else c
static void text_ex(const char *s, int x, int y, int k, uint8_t c, const uint8_t *ramp) {
    for (; *s; s++, x += 6 * k) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] >> (4 - b) & 1) fill(x + b * k, y + r * k, k, k, ramp ? ramp[r] : c);
    }
}
static void text(const char *s, int x, int y, int k, uint8_t c) { text_ex(s, x, y, k, c, 0); }
static void text_c(const char *s, int y, int k, uint8_t c) { text(s, 128 - text_w(s, k) / 2, y, k, c); }
static void text_shadow_c(const char *s, int y, int k, uint8_t c) {   // (a dark copy one step down-right first)
    const int x = 128 - text_w(s, k) / 2, d = k > 1 ? k / 2 + 1 : 1;
    text(s, x + d, y + d, k, 0x0F);
    text(s, x, y, k, c);
}

static void tone(int voice, int note, int vol) { snd_want[voice * 4] = (uint8_t)note; snd_want[voice * 4 + 1] = (uint8_t)vol; snd_want[voice * 4 + 2] = vol > 0; }
static void quiet(void) { for (int i = 0; i < 16; i++) snd_want[i] = 0; }

static uint32_t rng = 0x1234567;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

// ---- the scene behind everything: a night sky in bands, stars, a sun sinking into a neon grid that rolls toward you
#define HORIZON 150
static struct { int x, y, speed; } stars[40];
static void stars_init(void) { for (int i = 0; i < 40; i++) stars[i].x = (int)(rnd() % 256) << 4, stars[i].y = (int)(rnd() % (HORIZON - 40)), stars[i].speed = 2 + (int)(rnd() % 14); }

static void backdrop(int f, int bright) {
    static const uint8_t SKY[] = {0x0F, 0x0F, 0x0F, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x13, 0x14, 0x15};
    for (int y = 0; y < HORIZON; y++) {
        uint8_t c = SKY[y * 13 / HORIZON];
        if ((y & 1) && y > HORIZON - 60 && ((y >> 1) + f / 2) % 7 == 0) c = darker(c, 1);  // (a slow shimmer near the edge)
        uint8_t *row = fb + y * 256;
        for (int x = 0; x < 256; x++) row[x] = darker(c, bright);
    }
    for (int i = 0; i < 40; i++) {
        stars[i].x -= stars[i].speed;
        if (stars[i].x < 0) stars[i].x += 256 << 4, stars[i].y = (int)(rnd() % (HORIZON - 40));
        const int tw = ((f + i * 7) % 40) < 3;
        dot(stars[i].x >> 4, stars[i].y, darker(tw ? 0x30 : stars[i].speed > 10 ? 0x20 : 0x10, bright));
    }
    // the sun: a disc cut by bands that thin toward its top
    for (int y = -34; y < 0; y++) {
        const int band = (-y) / 5;
        if ((y & 3) == 0 && -y < 22) continue;
        if (band < 2 && (y & 1)) continue;
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= 34 * 34) w++;
        const uint8_t c = darker(-y > 24 ? 0x38 : -y > 14 ? 0x27 : 0x26, bright);
        fill(128 - w, HORIZON + y, 2 * w + 1, 1, c);
    }
    // the floor: lines toward the middle of the horizon, cross lines coming closer
    for (int y = HORIZON; y < 240; y++) fill(0, y, 256, 1, darker(0x0F, 0));
    fill(0, HORIZON, 256, 1, darker(0x25, bright));
    for (int k = 0; k < 10; k++) {   // (rows ever farther apart down the screen: the floor's depth)
        const int z = (k * 16 + (f % 16)) ;   // 0..160
        const int y = HORIZON + 1 + z * z / 290;
        if (y < 240) fill(0, y, 256, 1, darker(z > 80 ? 0x24 : 0x14, bright));
    }
    for (int i = -12; i <= 12; i++)
        for (int y = HORIZON + 1; y < 240; y++) {
            const int x = 128 + i * 6 * (y - HORIZON) / 8;
            dot(x, y, darker(y > 200 ? 0x24 : 0x14, bright));
        }
}

// ---- an icon (128x120) drawn w x h at (x, y), the nearest dot of it for each; `dim` rows darker
static void icon(const uint8_t *ic, int x, int y, int w, int h, int dim) {
    for (int j = 0; j < h; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= 240) continue;
        const uint8_t *row = ic + (j * 120 / h) * 128;
        for (int i = 0; i < w; i++) {
            const int sx = x + i;
            if (sx < 0 || sx >= 256) continue;
            fb[sy * 256 + sx] = darker(row[i * 128 / w] & 63, dim);
        }
    }
}

// ...its reflection on the floor below it: upside down, dark, fading out
static void reflection(const uint8_t *ic, int x, int y, int w, int h, int rows) {
    for (int j = 0; j < rows; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= 240) continue;
        const uint8_t *row = ic + ((h - 1 - j) * 120 / h) * 128;
        const int fade = 2 + j * 2 / rows;
        for (int i = 0; i < w; i++) {
            const int sx = x + i;
            if ((unsigned)sx >= 256 || ((i + j) & 1 && j > rows / 3)) continue;
            const uint8_t c = darker(row[i * 128 / w] & 63, fade);
            if (c != 0x0F) fb[sy * 256 + sx] = c;
        }
    }
}

static void frame_box(int x, int y, int w, int h, uint8_t c) {
    fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c); fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c);
}

static const uint8_t RAINBOW[] = {0x16, 0x27, 0x28, 0x2A, 0x2C, 0x21, 0x24, 0x25};
static const uint8_t LOGO_RAMP[7] = {0x30, 0x3C, 0x2C, 0x2C, 0x1C, 0x1C, 0x0C};   // (white to deep cyan, top to bottom)
static const uint8_t PINK_RAMP[7] = {0x30, 0x35, 0x25, 0x25, 0x15, 0x15, 0x05};

// ---- screen shake: the picture moved down by a few rows (the top filled with black)
static void shake(int dy) {
    if (dy <= 0) return;
    for (int y = 239; y >= dy; y--)
        for (int x = 0; x < 256; x++) fb[y * 256 + x] = fb[(y - dy) * 256 + x];
    fill(0, 0, 256, dy, 0x0F);
}

// ---- the badge that stays on screen once Tiny Logic has been named: whoever sees any part of it knows where it was built
static void badge(int y, int f) {
    const char *s = "BUILT IN TINY LOGIC";
    const int w = text_w(s, 1) + 10, x = 128 - w / 2;
    fill(x, y, w, 11, 0x0F);
    frame_box(x, y, w, 11, 0x13);
    fill(x + 3, y + 4, 3, 3, ((f / 16) & 1) ? 0x2A : 0x1A);   // (a little light that blinks, like a powered part)
    text(s, x + 8, y + 2, 1, 0x20);
}

// the Tiny Logic word as dots: each lit cell of "TINY LOGIC" at size 3, flying in from somewhere random
#define TL_K 3
static struct { int16_t tx, ty, sx, sy; uint8_t c, delay; } bits_[400];
static int nbits;
static void bits_init(int x0, int y0) {
    static const char *W = "TINY LOGIC";
    static const uint8_t RAMP[7] = {0x30, 0x3A, 0x2A, 0x2A, 0x1A, 0x1A, 0x0A};   // (white to green, like a lit wire)
    nbits = 0;
    for (int i = 0; W[i]; i++) {
        const uint8_t *g = glyph(W[i]);
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if ((g[r] >> (4 - b) & 1) && nbits < 400) {
                    bits_[nbits].tx = (int16_t)(x0 + i * 6 * TL_K + b * TL_K);
                    bits_[nbits].ty = (int16_t)(y0 + r * TL_K);
                    const uint32_t a = rnd() % 4;   // (from one of the four edges)
                    bits_[nbits].sx = (int16_t)(a == 0 ? -10 : a == 1 ? 266 : (int)(rnd() % 256));
                    bits_[nbits].sy = (int16_t)(a == 2 ? -10 : a == 3 ? 250 : (int)(rnd() % 240));
                    bits_[nbits].c = RAMP[r];
                    bits_[nbits].delay = (uint8_t)(i * 2 + rnd() % 8);
                    nbits++;
                }
    }
}
static void bits_draw(int t, int span) {   // t frames in; each dot takes `span` frames to land, easing out
    for (int i = 0; i < nbits; i++) {
        int u = t - bits_[i].delay;
        if (u <= 0) continue;
        if (u > span) u = span;
        const int e = 256 - (span - u) * (span - u) * 256 / (span * span);   // (fast start, soft landing)
        const int x = bits_[i].sx + (bits_[i].tx - bits_[i].sx) * e / 256, y = bits_[i].sy + (bits_[i].ty - bits_[i].sy) * e / 256;
        fill(x, y, TL_K, TL_K, u < span ? 0x30 : bits_[i].c);
        if (u < span - 2) dot(x - (bits_[i].tx - bits_[i].sx) / 24, y - (bits_[i].ty - bits_[i].sy) / 24, 0x10);   // (a trail)
    }
}

// a NAND gate drawn as its outline is traced: back, top, the round front, the bubble, the wires in and out
static void nand_draw(int cx, int cy, int t, uint8_t c) {
    int n = 0;
#define P(x, y) do { if (n++ < t) fill(cx + (x), cy + (y), 2, 2, c); } while (0)
    for (int y = -14; y <= 14; y += 2) P(-16, y);                         // back
    for (int x = -16; x <= 0; x += 2) P(x, -14);                          // top
    for (int a = 0; a <= 16; a++) {                                       // front: a half circle, top to bottom
        static const int8_t CX[17] = {0, 3, 5, 8, 10, 12, 13, 14, 14, 14, 13, 12, 10, 8, 5, 3, 0};
        P(CX[a], -14 + a * 28 / 16);
    }
    for (int x = 0; x >= -16; x -= 2) P(x, 14);                           // bottom
    static const int8_t BX[8] = {17, 18, 20, 21, 20, 18, 17, 17}, BY[8] = {-2, -3, -3, 0, 3, 3, 2, 0};
    for (int k = 0; k < 8; k++) P(BX[k], BY[k]);                          // the bubble that makes it NOT AND
    for (int x = -30; x < -16; x += 2) P(x, -7);                          // inputs
    for (int x = -30; x < -16; x += 2) P(x, 7);
#undef P
}

// ---- the boot (about 7 s; A, B or START skips it). 1: the tube comes on. 2: a NAND gate traces itself, and TINY LOGIC
// assembles from flying bits and lands with a thump: "built inside Tiny Logic, the logic simulator". 3: a dive into
// the chip (squares rushing past). 4: TINY-NES drops in letter by letter, a light sweeps it, the tagline types out
// and a transistor count races up. 5: the sky rises in behind and it slides up into the menu. From act 2 on the
// "BUILT IN TINY LOGIC" badge stays at the bottom.
static void boot(void) {
    static const char *NAME = "TINY-NES";
    static const uint8_t NOTES[] = {60, 64, 67, 72, 76, 79, 84, 88};
    static const char *TAG1 = "AN NES RUNNING ON A CPU", *TAG2 = "BUILT FROM LOGIC GATES";
    const int LX = 128 - text_w(NAME, 4) / 2, LY = 62;
    const int TLX = 128 - text_w("TINY LOGIC", TL_K) / 2, TLY = 98;
    bits_init(TLX, TLY);
    frame_reset();
    for (int f = 0; f < 470; f++) {
        if (f > 30 && ((uint8_t)KEYS & (PAD_A | PAD_START | PAD_B))) break;
        fill(0, 0, 256, 240, 0x0F);
        int sh = 0;
        if (f < 30) {   // 1: the tube coming on
            if (f < 8) fill(127 - f, 119, 2 + 2 * f, 2, 0x30);
            else if (f < 18) fill(0, 120 - (f - 8), 256, 2 * (f - 8) + 1, f & 1 ? 0x30 : 0x20);
            else if (f < 22) fill(0, 0, 256, 240, f < 20 ? 0x20 : 0x10);
            tone(3, f < 18 ? 1 + (f & 7) : 0, f < 18 ? 10 - f / 2 : 0);
        } else if (f < 160) {   // 2: built inside Tiny Logic
            const int t = f - 30;
            for (int i = 0; i < 40; i++) dot(stars[i].x >> 4, stars[i].y, ((t + i * 11) % 50) < 3 ? 0x30 : 0x00);
            text_c("BUILT INSIDE", 46, 1, t > 4 ? 0x10 : 0x00);
            nand_draw(128, 74, t * 3, t > 30 ? ((t / 4) & 1 ? 0x2A : 0x3A) : 0x20);
            if (t > 26) {   // its output wire runs out both ways and becomes the underline
                const int w = (t - 26) * 10 > 190 ? 190 : (t - 26) * 10;
                fill(128 + 22, 74, w / 2 > 70 ? 70 : w / 2, 2, 0x2A);
                fill(128 - w / 2, TLY + 26, w, 2, 0x1A);
            }
            if (t < 18) tone(2, 0, 0);
            if (t == 2) tone(0, 48, 8);
            bits_draw(t - 14, 26);
            if (t >= 14 && t < 40) tone(3, 15 - (t - 14) / 2, 6);   // (the bits rushing in)
            if (t == 46) { tone(3, 0, 0); tone(0, 72, 15); tone(1, 67, 12); tone(2, 36, 15); }   // (they land)
            if (t >= 46 && t < 52) sh = (52 - t) / 2;
            if (t == 60) { tone(1, 0, 0); tone(2, 0, 0); }
            if (t > 52) {
                const char *s = "THE LOGIC SIMULATOR";
                char buf[24]; int i = 0;
                for (; s[i] && i < (t - 52) / 2; i++) buf[i] = s[i];
                buf[i] = 0;
                text(buf, 128 - text_w(s, 1) / 2, TLY + 34, 1, 0x30);
                if (s[i] && (t & 1)) tone(0, 96, 3); else if (t > 52 && t < 95) tone(0, 0, 0);
            }
            if (t > 92) text_c("BY MAZELL SMACK-DUBOSE", TLY + 48, 1, t > 96 ? 0x00 : 0x0F);
            if (t > 110) {   // 2 -> 3: everything darkens toward the middle
                for (int i = 0; i < 256 * 240; i++) fb[i] = darker(fb[i], (t - 110) / 5);
            }
        } else if (f < 200) {   // 3: diving into the chip: squares rush out from the middle, faster and faster
            const int t = f - 160;
            for (int k = 0; k < 8; k++) {
                const int z = ((t * (4 + t / 8) + k * 32) % 256);
                const int r = z * z / 256 + 2;
                frame_box(128 - r, 120 - r * 15 / 16, 2 * r, 2 * r * 15 / 16, z > 200 ? 0x30 : z > 120 ? 0x2C : 0x1C);
            }
            fill(124, 116, 8, 8, (t / 2) & 1 ? 0x30 : 0x2C);
            tone(2, 36 + t / 2, 12);
            if (t > 34) fill(0, 0, 256, 240, 0x30);   // (white as it breaks through)
            if (t == 39) tone(2, 0, 0);
        } else {   // 4: TINY-NES
            const int t = f - 200 + 30;   // (the old timeline: the letters start at t = 30)
            for (int i = 0; i < 40; i++) dot(stars[i].x >> 4, stars[i].y, ((t + i * 11) % 50) < 3 ? 0x30 : 0x00);
            if (t < 36) fill(0, 0, 256, 240, t < 33 ? 0x20 : 0x10);   // (the white fading)
            for (int k = 0; k < 8; k++) {   // traces into the logo
                const int len = (t - 30) * 9 - k * 10;
                if (len <= 0) continue;
                const int ty = LY + 4 + (k % 4) * 6, left = k < 4, endx = left ? LX - 6 : LX + text_w(NAME, 4) + 6;
                const int sx = left ? 0 : 255, dist = left ? endx : 255 - endx;
                const int head = len < dist ? len : dist;
                for (int d = 0; d < head; d++) dot(left ? sx + d : sx - d, ty, head - d < 6 ? 0x30 : head - d < 30 ? 0x2C : 0x1C);
                if (len >= dist) fill(endx - 1, ty - 1, 3, 3, ((t / 6 + k) & 1) ? 0x30 : 0x2C);
            }
            for (int i = 0; i < 8; i++) {
                const int at = t - 34 - i * 4;
                if (at < 0) continue;
                int y = LY;
                if (at < 10) y = LY - (10 - at) * (10 - at);
                else if (at < 16) y = LY - (at - 10) * (16 - at) / 2;
                const char s1[2] = {NAME[i], 0};
                const int x = LX + i * 24;
                text(s1, x + 3, y + 3, 4, 0x0F);
                text_ex(s1, x, y, 4, 0, i == 4 ? PINK_RAMP : LOGO_RAMP);
                if (at == 10) { tone(0, NOTES[i], 12); tone(1, NOTES[i] - 12, 8); }
                if (at == 14) tone(1, 0, 0);
                if (i == 7 && at >= 10 && at < 16) sh = (16 - at) / 2;
            }
            if (t > 80 && t < 120) {   // the light sweep
                const int sx = LX - 30 + (t - 80) * 7;
                for (int y = LY - 2; y < LY + 30; y++)
                    for (int x = sx + (y - LY) / 2; x < sx + (y - LY) / 2 + 6; x++)
                        if ((unsigned)x < 256 && fb[y * 256 + x] != 0x0F) fb[y * 256 + x] = 0x30;
            }
            if (t == 81) { tone(0, 84, 13); tone(1, 72, 10); tone(2, 48, 14); }
            if (t == 105) { tone(0, 0, 0); tone(1, 0, 0); }
            if (t > 88) {
                const int w = (t - 88) * 8 > 200 ? 200 : (t - 88) * 8;
                fill(128 - w / 2, LY + 36, w, 2, 0x25);
            }
            if (t > 100) {   // the tagline, typed out on two lines
                char s1[32];
                int n = (t - 100) * 2 / 3, i = 0;
                for (; TAG1[i] && i < n; i++) s1[i] = TAG1[i];
                s1[i] = 0;
                text(s1, 128 - text_w(TAG1, 1) / 2, LY + 46, 1, 0x30);
                int j2 = 0;
                char s2[32];
                for (; TAG2[j2] && j2 < n - 23; j2++) s2[j2] = TAG2[j2];
                s2[j2] = 0;
                text(s2, 128 - text_w(TAG2, 1) / 2, LY + 57, 1, 0x30);
                if ((TAG1[i] || TAG2[j2]) && (t & 1)) tone(3, 12, 3); else if (!TAG2[j2]) tone(3, 0, 0);
            }
            if (t > 175) {   // the transistor count racing up (TINY-32's CPU32, all the way down to the NAND)
                const int goal = 93338, steps = 30, u = t - 175 > steps ? steps : t - 175;
                const int v = goal - (goal * (steps - u) / steps) * (steps - u) / steps;
                char num[16], out[24];
                int k = 0, m = v;
                do { num[k++] = (char)('0' + m % 10); m /= 10; } while (m);
                int o = 0;
                for (int d = k - 1; d >= 0; d--) { out[o++] = num[d]; if (d && d % 3 == 0) out[o++] = ','; }
                out[o] = 0;
                text_shadow_c(out, LY + 76, 2, u >= steps ? 0x38 : 0x30);
                text_c("TRANSISTORS IN ITS CPU", LY + 94, 1, 0x10);
                if (u < steps && (t & 1)) tone(0, 60 + u, 6); else if (u == steps) tone(0, 0, 0);
            }
            if (t >= 250) {   // 5: the sky rises in from the bottom and the logo slides up out of the way
                const int k = t - 250, edge = 240 - k * 5;
                static uint8_t top[256 * 120];
                for (int i = 0; i < 256 * 120; i++) top[i] = fb[i + 256 * 20];
                backdrop(f, 0);
                for (int y = 0; y < edge && y < 240; y++)
                    for (int x = 0; x < 256; x++) fb[y * 256 + x] = 0x0F;
                const int up = k * 2 > 60 ? 60 : k * 2;
                for (int y = 0; y < 120; y++) {
                    const int dy = y + 20 - up;
                    if (dy < 0 || dy >= edge) continue;
                    for (int x = 0; x < 256; x++)
                        if (top[y * 256 + x] != 0x0F) fb[dy * 256 + x] = top[y * 256 + x];
                }
                if (k >= 48) break;
            }
        }
        if (f >= 150) badge(226, f);
        shake(sh);
        show();
        frame_end();
    }
    quiet();
}

// ---- the menu: the games stand on the grid like a carousel, the picked one at the front, big, lit, bobbing, its
// reflection on the floor; the others smaller and darker either side. Left / right turn it, A or START plays (the
// picked one zooms to fill the screen), and the name, maker and year sit underneath.
static int menu(int pick) {
    const int n = GAME_COUNT;
    int pos = pick * 256, held = 0, blip = 0, launch = -1, intro = 0;
    uint8_t last = 0xFF;
    frame_reset();
    for (int f = 0;; f++) {
        const uint8_t pad = (uint8_t)KEYS;
        const uint8_t pressed = (uint8_t)(pad & ~last);
        last = pad;
        if (launch < 0 && intro > 10) {
            int dir = 0;
            held = (pad & (PAD_LEFT | PAD_RIGHT)) ? held + 1 : 0;
            if ((pressed & PAD_LEFT) || ((pad & PAD_LEFT) && held > 18 && held % 7 == 0)) dir = -1;
            if ((pressed & PAD_RIGHT) || ((pad & PAD_RIGHT) && held > 18 && held % 7 == 0)) dir = 1;
            if (dir) { pick = (pick + dir + n) % n; blip = 3; tone(0, dir > 0 ? 88 : 83, 9); tone(3, 4, 4); }
            if (pressed & (PAD_A | PAD_START)) { launch = 0; tone(0, 72, 14); tone(1, 79, 12); tone(2, 48, 14); }
        }
        intro++;
        if (blip && !--blip) { tone(0, 0, 0); tone(3, 0, 0); }
        int d = pick * 256 - pos;   // (the ring turns the short way round, easing in)
        if (d > n * 128) d -= n * 256;
        if (d < -n * 128) d += n * 256;
        pos += d / 4 + (d > 0 ? 1 : d < 0 ? -1 : 0);
        if (pos < 0) pos += n * 256;
        if (pos >= n * 256) pos -= n * 256;

        backdrop(f, intro < 8 ? (8 - intro) / 3 : 0);
        // the top bar: logo, position
        fill(0, 0, 256, 22, 0x0F);
        fill(0, 22, 256, 1, 0x14);
        text_ex("TINY-NES", 8, 6, 2, 0, LOGO_RAMP);
        char pos_s[8] = {(char)('1' + pick), ' ', '/', ' ', (char)('0' + n), 0};
        text(pos_s, 248 - text_w(pos_s, 1), 9, 1, 0x10);
        for (int i = 0; i < n; i++) fill(214 - n * 8 + i * 8, 10, 5, 5, i == pick ? RAINBOW[(f / 4) % 8] : 0x00);
        // the games, farthest first so the front one ends up on top
        int order[16], dist[16];
        for (int i = 0; i < n; i++) {
            int dd = i * 256 - pos;
            while (dd > n * 128) dd -= n * 256;
            while (dd < -n * 128) dd += n * 256;
            order[i] = i, dist[i] = dd;
        }
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++) {
                const int da = dist[order[a]] < 0 ? -dist[order[a]] : dist[order[a]], db = dist[order[b]] < 0 ? -dist[order[b]] : dist[order[b]];
                if (db > da) { const int tmp = order[a]; order[a] = order[b]; order[b] = tmp; }
            }
        const int zoom = launch < 0 ? 0 : launch;   // (picked: the front one grows to fill the screen)
        for (int k = 0; k < n; k++) {
            const int i = order[k], dd = dist[i], ad = dd < 0 ? -dd : dd;
            if (ad > 3 * 256) continue;
            const int near = ad > 256 ? 256 + (ad - 256) / 3 : ad;
            int w = 112 - near * 56 / 256, h = 105 - near * 52 / 256;
            int cx = 128 + dd * 92 / 256, cy = 88 + near * 16 / 256;
            const int front = ad < 128;
            if (front) cy += (f / 10) % 2 ? 1 : 0;   // (a slow bob)
            if (front && zoom) {
                w += (256 - w) * zoom / 16; h += (240 - h) * zoom / 16;
                cy += (120 - cy) * zoom / 16; cx += (128 - cx) * zoom / 16;
            }
            if (!zoom || front) {
                if (front && !zoom) {
                    fill(cx - w / 2 + 5, cy - h / 2 + 5, w, h, 0x0F);   // (shadow)
                    const uint8_t glow = RAINBOW[(f / 3) % 8];
                    frame_box(cx - w / 2 - 3, cy - h / 2 - 3, w + 6, h + 6, glow);
                    frame_box(cx - w / 2 - 2, cy - h / 2 - 2, w + 4, h + 4, 0x30);
                    frame_box(cx - w / 2 - 1, cy - h / 2 - 1, w + 2, h + 2, 0x0F);
                }
                icon(GAME_LIST[i].icon, cx - w / 2, cy - h / 2, w, h, front ? 0 : (ad > 300 ? 2 : 1));
                if (!zoom) reflection(GAME_LIST[i].icon, cx - w / 2, cy + h / 2 + 5, w, h, front ? 18 : 10);
            }
        }
        if (launch < 0) {
            // the name plate
            const char *name = GAME_LIST[pick].name;
            const int k = text_w(name, 2) <= 236 ? 2 : 1;
            fill(0, 172, 256, 1, 0x14);
            fill(0, 173, 256, 44, 0x0F);
            text_shadow_c(name, 180, k, 0x30);
            text_c(GAME_LIST[pick].info, 198, 1, 0x00);
            const int ar = (f / 8) % 3;   // (arrows that nudge outward)
            text("<", 4 - ar + ((pad & PAD_LEFT) ? -2 : 0), 82, 2, (pad & PAD_LEFT) ? 0x30 : 0x2C);
            text(">", 242 + ar + ((pad & PAD_RIGHT) ? 2 : 0), 82, 2, (pad & PAD_RIGHT) ? 0x30 : 0x2C);
            fill(0, 217, 256, 23, 0x0F);
            fill(0, 217, 256, 1, 0x14);
            if ((f / 24) & 1 || intro < 30) text("START", 8, 224, 1, 0x28);
            text("PLAY", 41, 224, 1, 0x10);
            text("< >", 72, 224, 1, 0x28);
            text("PICK", 95, 224, 1, 0x10);
            const char *b = "BUILT IN TINY LOGIC";
            fill(246 - text_w(b, 1) - 8, 225, 3, 3, ((f / 16) & 1) ? 0x2A : 0x1A);
            text(b, 246 - text_w(b, 1), 224, 1, 0x20);
        } else {
            if (launch >= 16) {   // a white flash, then black, then the game
                fill(0, 0, 256, 240, launch < 19 ? 0x30 : launch < 21 ? 0x20 : 0x0F);
                if (launch > 26) { quiet(); frame_end(); return pick; }
            }
            if (launch == 8) { tone(0, 84, 14); tone(1, 91, 10); }
            launch++;
        }
        show();
        frame_end();
    }
}

int main(void) {
    for (int i = 0; i < 64; i++) PALETTE[i] = NES_RGB[i];
    stars_init();
    boot();
    for (int pick = 0;;) {
        pick = menu(pick);
        run_game(GAME_LIST[pick].rom, (unsigned)(GAME_LIST[pick].end - GAME_LIST[pick].rom), 1);
    }
}
#endif
