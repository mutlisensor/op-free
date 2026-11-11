/*
 * resonator — tuned string resonator fed by the FX track's input; notes on the FX track tune it and form chords.
 *
 *   P1        decay       ring time (T60) 40 ms .. 16 s, exponential: a short knock to a long ring
 *   P2        structure   one macro over damping and partial spread:
 *                           0 .. 127    dark, muted string -> bright harmonic string (the loop low-pass opens)
 *                           128 .. 255  harmonic string -> inharmonic, bell- and metal-like (dispersion in the loop
 *                                       stretches the partials), with a little extra damping to tame the clang
 *   pan       dry / wet   the SDK's FX law: 128 .. 255 the strings only (the OS's dry level keeps the source), below
 *                         128 the dry blends in at equal power, 0 = dry only; output through the SDK's limiter
 *   level     output level (x gain)
 *   pitch bend            bends every string, +-2 semitones at full scale
 *   notes on the FX track up to four strings, one per note. Notes held together form a chord; the chord stays
 *                         after the keys are released (latch) and is replaced by the next note-on after all keys
 *                         are up. Strings of the old chord are no longer fed and ring out at their own pitch.
 *                         Before any note: one string at C3. Notes fold into C1 .. C7 by octaves.
 *
 * Each string is a fractional delay line with a one-pole low-pass (damping), two first-order all-passes
 * (dispersion), a loop gain set from the decay time and a soft clip in the feedback path. The delay is corrected
 * for the phase delay of the loop filters at the fundamental, so the string stays in tune whatever P2 does. The
 * strings are fed the mono sum of the input and spread across the stereo field (centre, left, right, centre). A
 * look-ahead limiter and an input AGC keep a sustained input at the string's pitch from overloading. A retuned
 * string glides (S-curve, ~30 ms); a string started from silence just starts.
 *
 * The filter and resonance knobs are the OS's own track filter (also on FX tracks); resonator never reads them.
 *
 * Clean-room design in the spirit of Mutable Instruments Rings and Elements (Karplus-Strong strings with damping
 * and dispersion); no code from them. Q31 audio, Q16/Q30 control maths, no floating point, no libc. MIT licence,
 * see CONTRIBUTING.md.
 */
#include "opz_engine.h"
#include "opz_dsp.h"

#define SLOTS           4                       /* strings: one per note of a chord */
#define LINE_LEN        2048                    /* samples per string, int32: C1 (1349 samples) fits */
#define LINE_MASK       (LINE_LEN - 1)
#define LINE_STRIDE     (2 * LINE_LEN)          /* every sample is written twice, so reads never wrap */
#define Q31_ONE         0x7fffffff
#define Q30_ONE         (1 << 30)
#define G_CAP           (Q31_ONE - (1 << 15))   /* loop gain limit: 1 - 2^-16, so the loop is stable at DC */
#define KNEE            0x04000000              /* strings run at 1/32 scale: soft clip from 1/32 up to 1/16 */
#define IDLE_PEAK       0x400                   /* an unfed string below -96 dB (at the output) stops */
#define LIMIT           0x6c000000              /* wet peak limit, 0.84 (-1.5 dBFS) */
#define AGC_PEAK        0x40000000              /* above 0.5 (-6 dBFS) the input ducks, so resonances stop
                                                   growing before the limiter has to work */
#define FEED_STEP       0x20000000              /* a string's input fades in or out over 4 blocks */
#define CLEAR_STEP      2048                    /* words cleared per block: init runs with interrupts masked */
#define DEFAULT_NOTE    48                      /* C3 */
#define NOTE_LO         24                      /* C1 */
#define NOTE_HI         96                      /* C7 */

typedef struct Str {                            /* one string, kept in the host buffer (state is only 200 B) */
    int32_t d, d_mid, d_target;                 /* delay, samples in Q16; d glides to d_target through d_mid */
    int32_t g;                                  /* loop gain, Q31 */
    int32_t k, a, k_target, a_target;           /* low-pass and all-pass coefficients, Q31 (ramped over a block:
                                                   a step would kink the waveform) */
    int32_t lp, ay, by;                         /* low-pass and all-pass outputs, last sample */
} Str;

typedef struct Mem {
    Str str[SLOTS];
    int32_t ex[OPZ_BLOCK_FRAMES];               /* input to the strings (mono) */
    int32_t zero[OPZ_BLOCK_FRAMES];             /* input of strings that are ringing out */
    int32_t ramp[OPZ_BLOCK_FRAMES];             /* input of a string fading in or out */
    int32_t wet[2 * OPZ_BLOCK_FRAMES];          /* the wet signal, one block late (limiter look-ahead) */
    int32_t bus[3][OPZ_BLOCK_FRAMES];           /* strings summed by position: centre, left, right */
    int32_t line[SLOTS][LINE_STRIDE];
} Mem;

#define MEM_BYTES       ((uint32_t)sizeof(Mem)) /* ~67 KB: FX1 and FX2 (256 KB), not master (16 KB) */
#define HEAD_WORDS      ((uint32_t)(sizeof(Mem) - sizeof(((Mem *)0)->line)) / 4)
#define LINE_WORDS      (SLOTS * LINE_STRIDE)

typedef struct State {
    Mem *mem;
    uint32_t age[SLOTS], age_counter;           /* allocation order */
    int32_t agc;                                /* input gain, Q31 */
    int32_t exg;                                /* input gain applied (ramped per sample) */
    int32_t lim;                                /* limiter gain on the wet signal, Q31 */
    int32_t lim_need;                           /* gain the delayed block needs */
    int32_t feed[SLOTS];                        /* input gain per string, Q31, faded */
    int32_t og;                                 /* output gain of the strings, Q31 (ramped) */
    int32_t og_late;                            /* og at the start of the previous block */
    int32_t dc_x, dc_y;                         /* input DC blocker */
    uint16_t write;                             /* write index shared by all lines */
    uint16_t cleared;                           /* line words cleared so far */
    uint8_t note[SLOTS];
    uint8_t fed, held, idle, fresh, dirty;      /* bit k = slot k */
    uint8_t rr;                                 /* slot refreshed this block (round robin) */
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* ---- fixed-point helpers ------------------------------------------------------------------------- */

static inline int32_t p16(uint32_t param) { return (int32_t)(param >> 15); }      /* Q31 param -> 0..65535 */

/* Control maths (once per block per retuned string): 32-bit fractional multiplies and 32-bit divisions only;
 * 64-bit division is a slow libgcc call on the Blackfin. */


/* a * b for Q30 values (|result| < 2). */
static inline int32_t mul30(int32_t a, int32_t b) { return opz_mul_q31(a, b) * 2; }

/* num / den in Q30 for 0 <= num <= 2 den (den > 0), ~15 bits: both normalised, then a 32-bit division. */
static int32_t div30(uint32_t num, uint32_t den)
{
    if (num >= 2 * den) return 0x7fffffff;
    while (den < 0x40000000u) { den <<= 1; num <<= 1; }        /* num < 2 den stays below 2^32 */
    return (int32_t)((num / (den >> 15)) << 15);
}

/* e^x for |x| <= ln 2, Q30 in and out: Horner on the Taylor series to x^8. */
static int32_t exp30(int32_t x)
{
    static const int32_t c[9] = { 1073741824, 1073741824, 536870912, 178956971, 44739243,
                                  8947849, 1491308, 213044, 26631 };
    int32_t r = c[8];
    for (int k = 7; k >= 0; k--) r = c[k] + mul30(x, r);
    return r;
}

#define LN2_Q30         744261118

/* 2^-x for x >= 0 in Q27 (up to 16 octaves), Q31 result. */
static int32_t exp2_neg_q31(int32_t x)
{
    int32_t i = x >> 27;
    if (i > 31) return 0;
    int32_t r = exp30(-mul30((x & ((1 << 27) - 1)) << 3, LN2_Q30));        /* 2^-f, Q30 in (0.5, 1] */
    return (r >> i) >= 0x40000000 ? Q31_ONE : (r >> i) << 1;
}

/* atan(z) for 0 <= z <= 1, Q30 in, Q29 out (Abramowitz & Stegun 4.4.49, error < 1e-5 rad). */
static int32_t atan29(int32_t z)
{
    int32_t z2 = mul30(z, z);
    int32_t p = 22371518;
    p = -91410863 + mul30(p, z2);
    p = 193424926 + mul30(p, z2);
    p = -354656388 + mul30(p, z2);
    p = 1073597943 + mul30(p, z2);
    return mul30(p, z) >> 1;
}

#define PI_Q29          1686629713

/* atan2(y, x) for y >= 0 (Q30, |x| < 2), Q29 in [0, pi]. */
static int32_t atan2_29(int32_t y, int32_t x)
{
    uint32_t ax = x < 0 ? (uint32_t)-x : (uint32_t)x;
    if (y == 0) return x >= 0 ? 0 : PI_Q29;
    int32_t t = (uint32_t)y <= ax ? atan29(div30((uint32_t)y, ax)) : (PI_Q29 >> 1) - atan29(div30(ax, (uint32_t)y));
    return x < 0 ? PI_Q29 - t : t;
}

/* Phase delay (samples, Q16) of a phase lag of atan2(y, x) at w = 2 pi / P: atan2 x P / 2 pi. */
static int32_t phase_delay(int32_t y, int32_t x, int32_t P)
{
    return (int32_t)(((((int64_t)atan2_29(y, x) * P) >> 29) * 683565276) >> 32);
}

/* Soft clip for |x| > K: y = K + 2K (t - t^2/2) with t = (|x| - K) / 2K, so slope 1 at the knee and 0 at 3K. */
static int32_t soft(int32_t x)
{
    int32_t m = x ^ (x >> 31);                                  /* |x| (ones' complement: no overflow) */
    int32_t y;
    if (m >= 3 * KNEE) {
        y = 2 * KNEE;
    } else {
        int32_t t = (m - KNEE) << 4;                            /* / 2K */
        y = KNEE + (t >> 4) - (opz_mul_q31(t, t) >> 5);
    }
    return x < 0 ? -y : y;
}

/* ---- strings ------------------------------------------------------------------------------------- */

/* Recompute slot k's string from its note, the bend and the knobs: when it gets a note, and once per block for
 * one slot in turn (round robin) to follow the knobs and the bend. */
static void update_slot(State *s, int k, int32_t bend_q16, int32_t decay_q31, int32_t k_c3, int32_t amt)
{
    Str *st = &s->mem->str[k];
    int32_t n_q16 = ((int32_t)s->note[k] << 16) + bend_q16;
    int32_t oct = (n_q16 - (69 << 16)) / 12;                   /* octaves above A4, Q16 */
    int32_t oi = oct >> 16, of = (oct & 0xffff) << 14;         /* floor and fraction (Q30) */
    int32_t up = exp30(mul30(of, LN2_Q30)), down = exp30(-mul30(of, LN2_Q30));   /* 2^f, 2^-f (Q30) */
    int32_t P = (int32_t)(((int64_t)6568495 * down) >> (30 + oi));                 /* period, samples Q16 */
    int32_t w = oi >= 0 ? mul30(67312206, up) << oi : mul30(67312206, up) >> -oi;  /* 2 pi / P, Q30 */
    int32_t w2 = mul30(w, w);
    int32_t sn = mul30(w, Q30_ONE - mul30(w2, 178956971 - mul30(w2, 8947849 - mul30(w2, 213044))));
    int32_t h = mul30(w2, (Q30_ONE >> 1) - mul30(w2, 44739243 - mul30(w2, 1491308 - mul30(w2, 26631))));
                                                                /* h = 1 - cos w, kept for precision */

    /* damping: one-pole low-pass y += k (x - y), cutoff ~ k / sqrt(1 - k). k follows the pitch (x2 per octave),
     * so the cutoff sits at the same harmonic for every note and the loss at the fundamental per pass stays small */
    int32_t above = (96 << 16) - n_q16;
    if (above < 0) above = 0;
    int32_t k15 = (int32_t)(((int64_t)k_c3 * exp2_neg_q31((above / 12) << 11)) >> 43);   /* x 16, Q15 */
    if (k15 > 32767) k15 = 32767;
    if (k15 < 1) k15 = 1;
    int32_t k30 = k15 << 15, p30 = Q30_ONE - k30;
    int32_t tau_lp = phase_delay(mul30(p30, sn), k30 + mul30(p30, h), P);

    /* loop gain for a T60 of 0.04 * 2^(8.64 p1) s at the fundamental: g = 2^-(9.966 P / (44100 T60)), divided by
     * the low-pass gain there, 1 / |H| = sqrt(1 + x), x = 2 p (1 - cos w) / k^2 (series, never above the true
     * value), so P1 sets the fundamental's decay whatever P2 does. The low-pass passes DC at unity, so the result
     * is capped below 1 (G_CAP): a dark, high note can't ring longer than its damping allows */
    int32_t e = (int32_t)(((((int64_t)758285 * P) >> 16) * decay_q31) >> 31);  /* octaves per pass, Q27 */
    if (e < 1) e = 1;
    int32_t x = div30((uint32_t)mul30(p30, h) * 2, (uint32_t)mul30(k30, k30) + 1);  /* <= 2 */
    int32_t comp = Q30_ONE + (x >> 1) - (mul30(x, x) >> 3);
    int64_t g = ((int64_t)exp2_neg_q31(e) * comp) >> 30;
    if (g > G_CAP) g = G_CAP;

    /* dispersion: two all-passes (a + z^-1) / (1 + a z^-1) with a DC delay of d samples each, a = (1-d)/(1+d);
     * d goes up to 80 samples or a quarter of the period (Q8 here, so the division fits 32 bits) */
    int32_t dmax = P >> 10;                                     /* P / 4 in Q8 */
    if (dmax > (80 << 8)) dmax = 80 << 8;
    if (dmax < (1 << 8)) dmax = 1 << 8;
    int32_t d = (1 << 8) + (int32_t)(((int64_t)(dmax - (1 << 8)) * amt) >> 31);
    int32_t a15 = (((1 << 8) - d) << 15) / ((1 << 8) + d);     /* (-1, 0] */
    int32_t a30 = a15 << 15, a2 = mul30(a30, a30);
    int32_t b = Q30_ONE + a30;                                  /* 2a + (1 + a^2) cos w = (1 + a)^2 - (1 + a^2) h */
    int32_t tau_ap = phase_delay(mul30(Q30_ONE - a2, sn), mul30(b, b) - mul30(Q30_ONE + a2, h), P);

    /* loop delay = line + low-pass + 2 all-passes at the fundamental (computed with the Q15 coefficients used) */
    int32_t dt = P - tau_lp - 2 * tau_ap;
    if (dt < (2 << 16)) dt = 2 << 16;
    if (dt > ((LINE_LEN - 4) << 16)) dt = (LINE_LEN - 4) << 16;
    st->d_target = dt;
    st->g = (int32_t)g;
    st->k_target = k15 << 16;
    st->a_target = a15 << 16;
    if (s->fresh & (1 << k)) {                                  /* from silence: start in tune, clean filters */
        st->d = st->d_mid = dt;
        st->k = st->k_target;
        st->a = st->a_target;
        st->lp = st->ay = st->by = 0;
        s->fresh &= (uint8_t)~(1 << k);
    }
}

/* One string for one block: read the delay, damp, disperse, feed back, add the output into bus. At 1/32 scale
 * nothing here can overflow, so plain adds. */
static void run_string(Str *st, int32_t *line, const int32_t *ex, int32_t *bus, unsigned w)
{
    int32_t d = st->d;                                          /* glide: two one-poles of 4 blocks (S-curve, ~95 %
                                                                   in 30 ms), so the speed of the slide ramps in */
    st->d_mid += (st->d_target - st->d_mid) >> 2;
    int32_t d_inc = ((st->d_mid - d) >> 2) >> 6;
    int32_t g = st->g, kk = st->k, a = st->a;
    int32_t k_inc = (st->k_target - kk) >> 6, a_inc = (st->a_target - a) >> 6;
    int32_t lp = st->lp, ay = st->ay, by = st->by;
    int32_t *wp = line + w;                                     /* write pointer; its twin is LINE_LEN further */
    const int32_t *rp = line + LINE_LEN + w - 2;                /* read base: x[n - di] is rp[-di + 2] */

    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        d += d_inc;
        /* x[n - d]: 4-point Hermite (Catmull-Rom) between delays di and di + 1 */
        const int32_t *p = rp + i - (d >> 16);
        int32_t fr = (d & 0xffff) << 15;
        int32_t y2 = p[0], y1 = p[1], y0 = p[2], ym1 = p[3];
        int32_t c1 = (y1 - ym1) >> 1;
        int32_t c2 = ym1 - 2 * y0 - (y0 >> 1) + 2 * y1 - (y2 >> 1);
        int32_t c3 = ((y2 - ym1) >> 1) + (y0 - y1) + ((y0 - y1) >> 1);
        int32_t v = opz_mul_q31(opz_mul_q31(opz_mul_q31(c3, fr) + c2, fr) + c1, fr) + y0;
        kk += k_inc;
        a += a_inc;
        int32_t l = lp + opz_mul_q31(v - lp, kk);               /* low-pass */
        int32_t y = lp + opz_mul_q31(l - ay, a);                /* all-pass (a + z^-1) / (1 + a z^-1) */
        int32_t yb = ay + opz_mul_q31(y - by, a);               /* and another */
        lp = l;
        ay = y;
        by = yb;
        int32_t fb = opz_mul_q31(g, yb);
        int32_t u = fb + ex[i];
        if ((uint32_t)u + KNEE > 2u * KNEE) u = soft(u);
        wp[i] = u;
        wp[i + LINE_LEN] = u;
        bus[i] += fb;
    }
    st->d = d;
    st->k = kk;
    st->a = a;
    st->lp = lp; st->ay = ay; st->by = by;
}

/* Peak of a string's last period, from 8 points of its line (for idle detection). */
static int32_t string_peak(const Str *st, const int32_t *line, unsigned w)
{
    unsigned step = (unsigned)(st->d >> 19) + 1;
    int32_t peak = 0;
    for (int j = 0; j < 8; j++) {
        int32_t v = line[(w - 1 - j * step) & LINE_MASK];
        v ^= v >> 31;
        if (v > peak) peak = v;
    }
    return peak;
}

/* ---- exports ------------------------------------------------------------------------------------- */

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = MEM_BYTES;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)scratch;
    State *s = state;
    volatile uint8_t *p = (volatile uint8_t *)s;               /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    s->mem = (Mem *)mem;
    if (mem) {                                                  /* strings and input buffers (~1 KB); the lines are
                                                                   cleared progressively in renderFX */
        volatile int32_t *h = (volatile int32_t *)mem;
        for (unsigned i = 0; i < HEAD_WORDS; i++) h[i] = 0;
    }
    s->agc = s->lim = s->lim_need = Q31_ONE;
    s->exg = Q31_ONE >> 4;
    s->note[0] = s->note[1] = s->note[2] = s->note[3] = DEFAULT_NOTE;
    s->fed = 1;                                                 /* one string at C3 until a note arrives */
    s->idle = 0x0e;
    s->fresh = s->dirty = 0x0f;
}

static int fold(int note)
{
    while (note < NOTE_LO) note += 12;
    while (note > NOTE_HI) note -= 12;
    return note;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;                                               /* may be NULL (legato re-trigger) */
    State *s = state;
    if (note < 0 || note > 127) return;                         /* 255: plug selected, no note */
    uint8_t n = (uint8_t)fold(note);

    /* Velocity 0 is the OS handing back a still-held note after another was released on a mono-style track
     * (FxManager::noteOff); treat it as "this note alone". */
    if (velocity == 0) s->held = 0;
    if (!s->held) s->fed = 0;                                   /* first key of a new chord: the old one rings out */

    int k = -1;
    for (int i = 0; i < SLOTS; i++)                             /* same pitch already on a string: reuse it */
        if (!(s->idle & (1 << i)) && s->note[i] == n) { k = i; break; }
    if (k < 0)
        for (int i = 0; i < SLOTS; i++)                         /* a silent string starts clean */
            if (s->idle & (1 << i)) { k = i; break; }
    if (k < 0) {                                                /* else the oldest ringing-out string, else the
                                                                   oldest of the chord: it glides to the note */
        uint32_t best = 0;
        for (int pass = 0; pass < 2 && k < 0; pass++)
            for (int i = 0; i < SLOTS; i++) {
                if (((s->fed >> i) & 1) != pass) continue;
                uint32_t old = s->age_counter - s->age[i];
                if (k < 0 || old > best) { k = i; best = old; }
            }
    }
    uint8_t bit = (uint8_t)(1 << k);
    if (s->idle & bit) s->fresh |= bit;
    s->idle &= (uint8_t)~bit;
    s->note[k] = n;
    s->fed |= bit;
    s->held |= bit;
    s->dirty |= bit;
    s->age[k] = ++s->age_counter;
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note < 0 || note > 127) return;
    uint8_t n = (uint8_t)fold(note);
    for (int i = 0; i < SLOTS; i++)
        if ((s->held & (1 << i)) && s->note[i] == n) {          /* the string stays in the chord (latch) */
            s->held &= (uint8_t)~(1 << i);
            break;
        }
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    State *s = state;
    Mem *m = s->mem;

    /* ---- per block ----------------------------------------------------------------------------------- */
    int32_t dry, wet;                                           /* the SDK's FX law (equal power) */
    opz_fx_drywet(params[OPZ_P_PAN], &dry, &wet);
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & Q31_ONE), gain);

    if (!m || s->cleared < LINE_WORDS) {                        /* no buffer, or still clearing it: dry only */
        if (m) {
            volatile int32_t *z = &m->line[0][0] + s->cleared;
            int n = LINE_WORDS - s->cleared < CLEAR_STEP ? LINE_WORDS - s->cleared : CLEAR_STEP;
            for (int i = 0; i < n; i++) z[i] = 0;
            s->cleared += n;
        }
        for (int j = 0; j < 2 * OPZ_BLOCK_FRAMES; j++) out[j] = opz_mul_q31(opz_mul_q31(dry, in[j]), out_gain);
        return;
    }

    int32_t bend_q16 = pitchBend >> 14;                         /* full scale = 2 semitones */
    int32_t decay_q31 = exp2_neg_q31(p16(params[OPZ_P_P1]) * 17703);          /* 2^-(8.64 p1) */
    int32_t u = p16(params[OPZ_P_P2]), k_c3, amt, makeup = 0x40000000;   /* make-up / 2 */
    if (u < 32768) {                                            /* dark -> bright: low-pass k 0.12 -> 0.85 at C3 */
        k_c3 = opz_mul_q31(1825361101, exp2_neg_q31((32768 - u) * 11567));
        amt = 0;
        int32_t t = (32768 - u) << 16;                          /* dark loses energy: up to +6 dB, (1 + t^2) / 2 */
        makeup += opz_mul_q31(t, t) >> 1;
    } else {                                                    /* harmonic -> inharmonic, k 0.85 -> 0.55 */
        amt = (u - 32768) << 16;
        k_c3 = 1825361101 - opz_mul_q31(644245094, amt);
    }
    s->rr = (uint8_t)((s->rr + 1) & (SLOTS - 1));
    uint8_t todo = (uint8_t)((s->dirty | (1 << s->rr)) & ~s->idle);
    for (int k = 0; k < SLOTS; k++)
        if (todo & (1 << k)) update_slot(s, k, bend_q16, decay_q31, k_c3, amt);
    s->dirty = 0;

    /* input: mono sum, DC blocker, then into the strings at 1/32 times the AGC (ramped: a step would come back
     * out of the lines as a click) */
    int32_t exg = s->exg, exg_inc = ((s->agc >> 4) - exg) >> 6;
    int32_t x1 = s->dc_x, y1 = s->dc_y;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t x = (in[2 * i] >> 2) + (in[2 * i + 1] >> 2);
        y1 = opz_add_q31(x - x1, opz_mul_q31(2136746230, y1));                    /* pole at 0.995 (~35 Hz) */
        x1 = x;
        exg += exg_inc;
        m->ex[i] = opz_mul_q31(y1, exg);
    }
    s->dc_x = x1;
    s->dc_y = y1;
    s->exg = exg;

    /* strings, summed into three buses; the chord's gain 1 / (2 sqrt(size)) and the make-up go in the mix.
     * Slots 0 and 3 sit in the centre, 1 to the left, 2 to the right (the far side at -6 dB) */
    static const int32_t chord_gain[SLOTS + 1] = { 0x40000000, 0x40000000, 0x2d413ccd, 0x24f34e8b, 0x20000000 };
    static const uint8_t bus_of[SLOTS] = { 0, 1, 2, 0 };
    int nfed = (s->fed & 1) + ((s->fed >> 1) & 1) + ((s->fed >> 2) & 1) + ((s->fed >> 3) & 1);
    int32_t og_inc = ((opz_mul_q31(chord_gain[nfed], makeup) - s->og) >> 2) >> 6;   /* ~6 ms */
    volatile int32_t *zb = &m->bus[0][0];
    for (int j = 0; j < 3 * OPZ_BLOCK_FRAMES; j++) zb[j] = 0;
    for (int k = 0; k < SLOTS; k++) {
        uint8_t bit = (uint8_t)(1 << k);
        if (s->idle & bit) continue;
        int32_t f0 = s->feed[k], f1 = (s->fed & bit) ? Q31_ONE : 0;    /* fade over 4 blocks (6 ms) */
        if (f1 - f0 > FEED_STEP) f1 = f0 + FEED_STEP;            /* both in 0..1: no overflow */
        if (f0 - f1 > FEED_STEP) f1 = f0 - FEED_STEP;
        const int32_t *x = f0 ? m->ex : m->zero;
        if (f0 != f1) {                                         /* fade the input in or out: a cut input would come
                                                                   back out of the line as a click */
            int32_t f = f0, f_inc = (f1 - f0) >> 6;
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                f += f_inc;
                m->ramp[i] = opz_mul_q31(m->ex[i], f);
            }
            x = m->ramp;
        }
        s->feed[k] = f1;
        run_string(&m->str[k], m->line[k], x, m->bus[bus_of[k]], s->write);
        if (!f1 && string_peak(&m->str[k], m->line[k], (s->write + OPZ_BLOCK_FRAMES) & LINE_MASK) < IDLE_PEAK)
            s->idle |= bit;                                     /* rung out: stop computing it */
    }
    s->write = (uint16_t)((s->write + OPZ_BLOCK_FRAMES) & LINE_MASK);

    /* wet = strings x chord gain x make-up x 128 (makes up the strings' 1/32 scale, the chord gain's 1/2 and the
     * make-up's /2). A look-ahead limiter keeps the wet peak at LIMIT: the wet signal plays one block late, so
     * the gain can ramp down before a loud block arrives (no steps) and recovers over ~100 ms. Above AGC_PEAK the
     * input ducks so the strings stop building up; it comes back over ~200 ms */
    int32_t og0 = s->og, og1 = og0 + og_inc * OPZ_BLOCK_FRAMES;
    int32_t peak = 0;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {                /* pan: the far side of a string at -6 dB */
        int32_t c = m->bus[0][i], l = m->bus[1][i], r = m->bus[2][i];
        int32_t L = c + l + (r >> 1), R = c + r + (l >> 1);
        out[2 * i] = L;
        out[2 * i + 1] = R;
        L ^= L >> 31;
        R ^= R >> 31;
        if (L > peak) peak = L;
        if (R > peak) peak = R;
    }
    peak = opz_mul_q31(peak, og1 > og0 ? og1 : og0);
    int32_t need = Q31_ONE;
    if (peak > (LIMIT >> 7))
        need = div30(LIMIT >> 7, (uint32_t)peak) * 2;           /* LIMIT / (128 peak) */
    if (peak > (AGC_PEAK >> 7)) s->agc -= s->agc >> 4;
    else s->agc += (Q31_ONE - s->agc) >> 7;
    int32_t lg0 = s->lim, lg1 = lg0 + ((Q31_ONE - lg0) >> 6);
    if (lg1 > s->lim_need) lg1 = s->lim_need;                   /* the block played now */
    if (lg1 > need) lg1 = need;                                 /* and the next one starts at or below its need */
    s->og = og1;
    s->lim = lg1;
    s->lim_need = need;

    /* out = dry x input + wet x (delayed strings), with every wet gain in one ramp */
    int32_t wg = opz_mul_q31(wet, out_gain), dg = opz_mul_q31(dry, out_gain) >> 1;   /* dry at half scale */
    int32_t G = opz_mul_q31(opz_mul_q31(s->og_late, lg0), wg);  /* the chord gain the delayed block was made with */
    int32_t G1 = opz_mul_q31(opz_mul_q31(og0, lg1), wg);
    s->og_late = og0;
    int32_t G_inc = (G1 - G) >> 6;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        G += G_inc;
        for (int c = 0; c < 2; c++) {
            int j = 2 * i + c;
            int32_t w = opz_shl_q31(opz_mul_q31(m->wet[j], G), 6);  /* the strings at half scale */
            m->wet[j] = out[j];
            out[j] = w;
        }
    }
    opz_fx_finish(out, in, dg, dg);                            /* + the dry, through the SDK's limiter */
}

OPZ_EXPORTS_FX();
