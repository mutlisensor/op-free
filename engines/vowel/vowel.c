/*
 * vowel — formant / choir synth: sung vowels, "aah" pads, talking leads, vocal basses.
 *
 *   P1        vowel       a smooth morph along hum [m] - oo [u] - oh [o] - ah [a] - eh [e] - ee [i] - ue [y] -
 *                         er [r], at P1 0, 36, 73, 109, 146, 182, 219, 255. Formant frequencies and bandwidths
 *                         are interpolated on a log scale, levels in dB
 *   P2        voice       0 .. 191: body size, every formant scaled together: x0.72 "giant" at 0, x1.0 male at
 *                         64, x1.18 female at 128, x1.36 child at 191. 192 .. 255 (breath zone): the size stays
 *                         at child and breath noise replaces the voice, a whisper at 255
 *   velocity              level, brightness (a 6 dB/octave tilt with its corner at 1.2 kHz .. 9 kHz, applied as
 *                         formant gains) and a short breathy "h" onset (louder notes, more breath)
 *   attack..release       host envelope
 *
 * Voice: a band-limited glottal pulse train (the derivative of a PolyBLEP saw: a flat line spectrum, three samples
 * per period) into four parallel band-pass formants (Chamberlin state-variable filters, unity gain at the peak,
 * alternating signs so neighbouring formants add instead of notching between their peaks). The input of each
 * formant is scaled by q x level, so a level change passes through the filter and never zippers.
 *
 * Musical at both ends of the keyboard:
 *   - high notes: F1 never sits below 1.11 f0 (sopranos tune F1 to the fundamental the same way), each formant
 *     stays above the one below, and bandwidths widen to at least 0.7 f0, so sparse harmonics are not missed
 *   - level: the output gain follows 1 / sqrt(f0 * max(f0, pi B1 / 2)), the RMS of a line spectrum through the
 *     formants; below C4, where the pulses ring apart, it meets the peaks half-way (README has the measurements)
 *   - formants are clamped to 7 kHz (the filters stay stable and accurate below fs / 6)
 *
 * Choir: every voice gets its own seeded "singer": a slow pitch drift (+-4 cents), a natural vibrato that fades
 * in after a quarter second (+-5 cents, 4.7 .. 5.7 Hz), a formant size offset and a stereo position. On the chord
 * track every voice is two singers (two pulse trains 7 .. 13 cents apart, each drifting on its own) through the
 * same formants, spread wider in stereo; the bass track is mono and centred.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine.
 *
 * Cost: between pulses the source is zero and each formant only rings (three multiplies a sample); the pulses are
 * found per block, so a second singer is almost free. Coefficients are recomputed per block only when P1, P2 or
 * the pitch (by more than 1.6 %) changed, and the host envelope is advanced per block in closed form (env_block). Formant table: gen_tables.py (published phonetic
 * averages). Original code, MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "vowel_tables.h"

#define N OPZ_BLOCK_FRAMES
#define NF 4
#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define OCT(x) ((int32_t)((x) * 65536.0 + ((x) < 0 ? -0.5 : 0.5)))   /* octaves, Q16 (constants only) */

typedef struct Form {
    int32_t lo, bp;         /* Chamberlin low-pass and band-pass states, Q31 */
} Form;

typedef struct Coef {
    int32_t f, q, qa;       /* 2 sin(pi F / fs), damping B / F, input gain +-q x level; Q15 */
} Coef;

typedef struct State {
    uint32_t ph[2];         /* singer phases */
    int32_t carry[2];       /* pulse samples that fell after the last block */
    Form fm[NF];
    Coef c[NF];
    uint32_t key_p1, key_p2, key_inc;   /* what the coefficients were computed for */
    int32_t g, gt, lb1h;    /* output gain and its target, log2 Q16; log2(B1) / 2 for the noise gain */
    int32_t gexp;           /* output gain = gm x 2^gexp */
    int32_t gl, gr;         /* gm x velocity x pan, Q15 */
    int32_t nbase;          /* noise input gain for "as loud as the voice", Q15 */
    int32_t breath;         /* breath share 0..1, Q15 */
    int32_t burst;          /* onset breath, Q15, decays per block */
    int32_t amp;            /* velocity, Q15 */
    int32_t pan;            /* -1..1, Q15 */
    int32_t tone;           /* velocity tone: corner of a one-pole low-pass, log2(Hz) Q16, applied as formant gains */
    uint32_t seed;          /* noise */
    uint32_t lfo[3], lfo_inc[3];        /* drift A, drift B, vibrato */
    int32_t vib;            /* vibrato depth fade-in, Q15 */
    int32_t vib_wait;       /* blocks before the vibrato fades in */
    int32_t detune;         /* singer spacing, frequency deviation Q31 */
    int32_t jitter;         /* formant size offset of this singer, Q16 octaves */
    uint8_t track, singers, fresh;
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");

/* Block buffers: the source and the formant sum, and the stretches of s_x that are not zero. Render runs one voice
 * at a time, so all voices share them; s_x is all zeros between renders (render clears what it wrote), s_y is
 * written before it is read. .bss keeps them inside the slot window. */
#define MAX_RUNS 40
static int32_t s_x[N + 2], s_y[N];
static uint8_t s_run[2][MAX_RUNS / 2][2], s_runs[MAX_RUNS][2];

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x6d2b79f5u;

#define INC_MIN 0x100000u           /* 10.7 Hz: below C0 bent down, and bounds the output gain */
#define INC_MAX 0x40000000u         /* 11 kHz */
#define LOG2_HZ_OF_INC (-OCT(16.5715))   /* log2(Hz) = log2(inc) + log2(44100) - 32 */
#define F_MAX OCT(12.7731)          /* 7 kHz */
#ifndef K_GAIN
#define K_GAIN OCT(13.12)           /* output gain constant: peaks ~-11 dBFS per voice at velocity 127 */
#endif
#ifndef K_KNEE
#define K_KNEE 27525                /* below C4 the gain gives up 0.42 octaves per octave (peaks vs RMS) */
#endif
#define LF0_KNEE OCT(8.03)          /* C4, log2(261.6 Hz) */
#define K_NOISE OCT(5.45)           /* breath noise ~1.5 dB under the voice (RMS): its crest is higher */
#define CENT 1240404                /* one cent as a frequency deviation, Q31 (ln 2 / 1200) */

static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
}

/* log2(x) in Q16 for 0 < x < 2^31: exponent from norm, log2(1 + m) ~ m (1.3466 - 0.3466 m) (0.002 octaves). */
static int32_t log2_q16(uint32_t x)
{
    int n = opz_norm32((int32_t)x);                          /* x << n in [2^30, 2^31) */
    uint32_t m = ((x << n) - 0x40000000u) >> 14;             /* Q16 */
    uint32_t t = 88252u - opz_mul_u32_shr16(22714u, m);
    return ((30 - n) << 16) + (int32_t)opz_mul_u32_shr16(m, t);
}

/* 2^(x / 65536) as Q15, clamped to 0 .. 32767 (x <= 0 octaves for a gain below 1). */
static int32_t exp2_q15(int32_t x)
{
    if (x < OCT(-14.9)) return 0;
    if (x > 0) x = 0;
    int32_t v = opz_exp2_q16(x) >> 1;
    return v > 32767 ? 32767 : v;
}

/* Table value (Q11) interpolated by fr (Q16), returned in Q16. */
static int32_t lerp11(int32_t a, int32_t b, int32_t fr) { return a * 32 + (((b - a) * fr) >> 11); }

/* P2 (0..65535) -> formant size, Q16 octaves; *breath gets the breath share, Q15. */
static int32_t p2_size(int32_t p, int32_t *breath)
{
    *breath = 0;
    if (p < 16448) return OCT(-0.4739) + ((p * 1933) >> 10);               /* x0.72 .. x1.0 */
    if (p < 32896) return ((p - 16448) * 974) >> 10;                        /* x1.0 .. x1.18 */
    if (p < 49344) return OCT(0.2388) + (((p - 32896) * 836) >> 10);        /* x1.18 .. x1.36 */
    int32_t b = ((p - 49344) * 132630) >> 16;
    *breath = b > 32767 ? 32767 : b;
    return OCT(0.4436);
}

/* Formant coefficients, the output gain and the noise gain for this P1, P2 and pitch. Per block, only on change. */
static void compute(State *s, uint32_t p1, uint32_t p2, uint32_t inc)
{
    int32_t lf0 = log2_q16(inc) + LOG2_HZ_OF_INC;
    int32_t size = p2_size(opz_p16(p2), &s->breath) + s->jitter;
    int32_t pos = opz_p16(p1) * (VOWELS - 1);
    int v = pos >> 16;
    int32_t fr = pos & 0xffff;
    const int16_t (*a)[3] = k_vowel[v], (*b)[3] = k_vowel[v + 1];
    int32_t lf_prev = 0, lb1 = 0;
    static const int32_t k_gap[NF] = { OCT(0.15), OCT(0.40), OCT(0.20), OCT(0.15) };
    for (int k = 0; k < NF; k++) {
        int32_t lf = lerp11(a[k][0], b[k][0], fr) + size;
        int32_t lb = lerp11(a[k][1], b[k][1], fr) + size;
        int32_t la = lerp11(a[k][2], b[k][2], fr);
        int32_t floor_ = (k == 0 ? lf0 : lf_prev) + k_gap[k];
        if (lf < floor_) lf = floor_;                        /* F1 above f0, each formant above the last */
        if (lf > F_MAX) lf = F_MAX;
        if (lb < lf0 - OCT(0.515)) lb = lf0 - OCT(0.515);    /* B >= 0.7 f0: no harmonic falls in a gap */
        if (k == 0) lb1 = lb;
        lf_prev = lf;
        uint32_t fq16 = (uint32_t)opz_exp2_q16(lf);          /* Hz, Q16 */
        s->c[k].f = opz_sine(opz_mul_u32_shr16(fq16, 48696)) >> 15;   /* 2 sin(pi F / fs): F / (2 fs) turns */
        int32_t q = exp2_q15(lb - lf);
        if (q > 0x7000) q = 0x7000;
        /* the velocity tone: an analog one-pole low-pass's gain at F, 1 / sqrt(1 + (F / fc)^2) */
        int32_t d = lf - s->tone + OCT(4);                   /* quarter octaves from -4 octaves, Q14 */
        if (d >= OCT(8)) la -= d - OCT(4);
        else if (d > 0) {
            int i = d >> 14;
            la -= k_tone[i] + (((k_tone[i + 1] - k_tone[i]) * (d & 0x3fff)) >> 14);
        }
        int32_t qa = (q * exp2_q15(la)) >> 15;
        s->c[k].q = q;
        s->c[k].qa = (k & 1) ? -qa : qa;
    }
    /* RMS of a flat line spectrum (harmonics f0 / fs) through a unity-peak formant of bandwidth B:
     * (f0 / fs) sqrt(max(1, pi B / (2 f0)) / 2), so gain ~ 1 / sqrt(f0 max(f0, pi B / 2)). */
    int32_t lw = lb1 + OCT(0.6515);
    int32_t g = K_GAIN - ((lf0 + (lw > lf0 ? lw : lf0)) >> 1);
    /* Below C4 the pulses ring apart and the crest factor grows by 3 dB per octave: meet peaks half-way */
    if (lf0 < LF0_KNEE) g -= ((LF0_KNEE - lf0) >> 4) * K_KNEE >> 12;
    if (s->singers == 2) g -= OCT(0.75);
    if (g < OCT(-6)) g = OCT(-6);
    if (g > OCT(14)) g = OCT(14);
    s->gt = g;
    s->lb1h = lb1 >> 1;
}

/* Output gain g (log2, Q16) -> shift, gains with velocity and pan, and the noise gain that matches it. */
static void set_gain(State *s, int32_t g)
{
    s->g = g;
    int32_t e = (g >> 16) + 1;
    if (e < 0) e = 0;
    s->gexp = e;
    int32_t gm = exp2_q15(g - (e << 16));
    int32_t ga = (gm * s->amp) >> 15;
    s->gl = (ga * (24576 - ((s->pan * 3) >> 3))) >> 15;    /* 0.75 (1 -+ pan / 2): at most 0.92 */
    s->gr = (ga * (24576 + ((s->pan * 3) >> 3))) >> 15;
    /* noise through the formants (power ~ pi B1 / fs) as loud as the voice: n ~ 1 / (gain sqrt(B1)) */
    int32_t ln = K_NOISE - g - s->lb1h;
    s->nbase = exp2_q15(ln > OCT(-1) ? OCT(-1) : ln);
}

/* ---- the three passes of a block ------------------------------------------------------------------------- */

/* One singer's glottal pulses in this block, added into s_x: the derivative of a PolyBLEP saw minus its slope,
 * three samples per wrap. With x = (2^32 - phase) / inc at the last sample before a wrap (opz_saw_blep's
 * corrections -(1 - x)^2 there and +x^2 after the wrap), the saw's differences less the slope are -(1 - x)^2,
 * -2 + x^2 + (1 - x)^2 and -x^2: a band-limited drop of 2, here scaled to 2^30 (two singers sum to at most 2^31).
 * Samples after the block go to carry[]; the stretches written go to run[] (sorted). g scales the pulses, Q15.
 * Returns the number of stretches. Finding the wraps follows super.c's saw_events. */
static int pulses(uint32_t p, uint32_t inc, uint32_t recip, int32_t g, uint8_t (*run)[2])
{
    int n = 0, j = 0;                                        /* p is the phase at sample j */
    for (;;) {
        uint32_t np = ~p;                                    /* the wrap comes after floor(np / inc) more samples */
        if (inc < (1u << 24) && np >= (inc << 8)) return n;  /* > 256 samples away; keeps the estimate in range */
        uint32_t q = opz_mul_u32_shr16(np >> 7, recip) >> 24;   /* floor(np / inc), give or take one */
        uint32_t rem = np - q * inc;
        if (rem >= inc) {
            if (rem >= 0x80000000u) { q--; rem += inc; }
            else { q++; rem -= inc; }
        }
        int jb = j + (int)q;                                 /* last sample before the wrap */
        if (jb >= N) return n;
        uint32_t xu = opz_mul_u32_shr16(rem, recip);
        fract32 x = xu > OPZ_Q31_ONE ? OPZ_Q31_ONE : (fract32)xu;   /* Q31; recip is a hair above 2^31 / inc */
        fract32 a = opz_mul_q31(x, x), b = opz_mul_q31(OPZ_Q31_ONE - x, OPZ_Q31_ONE - x);
        int32_t w0 = -(b >> 2), w1 = (a >> 2) + (b >> 2) - 0x40000000, w2 = -(a >> 2);
        if (g < 32767) { w0 = opz_mul_q31_q15(w0, g); w1 = opz_mul_q31_q15(w1, g); w2 = opz_mul_q31_q15(w2, g); }
        s_x[jb] = opz_add_q31(s_x[jb], w0);                  /* s_x has two spare samples after the block */
        s_x[jb + 1] = opz_add_q31(s_x[jb + 1], w1);
        s_x[jb + 2] = opz_add_q31(s_x[jb + 2], w2);
        run[n][0] = (uint8_t)jb;
        run[n][1] = (uint8_t)(jb + 3 < N ? jb + 3 : N);
        n++;
        if (jb + 1 >= N || n == MAX_RUNS / 2) return n;    /* inc < 2^31 / 1.9: at most 17 wraps a block */
        p = inc - 1u - rem;                                  /* phase at sample jb + 1, after the wrap */
        j = jb + 1;
    }
}

/* The block's source in s_x and its non-zero stretches in s_runs: carried pulse samples, one or two singers, and
 * breath noise (vg scales the voice, ng the noise, Q15). Returns the number of stretches. */
static int source(State *s, const uint32_t *inc, const uint32_t *rc, int two, int32_t vg, int32_t ng)
{
    s_x[0] = s->carry[0];
    s_x[1] = s->carry[1];
    int na = pulses(s->ph[0], inc[0], rc[0], vg, s_run[0]);
    int nb = two ? pulses(s->ph[1], inc[1], rc[1], vg, s_run[1]) : 0;
    s->ph[0] += inc[0] * N;
    s->ph[1] += inc[1] * N;
    int nr = 0;
    if (ng) {
        uint32_t seed = s->seed;
        for (int i = 0; i < N; i++) {
            seed = seed * 1664525u + 1013904223u;
            s_x[i] = opz_add_q31(s_x[i], opz_mul_q31_q15((int32_t)seed >> 1, ng));
        }
        s->seed = seed;
        s_runs[0][0] = 0;
        s_runs[0][1] = N;
        nr = 1;
    } else {
        int ia = 0, ib = 0, end = -1;
        if (s->carry[0] | s->carry[1]) {
            s_runs[0][0] = 0;
            s_runs[0][1] = 2;
            end = 2;
            nr = 1;
        }
        while (ia < na || ib < nb) {                         /* merge the two sorted lists */
            const uint8_t *r = (ib >= nb || (ia < na && s_run[0][ia][0] <= s_run[1][ib][0])) ? s_run[0][ia++]
                                                                                              : s_run[1][ib++];
            if (r[0] <= end) {
                if (r[1] > end) s_runs[nr - 1][1] = r[1], end = r[1];
            } else {
                s_runs[nr][0] = r[0];
                s_runs[nr][1] = r[1];
                end = r[1];
                nr++;
            }
        }
    }
    s->carry[0] = s_x[N];
    s->carry[1] = s_x[N + 1];
    s_x[N] = s_x[N + 1] = 0;
    return nr;
}

/* One formant over the block: Chamberlin SVF, band-pass output added into s_y (written on the first). Between the
 * stretches of s_x the input is zero and the filter only rings: three multiplies instead of four. */
#define SVF_RING()                                                                                             \
    lo = opz_add_q31(lo, opz_mul_q31_q15(bp, f));                                                              \
    bp = opz_sub_q31(bp, opz_mul_q31_q15(opz_add_q31(lo, opz_mul_q31_q15(bp, q)), f))
#define SVF_IN(x)                                                                                              \
    lo = opz_add_q31(lo, opz_mul_q31_q15(bp, f));                                                              \
    bp = opz_add_q31(bp, opz_mul_q31_q15(opz_sub_q31(opz_sub_q31(opz_mul_q31_q15(x, qa), lo),                \
                                                     opz_mul_q31_q15(bp, q)), f))

ALWAYS_INLINE void formant_body(Form *st, const Coef *c, int nr, int first)
{
    int32_t lo = st->lo, bp = st->bp, f = c->f, q = c->q, qa = c->qa;
    int i = 0;
    for (int r = 0; r <= nr; r++) {
        int a = r < nr ? s_runs[r][0] : N, b = r < nr ? s_runs[r][1] : N;
        for (; i < a; i++) {
            SVF_RING();
            s_y[i] = first ? bp : opz_add_q31(s_y[i], bp);
        }
        for (; i < b; i++) {
            SVF_IN(s_x[i]);
            s_y[i] = first ? bp : opz_add_q31(s_y[i], bp);
        }
    }
    st->lo = lo; st->bp = bp;
}

__attribute__((noinline)) static void pass_formant(Form *st, const Coef *c, int nr, int first)
{
    if (first) formant_body(st, c, nr, 1);
    else formant_body(st, c, nr, 0);
}

/* The host envelope 64 samples on (sdk/docs/engine-abi.md §3: per sample r = c level, level = r + (1 - r^3) s),
 * in closed form instead of 64 steps:
 *   s = 0 (decay, release):      level c^64, c^64 by six squarings
 *   c = 1 (attack, sustain):     (1 - r^3) s hardly moves within a block: level + 64 ((1 - r^3) s + (r - level))
 *   otherwise (a fade with a step, a few blocks per voice steal): the exact 64 steps
 * The audio follows it as a straight line across the block, so the stages and thresholds the host sees are the
 * per-sample ones to within rounding, at a fraction of the cost. */
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

/* Output gain, the envelope as a ramp from l0 to l1, and pan, added into out. */
static void pass_out(fract32 *out, int e, int32_t gl, int32_t gr, int32_t l0, int32_t l1)
{
    int32_t lv = l0, dl = (l1 - l0) / N;
    for (int i = 0; i < N; i++) {
        int32_t y = opz_mul_q31_q15(s_y[i], lv >> 16);       /* gains first, the shift last: no early clip */
        out[2 * i] = opz_add_q31(out[2 * i], opz_shl_q31(opz_mul_q31_q15(y, gl), e));
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], opz_shl_q31(opz_mul_q31_q15(y, gr), e));
        lv += dl;
    }
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
    s->track = (uint8_t)track;
    s->singers = track == OPZ_TRACK_CHORD ? 2 : 1;
    s->tone = OCT(13.1288);                                  /* 9 kHz */
    s->fresh = 1;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note; (void)params;                                /* params may be NULL (engine-abi.md §3); not needed */
    State *s = state;
    int chord = s->singers == 2;
    s->ph[0] = 0xf8000000u + (rnd() >> 6);                   /* the first pulse within 2^27 / inc samples (2 ms at C1) */
    s->ph[1] = rnd();                                        /* the second singer anywhere in the period */
    for (int k = 0; k < NF; k++) s->fm[k].lo = s->fm[k].bp = 0;
    s->carry[0] = s->carry[1] = 0;
    s->seed = rnd() | 1u;
    /* drift 0.21 .. 0.47 Hz and 0.17 .. 0.43 Hz, vibrato 4.7 .. 5.7 Hz; per block: Hz x 64 / 44100 x 2^32 */
    s->lfo_inc[0] = 1309000u + (rnd() >> 16) * 25u;
    s->lfo_inc[1] = 1060000u + (rnd() >> 16) * 25u;
    s->lfo_inc[2] = 29295000u + (rnd() >> 16) * 95u;
    for (int k = 0; k < 3; k++) s->lfo[k] = rnd();
    s->vib = 0;
    s->vib_wait = 172;                                       /* 0.25 s */
    s->detune = chord ? (int32_t)(7 * CENT + (rnd() >> 16) * 114u) : 0;          /* 7 .. 13 cents */
    int32_t r = (int32_t)(rnd() >> 16) - 32768;              /* -1..1, Q15 */
    s->jitter = (r * (chord ? 2294 : 983)) >> 15;            /* +-0.035 / +-0.015 octaves */
    r = (int32_t)(rnd() >> 16) - 32768;
    s->pan = s->track == OPZ_TRACK_BASS ? 0 : (r * (chord ? 14746 : 4915)) >> 15;   /* +-0.45 / +-0.15 */
    int32_t vel = velocity > 127 ? 127 : velocity;
    s->amp = vel * 258;                                      /* Q15, 32766 at 127 */
    s->burst = (s->amp >> 8) * (s->amp >> 8);                /* vel^2 / 2, Q15: half the voice */
    s->tone = OCT(10.2288) + ((vel * 190054) >> 7);          /* fc = 1.2 kHz x 2^(2.9 vel) */
    s->fresh = 1;
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
    uint32_t inc = pitch + (uint32_t)(pitchSlope * (N / 2));  /* the block's middle: glides move in 1.5 ms steps */
    if ((int32_t)inc < (int32_t)INC_MIN) inc = INC_MIN;
    if (inc > INC_MAX) inc = INC_MAX;
    uint32_t p1 = params[OPZ_P_P1], p2 = params[OPZ_P_P2];
    uint32_t dk = inc > s->key_inc ? inc - s->key_inc : s->key_inc - inc;
    if (s->fresh || p1 != s->key_p1 || p2 != s->key_p2 || dk > (s->key_inc >> 6)) {
        compute(s, p1, p2, inc);
        s->key_p1 = p1; s->key_p2 = p2; s->key_inc = inc;
        if (s->fresh) set_gain(s, s->gt);
        s->fresh = 0;
    }
    /* The gain follows the pitch: down at once, up by at most 1/16 octave a block on a glide, so formant energy
     * left from a higher note is never amplified all at once. */
    if (s->g != s->gt) set_gain(s, s->gt - s->g > OCT(0.0625) ? s->g + OCT(0.0625) : s->gt);

    /* singers: drift, vibrato (fades in after vib_wait blocks) and spacing, as frequency deviations */
    for (int k = 0; k < 3; k++) s->lfo[k] += s->lfo_inc[k];
    if (s->vib_wait) s->vib_wait--;
    else if (s->vib < 32767 - 80) s->vib += 80;              /* ~0.6 s fade-in */
    int32_t vib = opz_mul_q31_q15(5 * CENT, (opz_tri15(s->lfo[2]) * s->vib) >> 15);
    uint32_t incs[2], rc[2];
    int32_t dev0 = opz_mul_q31_q15(4 * CENT, opz_tri15(s->lfo[0])) + vib - (s->detune >> 1);
    int32_t dev1 = opz_mul_q31_q15(4 * CENT, opz_tri15(s->lfo[1])) - vib + (s->detune >> 1);
    incs[0] = inc + (uint32_t)opz_mul_q31((int32_t)inc, dev0);
    incs[1] = inc + (uint32_t)opz_mul_q31((int32_t)inc, dev1);
    int two = s->singers == 2;
    rc[0] = opz_blep_recip(incs[0]);
    rc[1] = two ? opz_blep_recip(incs[1]) : 0;

    /* breath crossfade, about equal power: voice 1 - b^2, noise 1 - (1 - b)^2; plus the onset breath */
    int32_t b = s->breath, nb = 32767 - b;
    int32_t nsh = b ? 32767 - ((nb * nb) >> 15) + s->burst : s->burst;
    if (s->burst) s->burst = s->burst > 1000 ? (s->burst * 27) >> 5 : 0;  /* x0.84 per block: ~8 ms, 30 ms long */
    int32_t ng = (s->nbase * (nsh > 32767 ? 32767 : nsh)) >> 15;
    int32_t vg = 32767 - ((b * b) >> 15);
    int nr = source(s, incs, rc, two, vg, ng);
    for (int k = 0; k < NF; k++) pass_formant(&s->fm[k], &s->c[k], nr, k == 0);
    for (int r = 0; r < nr; r++)                             /* leave s_x all zeros */
        for (int i = s_runs[r][0]; i < s_runs[r][1]; i++) s_x[i] = 0;

    int32_t l0 = OPZ_ADSR_LEVEL(env);
    int32_t l1 = env_block(l0, OPZ_ADSR_COEF(env), OPZ_ADSR_STEP(env));
    pass_out(out, s->gexp, s->gl, s->gr, l0, l1);
    OPZ_ADSR_LEVEL(env) = l1;
}

OPZ_EXPORTS_INSTRUMENT();
