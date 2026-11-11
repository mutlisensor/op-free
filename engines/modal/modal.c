/*
 * modal — struck and bowed modal resonator synth: ten decaying partials per voice whose frequency ratios, weights
 * and decay times morph between six structures, struck by a mallet whose hardness follows the velocity, or bowed
 * while the note is held.
 *
 *   P1        structure   0 string (harmonic 1, 2, 3 ...)  51 marimba (tuned wooden bar 1 : 3.99 : 9.95)
 *                         102 bar (free metal bar 1 : 2.76 : 5.40 : 8.93)  153 membrane (1 : 1.59 : 2.14 : 2.30 ...)
 *                         204 bell (hum 0.5, prime 1, tierce 1.18, quint 1.51, nominal 2 ...)
 *                         255 glass (1 : 2.32 : 4.25 : 6.63, each a pair 0.3 .. 0.5 % apart that beats)
 *                         Between the anchors every mode glides on a log2 line from one ratio to the next, its weight
 *                         and decay are interpolated: turning P1 while notes ring bends the partials smoothly.
 *   P2        damping     0 .. 191    the fundamental's T60 0.08 s -> 20 s (8 octaves, exponential); high-frequency
 *                                     damping falls (a 1 kHz partial decays 3x faster than the base at 0, 1.05x at
 *                                     191) and the mallet's brightness rises one octave either side of 96
 *                         192 .. 255  bow: a sustained, slightly rough excitation feeds every mode while the note is
 *                                     held, from nothing to full; the strike softens to 40 % on the way
 *   velocity              level (linear), mallet hardness (the strike's low-pass corner 600 Hz -> 13.6 kHz at P2 96,
 *                         12 dB per octave), contact time (4 ms -> 0.25 ms), click noise (velocity squared), strike
 *                         position (0.22 -> 0.15 of the length); the bow's level and speed
 *   note-off              the host envelope releases the level; the bow lifts
 *
 * Each mode is a sine from a 256-entry interpolated table, its amplitude an exact exponential per block, ramped
 * linearly per sample. There is no recursion, so no setting can make a mode blow up, and the sum of the amplitudes
 * is capped: the output peak is bounded by construction. Modes above 0.40 fs fade out (gone at 0.45 fs), modes below
 * 30 Hz fade (gone at 15 Hz), and modes that decayed below -90 dBFS are skipped. A released voice that has gone
 * silent is freed at once.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine; modal never reads them.
 *
 * Written from scratch after the modal synthesis literature (Adrien; Cook's PhISEM and modal bars), in the spirit of
 * Mutable Instruments' Rings, Elements and Plaits (their modal resonators are MIT-licensed; no code from them is used
 * here: they run filter banks, this engine runs decaying partials). The exp and division helpers follow engines/pluck
 * (same project, MIT). MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"
#include "modal_tables.h"

#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define NM          MODAL_NMODES
#define P2_KNEE     49152                       /* P2 192 of 256 in opz_p16 units: ring -> bow */
#define LIVE        (1 << 16)                   /* a mode quieter than this (-90 dBFS) is skipped */
#define NYQ_HI      1932735283u                 /* 0.45 fs: modes above are silent */
#define NYQ_LO      1717986918u                 /* 0.40 fs: modes fade out from here */
#define LOW_LO      1460900u                    /* 15 Hz: modes below are silent */
#define LOW_HI      (2u * LOW_LO)               /* 30 Hz: modes fade in up to here */

/* Levels (Q31). The partials' amplitudes sum to at most STRIKE_SUM at full velocity (their sum's peak is lower:
 * calibrated so a voice peaks near OPZ_VOICE_PEAK), the bow to at most BOW_SUM x 1.6 (jitter),
 * the click to CLICK_PEAK: together below 0.8 of full scale, so the per-voice sum never wraps. */
#define STRIKE_SUM  0x29000000                  /* 0.32 FS = 1.28 x OPZ_VOICE_PEAK */
#define BOW_SUM     0x18000000                  /* 0.1875 FS */
#define CLICK_PEAK  0x0c000000                  /* 0.094 FS before the click's low-pass */
_Static_assert((int64_t)STRIKE_SUM + (int64_t)BOW_SUM * 8 / 5 + CLICK_PEAK < 0x6a000000, "voice sum headroom");

typedef struct State {
    uint32_t ph[NM];                            /* phases, Q32 turns */
    int32_t amp[NM];                            /* output amplitude at the end of the last block, Q31 */
    int32_t lev[NM];                            /* mode level, Q31: exact exponential decay per block, bow */
    int32_t dec[NM];                            /* decay multiplier per block, Q31 */
    uint32_t ratio[NM];                         /* frequency ratio, Q16 */
    int16_t jit[NM];                            /* bow roughness, Q15 low-passed noise */
    uint32_t c_inc;                             /* fundamental increment the decays were computed for */
    int32_t c_p1;                               /* P1 the ratios were computed for */
    int32_t c_dp1, c_p2;                        /* P1 and P2 the decays were computed for */
    uint32_t seed;                              /* click and bow noise */
    int32_t ck_env, ck_lp;                      /* click envelope and low-pass state, Q31 */
    int32_t ck_k, ck_dec;                       /* click low-pass coefficient (Q15), decay per block (Q31) */
    int32_t rise, rise_inc;                     /* mallet contact: rise Q15 at the end of the last block, per block */
    OpzMix mix;                                 /* the track's level and pan (opz_mix.h) */
    uint8_t vel;
    uint8_t fresh;                              /* first block after note-on */
    uint8_t c_age;                              /* blocks since the decays were computed */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x6d2b79f5u;          /* note-on noise seeds, one draw per note */
static int32_t s_acc[OPZ_BLOCK_FRAMES + 1];     /* the voice's partials, summed (render runs one voice at a time);
                                                   the last entry is a guard pass_lin reads once */

/* ---- fixed-point helpers ------------------------------------------------------------------------------------ */

#if defined(__bfin__)
/* Fractional a.H x b.L and a.H x b.H -> Q31 (saturating -1 x -1): one instruction each. */
ALWAYS_INLINE int32_t mul_hl(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.H * %2.L;" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
ALWAYS_INLINE int32_t mul_hh(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.H * %2.H;" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
ALWAYS_INLINE int32_t mul_ll(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.L * %2.L;" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
/* a.H + b.H into the low half (16-bit, no saturation; the high half of the result is undefined). */
ALWAYS_INLINE int32_t add_hh(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0.L = %1.H + %2.H (NS);" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
#else
ALWAYS_INLINE int32_t frac16(int32_t a, int32_t b)
{
    int32_t p = (int32_t)(int16_t)a * (int16_t)b;
    return p == 0x40000000 ? 0x7fffffff : p * 2;
}
ALWAYS_INLINE int32_t mul_hl(int32_t a, int32_t b) { return frac16(a >> 16, b); }
ALWAYS_INLINE int32_t mul_hh(int32_t a, int32_t b) { return frac16(a >> 16, b >> 16); }
ALWAYS_INLINE int32_t mul_ll(int32_t a, int32_t b) { return frac16(a, b); }
ALWAYS_INLINE int32_t add_hh(int32_t a, int32_t b) { return (int16_t)((a >> 16) + (b >> 16)); }
#endif

/* n / d (n < 2^31, d > 0), to ~22 bits, without a division (engines/pluck). */
static uint32_t uquot(uint32_t n, uint32_t d)
{
    int nd = opz_norm32((int32_t)d);                                     /* d << nd in [2^30, 2^31) */
    uint32_t r = (uint32_t)opz_recip_q31((int32_t)(d << nd));           /* 2^61 / (d << nd) */
    return opz_mul_u32_shr16(n >> 8, r >> 7) >> (30 - nd);               /* n r / 2^31 = (n / d) 2^(30 - nd) */
}

#define LN2_Q31     1488522236

/* 2^-x for x >= 0 octaves in Q24, Q31 result: the top 6 bits of the fraction from k_exp2n, the rest (< 1/64 octave)
 * by 1 - e + e^2 / 2 (e = rest x ln 2, error < 2e-7 relative), so a decay of 1e-4 per block keeps its rate. */
static int32_t exp2_neg_q31(uint32_t x)
{
    uint32_t i = x >> 24;
    if (i > 30) return 0;
    int32_t e = opz_mul_q31((int32_t)((x & 0x3ffff) << 7), LN2_Q31);
    int32_t p = 0x7fffffff - e + (opz_mul_q31(e, e) >> 1);
    return opz_mul_q31(k_exp2n[(x >> 18) & 63], p) >> i;
}

/* 2^31 / d for 2^16 <= d < 2^31, to 0.4 %: a linear first guess and one Newton step (cf. opz_recip_q31). For gains. */
static int32_t inv_fast(uint32_t d)
{
    int n = opz_norm32((int32_t)d);
    int32_t m = (int32_t)(d << n);                                       /* [2^30, 2^31) */
    int32_t h = 884257973 + (0x7fffffff - opz_mul_q31(2021161080, m));
    int32_t e = (int32_t)(0x80000000u - ((uint32_t)opz_mul_q31(m, h) << 1));
    h = opz_add_q31(h, opz_mul_q31(h, e));                               /* 2^61 / m */
    return h >> (30 - n);
}

/* 2^(x / 65536) in Q16, as an unsigned word: x from -17 to 15 octaves (0 below, 0xffffffff above). */
static uint32_t pow2(int32_t x)
{
    int32_t i = x >> 16;
    if (i >= 16) return 0xffffffffu;
    if (i < -17) return 0;
    uint32_t p = (uint32_t)opz_exp2_q16(x & 0xffff);                     /* 65536 .. 131071 */
    return i >= 0 ? p << i : p >> -i;
}

/* ---- the structure: P1 -> zone and position between two anchors ------------------------------------------ */

typedef struct Zone {
    int z;                                      /* anchor below */
    int32_t t;                                  /* 0 .. 65535 towards anchor z + 1 */
} Zone;

static Zone zone(int32_t p1)
{
    uint32_t u = (uint32_t)p1 * (MODAL_ANCHORS - 1);
    Zone Z = { (int)(u >> 16), (int32_t)(u & 0xffff) };
    if (Z.z >= MODAL_ANCHORS - 1) { Z.z = MODAL_ANCHORS - 2; Z.t = 65535; }
    return Z;
}

static inline int32_t lerp16(int32_t a, int32_t b, int32_t t) { return a + ((b - a) * (t >> 1) >> 15); }

static uint32_t mode_ratio(const Zone *Z, int k)
{
    int32_t dl = k_dlog[Z->z][k];
    if (dl == 0 || Z->t == 0) return k_ratio[Z->z][k];
    int32_t e = (int32_t)opz_mul_u32_shr16((uint32_t)(dl < 0 ? -dl : dl), (uint32_t)Z->t);
    return opz_mul_u32_shr16(k_ratio[Z->z][k], (uint32_t)opz_exp2_q16(dl < 0 ? -e : e));
}

static inline int32_t mode_weight(const Zone *Z, int k)
{
    return lerp16(k_weight[Z->z][k], k_weight[Z->z + 1][k], Z->t);
}

/* ---- per block: increments and band gains ----------------------------------------------------------------- */

/* The mode's increment at pitch pm (0 above the band) and its band gain, Q15: 0 above 0.45 fs and below 15 Hz, linear
 * fades from 0.40 fs and up to 30 Hz. */
ALWAYS_INLINE uint32_t mode_inc(uint32_t pm, uint32_t ratio, int32_t *g)
{
    if (opz_mul_u16(pm >> 16, ratio >> 8) >= (NYQ_HI >> 8)) { *g = 0; return 0; }   /* <= inc / 256: no overflow below */
    uint32_t inc = opz_mul_u32_shr16(pm, ratio);
    int32_t gain = 32767;
    if (inc >= NYQ_HI) gain = 0;
    else if (inc > NYQ_LO) gain = (int32_t)(((NYQ_HI - inc) >> 13) * 5 >> 2);       /* span 0.05 fs */
    if (inc <= LOW_LO) gain = 0;
    else if (inc < LOW_HI) gain = (int32_t)((((inc - LOW_LO) >> 6) * 47038u) >> 15);  /* span 15 Hz */
    *g = gain > 32767 ? 32767 : gain;
    return inc;
}

/* Decay multipliers per block from P2, the structure and the pitch. The base T60 is 0.08 s x 2^(8 P2 / 192) (20.5 s
 * at the knee); each mode's T60 is that times its structure factor, divided by 1 + h f (f in kHz; h 2 at P2 0 ->
 * 0.05 at 191: absolute damping darkens the tail). */
__attribute__((noinline)) static void decays(State *s, const Zone *Z, int32_t p2, const uint32_t *inc)
{
    int32_t tb = p2 < P2_KNEE ? p2 : P2_KNEE;                          /* 0 .. 49152 */
    int32_t lgT0 = -238813 + (int32_t)opz_mul_u32_shr16((uint32_t)tb, 699051u); /* log2 T60: -3.644 + 8 tb / 49152, Q16 */
    int32_t lgh = 65536 - (int32_t)opz_mul_u32_shr16((uint32_t)tb, 463393u);   /* log2 h: 1 -> -4.3 */
    uint32_t h = pow2(lgh);                                              /* Q16 per kHz */
    for (int k = 0; k < NM; k++) {
        int32_t ld = lerp16(k_ldecay[Z->z][k], k_ldecay[Z->z + 1][k], Z->t) * 16;   /* Q16 */
        /* octaves per block: 64 x log2(1000) / (44100 T60) = 2^(-6.111 - log2 T60), Q24 */
        uint32_t xb = pow2(-400483 - lgT0 - ld + (8 << 16));
        uint32_t fk = opz_mul_u32_shr16(inc[k] >> 8, 11290);           /* kHz, Q16 */
        uint32_t damp = 65536 + opz_mul_u32_shr16(h, fk);               /* Q16 */
        uint32_t x = opz_mul_u32_shr16(xb, damp);
        if (xb > (31u << 24) || x > (31u << 24)) x = 31u << 24;
        s->dec[k] = exp2_neg_q31(x);
    }
}

/* ---- per note: the strike ---------------------------------------------------------------------------------- */

__attribute__((noinline)) static void strike(State *s, const Zone *Z, int32_t p2, uint32_t pm, const int32_t *g)
{
    int32_t vq = s->vel * 258;                                           /* Q15, 127 -> 32766 */
    int32_t tb = p2 < P2_KNEE ? p2 : P2_KNEE;
    int32_t u = p2 > P2_KNEE ? (p2 - P2_KNEE) * 2 : 0;                   /* bow amount, Q15 */

    /* mallet corner fc = 600 Hz x 2^(4.5 v) x 2^(2 tb - 1), as an increment: log2(600 / 44100 x 2^32) = 25.80 */
    int32_t lgc = 1690763 + vq * 9 - 65536 + (int32_t)opz_mul_u32_shr16((uint32_t)tb, 174763u);
    uint32_t inc_c = pow2(lgc - (16 << 16));
    if (inc_c > 0x7fffffffu) inc_c = 0x7fffffffu;
    if (inc_c < (1u << 16)) inc_c = 1u << 16;

    /* strike position 0.22 -> 0.15 of the length (Q16): each mode's weight times 0.4 + 0.6 |sin(pi beta r)| */
    uint32_t beta = 14418 - ((uint32_t)(vq * 4588) >> 15);
    int32_t c[NM], S = 0;
    for (int k = 0; k < NM; k++) {
        uint32_t ph = opz_mul_u32_shr16(beta, s->ratio[k]) << 15;        /* beta r / 2 turns */
        int32_t e = k_sin[ph >> 24];
        int32_t sn = (int16_t)add_hh(e, mul_ll(e, (int32_t)(ph >> 9)));  /* sin(2 pi ph), Q15 */
        int32_t pos = 13107 + ((sn < 0 ? -sn : sn) * 19661 >> 15);
        c[k] = (mode_weight(Z, k) * pos >> 15) * g[k] >> 15;
        S += c[k];
    }
    int32_t total = opz_mul_q31_q15(opz_mul_q31_q15(STRIKE_SUM, vq), 32767 - (u * 19661 >> 15));
    uint32_t r = S > 0 ? uquot((uint32_t)total, (uint32_t)S) : 0;          /* per unit of c; c[k] hard <= S */
    uint32_t q = uquot(pm, inc_c >> 16);                                /* f0 / fc, Q16 */
    for (int k = 0; k < NM; k++) {
        int32_t a = 0;
        if (S > 0 && c[k] > 0) {
            uint32_t x = opz_mul_u32_shr16(s->ratio[k], q);              /* f / fc, Q16 */
            if (x > (1u << 22)) x = 1u << 22;
            int32_t hard = inv_fast(65536 + opz_mul_u32_shr16(x, x));    /* 1 / (1 + x^2), Q15 */
            if (hard > 32767) hard = 32767;
            a = (int32_t)((uint32_t)(c[k] * hard >> 15) * r);
        }
        s->lev[k] = a;
        s->ph[k] = 0;
    }

    /* contact time Tc = 176 x 2^(-4 v) samples (4 ms -> 0.25 ms): the partials rise over it; shorter than a block,
     * the first block starts at 1 - Tc / 64 */
    int32_t tc = (int32_t)(pow2(488862 - vq * 8) >> 16);
    if (tc < 1) tc = 1;
    s->rise_inc = (int32_t)uquot(64u << 15, (uint32_t)tc);               /* Q15 per block */
    s->rise = tc < 64 ? 32767 - tc * 512 : 0;
    for (int k = 0; k < NM; k++) s->amp[k] = opz_mul_q31_q15(opz_mul_q31_q15(s->lev[k], g[k]), s->rise);

    /* click: noise through a one-pole at fc, decaying with a time constant of 8 + Tc / 2 samples */
    int32_t camt = lerp16(k_click[Z->z], k_click[Z->z + 1], Z->t);
    s->ck_env = opz_mul_q31_q15(opz_mul_q31_q15(CLICK_PEAK, camt), vq * vq >> 15);
    uint32_t kk = opz_mul_u32_shr16(inc_c >> 16, 205887);               /* 2 pi fc / fs, Q15 */
    s->ck_k = kk > 29491 ? 29491 : (int32_t)kk;
    s->ck_dec = exp2_neg_q31(uquot(1549082005u, (uint32_t)(8 + tc / 2)));   /* per block: 64 / (tau ln 2) octaves */
    s->ck_lp = 0;
}

/* ---- exports ------------------------------------------------------------------------------------------------ */

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;                                                     /* instruments get none, and need none */
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note;                                                          /* the pitch and knobs come with render */
    State *s = state;
    for (int k = 0; k < NM; k++) {
        s->amp[k] = s->lev[k] = 0;
        s->jit[k] = 0;
    }
    s->ck_env = s->ck_lp = 0;
    s_seed = s_seed * 1664525u + 1013904223u;
    s->seed = s_seed;
    s->vel = (uint8_t)(velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
    s->fresh = 1;
    if (params) opz_mix_start(&s->mix, params);                          /* NULL: ramps in at render */
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                             /* the host releases the envelope */
}

/* One partial into s_acc: a sine from the interpolated table (one load: value and step; the fraction is the signed
 * low half of ph >> 9, see modal_tables.h), its amplitude ramped by astep per sample. first: store instead of add.
 * On the Blackfin the body is scheduled by hand: the table and accumulator loads and the store issue in parallel
 * with arithmetic, 10 cycles per sample instead of GCC's 13. The C below is the same arithmetic, bit for bit. */
#if defined(__bfin__) && !defined(MODAL_C_LOOP)
ALWAYS_INLINE void mode_pass(uint32_t *php, uint32_t inc, int32_t *ampp, int32_t astep, int first)
{
    uint32_t ph = *php;
    int32_t amp = *ampp;
    int32_t *p = s_acc;
    int32_t t0, t1, a;
    const int32_t *pt;
    if (first)
        __asm__ volatile("LSETUP (1f, 2f) LC0 = %[n];\n"
                         "1: %[t0] = %[ph] >> 24;\n\t"
                         "%[pt] = %[t0];\n\t"
                         "%[t1] = %[ph] >> 9;\n\t"
                         "%[pt] = %[tab] + (%[pt] << 2);\n\t"
                         "%[ph] = %[ph] + %[inc] (NS) || %[t0] = [%[pt]] || NOP;\n\t"
                         "%[t1] = %[t0].L * %[t1].L;\n\t"
                         "%[t0].L = %[t0].H + %[t1].H (NS);\n\t"
                         "%[t0] = %[amp].H * %[t0].L;\n"
                         "2: %[amp] = %[amp] + %[st] (NS) || [%[p]++] = %[t0] || NOP;"
                         : [ph] "+d"(ph), [amp] "+d"(amp), [p] "+a"(p), [t0] "=&d"(t0), [t1] "=&d"(t1),
                           [pt] "=&a"(pt)
                         : [inc] "d"(inc), [st] "d"(astep), [tab] "a"(k_sin), [n] "a"(OPZ_BLOCK_FRAMES)
                         : "memory", "LC0", "LT0", "LB0");
    else
        __asm__ volatile("LSETUP (1f, 2f) LC0 = %[n];\n"
                         "1: %[t0] = %[ph] >> 24 || %[a] = [%[p]] || NOP;\n\t"
                         "%[pt] = %[t0];\n\t"
                         "%[t1] = %[ph] >> 9;\n\t"
                         "%[pt] = %[tab] + (%[pt] << 2);\n\t"
                         "%[ph] = %[ph] + %[inc] (NS) || %[t0] = [%[pt]] || NOP;\n\t"
                         "%[t1] = %[t0].L * %[t1].L;\n\t"
                         "%[t0].L = %[t0].H + %[t1].H (NS);\n\t"
                         "%[t0] = %[amp].H * %[t0].L;\n\t"
                         "%[a] = %[a] + %[t0] (NS);\n"
                         "2: %[amp] = %[amp] + %[st] (NS) || [%[p]++] = %[a] || NOP;"
                         : [ph] "+d"(ph), [amp] "+d"(amp), [p] "+a"(p), [t0] "=&d"(t0), [t1] "=&d"(t1),
                           [a] "=&d"(a), [pt] "=&a"(pt)
                         : [inc] "d"(inc), [st] "d"(astep), [tab] "a"(k_sin), [n] "a"(OPZ_BLOCK_FRAMES)
                         : "memory", "LC0", "LT0", "LB0");
    *php = ph;
    *ampp = amp;
}
#else
ALWAYS_INLINE void mode_pass(uint32_t *php, uint32_t inc, int32_t *ampp, int32_t astep, int first)
{
    uint32_t ph = *php;
    int32_t amp = *ampp;
    int32_t *acc = s_acc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t e = k_sin[ph >> 24];
        int32_t sv = add_hh(e, mul_ll(e, (int32_t)(ph >> 9)));          /* low half: sin, Q15 */
        int32_t v = mul_hl(amp, sv);
        acc[i] = first ? v : acc[i] + v;
        ph += inc;
        amp += astep;
    }
    *php = ph;
    *ampp = amp;
}
#endif

/* The mallet click into s_acc: noise, one-pole low-pass, exponential decay (exact per block, linear in between). */
static void click_pass(State *s, int first)
{
    uint32_t seed = s->seed;
    int32_t env = s->ck_env, lp = s->ck_lp, k = s->ck_k;
    int32_t end = opz_mul_q31(env, s->ck_dec), est = (end - env) >> 6;
    int32_t *acc = s_acc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        seed = seed * 1664525u + 1013904223u;
        lp += mul_hl(mul_hh((int32_t)seed, env) - lp, k);
        acc[i] = first ? lp : acc[i] + lp;
        env += est;
    }
    s->seed = seed;
    s->ck_env = end < LIVE ? 0 : end;
    s->ck_lp = end < LIVE ? 0 : lp;
}

/* s_acc x the envelope into the mono block buffer x (opz_mix_mono then adds it into out with the track's level and
 * pan), the level ramped by ls per sample (the decay, release and sustain stages: see render). On the Blackfin the
 * loop is scheduled by hand, 2 instructions per frame (the multiply reads x before the parallel load replaces it, as
 * in the classic Blackfin FIR loop); the C below is the same arithmetic, bit for bit. Reads s_acc[64] (the guard)
 * once. Returns the level. */
#if defined(__bfin__) && !defined(MODAL_C_LOOP)
ALWAYS_INLINE int32_t pass_lin(fract32 *x, int32_t level, int32_t ls)
{
    const int32_t *acc = s_acc;
    fract32 *w = x;
    int32_t a, v;
    __asm__ volatile("%[a] = [%[acc]++];\n\t"
                     "LSETUP (1f, 2f) LC0 = %[n];\n"
                     "1: %[v] = %[a].H * %[lev].H || %[a] = [%[acc]++] || NOP;\n"
                     "2: %[lev] = %[lev] + %[ls] (S) || [%[w]++] = %[v] || NOP;"
                     : [lev] "+d"(level), [acc] "+a"(acc), [w] "+a"(w), [a] "=&d"(a), [v] "=&d"(v)
                     : [ls] "d"(ls), [n] "a"(OPZ_BLOCK_FRAMES)
                     : "memory", "LC0", "LT0", "LB0");
    return level;
}
#else
ALWAYS_INLINE int32_t pass_lin(fract32 *x, int32_t level, int32_t ls)
{
    const int32_t *acc = s_acc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        x[i] = mul_hh(acc[i], level);
        level = opz_add_q31(level, ls);
    }
    return level;
}
#endif

/* The same with the host envelope's exact per-sample step, for the fade of a stolen voice (a few blocks). */
static int32_t pass_out(fract32 *x, int32_t level, int32_t coef, int32_t step)
{
    const int32_t *acc = s_acc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        x[i] = mul_hh(acc[i], level);
        level = opz_env_tick(level, coef, step);
    }
    return level;
}

/* The envelope's step per sample at coef 1: (1 - level^3) s. */
static inline int32_t env_slope(int32_t level, int32_t step)
{
    return opz_mul_q31_rnd(opz_sub_q31(0x7fffffff, opz_mul_q31_rnd(opz_mul_q31_rnd(level, level), level)), step);
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);
    int32_t stage = OPZ_ADSR_STAGE(env);
    if (stage == 0) return;

    int32_t p1 = opz_p16(params[OPZ_P_P1]), p2 = opz_p16(params[OPZ_P_P2]);
    if (p1 > 65535) p1 = 65535;                                          /* the host's words are below 2^31 */
    if (p2 > 65535) p2 = 65535;
    uint32_t pm = pitch + (uint32_t)pitchSlope * (OPZ_BLOCK_FRAMES / 2);  /* the block's mid-point pitch */
    Zone Z = zone(p1);
    int fresh = s->fresh;

    /* ratios when P1 moves; increments and band gains every block (vibrato, bends and glides follow at block rate) */
    if (fresh || p1 != s->c_p1)
        for (int k = 0; k < NM; k++) s->ratio[k] = mode_ratio(&Z, k);
    uint32_t inc[NM];
    int32_t g[NM];
    for (int k = 0; k < NM; k++) inc[k] = mode_inc(pm, s->ratio[k], &g[k]);

    /* decays when the pitch has drifted 3 % from where they were computed, or P1 or P2 have moved (then at most every
     * other block: an LFO on P1 or P2 moves them every block, and half the rate costs half) */
    uint32_t drift = pm > s->c_inc ? pm - s->c_inc : s->c_inc - pm;
    if (s->c_age < 255) s->c_age++;
    if (fresh) {                                                         /* the strike; the decays wait one block */
        strike(s, &Z, p2, pm, g);
        for (int k = 0; k < NM; k++) s->dec[k] = 0x7fffffff;             /* (1.45 ms: keeps the first block light) */
        s->c_inc = 0;
        s->fresh = 0;
    } else if (drift > s->c_inc >> 5 || ((p1 != s->c_dp1 || p2 != s->c_p2) && s->c_age >= 2)) {
        decays(s, &Z, p2, inc);
        s->c_inc = pm;
        s->c_dp1 = p1;
        s->c_p2 = p2;
        s->c_age = 0;
    }
    s->c_p1 = p1;

    /* contact rise */
    int32_t rise = s->rise + s->rise_inc;
    if (rise > 32767) rise = 32767;
    s->rise = rise;

    /* bow: targets per mode from the structure's weights squared (a darker spectrum than the strike), normalised to
     * BOW_SUM; each roughened by its own low-passed noise (x 0.4 .. 1.6), approached at 1.2 .. 3.6 % per block */
    int32_t u = (!fresh && stage < 4 && p2 > P2_KNEE) ? (p2 - P2_KNEE) * 2 : 0;     /* from the second block */
    int32_t B[NM], rho = 0;
    if (u) {
        int32_t S = 0;
        for (int k = 0; k < NM; k++) {
            int32_t w = mode_weight(&Z, k);
            B[k] = (w * w >> 15) * g[k] >> 15;
            S += B[k];
        }
        int32_t vq = s->vel * 258;
        int32_t total = opz_mul_q31_q15(opz_mul_q31_q15(BOW_SUM, u), 16384 + (vq >> 1));
        uint32_t r = S > 0 ? uquot((uint32_t)total, (uint32_t)S) : 0;     /* per unit of weight; B[k] <= S */
        uint32_t seed = s->seed;
        for (int k = 0; k < NM; k++) {
            seed = seed * 1664525u + 1013904223u;
            s->jit[k] = (int16_t)(s->jit[k] + ((((int32_t)seed >> 16) - s->jit[k]) >> 2));
            int32_t b = (int32_t)((uint32_t)B[k] * r);
            B[k] = b + opz_mul_q31_q15(b, (s->jit[k] * 19661) >> 15);    /* x (1 + 0.6 jit) */
        }
        s->seed = seed;
        rho = 393 + ((vq * 786) >> 15);
    }

    /* the partials */
    int first = 1;
    for (int k = 0; k < NM; k++) {
        int32_t lev = opz_mul_q31(s->lev[k], s->dec[k]);
        if (u && lev < B[k]) lev += opz_mul_q31_q15(B[k] - lev, rho);
        if (lev < LIVE) lev = 0;
        s->lev[k] = lev;
        int32_t tgt = opz_mul_q31_q15(lev, g[k] * rise >> 15);
        int32_t a = s->amp[k];
        if (a < LIVE && tgt < LIVE) {                                    /* inaudible or out of the band: skipped */
            s->amp[k] = 0;
            continue;
        }
        int32_t astep = (tgt - a) >> 6;
        if (first) mode_pass(&s->ph[k], inc[k], &s->amp[k], astep, 1);
        else mode_pass(&s->ph[k], inc[k], &s->amp[k], astep, 0);
        first = 0;
    }
    if (s->ck_env) {
        if (first) click_pass(s, 1);
        else click_pass(s, 0);
        first = 0;
    }

    if (first) {                                                         /* silent */
        if (stage >= 4) {                                                /* released or fading: free the voice */
            OPZ_ADSR_LEVEL(env) = 0;
            OPZ_ADSR_STAGE(env) = 0;
        }
        return;
    }

    int32_t level = OPZ_ADSR_LEVEL(env), env_coef = OPZ_ADSR_COEF(env), env_step = OPZ_ADSR_STEP(env);
    fract32 mono[OPZ_BLOCK_FRAMES];                                      /* the voice, before level and pan */
    /* The envelope, which the host only reads at the block's end. Decay and release (step 0) fall by coef per sample:
     * here exactly per block (coef^64 by six squarings), linear in between. Sustain (coef 1): its step
     * (1 - level^3) s once per block (the host recomputes s = (S - level) / 256 every block). Attack (coef 1): the
     * same step in two half-block Euler steps (close to 64 exact steps on slow attacks; a fast one saturates at full
     * level within the block, as the exact steps do), linear in between. The fade of a stolen voice: the exact
     * per-sample step. */
    if (env_step == 0) {
        int32_t c = env_coef;
        for (int i = 0; i < 6; i++) c = opz_mul_q31(c, c);
        OPZ_ADSR_LEVEL(env) = pass_lin(mono, level, (opz_mul_q31(level, c) - level) >> 6);
    } else if (env_coef == 0x7fffffff) {
        int32_t ls = env_slope(level, env_step);
        if (stage != 3) {                                                /* two half-block steps */
            int32_t mid = opz_add_q31(level, opz_shl_q31(ls, 5));
            int32_t end = opz_add_q31(mid, opz_shl_q31(env_slope(mid, env_step), 5));
            ls = opz_sub_q31(end, level) >> 6;
        }
        OPZ_ADSR_LEVEL(env) = pass_lin(mono, level, ls);
    } else {
        OPZ_ADSR_LEVEL(env) = pass_out(mono, level, env_coef, env_step);
    }
    opz_mix_mono(out, mono, &s->mix, params);                            /* level and pan, added into out */
}

OPZ_EXPORTS_INSTRUMENT();
