/*
 * filter — resonant stereo filter with an envelope follower, played by notes on its FX track.
 *
 *   P1        cutoff      30 Hz .. 18 kHz (exponential, 0.43 semitone per step). Steps 242..254 fade the filter out
 *                         to the dry signal, so P1 at 255 is fully open: flat and transparent.
 *   P2        character   one macro over resonance, shape and movement:
 *                           0 .. 95     low-pass, resonance rising from none (Q 0.7) to strong (Q 10)
 *                           96 .. 127   low-pass morphs into band-pass at Q 10
 *                           128 .. 255  auto-wah: the envelope follower opens the filter with the input's loudness,
 *                                       0 -> +5 octaves; the shape eases back from band-pass to a resonant low-pass
 *                                       and the resonance from Q 10 to Q 4
 *                         piecewise and continuous, so the OS LFO on P2 glides through it without steps.
 *   pan       dry / wet   0 dry .. 128 fully filtered; 128 .. 255 stay fully filtered (a half-dry filter is barely a
 *                         filter, so the centre is all wet, unlike TE's effects)
 *   level     output level (x gain)
 *   note on the FX track  the cutoff moves to the note's pitch (P1 then shifts it: P1 128 = exactly on the note, each
 *                         step 0.43 semitone up or down) and an envelope sweeps it up by 2..4 octaves (velocity),
 *                         falling back in ~250 ms while the note is held and ~60 ms after note off. With no note held
 *                         the filter sits at P1 (the note offset glides back in ~40 ms).
 *
 * No internal LFO: the OS LFO can target P1 (cutoff) with its own speed, shape and sync, and notes on the FX track give
 * tempo-locked sweeps. The filter and resonance knobs are the OS's own track filter, so this engine never reads them.
 *
 * Core: the trapezoidal (zero-delay-feedback) state-variable filter from Andrew Simper's published equations (Cytomic,
 * "Solving the continuous SVF equations using trapezoidal integration and equivalent currents", 2013), stable at any
 * cutoff and resonance. Q31 signals with 24 dB of internal headroom,
 * saturating everywhere, so there are no denormals and no wrap-around; a soft clip above -6 dBFS on the filtered path.
 * No floating point, no libc, no division. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_engine.h"

#define SUB             16                      /* control rate: coefficients, envelopes and mix every 16 samples */
#define HEADROOM        4                       /* the filter runs 24 dB below the signal */
#define Q31_ONE         0x7fffffff

/* Cutoff as octaves above 27.5 Hz (A0, MIDI 21) in Q16 */
#define OCT_P1_LO       8227                    /* log2(30 / 27.5) */
#define OCT_P1_SPAN     604820                  /* log2(18000 / 30) */
#define OCT_TOP         (OCT_P1_LO + OCT_P1_SPAN)       /* 18 kHz */
#define OCT_OPEN_END    (OCT_TOP - 1024)        /* fully dry from here (1/64 octave below the top) ... */
#define OCT_OPEN_START  (OCT_TOP - 32768)       /* ... fully filtered from half an octave below the top */
#define P1_CENTRE       32896                   /* P1 byte 128 as 0..65535: the note's own pitch */

/* P2 zones, 0..65535 */
#define P2_RES_END      24576                   /* byte 96 */
#define P2_WAH          32768                   /* byte 128 */
#define K_EXP_MAX       250460                  /* log2(1.414 / 0.1) in Q16: k = 1.414 * 2^-x, Q 0.7 .. 10 */
#define K_EXP_WAH       130938                  /* log2(1.414 / 0.354): Q 4 at P2 255 */
#define WAH_DEPTH       (5 << 16)               /* follower sweep at P2 255, octaves */

/* Envelopes, per 16-sample step: 1 - e^(-16 / (tau * 44100)) in Q31 */
#define FOL_ATT         0x153c5178              /* 2 ms */
#define FOL_REL         0x004f2963              /* 150 ms */
#define FOL_FLOOR       (8 << 16)               /* the follower sweeps over 2^-8 .. 2^-1 (-48 .. -6 dBFS), log */
#define ENV_ATT_STEP    (Q31_ONE / 8)           /* attack: 8 steps, 2.9 ms */
#define ENV_HOLD        0x002f851d              /* 250 ms while the note is held */
#define ENV_REL         0x00c58bb6              /* 60 ms after note off */
#define KEY_ON          0x153c5178              /* the note offset glides in 2 ms ... */
#define KEY_OFF         0x0127df1f              /* ... and back to P1 in 40 ms */

/* g = tan(pi f / 44100) in Q28 for f = 27.5 Hz * 2^(i/8), i = 0..76: Python
 *   [round(math.tan(math.pi * 27.5 * 2**(i/8) / 44100) * 2**28) for i in range(77)] */
static const int32_t k_g[77] = {
    525877, 573473, 625377, 681979, 743704, 811015, 884419, 964466,
    1051759, 1146952, 1250761, 1363967, 1487419, 1622045, 1768857, 1928957,
    2103550, 2293946, 2501577, 2728004, 2974929, 3244208, 3537867, 3858113,
    4207357, 4588227, 5003589, 5456572, 5950589, 6489365, 7076963, 7717821,
    8416782, 9179135, 10010656, 10917655, 11907029, 12986319, 14163771, 15448412,
    16850131, 18379762, 20049196, 21871490, 23861007, 26033566, 28406627, 30999495,
    33833575, 36932669, 40323333, 44035313, 48102082, 52561506, 57456682, 62836991,
    68759467, 75290560, 82508460, 90506196, 99395820, 109314154, 120430821, 132959676,
    147175454, 163438602, 182233436, 204228729, 230377822, 262092022, 301558657, 352367578,
    420862882, 519434734, 676025055, 969824964, 1748922779,
};

typedef struct State {
    int32_t ic1[2], ic2[2];                     /* SVF integrator states, Q31 at -24 dB */
    int32_t fol;                                /* envelope follower, Q31 peak level */
    int32_t env;                                /* note envelope, Q31 */
    int32_t key, key_target;                    /* note offset from P1, octaves Q16 */
    int32_t env_depth;                          /* note sweep, octaves Q16 */
    /* last values reached by the ramps: knob moves glide across the block instead of stepping at its start */
    int32_t wet;                                /* filtered share of the output, Q31 */
    int32_t g_lp, g_bp, out_gain;               /* Q31 */
    int32_t k_q16;                              /* damping k = 1 / Q */
    int16_t note;                               /* held note, -1 none */
    uint8_t attack;                             /* note envelope in its attack */
    uint8_t started;                            /* first block done: ramps start from the current values */
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* 2^(x / 65536) in Q16, x in [-15, 14] octaves. Cubic fit on the fraction (max error ~0.01 %). */
static int32_t exp2_q16(int32_t x)
{
    int32_t i = x >> 16;
    uint32_t f = x & 0xffff, p = 5125;          /* 0.0782 */
    p = ((p * f) >> 16) + 14824;                /* 0.2262 */
    p = ((p * f) >> 16) + 45586;                /* 0.6956 */
    p = ((p * f) >> 16) + 65536;                /* 1      */
    return i >= 0 ? (int32_t)(p << i) : (int32_t)(p >> -i);
}

static inline int32_t p16(uint32_t param) { return (int32_t)(param >> 15); }      /* Q31 param -> 0..65535 */

static inline int32_t p1_oct(int32_t u) { return OCT_P1_LO + ((u * 4725) >> 9); }  /* u * OCT_P1_SPAN / 65536 */

/* log2(x / 2^31) in Q16 for a Q31 level x > 0 (about -31 .. 0); quadratic on the mantissa, error < 0.01 octave. */
static int32_t log2_q16(int32_t x)
{
    if (x <= 0) return -(31 << 16);
    int32_t e = 0;
    while (x < 0x40000000) { x <<= 1; e++; }
    int32_t f = (x - 0x40000000) >> 14;                        /* mantissa - 1 in Q16, 0 .. 65535 */
    int32_t l = f + (opz_mul_q31(f * (65536 - f), 736801640) >> 16);   /* log2(1 + f) ~ f + 0.3431 f (1 - f) */
    return l - ((e + 1) << 16);
}

#if defined(__bfin__)
static inline int32_t sub_q31(int32_t a, int32_t b) { return __builtin_bfin_sub_fr1x32(a, b); }
static inline int32_t shl_sat(int32_t x, int n) { return __builtin_bfin_shl_fr1x32(x, n); }     /* saturating */
#else
static inline int32_t sub_q31(int32_t a, int32_t b) { return opz_sat32((int64_t)a - b); }
static inline int32_t shl_sat(int32_t x, int n)
{
    if (x > (INT32_MAX >> n)) return INT32_MAX;
    if (x < (INT32_MIN >> n)) return INT32_MIN;
    return x << n;
}
#endif

/* Soft clip, Q30 in (range +-2) -> Q31 out: linear up to 0.5 (-6 dBFS), then a parabola that reaches 1.0 with zero
 * slope at 1.5 and stays there. */
static inline int32_t soft_clip(int32_t x)
{
    int32_t a = x < 0 ? (x == INT32_MIN ? INT32_MAX : -x) : x;
    int32_t y;
    if (a <= (1 << 29)) y = a;
    else if (a >= (3 << 29)) y = 1 << 30;
    else {
        int32_t s = a - (1 << 29);
        y = (1 << 29) + s - opz_mul_q31(s, s);  /* 0.5 + s - s^2/2 */
    }
    y = shl_sat(y, 1);
    return x < 0 ? -y : y;
}

/* 1 / d for d in Q25, 1 <= d < 64, result Q31. Newton-Raphson on the mantissa: no division. */
static int32_t recip_q31(int32_t d)
{
    int e = 0;                                  /* d = m * 2^(e + 1), m in [0.5, 1) */
    while ((d >> e) >= (1 << 26)) e++;
    int32_t m = e <= 5 ? d << (5 - e) : d >> (e - 5);          /* m in Q31 */
    int32_t h = (1515870810 - opz_mul_q31(1010580540, m)) * 2; /* h = r / 2, r0 = 48/17 - 32/17 m */
    for (int i = 0; i < 3; i++) {
        int32_t eh = (int32_t)(0x80000000u - (uint32_t)opz_mul_q31(m, h));      /* (2 - m r) / 2 */
        h = shl_sat(opz_mul_q31(h, eh), 1);
    }
    return h >> e;                              /* 1/d = r * 2^-(e+1) = h * 2^-e */
}

/* g * c / 2^28 for g in Q28 (0 .. 8) and c in Q31, result Q31 (< 1), at full precision for small g */
static inline int32_t mul_g(int32_t g, int32_t c)
{
    return g < (1 << 28) ? opz_mul_q31(g << 3, c) : shl_sat(opz_mul_q31(g, c), 3);
}

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
    State *s = state;
    volatile uint8_t *p = (volatile uint8_t *)s;               /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    s->note = -1;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                       /* 255: plug selected, no note */
    int32_t oct = ((note - 21) * 349525) >> 6;                 /* semitones -> octaves Q16 (65536 / 12) */
    s->key_target = oct - p1_oct(P1_CENTRE);
    s->env_depth = (2 << 16) + velocity * 1032;                /* 2 .. 4 octaves (2^17 / 127) */
    s->attack = 1;
    s->note = (int16_t)note;
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note != s->note) return;                               /* a later note is still held */
    s->note = -1;
    s->attack = 0;
    s->key_target = 0;
}

typedef struct Coefs { int32_t a1, a2, a3, wet; } Coefs;

/* Control rate, once per 16 samples: follower, note envelope, note offset -> cutoff -> SVF coefficients and the
 * filtered share of the output. Not inlined: one copy keeps the engine inside its 5 KB slot. */
static void __attribute__((noinline)) control(State *s, const fract32 *in, int32_t base, int32_t fdepth, int32_t kq,
                                              int32_t pan_wet, Coefs *cf)
{
    int32_t peak = 0;
    for (int i = 0; i < 2 * SUB; i++) {
        int32_t x = in[i];
        int32_t a = x < 0 ? (x == INT32_MIN ? INT32_MAX : -x) : x;
        if (a > peak) peak = a;
    }
    s->fol = opz_add_q31(s->fol, opz_mul_q31(peak > s->fol ? FOL_ATT : FOL_REL, peak - s->fol));
    if (s->attack) {
        s->env = opz_add_q31(s->env, ENV_ATT_STEP);
        if (s->env == Q31_ONE) s->attack = 0;
    } else {
        s->env -= opz_mul_q31(s->env, s->note >= 0 ? ENV_HOLD : ENV_REL);
    }
    s->key += opz_mul_q31(s->note >= 0 ? KEY_ON : KEY_OFF, s->key_target - s->key);

    int32_t lv = log2_q16(s->fol) + FOL_FLOOR;                 /* -48 .. -6 dBFS -> 0 .. 1 Q31 (x 2^15 / 7) */
    int32_t curve = lv <= 0 ? 0 : (lv >= (7 << 16) ? Q31_ONE : lv * 4681);
    int32_t oct = base + s->key + opz_mul_q31(s->env, s->env_depth) + opz_mul_q31(curve, fdepth);

    int32_t open;                                               /* filtered share from the open-top fade, Q31 */
    if (oct >= OCT_OPEN_END) open = 0;
    else if (oct <= OCT_OPEN_START) open = Q31_ONE;
    else open = (OCT_OPEN_END - oct) * 67650;                   /* 2^31 / (OCT_OPEN_END - OCT_OPEN_START) */
    if (oct > OCT_TOP) oct = OCT_TOP;
    if (oct < 0) oct = 0;

    int32_t idx = oct >> 13, frac = oct & 0x1fff;              /* 1/8 octave per table entry */
    int32_t g = k_g[idx] + opz_mul_q31(k_g[idx + 1] - k_g[idx], frac << 18);      /* Q28 */
    int32_t d = (1 << 25) + opz_mul_q31(g, g + (kq << 12));                    /* 1 + g (g + k), Q25 */
    cf->a1 = recip_q31(d);
    cf->a2 = mul_g(g, cf->a1);
    cf->a3 = mul_g(g, cf->a2);
    cf->wet = opz_mul_q31(open, pan_wet);
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block: P2 -> resonance exponent, shape, follower depth ---------------------------------- */
    int32_t u = p16(params[OPZ_P_P2]), kexp, morph, fdepth;    /* morph: 0 low-pass .. 65535 band-pass */
    if (u < P2_RES_END) {
        kexp = (u * 10437) >> 10;                              /* u * K_EXP_MAX / P2_RES_END */
        morph = 0; fdepth = 0;
    } else if (u < P2_WAH) {
        kexp = K_EXP_MAX;
        morph = (u - P2_RES_END) * 8;
        fdepth = 0;
    } else {
        int32_t t = (u - P2_WAH) * 2;                          /* 0 .. 65534 */
        kexp = K_EXP_MAX - (((t >> 2) * (K_EXP_MAX - K_EXP_WAH)) >> 14);
        morph = 65535 - (((t >> 1) * 45875) >> 15);            /* band-pass -> 30 % band-pass, 70 % low-pass */
        fdepth = t * (WAH_DEPTH >> 16);
    }
    int32_t k_q16 = (exp2_q16(-kexp) * 23170) >> 14;           /* 1.414 * 2^-kexp */
    int32_t res = (kexp * 4287) >> 15;                         /* resonance 0 .. 32768: kexp / K_EXP_MAX */
    if (res > 32768) res = 32768;
    /* Output gains (Q31, applied to the -24 dB signals): the low-pass loses up to 6 dB as resonance rises; the
     * band-pass is scaled by 2 sqrt(k), so its peak rises as 1/sqrt(k) and its loudness stays near the low-pass. The
     * morph is a quadratic crossfade, so the mid-point does not dip. */
    uint32_t w_bp = ((uint32_t)morph * (uint32_t)(131072 - morph)) >> 16;              /* 2m - m^2 */
    uint32_t w_lp = 65536 - (((uint32_t)morph * (uint32_t)morph) >> 16);               /* 1 - m^2 */
    uint32_t glp = (uint32_t)((65536 - res) >> 1) * w_lp;                             /* (1 - res/2) w_lp */
    uint32_t gbp = (uint32_t)exp2_q16(-(kexp >> 1)) * w_bp;                           /* 2 sqrt(k / 1.414) w_bp */
    int32_t g_lp = glp > Q31_ONE ? Q31_ONE : (int32_t)glp;
    int32_t g_bp = gbp > Q31_ONE ? Q31_ONE : (int32_t)gbp;

    int32_t base = p1_oct(p16(params[OPZ_P_P1]));
    int32_t pan = (int32_t)(params[OPZ_P_PAN] & Q31_ONE);
    int32_t pan_wet = pan >= 0x40000000 ? Q31_ONE : pan * 2;
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & Q31_ONE), gain);
    if (!s->started) {
        s->g_lp = g_lp; s->g_bp = g_bp; s->out_gain = out_gain; s->k_q16 = k_q16;
        s->wet = -1;                                            /* set from the first cutoff below */
        s->started = 1;
    }
    int32_t gl = s->g_lp, gb = s->g_bp, og = s->out_gain;     /* per-sample ramps over the block */
    int32_t dgl = (g_lp - gl) >> 6, dgb = (g_bp - gb) >> 6, dog = (out_gain - og) >> 6;
    int32_t dk = (k_q16 - s->k_q16) >> 2;                       /* per 16-sample step */
    int32_t kq = s->k_q16;

    for (int sub = 0; sub < OPZ_BLOCK_FRAMES; sub += SUB) {
        Coefs cf;
        kq += dk;
        control(s, in + 2 * sub, base, fdepth, kq, pan_wet, &cf);
        int32_t a1 = cf.a1, a2 = cf.a2, a3 = cf.a3, wet_target = cf.wet;
        if (s->wet < 0) s->wet = wet_target;
        int32_t wet_step = (wet_target - s->wet) >> 4;           /* / SUB */
        int32_t wet = s->wet;

        for (int i = sub; i < sub + SUB; i++) {
            wet += wet_step;
            gl += dgl; gb += dgb; og += dog;
            for (int c = 0; c < 2; c++) {
                int32_t x = in[2 * i + c];                      /* interleaved L, R */
                int32_t v0 = x >> HEADROOM;
                int32_t ic1 = s->ic1[c], ic2 = s->ic2[c];
                int32_t v3 = sub_q31(v0, ic2);
                int32_t v1 = opz_add_q31(opz_mul_q31(a1, ic1), opz_mul_q31(a2, v3));      /* band-pass */
                int32_t v2 = opz_add_q31(ic2, opz_add_q31(opz_mul_q31(a2, ic1), opz_mul_q31(a3, v3)));  /* low */
                s->ic1[c] = sub_q31(opz_add_q31(v1, v1), ic1);
                s->ic2[c] = sub_q31(opz_add_q31(v2, v2), ic2);
                int32_t y = opz_add_q31(opz_mul_q31(v2, gl), opz_mul_q31(v1, gb));
                int32_t f = soft_clip(shl_sat(y, HEADROOM - 1));                      /* back to level, Q30 */
                int32_t mix = opz_add_q31(x, opz_mul_q31(wet, sub_q31(f, x)));
                out[2 * i + c] = opz_mul_q31(mix, og);
            }
        }
        s->wet = wet_target;
    }
    s->g_lp = g_lp; s->g_bp = g_bp; s->out_gain = out_gain; s->k_q16 = k_q16;
}

OPZ_EXPORTS_FX();
