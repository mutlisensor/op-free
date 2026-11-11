/*
 * super — JP-8000-style supersaw: seven band-limited saws, detuned on the classic curve, spread in stereo.
 *
 *   P1        detune      the seven saws' spread on the JP-8000's non-linear curve: 0 near unison (outer saws
 *                         ±0.6 cents), 128 classic trance (±19 cents), 240 ±77 cents, 255 -202/+177 cents. The
 *                         stereo width opens over 0 .. 32 (mono at 0)
 *   P2        mix         0 .. 192: the six side saws come in on the JP-8000's mix curves (the centre saw alone at
 *                         0, the sides dominant at 192); 192 .. 255: a triangle sub one octave down fades in.
 *                         Level per channel stays the same over the whole range
 *   velocity              level, and a little brightness: a one-pole low-pass at 3 kHz .. 18 kHz (velocity
 *                         0 .. 127) with half key tracking
 *   attack..release       host envelope
 *
 * Stereo: the centre saw sits in the middle; the six side saws are panned in three pairs, each pair with one saw
 * on each side and the flatter and sharper saws alternating between the sides (outer pair ±75 %, middle pair
 * ±100 %, inner pair ±50 %). The mono sum (L + R) / 2 is the plain supersaw. The side signal goes through a
 * one-pole high-pass at the note's fundamental (at least 150 Hz), so low notes stay mono-compatible.
 * Phases are free-running and start at random on every note, so chords shimmer differently each time.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine.
 *
 * How it stays cheap: a weighted sum of naive saws is one straight ramp (slope = the weighted sum of the phase
 * increments) plus a downward step wherever one saw wraps. So per block each saw only costs its starting value,
 * its slope and its wraps (a few per block): every wrap writes its step and its two-sample PolyBLEP residual (the
 * same polynomial as opz_saw_blep in sdk/include/opz_dsp.h) into an event buffer, once for the mid bus and once
 * for the side bus. Per sample, the mid and side signals are just integrated from slope + events. The cost hardly
 * depends on the number of saws, so all seven play at full polyphony. Each block starts again from the exact
 * phases, so the integration's rounding (under 64 LSB) never accumulates.
 *
 * The detune and mix curves are Adam Szabo's published fits of the JP-8000 ("How to Emulate the Super Saw",
 * 2010), tabulated by gen_tables.py. Original code, MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"
#include "super_tables.h"

#define SAWS 7
#define N OPZ_BLOCK_FRAMES

typedef struct State {
    uint32_t ph[SAWS];      /* phases: 0 centre, 1/2 outer pair, 3/4 middle pair, 5/6 inner pair (left, right) */
    uint32_t ph_sub;
    fract32 lp_m, lp_s;     /* velocity tone one-poles on mid and side, Q31 */
    fract32 hp_s;           /* side high-pass state (its low-pass part) */
    int32_t tone;           /* tone coefficient, Q15 */
    fract32 amp;            /* velocity x output gain, Q31 */
    OpzMix mix;             /* the track's level and pan (opz_mix.h) */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");

/* Frequency offset of each saw at full detune (Szabo's measurements), in state order: centre, then each pair
 * with its left saw first. Q31. */
static const int32_t k_offset[SAWS] = {
    0,
    -236272873,     /* -0.11002313 left,  outer */
    230752315,      /* +0.10745242 right, outer */
    133499137,      /* +0.06216538 left,  middle */
    -135043199,     /* -0.06288439 right, middle */
    -41926526,      /* -0.01952356 left,  inner */
    42761145,       /* +0.01991221 right, inner */
};

/* Event buffers of one block (steps and BLEP residuals, as differences: see render), mid and side. Index N takes
 * events that fall after the block and is never read. Render runs one voice at a time, so all voices share them;
 * pass_out leaves them zeroed. .bss keeps them inside the slot window (the stack's size is not known). */
static int32_t s_em[N + 1], s_es[N + 1];

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x2545f491u;

#define ALWAYS_INLINE static inline __attribute__((always_inline))
#define OUT_GAIN 0x52800000     /* internal RMS 0.15 -> 0.097 per channel at full velocity (-20 dBFS RMS) */
#define INC_MAX 0x40000000u     /* 11 kHz: far above any note, keeps 2 inc below 2^31 */

/* Table lookup with linear interpolation; x in Q31, `bits` = log2 of the number of segments. */
ALWAYS_INLINE int32_t interp(const int32_t *t, int32_t x, int bits)
{
    int i = x >> (31 - bits);
    int32_t f = (int32_t)(((uint32_t)x << bits) & 0x7fffffffu);
    return t[i] + opz_mul_q31(f, t[i + 1] - t[i]);
}

/* One saw's wraps in this block, with mid weight wm and side weight ws: its steps and BLEP residuals into the
 * event buffers, as differences (the output integrates them). p is the phase at sample 0.
 *
 * As in opz_saw_blep: the last sample before a wrap (phase >= 2^32 - inc) is corrected by -(1 - x)^2 with
 * x = (2^32 - 1 - phase) / inc, the first one after it (phase < inc) by +(1 - x')^2 with x' = phase / inc. Here
 * x' = 1 - x (to 1 / inc), so the second correction is +x^2: with A = w x and B = w x^2 the corrections are
 * -(w - 2A + B) and +B, and the saw's drop of 2 at the wrap is a step of -2 w. */
static void saw_events(uint32_t p, uint32_t inc, uint32_t recip, int32_t wm, int32_t ws)
{
    if (p < inc) {                                           /* sample 0 follows a wrap in the last block */
        fract32 y = OPZ_Q31_ONE - (int32_t)opz_mul_u32_shr16(p, recip);
        int32_t bm = opz_mul_q31(opz_mul_q31(wm, y), y), bs = opz_mul_q31(opz_mul_q31(ws, y), y);
        s_em[0] += bm; s_em[1] -= bm;
        s_es[0] += bs; s_es[1] -= bs;
    }
    int j = 0;                                               /* p is the phase at sample j */
    for (;;) {
        uint32_t np = ~p;                                    /* the wrap comes after floor(np / inc) more samples */
        if (inc < (1u << 24) && np >= (inc << 8)) return;    /* > 256 samples away; keeps the estimate in range */
        uint32_t q = opz_mul_u32_shr16(np >> 7, recip) >> 24;   /* floor(np / inc), give or take one */
        uint32_t rem = np - q * inc;                         /* exact modulo 2^32 */
        if (rem >= inc) {
            if (rem >= 0x80000000u) { q--; rem += inc; }
            else { q++; rem -= inc; }
        }
        int jb = j + (int)q;                                 /* last sample before the wrap; its phase is ~rem */
        if (jb >= N) return;
        fract32 x = (fract32)opz_mul_u32_shr16(rem, recip);
        int32_t am = opz_mul_q31(wm, x), as = opz_mul_q31(ws, x);
        int32_t bm = opz_mul_q31(am, x), bs = opz_mul_q31(as, x);
        int32_t cm = wm - 2 * am + bm, cs = ws - 2 * as + bs;   /* w (1 - x)^2 */
        s_em[jb] -= cm; s_em[jb + 1] += cm;
        s_es[jb] -= cs; s_es[jb + 1] += cs;
        if (jb + 1 >= N) return;                             /* the wrap itself falls in the next block */
        s_em[jb + 1] += bm - 2 * wm; s_em[jb + 2] -= bm;
        s_es[jb + 1] += bs - 2 * ws; s_es[jb + 2] -= bs;
        p = inc - 1u - rem;                                  /* phase at sample jb + 1, after the wrap */
        j = jb + 1;
    }
}

/* Integrate mid and side from slope + events, side high-pass, velocity tone on both, L = M + S and R = M - S
 * with the envelope, into the interleaved block buffer st that opz_mix_stereo adds into out. Returns the envelope
 * level. */
ALWAYS_INLINE int32_t pass_out(fract32 *st, State *s, uint32_t m, uint32_t dm, uint32_t sd, uint32_t ds,
                               int32_t hp, uint32_t ps, uint32_t is, fract32 g_sub, int sub,
                               int32_t level, int32_t coef, int32_t step, int step0)
{
    fract32 lm = s->lp_m, ls = s->lp_s, hs = s->hp_s;
    int32_t tone = s->tone;
    for (int i = 0; i < N; i++) {
        m += dm + (uint32_t)s_em[i];                         /* wraps modulo 2^32; in range at every sample */
        sd += ds + (uint32_t)s_es[i];
        s_em[i] = 0;
        s_es[i] = 0;
        fract32 x = (fract32)sd;
        hs = opz_add_q31(hs, opz_mul_q31_q15(opz_sub_q31(x, hs), hp));
        x = opz_sub_q31(x, hs);
        fract32 mid = (fract32)m;
        if (sub) mid = opz_add_q31(mid, opz_mul_q31_q15(opz_tri(ps), g_sub));
        lm = opz_add_q31(lm, opz_mul_q31_q15(opz_sub_q31(mid, lm), tone));
        ls = opz_add_q31(ls, opz_mul_q31_q15(opz_sub_q31(x, ls), tone));
        int32_t lv = level >> 16;
        st[2 * i] = opz_mul_q31_q15(opz_add_q31(lm, ls), lv);
        st[2 * i + 1] = opz_mul_q31_q15(opz_sub_q31(lm, ls), lv);
        level = step0 ? opz_env_tick0(level, coef) : opz_env_tick(level, coef, step);
        ps += is;
    }
    s->lp_m = lm; s->lp_s = ls; s->hp_s = hs;
    s->ph_sub = ps;
    return level;
}

static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
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
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    ((State *)state)->tone = 0x7fff;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    for (int k = 0; k < SAWS; k++) s->ph[k] = rnd();        /* free-running oscillators: any phase at note-on */
    s->ph_sub = 0;
    fract32 vel = (fract32)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u);
    s->amp = opz_mul_q31(vel, OUT_GAIN);

    /* tone: fc = 3 kHz * 2^(2.585 velocity + (note - 60) / 24), g = 1 - exp(-2 pi fc / fs) = 1 - 2^(-fc * 2.0555e-4) */
    int32_t oct = opz_mul_q31(vel, 169410) + ((note - 60) * 65536 / 24);    /* log2(6) = 2.585 octaves, Q16 */
    if (oct > 169410) oct = 169410;                          /* 18 kHz */
    int32_t fc_q16 = 3000 * opz_exp2_q16(oct);
    int32_t x = (int32_t)(((int64_t)fc_q16 * 882827) >> 32);                 /* fc / fs * 2 pi / ln 2, Q16 */
    s->tone = (65536 - opz_exp2_q16(-x)) >> 1;               /* Q15 */
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
    /* All saws run at the block's middle frequency: a glide moves in 64-frame steps (1.5 ms), phase-continuous. */
    uint32_t inc = pitch + (uint32_t)(pitchSlope * (N / 2));
    if (inc > INC_MAX) inc = INC_MAX;
    int32_t p1 = opz_pq31(params[OPZ_P_P1]);
    int32_t detune = interp(k_detune, p1, 6);
    /* Near unison the saws' phases hardly move, so a stereo spread would just lean to one side for seconds:
     * the width grows from mono at P1 0 to full at P1 32. */
    fract32 width = p1 >= 0x10000000 ? OPZ_Q31_ONE : p1 << 3;
    int32_t mix = opz_pq31(params[OPZ_P_P2]);
    fract32 gc = opz_mul_q31(interp(k_centre, mix, 5), s->amp);
    fract32 gs = opz_mul_q31(interp(k_side, mix, 5), s->amp);
    fract32 g_sub = opz_mul_q31(interp(k_sub, mix, 5), s->amp);
    /* pan of the pairs: outer 0.75, middle 1.0, inner 0.5; the left saw of each pair first */
    const int32_t wm[SAWS] = { gc, gs, gs, gs, gs, gs, gs };
    fract32 gw = opz_mul_q31(gs, width);
    const int32_t ws[SAWS] = { 0, gw - (gw >> 2), (gw >> 2) - gw, gw, -gw, gw >> 1, -(gw >> 1) };

    uint32_t m = 0, dm = 0, sd = 0, ds = 0;
    for (int k = 0; k < SAWS; k++) {
        uint32_t ik = inc + (uint32_t)opz_mul_q31((int32_t)inc, opz_mul_q31(detune, k_offset[k]));
        if (ik > INC_MAX) ik = INC_MAX;
        uint32_t p = s->ph[k];
        int32_t y = (int32_t)(p ^ 0x80000000u);              /* the naive saw at sample 0 */
        m += (uint32_t)opz_mul_q31(wm[k], y);
        sd += (uint32_t)opz_mul_q31(ws[k], y);
        dm += (uint32_t)opz_mul_q31(wm[k], (int32_t)ik);
        ds += (uint32_t)opz_mul_q31(ws[k], (int32_t)ik);
        saw_events(p, ik, opz_blep_recip(ik), wm[k], ws[k]);
        s->ph[k] = p + ik * N;
    }

    /* side high-pass at max(f0, 150 Hz), capped at 4 kHz: g = 1 - exp(-w) ~ w - w^2 / 2 + w^3 / 6, w = pi inc / 2^31 */
    uint32_t hinc = inc < 14608732u ? 14608732u : inc > 389566195u ? 389566195u : inc;
    fract32 w = opz_shl_q31(opz_mul_q31((int32_t)hinc, 0x6487ed51), 2);      /* pi / 4 in Q31, x 4 */
    fract32 w2 = opz_mul_q31(w, w);
    int32_t hp = (w - (w2 >> 1) + opz_mul_q31(opz_mul_q31(w2, w), 0x15555555)) >> 16;   /* Q15 */

    /* The integration adds slope + event before each sample, sample 0 included: start one slope early. */
    m -= dm;
    sd -= ds;
    int32_t level = OPZ_ADSR_LEVEL(env), env_coef = OPZ_ADSR_COEF(env), env_step = OPZ_ADSR_STEP(env);
    uint32_t ps = s->ph_sub, is = inc >> 1;
    int32_t gb = g_sub >> 16;
    fract32 st[2 * N];                                       /* the voice, interleaved L R, before level and pan */
    if (gb) {
        level = env_step ? pass_out(st, s, m, dm, sd, ds, hp, ps, is, gb, 1, level, env_coef, env_step, 0)
                         : pass_out(st, s, m, dm, sd, ds, hp, ps, is, gb, 1, level, env_coef, 0, 1);
    } else {
        level = env_step ? pass_out(st, s, m, dm, sd, ds, hp, ps, is, 0, 0, level, env_coef, env_step, 0)
                         : pass_out(st, s, m, dm, sd, ds, hp, ps, is, 0, 0, level, env_coef, 0, 1);
    }
    opz_mix_stereo(out, st, &s->mix, params);               /* level and pan, added into interleaved out */
    OPZ_ADSR_LEVEL(env) = level;
}

OPZ_EXPORTS_INSTRUMENT();
