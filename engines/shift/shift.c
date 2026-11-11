/*
 * shift — ring modulator and frequency shifter in one knob, for FX1 and FX2; keys on the FX track tune the carrier.
 *
 *   P1     carrier    0.2 Hz (0) -> 3.3 kHz (255), exponential (14 octaves: 128 = 26 Hz). Low settings are
 *                     tremolo (ring) or a slow barber-pole phasing (shift); higher ones the metallic, inharmonic
 *                     sounds
 *   P2     mode       a continuous morph: 0 = shift down (lower sideband only), 128 = ring modulation (both
 *                     sidebands), 255 = shift up (upper sideband only). In between, one sideband fades against the
 *                     other. The level is evened out: ring mode is +3 dB, so every setting is about as loud
 *   pan    dry / wet  the SDK's FX law: 128 .. 255 the shifted sound only (the OS's dry level keeps the source),
 *                     below 128 the dry blends in at equal power, 0 = dry only; output through the SDK's limiter
 *   level  output level (x gain)
 *   notes  on the FX track: a held key sets the carrier to the note's frequency (A4 = 440 Hz), for tuned ring
 *          modulation and shifts by a musical amount; release returns to P1. The newest note wins; the OS's
 *          velocity-0 re-send of a held note sets the carrier the same way
 *
 * How: a 90-degree phase splitter (two chains of four second-order all-passes, the second chain one sample
 * behind) turns each channel into an analytic pair (I, Q), 90 degrees apart within 1.9 degrees from 30 Hz to
 * 20 kHz: the unwanted sideband is ~35 dB down. Then out = I cos(wt) - m Q sin(wt), m = -1 .. 0 .. 1 from P2:
 * m = 1 is cos((w0 + w) t) for an input cos(w0 t) (shift up), m = -1 the lower sideband, m = 0 I cos(wt) (ring).
 * The carrier comes from the host's sine table (8,192 points, no interpolation: spurs below -70 dB). Nothing is
 * band-limited before the shift: content shifted past 22 kHz folds back, as on analogue-modelled shifters.
 *
 * All-pass coefficients: our own minimax design (Nelder-Mead on the phase error over 30 Hz .. 20 kHz at 44.1 kHz),
 * the structure of the classic IIR Hilbert pair; no code or coefficients from any source. All state lives in the
 * FX state (no buffer). The filter and resonance knobs are the OS's own track filter; pitch bend is not used. Q31
 * signals, no floating point, no libc, no 64-bit maths or division per sample. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define SECT            4                       /* second-order all-pass sections per chain */
#define CAR_MIN_Q16     13107                   /* carrier at P1 0: 0.2 Hz, Q16 */
#define CAR_OCT         (14 << 16)              /* range: 14 octaves */
#define RING_GAIN       27146                   /* +3 dB at m = 0 (sqrt 2 - 1, Q16): one sideband each side */

/* y[n] = c (x[n] + y[n-2]) - x[n-2]: H = (c - z^-2) / (1 - c z^-2). Chain A gives I, chain B (fed x[n-1]) Q */
static const int32_t k_ca[SECT] = { 476769766, 1780522483, 2076163403, 2123113749 };
static const int32_t k_cb[SECT] = { 1276516243, 2000367519, 2102126932, 2140805351 };

typedef struct State {
    /* the chains' signals at n-1 and n-2, by parity: h[channel][chain][signal][n & 1]; signal k is section k's
       input, k + 1 its output */
    int32_t h[2][2][SECT + 1][2];
    int32_t xd[2];                              /* chain B's one-sample delay */
    uint32_t phase;                             /* carrier, Q32 turns */
    /* written by onNoteOn/onNoteOff (a word, then bytes: safe if render runs in the middle) */
    uint32_t note_inc;                          /* carrier increment of the held note */
    uint8_t note_held;
    uint8_t note;
    uint8_t parity;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

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
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;           /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

/* params may be NULL (the OS re-sends a held note with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params; (void)velocity;
    State *s = state;
    if (note < 0 || note > 127) return;                       /* 255: plug selected, no note */
    int32_t hz_q16 = (int32_t)((uint32_t)440 * (uint32_t)opz_exp2_q16((note - 69) * 5461));   /* < 2^31 */
    s->note_inc = opz_hz_to_inc(hz_q16);
    s->note = (uint8_t)note;
    s->note_held = 1;
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note == s->note) s->note_held = 0;
}

/* One chain of four all-passes on x for parity p; h: its signals */
static inline int32_t chain(int32_t (*h)[2], const int32_t *c, int32_t x, int p)
{
    int32_t v = x;
    for (int k = 0; k < SECT; k++) {
        int32_t y = opz_sub_q31(opz_mul_q31(c[k], opz_add_q31(v, h[k + 1][p])), h[k][p]);
        h[k][p] = v;
        v = y;
    }
    h[SECT][p] = v;
    return v;
}

static inline fract32 sine_at(const fract32 *tab, uint32_t phase)
{
    return tab ? tab[phase >> 19] : opz_sine(phase);
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block --------------------------------------------------------------------------------------- */
    uint32_t inc;
    if (s->note_held) {
        inc = s->note_inc;
    } else {
        uint32_t u = (uint32_t)opz_p16(params[OPZ_P_P1]);
        int32_t hz_q16 = (int32_t)opz_mul_u32_shr16(CAR_MIN_Q16,
                                                    (uint32_t)opz_exp2_q16((int32_t)opz_mul_u32_shr16(u, CAR_OCT)));
        inc = opz_hz_to_inc(hz_q16);
    }
    /* P2 -> m in Q31 (-1 .. 1), and the gain that evens the level out (x 1 .. sqrt 2 at m = 0), as Q31 / 2 */
    int32_t u2 = opz_p16(params[OPZ_P_P2]);
    int32_t m = (u2 - 32768) << 16;                            /* -2^31 .. 2^31 - 2^16 */
    int32_t am = m < 0 ? -(m >> 15) : m >> 15;                 /* |m|, 0..65536 */
    int32_t lv = 32768 + ((RING_GAIN * (65536 - am)) >> 17);   /* level / 2, Q16: 0.5 .. 0.707 */

    int32_t pd, pw;                                            /* the SDK's FX law (equal power: a shifted signal */
    opz_fx_drywet(params[OPZ_P_PAN], &pd, &pw);                /* is unrelated to the dry) */
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & OPZ_Q31_ONE), gain);
    int32_t dg = opz_mul_q31(pd, out_gain) >> 1;               /* half scale, for the output limiter */
    /* the wet path runs at x / 4 for headroom in the all-passes (the 90-degree image of a saw or a pulse peaks far
     * above the input: at x / 2 the chains saturated); lv (level / 2) and << 2 bring it to half scale */
    int32_t wg = opz_mul_q31(pw, out_gain);
    int32_t wl = lv << 15;                                     /* level / 2, Q31 */

    const fract32 *tab = s_cb ? s_cb->g_sine : (const fract32 *)0;

    /* ---- per sample -------------------------------------------------------------------------------------- */
    uint32_t ph = s->phase;
    int p = s->parity;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        fract32 sn = sine_at(tab, ph), cs = sine_at(tab, ph + 0x40000000u);
        fract32 msn = opz_mul_q31(m, sn);
        ph += inc;
        for (int c = 0; c < 2; c++) {
            int32_t x = in[2 * i + c];
            int32_t xh = x >> 2;
            int32_t vi = chain(s->h[c][0], k_ca, xh, p);
            int32_t vq = chain(s->h[c][1], k_cb, s->xd[c], p);
            s->xd[c] = xh;
            int32_t y = opz_sub_q31(opz_mul_q31(vi, cs), opz_mul_q31(vq, msn));
            y = opz_shl_q31(opz_mul_q31(y, wl), 2);
            out[2 * i + c] = opz_mul_q31(wg, y);              /* the wet at half scale */
        }
        p ^= 1;
    }
    opz_fx_finish(out, in, dg, dg);                            /* + the dry, through the SDK's limiter */
    s->phase = ph;
    s->parity = (uint8_t)p;
}

OPZ_EXPORTS_FX();
