/*
 * snap — noise-burst percussion for the kick, snare, perc and sample tracks: claps, snaps, rims, sticks, shakers,
 * tambourines, brushes. Each of the 24 keys plays its own model (README.md has the table).
 *
 * A model is filtered noise shaped by two envelopes, plus up to two resonant bodies:
 *
 *   bursts   `count` short noise bursts `spacing` apart, each with its own random timing, level and stereo
 *            position (the hands of a clap; a flam on single hits). A one-pole decay restarted at every burst.
 *   tail     starts with the last burst: linear attack, hold, one-pole decay (the room of a clap, a shaker's swish).
 *   filter   one Chamberlin state-variable filter on the noise: band-, high- or low-pass.
 *   bodies   two two-pole resonators struck at every burst (rims, sticks, clave, woodblock, slap).
 *
 *   P1        spread   0: one tight, centred burst group (x0.5 spacing, two bursts fewer, no jitter);
 *                      128: the model as designed; 255: loose and wide (x2 spacing, two bursts more, heavy jitter
 *                      in time, level and stereo position) — single hits become flams and drags
 *   P2        decay    tail x0.25 .. x4 (and its hold), tail level x0.75 .. x1.25, bursts x0.7 .. x1.4, bodies
 *                      x0.5 .. x2; noise filter -0.75 .. +0.75 octave (short, dry, dark -> long, roomy, bright)
 *   velocity           level; softer hits are darker (down to -0.5 octave) with weaker bursts against the tail
 *   octave             each octave press moves the filter and the bodies by an octave (+-2)
 *
 * Knobs are read when a hit starts, as in kit. The filter and resonance knobs are the OS's own track filter.
 * Each hit ends itself (envelope stage 0) at -60 dB; a voice the host wants back (stage 5/6) fades in ~5 ms.
 *
 * Per sample there is no division, no 64-bit maths and no float; coefficients are computed once per hit. The
 * burst timer splits the block into runs, so the inner loops carry no event branches.
 *
 * Models follow the classic analog clap and rimshot circuits (TR-808/909 style: repeated noise bursts into a
 * band-pass with a reverb tail, bridged-T resonators), as also explored by Mutable Instruments Plaits' and
 * Peaks' noise and drum models (MIT); written from scratch on sdk/include/opz_dsp.h. MIT licence, see
 * CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

enum { F_BP = 0, F_HP = 1, F_LP = 2 };
#define G(n) ((n) << 2)            /* filtered noise x2^n (0 .. 3), saturating */
enum { T_WAIT = 0, T_ATTACK, T_HOLD, T_DECAY };

typedef struct Model {
    uint8_t count, jitter;    /* bursts; timing/level/pan randomness at P1 128, /256 */
    uint16_t spacing;         /* samples between bursts */
    uint16_t bdecay;          /* burst decay time constant, samples */
    uint8_t bgain, tgain;     /* burst and tail level, /256 */
    uint8_t attack, hold;     /* tail attack and hold, blocks */
    uint16_t tdecay;          /* tail decay time constant, samples */
    uint16_t fc;              /* noise filter, Hz */
    uint8_t q, mode;          /* filter damping x128 (q = 1/Q, 8 .. 185); F_BP / F_HP / F_LP + G(boost) */
    uint16_t f1, f2;          /* body resonators, Hz (0 = off) */
    uint16_t body;            /* body decay time constant, samples */
    uint8_t g1, g2;           /* body levels, /256 (1.0 = OPZ_VOICE_PEAK) */
} Model;

#define SMP(ms) ((uint16_t)((ms) * 44.1 + 0.5))         /* compile-time constants: no float in the image */
#define BLK(ms) ((uint8_t)((ms) * 44.1 / 64 + 0.5))

/* count spacing jitter bdecay bgain  attack hold tdecay tgain  fc q mode  f1 f2 body g1 g2 */
#define M(n, sp, j, bd, bg, at, ho, td, tg, fc, q, mo, f1, f2, bo, g1, g2) \
    { n, j, SMP(sp), SMP(bd), bg, tg, BLK(at), BLK(ho), SMP(td), fc, q, mo, f1, f2, SMP(bo), g1, g2 }

static const Model k_models[24] = {
    /*   n  sp(ms) jit  bd(ms) bg   att  hold  td(ms) tg    fc    q  mode              f1    f2  body  g1   g2 */
    M(   4,   10,     20,   2.5,  230,     0,     0,    60,  230,   1100,   64,   F_BP | G(3),      0,      0,    0,    0,    0), /* clap 909 */
    M(   3,   11,     10,   3.0,  230,     0,     0,    95,  230,    950,   48,   F_BP | G(3),      0,      0,    0,    0,    0), /* clap 808 */
    M(   2,    6,      0,   2.0,  240,     0,     0,    35,  200,   1500,   80,   F_BP | G(3),      0,      0,    0,    0,    0), /* tight clap */
    M(   4,   12,     30,   3.0,  240,     3,     0,   210,  255,   1000,   60,   F_BP | G(3),      0,      0,    0,    0,    0), /* big clap */
    M(   6,    7,    110,   4.0,  200,     0,     0,    70,  200,   1300,   90,   F_BP | G(3),      0,      0,    0,    0,    0), /* handclaps */
    M(  12,    5,    200,   5.0,  170,     2,     0,   120,  180,   1200,  120,   F_BP | G(3),      0,      0,    0,    0,    0), /* crowd */
    M(   1,   25,      0,   1.5,  150,     0,     0,    35,  170,   2600,   30,   F_BP | G(2),   2200,      0,    8,  110,    0), /* snap */
    M(   1,   22,      0,   1.0,  150,     0,     0,    20,  130,   3800,   40,   F_BP | G(2),   3000,      0,    5,   80,    0), /* bright snap */
    M(   1,   18,      0,   1.2,  150,     0,     0,     6,   40,   4000,  160,          F_HP,    720,   1700,   12,  220,  170), /* rim 909 */
    M(   1,   18,      0,   0.8,  150,     0,     0,     0,    0,   3000,  180,          F_HP,    455,   1667,   12,  255,  220), /* rim 808 */
    M(   1,   20,      0,   1.5,  110,     0,     0,    15,   40,   2200,  100,   F_BP | G(1),    380,   1050,   25,  200,  120), /* side stick */
    M(   1,   15,      0,   0.7,  170,     0,     0,     0,    0,   5000,  180,          F_HP,   2400,   3900,    9,  190,  120), /* sticks */
    M(   1,   20,      0,   0.5,   60,     0,     0,     0,    0,   6000,  180,          F_HP,   2500,      0,   30,  255,    0), /* clave */
    M(   1,   20,      0,   0.8,   80,     0,     0,     0,    0,   3000,   90,          F_BP,   1050,   1650,   35,  230,   70), /* woodblock */
    M(   1,   30,      0,   2.0,   60,    12,     0,    45,  190,   6500,  170,   F_HP | G(1),      0,      0,    0,    0,    0), /* shaker */
    M(   1,   25,      0,   1.0,  120,     3,     0,    25,  180,   7000,  150,   F_HP | G(2),      0,      0,    0,    0,    0), /* maracas */
    M(   3,    6,    120,   8.0,  170,     0,     0,   160,  120,   6800,   45,   F_BP | G(2),      0,      0,    0,    0,    0), /* tambourine */
    M(   5,   25,     60,   15.,  140,     0,     0,   200,  100,   6500,   45,   F_BP | G(2),      0,      0,    0,    0,    0), /* tamb shake */
    M(   1,   25,      0,   0.5,   60,     6,     0,    30,  190,   7000,  180,   F_HP | G(2),      0,      0,    0,    0,    0), /* cabasa */
    M(   1,   30,      0,   3.0,  140,     3,     0,   110,  150,   3200,  170,   F_BP | G(2),    190,      0,   40,   70,    0), /* brush hit */
    M(   1,   40,      0,   1.0,    0,    70,     0,   120,  170,   3000,  180,   F_BP | G(2),      0,      0,    0,    0,    0), /* brush swish */
    M(   1,   25,      0,   2.0,  220,     0,     0,    30,   70,   1200,  128,   F_LP | G(1),    160,      0,   35,  255,    0), /* slap */
    M(  10,   14,     20,   4.0,  220,     0,     0,    20,   60,   2800,   64,   F_BP | G(3),      0,      0,    0,    0,    0), /* guiro */
    M(   4,    9,     30,   3.0,  210,     0,   120,     8,  220,   1200,   76,   F_BP | G(2),      0,      0,    0,    0,    0), /* gated clap */
};

#define THRESHOLD 0x00200000   /* -60 dB: a part is over below this */
#define Q30_ONE   0x40000000

typedef struct State {
    uint32_t seed;
    int32_t lo, bd;                       /* filter states */
    int32_t f15, q15, g15;                /* filter: 2 sin(pi fc / fs), q / 2, input gain (Q15) */
    int32_t eb, cb;                       /* burst envelope (1 at a burst) and its decay */
    int32_t bl15, br15;                   /* this burst's level per channel, Q15 */
    int32_t bgain;                        /* burst level, Q31 */
    int32_t et, ct, st;                   /* tail level, coefficient, step (per sample: et = et ct + st) */
    int32_t tpeak, tcoef, tstep;          /* tail level, decay, attack step */
    int32_t y1a, y2a, y1b, y2b;           /* body resonators */
    int32_t a1a, a2a, a1b, a2b;           /* their coefficients: 2 r cos w (Q30), r^2 */
    int32_t kicka, kickb, ga, gb;         /* strike g sin w, and the amplitude g */
    int32_t ebody, rbody64;               /* bound on the bodies' amplitude, and r^64 */
    int32_t timer, spacing, jit, width;   /* samples to the next burst; spacing; jitter, stereo width (Q31) */
    int32_t tcount;                       /* blocks left in the tail's attack or hold */
    OpzMix mix;                           /* the track's level and pan (opz_mix.h) */
    uint8_t bursts, tstage, mode, body, model, active, fading, hold;
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes (the real limit, docs/STATUS.md)");

static const ModuleCallbackInterface *s_cb;

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    State *s = state;
    s->seed = 0x2468ace1u + track * 7919u;
}

/* ---- per hit ------------------------------------------------------------------------------------------------ */

#if defined(__GNUC__) && !defined(__clang__)
#define PER_HIT __attribute__((optimize("Os")))   /* once per hit or burst: size matters more than speed */
#else
#define PER_HIT
#endif

static inline int32_t m30(int32_t a, int32_t b) { return opz_mul_q31(a, b) << 1; }      /* Q30 x Q30, |ab| < 2 */

/* 1 - x2 k0 (1 - x2 k1 (1 - ...)) in Q30, from the innermost term out: the Taylor series of sin x / x and cos x */
PER_HIT static int32_t taylor(int32_t x2, const int32_t *k, int n)
{
    int32_t t = Q30_ONE;
    while (n--) t = Q30_ONE - m30(m30(x2, *k++), t);
    return t;
}

static const int32_t k_sin[3] = { Q30_ONE / 42, Q30_ONE / 20, Q30_ONE / 6 };
static const int32_t k_cos[4] = { Q30_ONE / 56, Q30_ONE / 30, Q30_ONE / 12, Q30_ONE / 2 };

/* sin x in Q30 for x in Q30, 0 <= x < 1.2 (error < 1e-4) */
static int32_t sin30(int32_t x) { return m30(x, taylor(m30(x, x), k_sin, 3)); }

/* Phase increment of hz moved by tilt octaves (Q16), clamped to [lo, hi]. hz <= 16000. */
PER_HIT static uint32_t tilt_inc(uint32_t hz, int32_t tilt, uint32_t lo, uint32_t hi)
{
    uint32_t inc = hz * 97391u;                                                  /* Q32 turns / sample */
    inc = opz_mul_u32_shr16(inc, (uint32_t)opz_exp2_q16(tilt & 0xffff));         /* fraction: < 2 inc < 2^31 */
    for (int i = tilt >> 16; i > 0; i--) inc = inc > hi ? hi : inc << 1;
    for (int i = tilt >> 16; i < 0; i++) inc >>= 1;
    return inc < lo ? lo : inc > hi ? hi : inc;
}

/* x = pi f / fs (half the angle per sample) in Q30 from a phase increment below 2^31 */
static inline int32_t half_angle(uint32_t inc) { return opz_mul_q31((int32_t)inc, 0x6487ed51); }

#define INC_HZ(hz) ((uint32_t)((hz) * 97391u))

/* A body resonator at hz (0: off) with decay coefficient r and level g. */
PER_HIT static void body_coef(uint32_t hz, int32_t tilt, int32_t r, int32_t g, int32_t *a1, int32_t *a2, int32_t *kick)
{
    if (!hz || !g) { *a1 = *a2 = *kick = 0; return; }
    int32_t x = half_angle(tilt_inc(hz, tilt, INC_HZ(30), INC_HZ(16000)));
    int32_t sx = sin30(x), cx = taylor(m30(x, x), k_cos, 4);
    int32_t cosw = Q30_ONE - (m30(sx, sx) << 1);                                 /* cos w = 1 - 2 sin^2 x */
    int32_t sinw = opz_shl_q31(m30(sx, cx), 2);                                  /* sin w = 2 sin x cos x, Q31 */
    *a1 = opz_shl_q31(opz_mul_q31(r, cosw), 1);                                  /* 2 r cos w, Q30 */
    *a2 = opz_mul_q31(r, r);
    *kick = opz_mul_q31(g, sinw);                                                /* response g r^n sin w(n+1) */
}

static int32_t rnd(State *s) { return opz_noise(&s->seed); }

PER_HIT static void tail_stage(State *s, int stage)
{
    s->tstage = (uint8_t)stage;
    s->ct = OPZ_Q31_ONE;
    s->st = 0;
    if (stage == T_ATTACK) {
        s->st = s->tstep;
        s->tcount = s->model < 24 ? k_models[s->model].attack : 0;
    } else if (stage == T_HOLD) {
        s->tcount = s->hold;
    } else if (stage == T_DECAY) {
        s->ct = s->tcoef;
    }
}

/* Start the tail: attack, hold and decay, skipping the stages of zero length. */
PER_HIT static void tail_start(State *s)
{
    const Model *m = &k_models[s->model];
    if (m->attack) {
        tail_stage(s, T_ATTACK);
        return;
    }
    s->et = s->tpeak;
    tail_stage(s, s->hold ? T_HOLD : T_DECAY);
}

/* One burst: restart the burst envelope at a random level and stereo position, strike the bodies, schedule the
 * next burst (or start the tail after the last). */
PER_HIT static void burst(State *s)
{
    int32_t r1 = rnd(s), r2 = rnd(s);
    int32_t lv = OPZ_Q31_ONE - opz_mul_q31(opz_abs32(r1), s->jit >> 2);        /* level: up to -2.5 dB at P1 255 */
    int32_t p = opz_mul_q31(r2, s->width);                                      /* -width .. width */
    int32_t g = opz_mul_q31(s->bgain, lv);
    s->bl15 = opz_mul_q31(g, p > 0 ? OPZ_Q31_ONE - p : OPZ_Q31_ONE) >> 16;
    s->br15 = opz_mul_q31(g, p < 0 ? OPZ_Q31_ONE + p : OPZ_Q31_ONE) >> 16;
    s->eb = OPZ_Q31_ONE;
    if (s->body) {
        s->y1a = opz_add_q31(s->y1a, opz_mul_q31(s->kicka, lv));
        s->y1b = opz_add_q31(s->y1b, opz_mul_q31(s->kickb, lv));
        s->ebody = opz_add_q31(s->ebody, opz_mul_q31(opz_add_q31(s->ga, s->gb), lv));
    }
    if (s->bursts) s->bursts--;
    if (s->bursts) {
        int32_t t = s->spacing + opz_mul_q31(rnd(s), opz_mul_q31(s->spacing, s->jit));
        s->timer = t < 16 ? 16 : t;
    } else {
        s->timer = 0;
        tail_start(s);
    }
}

PER_HIT void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    int key = note - 53, oct = 0;                                  /* key = (note - 53) mod 24, octave = floor(/24) */
    while (key < 0) { key += 24; oct--; }
    while (key >= 24) { key -= 24; oct++; }
    if (oct < -2) oct = -2;
    if (oct > 2) oct = 2;
    const Model *m = &k_models[key];
    s->model = (uint8_t)key;

    int32_t p1 = params ? opz_p16(params[OPZ_P_P1]) : 32768;      /* 0 .. 65535; NULL: knobs at the centre */
    int32_t p2 = params ? opz_p16(params[OPZ_P_P2]) : 32768;
    if (p1 > 65535) p1 = 65535;
    if (p2 > 65535) p2 = 65535;
    int32_t vel = velocity > 127 ? 127 : velocity;
    int32_t amp = vel * 0x01020408;                                /* vel / 127, Q31 */

    /* P1: spread */
    int32_t count = m->count + ((p1 * 5) >> 16) - 2;
    s->bursts = (uint8_t)(count < 1 ? 1 : count > 15 ? 15 : count);
    s->spacing = (int32_t)opz_mul_u32_shr16(m->spacing, (uint32_t)opz_exp2_q16((p1 - 32768) * 2));
    int32_t jit = ((m->jitter * (p1 >> 8)) >> 7) + ((int32_t)(opz_mul_u16(p1, p1) >> 16) * 96 >> 16);
    if (jit > 255) jit = 255;
    s->jit = jit << 23;
    s->width = s->jit;

    /* P2: decay and tone; velocity: level and brightness; octave: tone */
    int32_t tscale = opz_exp2_q16((p2 - 32768) * 4);              /* x0.25 .. x4, Q16 */
    int32_t tdecay = (int32_t)opz_mul_u32_shr16(m->tdecay, (uint32_t)tscale);
    int32_t hold = (int32_t)opz_mul_u32_shr16(m->hold, (uint32_t)tscale);
    s->hold = (uint8_t)(hold > 255 ? 255 : hold);
    s->tcoef = opz_decay_coef(tdecay);
    uint32_t tg = opz_mul_u32_shr16((uint32_t)m->tgain << 23, (uint32_t)(49152 + (p2 >> 1)));   /* x0.75 .. 1.25 */
    s->tpeak = opz_mul_q31(amp, tg > OPZ_Q31_ONE ? OPZ_Q31_ONE : (int32_t)tg);
    s->tstep = m->attack ? opz_mul_q31(s->tpeak, OPZ_Q31_ONE - opz_decay_coef(m->attack * 64)) : 0;
    s->cb = opz_decay_coef((int32_t)opz_mul_u32_shr16(m->bdecay, (uint32_t)opz_exp2_q16(p2 - 32768)));
    int32_t bv = (OPZ_Q31_ONE >> 1) + (amp >> 1);                 /* bursts x0.5 .. 1 by velocity */
    s->bgain = opz_mul_q31(opz_mul_q31(amp, bv), (int32_t)m->bgain << 23);

    int32_t tilt = oct * 65536;
    int32_t ftilt = tilt + (p2 - 32768) * 3 / 2 + (vel - 127) * 258;
    s->mode = m->mode & 15;
    int32_t x = half_angle(tilt_inc(m->fc, ftilt, INC_HZ(30), INC_HZ(7190)));
    s->f15 = opz_shl_q31(sin30(x), 2) >> 16;                      /* 2 sin(pi fc / fs) <= 0.98, Q15 */
    int32_t q = m->q < 8 ? 8 : m->q > 185 ? 185 : m->q;
    s->q15 = q << 7;                                              /* q / 2, Q15 */
    s->g15 = (m->mode & 3) == F_BP ? (q >= 128 ? 32767 : q << 8) : 16384;   /* band-pass: unity peak */

    int32_t r = opz_decay_coef((int32_t)opz_mul_u32_shr16(m->body, (uint32_t)opz_exp2_q16((p2 - 32768) * 2)));
    s->ga = opz_mul_q31(amp, (int32_t)m->g1 << 23);
    s->gb = opz_mul_q31(amp, (int32_t)m->g2 << 23);
    body_coef(m->f1, tilt, r, s->ga, &s->a1a, &s->a2a, &s->kicka);
    body_coef(m->f2, tilt, r, s->gb, &s->a1b, &s->a2b, &s->kickb);
    if (!m->f1) s->ga = 0;
    if (!m->f2) s->gb = 0;
    s->body = (s->kicka | s->kickb) != 0;
    if (!s->body) s->y1a = s->y2a = s->y1b = s->y2b = s->ebody = 0;   /* a retrigger on a model without one */
    int32_t r64 = r;
    for (int i = 0; i < 6; i++) r64 = opz_mul_q31(r64, r64);
    s->rbody64 = r64;

    s->et = 0;                                                     /* the tail waits for the last burst */
    tail_stage(s, T_WAIT);
    s->active = 1;
    s->fading = 0;
    if (params) opz_mix_start(&s->mix, params);
    else s->mix.l = s->mix.r = OPZ_MIX_UNITY;
    burst(s);                                                      /* the first burst now */
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                       /* one-shots: the hit decays on its own */
}

/* ---- render ------------------------------------------------------------------------------------------------- */

#define NOINLINE static __attribute__((noinline))

#define ALWAYS_INLINE static inline __attribute__((always_inline))

/* n samples of filtered noise under the burst and tail envelopes, plus the two body resonators (with_body), added
 * into the interleaved stereo buffer that opz_mix_stereo adds into out. A voice is computed in a range where 1.0 is
 * OPZ_VOICE_PEAK and saturates there once, then is shifted down by 2: no hit peaks above -12 dBFS. */
ALWAYS_INLINE void voice_run(State *s, fract32 *out, int n, int with_body)
{
    int mode = s->mode & 3, boost = s->mode >> 2;
    uint32_t seed = s->seed;
    int32_t lo = s->lo, bd = s->bd, f = s->f15, q = s->q15, g = s->g15;
    int32_t eb = s->eb, cb = s->cb, bl = s->bl15, br = s->br15, et = s->et, ct = s->ct, st = s->st;
    int32_t y1a = s->y1a, y2a = s->y2a, y1b = s->y1b, y2b = s->y2b;
    for (int i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        int32_t x = opz_mul_q31_q15((int32_t)seed, g);
        lo = opz_add_q31(lo, opz_mul_q31_q15(bd, f));
        int32_t hi = opz_sub_q31(opz_sub_q31(x, lo), opz_shl_q31(opz_mul_q31_q15(bd, q), 1));
        bd = opz_add_q31(bd, opz_mul_q31_q15(hi, f));
        int32_t y = opz_shl_q31(mode == F_BP ? bd : mode == F_HP ? hi : lo, boost);
        eb = opz_mul_q31(eb, cb);
        et = opz_add_q31(opz_mul_q31(et, ct), st);
        int32_t vl = opz_mul_q31_q15(y, opz_add_q31(opz_mul_q31_q15(eb, bl), et) >> 16);
        int32_t vr = opz_mul_q31_q15(y, opz_add_q31(opz_mul_q31_q15(eb, br), et) >> 16);
        if (with_body) {
            int32_t ya = opz_sub_q31(opz_shl_q31(opz_mul_q31(s->a1a, y1a), 1), opz_mul_q31(s->a2a, y2a));
            int32_t yb = opz_sub_q31(opz_shl_q31(opz_mul_q31(s->a1b, y1b), 1), opz_mul_q31(s->a2b, y2b));
            y2a = y1a;
            y1a = ya;
            y2b = y1b;
            y1b = yb;
            int32_t v = opz_add_q31(ya, yb);
            vl = opz_add_q31(vl, v);
            vr = opz_add_q31(vr, v);
        }
        out[2 * i] = vl >> 2;
        out[2 * i + 1] = vr >> 2;
    }
    s->seed = seed;
    s->lo = lo;
    s->bd = bd;
    s->eb = eb;
    s->et = et;
    if (with_body) {
        s->y1a = y1a;
        s->y2a = y2a;
        s->y1b = y1b;
        s->y2b = y2b;
    }
}

NOINLINE void noise_run(State *s, fract32 *out, int n) { voice_run(s, out, n, 0); }
NOINLINE void body_run(State *s, fract32 *out, int n) { voice_run(s, out, n, 1); }

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                                /* tone comes from the key's model */
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    if (!s->active || s->model >= 24) { OPZ_ADSR_STAGE(env) = 0; return; }

    if (OPZ_ADSR_STAGE(env) >= 5 && !s->fading) {
        /* The host wants this voice for a new note (stage 5/6) and starts it when we reach stage 0: fade out in a
         * few milliseconds instead of ringing on. */
        int32_t k = opz_decay_coef(32);
        s->fading = 1;
        s->bursts = 0;
        s->cb = s->tcoef = k;
        tail_stage(s, T_DECAY);
        s->a1a = opz_mul_q31(s->a1a, k);
        s->a1b = opz_mul_q31(s->a1b, k);
        s->a2a = opz_mul_q31(s->a2a, opz_mul_q31(k, k));
        s->a2b = opz_mul_q31(s->a2b, opz_mul_q31(k, k));
        s->rbody64 = opz_mul_q31(s->rbody64, 0x1152aaa4);           /* x k^64 = e^-2 */
    }

    fract32 st[2 * OPZ_BLOCK_FRAMES];                               /* the voice, interleaved stereo */
    int i = 0;
    while (i < OPZ_BLOCK_FRAMES) {
        int n = OPZ_BLOCK_FRAMES - i;
        if (s->bursts && s->timer < n) n = s->timer;
        if (n > 0) {
            if (s->body) body_run(s, st + 2 * i, n);
            else noise_run(s, st + 2 * i, n);
            i += n;
        }
        if (s->bursts) {
            s->timer -= n;
            if (s->timer <= 0) burst(s);
        }
    }
    opz_mix_stereo(out, st, &s->mix, params);                      /* level and pan, into interleaved out */

    if ((s->tstage == T_ATTACK || s->tstage == T_HOLD) && --s->tcount <= 0) {
        if (s->tstage == T_ATTACK && s->hold) tail_stage(s, T_HOLD);
        else tail_stage(s, T_DECAY);
    }
    s->ebody = opz_mul_q31(s->ebody, s->rbody64);
    if (s->body && !s->bursts && s->ebody < THRESHOLD) {         /* the bodies have died away: noise only */
        s->body = 0;
        s->y1a = s->y2a = s->y1b = s->y2b = s->ebody = 0;
    }
    if (!s->bursts && s->tstage == T_DECAY && s->et < THRESHOLD && s->eb < THRESHOLD && s->ebody < THRESHOLD) {
        s->active = 0;
        OPZ_ADSR_STAGE(env) = 0;                                   /* free the voice */
    }
}

OPZ_EXPORTS_INSTRUMENT();
