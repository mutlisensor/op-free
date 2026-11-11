/*
 * table — wavetable synth with a smooth 2-D morph through a 7 x 6 grid of our own band-limited waves.
 *
 *   P1        position    through the six waves of the bank, gentle to intense: waves at P1 0, 51, 102, 153, 204,
 *                         255, interpolated in between (README has the grid)
 *   P2        bank        through the seven banks, interpolated in between: analog 0, organ 43, vocal 85, reso 128,
 *                         sync 170, fm 213, metal 255. Together with P1 a bilinear morph over the whole grid
 *   velocity              level (linear) and position: P1 + (velocity - 100) / 2, so soft notes sit on gentler
 *                         waves (velocity 127: +13.5, velocity 1: -49.5 on the P1 scale)
 *   attack..release       host envelope, stepped every 4 samples and interpolated in between (envelope())
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine.
 *
 * Tables (table_waves.h, made by gen_tables.py): every wave in four mip levels of 256, 128, 64 and 32 samples holding
 * harmonics up to 127, 63, 31 and 15, and as the coefficients of its first 7 harmonics. Each block reads the mip whose
 * highest harmonic stays at or below 16 kHz for the block's highest pitch; above the last one (notes above 1,067 Hz)
 * the engine sums the harmonics itself (the additive path, add_static/add_ramp). When that changes, the block plays
 * both and crossfades.
 *
 * How the 2-D morph stays cheap and smooth: a block of 64 samples passes over at most ~50 samples of the mip (130
 * on the last one), so per block the four grid corners around the morph position are blended only over that
 * stretch of the table into a short scratch line, with the morph position ramping along it from the last block's
 * value to this block's. Per sample, the oscillator then reads the scratch line with a 4-point Hermite (Catmull-Rom)
 * interpolation. P1 and P2 therefore move sample by sample, never in block steps, and the cost of the blend
 * shrinks with the pitch. The tables are 2x oversampled, which puts the interpolation's images between 16 and 32
 * kHz: what folds back lands above 12 kHz; the second images near 44.1 kHz fold lower, and below 11 kHz the
 * aliasing stays 43-96 dB under the note, the least on the brightest waves in the top mip (gen_tables.py --report,
 * tests/alias.c).
 *
 * The additive path has no interpolation and so no images: per block the four corners' coefficients are blended at
 * the block's end position (each harmonic times a band gain, 1 up to 16 kHz and fading to 0 at 18.76 kHz; the
 * block's start is the last block's end, kept in the voice), per sample the coefficients ramp from one to the other
 * when they moved, sine and cosine of the phase come from a 7th-order polynomial and the higher harmonics from the
 * Chebyshev recurrence z(h+1) = 2 cos(t) z(h) - z(h-1), all in pairs of 16-bit lanes (sine and cosine side by side:
 * one Blackfin vector instruction per step). Below 11 kHz nothing but the harmonics comes within 77 dB of the note
 * from C#6 to F7 (higher up only sync 6.4, whose first three harmonics are weak, gets to -67 dB at C8-F8).
 *
 * Original code, MIT licence, see CONTRIBUTING.md. The 2-D wavetable idea follows Mutable Instruments' Plaits
 * wavetable model and the PPG Wave; no code or tables from either.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"
#include "table_waves.h"

#define N OPZ_BLOCK_FRAMES
#define ALWAYS_INLINE static inline __attribute__((always_inline))
#define NOINLINE static __attribute__((noinline))      /* per-block work kept out of render: smaller code */

#define XMAX ((TW_WAVES - 1) * 65536 - 1)   /* morph positions in Q16 waves and banks; the top stays just inside */
#define YMAX ((TW_BANKS - 1) * 65536 - 1)   /* the last segment, so the corner after it always exists */
#define INC_MAX 0x40000000u                 /* 11 kHz: keeps the scratch line short (SPAN) */
#define SPAN 136                            /* scratch entries: at most 132 (last mip at INC_MAX), 99 in a crossfade */
#define VEL_X ((TW_WAVES - 1) * 65536 / 512)    /* position per velocity step: half a P1 step */
#define OUT_AMP (0x10000000 / TW_SAW_PEAK)  /* Q15: the saw peaks at OPZ_VOICE_PEAK (-12 dBFS) at velocity 127 */
#define MIP_ADD TW_MIPS                     /* "mip" of the additive path, above the last table mip */
/* Band gain of harmonic h at phase increment inc: (BAND_HI - h (inc >> 4)) >> 9, clamped to 0 .. 1 (Q15): 1 up to
 * 16 kHz (97,391,549 = 16,000 / 44,100 x 2^28), 0 from 18,756 Hz on (2^24 higher). BAND_FULL: every harmonic at 1
 * while TW_ADD_H (inc >> 4) stays at or below it. */
#define BAND_HI (97391549 + (1 << 24))
#define BAND_FULL (BAND_HI - (32767 << 9))

typedef struct State {
    uint32_t ph;            /* oscillator phase, Q32 */
    int32_t x, y;           /* morph position at the end of the last block (Q16 waves, banks) */
    int32_t xv;             /* velocity's offset on the position, Q16 waves */
    int32_t amp;            /* velocity x output gain, Q15 */
    int32_t mip;            /* mip of the last block (MIP_ADD: the additive path); -1 before the first of a note */
    OpzMix mix;             /* the track's level and pan (opz_mix.h) */
    uint32_t bkey;          /* additive path: the band of co (its pitch, 0 when every harmonic is in full) */
    uint32_t co[TW_ADD_H];  /* additive path: the coefficients at the end of the last block (at x, y, bkey) */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");
_Static_assert(OUT_AMP < 32768, "the output gain must fit Q15");

/* The blended stretch of table of one block (two in a mip crossfade). Render runs one voice at a time, so all
 * voices share them; .bss keeps them inside the slot window. */
static int16_t s_line[2][SPAN];
static int32_t s_pos[2][N];         /* read positions in the lines, Q16 */
static int16_t s_gain[2][N];        /* gains, Q15: the envelope (times the crossfade's weights) */
static uint32_t s_co[2][TW_ADD_H];  /* additive path: coefficients at the block's end, end - start (word h - 1) */

/* 32768 / m (Q15) for the morph ramp over m entries. */
static uint16_t s_recip[SPAN];

static const ModuleCallbackInterface *s_cb;

typedef struct Span {
    int32_t pos;            /* read position of sample 0 in the scratch line, Q16 entries */
    int s16;                /* the phase increment >> s16 is the read position's increment */
#ifdef TABLE_CHECK
    int n;                  /* entries in the line (checks only) */
#endif
} Span;

/* Bounds checks for the host fuzz (tests/fuzz.c defines TABLE_CHECK); nothing in the device or opzsim builds. */
#ifndef TABLE_CHECK
#define TABLE_CHECK(cond) ((void)0)
#endif
#define CHECK_CORNER(m, p) TABLE_CHECK((p) >= k_tw + k_tw_off[m] && \
    (p) + (TW_WAVES + 1) * (1 << k_tw_log2[m]) < k_tw + k_tw_off[m] + (TW_BANKS * TW_WAVES << k_tw_log2[m]))

ALWAYS_INLINE int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

ALWAYS_INLINE int32_t mul16(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int16_t)b; }

/* 16 x 16 fractional multiplies, one Blackfin instruction each (the host versions give the same bits):
 * mulfr = 2 a b (Q31 from two Q15), mulfr16 = floor(a b / 2^15) (Q15); both saturate only -1 x -1. */
#if defined(__bfin__)
ALWAYS_INLINE int32_t mulfr(int32_t a, int32_t b) { return __builtin_bfin_mult_fr1x32((fract16)a, (fract16)b); }
ALWAYS_INLINE int32_t mulfr16(int32_t a, int32_t b) { return __builtin_bfin_mult_fr1x16((fract16)a, (fract16)b); }
#else
ALWAYS_INLINE int32_t mulfr(int32_t a, int32_t b)
{
    int32_t p = mul16(a, b);
    return p == 0x40000000 ? 0x7fffffff : p * 2;
}
ALWAYS_INLINE int32_t mulfr16(int32_t a, int32_t b)
{
    int32_t p = mul16(a, b);
    return p == 0x40000000 ? 0x7fff : p >> 15;
}
#endif

/* Bilinear mix of four grid corners (15-bit values: the differences fit 16 bits), Q15 fractions fx, fy. */
ALWAYS_INLINE int32_t bilerp(int32_t c00, int32_t c01, int32_t c10, int32_t c11, int32_t fx, int32_t fy)
{
    int32_t a = c00 + (mul16(c01 - c00, fx) >> 15);
    int32_t b = c10 + (mul16(c11 - c10, fx) >> 15);
    return a + (mul16(b - a, fy) >> 15);
}

/* One blended entry: the four grid corners around (x, y) at table sample j of mip `tab` (L samples per wave),
 * bilinear, times the velocity gain amp. */
ALWAYS_INLINE int16_t corner_mix(const int16_t *p, int L, int32_t fx, int32_t fy, int32_t amp)
{
    return (int16_t)mulfr16(bilerp(p[0], p[L], p[TW_WAVES * L], p[TW_WAVES * L + L], fx, fy), amp);
}

ALWAYS_INLINE const int16_t *corner(const int16_t *tab, int lg, int32_t x, int32_t y)
{
    return tab + (((y >> 16) * TW_WAVES + (x >> 16)) << lg);
}

/* (to - from) / m for the morph ramp, rounded toward zero so the ramp never passes `to`. */
ALWAYS_INLINE int32_t ramp_step(int32_t from, int32_t to, int32_t r)
{
    int32_t d = to - from;
    int32_t q = ((d < 0 ? -d : d) >> 5) * r >> 10;
    return d < 0 ? -q : q;
}

/* Blend the four corners around the morph position into line[0 .. n-1] over the stretch of mip m that this block
 * reads: entry k is the table sample (phase index - 1 + k) mod L. The position ramps from (xs, ys) at entry 1 (where
 * sample 0 reads) to (xe, ye) at entry n - 3 (the furthest one the last sample can read) and stays there; the ramp
 * steps round toward zero, so every position lies between the two ends (inside the grid). The entries carry the
 * velocity gain amp (Q15). Returns the number of entries, or 0 if the stretch would not fit (only in a crossfade
 * after a large jump). */
static int blend(int16_t *line, Span *sp, int m, uint32_t ph, uint32_t inc_max,
                 int32_t xs, int32_t xe, int32_t ys, int32_t ye, int32_t amp)
{
    int lg = k_tw_log2[m];
    int sh = 32 - lg;
    int s16 = sh - 16;
    uint32_t mask = (1u << lg) - 1u;
    int32_t pos = 65536 + (int32_t)((ph >> s16) & 0xffffu);
    /* every sample moves the read position by (its increment >> s16) <= inc_max >> s16 */
    uint32_t far = (uint32_t)pos + (N * (inc_max >> s16));
    if (far >= (uint32_t)(SPAN - 2) << 16) return 0;
    int n = (int)(far >> 16) + 3;
    sp->pos = pos;
    sp->s16 = s16;
    TABLE_CHECK(n <= SPAN && (sp->n = n) > 0);
    const int16_t *tab = k_tw + k_tw_off[m];
    const int L = 1 << lg;
    uint32_t j = ((ph >> sh) - 1u) & mask;
    if (xs == xe && ys == ye) {                              /* the morph holds still: one set of corners */
        const int16_t *p = corner(tab, lg, xe, ye);
        int32_t fx = (xe >> 1) & 0x7fff, fy = (ye >> 1) & 0x7fff;
        for (int k = 0; k < n; k++) {
            CHECK_CORNER(m, p + j);
            line[k] = corner_mix(p + j, L, fx, fy, amp);
            j = (j + 1u) & mask;
        }
        return n;
    }
    int ramp = n - 4 > 1 ? n - 4 : 1;
    int32_t r = s_recip[ramp];
    int32_t dx = ramp_step(xs, xe, r), dy = ramp_step(ys, ye, r);
    int32_t x = xs, y = ys;
    for (int k = 0; k < n; k++) {
        TABLE_CHECK(x >= 0 && x <= XMAX && y >= 0 && y <= YMAX);
        CHECK_CORNER(m, corner(tab, lg, x, y) + j);
        line[k] = corner_mix(corner(tab, lg, x, y) + j, L, (x >> 1) & 0x7fff, (y >> 1) & 0x7fff, amp);
        j = (j + 1u) & mask;
        if ((unsigned)(k - 1) < (unsigned)ramp) {            /* entries 1 .. ramp + 1 walk the ramp */
            x += dx;
            y += dy;
        }
    }
    return n;
}

/* Catmull-Rom read of a scratch line at a Q16 position: Q15 weights on the differences to the centre tap. The
 * line's values stay within 13,421 (16,000 x the largest velocity gain), so the differences fit 16 bits and the
 * result (at most 1.25 x that, the kernel's overshoot) does too. */
ALWAYS_INLINE int32_t hermite(const int16_t *line, int32_t pos)
{
    const int16_t *p = line + (pos >> 16);
    int32_t f = (pos & 0xffff) >> 1;
    int32_t f2 = mulfr16(f, f);
    int32_t f3 = mulfr16(f2, f);
    int32_t wm = (2 * f2 - f3 - f) >> 1;
    int32_t w1 = (4 * f2 + f - 3 * f3) >> 1;
    int32_t w2 = (f3 - f2) >> 1;
    if (w1 > 32767) w1 = 32767;                            /* 32768 just below f = 1 */
    int32_t x0 = p[0];
    int32_t acc = opz_add_q31(opz_add_q31(mulfr(wm, p[-1] - x0), mulfr(w1, p[1] - x0)), mulfr(w2, p[2] - x0));
    return x0 + (acc >> 16);
}

/* The envelope for the block, one Q15 gain per sample. It is stepped every 4 samples with the host's per-sample
 * rule run at 4x the rate (coef^4, 4 x step: exact in decay and release, where step is 0) and interpolated linearly
 * in between. Returns the level after the block. */
static int32_t envelope(int16_t *g, int32_t level, int32_t c4, int32_t s4)
{
    for (int q = 0; q < N; q += 4) {
        int32_t l4 = s4 ? opz_env_tick(level, c4, s4) : opz_env_tick0(level, c4);
        int32_t dl = (l4 - level) >> 2;
        g[q] = (int16_t)(level >> 16);
        g[q + 1] = (int16_t)((level + dl) >> 16);
        g[q + 2] = (int16_t)((level + 2 * dl) >> 16);
        g[q + 3] = (int16_t)((level + 3 * dl) >> 16);
        level = l4;
    }
    return level;
}

/* Read positions in a scratch line for the block's samples: pos, then + (each sample's increment >> s16). */
static void positions(int32_t *pp, int32_t pos, uint32_t inc, int32_t slope, int s16)
{
    for (int i = 0; i < N; i++) {
        pp[i] = pos;
        pos += (int32_t)(inc >> s16);
        inc += (uint32_t)slope;
    }
}

/* The block's samples: a line read at the positions, times the gains, added into the mono block buffer x. */
NOINLINE void kernel(fract32 *x, const int16_t *line, const int32_t *pp, const int16_t *g)
{
    for (int i = 0; i < N; i++) {
        fract32 y = mulfr(hermite(line, pp[i]), g[i]);
        x[i] = opz_add_q31(x[i], y);
    }
}

/* ---- the additive path ------------------------------------------------------------------------------------
 * A v2 holds two Q15 lanes, hi and lo: a harmonic's sine and cosine, or its coefficients b_h and a_h. On the Blackfin
 * every helper below is one vector instruction (vmul truncates: floor(a b / 2^15), -1 x -1 gives 0x7fff; vadds, vsubs,
 * vabs, vsum and subs16 saturate, vadd wraps); the host versions give the same bits (tests/add_check.c). */
#if defined(__bfin__)
typedef short v2 __attribute__((vector_size(4)));
ALWAYS_INLINE v2 v2_of(uint32_t w) { return (v2)w; }
ALWAYS_INLINE v2 vmul(v2 a, v2 b) { return __builtin_bfin_mult_fr2x16(a, b); }
ALWAYS_INLINE v2 vadds(v2 a, v2 b) { return __builtin_bfin_add_fr2x16(a, b); }
ALWAYS_INLINE v2 vsubs(v2 a, v2 b) { return __builtin_bfin_sub_fr2x16(a, b); }
ALWAYS_INLINE v2 vadd(v2 a, v2 b) { return a + b; }
ALWAYS_INLINE v2 vabs(v2 a) { return __builtin_bfin_abs_fr2x16(a); }
ALWAYS_INLINE v2 vdup_lo(v2 a) { v2 r; __asm__("%0 = PACK (%h1, %h1);" : "=d"(r) : "d"(a)); return r; }
ALWAYS_INLINE v2 vdup_hi(v2 a) { v2 r; __asm__("%0 = PACK (%d1, %d1);" : "=d"(r) : "d"(a)); return r; }
ALWAYS_INLINE int32_t vsum(v2 a) { return __builtin_bfin_sum_fr2x16(a); }   /* hi + lo */
ALWAYS_INLINE int32_t subs16(int32_t a, int32_t b) { return __builtin_bfin_sub_fr1x16((fract16)a, (fract16)b); }
/* GCC builds every constant word from two immediates where it is used; behind HIDE the kernel loads them instead
 * (mostly in a parallel slot). The kernel's harmonics run as one straight sequence, specialised for held and for
 * ramping coefficients. */
#define HIDE(p) __asm__("" : "+a"(p))
#define UNROLL_ADD _Pragma("GCC unroll 8")
#define ROLLED
#define ADD_KERNEL ALWAYS_INLINE
#else
typedef uint32_t v2;
static inline int32_t sat16(int32_t v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static inline int32_t hi16(v2 a) { return (int16_t)(a >> 16); }
static inline int32_t lo16(v2 a) { return (int16_t)a; }
static inline v2 pack(int32_t hi, int32_t lo) { return ((uint32_t)hi << 16) | ((uint32_t)lo & 0xffffu); }
static inline int32_t abs16(int32_t a) { return sat16(a < 0 ? -a : a); }
static inline v2 v2_of(uint32_t w) { return w; }
static inline v2 vmul(v2 a, v2 b) { return pack(mulfr16(hi16(a), hi16(b)), mulfr16(lo16(a), lo16(b))); }
static inline v2 vadds(v2 a, v2 b) { return pack(sat16(hi16(a) + hi16(b)), sat16(lo16(a) + lo16(b))); }
static inline v2 vsubs(v2 a, v2 b) { return pack(sat16(hi16(a) - hi16(b)), sat16(lo16(a) - lo16(b))); }
static inline v2 vadd(v2 a, v2 b) { return pack(hi16(a) + hi16(b), lo16(a) + lo16(b)); }
static inline v2 vabs(v2 a) { return pack(abs16(hi16(a)), abs16(lo16(a))); }
static inline v2 vdup_lo(v2 a) { return pack(lo16(a), lo16(a)); }
static inline v2 vdup_hi(v2 a) { return pack(hi16(a), hi16(a)); }
static inline int32_t vsum(v2 a) { return sat16(hi16(a) + lo16(a)); }
static inline int32_t subs16(int32_t a, int32_t b) { return sat16((int16_t)a - (int16_t)b); }
/* The host's lane helpers are several instructions each: with the loops rolled and one kernel for both cases, the
 * host build keeps opzsim's estimate of the image (its code is larger than the Blackfin's) inside the 50 KB window. */
#define HIDE(p) ((void)(p))
#define UNROLL_ADD _Pragma("clang loop unroll(disable)")
#define ROLLED _Pragma("clang loop unroll(disable)")
#define ADD_KERNEL NOINLINE
#endif
#define V2(hi, lo) (((uint32_t)(uint16_t)(hi) << 16) | (uint16_t)(lo))

/* The additive path's constants: the phase offsets of the two lanes, the triangle's offset, the sine polynomial's
 * coefficients (sincos) and the cosine of 0 for the double angle (z(0) = (0, 1): -32768 as the wrap of 32768). */
enum { K_OFFS, K_QUART, K_S7, K_S5, K_S3, K_S1, K_ONE, K_N };
static const uint32_t k_add[K_N] = { V2(16384, -32768), V2(-16384, -16384), V2(-146, -146), V2(2607, 2607),
                                     V2(-21163, -21163), V2(18703, 18703), V2(0, -32768) };

/* (sin, cos) of a Q32 phase, Q15 in the hi and lo lanes. Both lanes fold the phase into a triangle x in [-1, 1)
 * (sin: |phase + 1/4| - 1/4, cos: |phase + 1/2| - 1/4, in turns, times 4) and evaluate sin(pi x / 2) as the odd
 * 7th-order polynomial x + x r(x^2), r(0) = pi/2 - 1 (fitted to this arithmetic: within 2.3 LSB, smooth, 32,767 at
 * the top, so its error only shades the harmonics; the higher ones get the cosine's error near 0 and 1/2 turn h^2
 * times, which is why it is 7th-order: a 5th-order one would put images 63 dB down above 11 kHz). 16 bits of phase. */
ALWAYS_INLINE v2 sincos(uint32_t ph, const uint32_t *k)
{
    v2 u = vadd(vdup_hi(v2_of(ph)), v2_of(k[K_OFFS]));       /* (phase + 1/4, phase + 1/2), 16 bits */
    v2 x = vadd(vabs(u), v2_of(k[K_QUART]));
    x = vadd(x, x);
    v2 x2 = vmul(x, x);
    v2 r = vadds(vmul(x2, v2_of(k[K_S7])), v2_of(k[K_S5]));
    r = vadds(vmul(x2, r), v2_of(k[K_S3]));
    r = vadds(vmul(x2, r), v2_of(k[K_S1]));
    return vadds(x, vmul(x, r));
}

/* Each harmonic's gain for phase increment binc: the velocity gain amp times the band gain, 0 above the band. */
static void add_gains(int32_t *g, int32_t amp, uint32_t binc)
{
    int32_t inc4 = (int32_t)(binc >> 4);
    ROLLED
    for (int h = 1; h <= TW_ADD_H; h++) {
        int32_t band = (BAND_HI - h * inc4) >> 9;
        g[h - 1] = band >= 32767 ? amp : band > 0 ? mulfr16(amp, band) : 0;
    }
}

/* The coefficients of harmonics 1 .. TW_ADD_H at morph position (x, y) into co (word h - 1): the four corners' (a_h,
 * b_h), bilinear, times the harmonic's gain. */
NOINLINE void add_coefs(uint32_t *co, int32_t x, int32_t y, const int32_t *gain)
{
    int row = (y >> 16) * TW_WAVES + (x >> 16);
    TABLE_CHECK(row >= 0 && row + TW_WAVES + 1 < TW_BANKS * TW_WAVES);
    const uint32_t *p = k_tw_add[row];
    int32_t fx = (x >> 1) & 0x7fff, fy = (y >> 1) & 0x7fff;
    ROLLED
    for (int k = 0; k < TW_ADD_H; k++) {
        int32_t g = gain[k];
        uint32_t w = 0;
        if (g != 0) {
            const uint32_t *q = p + k;
            uint32_t c00 = q[0], c01 = q[TW_ADD_H], c10 = q[TW_WAVES * TW_ADD_H], c11 = q[(TW_WAVES + 1) * TW_ADD_H];
            int32_t lo = mulfr16(bilerp((int16_t)c00, (int16_t)c01, (int16_t)c10, (int16_t)c11, fx, fy), g);
            int32_t hi = mulfr16(bilerp((int16_t)(c00 >> 16), (int16_t)(c01 >> 16), (int16_t)(c10 >> 16),
                                        (int16_t)(c11 >> 16), fx, fy), g);
            w = V2(hi, lo);
        }
        co[k] = w;
    }
}

/* The block's coefficients: at its end (xe, ye, band of pitch be) into s_co[0] and, when cont (the last block played
 * the additive path too, so s->co holds its end, which is this block's start) and they moved, end - start into
 * s_co[1]. A block whose morph and band stayed put blends nothing. Returns whether the block must ramp. */
NOINLINE int add_setup(State *s, int32_t xe, int32_t ye, uint32_t be, int cont)
{
    uint32_t *ce = s_co[0], *cd = s_co[1];
    uint32_t key = TW_ADD_H * (int32_t)(be >> 4) <= BAND_FULL ? 0 : be;
    if (cont && xe == s->x && ye == s->y && key == s->bkey) {
        ROLLED
        for (int k = 0; k < TW_ADD_H; k++) ce[k] = s->co[k];
        return 0;
    }
    int32_t g[TW_ADD_H];
    add_gains(g, s->amp, be);
    add_coefs(ce, xe, ye, g);
    uint32_t moved = 0;
    ROLLED
    for (int k = 0; k < TW_ADD_H; k++) {                     /* the lanes' differences fit: |a_h|, |b_h| < 16384 */
        uint32_t e = ce[k], st = s->co[k];
        cd[k] = V2((int16_t)(e >> 16) - (int16_t)(st >> 16), (int16_t)e - (int16_t)st);
        moved |= cd[k];
        s->co[k] = e;
    }
    s->bkey = key;
    return cont && moved != 0;
}

/* The additive path's samples, times the gains, added into the mono buffer x. Per sample: sine and cosine of the
 * phase, z(2) from the double angle, z(h + 1) = 2 cos z(h) - z(h - 1) for the others (as q + (q - z(h - 1)) with
 * q = cos z(h): the difference is -sin(h t) sin(t) and never saturates), each z(h) lane-wise times its word (b_h,
 * a_h): the sine terms add up in hi, the cosine terms in lo. All TW_ADD_H harmonics every time (the words above
 * the band are 0): a straight run of vector instructions, no inner loop. With ramp, the coefficients move over the
 * block: sample i plays end - (63 - i) / 64 (end - start), so the last sample is at the end and the first one step
 * past the last block's last. */
ADD_KERNEL void additive(fract32 *x, uint32_t ph, uint32_t inc, int32_t slope, const int16_t *g, int ramp)
{
    const uint32_t *ce = s_co[0], *cd = s_co[1], *k = k_add;
    HIDE(k);
    for (int i = 0; i < N; i++) {
        v2 z = sincos(ph, k), c = vdup_lo(z), q = vmul(z, c), zp = z;
        v2 acc = vmul(z, v2_of(ce[0])), accd = vmul(z, v2_of(cd[0]));
        z = vadds(q, vadd(q, v2_of(k[K_ONE])));                  /* (2 sin cos, 2 cos^2 - 1) */
        acc = vadds(acc, vmul(z, v2_of(ce[1])));
        if (ramp) accd = vadds(accd, vmul(z, v2_of(cd[1])));
        UNROLL_ADD
        for (int h = 3; h <= TW_ADD_H; h++) {
            q = vmul(z, c);
            v2 zn = vadds(q, vsubs(q, zp));
            zp = z;
            z = zn;
            acc = vadds(acc, vmul(z, v2_of(ce[h - 1])));
            if (ramp) accd = vadds(accd, vmul(z, v2_of(cd[h - 1])));
        }
        int32_t v = vsum(acc);
        if (ramp) v = subs16(v, mulfr16(vsum(accd), (N - 1 - i) * (32768 / N)));
        fract32 y = mulfr(v, g[i]);
        x[i] = opz_add_q31(x[i], y);
        ph += inc;
        inc += (uint32_t)slope;
    }
}

NOINLINE void add_static(fract32 *x, uint32_t ph, uint32_t inc, int32_t slope, const int16_t *g)
{
    additive(x, ph, inc, slope, g, 0);
}

NOINLINE void add_ramp(fract32 *x, uint32_t ph, uint32_t inc, int32_t slope, const int16_t *g)
{
    additive(x, ph, inc, slope, g, 1);
}

/* One path of the block, added into the mono block buffer x with the gains g: the additive path (m == MIP_ADD;
 * ramp: its coefficients move) or mip m's scratch line k, read along the block's phase. */
ALWAYS_INLINE void play(fract32 *x, int m, int k, const Span *sp, uint32_t ph, uint32_t inc, int32_t slope, int ramp,
                        const int16_t *g)
{
    if (m == MIP_ADD) {
        if (ramp) add_ramp(x, ph, inc, slope, g);
        else add_static(x, ph, inc, slope, g);
        return;
    }
    positions(s_pos[k], sp->pos, inc, slope, sp->s16);
    for (int i = 0; i < N; i++) TABLE_CHECK(s_pos[k][i] >= 65536 && (s_pos[k][i] >> 16) + 2 < sp->n);
    kernel(x, s_line[k], s_pos[k], g);
}

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    s_recip[0] = 32768;
    for (uint32_t m = 1; m < SPAN; m++) {                    /* floor(32768 / m), long division (no libgcc) */
        uint32_t q = 0, r = 32768;
        ROLLED
        for (int b = 15; b >= 0; b--) {
            if ((r >> b) >= m) {
                r -= m << b;
                q |= 1u << b;
            }
        }
        s_recip[m] = (uint16_t)q;
    }
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    ((State *)state)->mip = -1;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note;                                              /* the knobs are read in render */
    State *s = state;
    int v = velocity > 127 ? 127 : velocity;
    s->ph = 0;
    s->mip = -1;
    s->xv = (v - 100) * VEL_X;
    s->amp = opz_mul_q31((fract32)((uint32_t)v * 0x01020408u), OUT_AMP);
    if (params) opz_mix_start(&s->mix, params);              /* NULL (engine-abi.md §3): ramps in at render */
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                 /* the host releases the envelope */
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* ---- per block -------------------------------------------------------------------------------------- */
    uint32_t inc0 = pitch > INC_MAX ? INC_MAX : pitch;
    uint32_t inc1 = pitch + (uint32_t)pitchSlope * N;
    if (inc1 > INC_MAX) inc1 = INC_MAX;
    int32_t slope = (int32_t)(inc1 - inc0) >> 6;             /* inc0 + i slope stays between inc0 and inc1 */
    uint32_t inc_max = inc0 > inc1 ? inc0 : inc1;
    int m = 0;                                               /* m == MIP_ADD: above the last mip */
    while (m < TW_MIPS && inc_max > k_tw_inc[m]) m++;

    int32_t xe = clampi(opz_p16(params[OPZ_P_P1]) * (TW_WAVES - 1) + s->xv, 0, XMAX);
    int32_t ye = clampi(opz_p16(params[OPZ_P_P2]) * (TW_BANKS - 1), 0, YMAX);
    int fresh = s->mip < 0;
    int32_t xs = fresh ? xe : s->x, ys = fresh ? ye : s->y;

    /* the new mip (or the additive path) and, in a crossfade, the old one: lines 0 and 1 */
    Span sa, sb;
    int32_t amp = s->amp;
    int mb = s->mip, ramp = 0;
    int xf = !fresh && (mb == m - 1 || mb == m + 1);
    if (m == MIP_ADD || (xf && mb == MIP_ADD)) ramp = add_setup(s, xe, ye, inc_max, mb == MIP_ADD);
    if (m != MIP_ADD && !blend(s_line[0], &sa, m, s->ph, inc_max, xs, xe, ys, ye, amp)) {
        inc0 = inc_max = 0;                                  /* cannot happen (m was chosen for inc_max <= INC_MAX); */
        slope = 0;                                           /* if it did, hold the phase for one block */
        blend(s_line[0], &sa, m, s->ph, 0, xs, xe, ys, ye, amp);
    }
    if (xf && mb != MIP_ADD)
        xf = blend(s_line[1], &sb, mb, s->ph, inc_max, xs, xe, ys, ye, amp) != 0;

    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    int32_t c2 = opz_mul_q31_rnd(coef, coef);
    int32_t c4 = opz_mul_q31_rnd(c2, c2), s4 = opz_shl_q31(step, 2);
    OPZ_ADSR_LEVEL(env) = envelope(s_gain[0], level, c4, s4);
    fract32 mono[N];                                         /* the voice, before level and pan: both paths add here */
    for (int i = 0; i < N; i++) mono[i] = 0;
    if (xf) {                                                /* old -> new over the block */
        for (int i = 0; i < N; i++) {
            int32_t gn = mulfr16(s_gain[0][i], i * 512 + 256);
            s_gain[1][i] = (int16_t)(s_gain[0][i] - gn);
            s_gain[0][i] = (int16_t)gn;
        }
        play(mono, mb, 1, &sb, s->ph, inc0, slope, ramp, s_gain[1]);
    }
    play(mono, m, 0, &sa, s->ph, inc0, slope, ramp, s_gain[0]);
    opz_mix_mono(out, mono, &s->mix, params);               /* level and pan, added into interleaved out */

    s->ph += inc0 * N + (uint32_t)slope * (N * (N - 1) / 2);
    s->x = xe;
    s->y = ye;
    s->mip = m;
}

OPZ_EXPORTS_INSTRUMENT();
