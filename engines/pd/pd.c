/*
 * pd — phase-distortion voice in the Casio CZ tradition: a cosine read through a bent phase.
 *
 *   P1        distortion  0 = a pure sine on every wave .. 255 = the full wave. The sharpness s (how much faster
 *                         than the note the fastest part of the wave runs) grows exponentially, s = s_top^(P1/255),
 *                         where s_top is the wave's largest sharpness or, on high notes, the pitch limit (below)
 *   P2        wave        eight waves at 0, 36, 73, 109, 146, 182, 219, 255: saw, square, pulse, double sine,
 *                         saw-pulse, resonance I, II, III. Each holds for +-9 around its point; in between, the two
 *                         neighbours crossfade (both are computed, from the same phase), so turning P2 never clicks
 *   DCW                   per note, the distortion starts above P1 and decays to it (a CZ "DCW" envelope): the
 *                         depth is the velocity (127: from the full wave), the time follows the decay knob (15 ms at
 *                         0, x2 every 32 steps, 3.7 s at 255; shorter on higher notes: x0.5 every two octaves above
 *                         C4), and it holds while the amplitude envelope is in its attack
 *   velocity              level and DCW depth
 *
 * The waves (phase phi in turns, theta the bent phase, output cos(2 pi theta)):
 *   saw          the first half-cycle squeezed into d = 0.5 / s of the period, the second spread over the rest
 *   square       in each half period, a half-cycle in 1 / (2 s) of the period, then holding at -1 or +1
 *   pulse        one whole cycle in 1 / s of the period, then holding at +1 (a narrow pulse, DC removed)
 *   double sine  theta = phi + a (frac(2 phi) - phi): sine -> two dips per period -> the octave
 *   saw-pulse    the saw's fast half-cycle, the slow half-cycle in half a period, then holding at +1
 *   resonance    1 - W(phi) (1 - cos(2 pi k phi)), k = s: a cosine at k times the note, restarted every period
 *                under a window that ends each period at zero: I decaying (1 - phi)^2, II a bell (4 phi (1 - phi))^2,
 *                III flat then a smooth fall (smoothstep). The first quarter of P1 crossfades from the sine
 * Every bend in the saw, square, pulse and saw-pulse sits where the cosine is flat (theta a multiple of 1/2), so the
 * waves are smooth to the first derivative and their harmonics fall at 18 dB/octave above s x f0; the resonance
 * windows end with zero slope for the same reason.
 *
 * Largest sharpness: 128 (saw, square, saw-pulse), 12 (pulse), 32 (resonance k).
 * Anti-aliasing: s x f0 stays at or below 6 kHz (pulse 4 kHz, resonance k x f0 6 kHz), so the sharp waves get
 * gentler on high notes instead of aliasing; double sine's kink in the middle of the period gets a polyBLAMP
 * (a two-sample integrated polynomial BLEP). Every wave's mean is removed and its RMS matched to the sine's (peaks
 * at most 3 dB above), from tables made by gen_tables.py. All shape parameters ramp per sample across the block.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine; pd never reads them.
 * Inspired by the phase distortion of Casio's CZ synthesizers; the waves are our own definitions and the code is
 * written from scratch (no code from any synthesizer or library). MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "pd_tables.h"

#define N OPZ_BLOCK_FRAMES
#define ALWAYS_INLINE static inline __attribute__((always_inline))

enum { SAW, SQUARE, PULSE, DBL, SAWPULSE, RES1, RES2, RES3, NWAVES };

typedef struct State {
    uint32_t ph;             /* phase, Q32 turns */
    fract32 dcw;             /* DCW envelope, 1 at note-on, Q31 */
    int32_t depth;           /* DCW depth from velocity, Q15 */
    fract32 amp;             /* velocity x OPZ_VOICE_PEAK, Q31 */
    int32_t a_prev;          /* the distortion amount at the end of the last block, Q16 (0 .. 65536) */
    int32_t x_prev;          /* the P2 position at the end of the last block, Q16 (0 .. 7 waves) */
    int32_t fresh;           /* first block after note-on: no ramps from the last block */
} State;

/* The OS keeps its own fields after the first 280 bytes of the voice record (docs/STATUS.md) */
_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;
static int16_t s_y[3][N];    /* the block's waves (two, and the resonance waves' sine), Q15; rewritten every render */

/* One wave's shape parameters at one end of a block. */
typedef struct Shape {
    uint32_t knee;           /* saw, saw-pulse: 0.5 / s; square, pulse: 1 / s (Q32 turns); double sine: a (Q31) */
    int32_t sl;              /* s, Q23 (saw, square, pulse, saw-pulse); k, Q24 (resonance) */
    int32_t r;               /* saw: slope of the slow half, (1 - theta at the knee) / (1 - d), Q30 */
    int32_t dc;              /* the wave's mean, Q15 */
    int32_t gh;              /* gain / 2, Q15 (resonance: times its share against the sine) */
    int32_t sw;              /* resonance: the sine's share, Q15 (32768 = all sine); 0 for the other waves */
} Shape;

/* ---- small maths ---------------------------------------------------------------------------------------------- */

/* Signed 16 x 16 -> 32 multiply of the low halves: one Blackfin instruction. */
#if defined(__bfin__)
ALWAYS_INLINE int32_t mul_s16(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
#else
ALWAYS_INLINE int32_t mul_s16(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int16_t)b; }
#endif

/* cos(2 pi theta) in Q15 from the table, linearly interpolated on the next 15 bits. */
ALWAYS_INLINE int32_t cos15(uint32_t th)
{
    uint32_t i = th >> 23;
    int32_t f = (int32_t)((th >> 8) & 0x7fff);
    int32_t a = k_cos[i];
    return a + (mul_s16(k_cos[i + 1] - a, f) >> 15);
}

/* log2(x) in Q16 for x >= 1 (0 is taken as 1); within 0.0004 octave. Per block. */
static int32_t log2_q16(uint32_t x)
{
    int32_t y = (int32_t)(x >> 1);
    if (y == 0) return 0;
    int n = opz_norm32(y);                                   /* y << n in [2^30, 2^31) */
    uint32_t f = ((uint32_t)y << n) - 0x40000000u;           /* mantissa - 1, Q30 */
    uint32_t i = f >> 26, r = (f >> 10) & 0xffff;
    int32_t v = k_log2[i] + (int32_t)(((uint32_t)(k_log2[i + 1] - k_log2[i]) * r) >> 16);
    return ((31 - n) << 16) + v;
}

/* ---- the waves' parameters (per block) ------------------------------------------------------------------------ */

/* Shape of wave w at distortion a (Q16, 0 .. 65536) and pitch log2(inc) lg (Q16). */
static void shape(Shape *o, int w, int32_t a, int32_t lg)
{
    int32_t jq;                                               /* table position, Q16 */
    o->r = 0;
    o->sw = 0;
    if (w == DBL) {
        o->knee = a >= 65536 ? 0x7fffffffu : (uint32_t)a << 15;
        o->sl = 0;
        jq = a * (NJ - 1);                                    /* a / 28 per table point */
    } else {
        int32_t ls = k_limit[w][0], lim = k_limit[w][1] - lg;
        if (lim < ls) ls = lim;
        if (ls < 0) ls = 0;
        uint32_t l = ((uint32_t)a * (uint32_t)(ls >> 3)) >> 13;   /* log2 s, Q16, 0 .. 7 */
        jq = (int32_t)(l << 2);                                   /* a quarter octave per table point */
        int32_t s16 = opz_exp2_q16((int32_t)l), inv16 = opz_exp2_q16(-(int32_t)l);
        if (w >= RES1) {
            o->knee = 0;
            o->sl = s16 << 8;                                     /* k, Q24 (<= 32) */
            o->sw = a >= 16384 ? 0 : 32768 - (a << 1);            /* the sine crossfades out over P1 0 .. 64 */
        } else if (w == SQUARE || w == PULSE) {
            o->knee = inv16 >= 65536 ? 0xffffffffu : (uint32_t)inv16 << 16;    /* 1 / s */
            o->sl = s16 << 7;
        } else {
            if (inv16 > 65536) inv16 = 65536;
            o->knee = (uint32_t)inv16 << 15;                      /* d = 0.5 / s, <= 0.5 */
            o->sl = s16 << 7;
            if (w == SAW) {                                       /* the slow half ends at exactly one turn */
                uint32_t at = (o->knee >> 7) * (uint32_t)(o->sl >> 16);       /* theta at the knee, ~1/2 */
                fract32 rc = opz_recip_q31((int32_t)(0x80000000u - (o->knee >> 1)));   /* 1 / (2 (1 - d)) */
                o->r = opz_mul_q31((int32_t)((0u - at) >> 1), rc);            /* (1 - at) / (2 (1 - d)), Q31 */
            }
        }
    }
    int j = jq >> 16;
    int32_t f = (jq & 0xffff) >> 1;
    if (j >= NJ - 1) { j = NJ - 2; f = 32767; }
    const int16_t *p = k_level[w][j];
    o->dc = p[0] + ((p[2] - p[0]) * f >> 15);
    o->gh = p[1] + ((p[3] - p[1]) * f >> 15);
    if (o->sw) o->gh = (o->gh * (32768 - o->sw)) >> 15;
}

/* ---- the waves (per sample) ----------------------------------------------------------------------------------- */

/* Each pass renders one wave over the block into y (Q15) from the block's start phase, ramping its shape from a
 * to b. The shape parameters are the only state the loops carry; gains, weights, DC and envelope come later. */

#define RAMP_U(x, y) ((int32_t)(((y) >> 1) - ((x) >> 1)) >> 5)   /* per-sample step of a ramp of Q32 values */

__attribute__((noinline)) static void pass_saw(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope, const Shape *a,
                                               const Shape *b)
{
    uint32_t knee = a->knee;
    int32_t dk = RAMP_U(a->knee, b->knee), sl = a->sl, dsl = (b->sl - a->sl) >> 6, r = a->r, dr = (b->r - a->r) >> 6;
    for (int i = 0; i < N; i++) {
        uint32_t m = ph < knee ? ph : knee;                   /* fast half: theta = phi s */
        uint32_t th = (m >> 7) * (uint32_t)(sl >> 16) + ((ph - m) >> 15) * (uint32_t)(r >> 15);   /* + slow half */
        y[i] = (int16_t)cos15(th);
        ph += inc;
        inc += (uint32_t)slope;
        knee += (uint32_t)dk;
        sl += dsl;
        r += dr;
    }
}

__attribute__((noinline)) static void pass_square(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope,
                                                  const Shape *a, const Shape *b)
{
    uint32_t knee = a->knee;
    int32_t dk = RAMP_U(a->knee, b->knee), sl = a->sl, dsl = (b->sl - a->sl) >> 6;
    for (int i = 0; i < N; i++) {
        uint32_t u = ph << 1;                                 /* phase within the half period */
        uint32_t m = u < knee ? u : knee;
        y[i] = (int16_t)cos15((ph & 0x80000000u) + (m >> 8) * (uint32_t)(sl >> 16));
        ph += inc;
        inc += (uint32_t)slope;
        knee += (uint32_t)dk;
        sl += dsl;
    }
}

__attribute__((noinline)) static void pass_pulse(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope,
                                                 const Shape *a, const Shape *b)
{
    uint32_t knee = a->knee;
    int32_t dk = RAMP_U(a->knee, b->knee), sl = a->sl, dsl = (b->sl - a->sl) >> 6;
    for (int i = 0; i < N; i++) {
        uint32_t m = ph < knee ? ph : knee;
        y[i] = (int16_t)cos15((m >> 7) * (uint32_t)(sl >> 16));    /* a whole turn at the knee: wraps to 0 */
        ph += inc;
        inc += (uint32_t)slope;
        knee += (uint32_t)dk;
        sl += dsl;
    }
}

__attribute__((noinline)) static void pass_sawpulse(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope,
                                                    const Shape *a, const Shape *b)
{
    uint32_t knee = a->knee;
    int32_t dk = RAMP_U(a->knee, b->knee), sl = a->sl, dsl = (b->sl - a->sl) >> 6;
    for (int i = 0; i < N; i++) {
        uint32_t m = ph < knee ? ph : knee;
        uint32_t x = ph - m;                                  /* the slow half-cycle at the note's speed, then hold */
        y[i] = (int16_t)cos15((m >> 7) * (uint32_t)(sl >> 16) + (x < 0x80000000u ? x : 0x80000000u));
        ph += inc;
        inc += (uint32_t)slope;
        knee += (uint32_t)dk;
        sl += dsl;
    }
}

/* The polyBLAMP residual of double sine's kink (rare, out of line): rel is the phase from the kink, k the slope
 * change times inc / 6 (Q15). k (1 - |t|)^3 for t = rel / inc in samples, either side. */
__attribute__((noinline)) static int32_t dbl_blamp(uint32_t rel, uint32_t inc, uint32_t recip, int32_t k)
{
    uint32_t d = rel < inc ? rel : ~rel;
    uint32_t t = opz_mul_u32_shr16(d, recip);                /* |t|, Q31 */
    if (t > 0x7fffffffu) t = 0x7fffffffu;
    int32_t u = (int32_t)(0x7fffffffu - t);
    int32_t u3 = opz_mul_q31(opz_mul_q31(u, u), u);
    return opz_mul_q31(u3, k);
}

/* Double sine, at half level (the residual may take it past 1). */
__attribute__((noinline)) static void pass_dbl(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope, const Shape *a,
                                               const Shape *b, uint32_t recip, int32_t k)
{
    int32_t am = (int32_t)a->knee, dam = ((int32_t)b->knee - am) >> 6;
    for (int i = 0; i < N; i++) {
        uint32_t th = ph + (uint32_t)opz_mul_q31(am, (int32_t)((ph << 1) - ph));   /* phi + a (frac(2 phi) - phi) */
        int32_t v = cos15(th);
        uint32_t rel = ph ^ 0x80000000u;                      /* phase from the kink at 1/2 */
        if (__builtin_expect(rel + inc < 2u * inc, 0)) v += dbl_blamp(rel, inc, recip, k);
        v >>= 1;
        y[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        ph += inc;
        inc += (uint32_t)slope;
        am += dam;
    }
}

/* Resonance: 1 - W(phi) (1 - cos(2 pi k phi)) with window W (Q15). */
ALWAYS_INLINE int32_t window(int w, uint32_t ph)
{
    if (w == RES1) {
        uint32_t q = ~ph >> 16;                               /* 1 - phi, Q16 */
        return (int32_t)((q * q) >> 17);
    }
    if (w == RES2) {
        int32_t p = (int32_t)(ph >> 16);
        uint32_t q = (uint32_t)(p * (65535 - p)) >> 14;      /* 4 phi (1 - phi), Q16, <= 65534 */
        return (int32_t)((q * q) >> 17);
    }
    uint32_t t = ~ph >> 15;                                   /* 2 (1 - phi), Q16 (only used when phi >= 1/2) */
    uint32_t t2 = (t * t) >> 16;
    uint32_t v = (t2 * ((196608u - 2 * t) >> 2)) >> 15;       /* t^2 (3 - 2t), Q15 */
    return ph < 0x80000000u ? 32767 : (int32_t)(v > 32767 ? 32767 : v);
}

ALWAYS_INLINE void body_res(int w, int16_t *y, uint32_t ph, uint32_t inc, int32_t slope, const Shape *a,
                            const Shape *b)
{
    int32_t k = a->sl, dk = (b->sl - a->sl) >> 6;
    for (int i = 0; i < N; i++) {
        int32_t c = cos15((ph >> 8) * (uint32_t)(k >> 16));   /* k phi, wrapping */
        y[i] = (int16_t)(32767 - (int32_t)((uint32_t)(window(w, ph) * (32767 - c)) >> 15));
        ph += inc;
        inc += (uint32_t)slope;
        k += dk;
    }
}

#define PASS_RES(name, w)                                                                                        \
    __attribute__((noinline)) static void name(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope,           \
                                               const Shape *a, const Shape *b)                                  \
    {                                                                                                            \
        body_res(w, y, ph, inc, slope, a, b);                                                                    \
    }
PASS_RES(pass_res1, RES1)
PASS_RES(pass_res2, RES2)
PASS_RES(pass_res3, RES3)

/* The plain sine the resonance waves start from. */
__attribute__((noinline)) static void pass_sine(int16_t *y, uint32_t ph, uint32_t inc, int32_t slope)
{
    for (int i = 0; i < N; i++) {
        y[i] = (int16_t)cos15(ph);
        ph += inc;
        inc += (uint32_t)slope;
    }
}

/* The polyBLAMP factor for double sine at amount a (Q16) and increment inc: the slope change at the kink,
 * -4 pi (1 + a) sin(pi a) per turn, times inc / 6, Q15. */
static int32_t dbl_k(int32_t a, uint32_t inc)
{
    if (inc > 0x40000000u) inc = 0x40000000u;
    int32_t sn = cos15(((uint32_t)a << 15) - 0x40000000u);  /* sin(pi a), Q15, >= 0 */
    uint32_t t = (uint32_t)(sn * 17157) >> 13;                /* 4 pi / 6 sin(pi a), Q15 */
    t = opz_mul_u32_shr16(t, (uint32_t)(65536 + a));          /* x (1 + a) */
    return -(int32_t)opz_mul_u32_shr16(t, inc >> 16);         /* x inc (turns) */
}

static void run(int w, int16_t *y, uint32_t ph, uint32_t inc, int32_t slope, const Shape *a, const Shape *b,
                uint32_t recip, int32_t dk)
{
    switch (w) {
    case SAW: pass_saw(y, ph, inc, slope, a, b); break;
    case SQUARE: pass_square(y, ph, inc, slope, a, b); break;
    case PULSE: pass_pulse(y, ph, inc, slope, a, b); break;
    case DBL: pass_dbl(y, ph, inc, slope, a, b, recip, dk); break;
    case SAWPULSE: pass_sawpulse(y, ph, inc, slope, a, b); break;
    case RES1: pass_res1(y, ph, inc, slope, a, b); break;
    case RES2: pass_res2(y, ph, inc, slope, a, b); break;
    default: pass_res3(y, ph, inc, slope, a, b); break;
    }
}

/* ---- envelope and output -------------------------------------------------------------------------------------- */

/* The host envelope 64 samples on (sdk/docs/engine-abi.md §3: per sample r = c level, level = r + (1 - r^3) s),
 * in closed form as in engines/vowel: s = 0 (decay, release): level c^64; c = 1 (attack, sustain): the step
 * (1 - r^3) s held for the block; otherwise (a fade with a step) the exact 64 steps. The audio follows it as a
 * straight line across the block. */
static int32_t env_block(int32_t level, int32_t c, int32_t s)
{
    if (s == 0) {
        int32_t p = c;
        for (int i = 0; i < 6; i++) p = opz_mul_q31_rnd(p, p);
        return opz_mul_q31_rnd(level, p);
    }
    if (c == OPZ_Q31_ONE) {
        int32_t r = opz_mul_q31_rnd(c, level);
        int32_t k = opz_mul_q31_rnd(opz_sub_q31(OPZ_Q31_ONE, opz_mul_q31_rnd(opz_mul_q31_rnd(r, r), r)), s);
        int32_t d = opz_add_q31(k, r - level);
        if (d > 0x1ffffff) d = 0x1ffffff;
        if (d < -0x2000000) d = -0x2000000;
        level = opz_add_q31(level, d * N);
        return level < 0 ? 0 : level;
    }
    for (int i = 0; i < N; i++) level = opz_env_tick(level, c, s);
    return level < 0 ? 0 : level;
}

/* Up to three buffers times their gains (each wave's gain x weight x envelope x velocity x 2, Q31, ramped; used as
 * Q15), minus the DC (Q31, ramped), added into both channels of out. n buffers, a compile-time constant. */
typedef struct Mix {
    int32_t g[3], dg[3];
    int32_t dc, ddc;
} Mix;

ALWAYS_INLINE void body_out(fract32 *out, const Mix *m, int n)
{
    int32_t g0 = m->g[0], g1 = m->g[1], g2 = m->g[2], dc = m->dc;
    const int16_t *y0 = s_y[0], *y1 = s_y[1], *y2 = s_y[2];
    for (int i = 0; i < N; i++) {
        int32_t v = mul_s16(y0[i], g0 >> 16) - dc;
        if (n > 1) v += mul_s16(y1[i], g1 >> 16);
        if (n > 2) v += mul_s16(y2[i], g2 >> 16);
        out[2 * i] = opz_add_q31(out[2 * i], v);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], v);
        g0 += m->dg[0];
        if (n > 1) g1 += m->dg[1];
        if (n > 2) g2 += m->dg[2];
        dc += m->ddc;
    }
}

__attribute__((noinline)) static void pass_out1(fract32 *out, const Mix *m) { body_out(out, m, 1); }
__attribute__((noinline)) static void pass_out2(fract32 *out, const Mix *m) { body_out(out, m, 2); }
__attribute__((noinline)) static void pass_out3(fract32 *out, const Mix *m) { body_out(out, m, 3); }

/* ---- entry points --------------------------------------------------------------------------------------------- */

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note; (void)params;                                /* params may be NULL (engine-abi.md §3); not needed */
    State *s = state;
    uint32_t v = velocity > 127 ? 127 : velocity;
    s->ph = 0xc0000000u;                                     /* three quarters: a sine starts at its zero crossing */
    s->dcw = OPZ_Q31_ONE;
    s->depth = (int32_t)(v * 258u);                          /* 127 -> 32766 */
    s->amp = (fract32)(v * (uint32_t)(OPZ_VOICE_PEAK / 127));
    s->fresh = 1;
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                 /* the host releases the envelope; the DCW runs on */
}

/* Weight of the upper wave of a pair at position x (Q16) within it: holds for the first and last quarter. Q15. */
static int32_t upper(int32_t x)
{
    int32_t t = x - 16384;
    return t < 0 ? 0 : (t > 32768 ? 32768 : t);
}

/* gain x weight x envelope (Q15, Q15, Q31) -> the mix's Q31 gain, shifted left by sh (see Mix). */
static int32_t mix_gain(int32_t gh, int32_t w, fract32 e, int sh)
{
    return opz_shl_q31(opz_mul_q31((int32_t)((uint32_t)gh * (uint32_t)w), e), sh);
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* ---- per block -------------------------------------------------------------------------------------------- */
    uint32_t pitch_end = pitch + (uint32_t)pitchSlope * N;
    int32_t lg0 = log2_q16(pitch), lg1 = log2_q16(pitch_end);

    /* DCW: holds in the attack, then decays: per block x 2^-x, x = 64 / (fs ln2 tau),
     * tau = 15 ms x 2^(decay / 32) x 2^(-(log2 f - log2 f_C4) / 2) */
    if (OPZ_ADSR_STAGE(env) != 1) {
        int32_t lx = DCW_LOG2X0 - (int32_t)(params[OPZ_P_DECAY] >> 23) * 2048 + ((lg0 - LOG2_C4) >> 1);
        if (lx > (3 << 16)) lx = 3 << 16;
        if (lx < -(14 << 16)) lx = -(14 << 16);
        int32_t m = opz_exp2_q16(-opz_exp2_q16(lx));         /* Q16, < 65536 */
        s->dcw = opz_mul_q31(s->dcw, m << 15);
    }
    int32_t a1 = opz_p16(params[OPZ_P_P1]);
    if (a1 >= 65535) a1 = 65536;
    a1 += (int32_t)(((uint32_t)(65536 - a1) * (uint32_t)(opz_mul_q31(s->dcw, s->depth << 16) >> 15)) >> 16);
    int32_t x1 = opz_p16(params[OPZ_P_P2]) * 7;              /* 0 .. 7 waves, Q16 */
    if (s->fresh) {
        s->a_prev = a1;
        s->x_prev = x1;
        s->fresh = 0;
    }
    int32_t a0 = s->a_prev, x0 = s->x_prev;

    /* the pair of waves around x1; the start of the ramp is last block's position, clamped into the pair */
    int lo = x1 >> 16;
    if (lo > NWAVES - 2) lo = NWAVES - 2;
    int32_t base = lo << 16;
    int32_t u0 = x0 - base, u1 = x1 - base;
    u0 = u0 < 0 ? 0 : (u0 > 65536 ? 65536 : u0);
    int32_t wb0 = upper(u0), wb1 = upper(u1);

    /* the envelope, 64 samples on, times velocity x 2 (a wave's gain / 2 is what the table holds) */
    int32_t l0 = OPZ_ADSR_LEVEL(env);
    int32_t l1 = env_block(l0, OPZ_ADSR_COEF(env), OPZ_ADSR_STEP(env));
    OPZ_ADSR_LEVEL(env) = l1;
    fract32 e0 = opz_mul_q31(l0, s->amp), e1 = opz_mul_q31(l1, s->amp);

    Mix mx;
    Shape sa, sb;
    int n = 0;
    int32_t dc0 = 0, dc1 = 0, sg0 = 0, sg1 = 0;
    for (int k = 0; k < 2; k++) {
        int w = lo + k;
        int32_t w0 = k ? wb0 : 32768 - wb0, w1 = k ? wb1 : 32768 - wb1;
        if ((w0 | w1) == 0) continue;
        shape(&sa, w, a0, lg0);
        if (a1 == a0 && lg1 == lg0) sb = sa;                 /* a held note with the DCW run out: one shape */
        else shape(&sb, w, a1, lg1);
        uint32_t recip = 0;
        int32_t dk = 0;
        if (w == DBL) {
            uint32_t mid = pitch + (uint32_t)(pitchSlope * (N / 2));
            recip = opz_blep_recip(mid);
#ifndef PD_NO_BLAMP                                          /* tests/alias.c compares without it */
            dk = dbl_k(a1, mid);
#endif
        }
        run(w, s_y[n], s->ph, pitch, pitchSlope, &sa, &sb, recip, dk);
        int sh = w == DBL ? 4 : 3;                           /* gain / 2 x 2^3 = gain x 2 x ... (DBL is at half) */
        int32_t g0 = mix_gain(sa.gh, w0, e0, sh), g1 = mix_gain(sb.gh, w1, e1, sh);
        mx.g[n] = g0;
        mx.dg[n] = (g1 - g0) >> 6;
        dc0 += mul_s16(sa.dc, g0 >> 16) >> (sh - 3);          /* the mean of what the buffer holds x gain, Q31 */
        dc1 += mul_s16(sb.dc, g1 >> 16) >> (sh - 3);
        sg0 += (sa.sw * w0) >> 15;                            /* the sine's share x weight, Q15 */
        sg1 += (sb.sw * w1) >> 15;
        n++;
    }
    if (sg0 | sg1) {
        pass_sine(s_y[n], s->ph, pitch, pitchSlope);
        int32_t g0 = mix_gain(16384, sg0, e0, 3), g1 = mix_gain(16384, sg1, e1, 3);
        mx.g[n] = g0;
        mx.dg[n] = (g1 - g0) >> 6;
        n++;
    }
    mx.dc = dc0;
    mx.ddc = (dc1 - dc0) >> 6;
    if (n == 1) pass_out1(out, &mx);
    else if (n == 2) pass_out2(out, &mx);
    else pass_out3(out, &mx);

    /* phase at the end of the block: 64 inc + 2016 slope */
    s->ph += pitch * N + (uint32_t)pitchSlope * (N * (N - 1) / 2);
    s->a_prev = a1;
    s->x_prev = x1;
}

OPZ_EXPORTS_INSTRUMENT();
