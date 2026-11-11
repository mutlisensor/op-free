/*
 * opz_dsp.h — shared fixed-point DSP kernels for OP-Z engines.
 *
 * Integer only (the Blackfin has no FPU), no libc, header-only so each engine links just what it uses.
 * Conventions: audio and gains are Q31 (fract32), control values are Q16 unless named otherwise, phases are
 * uint32_t turns (Q32). 64-bit divisions pull ~1.2 KB of libgcc into an engine and cost ~800 Blackfin cycles each:
 * keep them per note, and use opz_recip_q31 per block. Everything meant for per-sample use is 32-bit maths that
 * maps to single Blackfin instructions (no int64, no libgcc calls); research/cycle-model.md has the costs.
 * MIT licence, see CONTRIBUTING.md.
 */
#ifndef OPZ_DSP_H
#define OPZ_DSP_H

#include "opz_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPZ_Q31_ONE 0x7fffffff
#define OPZ_FS 44100

#if defined(__bfin__)
static inline int32_t opz_abs32(int32_t x) { return __builtin_bfin_abs_fr1x32(x); }
#else
static inline int32_t opz_abs32(int32_t x) { return x < 0 ? (x == INT32_MIN ? INT32_MAX : -x) : x; }
#endif

/* Q31 host parameter -> 0..65535 */
static inline int32_t opz_p16(uint32_t param) { return (int32_t)(param >> 15); }

/* Q31 host parameter -> 0..OPZ_Q31_ONE */
static inline int32_t opz_pq31(uint32_t param) { return (int32_t)(param & OPZ_Q31_ONE); }

/* ---- 32-bit arithmetic without libgcc ------------------------------------------------------------- */

/* (a * b) >> 16 of two unsigned 32-bit values, truncated to 32 bits, from four 16 x 16 products: the same bits as
 * (uint32_t)(((uint64_t)a * b) >> 16) without a libgcc call. */
static inline uint32_t opz_mul_u32_shr16(uint32_t a, uint32_t b)
{
    uint32_t al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
    return (opz_mul_u16(ah, bh) << 16) + opz_mul_u16(ah, bl) + opz_mul_u16(al, bh) + (opz_mul_u16(al, bl) >> 16);
}

/* 1 / (2 m) in Q31 for m in [0.5, 1) (Q31, i.e. 2^30 <= m < 2^31): a linear first guess (error 1/17) and three
 * Newton steps, seven multiplies and no division. Within 2 LSB of 2^61 / m (m = 0.5 gives OPZ_Q31_ONE). */
static inline fract32 opz_recip_q31(int32_t m)
{
    fract32 h = 884257973 + (0x7fffffff - opz_mul_q31(2021161080, m));     /* 24/17 - 16/17 m */
    for (int i = 0; i < 3; i++) {
        int32_t e = (int32_t)(0x80000000u - ((uint32_t)opz_mul_q31(m, h) << 1));   /* 1 - 2 m h, small */
        h = opz_add_q31(h, opz_mul_q31(h, e));
    }
    return h;
}

/* floor(n / d) for n < d * 2^32 (the quotient fits 32 bits): a restoring shift-and-subtract division, the same
 * result as the 64-bit C division in ~350 cycles and ~80 bytes, instead of libgcc's ___divdi3 (1.4 KB with its
 * table). For per-note work. */
__attribute__((noinline, unused)) static uint32_t opz_udiv64_32(uint64_t n, uint32_t d)
{
    uint32_t hi = (uint32_t)(n >> 32), lo = (uint32_t)n, q = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t carry = hi >> 31;
        hi = (hi << 1) | (lo >> 31);
        lo <<= 1;
        q <<= 1;
        if (carry || hi >= d) {
            hi -= d;
            q |= 1;
        }
    }
    return q;
}

/* 2^(x / 65536) in Q16, x in [-15, 14] octaves. Cubic on the fraction, max error ~0.01 %. Every partial result
 * fits 16 bits unsigned (at most 19948, then 65533), so three 16 x 16 multiplies give the exact Horner values. */
static inline int32_t opz_exp2_q16(int32_t x)
{
    int32_t i = x >> 16;
    uint32_t f = (uint32_t)x & 0xffff;
    uint32_t p = 5125;
    p = (opz_mul_u16(p, f) >> 16) + 14824;
    p = (opz_mul_u16(p, f) >> 16) + 45586;
    p = (opz_mul_u16(p, f) >> 16) + 65536;
    return i >= 0 ? (int32_t)(p << i) : (int32_t)(p >> -i);
}

/* Note number -> frequency in Hz, Q16 (A4 = 69 = 440 Hz). */
static inline int32_t opz_note_hz_q16(int32_t note)
{
    return (int32_t)((int64_t)440 * opz_exp2_q16((int32_t)(((int64_t)(note - 69) << 16) / 12)));
}

/* Hz in Q16 (>= 0) -> phase increment (Q32 turns per sample at 44.1 kHz). */
static inline uint32_t opz_hz_to_inc(int32_t hz_q16) { return opz_mul_u32_shr16((uint32_t)hz_q16, 97391); }

/* Triangle from a phase, -1..1 in Q31 (0 at phase 0, +1 at 1/4 turn): 2 |x| - 1, saturated at +1. */
static inline fract32 opz_tri(uint32_t phase)
{
    int32_t x = (int32_t)(phase + 0x40000000u);          /* shift so phase 0 -> 0 */
    uint32_t m = (uint32_t)(x >> 31);
    uint32_t a = ((uint32_t)x ^ m) - m;                    /* |x|, 0..2^31, without a branch */
    return opz_shl_q31((int32_t)(a - 0x40000000u), 1);
}

/* Triangle LFO in Q15, -1..1, from a phase (0 at the trough). */
static inline int32_t opz_tri15(uint32_t phase)
{
    int32_t x = (int32_t)(phase >> 16);
    return x < 32768 ? x * 2 - 32768 : 98303 - x * 2;
}

/* sin(2 pi phase) in Q31: parabola plus one correction step, ~0.1 % error. */
static inline fract32 opz_sine(uint32_t phase)
{
    int32_t x = (int32_t)phase;                            /* -1..1 for -pi..pi */
    int32_t y = opz_mul_q31(x, OPZ_Q31_ONE - opz_abs32(x));
    y = opz_shl_q31(y, 2);
    int32_t c = opz_mul_q31(y, opz_abs32(y)) - y;
    return opz_add_q31(y, opz_mul_q31(0x1ccccccd, c));
}

/* Gentle odd saturation x - x^3/4 for feedback paths. */
static inline fract32 opz_soft(fract32 x)
{
    fract32 x3 = opz_mul_q31(opz_mul_q31(x, x), x);
    return opz_sub_q31(x, x3 >> 2);
}

/* Unity-at-full-scale soft clip 1.5x - 0.5x^3 (input clamped to +-1). x3 has the sign of x, so x - x3 / 2 cannot
 * overflow and only the last add saturates: the same bits as one saturation of the three-term sum. */
static inline fract32 opz_clip(fract32 x)
{
    fract32 x3 = opz_mul_q31(opz_mul_q31(x, x), x);
    return opz_add_q31(opz_sub_q31(x, x3 >> 1), x >> 1);
}

static inline fract32 opz_lerp(fract32 a, fract32 b, fract32 w) { return opz_add_q31(a, opz_mul_q31(w, opz_sub_q31(b, a))); }


/* ---- band-limited sawtooth (PolyBLEP) ------------------------------------------------------------- */

/* 2^31 / inc in Q16 (2^47 / inc, for inc >= 2^16), for opz_saw_blep. No division: opz_recip_q31, per block.
 * Within ~1e-9 of the exact quotient, which moves the BLEP correction by far less than one LSB of 16-bit audio. */
static inline uint32_t opz_blep_recip(uint32_t inc)
{
    if (inc < 0x10000) inc = 0x10000;
    if (inc > 0x7fffffff) inc = 0x7fffffff;
    int n = opz_norm32((int32_t)inc);                        /* 0..14: inc << n in [2^30, 2^31) */
    return (uint32_t)opz_recip_q31((int32_t)(inc << n)) >> (14 - n);
}

/* The polynomial BLEP residual around a wrap, to subtract from the naive saw (the rare case, out of line: inline,
 * GCC reaches it through a long jump and a predicted-taken branch on every sample). */
__attribute__((noinline, unused)) static int32_t opz__blep_edge(uint32_t phase, uint32_t inc, uint32_t recip)
{
    if (phase < inc) {                                       /* just after the wrap */
        int32_t x = (int32_t)opz_mul_u32_shr16(phase, recip);              /* t / dt, Q31, below 2^31 */
        return (int32_t)(2u * (uint32_t)x - 0x7fffffffu) - opz_mul_q31(x, x);   /* 2x - x^2 - 1, in -1..0 */
    }
    int32_t x = (int32_t)opz_mul_u32_shr16(~phase, recip);              /* just before: (1 - t) / dt */
    return (int32_t)(0x7fffffffu - 2u * (uint32_t)x) + opz_mul_q31(x, x);       /* x^2 - 2x + 1, in 0..1 */
}

/* Naive saw 2t - 1 minus a two-sample polynomial BLEP around the wrap. inc < 2^31 (below fs/2). One unsigned
 * compare finds both sides of the wrap (phase < inc, or phase > ~inc). */
static inline fract32 opz_saw_blep(uint32_t phase, uint32_t inc, uint32_t recip)
{
    int32_t y = (int32_t)(phase ^ 0x80000000u);             /* 2t - 1 in Q31 */
    if (__builtin_expect(phase + inc < 2u * inc, 0)) y = opz_sub_q31(y, opz__blep_edge(phase, inc, recip));
    return y;
}

/* ---- one-pole and state-variable filters ----------------------------------------------------------- */

/* Decay coefficient for a one-pole: exp(-1/N) ~ 1 - 1/N for a time constant of N samples, Q31. Per note.
 * OPZ_Q31_ONE / N exactly (N up to 2^28), from opz_recip_q31's estimate and a remainder check instead of a
 * division. */
__attribute__((noinline, unused)) static int32_t opz_decay_coef(int32_t samples)
{
    if (samples < 2) samples = 2;
    int n = opz_norm32(samples);                             /* samples << n in [2^30, 2^31) */
    int32_t q = (int32_t)((uint32_t)opz_recip_q31(samples << n) >> (30 - n));   /* ~2^31 / samples */
    int32_t r = (int32_t)(0x7fffffffu - (uint32_t)q * (uint32_t)samples);   /* q is within a few of the quotient */
    while (r < 0) { q--; r += samples; }
    while (r >= samples) { q++; r -= samples; }
    return OPZ_Q31_ONE - q;
}

/* Zero-delay-feedback (TPT) state-variable filter. Coefficients a1..a3 in Q31 (all below 1), k in Q30, states
 * in Q31. */
typedef struct OpzSvf {
    int32_t ic1, ic2;
} OpzSvf;

typedef struct OpzSvfCoef {
    int32_t a1, a2, a3;                                      /* Q31 */
    int32_t k;                                               /* Q30 */
} OpzSvfCoef;

/* fc in Hz Q16 (clamped 20 Hz..16 kHz), damping k = 2 - 2 * res in Q30 (res 0..~0.98). Two divisions: per note. */
static inline OpzSvfCoef opz_svf_coef(int32_t fc_q16, int32_t k_q30)
{
    if (fc_q16 < (20 << 16)) fc_q16 = 20 << 16;
    if (fc_q16 > (16000 << 16)) fc_q16 = 16000 << 16;
    int64_t w = ((int64_t)fc_q16 * 4781) >> 14;              /* pi fc / fs in Q28 */
    int64_t w2 = (w * w) >> 28;
    int64_t g = opz_udiv64_32((uint64_t)(w * ((15LL << 28) - w2)),
                              (uint32_t)((15LL << 28) - 6 * w2));          /* tan(w), Pade [3/2], Q28 */
    int64_t k = (int64_t)k_q30 >> 2;                         /* Q28 */
    int64_t den = (1LL << 28) + ((g * (g + k)) >> 28);       /* Q28 */
    OpzSvfCoef c;
    c.a1 = (int32_t)opz_udiv64_32(1ULL << 59, (uint32_t)den);  /* Q31: den > 1, so a1 < 1 */
    c.a2 = (int32_t)((g * c.a1) >> 28);                      /* g / den < 1/2 */
    c.a3 = (int32_t)((g * c.a2) >> 28);                      /* g^2 / den < 1 */
    c.k = k_q30;
    return c;
}

/* One sample; writes low-, band- and high-pass. Keep inputs at about half scale for resonance headroom. Five
 * fractional multiplies; each product is rounded down on its own (within an LSB of a 64-bit sum). 2 v - ic is
 * v + (v - ic), which saturates exactly like the 64-bit form. */
static inline void opz_svf(OpzSvf *s, const OpzSvfCoef *c, fract32 x, fract32 *lp, fract32 *bp, fract32 *hp)
{
    int32_t ic1 = s->ic1, ic2 = s->ic2;
    int32_t v3 = opz_sub_q31(x, ic2);
    int32_t v1 = opz_add_q31(opz_mul_q31(c->a1, ic1), opz_mul_q31(c->a2, v3));
    int32_t v2 = opz_add_q31(ic2, opz_add_q31(opz_mul_q31(c->a2, ic1), opz_mul_q31(c->a3, v3)));
    s->ic1 = opz_add_q31(v1, opz_sub_q31(v1, ic1));
    s->ic2 = opz_add_q31(v2, opz_sub_q31(v2, ic2));
    if (lp) *lp = v2;
    if (bp) *bp = v1;
    if (hp) *hp = opz_sub_q31(opz_sub_q31(x, opz_shl_q31(opz_mul_q31(c->k, v1), 1)), v2);
}

/* ---- noise ------------------------------------------------------------------------------------------ */

static inline fract32 opz_noise(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (fract32)*seed;
}

/* ---- effect output: dry / wet and the output limiter ------------------------------------------------------ */

/* Dry and wet gains (Q31) from an FX track's pan word (params[OPZ_P_PAN]): the mix law every effect here follows.
 * 128 .. 255 = the effect only, no dry, so the OS's own dry level (FX byte 12, the manual's "dry mix") alone
 * decides how much dry is heard, and a track sent at 100 % passes fully through the effect. 0 .. 127 blends the
 * engine's own dry in, equal power (cos / sin of a quarter turn): 0 = dry only, 64 = -3 dB each, for a wet signal
 * that is unrelated to the dry (reverb, delay, grains, pitch or frequency shift, stutter); opz_fx_wet_linear below is
 * the same law for a wet signal that is the input reshaped. Presets cannot set
 * pan (the OS skips it when it applies one), so the centre is what most users have: it must be the full effect.
 * sdk/docs/engine-abi.md, renderFX. */
static inline void opz_fx_drywet(uint32_t pan_word, fract32 *dry, fract32 *wet)
{
    uint32_t p = pan_word & OPZ_Q31_ONE;                  /* 0x40000000 = pan byte 127.5 */
    if (p >= 0x40000000u) { *dry = 0; *wet = OPZ_Q31_ONE; return; }
    *wet = opz_sine(p);                                    /* sin(pi/2 x), x = p / 2^30 */
    *dry = opz_sine(0x40000000u - p);                      /* cos(pi/2 x) */
}

/* The same law for effects whose wet is the input itself reshaped (filter, compressor, saturation, phaser): a
 * linear crossfade with the same end points (0 = dry only, 128 .. 255 = the effect only), since an equal-power sum
 * of two near-identical signals rises by 3 dB in the middle. Returns the wet share; dry = 1 - wet. */
static inline fract32 opz_fx_wet_linear(uint32_t pan_word)
{
    uint32_t p = pan_word & OPZ_Q31_ONE;
    return p >= 0x40000000u ? OPZ_Q31_ONE : (fract32)(p * 2);
}

/* Output limiter for a signal held at half scale (h = x / 2, so a mix of dry and wet can reach 2 without
 * saturating): unity below -3 dBFS, then a quadratic knee that reaches 0 dBFS with zero slope at +2.2 dBFS and
 * stays there. Full scale in gives -0.7 dB; nothing above 0 dBFS ever comes out, and there is no hard corner below
 * +2.2 dBFS. A quiet sample costs an abs, a compare and an add (the knee is kept off the straight path). */
#define OPZ_LIMIT_T     0x5a9df7ac                        /* t = -3 dBFS = 0.70795 (Q31) */
#define OPZ_LIMIT_K     28050                             /* 2 / (1 - t) x 2^12: the knee in Q31 from a Q15 square */
static inline fract32 opz_limit_half(fract32 h)
{
    int32_t a = opz_abs32(h);
    if (__builtin_expect(a > (OPZ_LIMIT_T >> 1), 0)) {     /* the knee: rare, so off the straight path */
        int32_t v = (OPZ_Q31_ONE - OPZ_LIMIT_T) - (a - (OPZ_LIMIT_T >> 1));   /* (2 - t - |x|) / 2, <= 1 - t */
        int32_t vh = v > 0 ? v >> 16 : 0;                  /* Q15; 0 from |x| >= 2 - t: the ceiling */
        int32_t y = OPZ_Q31_ONE - ((vh * vh) >> 12) * OPZ_LIMIT_K;   /* 1 - (2 - t - |x|)^2 / (4 (1 - t)) */
        return h < 0 ? -y : y;                              /* 16-bit square: within 1e-4 of the exact knee */
    }
    return h + h;                                           /* |x| <= t: exact (|h| < 0.5, no overflow) */
}

/* opz_limit_half over a whole block in place (2 * OPZ_BLOCK_FRAMES values): for an engine whose sample loop is
 * already short of registers, write the half-scale sums to out and limit them here in a tight loop afterwards. */
static inline void opz_limit_half_block(fract32 *buf)
{
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) buf[i] = opz_limit_half(buf[i]);
}

/* The end of an effect's block: out holds the wet signal at half scale (written in the sample loop); this adds the
 * dry input with its gain / 2 ramping from dg0 to dg1 over the block (Q31, at most 0.5) and limits the sum in place
 * with opz_limit_half. Kept out of the sample loop, it costs a few cycles a sample, and at pan 128 .. 255 (dry gain 0)
 * the dry costs nothing. */
static inline void opz_fx_finish(fract32 *out, const fract32 *in, fract32 dg0, fract32 dg1)
{
    if (!dg0 && !dg1) { opz_limit_half_block(out); return; }
    int32_t g = dg0, inc = (dg1 - dg0) >> 6;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        g += inc;
        out[2 * i] = opz_limit_half(opz_add_q31(out[2 * i], opz_mul_q31(g, in[2 * i])));
        out[2 * i + 1] = opz_limit_half(opz_add_q31(out[2 * i + 1], opz_mul_q31(g, in[2 * i + 1])));
    }
}

/* One output sample: dry x and wet y with gains given at half scale (dg2 = dry x level / 2, wg2 = wet x level / 2,
 * as Q31), summed and limited. Each product stays within +-0.5, so the sum cannot saturate. */
static inline fract32 opz_fx_out(fract32 x, fract32 y, fract32 dg2, fract32 wg2)
{
    return opz_limit_half(opz_mul_q31(dg2, x) + opz_mul_q31(wg2, y));
}

#ifdef __cplusplus
}
#endif

#endif /* OPZ_DSP_H */
