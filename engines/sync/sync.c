/*
 * sync — hard-sync lead: a band-limited saw hard-synced to a master oscillator, with a square sub one octave
 * down and noise. Screaming sync leads, sync basses, nasal sweeps.
 *
 *   P1        ratio       the slave's frequency over the master's: 1 (a plain saw) at 0, then up 8 octaves on
 *                         ratio = 2^(4x + 4x^2), x = P1 / 255: fine steps at the bottom, faster at the top.
 *                         1.5 at P1 33, 2 at 53, 4 at 93, 8 at 128, 16 at 158, 64 at 210, 256 at 255. The slave
 *                         stops at 10 kHz, so on high notes the top of the knob holds still (C4: from P1 191)
 *   P2        sub, noise  0 .. 128: a square one octave below the master comes in on an equal-power curve, level
 *                         with the saw at 128; 128 .. 255: white noise comes in the same way against both, level
 *                         with the oscillators at 255. The RMS stays the same over the whole range
 *   velocity              level, and the ratio: 1/48 octave per step around 100 (127: +0.56 octave, brighter;
 *                         52: -1 octave). The ratio never goes below 1
 *   attack..release       host envelope
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine.
 *
 * How it works. Between discontinuities the synced saw is a straight line, so per block the engine only works out
 * where the discontinuities fall: the slave's own wraps (a step of -2) and the sync resets at every master wrap
 * (a step from wherever the slave was down to -1), found from the two phases with one reciprocal per oscillator
 * (no division). The sub square toggles at the same master wraps, so its steps ride on the reset events for free.
 * Every step is band-limited with a 4-point BLEP (the integral of a cubic B-spline: two samples before the step,
 * two after) written into an event buffer as differences; per sample the output is just integrated (one add) and
 * scaled. The two samples before a step need one sample of look-ahead, so the output runs one sample (23 us)
 * late; the block's last sample and the residuals that fall after the block are carried in the state. Each block
 * restarts the integration from the exact phases, so its rounding never builds up.
 *
 * A synced saw has a DC offset that moves with the ratio (the cut-off ramp of each period sits below zero:
 * -f (1 - f) / r for a ratio r with fractional part f, down to -0.17 near r = 1.4). It is removed per block, so a
 * ratio sweep does not push the track filter around. The master runs free (phases continue across notes, like an
 * analog oscillator), so notes on a busy voice don't click. Near the cap the band-limited slave keeps few
 * harmonics and loses level (-4.6 dB RMS at 10 kHz); the saw's gain makes it up per block (1 + 0.7 f / 10 kHz), so a
 * sweep stays level within 1 dB.
 *
 * Original code, MIT licence, see CONTRIBUTING.md. The B-spline BLEP is the integrated-polynomial step of
 * Valimaki, Pekonen and Nam, "Perceptually informed synthesis of bandlimited classical waveforms using integrated
 * polynomial interpolation" (JASA 2012); the polynomials here are our own derivation.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"

#define N OPZ_BLOCK_FRAMES
#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define INC_MIN 0x10000u        /* 0.67 Hz */
#define INC_MAX 0x40000000u     /* 11 kHz: the master */
#ifndef SLAVE_MAX
#define SLAVE_MAX 0x3a0c6c3cu   /* 10 kHz: the slave (sync aliasing grows fast above it, tests/alias.c) */
#endif
#define OCT_MAX (8 << 16)       /* ratio 256 */
#define VEL_OCT 1365            /* 1/48 octave per velocity step, Q16 */
#define COMP_K 101170           /* 0.7 x 2^47 / SLAVE_MAX: the level compensation, 0.7 at the cap */
#define NOISE_NORM 0x5a82799a   /* sqrt(2) / 2: osc RMS 1/sqrt(3) over the noise's 1/sqrt(6), halved (Q31) */

typedef struct State {
    uint32_t pm, ps;        /* master and slave phase at the block's first sample */
    int32_t carry;          /* the oscillator's last sample of the previous block (the output runs 1 late) */
    int32_t c0, c1;         /* BLEP residuals the previous block left on samples 0 and 1 */
    uint32_t seed;          /* noise */
    int32_t nprev;          /* the last noise sample (the two-tap sum) */
    fract32 amp;            /* velocity x OPZ_VOICE_PEAK, Q31 */
    int32_t voct;           /* velocity ratio offset, octaves Q16 */
    int32_t sub;            /* the sub square's sign at the block's first sample, +1 / -1 */
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in the 280 bytes the OS leaves");

/* Differences of the oscillator in this block: steps and BLEP residuals. Index = sample + 1 (index 0 is the
 * previous block's last sample, still to be output); indices N + 1 .. N + 3 take what falls after the block and are
 * never read. Render runs one voice at a time, so all voices share it; pass_out leaves it zeroed. */
static int32_t s_e[N + 4];
static int32_t s_k0, s_k1;  /* residuals on samples N and N + 1, for the state's c0 and c1 */

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x7b3a19e5u;

static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
}

/* ---- events ------------------------------------------------------------------------------------------------ */

/* A step of h at the transition from sample T to T + 1 (0 <= T < N), d (Q31) of a sample before sample T + 1.
 * The 4-point B-spline BLEP: the band-limited step minus the ideal one is, in units of h,
 *   sample T - 1: d^4 / 24,   T: B(1 - d),   T + 1: -B(d),   T + 2: -(1 - d)^4 / 24
 * with B(x) = 1/2 - 2x/3 + x^3/3 - x^4/8 (the cubic B-spline's integral), written as differences together with
 * the step itself (from sample T + 1 on). */
static void emit(int T, fract32 d, int32_t h)
{
    int32_t ra, rb, rc, rd;
#if defined(SYNC_BLEP0)                                      /* naive steps, for tests/alias.c */
    ra = rb = rc = rd = 0;
    (void)d;
#elif defined(SYNC_BLEP2)                                    /* the 2-point PolyBLEP, for tests/alias.c */
    fract32 e = OPZ_Q31_ONE - d;
    ra = rd = 0;
    rb = opz_mul_q31(h, opz_mul_q31(d, d) >> 1);
    rc = -opz_mul_q31(h, opz_mul_q31(e, e) >> 1);
#else
    /* Q15 is plenty here: the residuals are at most h / 2 and the timing is good to 1/32768 of a sample */
    uint32_t d1 = ((uint32_t)d + 0x8000u) >> 16, e1 = 32768u - d1;           /* Q15, rounded */
    uint32_t d2 = (opz_mul_u16(d1, d1) + 0x4000u) >> 15, e2 = (opz_mul_u16(e1, e1) + 0x4000u) >> 15;
    uint32_t d4 = (opz_mul_u16(d2, d2) + 0x4000u) >> 15, e4 = (opz_mul_u16(e2, e2) + 0x4000u) >> 15;
    uint32_t d3 = (opz_mul_u16(d2, d1) + 0x4000u) >> 15, e3 = (opz_mul_u16(e2, e1) + 0x4000u) >> 15;
    /* 2x/3 from 16 bits of d, rounded */
    uint32_t ld = (opz_mul_u16((uint32_t)d >> 15, 43691) + 0x10000u) >> 17, le = 21845u - ld;
    int32_t bd = 16384 - (int32_t)ld + (int32_t)((opz_mul_u16(d3, 10923) + 0x4000u) >> 15) - (int32_t)((d4 + 4) >> 3);
    int32_t be = 16384 - (int32_t)le + (int32_t)((opz_mul_u16(e3, 10923) + 0x4000u) >> 15) - (int32_t)((e4 + 4) >> 3);
    ra = opz_mul_q31_q15(h, (int32_t)((opz_mul_u16(d4, 43691) + 0x80000u) >> 20));   /* / 24 */
    rb = opz_mul_q31_q15(h, be);
    rc = -opz_mul_q31_q15(h, bd);
    rd = -opz_mul_q31_q15(h, (int32_t)((opz_mul_u16(e4, 43691) + 0x80000u) >> 20));
#endif
#ifdef SYNC_CHECK
    SYNC_CHECK(T, d);
#endif
    uint32_t *x = (uint32_t *)s_e + T;
    x[0] += (uint32_t)ra;
    x[1] += (uint32_t)(rb - ra);
    x[2] += (uint32_t)h + (uint32_t)(rc - rb);
    x[3] += (uint32_t)(rd - rc);
    x[4] -= (uint32_t)rd;
    if (T >= N - 2) {                                        /* residuals after the block: the next block's */
        if (T == N - 1) { s_k0 += rc; s_k1 += rd; }
        else s_k0 += rd;
    }
}

/* Transitions until phase p wraps, stepping by inc (recip = opz_blep_recip(inc)): the wrap falls between sample
 * +q and +q + 1, and *after is the phase at +q + 1. Returns N + 1 when it is further than that. */
ALWAYS_INLINE int until_wrap(uint32_t p, uint32_t inc, uint32_t recip, uint32_t *after)
{
    uint32_t np = ~p;                                        /* q = floor(np / inc) */
    if (inc < (1u << 24) && np >= (inc << 8)) return N + 1;  /* > 256 samples away; keeps the estimate in range */
    uint32_t q = opz_mul_u32_shr16(np >> 7, recip) >> 24;    /* give or take one */
    uint32_t rem = np - q * inc;                             /* exact modulo 2^32 */
    if (rem >= inc) {
        if (rem >= 0x80000000u) { q--; rem += inc; }
        else { q++; rem -= inc; }
    }
    *after = inc - 1u - rem;
    return q > N ? N + 1 : (int)q;
}

/* (a b) >> 32, to within 2: three 16 x 16 products (the low one only carries). */
ALWAYS_INLINE uint32_t mulhi(uint32_t a, uint32_t b)
{
    uint32_t ah = a >> 16, al = a & 0xffff, bh = b >> 16, bl = b & 0xffff;
    return opz_mul_u16(ah, bh) + (opz_mul_u16(ah, bl) >> 16) + (opz_mul_u16(al, bh) >> 16);
}

/* ph / inc in Q31 for ph < inc: how far past a wrap the next sample is, in samples. */
ALWAYS_INLINE fract32 frac(uint32_t ph, uint32_t recip)
{
    uint32_t x = opz_mul_u32_shr16(ph, recip);
    return x > OPZ_Q31_ONE ? OPZ_Q31_ONE : (fract32)x;
}

/* Every discontinuity of the block, in time order: the slave's wraps between resets, then each reset (with the
 * sub's toggle). Leaves the phases and the sub's sign at sample N in the state. gs: saw gain, gb: sub gain. */
static void events(State *s, uint32_t im, uint32_t rm, uint32_t is, uint32_t rs, int32_t gs, int32_t gb)
{
    uint32_t pm = s->pm, ps = s->ps;
    int32_t sub = s->sub, hw = -2 * gs;
    int t = 0;                                               /* pm, ps: the phases at sample t */
    if (is == im && ps - pm + 0x100u < 0x200u) {
        /* Ratio 1 and already in step (one reset after the ratio reached 1): a plain saw, one step per wrap.
         * P1 at 0 and every note above the slave's cap. */
        for (;;) {
            uint32_t pam;
            int tm = t + until_wrap(pm, im, rm, &pam);
            if (tm >= N) break;
            sub = -sub;
            emit(tm, frac(pam, rm), hw + (sub > 0 ? 2 * gb : -2 * gb));
            t = tm + 1;
            pm = pam;
        }
        s->pm = s->ps = pm + (uint32_t)(N - t) * im;
        s->sub = sub;
        return;
    }
    for (;;) {
        uint32_t pam;
        int tm = t + until_wrap(pm, im, rm, &pam);           /* the next reset, between tm and tm + 1 */
        int last = (tm < N ? tm : N) - 1;                    /* the slave's own wraps up to here */
        int j = t;
        uint32_t p = ps;                                     /* the slave's phase at sample j */
        for (;;) {
            uint32_t pa;
            int ts = j + until_wrap(p, is, rs, &pa);
            if (ts > last) break;
            emit(ts, frac(pa, rs), hw);
            p = pa;
            j = ts + 1;
        }
        if (tm >= N) {
            s->pm = pm + (uint32_t)(N - t) * im;
            s->ps = p + (uint32_t)(N - j) * is;
            break;
        }
        uint32_t pt = p + (uint32_t)(tm - j) * is;           /* the slave at sample tm */
        fract32 dm = frac(pam, rm);
        uint32_t post = mulhi((uint32_t)dm << 1, is);   /* its travel after the reset, < is */
        uint32_t pr = pt + (is - post);                      /* its phase at the reset */
        if (pr < pt) emit(tm, frac(pr + post, rs), hw);      /* it wrapped in the same sample, before the reset */
        sub = -sub;
        int32_t h = -gs - opz_mul_q31(gs, (int32_t)(pr ^ 0x80000000u)) + (sub > 0 ? 2 * gb : -2 * gb);
        emit(tm, dm, h);
        t = tm + 1;
        pm = pam;
        ps = post;
        if (t >= N) {
            s->pm = pm;
            s->ps = ps;
            break;
        }
    }
    s->sub = sub;
}

/* ---- envelope and output ----------------------------------------------------------------------------------- */

/* The host envelope 64 samples on (engine-abi.md §3: per sample r = c level, level = r + (1 - r^3) s), in closed
 * form as in vowel: s = 0 (decay, release): level c^64 by six squarings; c = 1 (attack, sustain): the step
 * (1 - r^3) s hardly moves within a block; otherwise (a fade with a step, a few blocks per voice steal) the exact
 * 64 steps. The audio follows it as a straight line across the block. */
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

/* Integrate the oscillator in place from acc (its value at sample 0, before the residuals) by slope + events:
 * afterwards s_e[i] is the output sample i (the oscillator one sample late) and s_e[N] the next block's first. */
static void integrate(State *s, uint32_t acc, uint32_t slope)
{
    uint32_t *e = (uint32_t *)s_e;
    uint32_t e0 = e[0];
    e[0] = e0 + (uint32_t)s->carry;                          /* sample -1, complete now */
    acc += e0 - slope;
    for (int i = 1; i <= N; i++) {
        acc += slope + e[i];                                 /* modulo 2^32; the sum is in range at every sample */
        e[i] = acc;
    }
    s->carry = (int32_t)e[N];
    e[N] = e[N + 1] = e[N + 2] = e[N + 3] = 0;
}

/* Noise: white (the LCG's top bits), the sum of two neighbours (a gentle zero at 22 kHz), times g (Q15). */
static void add_noise(State *s, int32_t g)
{
    uint32_t seed = s->seed;
    int32_t prev = s->nprev;
    for (int i = 0; i < N; i++) {
        seed = seed * 1664525u + 1013904223u;
        int32_t n = (int32_t)seed >> 1;
        s_e[i] = opz_add_q31(s_e[i], opz_mul_q31_q15(n + prev, g));
        prev = n;
    }
    s->seed = seed;
    s->nprev = prev;
}

/* The envelope as a ramp lv += dl, added into both channels; leaves s_e zeroed. */
static void pass_out(fract32 *out, int32_t lv, int32_t dl)
{
    for (int i = 0; i < N; i++) {
        fract32 v = opz_mul_q31_q15(s_e[i], lv >> 16);
        s_e[i] = 0;
        out[2 * i] = opz_add_q31(out[2 * i], v);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], v);
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
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    State *s = state;
    s->pm = rnd();                                           /* free-running from here on */
    s->ps = rnd();
    s->sub = 1;
    s->seed = rnd() | 1u;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note; (void)params;                                /* params may be NULL (engine-abi.md §3); not needed */
    State *s = state;
    int32_t vel = velocity > 127 ? 127 : velocity;
    s->amp = (fract32)((uint32_t)vel * 0x01020408u) >> 2;    /* vel / 127 x OPZ_VOICE_PEAK */
    s->voct = (vel - 100) * VEL_OCT;
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                 /* the host releases the envelope */
}

/* What a block needs from the knobs and the pitch. */
typedef struct Block {
    uint32_t im, is;        /* master and slave increments */
    uint32_t rm, rs;        /* their opz_blep_recip */
    int32_t gs, gb;         /* saw and sub gains, Q31 (velocity included) */
    int32_t gn;             /* noise gain, Q15 */
    int32_t dc;             /* the saw's DC correction, output units */
} Block;

static void setup(Block *k, const State *s, uint32_t pitch, int32_t pitchSlope, const uint32_t *params)
{
    /* Both oscillators run at the block's middle frequency: a glide moves in 64-frame steps (1.5 ms). */
    uint32_t im = pitch + (uint32_t)pitchSlope * (N / 2);
    if (im < INC_MIN) im = INC_MIN;
    if (im > INC_MAX) im = INC_MAX;
    uint32_t x = (uint32_t)opz_p16(params[OPZ_P_P1]);       /* 0 .. 65535 */
    int32_t oct = (int32_t)((x << 2) + ((x * x) >> 14)) + s->voct;   /* 4x + 4x^2 octaves, Q16 */
    if (oct < 0) oct = 0;
    if (oct > OCT_MAX) oct = OCT_MAX;
    uint32_t ratio = (uint32_t)opz_exp2_q16(oct);           /* Q16, 1 .. 256 */
    uint32_t is = opz_mul_u32_shr16(im >> 8, ratio) >= (SLAVE_MAX >> 8) ? SLAVE_MAX : opz_mul_u32_shr16(im, ratio);
    if (is < im) is = im;
    k->im = im;
    k->is = is;
    k->rm = opz_blep_recip(im);
    k->rs = opz_blep_recip(is);

    /* P2: sub on an equal-power curve over 0 .. 128 (saw cos, sub sin / sqrt 3: the square's RMS is sqrt 3 times
     * the saw's), then the noise over 128 .. 255 against both */
    int32_t p2 = opz_p16(params[OPZ_P_P2]);
    fract32 a, b;
    k->gn = 0;
    if (p2 < 32768) {
        uint32_t ph = (uint32_t)p2 << 14;                    /* 0 .. 1/8 turn */
        a = opz_sine(ph + 0x40000000u);
        b = opz_mul_q31(opz_sine(ph), 0x49e69d16);           /* 1 / sqrt 3 */
    } else {
        uint32_t ph = (uint32_t)(p2 - 32768) << 14;
        fract32 c = opz_sine(ph + 0x40000000u);
        a = opz_mul_q31(c, 0x5a82799a);                      /* 1 / sqrt 2 */
        b = opz_mul_q31(c, 0x34417ae0);                      /* 1 / sqrt 6 */
        k->gn = opz_shl_q31(opz_mul_q31(opz_mul_q31(opz_sine(ph), s->amp), NOISE_NORM), 1) >> 16;
    }
    /* A band-limited slave near the top keeps few harmonics (the BLEP rolls off too): -4.6 dB RMS at the cap, -2.4 dB
     * at 4.4 kHz. The saw gets it back, 1 + 0.7 f / 10 kHz, so a ratio sweep stays level. */
    uint32_t in = is > SLAVE_MAX ? SLAVE_MAX : is;
    int32_t gs = opz_mul_q31(a, s->amp);
    k->gs = gs + opz_mul_q31(gs, (fract32)opz_mul_u32_shr16(in, COMP_K));
    k->gb = opz_mul_q31(b, s->amp);

    /* the synced saw's DC, -f (1 - f) / r (r = is / im, f its fractional part), taken off */
    uint32_t r23 = opz_mul_u32_shr16(is >> 8, k->rm);       /* r, Q23 */
    fract32 f = (fract32)((r23 & 0x7fffffu) << 8);
    uint32_t inv = opz_mul_u32_shr16(im, k->rs);            /* 1 / r, Q31 */
    fract32 dc = opz_mul_q31(opz_mul_q31(f, OPZ_Q31_ONE - f), inv > OPZ_Q31_ONE ? OPZ_Q31_ONE : (fract32)inv);
    k->dc = opz_mul_q31(k->gs, dc);
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);
    Block k;
    setup(&k, s, pitch, pitchSlope, params);
    /* the naive oscillator at sample 0, its slope */
    uint32_t acc = (uint32_t)opz_mul_q31(k.gs, (int32_t)(s->ps ^ 0x80000000u)) + (uint32_t)(s->sub > 0 ? k.gb : -k.gb)
                 + (uint32_t)k.dc;
    uint32_t slope = (uint32_t)opz_mul_q31(k.gs, (int32_t)k.is);

    /* residuals the last block left on samples 0 and 1, then this block's events */
    uint32_t *e = (uint32_t *)s_e;
    e[1] += (uint32_t)s->c0;
    e[2] += (uint32_t)s->c1 - (uint32_t)s->c0;
    e[3] -= (uint32_t)s->c1;
    s_k0 = s_k1 = 0;
    events(s, k.im, k.rm, k.is, k.rs, k.gs, k.gb);
    s->c0 = s_k0;
    s->c1 = s_k1;

    int32_t l0 = OPZ_ADSR_LEVEL(env);
    int32_t l1 = env_block(l0, OPZ_ADSR_COEF(env), OPZ_ADSR_STEP(env));
    integrate(s, acc, slope);
    if (k.gn) add_noise(s, k.gn);
    pass_out(out, l0, (l1 - l0) >> 6);
    OPZ_ADSR_LEVEL(env) = l1;
}

OPZ_EXPORTS_INSTRUMENT();
