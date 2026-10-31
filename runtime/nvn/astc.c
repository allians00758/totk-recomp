// ASTC LDR decoder, 2D only (Khronos Data Format spec, ASTC chapter). Matches ARM astcenc's 8-bit
// decode bit for bit (the decode_unorm8 path its CLI uses for -dl / -ds into 8-bit images):
//   endpoints expand to 16 bits (linear: v * 257, sRGB: v << 8 | 0x80, all four channels),
//   texel = (e0 * (64 - w) + e1 * w + 32) >> 6, output = top 8 bits.
// Error blocks (illegal encodings) -> magenta. HDR blocks (HDR endpoint modes 2, 3, 7, 11, 14, 15 and fp16 void
// extents) are decoded as the HDR profile does (astcenc's LNS endpoints, interpolated, then LNS -> fp16) and clamped
// to 0..1 for the 8-bit image; an LDR-profile decoder makes them magenta, and TOTK has them (its whole scene was
// tinted magenta).
#include "astc.h"

#include <string.h>

// Quantization levels QUANT_2 .. QUANT_256 (index 0..20): bits / trits / quints per level.
static const uint8_t q_bits[21]   = {1, 0, 2, 0, 1, 3, 1, 2, 4, 2, 3, 5, 3, 4, 6, 4, 5, 7, 5, 6, 8};
static const uint8_t q_trits[21]  = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
static const uint8_t q_quints[21] = {0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};
enum { QUANT_6 = 4, QUANT_256 = 20 };

// Unquantization tables indexed by the raw ISE value (trit/quint << bits | bits), generated from the
// spec's formulas. Weights: QUANT_2..QUANT_32 -> 0..64. Colours: QUANT_6..QUANT_256 -> 0..255.
static const uint8_t wunq_data[] = {
    0,64,
    0,32,64,
    0,21,43,64,
    0,16,32,48,64,
    0,64,12,52,25,39,
    0,9,18,27,37,46,55,64,
    0,64,7,57,14,50,21,43,28,36,
    0,64,17,47,5,59,23,41,11,53,28,36,
    0,4,8,12,17,21,25,29,35,39,43,47,52,56,60,64,
    0,64,16,48,3,61,19,45,6,58,23,41,9,55,26,38,13,51,29,35,
    0,64,8,56,16,48,24,40,2,62,11,53,19,45,27,37,5,59,13,51,22,42,30,34,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,
};
static const uint16_t wunq_off[12] = {0,2,5,9,14,20,28,38,50,66,86,110};
static const uint8_t cunq_data[] = {
    /* 6 */
    0,255,51,204,102,153,
    /* 8 */
    0,36,73,109,146,182,219,255,
    /* 10 */
    0,255,28,227,56,199,84,171,113,142,
    /* 12 */
    0,255,69,186,23,232,92,163,46,209,116,139,
    /* 16 */
    0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255,
    /* 20 */
    0,255,67,188,13,242,80,175,27,228,94,161,40,215,107,148,54,201,121,134,
    /* 24 */
    0,255,33,222,66,189,99,156,11,244,44,211,77,178,110,145,22,233,55,200,88,167,121,134,
    /* 32 */
    0,8,16,24,33,41,49,57,66,74,82,90,99,107,115,123,132,140,148,156,165,173,181,189,198,206,214,222,
    231,239,247,255,
    /* 40 */
    0,255,32,223,65,190,97,158,6,249,39,216,71,184,104,151,13,242,45,210,78,177,110,145,19,236,52,203,
    84,171,117,138,26,229,58,197,91,164,123,132,
    /* 48 */
    0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,70,185,86,169,
    103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131,
    /* 64 */
    0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
    130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,195,199,203,207,211,215,219,223,227,
    231,235,239,243,247,251,255,
    /* 80 */
    0,255,16,239,32,223,48,207,64,191,80,175,96,159,112,143,3,252,19,236,35,220,51,204,67,188,83,172,
    100,155,116,139,6,249,22,233,38,217,54,201,71,184,87,168,103,152,119,136,9,246,25,230,42,213,58,197,
    74,181,90,165,106,149,122,133,13,242,29,226,45,210,61,194,77,178,93,162,109,146,125,130,
    /* 96 */
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,2,253,10,245,18,237,26,229,35,220,43,212,51,204,59,196,67,188,75,180,83,172,91,164,
    99,156,107,148,115,140,123,132,5,250,13,242,21,234,29,226,37,218,45,210,53,202,61,194,70,185,78,177,
    86,169,94,161,102,153,110,145,118,137,126,129,
    /* 128 */
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,
    70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124,
    126,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,
    177,179,181,183,185,187,189,191,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,223,225,
    227,229,231,233,235,237,239,241,243,245,247,249,251,253,255,
    /* 160 */
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,1,254,9,246,17,238,25,230,33,222,41,214,49,206,57,198,65,190,73,182,81,174,89,166,
    97,158,105,150,113,142,121,134,3,252,11,244,19,236,27,228,35,220,43,212,51,204,59,196,67,188,75,180,
    83,172,91,164,99,156,107,148,115,140,123,132,4,251,12,243,20,235,28,227,36,219,44,211,52,203,60,195,
    68,187,76,179,84,171,92,163,100,155,108,147,116,139,124,131,6,249,14,241,22,233,30,225,38,217,46,
    209,54,201,62,193,70,185,78,177,86,169,94,161,102,153,110,145,118,137,126,129,
    /* 192 */
    0,255,4,251,8,247,12,243,16,239,20,235,24,231,28,227,32,223,36,219,40,215,44,211,48,207,52,203,56,
    199,60,195,64,191,68,187,72,183,76,179,80,175,84,171,88,167,92,163,96,159,100,155,104,151,108,147,
    112,143,116,139,120,135,124,131,1,254,5,250,9,246,13,242,17,238,21,234,25,230,29,226,33,222,37,218,
    41,214,45,210,49,206,53,202,57,198,61,194,65,190,69,186,73,182,77,178,81,174,85,170,89,166,93,162,
    97,158,101,154,105,150,109,146,113,142,117,138,121,134,125,130,2,253,6,249,10,245,14,241,18,237,22,
    233,26,229,30,225,34,221,38,217,42,213,46,209,50,205,54,201,58,197,62,193,66,189,70,185,74,181,78,
    177,82,173,86,169,90,165,94,161,98,157,102,153,106,149,110,145,114,141,118,137,122,133,126,129,
};
static const uint16_t cunq_off[16] = {0,6,14,24,36,52,72,96,128,168,216,280,360,456,584,744};  // QUANT_256: identity
_Static_assert(sizeof wunq_data == 142 && sizeof cunq_data == 936, "unquantization table size");

// ---- bits ----

typedef struct { uint64_t lo, hi; } Bits128;

static inline uint64_t load64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

// n (<= 32) bits at pos; bits past 127 read as zero.
static inline uint32_t rbits(const Bits128 *b, int pos, int n) {
    if (n == 0 || pos >= 128) return 0;
    uint64_t v;
    if (pos >= 64) v = b->hi >> (pos - 64);
    else if (pos == 0) v = b->lo;
    else v = b->lo >> pos | b->hi << (64 - pos);
    return (uint32_t)(v & ((1ull << n) - 1));
}

static inline uint64_t bitrev64(uint64_t v) {
    v = (v >> 1 & 0x5555555555555555ull) | (v & 0x5555555555555555ull) << 1;
    v = (v >> 2 & 0x3333333333333333ull) | (v & 0x3333333333333333ull) << 2;
    v = (v >> 4 & 0x0F0F0F0F0F0F0F0Full) | (v & 0x0F0F0F0F0F0F0F0Full) << 4;
    v = (v >> 8 & 0x00FF00FF00FF00FFull) | (v & 0x00FF00FF00FF00FFull) << 8;
    v = (v >> 16 & 0x0000FFFF0000FFFFull) | (v & 0x0000FFFF0000FFFFull) << 16;
    return v >> 32 | v << 32;
}

// ---- integer sequence encoding ----

static int ise_bitcount(int count, int q) {
    int n = count * q_bits[q];
    if (q_trits[q]) n += (8 * count + 4) / 5;
    if (q_quints[q]) n += (7 * count + 2) / 3;
    return n;
}

static void trits_of(uint32_t T, int t[5]) {
    uint32_t C;
    if ((T >> 2 & 7) == 7) {
        C = (T >> 5 & 7) << 2 | (T & 3);
        t[4] = t[3] = 2;
    } else {
        C = T & 0x1F;
        if ((T >> 5 & 3) == 3) { t[4] = 2; t[3] = T >> 7 & 1; }
        else { t[4] = T >> 7 & 1; t[3] = T >> 5 & 3; }
    }
    if ((C & 3) == 3) {
        t[2] = 2; t[1] = C >> 4 & 1;
        t[0] = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1);
    } else if ((C >> 2 & 3) == 3) {
        t[2] = 2; t[1] = 2; t[0] = C & 3;
    } else {
        t[2] = C >> 4 & 1; t[1] = C >> 2 & 3;
        t[0] = (C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1);
    }
}

static void quints_of(uint32_t Q, int q[3]) {
    if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) {
        uint32_t q0 = Q & 1;
        q[2] = (int)(q0 << 2 | ((Q >> 4 & 1) & ~q0 & 1) << 1 | ((Q >> 3 & 1) & ~q0 & 1));
        q[1] = q[0] = 4;
        return;
    }
    uint32_t C;
    if ((Q >> 1 & 3) == 3) { q[2] = 4; C = (Q >> 3 & 3) << 3 | (~(Q >> 5) & 3) << 1 | (Q & 1); }
    else { q[2] = Q >> 5 & 3; C = Q & 0x1F; }
    if ((C & 7) == 5) { q[1] = 4; q[0] = C >> 3 & 3; }
    else { q[1] = C >> 3 & 3; q[0] = C & 7; }
}

// Decodes count raw ISE values (tq << bits | bits) starting at bit pos.
static void ise_decode(const Bits128 *b, int pos, int count, int q, uint8_t *out) {
    int nb = q_bits[q];
    if (q_trits[q]) {
        static const uint8_t tb[5] = {2, 2, 1, 2, 1}, ts[5] = {0, 2, 4, 5, 7};
        for (int i = 0; i < count; i += 5) {
            uint32_t m[5], T = 0;
            int k = 0, t[5];
            for (; k < 5 && i + k < count; k++) {
                m[k] = rbits(b, pos, nb); pos += nb;
                T |= rbits(b, pos, tb[k]) << ts[k]; pos += tb[k];
            }
            trits_of(T, t);
            for (int j = 0; j < k; j++) out[i + j] = (uint8_t)((uint32_t)t[j] << nb | m[j]);
        }
    } else if (q_quints[q]) {
        static const uint8_t qb[3] = {3, 2, 2}, qs[3] = {0, 3, 5};
        for (int i = 0; i < count; i += 3) {
            uint32_t m[3], Q = 0;
            int k = 0, v[3];
            for (; k < 3 && i + k < count; k++) {
                m[k] = rbits(b, pos, nb); pos += nb;
                Q |= rbits(b, pos, qb[k]) << qs[k]; pos += qb[k];
            }
            quints_of(Q, v);
            for (int j = 0; j < k; j++) out[i + j] = (uint8_t)((uint32_t)v[j] << nb | m[j]);
        }
    } else {
        for (int i = 0; i < count; i++, pos += nb) out[i] = (uint8_t)rbits(b, pos, nb);
    }
}

// ---- block mode ----

typedef struct { int wx, wy, dual, quant, wbits; } BlockMode;

static bool block_mode_2d(uint32_t m, uint32_t bw, uint32_t bh, BlockMode *bm) {
    uint32_t r = m >> 4 & 1, h = m >> 9 & 1, d = m >> 10 & 1, a = m >> 5 & 3;
    int wx, wy;
    if (m & 3) {
        r |= (m & 3) << 1;
        uint32_t b = m >> 7 & 3;
        switch (m >> 2 & 3) {
        case 0: wx = b + 4; wy = a + 2; break;
        case 1: wx = b + 8; wy = a + 2; break;
        case 2: wx = a + 2; wy = b + 8; break;
        default:
            b &= 1;
            if (m & 0x100) { wx = b + 2; wy = a + 2; }
            else { wx = a + 2; wy = b + 6; }
            break;
        }
    } else {
        r |= (m >> 2 & 3) << 1;
        if ((m >> 2 & 3) == 0) return false;
        uint32_t b = m >> 9 & 3;
        switch (m >> 7 & 3) {
        case 0: wx = 12; wy = a + 2; break;
        case 1: wx = a + 2; wy = 12; break;
        case 2: wx = a + 6; wy = b + 6; d = 0; h = 0; break;
        default:
            if (a == 0) { wx = 6; wy = 10; }
            else if (a == 1) { wx = 10; wy = 6; }
            else return false;
            break;
        }
    }
    int count = wx * wy * (int)(d + 1);
    bm->wx = wx; bm->wy = wy; bm->dual = (int)d;
    bm->quant = (int)(r - 2 + 6 * h);
    bm->wbits = ise_bitcount(count, bm->quant);
    return count <= 64 && bm->wbits >= 24 && bm->wbits <= 96 && wx <= (int)bw && wy <= (int)bh;
}

// ---- partitions ----

static uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;
    p ^= p >> 5;  p += p << 16; p ^= p >> 7; p ^= p >> 3;
    p ^= p << 6;  p ^= p >> 17;
    return p;
}

typedef struct { int s[8]; uint32_t r[4]; int count; } PartSel;

static void part_init(PartSel *ps, int seed, int count) {
    seed += (count - 1) * 1024;
    uint32_t rnum = hash52((uint32_t)seed);
    int s[8];
    for (int i = 0; i < 8; i++) { int v = (int)(rnum >> (4 * i) & 0xF); s[i] = v * v; }
    int sh1, sh2;
    if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    for (int i = 0; i < 8; i++) ps->s[i] = s[i] >> (i & 1 ? sh2 : sh1);
    ps->r[0] = rnum >> 14; ps->r[1] = rnum >> 10; ps->r[2] = rnum >> 6; ps->r[3] = rnum >> 2;
    ps->count = count;
}

static inline int part_of(const PartSel *ps, int x, int y) {
    // z = 0 for 2D, so seeds 9..12 drop out
    int a = (int)((uint32_t)(ps->s[0] * x + ps->s[1] * y) + ps->r[0]) & 0x3F;
    int b = (int)((uint32_t)(ps->s[2] * x + ps->s[3] * y) + ps->r[1]) & 0x3F;
    int c = ps->count >= 3 ? (int)((uint32_t)(ps->s[4] * x + ps->s[5] * y) + ps->r[2]) & 0x3F : 0;
    int d = ps->count >= 4 ? (int)((uint32_t)(ps->s[6] * x + ps->s[7] * y) + ps->r[3]) & 0x3F : 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

// ---- colour endpoints ----

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// e[0..3] endpoint 0, e[4..7] endpoint 1 (8-bit). Returns false for an HDR mode.
static bool unpack_endpoints(int cem, const uint8_t *v, int e0[4], int e1[4]) {
    switch (cem) {
    case 0:  // L direct
        e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1];
        e0[3] = e1[3] = 255;
        return true;
    case 1: {  // L delta
        int l0 = v[0] >> 2 | (v[1] & 0xC0);
        int l1 = l0 + (v[1] & 0x3F);
        if (l1 > 255) l1 = 255;
        e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1;
        e0[3] = e1[3] = 255;
        return true;
    }
    case 4:  // LA direct
        e0[0] = e0[1] = e0[2] = v[0]; e1[0] = e1[1] = e1[2] = v[1];
        e0[3] = v[2]; e1[3] = v[3];
        return true;
    case 5: {  // LA delta
        int l0 = v[0], l1 = v[1], a0 = v[2], a1 = v[3];
        l0 |= (l1 & 0x80) << 1; a0 |= (a1 & 0x80) << 1;
        l1 &= 0x7F; a1 &= 0x7F;
        if (l1 & 0x40) l1 -= 0x80;
        if (a1 & 0x40) a1 -= 0x80;
        l0 >>= 1; a0 >>= 1; l1 >>= 1; a1 >>= 1;  // arithmetic shift of the signed deltas
        l1 = clamp255(l1 + l0); a1 = clamp255(a1 + a0);
        e0[0] = e0[1] = e0[2] = l0; e1[0] = e1[1] = e1[2] = l1;
        e0[3] = a0; e1[3] = a1;
        return true;
    }
    case 6:  // RGB scale
    case 10: {  // RGB scale + two alphas
        int s = v[3];
        for (int c = 0; c < 3; c++) { e1[c] = v[c]; e0[c] = v[c] * s >> 8; }
        if (cem == 6) e0[3] = e1[3] = 255;
        else { e0[3] = v[4]; e1[3] = v[5]; }
        return true;
    }
    case 8:  // RGB(A) direct
    case 12: {
        int a[4] = {v[0], v[2], v[4], cem == 12 ? v[6] : 0};
        int b[4] = {v[1], v[3], v[5], cem == 12 ? v[7] : 0};
        if (a[0] + a[1] + a[2] > b[0] + b[1] + b[2]) {  // blue contraction: swap + uncontract
            for (int c = 0; c < 4; c++) { e0[c] = b[c]; e1[c] = a[c]; }
            e0[0] = (e0[0] + e0[2]) >> 1; e0[1] = (e0[1] + e0[2]) >> 1;
            e1[0] = (e1[0] + e1[2]) >> 1; e1[1] = (e1[1] + e1[2]) >> 1;
        } else {
            for (int c = 0; c < 4; c++) { e0[c] = a[c]; e1[c] = b[c]; }
        }
        if (cem == 8) e0[3] = e1[3] = 255;
        return true;
    }
    case 9:  // RGB(A) base + delta
    case 13: {
        int n = cem == 13 ? 4 : 3;
        int a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
        for (int c = 0; c < n; c++) {
            int base = v[2 * c], delta = v[2 * c + 1];
            a[c] = base >> 1 | (delta & 0x80);
            b[c] = delta >> 1 & 0x3F;
            if (b[c] & 0x20) b[c] -= 0x40;
        }
        int sum = b[0] + b[1] + b[2];
        for (int c = 0; c < 4; c++) b[c] += a[c];
        if (sum < 0) {  // swap + uncontract
            for (int c = 0; c < 4; c++) { e0[c] = b[c]; e1[c] = a[c]; }
            e0[0] = (e0[0] + e0[2]) >> 1; e0[1] = (e0[1] + e0[2]) >> 1;
            e1[0] = (e1[0] + e1[2]) >> 1; e1[1] = (e1[1] + e1[2]) >> 1;
        } else {
            for (int c = 0; c < 4; c++) { e0[c] = a[c]; e1[c] = b[c]; }
        }
        for (int c = 0; c < 4; c++) { e0[c] = clamp255(e0[c]); e1[c] = clamp255(e1[c]); }
        if (cem == 9) e0[3] = e1[3] = 255;
        return true;
    }
    default:  // 2, 3, 7, 11, 14, 15: HDR
        return false;
    }
}


// ---- HDR endpoints (astcenc color_unquantize.cpp): 16-bit LNS values (12 bits << 4); alpha 0x7800 is fp16 1.0
enum { CH_UNORM = 0, CH_LNS = 1, CH_FP16 = 2 };

static int clamp12(int v) { return v < 0 ? 0 : v > 0xFFF ? 0xFFF : v; }

static void hdr_lum_large(const uint8_t *v, int e0[4], int e1[4]) {
    int y0, y1;
    if (v[1] >= v[0]) { y0 = v[0] << 4; y1 = v[1] << 4; }
    else { y0 = (v[1] << 4) + 8; y1 = (v[0] << 4) - 8; }
    for (int c = 0; c < 3; c++) { e0[c] = y0 << 4; e1[c] = y1 << 4; }
    e0[3] = e1[3] = 0x7800;
}

static void hdr_lum_small(const uint8_t *v, int e0[4], int e1[4]) {
    int y0, y1;
    if (v[0] & 0x80) { y0 = ((v[1] & 0xE0) << 4) | ((v[0] & 0x7F) << 2); y1 = (v[1] & 0x1F) << 2; }
    else { y0 = ((v[1] & 0xF0) << 4) | ((v[0] & 0x7F) << 1); y1 = (v[1] & 0xF) << 1; }
    y1 += y0;
    if (y1 > 0xFFF) y1 = 0xFFF;
    for (int c = 0; c < 3; c++) { e0[c] = y0 << 4; e1[c] = y1 << 4; }
    e0[3] = e1[3] = 0x7800;
}

static void hdr_rgbo(const uint8_t *in, int e0[4], int e1[4]) {
    int v0 = in[0], v1 = in[1], v2 = in[2], v3 = in[3];
    int modeval = ((v0 & 0xC0) >> 6) | (((v1 & 0x80) >> 7) << 2) | (((v2 & 0x80) >> 7) << 3);
    int majcomp, mode;
    if ((modeval & 0xC) != 0xC) { majcomp = modeval >> 2; mode = modeval & 3; }
    else if (modeval != 0xF) { majcomp = modeval & 3; mode = 4; }
    else { majcomp = 0; mode = 5; }
    int red = v0 & 0x3F, green = v1 & 0x1F, blue = v2 & 0x1F, scale = v3 & 0x1F;
    int bit0 = (v1 >> 6) & 1, bit1 = (v1 >> 5) & 1, bit2 = (v2 >> 6) & 1, bit3 = (v2 >> 5) & 1;
    int bit4 = (v3 >> 7) & 1, bit5 = (v3 >> 6) & 1, bit6 = (v3 >> 5) & 1;
    int oh = 1 << mode;
    if (oh & 0x30) green |= bit0 << 6;
    if (oh & 0x3A) green |= bit1 << 5;
    if (oh & 0x30) blue |= bit2 << 6;
    if (oh & 0x3A) blue |= bit3 << 5;
    if (oh & 0x3D) scale |= bit6 << 5;
    if (oh & 0x2D) scale |= bit5 << 6;
    if (oh & 0x04) scale |= bit4 << 7;
    if (oh & 0x3B) red |= bit4 << 6;
    if (oh & 0x04) red |= bit3 << 6;
    if (oh & 0x10) red |= bit5 << 7;
    if (oh & 0x0F) red |= bit2 << 7;
    if (oh & 0x05) red |= bit1 << 8;
    if (oh & 0x0A) red |= bit0 << 8;
    if (oh & 0x05) red |= bit0 << 9;
    if (oh & 0x02) red |= bit6 << 9;
    if (oh & 0x01) red |= bit3 << 10;
    if (oh & 0x02) red |= bit5 << 10;
    static const int shamts[6] = {1, 1, 2, 3, 4, 5};
    int sh = shamts[mode];
    red <<= sh; green <<= sh; blue <<= sh; scale <<= sh;
    if (mode != 5) { green = red - green; blue = red - blue; }
    int t;
    if (majcomp == 1) { t = red; red = green; green = t; }
    else if (majcomp == 2) { t = red; red = blue; blue = t; }
    int red0 = red - scale, green0 = green - scale, blue0 = blue - scale;
    e0[0] = clamp12(red0) << 4; e0[1] = clamp12(green0) << 4; e0[2] = clamp12(blue0) << 4;
    e1[0] = clamp12(red) << 4; e1[1] = clamp12(green) << 4; e1[2] = clamp12(blue) << 4;
    e0[3] = e1[3] = 0x7800;
}

static void hdr_rgb(const uint8_t *in, int e0[4], int e1[4]) {
    int v0 = in[0], v1 = in[1], v2 = in[2], v3 = in[3], v4 = in[4], v5 = in[5];
    int modeval = ((v1 & 0x80) >> 7) | (((v2 & 0x80) >> 7) << 1) | (((v3 & 0x80) >> 7) << 2);
    int majcomp = ((v4 & 0x80) >> 7) | (((v5 & 0x80) >> 7) << 1);
    if (majcomp == 3) {
        e0[0] = v0 << 8; e0[1] = v2 << 8; e0[2] = (v4 & 0x7F) << 9;
        e1[0] = v1 << 8; e1[1] = v3 << 8; e1[2] = (v5 & 0x7F) << 9;
        e0[3] = e1[3] = 0x7800;
        return;
    }
    int a = v0 | ((v1 & 0x40) << 2), b0 = v2 & 0x3F, b1 = v3 & 0x3F, c = v1 & 0x3F, d0 = v4 & 0x7F, d1 = v5 & 0x7F;
    static const int dbits_tab[8] = {7, 6, 7, 6, 5, 6, 5, 6};
    int dbits = dbits_tab[modeval];
    int bit0 = (v2 >> 6) & 1, bit1 = (v3 >> 6) & 1, bit2 = (v4 >> 6) & 1, bit3 = (v5 >> 6) & 1;
    int bit4 = (v4 >> 5) & 1, bit5 = (v5 >> 5) & 1;
    int oh = 1 << modeval;
    if (oh & 0xA4) a |= bit0 << 9;
    if (oh & 0x8) a |= bit2 << 9;
    if (oh & 0x50) a |= bit4 << 9;
    if (oh & 0x50) a |= bit5 << 10;
    if (oh & 0xA0) a |= bit1 << 10;
    if (oh & 0xC0) a |= bit2 << 11;
    if (oh & 0x4) c |= bit1 << 6;
    if (oh & 0xE8) c |= bit3 << 6;
    if (oh & 0x20) c |= bit2 << 7;
    if (oh & 0x5B) { b0 |= bit0 << 6; b1 |= bit1 << 6; }
    if (oh & 0x12) { b0 |= bit2 << 7; b1 |= bit3 << 7; }
    if (oh & 0xAF) { d0 |= bit4 << 5; d1 |= bit5 << 5; }
    if (oh & 0x5) { d0 |= bit2 << 6; d1 |= bit3 << 6; }
    int sx = 32 - dbits;
    d0 = (int)((uint32_t)d0 << sx) >> sx;
    d1 = (int)((uint32_t)d1 << sx) >> sx;
    int vs = (modeval >> 1) ^ 3;
    a <<= vs; b0 <<= vs; b1 <<= vs; c <<= vs; d0 *= 1 << vs; d1 *= 1 << vs;
    int r1 = a, g1 = a - b0, bl1 = a - b1, r0 = a - c, g0 = a - b0 - c - d0, bl0 = a - b1 - c - d1;
    r0 = clamp12(r0); g0 = clamp12(g0); bl0 = clamp12(bl0); r1 = clamp12(r1); g1 = clamp12(g1); bl1 = clamp12(bl1);
    int t;
    if (majcomp == 1) { t = r0; r0 = g0; g0 = t; t = r1; r1 = g1; g1 = t; }
    else if (majcomp == 2) { t = r0; r0 = bl0; bl0 = t; t = r1; r1 = bl1; bl1 = t; }
    e0[0] = r0 << 4; e0[1] = g0 << 4; e0[2] = bl0 << 4;
    e1[0] = r1 << 4; e1[1] = g1 << 4; e1[2] = bl1 << 4;
    e0[3] = e1[3] = 0x7800;
}

static void hdr_alpha(const uint8_t *in, int *a0, int *a1) {
    int v6 = in[0], v7 = in[1];
    int sel = ((v6 >> 7) & 1) | ((v7 >> 6) & 2);
    v6 &= 0x7F; v7 &= 0x7F;
    if (sel == 3) { *a0 = v6 << 5; *a1 = v7 << 5; }
    else {
        v6 |= (v7 << (sel + 1)) & 0x780;
        v7 &= 0x3F >> sel;
        v7 ^= 32 >> sel;
        v7 -= 32 >> sel;
        v6 <<= 4 - sel;
        v7 *= 1 << (4 - sel);
        v7 += v6;
        *a0 = v6; *a1 = clamp12(v7);
    }
    *a0 <<= 4; *a1 <<= 4;
}

// HDR endpoint modes: 16-bit endpoints and each channel's kind. False for an LDR mode.
static bool hdr_endpoints(int cem, const uint8_t *v, int e0[4], int e1[4], uint8_t kind[4]) {
    switch (cem) {
    case 2: case 3: case 7: case 11: case 14: case 15:
        kind[0] = kind[1] = kind[2] = CH_LNS; kind[3] = CH_FP16;
        break;
    default: return false;
    }
    switch (cem) {
    case 2: hdr_lum_large(v, e0, e1); return true;
    case 3: hdr_lum_small(v, e0, e1); return true;
    case 7: hdr_rgbo(v, e0, e1); return true;
    case 11: hdr_rgb(v, e0, e1); return true;
    case 14: hdr_rgb(v, e0, e1); e0[3] = v[6] * 257; e1[3] = v[7] * 257; kind[3] = CH_UNORM; return true;
    case 15: hdr_rgb(v, e0, e1); hdr_alpha(v + 6, &e0[3], &e1[3]); kind[3] = CH_LNS; return true;
    default: return false;
    }
}

static int lns_to_fp16(int p) {
    int mc = p & 0x7FF, ec = p >> 11, mt;
    if (mc < 512) mt = mc * 3;
    else if (mc < 1536) mt = mc * 4 - 512;
    else mt = mc * 5 - 2048;
    int r = ec << 10 | mt >> 3;
    return r > 0x7BFF ? 0x7BFF : r;
}

// An fp16 value to 0..255 (clamped to 0..1, rounded).
static uint8_t fp16_unorm8(int h) {
    if (h & 0x8000) return 0;
    int e = h >> 10 & 0x1F, m = h & 0x3FF;
    if (e >= 15) return 255;  // (>= 1.0, inf, NaN)
    float f = e ? (float)(1024 + m) / (float)(1 << (25 - e)) : (float)m / (float)(1 << 24);
    int r = (int)(f * 255.0f + 0.5f);
    return (uint8_t)(r > 255 ? 255 : r);
}

// ---- block ----

// Error colour (astc.h astc_error_rgba): magenta, as astcenc; the renderer sets transparent black.
uint8_t astc_error_rgba[4] = {0xFF, 0x00, 0xFF, 0xFF};

static void fill_error(uint8_t *out, int texels) {
    for (int i = 0; i < texels; i++) memcpy(out + 4 * i, astc_error_rgba, 4);
}

bool astc_decode_block(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t *out) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12) return false;  // not an ASTC 2D footprint
    int texels = (int)(bw * bh);
    Bits128 b = {load64(block), load64(block + 8)};
    uint32_t mode = (uint32_t)(b.lo & 0x7FF);

    if ((mode & 0x1FF) == 0x1FC) {  // void extent
        if ((b.lo >> 10 & 3) != 3) goto error;  // reserved bits
        uint32_t s0 = rbits(&b, 12, 13), s1 = rbits(&b, 25, 13);
        uint32_t t0 = rbits(&b, 38, 13), t1 = rbits(&b, 51, 13);
        bool all_ones = s0 == 0x1FFF && s1 == 0x1FFF && t0 == 0x1FFF && t1 == 0x1FFF;
        if ((s0 >= s1 || t0 >= t1) && !all_ones) goto error;
        uint8_t c[4] = {block[9], block[11], block[13], block[15]};  // top 8 bits of the UNORM16s
        if (mode & 0x200)  // HDR: fp16 values
            for (int i = 0; i < 4; i++) c[i] = fp16_unorm8(block[8 + 2 * i] | block[9 + 2 * i] << 8);
        for (int i = 0; i < texels; i++) memcpy(out + 4 * i, c, 4);
        return true;
    }

    BlockMode bm;
    if (!block_mode_2d(mode, bw, bh, &bm)) goto error;
    int pcount = (int)(b.lo >> 11 & 3) + 1;
    int wcount = bm.wx * bm.wy;

    // Weights, read backwards from the top of the block.
    uint8_t raw[64];
    Bits128 rev = {bitrev64(b.hi), bitrev64(b.lo)};
    ise_decode(&rev, 0, wcount << bm.dual, bm.quant, raw);
    if (bm.dual && pcount == 4) goto error;

    const uint8_t *wt = wunq_data + wunq_off[bm.quant];
    uint8_t grid[2][80];
    memset(grid, 0, sizeof grid);
    if (bm.dual) {
        for (int i = 0; i < wcount; i++) { grid[0][i] = wt[raw[2 * i]]; grid[1][i] = wt[raw[2 * i + 1]]; }
    } else {
        for (int i = 0; i < wcount; i++) grid[0][i] = wt[raw[i]];
    }

    // Colour endpoint modes.
    int cem[4];
    int below = 128 - bm.wbits, high = 0;
    if (pcount == 1) {
        cem[0] = (int)(b.lo >> 13 & 0xF);
    } else {
        high = 3 * pcount - 4;
        below -= high;
        uint32_t enc = (uint32_t)(b.lo >> 23 & 0x3F) | rbits(&b, below, high) << 6;
        uint32_t base = enc & 3;
        if (base == 0) {
            for (int i = 0; i < pcount; i++) cem[i] = (int)(enc >> 2 & 0xF);
            below += high;
            high = 0;
        } else {
            base--;
            for (int i = 0; i < pcount; i++) cem[i] = (int)(((enc >> (2 + i) & 1) + base) << 2);
            for (int i = 0; i < pcount; i++) cem[i] |= (int)(enc >> (2 + pcount + 2 * i) & 3);
        }
    }
    int ccount = 0;
    for (int i = 0; i < pcount; i++) ccount += ((cem[i] >> 2) + 1) * 2;
    if (ccount > 18) goto error;
    int cbits = (pcount == 1 ? 111 : 99) - bm.wbits - high - (bm.dual ? 2 : 0);
    if (cbits < 0) cbits = 0;
    int cq = -1;
    for (int q = 20; q >= 0; q--) {
        if (ise_bitcount(ccount, q) <= cbits) { cq = q; break; }
    }
    if (cq < QUANT_6) goto error;
    uint8_t cv[18];
    ise_decode(&b, pcount == 1 ? 17 : 29, ccount, cq, cv);
    if (cq != QUANT_256) {
        const uint8_t *ct = cunq_data + cunq_off[cq - QUANT_6];
        for (int i = 0; i < ccount; i++) cv[i] = ct[cv[i]];
    }
    int ccs = bm.dual ? (int)rbits(&b, below - 2, 2) : -1;

    // Endpoints, expanded to 16 bits.
    int ep[4][2][4];
    uint8_t kind[4][4] = {{0}};  // (per partition and channel: CH_UNORM, or an HDR mode's CH_LNS / CH_FP16)
    bool any_hdr = false;
    for (int p = 0, k = 0; p < pcount; p++) {
        int *e0 = ep[p][0], *e1 = ep[p][1];
        if (hdr_endpoints(cem[p], cv + k, e0, e1, kind[p])) {
            any_hdr = true;
            k += ((cem[p] >> 2) + 1) * 2;
            continue;  // (already 16-bit)
        }
        if (!unpack_endpoints(cem[p], cv + k, e0, e1)) {
            static const int magenta[4] = {0xFF, 0x00, 0xFF, 0xFF};
            memcpy(e0, magenta, sizeof magenta);
            memcpy(e1, magenta, sizeof magenta);
        }
        k += ((cem[p] >> 2) + 1) * 2;
        for (int c = 0; c < 4; c++) {
            e0[c] = srgb ? e0[c] << 8 | 0x80 : e0[c] * 257;
            e1[c] = srgb ? e1[c] << 8 | 0x80 : e1[c] * 257;
        }
    }

    // Weight grid -> texel weights (bilinear infill), per axis.
    int xi[12], xf[12], yi[12], yf[12];
    for (uint32_t x = 0; x < bw; x++) {
        int v = (int)(((1024 + bw / 2) / (bw - 1)) * x * (uint32_t)(bm.wx - 1) + 32) >> 6;
        xi[x] = v >> 4; xf[x] = v & 0xF;
    }
    for (uint32_t y = 0; y < bh; y++) {
        int v = (int)(((1024 + bh / 2) / (bh - 1)) * y * (uint32_t)(bm.wy - 1) + 32) >> 6;
        yi[y] = (v >> 4) * bm.wx; yf[y] = v & 0xF;
    }

    PartSel ps = {0};
    if (pcount > 1) part_init(&ps, (int)(b.lo >> 13 & 0x3FF), pcount);
    int small = texels < 32;

    uint8_t *o = out;
    for (uint32_t y = 0; y < bh; y++) {
        for (uint32_t x = 0; x < bw; x++, o += 4) {
            int fs = xf[x], ft = yf[y];
            int w11 = (fs * ft + 8) >> 4, w10 = fs - w11, w01 = ft - w11, w00 = 16 - fs - ft + w11;
            int i0 = yi[y] + xi[x];
            int w[2];
            for (int pl = 0; pl <= bm.dual; pl++) {
                const uint8_t *g = grid[pl];
                w[pl] = (g[i0] * w00 + g[i0 + 1] * w10 + g[i0 + bm.wx] * w01 + g[i0 + bm.wx + 1] * w11 + 8) >> 4;
            }
            int p = pcount > 1 ? part_of(&ps, small ? (int)x << 1 : (int)x, small ? (int)y << 1 : (int)y) : 0;
            const int *e0 = ep[p][0], *e1 = ep[p][1];
            for (int c = 0; c < 4; c++) {
                int wc = c == ccs ? w[1] : w[0];
                int v = (e0[c] * (64 - wc) + e1[c] * wc + 32) >> 6;
                if (!any_hdr || kind[p][c] == CH_UNORM) o[c] = (uint8_t)(v >> 8);  // (top 8 bits)
                else o[c] = fp16_unorm8(kind[p][c] == CH_LNS ? lns_to_fp16(v) : v);
            }
        }
    }
    return true;

error:
    fill_error(out, texels);
    return false;
}

void astc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh,
                     bool srgb, uint8_t *rgba_out) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12) return;
    uint32_t bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    uint8_t tmp[12 * 12 * 4];
    for (uint32_t j = 0; j < by; j++) {
        for (uint32_t i = 0; i < bx; i++) {
            const uint8_t *src = blocks + 16 * ((size_t)j * bx + i);
            uint32_t x0 = i * bw, y0 = j * bh;
            uint32_t cw = width - x0 < bw ? width - x0 : bw, ch = height - y0 < bh ? height - y0 : bh;
            // decode in place when the block's rows are contiguous in the output
            uint8_t *dst = width == bw && ch == bh ? rgba_out + (size_t)y0 * width * 4 : tmp;
            astc_decode_block(src, bw, bh, srgb, dst);
            if (dst == tmp) {
                for (uint32_t y = 0; y < ch; y++)
                    memcpy(rgba_out + ((size_t)(y0 + y) * width + x0) * 4, tmp + y * bw * 4, cw * 4);
            }
        }
    }
}
