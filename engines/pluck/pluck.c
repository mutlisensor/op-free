/*
 * pluck — plucked and struck strings: one Karplus-Strong waveguide per voice, with a tuned loop low-pass, four
 * dispersion all-passes for stiff and metallic strings, and an exciter shaped by velocity.
 *
 *   P1        material    0 .. 84     nylon -> steel: the loop's high-frequency damping B falls from 300 to 20
 *                                     (the 4 kHz region's T60 from 0.14 s to 2.1 s, the same on every note);
 *                                     plucked: a noise burst one period long
 *                         85 .. 169   stiff string, piano-like: the four all-passes take 0 -> 30 % of the period
 *                                     at DC (quadratic over the zone), which stretches the upper partials sharp
 *                                     (10th partial +0.8 % at 128, +6 % at 169); B 20 -> 40; the exciter turns
 *                                     from a pick into a hammer (a smooth pulse) over 85 .. 127
 *                         170 .. 255  metallic, bell-like: dispersion 30 -> 70 % of the period (at 255 the
 *                                     partials sit at 1, 2.34, 4.22, 6.51, 9.03, 11.65); B 40 -> 5 (4 kHz T60 8.5 s)
 *   P2        decay       0 .. 191    T60 of the fundamental 0.1 s -> 25.6 s (8 octaves, exponential); below 112
 *                                     the damping B also rises, up to x 8 at 0 (palm mute)
 *                         192 .. 255  a bow-like noise feed sustains the string, from nothing to full at 255, with
 *                                     the same sustained level on every note; the T60 shortens from 25.6 s to 4 s
 *                                     on the way, so the bow takes hold faster; it stops at note-off
 *   velocity              level (linear); exciter brightness (two one-pole low-passes at the 2nd .. 72nd
 *                         harmonic: soft = dark thump, hard = bright pick or hammer); pluck position (1/4 of the
 *                         string at velocity 0 -> 1/8 at 127: hard notes are thinner; the hammer strikes at 1/8);
 *                         hammer width P/4 -> P/64
 *   note-off              the host envelope releases the level as on every engine, and a damper touches the string:
 *                         from the first release block the string's own decay is at most twice the release time (it
 *                         loses c^(P/2) per period, c = the envelope's per-sample release multiplier, P = the period
 *                         in samples), its damping B rises x 4 (darker), and the bow stops
 *
 * The read delay is fractional (4-point Hermite) and corrected for the phase delay of the loop low-pass and the
 * all-passes at the fundamental, and the loop gain for their loss there, so every note is in tune (within 0.6 cent
 * in the simulator, C0 .. C8) and decays as P2 says at any P1 and pitch bend. Each voice owns a 1,024-sample line
 * from a static pool of 13 (every synth voice of the OP-Z: bass 2, lead 3, arp 2, chord 6 with generous chords).
 * Notes whose period is longer than 960 samples (below ~46 Hz) run the string at 1/2, 1/4 or 1/8 of the sample
 * rate, linearly interpolated to 44.1 kHz.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine; pluck never reads
 * them.
 *
 * Written from scratch after the classic Karplus-Strong / digital waveguide string (Karplus & Strong; Jaffe &
 * Smith; Van Duyne & Smith's dispersion all-passes), in the spirit of the inharmonic string of Mutable
 * Instruments' Plaits and the OP-1's string engine (no code from either). MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define POOL        13                          /* every synth voice: bass 2 + lead 3 + arp 2 + chord 6 */
#define RING        1024                        /* samples per line */
#define RING_MASK   (RING - 1)
#define GUARD       4                           /* line[RING .. RING + 2] mirror line[0 .. 2]: reads never wrap */
#define NO_SLOT     0xff
#define Q30_ONE     (1 << 30)
#define D_MIN       (3 << 16)                   /* read delay limits, Q16 samples (the Hermite needs 2 .. RING - 3) */
#define D_MAX       ((RING - 4) << 16)
#define P_SRC_MAX   (960 << 16)                 /* a period longer than this halves the string's rate: ~1 semitone of
                                                   bend-down headroom below D_MAX */
#define M_MAX       3                           /* string rate down to fs / 8: periods up to 7,680 samples (5.7 Hz) */
#define G_CAP       (0x7fffffff - (1 << 15))    /* loop gain limit, 1 - 2^-16 */
#define KE_MAX      0x3cccccc0                  /* exciter low-pass coefficient limit, 0.95 in Q30 */
#define P2_KNEE     49152                       /* P2 192 of 256 in opz_p16 units: decay -> bow */
#define P2_MUTE     28672                       /* P2 112: below it the loop darkens */
#define Z1          21760                       /* P1 85: material zones */
#define Z2          43520                       /* P1 170 */

/* Output calibration (see README, facts): the string's peak level in the line and the output gain that maps it to
 * OPZ_VOICE_PEAK at velocity 127 */
#define EXC_NOISE   9000                        /* burst amplitude, Q15, before the 1/sqrt(ke) boost */
#define EXC_HAMMER  0x38000000                  /* hammer pulse height, Q31 */
#define EXC_LIM     0x34000000                  /* the burst is limited to 0.41 (the noise's crest varies by note) */
#define OUT_GAIN    20000                       /* Q15 */
#define BOW_GAIN    16000                       /* bow amplitude at P2 255 for a loop that loses 1 - g per period */

/* One line per pool entry. Owners are the voice state pointers; `seen` is the render clock of each entry's last
 * block, so a note-on that finds no free line (more than 13 voices) takes the one idle longest. */
typedef struct Line {
    int16_t s[RING + GUARD];
} Line;

static Line s_line[POOL] __attribute__((aligned(4)));
static const void *s_owner[POOL];
static uint32_t s_seen[POOL];
static uint32_t s_clock;
static uint32_t s_seed = 0x2545f491u;          /* note-on noise seeds, one draw per note */

/* render's block buffers: render runs one voice at a time, so all voices share them */
static int32_t s_exc[OPZ_BLOCK_FRAMES];        /* exciter, Q31, per string step */
static int32_t s_str[OPZ_BLOCK_FRAMES];        /* string output, Q15, per string step */
static int32_t s_buf[OPZ_BLOCK_FRAMES];        /* the string's loop signal between its passes, Q31 */

typedef struct State {
    int32_t D, tD;                              /* read delay now and at the end of this block, string samples, Q16 */
    int32_t k, a;                               /* loop low-pass and all-pass coefficients, Q15 */
    int32_t b0;                                 /* loop gain x k, Q31 */
    int32_t lp;                                 /* loop low-pass output (after the gain), Q31 */
    int32_t err;                                /* the line's quantisation error, carried to the next sample */
    int32_t ap[5];                              /* all-pass memories: input of stage 0, outputs of stages 0 .. 3 */
    int32_t last;                               /* last string output of the previous block (sub-rate), Q15 */
    int32_t e1, e2;                             /* exciter low-pass states, Q31 */
    int32_t ke;                                 /* exciter low-pass coefficient, Q15 */
    int32_t carry;                              /* what the burst's limiter has still to give back, Q31 */
    uint32_t seed1, seed2;                      /* exciter noise, the comb's two taps (same sequence, lagged) */
    int32_t t;                                  /* string steps since note-on */
    int32_t burst, comb, ham, quiet;            /* burst length, comb lag, hammer width, end of the exciter (steps) */
    int32_t na;                                 /* burst noise amplitude, Q15 */
    int32_t ha;                                 /* hammer height, Q31 */
    int32_t bow;                                /* bow noise amplitude, Q15 (per block) */
    uint32_t seed3;                             /* bow noise */
    int32_t b1, b2, bdc;                        /* bow low-pass states and its DC trap, Q31 */
    uint32_t c_inc;                             /* what the coefficients were computed for: string increment, */
    int32_t c_p1, c_p2, c_damp;                 /* P1, P2, damper */
    uint32_t p_inc;                             /* the increment tD was last computed for */
    uint32_t c_age;                             /* blocks since the coefficients were computed */
    int32_t amp15;                              /* output gain: velocity x calibration, Q15 */
    OpzMix mix;                                 /* the track's level and pan (opz_mix.h) */
    uint16_t w;                                 /* write index */
    uint8_t slot;                               /* pool entry, NO_SLOT for none */
    uint8_t m;                                  /* the string runs at fs / 2^m */
    uint8_t vel;
    uint8_t fresh;                              /* first block after note-on */
    uint8_t mode;                               /* all-pass chain, Tune.mode */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;

/* Signed 16 x 16 -> 32 multiply of the low halves: one Blackfin instruction. */
#if defined(__bfin__)
ALWAYS_INLINE int32_t mul_s16(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
#else
ALWAYS_INLINE int32_t mul_s16(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int16_t)b; }
#endif

/* ---- control maths (once per block: 32-bit fractional multiplies, a few 32-bit divisions) -------------------- */

/* a * b for Q30 values (|result| < 2). */
static inline int32_t mul30(int32_t a, int32_t b) { return opz_mul_q31(a, b) * 2; }

/* num / den in Q30 for 0 <= num < 2 den (den > 0; 2 or more gives 0x7fffffff), to ~30 bits: den normalised and
 * its reciprocal by opz_recip_q31 (Newton, no division). */
static int32_t div30(uint32_t num, uint32_t den)
{
    if (den == 0 || num >= 2 * den || num >= 0x80000000u) return 0x7fffffff;
    if (den >= 0x80000000u) { den >>= 1; num >>= 1; }
    int n = opz_norm32((int32_t)den);                                    /* den << n in [2^30, 2^31) */
    uint32_t r = (uint32_t)opz_recip_q31((int32_t)(den << n));          /* 2^61 / (den << n) */
    return (int32_t)opz_mul_u32_shr16((num << n) >> 8, r >> 7);         /* (num / den) 2^30 < 2^31 */
}

/* n / d (n < 2^31, d > 0), to ~22 bits, without a division: d normalised and its reciprocal by opz_recip_q31. */
static uint32_t uquot(uint32_t n, uint32_t d)
{
    int nd = opz_norm32((int32_t)d);                                     /* d << nd in [2^30, 2^31) */
    uint32_t r = (uint32_t)opz_recip_q31((int32_t)(d << nd));           /* 2^61 / (d << nd) */
    return opz_mul_u32_shr16(n >> 8, r >> 7) >> (30 - nd);               /* n r / 2^31 = (n / d) 2^(30 - nd) */
}

/* e^x for |x| <= ln 2, Q30 in and out: Taylor to x^8. */
static int32_t exp30(int32_t x)
{
    static const int32_t c[9] = { 1073741824, 1073741824, 536870912, 178956971, 44739243,
                                  8947849, 1491308, 213044, 26631 };
    int32_t r = c[8];
    for (int k = 7; k >= 0; k--) r = c[k] + mul30(x, r);
    return r;
}

#define LN2_Q30     744261118

/* 2^-x for x >= 0 in Q27 (up to 16 octaves), Q31 result. */
static int32_t exp2_neg_q31(int32_t x)
{
    if (x <= 0) return 0x7fffffff;
    int32_t i = x >> 27;
    if (i > 30) return 0;
    int32_t r = exp30(-mul30((x & ((1 << 27) - 1)) << 3, LN2_Q30));        /* 2^-f, Q30 in (0.5, 1] */
    return (r >> i) >= 0x40000000 ? 0x7fffffff : (r >> i) << 1;
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

#define PI_Q29      1686629713

/* atan2(y, x) for y >= 0 (Q30, |x| < 2), Q29 in [0, pi]. */
static int32_t atan2_29(int32_t y, int32_t x)
{
    uint32_t ax = x < 0 ? (uint32_t)-x : (uint32_t)x;
    if (y <= 0) return x >= 0 ? 0 : PI_Q29;
    int32_t t = (uint32_t)y <= ax ? atan29(div30((uint32_t)y, ax)) : (PI_Q29 >> 1) - atan29(div30(ax, (uint32_t)y));
    return x < 0 ? PI_Q29 - t : t;
}

/* Phase delay in samples (Q16) of a phase lag atan2(y, x) at w = 2 pi / P (P in Q16): lag x P / 2 pi. */
static int32_t phase_delay(int32_t y, int32_t x, int32_t P)
{
    uint32_t lag = opz_mul_u32_shr16((uint32_t)atan2_29(y, x) >> 8, (uint32_t)P >> 5);   /* lag P, Q16 */
    return (int32_t)opz_mul_u32_shr16(lag, 10430);                                        /* / 2 pi */
}

/* sqrt(x) for a Q30 x in [0, 1], Q15 result (bit by bit, 15 steps). */
static int32_t sqrt15(uint32_t x)
{
    uint32_t r = 0;
    for (uint32_t b = 1u << 14; b; b >>= 1) {
        uint32_t t = r | b;
        if (t * t <= x) r = t;
    }
    return (int32_t)r;
}

/* ---- the voice pool ---------------------------------------------------------------------------------------- */

static void free_slot(State *s)
{
    if (s->slot < POOL && s_owner[s->slot] == s) s_owner[s->slot] = 0;
    s->slot = NO_SLOT;
}

static int claim_slot(State *s)
{
    if (s->slot < POOL && s_owner[s->slot] == s) return s->slot;
    int best = -1;
    uint32_t idle = 0;
    for (int i = 0; i < POOL; i++) {
        if (!s_owner[i]) { best = i; break; }
        uint32_t age = s_clock - s_seen[i];
        if (best < 0 || age > idle) { best = i; idle = age; }
    }
    s_owner[best] = s;
    s_seen[best] = s_clock;
    return best;
}

/* ---- per block: the string's filters, delay and gain from the pitch and the knobs --------------------------- */

typedef struct Tune {
    int32_t D;                                  /* read delay, Q16 */
    int32_t k, a;                               /* Q15 */
    int32_t g;                                  /* Q31 */
    int32_t P;                                  /* period in string samples, Q16 */
    int32_t w;                                  /* 2 pi / P, Q30 */
    int mode;                                   /* all-pass chain: 0 bypassed, 1 unit delays (a = 0), 2 all-passes */
} Tune;

/* From P1, piecewise linear over the three zones: log2 of the loop's damping B (Q16) and the dispersion (fraction of
 * the period at DC, Q16). B is the decay rate of high frequencies in nepers per second at w = 1 rad (6.3 kHz):
 * the 4 kHz region dies with a T60 of 42.5 / B seconds on every note, whatever its pitch. */
static void material(int32_t p1, int32_t *lg_b, int32_t *lambda)
{
    if (p1 < Z1) {                              /* nylon B 300 (4 kHz T60 0.14 s) -> steel B 20 (2.1 s) */
        uint32_t t = opz_mul_u32_shr16((uint32_t)p1 << 8, 197379) >> 8;      /* p1 / Z1, Q16 */
        *lg_b = 539300 - (int32_t)opz_mul_u32_shr16(t, 256050);              /* log2 300 -> log2 20 */
        *lambda = 0;
    } else if (p1 < Z2) {                       /* stiff: B 20 -> 40, dispersion 0 -> 0.3, quadratic */
        uint32_t t = opz_mul_u32_shr16((uint32_t)(p1 - Z1) << 8, 197379) >> 8;
        *lg_b = 283250 + (int32_t)(t >> 0);                                   /* + 1 octave */
        *lambda = (int32_t)opz_mul_u32_shr16(opz_mul_u32_shr16(t, t), 19661);   /* 0.3 t^2 */
    } else {                                    /* metal: B 40 -> 5 (4 kHz T60 8.5 s), dispersion 0.3 -> 0.7 */
        uint32_t t = opz_mul_u32_shr16((uint32_t)(p1 - Z2) << 8, 195084) >> 8;   /* / 22016 */
        *lg_b = 348786 - (int32_t)opz_mul_u32_shr16(t, 196608);              /* - 3 octaves */
        *lambda = 19661 + (int32_t)opz_mul_u32_shr16(t, 26214);              /* 0.3 + 0.4 t */
    }
}

/* floor(sqrt(x)) */
static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) {
        if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* sqrt of a Q16 value >= 1, Q16, to 16 significant bits */
static int32_t sqrt_q16(uint32_t u)
{
    int n = 0;
    while (n < 14 && u < (1u << 30)) { u <<= 2; n++; }
    return (int32_t)(isqrt32(u) << (8 - n));
}

/* 1 / |H(w)| of the loop low-pass and the Hermite read at the fraction of D (Q30): the loss per pass the gain makes
 * up for. |H_lp|^2 = 1 / (1 + x), x = 2 p (1 - cos w) / k^2; |H_int|^2 = re^2 + im^2 from the four weights;
 * 1 / |H|^2 = 1 + y, y = (1 + x - |H_int|^2) / |H_int|^2, and sqrt(1 + y) by its series (y <= 1.5, never above the
 * true value). Every term is a difference from 1, so a fundamental that loses 10^-5 per pass is still made up for
 * exactly. */
static int32_t loss_comp(int32_t D, int32_t sn, int32_t h, int32_t k30, int32_t p30)
{
    int32_t x = div30((uint32_t)mul30(p30, h) * 2, (uint32_t)mul30(k30, k30) + 1);    /* <= 2 */
    int32_t f = (D >> 1) & 0x7fff, f2 = (f * f) >> 15, f3 = (f2 * f) >> 15;
    int32_t wm1 = (2 * f2 - f3 - f) >> 2, w1 = (4 * f2 - 3 * f3 + f) >> 2, w2 = (f3 - f2) >> 2;
    int32_t c1 = Q30_ONE - h;                                            /* cos w */
    int32_t s2 = mul30(sn, c1) * 2, c2 = Q30_ONE - mul30(sn, sn) * 2;   /* sin 2w, cos 2w */
    /* re - 1 = (w0 - 1) + (wm1 + w1) cos w + w2 cos 2w, with w0 - 1 = -(wm1 + w1 + w2): no large terms */
    int32_t re1 = -mul30((wm1 + w1) << 16, h) - mul30(w2 << 16, Q30_ONE - c2);
    int32_t im = mul30((wm1 - w1) << 16, sn) - mul30(w2 << 16, s2);
    int32_t m2 = Q30_ONE + 2 * re1 + mul30(re1, re1) + mul30(im, im);    /* |H_int|^2 */
    int32_t y = div30((uint32_t)(x + (Q30_ONE - m2)), (uint32_t)m2);
    if (y > (3 << 29)) y = 3 << 29;
    int32_t y2 = mul30(y, y);
    return Q30_ONE + (y >> 1) - (y2 >> 3) + (mul30(y2, y) >> 4) - (mul30(y2, y2) * 5 >> 7);
}

/* inc_s: the string's phase increment per string sample (pitch << m); damp: extra octaves of decay per period
 * (Q27, the note-off damper, 0 while held); dark: the damper's low-pass drop (1 octave when released). */
/* The fundamental's angle: period P (Q16), w = 2 pi / P, sin w and h = 1 - cos w (Q30) from the half angle (Taylor;
 * w / 2 <= pi / 4; h kept for precision). */
typedef struct Angle {
    int32_t P, w, sn, h;
} Angle;

static void angle(Angle *A, uint32_t inc_s)
{
    if (inc_s < (1u << 20)) inc_s = 1u << 20;   /* period <= 4096 */
    if (inc_s > (1u << 30)) inc_s = 1u << 30;   /* period >= 4: w <= pi / 2 */
    A->P = (int32_t)(2u * opz_blep_recip(inc_s));                       /* 2^48 / inc, Q16 */
    A->w = (int32_t)opz_mul_u32_shr16(inc_s, 102944);                   /* 2 pi inc / 2^32 in Q30 = inc pi / 2 */
    int32_t x = A->w >> 1, x2 = mul30(x, x);
    int32_t sh = mul30(x, Q30_ONE - mul30(x2, 178956971 - mul30(x2, 8947849 - mul30(x2, 213044))));
    int32_t ch = Q30_ONE - mul30(x2, (Q30_ONE >> 1) - mul30(x2, 44739243 - mul30(x2, 1491308 - mul30(x2, 26631))));
    A->sn = mul30(sh, ch) * 2;
    A->h = mul30(sh, sh) * 2;
}

/* Phase delays at the fundamental (Q16 samples) of the loop low-pass y += k (x - y) and of one all-pass
 * (a + z^-1) / (1 + a z^-1) (Q15 coefficients, as the loop uses them). */
static int32_t tau_lp(const Angle *A, int32_t k15)
{
    int32_t k30 = k15 << 15, p30 = Q30_ONE - k30;
    return phase_delay(mul30(p30, A->sn), k30 + mul30(p30, A->h), A->P);
}

static int32_t tau_ap(const Angle *A, int32_t a15)
{
    int32_t a30 = a15 << 15, a2 = mul30(a30, a30);
    int32_t b = Q30_ONE + a30;                                           /* 2a + (1 + a^2) cos w = (1+a)^2 - (1+a^2) h */
    return phase_delay(mul30(Q30_ONE - a2, A->sn), mul30(b, b) - mul30(Q30_ONE + a2, A->h), A->P);
}

/* The read delay that makes the loop one period long at the fundamental: line + low-pass + 4 all-passes. */
static int32_t loop_delay(const Angle *A, int32_t tlp, int32_t tap)
{
    int32_t D = A->P - tlp - 4 * tap;
    return D < D_MIN ? D_MIN : (D > D_MAX ? D_MAX : D);
}

static void tune(Tune *T, uint32_t inc_s, int m, int32_t p1, int32_t p2, int32_t damp, int dark)
{
    Angle A;
    angle(&A, inc_s);
    int32_t P = A.P, w = A.w, sn = A.sn, h = A.h;

    /* loop low-pass y += k (x - y). Per pass it takes x / 2 = (1 - k) w_s^2 / (2 k^2) nepers at a low w_s; with
     * (1 - k) / k^2 = c = B P_s / (fs 2^m) that is B w^2 / 2 per second at every pitch (w at 44.1 kHz). So
     * k = 2 / (1 + sqrt(1 + 4c)): darker on low notes, transparent on high ones. B x 8 at P2 0 (palm mute, from
     * P2 112 down), x 4 after note-off (the damper) */
    int32_t lg_b, lambda;
    material(p1, &lg_b, &lambda);
    if (p2 < P2_MUTE) lg_b += (int32_t)opz_mul_u32_shr16((uint32_t)(P2_MUTE - p2), 449390);  /* +3 oct at 0 */
    if (dark) lg_b += 2 << 16;
    int32_t B = opz_exp2_q16(lg_b);                                      /* Q16, < 2^31 (B < 32768) */
    int64_t c = ((((int64_t)B * P) >> 16) * 97391) >> (32 + m);         /* Q16; 97391 = 2^32 / 44100 */
    if (c > (1 << 26)) c = 1 << 26;
    int32_t sq = sqrt_q16((uint32_t)(65536 + 4 * c));
    int32_t k30 = div30(2u << 16, (uint32_t)(65536 + sq));
    int32_t k15 = k30 >> 15;
    if (k15 > 32767) k15 = 32767;
    if (k15 < 1) k15 = 1;
    k30 = k15 << 15;                                                     /* the coefficient the loop uses */
    int32_t p30 = Q30_ONE - k30;
    int32_t tlp = tau_lp(&A, k15);

    /* dispersion: four all-passes (a + z^-1) / (1 + a z^-1) with a DC delay of d = lambda P / 4 each,
     * a = (1 - d) / (1 + d); d is limited so the line keeps at least 3 samples */
    int32_t d = (int32_t)(opz_mul_u32_shr16((uint32_t)P, (uint32_t)lambda) >> 2);   /* Q16 */
    int32_t room = P - tlp - (3 << 16);
    int32_t dmax = room > 0 ? (int32_t)(((int64_t)room * 15) >> 6) : 0;            /* 0.94 room / 4 */
    if (d > dmax) d = dmax;
    if (d > (250 << 16)) d = 250 << 16;
    int32_t a15 = 0, tap = 1 << 16, mode = 1;                           /* a = 0: four unit delays */
    if (P < (12 << 16)) {                                                /* top notes: the chain is bypassed, so
                                                                            the loop can be 3 samples short */
        mode = 0;
        tap = 0;
    } else if (d > (1 << 16)) {
        mode = 2;
        a15 = -(div30((uint32_t)(d - (1 << 16)), (uint32_t)(d + (1 << 16))) >> 15);   /* (-1, 0) */
        tap = tau_ap(&A, a15);
    }
    int32_t D = loop_delay(&A, tlp, tap);

    /* loop gain for the fundamental's T60 = 0.1 s x 2^(8 P2 / 192): g = 2^-(9.966 P_out / (44100 T60)), times
     * 1 / |H| of the low-pass and the interpolator there, so P2 sets the fundamental's decay whatever they do;
     * capped below 1 (the loop passes DC at unity). See comp below. */
    int32_t oct = p2 < P2_KNEE ? p2 * 21845                              /* T60 octaves above 0.1 s, Q27 */
                               : (8 << 27) - (p2 - P2_KNEE) * 21940;    /* bow zone: 25.6 s -> 4 s */
    int32_t e = (int32_t)(((int64_t)(P << m) * 9706050) >> 21);               /* octaves per period at 0.1 s, Q27 */
    e = opz_mul_q31(e, exp2_neg_q31(oct));
    if (e < damp) e = damp;
    if (e < 1) e = 1;
    int32_t comp = loss_comp(D, sn, h, k30, p30);
    int64_t g = ((int64_t)exp2_neg_q31(e) * comp) >> 30;
    if (g > G_CAP) g = G_CAP;

    T->D = D;
    T->k = k15;
    T->a = a15;
    T->g = (int32_t)g;
    T->P = P;
    T->w = w;
    T->mode = mode;
}

/* ---- per note: the exciter ---------------------------------------------------------------------------------- */

/* From the velocity, P1 and the first block's period: brightness, pluck position, burst and hammer. */
static void excite(State *s, const Tune *T, int32_t p1)
{
    int32_t v = s->vel;                                                  /* 1 .. 127 */
    int32_t v2 = (v * v * 33) >> 4;                                      /* v^2 / 127^2 in Q15: 127 -> 33270 ~ 1 */
    if (v2 > 32767) v2 = 32767;

    /* hammer share: 0 in the plucked zone, 0 -> 1 over P1 85 .. 127 */
    int32_t hw = p1 <= Z1 ? 0 : (p1 >= 32768 ? 32767 : (int32_t)(((uint32_t)(p1 - Z1) * 195078u) >> 16));
    int32_t nw = 32767 - ((hw * 28000) >> 15);                           /* noise share: 1 -> 0.15 */

    /* exciter low-pass: 2 + 70 v^2 harmonics, two one-poles */
    int32_t he = (2 << 16) + 70 * (v2 << 1);                             /* harmonics, Q16 */
    uint32_t k22 = opz_mul_u32_shr16((uint32_t)T->w >> 8, (uint32_t)he);
    int32_t ke30 = k22 >= (KE_MAX >> 8) ? KE_MAX : (int32_t)(k22 << 8);
    int32_t ke15 = ke30 >> 15;
    if (ke15 < 16) ke15 = 16;
    s->ke = ke15;

    /* pluck position beta: 1/4 at velocity 0 -> 1/8 at 127; the hammer strikes at 1/8. The comb (the source minus
     * itself beta P later) has no DC and gives the fundamental 2 sin(pi beta) */
    int32_t beta = 16384 - ((v2 * 8192) >> 15);                          /* Q16: 0.25 -> 0.125 */
    beta += (int32_t)(((int64_t)(8192 - beta) * hw) >> 15);
    int32_t Ps = T->P >> 16;
    int32_t K = (int32_t)(((int64_t)T->P * beta) >> 32);
    if (K < 1) K = 1;
    s->comb = K;
    s->burst = Ps < 1 ? 1 : Ps;
    int32_t W = (int32_t)uquot((uint32_t)T->P, (uint32_t)((4 << 16) + 60 * (v2 << 1)));   /* P / (4 + 60 v^2) */
    if (W > Ps / 2) W = Ps / 2;
    if (W < 1) W = 1;
    s->ham = W;

    /* amplitudes: the burst's level drops ~ sqrt(ke) through the low-pass; boost it back (up to x4) */
    int32_t boost = sqrt15((uint32_t)ke15 << 15);                        /* sqrt(ke), Q15 */
    int32_t na = boost < 8192 ? EXC_NOISE * 4 : (int32_t)uquot(EXC_NOISE << 15, (uint32_t)boost);
    s->na = (na * nw) >> 15;
    s->ha = (int32_t)(((int64_t)EXC_HAMMER * hw) >> 15);
    s->quiet = (s->burst > W ? s->burst : W) + K + (int32_t)uquot(9 * 32768, (uint32_t)ke15) + 8;
}

/* ---- exports ------------------------------------------------------------------------------------------------ */

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;                                                     /* instruments get none: the pool is ours */
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    for (int i = 0; i < POOL; i++)                                       /* a voice record coming back to us */
        if (s_owner[i] == state) s_owner[i] = 0;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    ((State *)state)->slot = NO_SLOT;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note;                                                          /* the pitch comes with render */
    State *s = state;
    int slot = claim_slot(s);
    s->slot = (uint8_t)slot;
    volatile uint32_t *z = (volatile uint32_t *)s_line[slot].s;          /* a new note starts on a still string */
    for (unsigned i = 0; i < (RING + GUARD) / 2; i++) z[i] = 0;
    s->w = 0;
    s->lp = 0;
    s->err = 0;
    for (int i = 0; i < 5; i++) s->ap[i] = 0;
    s->last = 0;
    s->e1 = s->e2 = s->carry = 0;
    s->b1 = s->b2 = s->bdc = 0;
    s->t = 0;
    s->bow = 0;
    s_seed = s_seed * 1664525u + 1013904223u;
    s->seed1 = s->seed2 = s_seed;
    s->seed3 = s_seed ^ 0x9e3779b9u;
    s->vel = (uint8_t)(velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
    s->amp15 = (int32_t)(((uint32_t)s->vel * 258u * OUT_GAIN) >> 15);   /* velocity 127 -> OUT_GAIN */
    s->fresh = 1;
    if (params) opz_mix_start(&s->mix, params);                          /* NULL: ramps in at render */
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                             /* the host releases the envelope */
}

/* The exciter's burst for ns string steps into s_exc: noise burst and hammer pulse, each minus itself `comb` steps
 * later (the pluck position: no DC, the fundamental at 2 sin(pi beta)), through two one-pole low-passes. */
static void pass_burst(State *s, int ns)
{
    uint32_t s1 = s->seed1, s2 = s->seed2;
    int32_t t = s->t, K = s->comb, L = s->burst, W = s->ham, na = s->na, ha = s->ha, ke = s->ke;
    int32_t e1 = s->e1, e2 = s->e2, carry = s->carry;
    for (int i = 0; i < ns; i++) {
        s1 = s1 * 1664525u + 1013904223u;
        int32_t x = t < L ? opz_mul_q31_q15((int32_t)s1, na) : 0;
        if (t < W) x += ha;
        int32_t u = t - K;
        if (u >= 0) {
            s2 = s2 * 1664525u + 1013904223u;
            if (u < L) x -= opz_mul_q31_q15((int32_t)s2, na);
            if (u < W) x -= ha;
        }
        e1 += opz_mul_q31_q15(x - e1, ke);
        e2 += opz_mul_q31_q15(e1 - e2, ke);
        /* the noise's rare peaks are limited, and what they lose is carried into the next samples: the burst's sum
         * (its DC) stays zero, which a plain clamp would break; the string would ring with that DC for its T60 */
        int32_t v = e2 + carry;
        int32_t c = v > EXC_LIM ? EXC_LIM : (v < -EXC_LIM ? -EXC_LIM : v);
        carry = v - c;
        s_exc[i] = c;
        t++;
    }
    s->seed1 = s1;
    s->seed2 = s2;
    s->e1 = e1;
    s->e2 = e2;
    s->carry = carry;
}

/* The bow for ns string steps: noise at s->bow through the same two low-passes, minus its slow mean (a one-pole
 * DC trap at fs / 2^(m + 9) / 2 pi, ~14 Hz at the full rate: no DC builds up in the loop), into s_exc (added
 * when the burst is still running). */
ALWAYS_INLINE void pass_bow(State *s, int ns, int add)
{
    uint32_t s3 = s->seed3;
    int32_t bow = s->bow, ke = s->ke, b1 = s->b1, b2 = s->b2, dc = s->bdc;
    for (int i = 0; i < ns; i++) {
        s3 = s3 * 1664525u + 1013904223u;
        int32_t x = opz_mul_q31_q15((int32_t)s3, bow);
        b1 += opz_mul_q31_q15(x - b1, ke);
        b2 += opz_mul_q31_q15(b1 - b2, ke);
        dc += (b2 - dc) >> 9;
        s_exc[i] = add ? s_exc[i] + (b2 - dc) : b2 - dc;
    }
    s->seed3 = s3;
    s->b1 = b1;
    s->b2 = b2;
    s->bdc = dc;
}

/* The four Catmull-Rom weights (Q14) for a fraction f (Q15). w0 is the remainder, so they sum to exactly 1 at any
 * f: the loop passes DC exactly and the rounding of f^2 and f^3 can't build up an offset (with Horner's rounded
 * steps it did, by a fraction of an LSB per pass, which a loop gain near 1 turns into a large DC). */
ALWAYS_INLINE void weights(int32_t f, int32_t *wm1, int32_t *w0, int32_t *w1, int32_t *w2)
{
    int32_t f2 = mul_s16(f, f) >> 15, f3 = mul_s16(f2, f) >> 15;
    *wm1 = (2 * f2 - f3 - f) >> 2;
    *w1 = (4 * f2 - 3 * f3 + f) >> 2;
    *w2 = (f3 - f2) >> 2;
    *w0 = 16384 - *wm1 - *w1 - *w2;
}

/* The string runs in three short passes per chunk of steps, so each loop keeps its values in registers:
 *   pass_read   x at age D (4-point Hermite between ages di and di + 1; ramp: D moves by dD per step and the
 *               weights follow it every 8 steps, else they are computed once), then the loop low-pass and gain,
 *               y = (1 - k) y + g k x, into s_buf (Q31)
 *   pass_ap     the all-pass chain in place (ap 1: four unit delays, a = 0; ap 2: four all-passes)
 *   pass_write  plus the exciter (ex), truncated to 16 bits, into the line and s_str (the output is the line's
 *               input: no period of latency)
 * A chunk is never longer than the newest tap's age (di - 1), so nothing written in a chunk is read in it. */
ALWAYS_INLINE void pass_read(State *s, const int16_t *line, int n, int32_t dD, int ramp)
{
    uint32_t w = s->w;
    int32_t D = s->D, c1 = 32768 - s->k, b0 = s->b0, lp = s->lp;
    int32_t *buf = s_buf;
    int32_t wm1, w0, w1, w2;

    for (int i = 0; i < n; i += 8) {
        int g = n - i < 8 ? n - i : 8;
        int32_t Dg = ramp ? D + dD * (g >> 1) : D;                       /* ramp: the weights follow D every 8 steps */
        uint32_t back = (uint32_t)(Dg >> 16) + 2u;
        weights((Dg >> 1) & 0x7fff, &wm1, &w0, &w1, &w2);
        for (int j = 0; j < g; j++) {
            const int16_t *p = line + ((w - back) & RING_MASK);
            int32_t acc = mul_s16(w2, p[0]) + mul_s16(w1, p[1]) + mul_s16(w0, p[2]) + mul_s16(wm1, p[3]);
            lp = opz_add_q31(opz_mul_q31_q15(lp, c1), opz_mul_q31(opz_shl_q31(acc, 2), b0));
            *buf++ = lp;
            w++;
        }
        if (ramp) D += dD * g;
    }
    s->D = D;
    s->lp = lp;
}

ALWAYS_INLINE void pass_ap(State *s, int n, int ap)
{
    int32_t a = s->a, a0 = s->ap[0], a1 = s->ap[1], a2 = s->ap[2], a3 = s->ap[3], a4 = s->ap[4];
    int32_t *buf = s_buf;
    for (int i = 0; i < n; i++) {
        int32_t y = buf[i];
        if (ap == 2) {
            int32_t o0 = opz_add_q31(a0, opz_mul_q31_q15(opz_sub_q31(y, a1), a));
            int32_t o1 = opz_add_q31(a1, opz_mul_q31_q15(opz_sub_q31(o0, a2), a));
            int32_t o2 = opz_add_q31(a2, opz_mul_q31_q15(opz_sub_q31(o1, a3), a));
            int32_t o3 = opz_add_q31(a3, opz_mul_q31_q15(opz_sub_q31(o2, a4), a));
            a0 = y; a1 = o0; a2 = o1; a3 = o2; a4 = o3;
            buf[i] = o3;
        } else {                                /* a = 0: four unit delays (tune() counts them) */
            buf[i] = a4 = a3;
            a3 = a2; a2 = a1; a1 = a0; a0 = y;
        }
    }
    s->ap[0] = a0; s->ap[1] = a1; s->ap[2] = a2; s->ap[3] = a3; s->ap[4] = a4;
}

/* Writes go to 16 bits with error feedback: the part below the LSB is carried into the next sample, so the line
 * keeps the loop's precision on average. Plain rounding freezes a decaying string once (1 - g) |v| < 1/2 LSB (a
 * limit cycle), truncation towards zero damps it far beyond its T60 (a high note's tail lost 20 dB/s); with the
 * carry neither happens, and the rounding noise is pushed up in frequency. The first GUARD - 1 samples also go
 * into the mirror. */
ALWAYS_INLINE void pass_write(State *s, int16_t *line, int n, int off, int ex)
{
    uint32_t w = s->w;
    int32_t err = s->err;
    const int32_t *buf = s_buf, *exc = s_exc + off;
    int32_t *str = s_str + off;
    for (int i = 0; i < n; i++) {
        int32_t y = buf[i];
        if (ex) y = opz_add_q31(y, exc[i]);
        uint32_t wi = w & RING_MASK;
        int32_t u = opz_add_q31(y, err);
        int32_t q = u >> 16;
        err = u - (q << 16);                    /* 0 .. 65535 */
        line[wi] = (int16_t)q;
        line[wi + (((wi - (GUARD - 1)) >> 31) << 10)] = (int16_t)q;      /* + RING if wi < 3, no branch */
        str[i] = q;
        w = wi + 1;
    }
    s->w = (uint16_t)(w & RING_MASK);
    s->err = err;
}

ALWAYS_INLINE void string_chunk(State *s, int16_t *line, int n, int off, int32_t dD, int ap, int ex)
{
    if (dD) pass_read(s, line, n, dD, 1);
    else pass_read(s, line, n, 0, 0);
    if (ap) pass_ap(s, n, ap);
    pass_write(s, line, n, off, ex);
}

/* Output: s_str x velocity x the envelope, into the mono block buffer x (all 64 frames; opz_mix_mono then adds it
 * into out with the track's level and pan). m > 0: the string ran at fs / 2^m, linearly
 * interpolated between its steps. em (compile-time): 0 the envelope's step is 0 (decay, release: one multiply),
 * 1 sustain (coef 1: the step's (1 - level^3) s is taken once per block and added per sample; the host
 * recomputes s = (S - level) / 256 every block, so this only rounds the approach to S differently), 2 the exact
 * per-sample step (fade), 3 the same with coef 1 (attack: r = level, within an LSB of the host's coef x level). Returns the envelope level. */
ALWAYS_INLINE int32_t pass_out(fract32 *x, State *s, int m, int32_t level, int32_t coef, int32_t step, int em)
{
    int32_t amp15 = s->amp15, inc = 0;
    if (em == 1)
        inc = opz_mul_q31_rnd(opz_sub_q31(0x7fffffff, opz_mul_q31_rnd(opz_mul_q31_rnd(level, level), level)), step);
    if (m == 0) {
        const int32_t *str = s_str;
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
            x[i] = opz_mul_q31_q15(opz_mul_q31_q15(level, amp15), str[i]);
            level = em == 0 ? opz_env_tick0(level, coef) : em == 1 ? opz_add_q31(level, inc)
                    : em == 3 ? opz_add_q31(level, opz_mul_q31_rnd(opz_sub_q31(0x7fffffff,
                                opz_mul_q31_rnd(opz_mul_q31_rnd(level, level), level)), step))
                              : opz_env_tick(level, coef, step);
        }
        s->last = str[OPZ_BLOCK_FRAMES - 1];
        return level;
    }
    int n = 1 << m, ns = OPZ_BLOCK_FRAMES >> m;
    int32_t prev = s->last;
    for (int q = 0; q < ns; q++) {
        int32_t cur = s_str[q], d = cur - prev;
        for (int j = 1; j <= n; j++) {
            int32_t y = prev + ((d * j) >> m);
            *x++ = opz_mul_q31_q15(opz_mul_q31_q15(level, amp15), y);
            level = em == 0 ? opz_env_tick0(level, coef) : em == 1 ? opz_add_q31(level, inc)
                    : em == 3 ? opz_add_q31(level, opz_mul_q31_rnd(opz_sub_q31(0x7fffffff,
                                opz_mul_q31_rnd(opz_mul_q31_rnd(level, level), level)), step))
                              : opz_env_tick(level, coef, step);
        }
        prev = cur;
    }
    s->last = prev;
    return level;
}

/* The string for ns steps (ap, ex: compile-time choices in each copy), in chunks no longer than the delay. */
static void run_string(State *s, int16_t *line, int ns, int32_t dD, int ap, int ex)
{
    int32_t dmin = (dD < 0 ? s->tD : s->D) >> 16;                        /* shortest delay in this block, >= 3 */
    int chunk = dmin - 1 < ns ? dmin - 1 : ns;
    for (int off = 0; off < ns; off += chunk) {
        int n = ns - off < chunk ? ns - off : chunk;
        if (ap == 2) { if (ex) string_chunk(s, line, n, off, dD, 2, 1); else string_chunk(s, line, n, off, dD, 2, 0); }
        else if (ap == 1) { if (ex) string_chunk(s, line, n, off, dD, 1, 1); else string_chunk(s, line, n, off, dD, 1, 0); }
        else { if (ex) string_chunk(s, line, n, off, dD, 0, 1); else string_chunk(s, line, n, off, dD, 0, 0); }
    }
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    s_clock++;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);
    int32_t stage = OPZ_ADSR_STAGE(env);
    if (stage == 0) {                                                    /* the host frees the voice: so do we */
        free_slot(s);
        return;
    }
    if (s->slot >= POOL || s_owner[s->slot] != s) {                     /* line taken by a 14th voice: end */
        s->slot = NO_SLOT;
        OPZ_ADSR_LEVEL(env) = 0;
        OPZ_ADSR_STAGE(env) = 0;
        return;
    }
    s_seen[s->slot] = s_clock;
    int16_t *line = s_line[s->slot].s;

    int32_t p1 = opz_p16(params[OPZ_P_P1]), p2 = opz_p16(params[OPZ_P_P2]);
    int32_t level = OPZ_ADSR_LEVEL(env), env_coef = OPZ_ADSR_COEF(env), env_step = OPZ_ADSR_STEP(env);
    uint32_t pitch_end = pitch + (uint32_t)pitchSlope * OPZ_BLOCK_FRAMES;

    if (s->fresh) {                                                      /* sub-rate for long periods */
        uint32_t P = 2u * opz_blep_recip(pitch < (1u << 19) ? (1u << 19) : pitch);
        int m = 0;
        while (m < M_MAX && (P >> m) > (uint32_t)P_SRC_MAX) m++;
        s->m = (uint8_t)m;
    }
    int m = s->m;

    /* note-off: the damper (decay at most twice the release time: (1 - c) P / (2 ln 2) octaves per period) and
     * a darker loop; the bow lifts */
    int released = stage == 4;
    int32_t damp = 0;
    if (released) {
        uint32_t lossq31 = (uint32_t)(0x7fffffff - env_coef);
        uint32_t Pout = 2u * opz_blep_recip(pitch_end < (1u << 19) ? (1u << 19) : pitch_end);   /* Q16 */
        uint32_t dd = opz_mul_u32_shr16(opz_mul_u32_shr16(Pout, lossq31) >> 4, 47274);     /* x 1 / (2 ln 2) */
        damp = dd > 0x7fffffffu ? 0x7fffffff : (int32_t)dd;
    }

    /* The string's coefficients, recomputed when the knobs or the damper change (knobs alone at most every other
     * block: an LFO on P1 or P2 moves them every block) or the pitch has moved 3 % (half a semitone) from where
     * they were computed. Smaller pitch moves (vibrato, bends, glides) keep k, a and g and recompute only the two
     * phase delays at the new pitch, so the delay is exact (a third of the work). */
    uint32_t inc_s = pitch_end << m;
    if (inc_s > (1u << 30)) inc_s = 1u << 30;
    int knobs = p1 != s->c_p1 || p2 != s->c_p2;
    uint32_t drift = inc_s > s->c_inc ? inc_s - s->c_inc : s->c_inc - inc_s;
    s->c_age++;
    if (!s->fresh && !knobs && damp == s->c_damp && drift <= s->c_inc >> 5) {
        if (inc_s != s->p_inc) {                                         /* k and a kept: only the delays */
            Angle A;
            angle(&A, inc_s);
            int32_t tap = s->mode == 2 ? tau_ap(&A, s->a) : (s->mode == 1 ? 1 << 16 : 0);
            s->tD = loop_delay(&A, tau_lp(&A, s->k), tap);
            s->p_inc = inc_s;
        }
    } else if (s->fresh || damp != s->c_damp || !knobs || s->c_age >= 2) {
        Tune T;
        tune(&T, inc_s, m, p1, p2, damp, released);
        s->c_inc = inc_s;
        s->c_p1 = p1;
        s->c_p2 = p2;
        s->c_damp = damp;
        s->p_inc = inc_s;
        s->c_age = 0;
        if (s->fresh) {
            s->D = T.D;
            excite(s, &T, p1);
        }
        s->tD = T.D;
        s->k = T.k;
        s->a = T.a;
        s->b0 = opz_mul_q31(T.g, T.k << 16);
        s->mode = (uint8_t)T.mode;

        /* bow: P2 192 .. 255, normalised by sqrt(1 - g) so the sustained level is the same at any pitch, and by
         * 1 / sqrt(ke) for the exciter low-pass */
        int32_t bow = 0;
        if (!released && p2 > P2_KNEE) {
            int32_t u = (p2 - P2_KNEE) >> 1;                             /* 0 .. 8192 */
            int32_t loss = sqrt15((uint32_t)(0x7fffffff - T.g) >> 1);    /* sqrt(1 - g), Q15 */
            bow = (int32_t)((uint32_t)(u * loss) >> 13) * BOW_GAIN >> 15;
            bow = (int32_t)uquot((uint32_t)bow << 15, (uint32_t)(sqrt15((uint32_t)s->ke << 15) | 1));
            if (bow > 12000) bow = 12000;
        }
        s->bow = bow;
    }
    s->fresh = 0;

    int ns = OPZ_BLOCK_FRAMES >> m;
    int32_t dD = (s->tD - s->D) >> (6 - m);                              /* per string step */
    int burst = s->t < s->quiet || s->carry, bowing = s->bow > 0;
    if (burst) pass_burst(s, ns);
    if (bowing) { if (burst) pass_bow(s, ns, 1); else pass_bow(s, ns, 0); }
    s->t += ns;
    run_string(s, line, ns, dD, s->mode, burst || bowing);
    s->D = s->tD;

    fract32 mono[OPZ_BLOCK_FRAMES];                                      /* the voice, before level and pan */
    int em = env_step == 0 ? 0 : env_coef != 0x7fffffff ? 2 : stage == 3 ? 1 : 3;
    if (em == 0) OPZ_ADSR_LEVEL(env) = pass_out(mono, s, m, level, env_coef, 0, 0);
    else if (em == 1) OPZ_ADSR_LEVEL(env) = pass_out(mono, s, m, level, env_coef, env_step, 1);
    else if (em == 3) OPZ_ADSR_LEVEL(env) = pass_out(mono, s, m, level, env_coef, env_step, 3);
    else OPZ_ADSR_LEVEL(env) = pass_out(mono, s, m, level, env_coef, env_step, 2);
    opz_mix_mono(out, mono, &s->mix, params);                            /* level and pan, added into out */
}

OPZ_EXPORTS_INSTRUMENT();
