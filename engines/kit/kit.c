/*
 * kit — synthesized drum kit for the kick, snare, perc and sample tracks. Each of the 24 keys plays its own model.
 *
 *   keys 1-4   kicks: deep, punchy, boom, hard        keys 13-15 toms: low, mid, high
 *   keys 5-6   snares: 808, tight                     key  16    conga
 *   key  7     clap                                   key  17    cowbell
 *   key  8     rim                                    key  18    clave
 *   keys 9-10  hats: closed, pedal (choke 11-12)      key  19    shaker
 *   keys 11-12 open hat, cymbal                       keys 20-24 zap, blip, noise, hi-q, metal
 *
 *   P1        tune        +-1 octave on every pitch and filter of the model (128 = nominal)
 *   P2        decay       x0.25 .. x2 on every decay (128 = nominal)
 *   velocity              level
 *   attack .. release     the OP-Z drum envelope over the hit, as on TE's sample kits: start, attack, hold, decay
 *                         (opz_drumenv in opz_adsr.h). The presets' 0 / 0 / 255 / 0 leaves every hit as designed.
 *   note style gate       a key release fades the hit (~20 ms); the other styles play one-shots
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine; kit never reads them.
 *
 * The key index is (note - 53) mod 24, i.e. F3 is the first key (to confirm on hardware). Knobs are read when a
 * hit starts: drums are short, and it keeps the per-sample work to oscillators, filters and one-pole decays.
 * Each voice ends itself (envelope stage 0) when its sound has died away, like TE's analoga does.
 *
 * Models follow the classic analog drum circuits (TR-808/909 style), as also explored by Mutable Instruments
 * Plaits' and Peaks' drum models; written from scratch on sdk/include/opz_dsp.h. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

enum { W_SINE = 0, W_TRI = 1, W_SQUARE = 2 };
enum { N_LP = 0, N_BP = 1, N_HP = 2 };
enum { F_CLAP = 1, F_CHOKER = 2, F_CHOKED = 4 };

typedef struct Model {
    uint16_t fa, fb;          /* tone frequencies, Hz (0 = off) */
    uint8_t pitch16;          /* pitch-envelope depth, 1/16 octave */
    uint8_t wave;             /* tone waveform */
    uint16_t tp, ta, tn;      /* decay time constants, ms: pitch, amplitude, noise */
    uint8_t ga, gb, gn, gm, gc;   /* gains: tone a, tone b, noise, metal, click */
    uint8_t nmode, nres;      /* noise/metal filter mode and resonance */
    uint16_t nfc;             /* noise/metal filter, Hz */
    uint8_t flags;
    uint8_t trim;             /* output trim, x/64 (64 = unity) to balance the kit */
} Model;

static const Model k_models[24] = {
    /*  fa    fb  p16 wave        tp    ta    tn   ga   gb   gn   gm   gc  nmode  nres  nfc   flags  trim */
    {   50,    0,  40, W_SINE,    40,  450,    0, 255,   0,   0,   0,  60, N_LP,    0,     0, 0, 64 },           /* kick deep */
    {   58,    0,  48, W_SINE,    25,  220,    6, 255,   0,  40,   0, 130, N_HP,   20,  3000, 0, 64 },           /* punchy */
    {   44,    0,  24, W_SINE,    60,  900,    0, 255,   0,   0,   0,  30, N_LP,    0,     0, 0, 64 },           /* boom */
    {   65,    0,  56, W_TRI,     18,  160,    4, 255,   0,  60,   0, 200, N_HP,   20,  4000, 0, 64 },           /* hard */
    {  185,  330,   8, W_SINE,    15,  120,  180, 160, 110, 200,   0,   0, N_HP,   30,  1800, 0, 77 },           /* snare 808 */
    {  220,  400,   5, W_SINE,    10,   70,  110, 140,  90, 230,   0,  40, N_BP,   20,  4000, 0, 90 },           /* tight */
    {    0,    0,   0, W_SINE,     1,    1,  250,   0,   0, 255,   0,   0, N_BP,   60,  1200, F_CLAP, 160 },      /* clap */
    {  455, 1665,   0, W_TRI,      1,   14,   10, 200, 150,  60,   0,   0, N_HP,   20,  5000, 0, 128 },           /* rim */
    {    0,    0,   0, W_SINE,     1,   45,   40,   0,   0,  60, 255,   0, N_HP,   40,  7000, F_CHOKER, 255 },    /* closed hat */
    {    0,    0,   0, W_SINE,     1,   90,   70,   0,   0,  50, 255,   0, N_HP,   40,  6500, F_CHOKER, 230 },    /* pedal hat */
    {    0,    0,   0, W_SINE,     1,  450,  300,   0,   0,  70, 255,   0, N_HP,   40,  6500, F_CHOKED, 150 },    /* open hat */
    {    0,    0,   0, W_SINE,     1, 1500,  900,   0,   0,  90, 220,   0, N_HP,   20,  5000, F_CHOKED, 72 },    /* cymbal */
    {   90,    0,  10, W_SINE,    60,  380,   30, 255,   0,  30,   0,  30, N_LP,    0,  2000, 0, 64 },           /* low tom */
    {  130,    0,  10, W_SINE,    55,  330,   30, 255,   0,  30,   0,  30, N_LP,    0,  2500, 0, 64 },           /* mid tom */
    {  180,    0,  10, W_SINE,    50,  280,   30, 255,   0,  30,   0,  30, N_LP,    0,  3000, 0, 64 },           /* high tom */
    {  330,    0,   6, W_SINE,    15,  160,    8, 255,   0,  25,   0,  60, N_BP,   20,  2000, 0, 72 },           /* conga */
    {  540,  800,   0, W_SQUARE,   1,  300,    0, 150, 150,   0,   0,   0, N_LP,    0,     0, 0, 72 },           /* cowbell */
    { 2500,    0,   0, W_SINE,     1,   25,    0, 255,   0,   0,   0,   0, N_LP,    0,     0, 0, 100 },           /* clave */
    {    0,    0,   0, W_SINE,     1,    1,   60,   0,   0, 255,   0,   0, N_HP,   10,  6000, 0, 128 },           /* shaker */
    {  120,    0,  80, W_SINE,    30,  150,    0, 255,   0,   0,   0,   0, N_LP,    0,     0, 0, 64 },           /* zap */
    {  880,    0,   0, W_SQUARE,   1,   60,    0, 160,   0,   0,   0,   0, N_LP,    0,     0, 0, 90 },           /* blip */
    {    0,    0,   0, W_SINE,     1,    1,  300,   0,   0, 255,   0,   0, N_LP,    0,  9000, 0, 100 },           /* noise */
    {  600,    0,  32, W_SINE,     8,  120,   90, 200,   0, 160,   0,   0, N_BP,  230,   600, 0, 72 },           /* hi-q */
    {  410,  580,   0, W_SINE,     1,  600,    0, 180, 180,   0,   0,   0, N_LP,    0,     0, 0, 56 },           /* metal */
};

/* TR-808 hi-hat square oscillators, Hz */
static const uint16_t k_metal_hz[6] = { 205, 304, 370, 523, 540, 800 };

#define THRESHOLD 0x00200000   /* -60 dB: the hit is over below this (6.9 time constants) */
#define CLAP_GAP  441          /* 10 ms between clap bursts */

typedef struct State {
    uint32_t ph_a, ph_b, ph_m[6];
    uint32_t inc_a, inc_b, inc_m[6];
    uint32_t seed;
    int32_t env_a, coef_a, env_n, coef_n, env_p, coef_p, env_c;
    int32_t pitch_q16;                        /* pitch envelope depth, octaves Q16 */
    int32_t ga, gb, gn, gm, gc;               /* Q31 gains, velocity included */
    OpzSvf nf;
    OpzSvfCoef ncoef;
    int32_t clap_left, clap_timer, dscale;
    OpzMix mix;                               /* the track's level and pan (opz_mix.h) */
    OpzDrumEnv denv;                          /* the envelope page over the hit */
    uint8_t model, track, choke, wave, nmode, active, fading;
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;
static uint8_t s_choke[16];                   /* per track: bumped by closed hats, watched by open hats */

/* Noise filter coefficients per model for the last tune they were computed at (two 64-bit divisions per note
 * otherwise). tune is 2^(-1..1) in Q16, never 0, so the zeroed .bss reads as empty. */
static struct { int32_t tune; OpzSvfCoef c; } s_svf_cache[24];

static int32_t decay_for(uint32_t ms, int32_t decay_scale_q16)
{
    /* ms * 441 * scale / 655360 samples, as floor(floor(x / 2^16) / 10) in 32 bits (ms <= 1500, scale <= 2^17) */
    uint32_t t = opz_mul_u32_shr16(ms * 441u, (uint32_t)decay_scale_q16);    /* < 2^21 */
    uint32_t n = opz_mul_u32_shr16(t, 838861) >> 7;                             /* t / 10 exactly (t < 2^22) */
    return opz_decay_coef((int32_t)n);
}

static inline uint32_t scaled_inc(uint32_t hz, int32_t tune_q16)
{
    return opz_hz_to_inc((int32_t)(hz * (uint32_t)tune_q16));      /* below 2^31: hz <= 2500, tune <= 2^17 */
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
    (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    State *s = state;
    s->track = (uint8_t)(track & 15);
    s->seed = 0x1234567u + track * 7919u;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("Os")))   /* once per hit: size matters more than speed */
#endif
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    int idx = ((note - 53) % 24 + 24) % 24;
    const Model *m = &k_models[idx];
    s->model = (uint8_t)idx;

    int32_t tune = opz_exp2_q16((opz_p16(params[OPZ_P_P1]) - 32768) * 2);       /* 2^(-1..1), Q16 */
    int32_t p2 = opz_p16(params[OPZ_P_P2]) - 32768;
    int32_t dscale = opz_exp2_q16(p2 < 0 ? p2 * 4 : p2 * 2);                    /* 2^(-2..1), Q16 */
    int32_t amp = (int32_t)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u);  /* velocity, Q31 */

    s->wave = m->wave;
    s->inc_a = scaled_inc(m->fa, tune);
    s->inc_b = scaled_inc(m->fb, tune);
    for (int i = 0; i < 6; i++) s->inc_m[i] = scaled_inc(k_metal_hz[i], tune);
    s->ph_a = s->ph_b = 0;
    s->pitch_q16 = m->pitch16 << 12;
    s->env_p = OPZ_Q31_ONE;
    s->coef_p = decay_for(m->tp, 65536);                                        /* pitch sweep is not scaled */
    s->env_a = OPZ_Q31_ONE;
    s->coef_a = decay_for(m->ta, dscale);
    s->env_n = (m->flags & F_CLAP) ? OPZ_Q31_ONE : (m->gn ? OPZ_Q31_ONE : 0);
    s->coef_n = decay_for((m->flags & F_CLAP) ? 8 : m->tn, (m->flags & F_CLAP) ? 65536 : dscale);
    s->env_c = m->gc ? OPZ_Q31_ONE : 0;
    s->clap_left = (m->flags & F_CLAP) ? 3 : 0;
    s->clap_timer = CLAP_GAP;
    s->dscale = dscale;

    /* The hit's length for the envelope page: it ends when its amplitude and noise decays are below -60 dB, 6.9
     * time constants (7 here: a little margin), after the clap's three bursts. */
    uint32_t tmax = m->ta > m->tn ? m->ta : m->tn;                                /* ms, at most 1500 */
    uint32_t ms16 = (tmax * (uint32_t)dscale) >> 12;                            /* 1/16 ms; dscale <= 2^17 */
    opz_drumenv_start(&s->denv, ((ms16 * 312) >> 4) + (uint32_t)s->clap_left * CLAP_GAP, params);   /* 44.1 x 7.07 */

    s->ga = opz_mul_q31(amp, m->ga << 23);
    s->gb = opz_mul_q31(amp, m->gb << 23);
    s->gn = opz_mul_q31(amp, m->gn << 23);
    s->gm = opz_mul_q31(amp, m->gm << 23);
    s->gc = opz_mul_q31(amp, m->gc << 23);

    s->nmode = m->nmode;
    s->nf.ic1 = s->nf.ic2 = 0;
    s->ncoef.a1 = s->ncoef.a2 = s->ncoef.a3 = s->ncoef.k = 0;      /* no filter unless the model has one */
    if (m->nfc) {
        if (s_svf_cache[idx].tune != tune) {
            int32_t fc = (int32_t)(((int64_t)m->nfc * tune));
            int32_t k = OPZ_Q31_ONE - opz_mul_q31(m->nres << 23, 0x7c28f5c3);
            s_svf_cache[idx].c = opz_svf_coef(fc, k);
            s_svf_cache[idx].tune = tune;
        }
        s->ncoef = s_svf_cache[idx].c;
    }

    if (m->flags & F_CHOKER) s_choke[s->track]++;
    s->choke = s_choke[s->track];
    s->active = 1;
    s->fading = 0;
    opz_mix_start(&s->mix, params);
}

void onNoteOff(int note, void *state)
{
    (void)note;
    opz_drumenv_off(&((State *)state)->denv);                 /* gate style only; one-shots decay on their own */
}

/* ---- render passes ------------------------------------------------------------------------------------------
 * render runs each part of a model over the whole block in its own short loop (tone, metal and noise, then filter
 * and output) through three block buffers. The model's flags are compile-time constants inside each copy of a
 * pass, so the loops carry no per-sample branches; any other combination takes the generic copy. The arithmetic
 * and the order of the noise generator's draws are the same as one per-sample loop over the whole model. */

#define ALWAYS_INLINE static inline __attribute__((always_inline))
#define NOINLINE static __attribute__((noinline))

/* Block buffers, shared by all voices: render runs one voice at a time and nothing in them outlives the call. In
 * .bss, inside the slot window (the stack's size is not known). */
static fract32 s_tone[OPZ_BLOCK_FRAMES], s_noise[OPZ_BLOCK_FRAMES], s_click[OPZ_BLOCK_FRAMES];

_Static_assert(OPZ_VOICE_PEAK == 0x20000000, "out_pass applies OPZ_VOICE_PEAK as a shift by 2");

static void zero_block(fract32 *b)
{
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) b[i] = 0;
}

/* A one-pole envelope e * c per sample, over a block, when no pass needs its values. */
static int32_t env_block(int32_t e, int32_t c)
{
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) e = opz_mul_q31(e, c);
    return e;
}

/* The tone runs in four passes, each with few enough values to stay in registers: the pitch sweep's phase
 * increments (into s_noise and s_click, which the metal and noise passes fill later), oscillator a at ga, oscillator
 * b at gb added on, then the amplitude envelope. The same products and saturating adds as
 * x = (a ga + b gb) env_a per sample. */

/* Phase increments of tones a (-> s_noise) and b (-> s_click, if two) under the pitch envelope. */
ALWAYS_INLINE void sweep_pass(State *s, int two)
{
    uint32_t inc_a = s->inc_a, inc_b = s->inc_b;
    int32_t ep = s->env_p, cp = s->coef_p, depth = s->pitch_q16;
    uint32_t *ia = (uint32_t *)s_noise, *ib = (uint32_t *)s_click;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        uint32_t up = (uint32_t)opz_exp2_q16(opz_mul_q31(depth, ep));          /* exact (pitch_q16 * env_p) >> 31 */
        ia[i] = opz_mul_u32_shr16(inc_a, up);
        if (two) ib[i] = opz_mul_u32_shr16(inc_b, up);
        ep = opz_mul_q31(ep, cp);
    }
    s->env_p = ep;
}

/* One tone oscillator at gain g into s_tone (add: onto it), stepping by incs[i] (sweep) or inc. */
ALWAYS_INLINE void osc_pass(uint32_t *phase, const uint32_t *incs, uint32_t inc, int32_t g, int wave, int sweep,
                            int add)
{
    uint32_t ph = *phase;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        fract32 w;
        if (wave == W_SQUARE) w = (int32_t)ph < 0 ? -0x40000000 : 0x40000000;
        else if (wave == W_TRI) w = opz_tri(ph);
        else w = opz_sine(ph);
        ph += sweep ? incs[i] : inc;
        fract32 v = opz_mul_q31(w, g);
        s_tone[i] = add ? opz_add_q31(s_tone[i], v) : v;
    }
    *phase = ph;
}

NOINLINE void sweep_one(State *s) { sweep_pass(s, 0); }
NOINLINE void sweep_two(State *s) { sweep_pass(s, 1); }
/* One copy per waveform, each with the four sweep/add loops. */
#define OSC_COPY(NAME, WAVE)                                                                                   \
    NOINLINE void NAME(uint32_t *ph, const uint32_t *incs, uint32_t inc, int32_t g, int sweep, int add)       \
    {                                                                                                          \
        if (sweep) {                                                                                           \
            if (add) osc_pass(ph, incs, inc, g, WAVE, 1, 1);                                                   \
            else osc_pass(ph, incs, inc, g, WAVE, 1, 0);                                                       \
        } else {                                                                                               \
            if (add) osc_pass(ph, incs, inc, g, WAVE, 0, 1);                                                   \
            else osc_pass(ph, incs, inc, g, WAVE, 0, 0);                                                       \
        }                                                                                                      \
    }
OSC_COPY(osc_sine, W_SINE)
/* The triangle and square copies leave out the loops no model uses, to keep kit inside its 10 KB slot: the only
 * swept triangle (hard kick) has one tone, and no square model (cowbell, blip) sweeps. A model that needs another
 * combination would play without its sweep or its second tone: give the copy that loop back first. */
NOINLINE void osc_tri(uint32_t *ph, const uint32_t *incs, uint32_t inc, int32_t g, int sweep, int add)
{
    if (sweep) osc_pass(ph, incs, inc, g, W_TRI, 1, 0);
    else if (add) osc_pass(ph, incs, inc, g, W_TRI, 0, 1);
    else osc_pass(ph, incs, inc, g, W_TRI, 0, 0);
}
NOINLINE void osc_square(uint32_t *ph, const uint32_t *incs, uint32_t inc, int32_t g, int sweep, int add)
{
    (void)incs; (void)sweep;
    if (add) osc_pass(ph, 0, inc, g, W_SQUARE, 0, 1);
    else osc_pass(ph, 0, inc, g, W_SQUARE, 0, 0);
}

/* The amplitude envelope over s_tone. Returns it at the end of the block. */
NOINLINE int32_t tone_env(State *s)
{
    int32_t ea = s->env_a, ca = s->coef_a;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        s_tone[i] = opz_mul_q31(s_tone[i], ea);
        ea = opz_mul_q31(ea, ca);
    }
    return ea;
}

/* Tone oscillators a and b with the pitch sweep, at gains ga and gb, under the amplitude envelope -> s_tone.
 * Returns the amplitude envelope at the end of the block. b is silent without gb, so it is skipped then. */
static int32_t tone_pass(State *s)
{
    int two = s->gb != 0, sweep = s->pitch_q16 != 0;
    if (sweep) {
        if (two) sweep_two(s);
        else sweep_one(s);
    }
    const uint32_t *ia = (const uint32_t *)s_noise, *ib = (const uint32_t *)s_click;
    void (*osc)(uint32_t *, const uint32_t *, uint32_t, int32_t, int, int) =
        s->wave == W_SQUARE ? osc_square : s->wave == W_TRI ? osc_tri : osc_sine;
    osc(&s->ph_a, ia, s->inc_a, s->ga, sweep, 0);
    if (two) osc(&s->ph_b, ib, s->inc_b, s->gb, sweep, 1);
    return tone_env(s);
}

/* The sum of the six squares (+-1/6 each, 0x0aaaaaaa) by how many are below zero. */
#define SQ6 0x0aaaaaaa
static const int32_t k_metal_sum[7] = { 6 * SQ6, 4 * SQ6, 2 * SQ6, 0, -2 * SQ6, -4 * SQ6, -6 * SQ6 };

/* Three of the squares: how many are below zero, per sample, added to neg[] (or stored, first). In registers. */
ALWAYS_INLINE void count_neg(uint32_t *ph, const uint32_t *inc, int32_t *neg, int first)
{
    uint32_t p0 = ph[0], p1 = ph[1], p2 = ph[2], i0 = inc[0], i1 = inc[1], i2 = inc[2];
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t n = (int32_t)((p0 >> 31) + (p1 >> 31) + (p2 >> 31));
        neg[i] = first ? n : neg[i] + n;
        p0 += i0;
        p1 += i1;
        p2 += i2;
    }
    ph[0] = p0;
    ph[1] = p1;
    ph[2] = p2;
}

/* Six squares at 1/6 each under gm and the amplitude envelope -> s_noise. Returns the envelope at the end.
 * s_click holds the counts until noise_pass writes the click. */
NOINLINE int32_t metal_pass(State *s, int32_t ea)
{
    int32_t *neg = s_click, ca = s->coef_a, gm = s->gm;
    count_neg(s->ph_m, s->inc_m, neg, 1);
    count_neg(s->ph_m + 3, s->inc_m + 3, neg, 0);
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        s_noise[i] = opz_mul_q31(opz_mul_q31(k_metal_sum[neg[i]], gm), ea);
        ea = opz_mul_q31(ea, ca);
    }
    return ea;
}

/* Noise at gn under its envelope, added to the metal in s_noise (or alone), the click -> s_click, and the clap's
 * bursts. The noise draws come in the original order: the noise first, then the click, within each sample. */
ALWAYS_INLINE void noise_pass(State *s, const Model *m, int metal, int noise, int click, int clap)
{
    uint32_t seed = s->seed;
    int32_t en = s->env_n, cn = s->coef_n, ec = s->env_c, gn = s->gn, gc = s->gc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        if (noise) {
            fract32 n = opz_mul_q31(opz_mul_q31(opz_noise(&seed) >> 1, gn), en);
            s_noise[i] = metal ? opz_add_q31(s_noise[i], n) : n;
        } else if (!metal) {
            s_noise[i] = 0;
        }
        if (click) {
            fract32 c = 0;
            if (ec) {
                c = opz_mul_q31(opz_mul_q31(opz_noise(&seed), gc), ec);
                ec = opz_mul_q31(ec, 0x7a000000);                              /* ~1 ms click */
                if (ec < THRESHOLD) ec = 0;
            }
            s_click[i] = c;
        }
        en = opz_mul_q31(en, cn);
        if (clap && s->clap_left && --s->clap_timer == 0) {                       /* next clap burst */
            en = OPZ_Q31_ONE;
            if (--s->clap_left == 0) cn = decay_for(m->tn, s->dscale);           /* the last one rings out */
            else s->clap_timer = CLAP_GAP;
        }
    }
    if (!click) zero_block(s_click);
    s->seed = seed;
    s->env_n = en;
    s->coef_n = cn;
    s->env_c = ec;
}

/* Filter the noise and metal (filt: 0 none, 1 + N_LP / N_BP / N_HP), add tone and click, trim, soft clip, peak
 * level, into the mono buffer opz_mix_mono adds into out. The trim carries the envelope page's ramp: Q15 in the top
 * half of ta (Q15.16), dt per frame. */
ALWAYS_INLINE void out_pass(fract32 *out, State *s, int32_t ta, int32_t dt, int filt)
{
    OpzSvf nf = s->nf;
    OpzSvfCoef c = s->ncoef;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        fract32 v = s_noise[i];
        if (filt) {
            fract32 lp, bp, hp;
            opz_svf(&nf, &c, v, &lp, &bp, &hp);
            v = filt == 1 + N_HP ? hp : filt == 1 + N_BP ? opz_shl_q31(bp, 1) : lp;
        }
        fract32 x = opz_add_q31(opz_add_q31(s_tone[i], v), s_click[i]);
        x = opz_clip(opz_shl_q31(opz_mul_q31_q15(x, ta >> 16), 2));               /* soft clip loud hits */
        ta += dt;
        x >>= 2;                                                                 /* x OPZ_VOICE_PEAK (2^-2) */
        out[i] = x;
    }
    s->nf = nf;
}

/* The copies of the passes that render picks from (the specialised ones cover the models of k_models). */
NOINLINE void noise_metal(State *s, const Model *m) { noise_pass(s, m, 1, 1, 0, 0); }
NOINLINE void noise_only(State *s, const Model *m) { noise_pass(s, m, 0, 1, 0, 0); }
NOINLINE void noise_click(State *s, const Model *m) { noise_pass(s, m, 0, 1, 1, 0); }
NOINLINE void noise_any(State *s, const Model *m, int metal, int noise, int click, int clap)
{
    noise_pass(s, m, metal, noise, click, clap);
}
NOINLINE void out_hp(fract32 *out, State *s, int32_t ta, int32_t dt) { out_pass(out, s, ta, dt, 1 + N_HP); }
NOINLINE void out_bp(fract32 *out, State *s, int32_t ta, int32_t dt) { out_pass(out, s, ta, dt, 1 + N_BP); }
NOINLINE void out_lp(fract32 *out, State *s, int32_t ta, int32_t dt) { out_pass(out, s, ta, dt, 1 + N_LP); }
NOINLINE void out_plain(fract32 *out, State *s, int32_t ta, int32_t dt) { out_pass(out, s, ta, dt, 0); }

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                             /* pitch comes from the key's model */
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    if (!s->active) { OPZ_ADSR_STAGE(env) = 0; return; }

    const Model *m = &k_models[s->model];
    /* Q15 trim / 256 in Q15.16, times the envelope page's ramp (exact while it holds) -> denv.g0 .. g1 */
    if (!opz_drumenv_block(&s->denv, s_cb, env, m->trim << 23)) return;     /* the envelope ended the hit */

    if (OPZ_DRUMENV_STOLEN(&s->denv, env) && !s->fading) {
        /* The host wants this voice for a new note (voice steal, stage 5/6) and starts it when we reach stage 0:
         * fade out in a few milliseconds instead of ringing on (hardware: long hits blocked new ones). */
        s->fading = 1;
        s->coef_a = s->coef_n = opz_decay_coef(32);
        s->clap_left = 0;
        s->env_c = 0;
    }
    if ((m->flags & F_CHOKED) && s->choke != s_choke[s->track]) {
        s->coef_a = s->coef_n = opz_decay_coef(441);            /* choked by a closed hat: 10 ms fade */
        s->choke = s_choke[s->track];
    }
    int has_tone = s->ga | s->gb, has_metal = s->gm != 0, has_noise = s->gn != 0;
    int click = s->env_c != 0, clap = s->clap_left != 0;
    int32_t env_a = s->env_a;

    /* Tone and metal each run the amplitude envelope over the block from the same start value; the last pass
     * that runs stores where it ends. */
    if (has_tone) {
        s->env_a = tone_pass(s);
    } else {
        zero_block(s_tone);
    }
    if (has_metal) {
        int32_t e = metal_pass(s, env_a);
        if (!has_tone) s->env_a = e;
    }
    if (has_noise || click || clap || has_metal) {
        if (has_metal && has_noise && !click && !clap) noise_metal(s, m);
        else if (!has_metal && has_noise && !click && !clap) noise_only(s, m);
        else if (!has_metal && has_noise && click && !clap) noise_click(s, m);
        else noise_any(s, m, has_metal, has_noise, click, clap);
    } else {
        zero_block(s_noise);
        zero_block(s_click);
        s->env_n = env_block(s->env_n, s->coef_n);
    }
    if (!has_tone && !has_metal) s->env_a = env_block(env_a, s->coef_a);

    int filt = (has_metal || has_noise) && s->ncoef.a1 ? s->nmode + 1 : 0;
    int32_t ta = s->denv.g0, dt = (s->denv.g1 - ta) >> 6;
    fract32 mono[OPZ_BLOCK_FRAMES];
    if (filt == 1 + N_HP) out_hp(mono, s, ta, dt);
    else if (filt == 1 + N_BP) out_bp(mono, s, ta, dt);
    else if (filt == 1 + N_LP) out_lp(mono, s, ta, dt);
    else out_plain(mono, s, ta, dt);
    opz_mix_mono(out, mono, &s->mix, params);                      /* level and pan, into interleaved out */

    if (s->env_a < THRESHOLD && s->env_n < THRESHOLD && s->env_c == 0 && s->clap_left == 0) {
        s->active = 0;
        OPZ_ADSR_STAGE(env) = 0;                                                  /* free the voice */
    }
}

OPZ_EXPORTS_INSTRUMENT();
