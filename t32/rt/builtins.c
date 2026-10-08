// The compiler's helpers for 64-bit division on a 32-bit machine (RV32IM divides 32-bit numbers in one
// instruction; DOOM's FixedDiv divides a 64-bit number).
#include <stdint.h>

// A 64-bit number over a 32-bit one when the answer fits in 32 bits (hi < d): long division in two 16-bit digits,
// each guessed with the CPU's DIVU and put right (Hacker's Delight, divlu). ~30 instructions instead of ~400 for the
// bit-by-bit loop below; DOOM's FixedDiv is always this case.
static uint32_t divlu(uint32_t hi, uint32_t lo, uint32_t d, uint32_t* rem) {
    const int s = __builtin_clz(d);   // (d shifted up until its top bit is set: each digit's guess is then off by at most 2)
    d <<= s;
    const uint32_t un32 = s ? hi << s | lo >> (32 - s) : hi, un10 = lo << s;
    const uint32_t dn1 = d >> 16, dn0 = d & 0xffff, un1 = un10 >> 16, un0 = un10 & 0xffff;
    uint32_t q1 = un32 / dn1, rhat = un32 - q1 * dn1;
    while (q1 >> 16 || q1 * dn0 > (rhat << 16 | un1)) {
        q1--, rhat += dn1;
        if (rhat >> 16) break;
    }
    const uint32_t un21 = (un32 << 16 | un1) - q1 * d;
    uint32_t q0 = un21 / dn1;
    rhat = un21 - q0 * dn1;
    while (q0 >> 16 || q0 * dn0 > (rhat << 16 | un0)) {
        q0--, rhat += dn1;
        if (rhat >> 16) break;
    }
    if (rem) *rem = ((un21 << 16 | un0) - q0 * d) >> s;
    return q1 << 16 | q0;
}

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t* rem) {
    if (d == 0) { if (rem) *rem = n; return ~0ull; }
    if ((n >> 32) == 0 && (d >> 32) == 0) {
        if (rem) *rem = (uint32_t)n % (uint32_t)d;
        return (uint32_t)n / (uint32_t)d;
    }
    if ((d >> 32) == 0) {   // (a 32-bit divisor: the top word first, then the rest with what's left of it)
        const uint32_t hi = (uint32_t)(n >> 32), dv = (uint32_t)d, qhi = hi / dv;
        uint32_t r;
        const uint32_t qlo = divlu(hi - qhi * dv, (uint32_t)n, dv, &r);
        if (rem) *rem = r;
        return (uint64_t)qhi << 32 | qlo;
    }
    uint64_t q = 0, r = 0;
    int top = 63;
    while (top >= 0 && !((n >> top) & 1)) top--;
    for (int i = top; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) r -= d, q |= 1ull << i;
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return __udivmoddi4(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; __udivmoddi4(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d) {
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return (n < 0) != (d < 0) ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r;
    __udivmoddi4(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
