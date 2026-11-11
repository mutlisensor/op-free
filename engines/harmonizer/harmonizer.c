/*
 * harmonizer — two pitch-shifted voices over a short, moving delay network for FX1 and FX2: a slow detune shimmer
 * at the centre of P1, harmonised intervals towards its ends, and keys on the FX track that play intervals while
 * they are held.
 *
 *   P1     shift      bipolar around 128. 96..159: detune, the two voices +- up to 25 cents (a wide doubler at
 *                     the edges of the zone, nothing at 128). Outside it, intervals in steps of 16: up 160 +3
 *                     (minor third), 176 +4, 192 +5, 208 +7, 224 +9, 240..255 +12; down the same mirrored (95 -3 ..
 *                     0..15 -12): symmetric, both voices on the interval, 6 cents apart for thickness
 *   P2     space      0 = the voices alone (dry-sounding harmony); up: a stereo delay network (20 -> 110 ms left,
 *                     1.37x right, slowly moving) mixes in and feeds back through the shifter (up to 0.85), so the
 *                     harmony repeats and climbs (or falls) by the interval and smears into a swirl
 *   pan    dry / wet  the SDK's FX law: 128 .. 255 the harmony only (the OS's dry level keeps the source), below
 *                     128 the dry blends in at equal power, 0 = dry only; output through the SDK's limiter
 *   level  output level (x gain)
 *   notes  on the FX track: a held key sets the interval for as long as it is held, its distance from C4 (C4 =
 *          unison, G4 +7, C5 +12, F3 -7, clamped to +-24): voice 1 on the newest key, voice 2 on the key held
 *          before it (two keys = two-voice harmony) or 6 cents below voice 1. Release returns to P1 (momentary
 *          keys; there is no latch). The OS's velocity-0 re-send of a held note keeps it held
 *
 * The shifter: the input (mono sum, plus the feedback) is written into an 8,192-sample ring; each voice reads it with
 * two taps whose delays sweep across a 4,096-sample window (93 ms) half a window apart, at a rate set by the
 * ratio, crossfaded by triangles (the classic delay-line pitch shifter, as engines/shimmer uses), with +3.5 dB to
 * make up what the crossfade of two unrelated taps loses. Voice 1 leans left, voice 2 right. Like every shifter of
 * this kind it lands a pure tone up to half the window rate off the interval (the taps' phase steps: up to 5 Hz at
 * +12, 2.6 Hz at +7); sounds with many partials average out on the interval. The delay network is a stereo pair of 16-bit lines (8,192 frames) read with linear interpolation;
 * their lengths glide (1/8 sample per sample) as P2 moves. The feedback is band-limited (150 Hz .. 5 kHz) and
 * softly saturated, so the cascade fades out instead of building up.
 *
 * Inspired by the behaviour of TE's unreleased Swirl (a pitch-shift ambience that follows held keys; docs/ideas
 * effects.md X-11): our own delay lengths, modulation, symmetric intervals and code; none of TE's. The filter and
 * resonance knobs are the OS's own track filter; pitch bend is not used. Q31 signals, Q15 coefficients, no floating
 * point, no libc, no 64-bit maths or division per sample. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define SR_N            8192                    /* shifter ring, int32, a power of two */
#define SR_MASK         (SR_N - 1)
#define SW              4096                    /* shifter window: the taps' delays sweep 0 .. SW (93 ms) */
#define SW_K            16                      /* 2^32 / SW / 2^16: window phase per sample = (ratio - 1) Q16 x this */
#define DL_N            8192                    /* delay network, packed stereo int16 frames */
#define DL_MASK         (DL_N - 1)
#define MEM_WORDS       (SR_N + DL_N)
#define MEM_BYTES       (MEM_WORDS * 4u)        /* 64 KB: fits FX1 and FX2 */
#define CLEAR_STEP      4096                    /* words cleared per block (init runs with interrupts masked) */
#define NOTE_ZERO       60                      /* C4: unison */
#define SEMI_LIMIT      24
#define THICK           3932                    /* 6 cents in Q16 semitones */
#define DETUNE_ZONE     8192                    /* P1 within +-8192 of the centre (bytes 96..159): detune */
#define D_MIN           900                     /* delay network, left: 900 .. 4900 samples (20 .. 111 ms) */
#define D_RANGE         4000
#define D_RATIO         89784                   /* right = 1.37 x left, Q16 */
#define MOD_MIN         10                      /* delay modulation +-10 .. 70 samples */
#define MOD_RANGE       60
#define MOD_INC_L       1932735u                /* 0.31 Hz per block, Q32 */
#define MOD_INC_R       2680413u                /* 0.43 Hz */
#define FB_MAX          27853                   /* feedback 0.85, Q15 */
#define K_FB_HP         700                     /* feedback high-pass 150 Hz, Q15 */
#define K_FB_LP         16696                   /* feedback low-pass 5 kHz, Q15 */
#define SLEW            8192                    /* delay glide: Q16 samples per sample (1/8) */

static const int8_t k_interval[6] = { 3, 4, 5, 7, 9, 12 };

typedef struct State {
    int32_t *mem;                               /* this FX track's host buffer: shifter ring, then the delay pair */
    uint32_t cleared;                           /* words cleared so far */
    uint32_t w;                                 /* write position (both rings: SR_N divides DL_N) */
    uint32_t ph[2];                             /* window phase of each voice */
    uint32_t mod_ph[2];                         /* delay modulation phases */
    int32_t d[2];                               /* delay lengths reached, Q16 samples */
    int32_t fb;                                 /* feedback into the shifter, filtered, last sample */
    int32_t fb_hp, fb_lp;
    uint8_t started;
    /* written by onNoteOn/onNoteOff (single bytes: safe if render runs in the middle); 0xff = none */
    uint8_t n1;                                 /* the newest held key */
    uint8_t n2;                                 /* the key held before it */
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

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
    s->mem = (track == OPZ_TRACK_FX1 || track == OPZ_TRACK_FX2) ? (int32_t *)mem : (int32_t *)0;
    s->ph[1] = 0x40000000u;                                    /* voice 2 a quarter window on: decorrelated */
    s->mod_ph[1] = 0x80000000u;
    s->n1 = s->n2 = 0xff;
}

/* params may be NULL (the OS re-sends a held note with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params; (void)velocity;
    State *s = state;
    if (note < 0 || note > 127) return;                       /* 255: plug selected, no note */
    if (note == s->n1) return;                                 /* already the newest (or the OS's re-send) */
    s->n2 = note == s->n2 ? 0xff : s->n1;
    s->n1 = (uint8_t)note;
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note == s->n1) {
        s->n1 = s->n2;
        s->n2 = 0xff;
    } else if (note == s->n2) {
        s->n2 = 0xff;
    }
}

static inline int32_t note_semis(int n)
{
    int v = n - NOTE_ZERO;
    return (v < -SEMI_LIMIT ? -SEMI_LIMIT : v > SEMI_LIMIT ? SEMI_LIMIT : v) * 65536;
}

/* Window phase step for a shift of semis (Q16 semitones): (2^(semis / 12) - 1) x 2^32 / SW, signed */
static inline int32_t shift_inc(int32_t semis)
{
    int32_t oct = (semis >> 16) * 5461 + (int32_t)(((uint32_t)semis & 0xffff) * 5461u >> 16);   /* semis / 12 */
    return (opz_exp2_q16(oct) - 65536) * SW_K;
}

/* One voice: two taps sweeping the window, triangle-crossfaded. sr: the ring, w: this sample's write position */
static inline int32_t voice(const int32_t *sr, uint32_t w, uint32_t ph)
{
    uint32_t u = ph >> 16;
    uint32_t pa = opz_mul_u16(u, SW), pb = opz_mul_u16(u ^ 0x8000u, SW);   /* delays, Q16 samples */
    uint32_t ja = w - (pa >> 16), jb = w - (pb >> 16);
    int32_t a0 = sr[ja & SR_MASK], b0 = sr[jb & SR_MASK];
    int32_t ta = opz_add_q31(a0, opz_mul_q31_q15(opz_sub_q31(sr[(ja - 1) & SR_MASK], a0), (pa >> 1) & 0x7fff));
    int32_t tb = opz_add_q31(b0, opz_mul_q31_q15(opz_sub_q31(sr[(jb - 1) & SR_MASK], b0), (pb >> 1) & 0x7fff));
    int32_t g = (int32_t)((u ^ (0u - (u >> 15))) & 0x7fffu);    /* tap a's weight: 0 where its delay jumps */
    return opz_add_q31(tb, opz_mul_q31_q15(opz_sub_q31(ta, tb), g));
}

/* One channel of the delay pair at d (Q16 samples); hi: the channel's half of the packed word */
static inline int32_t dl_read(const uint32_t *dl, uint32_t w, int32_t d, int hi)
{
    uint32_t i = (uint32_t)d >> 16;
    int32_t f = (d & 0xffff) >> 1;
    uint32_t a = dl[(w - i) & DL_MASK], b = dl[(w - i - 1) & DL_MASK];
    int32_t va = hi ? (int32_t)(a & 0xffff0000u) : (int32_t)(a << 16);
    int32_t vb = hi ? (int32_t)(b & 0xffff0000u) : (int32_t)(b << 16);
    return opz_add_q31(va, opz_mul_q31_q15(opz_sub_q31(vb, va), f));
}

static inline int32_t glide(int32_t d, int32_t t)
{
    return d < t ? (t - d < SLEW ? t : d + SLEW) : (d - t < SLEW ? t : d - SLEW);
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block: the two voices' shifts ----------------------------------------------------------------- */
    int32_t s1, s2;
    uint8_t n1 = s->n1, n2 = s->n2;
    if (n1 != 0xff) {
        s1 = note_semis(n1) + THICK;
        s2 = n2 != 0xff ? note_semis(n2) : s1 - 2 * THICK;
    } else {
        int32_t dv = opz_p16(params[OPZ_P_P1]) - 32768, a = dv < 0 ? -dv : dv;
        if (a < DETUNE_ZONE) {                                 /* +-25 cents at the zone's edges */
            s1 = (dv * 2);                                     /* 25 cents = 16384 Q16 semitones at 8192 */
            s2 = -s1;
        } else {
            int k = (a - DETUNE_ZONE) >> 12;
            int32_t semi = (int32_t)k_interval[k > 5 ? 5 : k] * 65536;
            if (dv < 0) semi = -semi;
            s1 = semi + THICK;
            s2 = semi - THICK;
        }
    }
    int32_t inc1 = shift_inc(s1), inc2 = shift_inc(s2);

    /* P2: space */
    int32_t t = opz_p16(params[OPZ_P_P2]);
    int32_t t2 = (int32_t)(opz_mul_u16((uint32_t)t, (uint32_t)t) >> 16);
    int32_t fbg = (FB_MAX * t2) >> 16;                         /* Q15 */
    int32_t gs = (26214 * t) >> 16;                            /* delay mix 0 .. 0.8, Q15 */
    int32_t gd = 32767 - ((13107 * t) >> 16);                  /* voices 1 .. 0.6 */
    int32_t dl_t = (D_MIN << 16) + D_RANGE * t;                /* Q16 samples */
    int32_t mod = (MOD_MIN << 16) + MOD_RANGE * t;
    s->mod_ph[0] += MOD_INC_L;
    s->mod_ph[1] += MOD_INC_R;
    int32_t tl = dl_t + opz_mul_q31(opz_sine(s->mod_ph[0]), mod);
    int32_t tr = (int32_t)opz_mul_u32_shr16((uint32_t)dl_t, D_RATIO) + opz_mul_q31(opz_sine(s->mod_ph[1]), mod);

    int32_t pd, pw;                                            /* the SDK's FX law (equal power) */
    opz_fx_drywet(params[OPZ_P_PAN], &pd, &pw);
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & OPZ_Q31_ONE), gain);
    int32_t dg = opz_mul_q31(pd, out_gain) >> 1;               /* half scale: the dry, for the output limiter */
    int32_t wg = opz_mul_q31(pw, out_gain);                    /* the wet sum is held at a quarter */

    int32_t *mem = s->mem;
    if (mem && s->cleared < MEM_WORDS) {
        volatile int32_t *z = mem + s->cleared;
        for (int i = 0; i < CLEAR_STEP; i++) z[i] = 0;
        s->cleared += CLEAR_STEP;
    }
    if (!mem || s->cleared < MEM_WORDS) {                      /* clearing (4 blocks) or no buffer: dry only */
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = opz_limit_half(opz_mul_q31(dg, in[i]));
        return;
    }
    if (!s->started) { s->d[0] = tl; s->d[1] = tr; s->started = 1; }

    /* ---- per sample -------------------------------------------------------------------------------------- */
    int32_t *sr = mem;
    uint32_t *dl = (uint32_t *)(mem + SR_N);
    uint32_t w = s->w, ph1 = s->ph[0], ph2 = s->ph[1];
    int32_t dlen = s->d[0], drlen = s->d[1], fb = s->fb;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t xl = in[2 * i], xr = in[2 * i + 1];
        sr[w & SR_MASK] = opz_add_q31((xl >> 1) + (xr >> 1), fb);
        ph1 -= (uint32_t)inc1;
        ph2 -= (uint32_t)inc2;
        int32_t v1 = voice(sr, w, ph1), v2 = voice(sr, w, ph2);
        dl[w & DL_MASK] = ((uint32_t)v1 >> 16) | ((uint32_t)v2 & 0xffff0000u);
        dlen = glide(dlen, tl);
        drlen = glide(drlen, tr);
        int32_t el = dl_read(dl, w, dlen, 0), er = dl_read(dl, w, drlen, 1);
        /* feedback: the pair's mean, band-limited and softly saturated, into the next sample's shifter input */
        int32_t f = (el >> 1) + (er >> 1);
        s->fb_hp = opz_add_q31(s->fb_hp, opz_mul_q31_q15(opz_sub_q31(f, s->fb_hp), K_FB_HP));
        s->fb_lp = opz_add_q31(s->fb_lp, opz_mul_q31_q15(opz_sub_q31(opz_sub_q31(f, s->fb_hp), s->fb_lp), K_FB_LP));
        fb = opz_mul_q31_q15(opz_soft(s->fb_lp), fbg);
        w++;
        /* voice 1 leans left, voice 2 right; the wet sum at a quarter scale, so nothing in it saturates (at full
         * scale the voices, the makeup and the space could add up to 2.4) */
        int32_t ml = opz_mul_q31_q15((v1 >> 1) + (v2 >> 3), gd), mr = opz_mul_q31_q15((v2 >> 1) + (v1 >> 3), gd);
        ml = (ml >> 1) + (ml >> 2);                            /* +3.5 dB makeup (x 1.5), / 2 */
        mr = (mr >> 1) + (mr >> 2);
        int32_t yl = ml + (opz_mul_q31_q15(el, gs) >> 2), yr = mr + (opz_mul_q31_q15(er, gs) >> 2);
        out[2 * i] = opz_shl_q31(opz_mul_q31(wg, yl), 1);    /* the wet at half scale */
        out[2 * i + 1] = opz_shl_q31(opz_mul_q31(wg, yr), 1);
    }
    opz_fx_finish(out, in, dg, dg);                            /* + the dry, through the SDK's limiter */
    s->w = w & DL_MASK;
    s->ph[0] = ph1;
    s->ph[1] = ph2;
    s->d[0] = dlen;
    s->d[1] = drlen;
    s->fb = fb;
}

OPZ_EXPORTS_FX();
