/*
 * spring — dispersive spring reverb for FX1 and FX2: two springs whose echoes chirp ("drip") the way a spring tank's
 * do, and a tank you can kick from the FX track's keys.
 *
 *   P1     tension    loose (0) -> tight (255): spring A's round trip 75 ms -> 25 ms (spring B 12.7 % longer),
 *                     the late decay (T60) from ~3.1 s to ~2.5 s. Moving P1 glides the length (1/8 sample per sample),
 *                     so turning it bends the tail like a stretched spring instead of clicking
 *   P2     drip       how much each pass disperses and how bright it stays: the chirp all-pass coefficient
 *                     0.35 -> 0.82 (the low end arrives 1.7 -> 11 ms after the top end, per pass) and the loop's
 *                     damping 2.5 -> 6.5 kHz. 0 = a smooth, dark spring; 255 = the surf-guitar "pew" on every hit
 *   pan    dry / wet  the SDK's FX law: 128 .. 255 the springs only (the OS's dry level keeps the source), below
 *                     128 the dry blends in at equal power, 0 = dry only; output through the SDK's limiter
 *   level  output level (x gain)
 *   notes  on the FX track: kick the tank. A noise burst (20 ms decay) goes into both springs, low-passed at 8x the
 *          note's frequency (C3 a thud, C6 a bright crash) and as loud as the velocity. Note off does nothing;
 *          the OS's velocity-0 re-send of a held note is ignored
 *
 * Each spring is a loop: input + feedback -> 8 stretched first-order all-passes (H = (-a + z^-6) / (1 - a z^-6),
 * a lossless chirp: group delay 6 (1 + a) / (1 - a) samples at DC down to 6 (1 - a) / (1 + a) at 3.7 kHz, so the
 * low end of each echo trails the top: the descending "drip") -> two short Schroeder all-passes (so the passes
 * smear into a dense tail instead of a flutter echo) -> the spring's delay line (linear interpolation) -> a
 * one-pole damping low-pass -> the decay gain -> back in. Spring A goes left, spring B right, both fed with the
 * mono sum, band-limited 100 Hz .. 5 kHz like a tank's transducers. The loops run at -24 dB (input x 1/16) for
 * headroom inside the all-passes (their memories reach |x| / (1 - a)); every add saturates.
 *
 * The stretched all-pass chain is the standard dispersion model of spring reverbs: J. Parker, "Efficient
 * dispersion generation structures for spring reverb emulation", EURASIP JASP 2011; V. Välimäki, J. Parker,
 * J. S. Abel, "Parametric spring reverberation effect", JAES 58(7/8), 2010. Our structure, lengths and
 * coefficients; no code from either. The filter and resonance knobs are the OS's own track filter; pitch bend is
 * not used. Q31 signals, no floating point, no libc, no 64-bit maths or division per sample. MIT licence, see
 * CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define STAGES          8
#define K               6                       /* all-pass stretch: z^-6 */
#define RING            4096                    /* samples per spring delay line, a power of two */
#define RMASK           (RING - 1)
#define H_WORDS         (2 * STAGES * K)        /* all-pass memories, both springs */
#define DIF             512                     /* diffuser rings, a power of two dividing RING */
#define DMASK           (DIF - 1)
#define MEM_WORDS       (2 * RING + H_WORDS + 4 * DIF)
#define MEM_BYTES       (MEM_WORDS * 4u)        /* 41,344 B */
#define DIF_G           19661                   /* diffuser coefficient 0.6, Q15 */
#define CLEAR_STEP      2048                    /* words cleared per block: init runs with interrupts masked */

#define D_LOOSE         3300                    /* spring A's delay at P1 0, samples (75 ms) */
#define LOG2_RANGE      103872                  /* log2(3) in Q16: 75 ms -> 25 ms */
#define B_RATIO         73859                   /* spring B = 1.127 x A, Q16 */
#define LG_LOOSE        (-15273)                /* log2 of the loop gain at P1 0 (T60 3.2 s over 3300), Q16 */
#define LG_TIGHT        (-10182)                /* at P1 255 (T60 1.6 s over 1100) */
#define A_MIN           11469                   /* chirp all-pass coefficient 0.35, Q15 */
#define A_MAX           26870                   /* 0.82 */
#define DAMP_MIN        9819                    /* one-pole 1 - e^(-2 pi fc / fs), Q15: 2.5 kHz */
#define DAMP_MAX        19789                   /* 6.5 kHz */
#define K_HP            464                     /* input high-pass 100 Hz, Q15 */
#define K_LP            16696                   /* input low-pass 5 kHz, Q15 */
#define KICK_DECAY      32731                   /* e^(-1 / 20 ms) per sample, Q15 */
#define KICK_HZ_K       75                      /* 2^15 2 pi / fs, x 16: Q15 one-pole coefficient per Hz / 16 */
#define IN_SHIFT        4                       /* the loops run at x / 16 */
#define OUT_GAIN        0x60000000              /* wet level 0.75 x 32 / 16 = 1.5: the loop output << 5, x 0.75 */
#define SLEW            8192                    /* length glide: Q16 samples per sample (1/8) */

typedef struct State {
    int32_t *mem;                               /* this FX track's host buffer: rings, then all-pass memories */
    uint32_t cleared;                           /* words cleared so far */
    uint32_t w;                                 /* write position in the rings */
    uint32_t j;                                 /* slot of z^-6 in the all-pass memories, 0..K-1 */
    int32_t d[2];                               /* delay lengths reached, Q16 samples */
    int32_t a;                                  /* chirp coefficient reached, Q15 */
    int32_t hp, lp;                             /* input band limit */
    int32_t damp[2];                            /* loop low-passes */
    int32_t fb[2];                              /* loop outputs after the decay gain */
    uint32_t seed;                              /* kick noise */
    int32_t kick_lp;                            /* kick low-pass memory */
    /* written by onNoteOn (word stores, the level last: safe if render runs in the middle) */
    int32_t kick_c;                             /* kick low-pass coefficient, Q15 */
    int32_t kick_env;                           /* kick level, Q31 */
    uint8_t started;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* Two Schroeder all-passes per spring after the chirp (5.1, 7.7 ms left; 5.7, 8.6 ms right: mutually prime), so
 * the passes of the loop smear into a dense tail instead of a flutter echo */
static const uint16_t k_dif[4] = { 223, 341, 251, 379 };

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = MEM_BYTES;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)arg2; (void)scratch;
    State *s = state;
    volatile uint8_t *p = (volatile uint8_t *)s;               /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    /* 41 KB fits FX1 and FX2 (and master's 16 KB would not): dry anywhere else */
    s->mem = (track == OPZ_TRACK_FX1 || track == OPZ_TRACK_FX2) ? (int32_t *)mem : (int32_t *)0;
    s->seed = 0x2545f491u;
}

/* params may be NULL (the OS re-sends a held note with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127 || velocity == 0) return;      /* 255: plug selected; 0: a re-sent held note */
    int32_t hz = (440 * opz_exp2_q16((note - 69) * 5461)) >> 13;   /* 8 x the note's frequency, Hz */
    if (hz < 60) hz = 60;
    if (hz > 6000) hz = 6000;
    s->kick_c = (hz * KICK_HZ_K) >> 4;                         /* small-angle one-pole, Q15: <= 0.86 */
    s->kick_env = (int32_t)(velocity > 127 ? 127 : velocity) << 24;   /* velocity 127 = 0.99 */
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;
}

#define ALWAYS_INLINE static inline __attribute__((always_inline))

/* One spring for one sample: the chirp, the delay line and the loop. h: its all-pass memories, j: the slot of
 * z^-6 in them (shared by all stages), ring: its delay line. Returns the spring's output. */
ALWAYS_INLINE int32_t spring_tick(State *s, int c, int32_t x, int32_t a, int32_t *h, uint32_t j, int32_t *ring,
                                  int32_t *dif, uint32_t w, int32_t dq, int32_t damp, int32_t g)
{
    int32_t v = opz_add_q31(x, s->fb[c]);
    for (int k = 0; k < STAGES; k++) {
        int32_t *hk = h + k * K + j;
        int32_t old = *hk;
        int32_t u = opz_add_q31(v, opz_mul_q31_q15(old, a));       /* w[n] = v + a w[n - 4] */
        v = opz_sub_q31(old, opz_mul_q31_q15(u, a));               /* y = w[n - 4] - a w[n] */
        *hk = u;
    }
    for (int k = 0; k < 2; k++) {                              /* y = d - g x', x' = x + g d: the delay keeps x' */
        int32_t *dk = dif + k * DIF;
        int32_t d = dk[(w - k_dif[2 * c + k]) & DMASK];
        int32_t xi = opz_add_q31(v, opz_mul_q31_q15(d, DIF_G));
        dk[w & DMASK] = xi;
        v = opz_sub_q31(d, opz_mul_q31_q15(xi, DIF_G));
    }
    ring[w] = v;
    uint32_t di = (uint32_t)dq >> 16;
    int32_t fr = (dq & 0xffff) >> 1;                           /* Q15 */
    int32_t s0 = ring[(w - di) & RMASK], s1 = ring[(w - di - 1) & RMASK];
    int32_t r = opz_add_q31(s0, opz_mul_q31_q15(opz_sub_q31(s1, s0), fr));
    s->damp[c] = opz_add_q31(s->damp[c], opz_mul_q31_q15(opz_sub_q31(r, s->damp[c]), damp));
    s->fb[c] = opz_mul_q31_q15(s->damp[c], g);
    return r;
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block --------------------------------------------------------------------------------------- */
    int32_t u1 = opz_p16(params[OPZ_P_P1]), u2 = opz_p16(params[OPZ_P_P2]);
    int32_t ta = D_LOOSE * opz_exp2_q16(-(int32_t)opz_mul_u32_shr16((uint32_t)u1, LOG2_RANGE));   /* Q16 samples */
    int32_t tb = (int32_t)opz_mul_u32_shr16((uint32_t)ta, B_RATIO);
    int32_t lg = LG_LOOSE + (((LG_TIGHT - LG_LOOSE) * u1) >> 16);
    int32_t ga = opz_exp2_q16(lg) >> 1;                                   /* Q15, < 1: lg < 0 */
    int32_t gb = opz_exp2_q16((lg * B_RATIO) >> 16) >> 1;           /* fits: |lg| < 2^14 */
    int32_t a_t = A_MIN + (((A_MAX - A_MIN) * u2) >> 16);
    int32_t damp = DAMP_MIN + (((DAMP_MAX - DAMP_MIN) * u2) >> 16);

    int32_t dry, wet;                                          /* the SDK's FX law (equal power) */
    opz_fx_drywet(params[OPZ_P_PAN], &dry, &wet);
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & OPZ_Q31_ONE), gain);
    int32_t dg = opz_mul_q31(dry, out_gain) >> 1;              /* dry and wet with the level in them, the dry at */
    int32_t wg = opz_mul_q31(opz_mul_q31(wet, out_gain), OUT_GAIN);   /* half scale for the output limiter */

    int32_t *mem = s->mem;
    int ready = 0;
    if (mem) {
        if (s->cleared < MEM_WORDS) {
            volatile int32_t *z = mem + s->cleared;
            uint32_t n = MEM_WORDS - s->cleared;
            if (n > CLEAR_STEP) n = CLEAR_STEP;
            for (uint32_t i = 0; i < n; i++) z[i] = 0;
            s->cleared += n;
        } else {
            ready = 1;
        }
    }
    if (!ready) {                                              /* clearing (5 blocks) or no buffer: dry only */
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = opz_limit_half(opz_mul_q31(dg, in[i]));
        return;
    }
    if (!s->started) {                                         /* first block: no glide from zero */
        s->d[0] = ta; s->d[1] = tb; s->a = a_t;
        s->started = 1;
    }
    /* the chirp coefficient moves 1/8 of the way per block; the lengths glide per sample below */
    int32_t a = s->a = s->a + ((a_t - s->a) >> 3);

    /* ---- per sample -------------------------------------------------------------------------------------- */
    int32_t *ha = mem + 2 * RING, *hb = ha + STAGES * K, *dfa = mem + 2 * RING + H_WORDS, *dfb = dfa + 2 * DIF;
    uint32_t w = s->w, j = s->j;
    int32_t da = s->d[0], db = s->d[1];
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t xl = in[2 * i], xr = in[2 * i + 1];
        int32_t x = ((xl >> 1) + (xr >> 1)) >> IN_SHIFT;
        s->hp = opz_add_q31(s->hp, opz_mul_q31_q15(opz_sub_q31(x, s->hp), K_HP));
        s->lp = opz_add_q31(s->lp, opz_mul_q31_q15(opz_sub_q31(opz_sub_q31(x, s->hp), s->lp), K_LP));
        x = s->lp;
        if (s->kick_env > 0x10000) {                           /* a kick: decaying noise through the note's low-pass */
            int32_t n = opz_mul_q31_q15(opz_noise(&s->seed), s->kick_env >> 16);
            s->kick_lp = opz_add_q31(s->kick_lp, opz_mul_q31_q15(opz_sub_q31(n, s->kick_lp), s->kick_c));
            s->kick_env = opz_mul_q31_q15(s->kick_env, KICK_DECAY);
            x = opz_add_q31(x, s->kick_lp >> 2);
        }
        da += da < ta ? (ta - da < SLEW ? ta - da : SLEW) : -(da - ta < SLEW ? da - ta : SLEW);
        db += db < tb ? (tb - db < SLEW ? tb - db : SLEW) : -(db - tb < SLEW ? db - tb : SLEW);
        int32_t ra = spring_tick(s, 0, x, a, ha, j, mem, dfa, w, da, damp, ga);
        int32_t rb = spring_tick(s, 1, x, a, hb, j, mem + RING, dfb, w, db, damp, gb);
        w = (w + 1) & RMASK;
        if (++j == K) j = 0;
        out[2 * i] = opz_mul_q31(wg, opz_shl_q31(ra, 4));     /* the springs at half scale */
        out[2 * i + 1] = opz_mul_q31(wg, opz_shl_q31(rb, 4));
    }
    opz_fx_finish(out, in, dg, dg);                            /* + the dry (none at pan 128 .. 255), limited */
    s->w = w;
    s->j = j;
    s->d[0] = da;
    s->d[1] = db;
}

OPZ_EXPORTS_FX();
