/*
 * ensemble — a 70s string machine: divide-down style string registrations through a lush three-phase ensemble.
 *
 *   P1        registration  a morph along five registrations, each a mix of three footages (8' saw, 8' square,
 *                           4' saw) through a two-pole tone low-pass:
 *                             0    cello        8' saw + 8' square (hollow, woody), tone at 600 Hz
 *                             64   dark viola   8' saw + a little square, tone at 1.3 kHz
 *                             128  viola        8' saw alone, tone at 2.8 kHz
 *                             192  violin       8' saw + 4' saw at 0.43, tone at 6 kHz
 *                             255  full         8' saw + 4' saw at equal level, tone at 16 kHz (open)
 *                           the tone follows the keyboard by half (an octave every two octaves) and velocity
 *   P2        ensemble      0 .. 76: the three ensemble voices fade in while the dry oscillator fades out (from 76
 *                           on the sound is all ensemble, like the classic machines); meanwhile their modulation
 *                           depth grows to the classic setting at 153: a slow LFO at 0.63 Hz (+-6 cents per voice)
 *                           and a fast one at 6.3 Hz (+-4 cents), the three voices 120 degrees apart: dry -> lush.
 *                           153 .. 255: lush -> seasick: a slow wow common to all voices grows to +-18 cents at
 *                           0.42 Hz, the fast vibrato deepens to +-7 cents at 5.6 Hz (each voice +-22 .. 30 cents
 *                           in all). Level per channel within about +-2 dB over the range (README)
 *   velocity                level (0.4 .. 1: string machines barely respond to touch) and tone (+-0.6 octave)
 *   attack..release         host envelope
 *
 * The ensemble. A string machine's ensemble is three copies of the signal through short delay lines whose delays
 * are swept by a slow and a fast LFO, the three copies 120 degrees apart. Delaying a periodic waveform by d(t) is
 * the same as running the oscillator with its phase moved back by f0 d(t), and the ensemble is linear, so the
 * chorus of the summed chord is the sum of each voice's own "chorus". So every voice here runs four phases of the
 * same oscillator - the dry one and three ensemble taps - and each tap's phase follows f0 times its delay
 * d_k(t) = A_slow sin(phi_slow + k 120 deg) + A_fast sin(phi_fast + k 120 deg) (40 and 2.5 samples at the classic
 * setting), applied as a frequency offset per block (the delay's change over the block), so turning the depth never
 * makes the pitch jump. Identical to a delay-line ensemble for steady notes; it needs no delay memory at all
 * (instruments get no large buffer and the voice state is 280 bytes), is alias-free like the dry oscillator, and
 * costs almost nothing per tap (below). The LFO phases are shared by every voice of a track (a new voice picks
 * them up at note-on), so a chord swims together as through one ensemble.
 * Seasick deepens a common wow rather than the three-phase sweep: three copies 120 degrees apart whose phase
 * swings reach about a turn cancel each other's fundamental on some notes (the mean of the three phasors is
 * 3 J0(beta)), a hollow, quieter note; a wow moves all copies together and cannot.
 *
 * Stereo: the dry phase sits in the middle; tap 1 is panned left (L 1, R 0.25), tap 2 in the middle (0.625 each),
 * tap 3 right (0.25, 1). Mono is the sum of all four phases; the side signal is the difference of the outer taps.
 * At the fundamental the taps differ by only f0 times about a millisecond of delay, so low notes stay almost mono
 * (side -15 .. -25 dB below mid at C1 .. C2) while the upper harmonics spread wide (side -4 .. -7 dB at C4 and up),
 * the way a real ensemble's width grows with frequency. On the bass track the width is halved.
 *
 * How it stays cheap (the trick from engines/super): all three footages jump only at the 8' phase 0 and 1/2, so a
 * weighted sum of them is a straight ramp plus two kinds of steps. Per block each of the four phases writes its
 * steps and their two-sample PolyBLEP residuals into shared mid/side event buffers; per sample only mid and side
 * are integrated, filtered (two one-poles each) and added into out with the envelope. Every block starts again
 * from the exact phases, so the integration's rounding never accumulates. The host envelope is advanced per block
 * in closed form and applied as a ramp (as in engines/vowel).
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine.
 *
 * After the classic string ensembles (Solina / ARP String Ensemble, Eminent 310: three BBD lines on a slow and a
 * fast LFO 120 degrees apart) and the string machine of Mutable Instruments' Plaits 1.2; no code from either.
 * Original code, MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

#define N OPZ_BLOCK_FRAMES
#define COPIES 4                /* the dry phase and three ensemble taps */
#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define INC_MIN 0x20000u        /* 1.3 Hz: below any note; keeps opz_blep_recip exact for every tap */
#define INC_MAX 0x40000000u     /* 11 kHz */
#define INC_4LO 243478872u      /* 2.5 kHz: the 4' (an octave up) starts to fade ... */
#define INC_4HI 486957743u      /* ... and is gone at 5 kHz */
#define FC_MAX 926398           /* tone corner limits, log2(Hz) in Q16: 18 kHz */
#define FC_MIN 414314           /* 80 Hz */
#define LOG2_W 802700           /* log2(fs ln 2 / 2 pi) in Q16: w / ln 2 = 2^(log2 fc - this) */
#define DD_MAX (4 << 16)        /* a tap's delay moves by at most 4 samples a block (seasick is ~1) */
#define SQRT3_2 28378           /* sqrt(3) / 2, Q15 */
#define OUT_GAIN 0x16100000     /* calibration: ~-21 dBFS RMS per channel at velocity 127, see README */

typedef struct State {
    uint32_t ph[COPIES];    /* 8' phases: 0 dry, 1 .. 3 the taps (left, middle, right) */
    uint32_t lfo[2];        /* ensemble LFO phases, slow and fast (this voice's copy of the track's) */
    int32_t sn[2][3];       /* the LFOs at the three tap phases at the end of the last block, Q30 */
    fract32 m1, m2, s1, s2; /* tone low-pass states, two poles on mid and on side */
    int32_t amp;            /* velocity gain, Q15 */
    int32_t key;            /* tone offset from key tracking and velocity, Q16 octaves */
    OpzMix mix;             /* the track's level and pan (opz_mix.h) */
    uint8_t track;
    uint8_t fresh;          /* first block after note-on: place the taps */
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes (the OS keeps the rest)");

/* One registration: footage levels (Q15; a + b + c <= 32767, since the step at phase 0 is a + b + c), level trim
 * (Q15) and tone corner (log2 Hz, Q16). */
typedef struct Reg {
    int16_t a, b, c;        /* 8' saw, 8' square, 4' saw */
    int16_t gain;
    int32_t fc;
} Reg;

/* P1 0, 64, 128, 192, 256 (255). Gains even out the RMS level (measured in opzsim at C3 .. C5, see README). */
static const Reg k_reg[5] = {
    { 16384, 16383,     0, 26330, 604820 },     /* cello: 600 Hz */
    { 22938,  9829,     0, 26029, 677924 },     /* dark viola: 1.3 kHz */
    { 32767,     0,     0, 29543, 750467 },     /* viola: 2.8 kHz */
    { 22938,     0,  9829, 32580, 822526 },     /* violin: 6 kHz */
    { 16384,     0, 16383, 32767, 915262 },     /* full: 16 kHz */
};

/* Level trim over P2 (17 points, Q15): the four phases add almost in phase at low depth and in power when the
 * taps swim apart; this keeps the level even across dry -> lush -> seasick. */
static const int16_t k_comp[17] = {
    32767, 27255, 24291, 22410, 21157, 20439, 21157, 21525, 21900,
    22027, 22410, 23198, 23875, 24291, 24013, 23875, 23738,
};

/* Event buffers of one block (steps and BLEP residuals, as differences), mid and side; unsigned, because the
 * sums wrap modulo 2^32 like the integration that reads them. Index N takes events that fall after the block and
 * is never read. Render runs one voice at a time, so all voices share them; pass_out
 * leaves them zeroed. */
static uint32_t s_em[N + 1], s_es[N + 1];

/* The ensemble LFO phases of each track at the start of the latest block rendered: a new voice starts from them. */
static uint32_t s_lfo[16][2];

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x2545f491u;

static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
}

/* 0 .. 65536 (Q16, 1.0 included) -> Q31, 1.0 saturated */
ALWAYS_INLINE int32_t q16_q31(int32_t x) { return x >= 65536 ? OPZ_Q31_ONE : x << 15; }

ALWAYS_INLINE int32_t lerp15(int32_t a, int32_t b, int32_t f) { return a + (((b - a) * f) >> 15); }

/* Steps of one phase in this block where it crosses 0 (call with phase + 1/2 for the 1/2 crossing): a drop of
 * 2 w on the mid bus (wm) and the side bus (ws), with its two-sample PolyBLEP residual, all as differences (the
 * output integrates them). p is the phase at sample 0, inc the increment, recip = opz_blep_recip(inc).
 *
 * The last sample before the crossing (phase >= 2^32 - inc) is corrected by -w (1 - x)^2 with x the distance to
 * the crossing in samples, the first one after it by +w x^2 (the polynomial of opz_saw_blep, as in engines/super). */
static void step_events(uint32_t p, uint32_t inc, uint32_t recip, int32_t wm, int32_t ws)
{
    if (p < inc) {                                           /* sample 0 follows a crossing in the last block */
        fract32 y = OPZ_Q31_ONE - (int32_t)opz_mul_u32_shr16(p, recip);
        uint32_t bm = (uint32_t)opz_mul_q31(opz_mul_q31(wm, y), y), bs = (uint32_t)opz_mul_q31(opz_mul_q31(ws, y), y);
        s_em[0] += bm; s_em[1] -= bm;
        s_es[0] += bs; s_es[1] -= bs;
    }
    int j = 0;                                               /* p is the phase at sample j */
    for (;;) {
        uint32_t np = ~p;                                    /* the crossing comes after floor(np / inc) samples */
        if (inc < (1u << 24) && np >= (inc << 8)) return;    /* > 256 samples away; keeps the estimate in range */
        uint32_t q = opz_mul_u32_shr16(np >> 7, recip) >> 24;   /* floor(np / inc), give or take one */
        uint32_t rem = np - q * inc;                         /* exact modulo 2^32 */
        if (rem >= inc) {
            if (rem >= 0x80000000u) { q--; rem += inc; }
            else { q++; rem -= inc; }
        }
        int jb = j + (int)q;                                 /* last sample before the crossing */
        if (jb >= N) return;
        fract32 x = (fract32)opz_mul_u32_shr16(rem, recip);
        int32_t am = opz_mul_q31(wm, x), as = opz_mul_q31(ws, x);
        uint32_t bm = (uint32_t)opz_mul_q31(am, x), bs = (uint32_t)opz_mul_q31(as, x);
        uint32_t cm = (uint32_t)wm - 2u * (uint32_t)am + bm;   /* w (1 - x)^2 */
        uint32_t cs = (uint32_t)ws - 2u * (uint32_t)as + bs;
        s_em[jb] -= cm; s_em[jb + 1] += cm;
        s_es[jb] -= cs; s_es[jb + 1] += cs;
        if (jb + 1 >= N) return;                             /* the crossing itself falls in the next block */
        s_em[jb + 1] += bm - 2u * (uint32_t)wm; s_em[jb + 2] -= bm;
        s_es[jb + 1] += bs - 2u * (uint32_t)ws; s_es[jb + 2] -= bs;
        p = inc - 1u - rem;                                  /* phase at sample jb + 1, after the crossing */
        j = jb + 1;
    }
}

/* The four copies of one block: each one's value at sample 0 and slope (summed into acc: mid value, mid slope, side
 * value, side slope) and its steps into the event buffers. a, b, c: footage levels (Q15, a + b + c <= 32767);
 * wm, ws: each copy's mid and side weight (Q31); the phases advance by 64 increments. */
static void copies(uint32_t *ph, const uint32_t *incs, const int32_t *wm, const int32_t *ws, int32_t a, int32_t b,
                   int32_t c, uint32_t *acc)
{
    /* the footages jump at phase 0 (saws down 2, square down 2) and at 1/2 (4' saw down 2, square up 2) */
    int32_t a31 = a << 16, b31 = b << 16, c31 = c << 16;
    int32_t w0 = (a + b + c) * 65536, w1 = (c - b) * 65536;
    uint32_t m = 0, dm = 0, sd = 0, ds = 0;
    for (int k = 0; k < COPIES; k++) {
        uint32_t p = ph[k], ik = incs[k];
        ph[k] = p + ik * N;
        if (wm[k] == 0) continue;                            /* dry at full ensemble, taps at P2 0 */
        int32_t y = opz_mul_q31(a31, (int32_t)(p ^ 0x80000000u))
                  + opz_mul_q31(b31, p < 0x80000000u ? -OPZ_Q31_ONE : OPZ_Q31_ONE)
                  + opz_mul_q31(c31, (int32_t)((p << 1) ^ 0x80000000u));
        int32_t sl = opz_mul_q31(a31, (int32_t)ik) + 2 * opz_mul_q31(c31, (int32_t)ik);
        m += (uint32_t)opz_mul_q31(wm[k], y);
        sd += (uint32_t)opz_mul_q31(ws[k], y);
        dm += (uint32_t)opz_mul_q31(wm[k], sl);
        ds += (uint32_t)opz_mul_q31(ws[k], sl);
        uint32_t rc = opz_blep_recip(ik);
        if (w0) step_events(p, ik, rc, opz_mul_q31(wm[k], w0), opz_mul_q31(ws[k], w0));
        if (w1) step_events(p + 0x80000000u, ik, rc, opz_mul_q31(wm[k], w1), opz_mul_q31(ws[k], w1));
    }
    acc[0] = m; acc[1] = dm; acc[2] = sd; acc[3] = ds;
}

/* The LFO at the three tap phases (0, 120, 240 degrees), Q30: half scale, so that opz_sine's 0.1 % error can
 * never take a sum past full scale. */
static void lfo3(uint32_t ph, int32_t *t)
{
    fract32 sn = opz_sine(ph) >> 1, cs = opz_sine(ph + 0x40000000u) >> 1;
    fract32 h = sn >> 1, r = opz_mul_q31_q15(cs, SQRT3_2);
    t[0] = sn;
    t[1] = r - h;
    t[2] = -r - h;
}

/* The host envelope 64 samples on, in closed form (engines/vowel: s = 0 by squaring c six times, c = 1 with the
 * step frozen for the block, anything else - a fade with a step - the exact 64 steps). */
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

/* Integrate mid and side from slope + events, two-pole tone on both, L = M + S and R = M - S with the envelope as
 * a ramp from l0 to l1, into the interleaved block buffer st that opz_mix_stereo adds into out. */
static void pass_out(fract32 *st, State *s, uint32_t m, uint32_t dm, uint32_t sd, uint32_t ds, int32_t g,
                     int32_t l0, int32_t l1)
{
    fract32 m1 = s->m1, m2 = s->m2, s1 = s->s1, s2 = s->s2;
    int32_t lv = l0, dl = (l1 - l0) >> 6;
    for (int i = 0; i < N; i++) {
        m += dm + s_em[i];                                   /* wraps modulo 2^32; in range at every sample */
        sd += ds + s_es[i];
        s_em[i] = 0;
        s_es[i] = 0;
        m1 = opz_add_q31(m1, opz_mul_q31_q15(opz_sub_q31((fract32)m, m1), g));
        m2 = opz_add_q31(m2, opz_mul_q31_q15(opz_sub_q31(m1, m2), g));
        s1 = opz_add_q31(s1, opz_mul_q31_q15(opz_sub_q31((fract32)sd, s1), g));
        s2 = opz_add_q31(s2, opz_mul_q31_q15(opz_sub_q31(s1, s2), g));
        int32_t lq = lv >> 16;
        st[2 * i] = opz_mul_q31_q15(opz_add_q31(m2, s2), lq);
        st[2 * i + 1] = opz_mul_q31_q15(opz_sub_q31(m2, s2), lq);
        lv += dl;
    }
    s->m1 = m1; s->m2 = m2; s->s1 = s1; s->s2 = s2;
}

/* ---- entry points ------------------------------------------------------------------------------------------ */

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
    s->track = (uint8_t)(track & 15);
    s->fresh = 1;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    s->ph[0] = rnd();                                        /* free-running: any phase at note-on */
    s->m1 = s->m2 = s->s1 = s->s2 = 0;
    int32_t vel = velocity > 127 ? 127 : velocity;
    s->amp = 13107 + ((vel * 19660) >> 7);                   /* 0.4 .. 1.0, Q15 */
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    s->key = (note - 60) * 2731 + (vel - 100) * 310;          /* half key tracking, +-0.6 octave of velocity */
    s->fresh = 1;
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

    /* ---- per block ------------------------------------------------------------------------------------------ */
    /* All phases run at the block's middle frequency: a glide moves in 64-frame steps (1.5 ms), phase-continuous. */
    uint32_t inc = pitch + (uint32_t)(pitchSlope * (N / 2));
    if ((int32_t)inc < (int32_t)INC_MIN) inc = INC_MIN;
    if (inc > INC_MAX) inc = INC_MAX;

    /* registration: five anchors 64 apart on P1 */
    int32_t x1 = opz_p16(params[OPZ_P_P1]);
    int seg = x1 >> 14;
    int32_t f1 = (x1 & 0x3fff) << 1;
    const Reg *r0 = &k_reg[seg], *r1 = r0 + 1;
    int32_t a = lerp15(r0->a, r1->a, f1), b = lerp15(r0->b, r1->b, f1), c = lerp15(r0->c, r1->c, f1);
    int32_t trim = lerp15(r0->gain, r1->gain, f1);
    int32_t fc = r0->fc + ((((r1->fc - r0->fc) >> 4) * f1) >> 11) + s->key;
    if (inc > INC_4LO)                                       /* the 4' fades out above 2.5 kHz (5 kHz harmonics) */
        c = inc >= INC_4HI ? 0 : (c * ((((int32_t)((INC_4HI - inc) >> 13)) * 36124) >> 15)) >> 15;
    if (fc > FC_MAX) fc = FC_MAX;
    if (fc < FC_MIN) fc = FC_MIN;
    int32_t w = opz_exp2_q16(fc - LOG2_W);                   /* 2 pi fc / fs / ln 2, Q16 */
    int32_t g = (65536 - opz_exp2_q16(-w)) >> 1;             /* one-pole: 1 - exp(-2 pi fc / fs), Q15 */
    if (g > 32767) g = 32767;

    /* ensemble: wet fade-in u, classic depth z, seasick part t (Q16) */
    int32_t x2 = opz_p16(params[OPZ_P_P2]);
    int32_t u = (int32_t)opz_mul_u32_shr16((uint32_t)x2, 218453u);       /* P2 / 0.3 */
    if (u > 65536) u = 65536;
    int32_t z = (int32_t)opz_mul_u32_shr16((uint32_t)x2, 109227u);       /* P2 / 0.6 */
    if (z > 65536) z = 65536;
    int32_t t = x2 > 39321 ? ((x2 - 39321) * 5) >> 1 : 0;                /* (P2 - 0.6) / 0.4 */
    if (t > 65536) t = 65536;
    int32_t amp_s = 40 * z + 15 * t;                         /* slow three-phase depth, samples Q16 */
    int32_t amp_f = ((5 * z) >> 1) + ((5 * t) >> 1);         /* fast three-phase depth, samples Q16 */
    int32_t amp_c = 170 * t;                                 /* common slow wow (every copy), samples Q16 */
    uint32_t rate_s = 3926827u - opz_mul_u32_shr16(1308942u, (uint32_t)t);   /* 0.63 -> 0.42 Hz, per block */
    uint32_t rate_f = 39268272u - opz_mul_u32_shr16(4363141u, (uint32_t)t);  /* 6.3 -> 5.6 Hz */

    /* LFOs: the tap delays' change over this block, as each tap's relative frequency offset */
    uint32_t *glfo = s_lfo[s->track];
    int fresh = s->fresh;
    if (fresh) {
        s->lfo[0] = glfo[0];
        s->lfo[1] = glfo[1];
        lfo3(s->lfo[0], s->sn[0]);
        lfo3(s->lfo[1], s->sn[1]);
        /* the taps start where their delays put them now: phase - f0 d */
        for (int k = 0; k < 3; k++) {
            int32_t d = 2 * (opz_mul_q31(amp_s, s->sn[0][k]) + opz_mul_q31(amp_f, s->sn[1][k]));
            uint32_t th = opz_mul_u32_shr16(inc, (uint32_t)(d < 0 ? -d : d));
            s->ph[k + 1] = d < 0 ? s->ph[0] + th : s->ph[0] - th;
        }
        s->fresh = 0;
    }
    glfo[0] = s->lfo[0];                                     /* the block's start: a fresh voice joins in step, */
    glfo[1] = s->lfo[1];                                     /* and rendering a block twice writes the same */
    s->lfo[0] += rate_s;
    s->lfo[1] += rate_f;
    int32_t ns[3], nf[3];
    lfo3(s->lfo[0], ns);
    lfo3(s->lfo[1], nf);
    uint32_t incs[COPIES];
    int32_t dc = 2 * opz_mul_q31(amp_c, ns[0] - s->sn[0][0]);   /* the wow moves all four copies together */
    if (dc > DD_MAX / 2) dc = DD_MAX / 2;
    if (dc < -DD_MAX / 2) dc = -DD_MAX / 2;
    incs[0] = inc - (uint32_t)opz_mul_q31((int32_t)inc, dc * 512);
    for (int k = 0; k < 3; k++) {
        int32_t dd = dc + 2 * (opz_mul_q31(amp_s, ns[k] - s->sn[0][k]) + opz_mul_q31(amp_f, nf[k] - s->sn[1][k]));
        if (dd > DD_MAX) dd = DD_MAX;
        if (dd < -DD_MAX) dd = -DD_MAX;
        s->sn[0][k] = ns[k];
        s->sn[1][k] = nf[k];
        uint32_t ik = inc - (uint32_t)opz_mul_q31((int32_t)inc, dd * 512);   /* more delay: lower frequency */
        if ((int32_t)ik < (int32_t)INC_MIN) ik = INC_MIN;
        if (ik > INC_MAX) ik = INC_MAX;
        incs[k + 1] = ik;
    }

    /* copy weights: dry in the middle, taps left / middle / right; velocity, trims; Q31 */
    int32_t base = opz_mul_q31_q15(opz_mul_q31_q15(opz_mul_q31_q15(OUT_GAIN, s->amp), trim),
                                   k_comp[x2 >> 12] + (((k_comp[(x2 >> 12) + 1] - k_comp[x2 >> 12]) *
                                                        (x2 & 0xfff)) >> 12));
    int32_t gd = opz_mul_q31(base, q16_q31(65536 - u));
    int32_t gt = opz_mul_q31(base, q16_q31(u));
    int32_t tm = gt - (gt >> 2) - (gt >> 3);                 /* 0.625 */
    int32_t tsd = (gt >> 2) + (gt >> 3);                     /* 0.375 */
    if (s->track == OPZ_TRACK_BASS) tsd >>= 1;
    const int32_t wm[COPIES] = { gd, tm, tm, tm };
    const int32_t ws[COPIES] = { 0, tsd, 0, -tsd };

    uint32_t acc[4];
    copies(s->ph, incs, wm, ws, a, b, c, acc);

    /* The integration adds slope + event before each sample, sample 0 included: start one slope early. */
    int32_t l0 = OPZ_ADSR_LEVEL(env);
    int32_t l1 = env_block(l0, OPZ_ADSR_COEF(env), OPZ_ADSR_STEP(env));
    fract32 st[2 * N];                                       /* the voice, interleaved L R, before level and pan */
    pass_out(st, s, acc[0] - acc[1], acc[1], acc[2] - acc[3], acc[3], g, l0, l1);
    opz_mix_stereo(out, st, &s->mix, params);               /* level and pan, added into interleaved out */
    OPZ_ADSR_LEVEL(env) = l1;
}

OPZ_EXPORTS_INSTRUMENT();
