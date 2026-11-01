/*
 * glue — stereo bus compressor and soft clipper for FX1/FX2, ducked by notes on its track.
 *
 *   P1        amount      one macro over threshold, ratio and make-up: 0 = no compression (only the safety clipper);
 *                         up to 255 the threshold falls from 0 to -30 dBFS while the ratio rises from 1:1 to 5:1.
 *                         Auto make-up keeps a signal peaking at -12 dBFS at the same level, so turning it up makes
 *                         the mix denser rather than louder or quieter.
 *   P2        character   0 .. 127    slow, clean glue (20 ms attack, 150 ms .. 0.7 s program-dependent release,
 *                                     12 dB soft knee) -> fast, pumping (1 ms attack, 50 .. 120 ms release, 4 dB knee)
 *                         128 .. 255  fast, plus drive into the clipper, 0 -> +12 dB (the output is turned down by
 *                                     half the drive): from glue to saturation
 *                         continuous, so the OS LFO on P2 glides through it.
 *   pan       dry / wet   0 dry .. 128 fully processed; 128 .. 255 stay fully processed (as filter). Below the centre
 *                         it is a parallel ("New York") compressor
 *   level     output level (x gain)
 *   notes     on its track: each note ducks the signal, sidechain-style. The gain drops within ~2 ms by up to 24 dB
 *             (velocity / 127 x 24 dB), stays down while any note is held, and comes back at the release speed
 *             (150 ms at P2 0, 50 ms from P2 128). With no notes it is a plain bus compressor.
 *
 * Signal path per sample: stereo-linked peak detector -> gain computer in the log domain (soft knee) -> attack /
 * two-stage release smoothing -> gain (with make-up, drive and duck) -> soft clipper that never exceeds -0.2 dBFS ->
 * dry/wet and level. The detector and gain computer run every 16 samples on the peak of the next 16 input frames (we
 * have the whole block, so the gain sees the peak it will be applied to); the gain ramps per sample, so nothing
 * steps. Program-dependent release: a fast stage follows the gain computer, a slow stage charges from the fast one
 * over ~0.4 s and releases slowly; the deeper of the two is used, so short peaks recover quickly and sustained
 * compression recovers slowly (the classic "auto release" of bus compressors).
 *
 * No buffer (memRequest 0). Integer only, no division, no libc, no int64 in
 * per-sample code. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_engine.h"
#include "opz_dsp.h"

#define CTRL            16                      /* control rate: detector and gain computer every 16 frames */
#define LOG_ONE         65536                   /* log2 units (6.02 dB) in Q16 */
#define HR              5                       /* the gain stage runs in Q26: 30 dB of headroom for make-up/drive */

/* P1 macro */
#define T_SPAN          (5 * LOG_ONE)           /* threshold 0 .. -30 dBFS */
#define REF_LEVEL       (-2 * LOG_ONE)          /* auto make-up: a -12 dBFS peak keeps its level */

/* P2 macro: log2 of the time constants in samples, P2 0 -> P2 128 */
#define L_ATT_SLOW      641246                  /* 20 ms   (log2(882) in Q16) */
#define L_ATT_FAST      358004                  /* 1 ms    (log2(44.1)) */
#define L_RELF_SLOW     831752                  /* 150 ms  (log2(6615)) */
#define L_RELF_FAST     727880                  /* 50 ms   (log2(2205)) */
#define L_RELS_SLOW     977399                  /* 700 ms  (log2(30870)) */
#define L_RELS_FAST     810654                  /* 120 ms  (log2(5292)) */
#define L_CHG_SLOW      924488                  /* 400 ms: how fast the slow release stage charges */
#define L_CHG_FAST      831752                  /* 150 ms */
#define KNEE_WIDE       (2 * LOG_ONE)           /* 12 dB */
#define KNEE_NARROW     43541                   /* 4 dB */
#define DRIVE_MAX       (2 * LOG_ONE)           /* +12 dB at P2 255 */

/* Soft clipper, real values in Q26: linear up to the knee, then a parabola that meets the ceiling with zero slope */
#define CEIL            65766687                /* 0.98 (-0.18 dBFS) */
#define KNEE_CLEAN      57042534                /* 0.85 (-1.4 dBFS) at P2 0 */
#define KNEE_PUMP       46976205                /* 0.70 (-3.1 dBFS) at P2 128 */
#define KNEE_SAT        20132659                /* 0.30 (-10.5 dBFS) at P2 255 */

#define DUCK_MAX        (4 << 24)               /* 24 dB at velocity 127, Q24 */
#define C_DUCK          356274552               /* duck attack, 2 ms: 1 - e^(-16 / 88.2) in Q31 */

/* Everything that follows from P1 and P2, recomputed only when one of them moves */
typedef struct Macro {
    uint32_t w1, w2;                            /* the parameter words these values were computed for */
    int32_t thr, slope, knee, kw;               /* static curve: log2 Q16, Q31, log2 Q16, Q31 */
    int32_t makeup, drive;                      /* log2 Q16 */
    int32_t c_att, c_relf, c_rels, c_chg;       /* one-pole coefficients per CTRL frames, Q31 */
    int32_t ck, h2, q;                          /* clipper: knee and 2h (Q26), 1 / (4h) (Q14) */
    int32_t comp;                               /* output compensation for the drive, Q31 */
} Macro;

typedef struct State {
    Macro m;
    int32_t fast, slow;                         /* gain reduction stages, log2 Q24 (>= 0) */
    int32_t duck, duck_depth;                   /* duck, log2 Q24 */
    int32_t gain;                               /* current linear gain, Q26 */
    int32_t wet_og, dry_og;                     /* output gains reached at the end of the last block, Q31 */
    uint32_t held[4];                           /* notes held on the track (bit per MIDI note) */
    uint8_t started;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* log2(x / 2^31) in Q16 for a Q31 level x > 0 (-31 .. 0); quadratic on the mantissa, error < 0.01 octave. */
static inline int32_t log2_q16(int32_t x)
{
    if (x <= 0) return -(31 << 16);
    int e = opz_norm32(x);                                     /* x << e in [2^30, 2^31) */
    int32_t f = ((x << e) - 0x40000000) >> 14;                 /* mantissa - 1, Q16 */
    int32_t l = f + (opz_mul_q31(f * (65536 - f), 736801640) >> 16);   /* log2(1 + f) ~ f + 0.3431 f (1 - f) */
    return l - ((e + 1) << 16);
}

/* One-pole coefficient per CTRL frames for a time constant of 2^(L / 65536) samples (L >= 5 octaves):
 * 1 - e^-x with x = CTRL / tau, from its cubic series (x <= 0.37 here, error < 0.3 %). Q31. Per knob move. */
static int32_t coef_from_log(int32_t l)
{
    int32_t x = opz_exp2_q16((4 << 16) - l + (15 << 16));     /* 16 / tau in Q31 */
    int32_t x2 = opz_mul_q31(x, x);
    int32_t x3 = opz_mul_q31(x2, x);
    return x - (x2 >> 1) + opz_mul_q31(x3, 0x15555555);
}

/* a + (b - a) t for t in 0 .. 65535 (Q16), |b - a| < 2^31 */
static inline int32_t lerp_i(int32_t a, int32_t b, int32_t t16)
{
    return a + opz_mul_q31(b - a, t16 << 15);
}

/* 2^k / v for v > 0, no division: opz_recip_q31 on the normalised v (2^61 / m within 2 LSB). The caller makes sure
 * the result fits 31 bits. Per block. */
static int32_t recip_pow2(int32_t v, int k)
{
    int n = opz_norm32(v);
    int32_t r = opz_recip_q31(v << n);                         /* 2^61 / (v 2^n) */
    int sh = n + k - 61;
    return sh >= 0 ? r << sh : r >> -sh;
}

/* Static curve: gain reduction in log2 Q16 for a level x (log2 Q16), threshold t, slope s = 1 - 1/ratio (Q31),
 * knee width w, kw = s / (2 w) in Q31 (quadratic knee across t +- w/2). */
static inline int32_t curve(int32_t x, int32_t t, int32_t s, int32_t w, int32_t kw)
{
    int32_t over = x - t;
    if (over <= -(w >> 1)) return 0;
    if (over >= (w >> 1)) return opz_mul_q31(over, s);
    int32_t d = (over + (w >> 1)) << 13;                       /* Q29, d <= w <= 2 */
    return opz_mul_q31(opz_mul_q31(d, kw), d) >> 11;           /* s d^2 / (2 w): Q27 -> Q16 */
}

/* Soft clip in Q26: |v| <= k passes unchanged, then k + s - q s^2 (s = |v| - k, q = 1 / (4h)) up to the ceiling
 * k + h at s = 2h, flat beyond; continuous slope. Branchless: s is clamped to 0 .. 2h, so below the knee the correction
 * is 0 and y = |v| exactly. The correction uses two 16 x 16 multiplies (s to 2^-15, q in Q14): within 2e-4 of the exact
 * curve, and only above the knee. */
static inline int32_t clip(int32_t v, int32_t k, int32_t h2, int32_t q14)
{
    int32_t a = opz_abs32(v);
    int32_t s = a - k;
    s = s > 0 ? s : 0;
    int32_t sc = s < h2 ? s : h2;
    uint32_t s16 = (uint32_t)sc >> 11;                         /* Q15, below 2^16 (2h < 1.4) */
    uint32_t ss = opz_mul_u16(s16, s16) >> 16;                 /* s^2 in Q14 */
    int32_t y = a - s + sc - (int32_t)(opz_mul_u16(ss, q14) >> 2);         /* Q28 -> Q26 */
    int32_t m = v >> 31;
    return (y ^ m) - m;
}

/* P1 and P2 -> curve, times, clipper. Called when a knob (or the LFO on it) moves. */
static void __attribute__((noinline)) macros(Macro *m, uint32_t w1, uint32_t w2)
{
    m->w1 = w1; m->w2 = w2;
    int32_t u1 = opz_p16(w1), u2 = opz_p16(w2);                /* 0 .. 65535 */
    int32_t uq = u1 << 15;                                     /* Q31 */
    m->thr = -opz_mul_q31(T_SPAN, uq);
    m->slope = opz_mul_q31(uq, 0x66666666 - opz_mul_q31(0x33333333, uq)) << 1;   /* u (1.6 - 0.8 u), Q31 */
    int32_t t2 = u2 < 32768 ? u2 * 2 : 65535;                  /* 0 .. 1 over P2 0 .. 127 */
    int32_t t3 = u2 < 32768 ? 0 : (u2 - 32768) * 2;            /* 0 .. 1 over P2 128 .. 255 */
    m->knee = lerp_i(KNEE_WIDE, KNEE_NARROW, t2);
    m->kw = opz_mul_q31(m->slope, recip_pow2(m->knee, 46));    /* s / (2 w) in Q31: s 2^15 / knee */
    m->makeup = curve(REF_LEVEL, m->thr, m->slope, m->knee, m->kw);
    m->drive = opz_mul_q31(DRIVE_MAX, t3 << 15);

    m->c_att = coef_from_log(lerp_i(L_ATT_SLOW, L_ATT_FAST, t2));
    m->c_relf = coef_from_log(lerp_i(L_RELF_SLOW, L_RELF_FAST, t2));
    m->c_rels = coef_from_log(lerp_i(L_RELS_SLOW, L_RELS_FAST, t2));
    m->c_chg = coef_from_log(lerp_i(L_CHG_SLOW, L_CHG_FAST, t2));

    m->ck = t3 ? lerp_i(KNEE_PUMP, KNEE_SAT, t3) : lerp_i(KNEE_CLEAN, KNEE_PUMP, t2);
    int32_t h = CEIL - m->ck;                                  /* Q26, 0.13 .. 0.68 */
    m->q = recip_pow2(h, 38);                                  /* 1 / (4h) in Q14 = 2^38 / h, below 2 (h > 0.125) */
    m->h2 = 2 * h;
    m->comp = opz_exp2_q16(-(m->drive >> 1) + (15 << 16));     /* 2^(-drive/2), Q31 */
    if (m->comp < 0) m->comp = OPZ_Q31_ONE;                    /* 2^15 in Q16 wraps: drive 0 */
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
    volatile uint8_t *p = (volatile uint8_t *)state;           /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

/* params may be NULL (a held note re-sent with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                        /* 255: plug selected, no note */
    int was_held = (s->held[0] | s->held[1] | s->held[2] | s->held[3]) != 0;
    s->held[note >> 5] |= 1u << (note & 31);
    if (velocity == 0) return;                                 /* re-sent held note: keep the depth */
    int32_t depth = velocity * (DUCK_MAX / 127);
    if (!was_held || depth > s->duck_depth) s->duck_depth = depth;   /* a chord ducks as deep as its loudest note */
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note < 0 || note > 127) return;
    s->held[note >> 5] &= ~(1u << (note & 31));
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    Macro *m = &s->m;
    if (!s->started || params[OPZ_P_P1] != m->w1 || params[OPZ_P_P2] != m->w2)
        macros(m, params[OPZ_P_P1], params[OPZ_P_P2]);
    int32_t thr = m->thr, slope = m->slope, knee = m->knee, kw = m->kw, makeup = m->makeup, drive = m->drive;
    int32_t c_att = m->c_att, c_relf = m->c_relf, c_rels = m->c_rels, c_chg = m->c_chg;
    int32_t ck = m->ck, h2 = m->h2, q = m->q;

    /* dry / wet from pan (0 dry .. 128 wet, then wet) and the output level; the drive is half compensated */
    int32_t pan = opz_pq31(params[OPZ_P_PAN]);
    int32_t wet = pan >= 0x40000000 ? OPZ_Q31_ONE : pan * 2;
    int32_t og = opz_mul_q31(opz_pq31(params[OPZ_P_LEVEL]), gain);
    int32_t wet_og = opz_mul_q31(opz_mul_q31(wet, og), m->comp);
    int32_t dry_og = opz_mul_q31(OPZ_Q31_ONE - wet, og);
    int32_t gain0 = opz_exp2_q16(makeup + drive + (10 << 16));                /* Q26 */

    if (!s->started) {
        s->wet_og = wet_og; s->dry_og = dry_og; s->gain = gain0;
        s->started = 1;
    }
    int32_t w_og = s->wet_og, d_og = s->dry_og;
    int32_t dw = (wet_og - w_og) >> 6, dd = (dry_og - d_og) >> 6;
    int32_t g = s->gain;
    int held = (s->held[0] | s->held[1] | s->held[2] | s->held[3]) != 0;
    int dry = (dry_og | d_og) != 0;

    for (int sub = 0; sub < OPZ_BLOCK_FRAMES; sub += CTRL) {
        const fract32 *x = in + 2 * sub;
        fract32 *y = out + 2 * sub;

        /* ---- control: stereo-linked peak of the next 8 frames -> gain reduction -> target gain ------------- */
        int32_t pk = 0;
        for (int i = 0; i < 2 * CTRL; i++) {
            int32_t a = opz_abs32(x[i]);
            if (a > pk) pk = a;
        }
        int32_t gr = curve(log2_q16(pk), thr, slope, knee, kw) << 8;          /* Q24 */
        s->fast += opz_mul_q31(gr > s->fast ? c_att : c_relf, gr - s->fast);
        s->slow += opz_mul_q31(s->fast > s->slow ? c_chg : c_rels, s->fast - s->slow);
        int32_t red = s->fast > s->slow ? s->fast : s->slow;
        s->duck += opz_mul_q31(held ? C_DUCK : c_relf, (held ? s->duck_depth : 0) - s->duck);
        int32_t lg = makeup + drive - ((red + s->duck) >> 8);                 /* log2 Q16 */
        if (lg < -(24 << 16)) lg = -(24 << 16);
        int32_t g_new = opz_exp2_q16(lg + (10 << 16));                        /* Q26 */
        int32_t dg = (g_new - g) >> 4;                                        /* / CTRL */

        /* ---- audio: the output gains need only 16 bits (3-instruction multiply); the dry path only below the
         * centre of pan ------------------------------------------------------------------------------------ */
        if (dry) {
            for (int i = 0; i < CTRL; i++) {
                g += dg;
                w_og += dw; d_og += dd;
                int32_t w15 = w_og >> 16, d15 = d_og >> 16;
                int32_t xl = x[2 * i], xr = x[2 * i + 1];
                int32_t vl = clip(opz_mul_q31(xl, g), ck, h2, q);             /* Q26 */
                int32_t vr = clip(opz_mul_q31(xr, g), ck, h2, q);
                y[2 * i] = opz_add_q31(opz_mul_q31_q15(vl << HR, w15), opz_mul_q31_q15(xl, d15));
                y[2 * i + 1] = opz_add_q31(opz_mul_q31_q15(vr << HR, w15), opz_mul_q31_q15(xr, d15));
            }
        } else {
            for (int i = 0; i < CTRL; i++) {
                g += dg;
                w_og += dw;
                int32_t w15 = w_og >> 16;
                int32_t vl = clip(opz_mul_q31(x[2 * i], g), ck, h2, q);
                int32_t vr = clip(opz_mul_q31(x[2 * i + 1], g), ck, h2, q);
                y[2 * i] = opz_mul_q31_q15(vl << HR, w15);
                y[2 * i + 1] = opz_mul_q31_q15(vr << HR, w15);
            }
        }
        g = g_new;
    }
    s->gain = g;
    s->wet_og = wet_og; s->dry_og = dry_og;
}

OPZ_EXPORTS_FX();
