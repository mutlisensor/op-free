/*
 * mallet — modal percussion for the kick, snare, perc and sample tracks: marimba, xylophone, vibes, glockenspiel,
 * steel drum, bells, gongs, glass and wood, as ten decaying partials per hit struck by a mallet whose hardness
 * follows the velocity.
 *
 *   keys      two chromatic octaves: the 24 keys play MIDI notes 53 .. 76 (F3 .. E5), the pitch of the hit is the
 *             note itself, so each octave press on a drum track (+-24 notes) moves the keyboard by exactly its span
 *   P1        material    a continuous morph through nine materials, 32 apart: 0 wood, 32 marimba, 64 xylophone,
 *                         96 vibes, 128 glock, 159 steel drum, 191 bell, 223 gong, 255 glass. Between two materials
 *                         every partial glides on a log2 line from one ratio to the next, its weight, its decay and
 *                         the material's ring, damping, mallet and click are interpolated
 *   P2        ring        0 dead -> 128 the material's natural ring -> 255 long: the fundamental's T60 x 1/16 .. x1
 *                         .. x8, the high-frequency damping x4 .. x1/4 and the mallet's brightness -1 .. +1 octave
 *   velocity              level (linear), mallet hardness (the strike's low-pass corner 500 Hz -> 16 kHz before the
 *                         material's and P2's offsets, 12 dB per octave), contact time (4 ms -> 0.25 ms), click noise
 *                         (velocity squared), strike position (0.22 -> 0.15 of the length)
 *
 * The knobs are read when a hit starts (as kit does): a hit keeps its sound while the knobs move, and nothing is
 * recomputed while it rings. Each mode is a sine from a 256-entry interpolated table, its amplitude an exact
 * exponential per block, ramped linearly per sample. There is no recursion, so no setting can make a mode blow up,
 * and the strike's amplitudes sum to a fixed bound: the output peak is bounded by construction. Modes above 0.40 fs
 * fade out (gone at 0.45 fs), modes below 30 Hz fade (gone at 15 Hz), modes that decayed below -90 dBFS are skipped
 * and cost nothing, and the hit frees its voice once its partials sum below -78 dBFS. The OP-Z envelope is not used
 * (one-shots, as kit); a voice the OS steals fades out in 2.9 ms.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine; mallet never reads them.
 *
 * Written from scratch after the modal synthesis literature (Adrien; Cook's modal bars; mode ratios from Fletcher
 * and Rossing), in the spirit of Mutable Instruments' Rings, Elements and Plaits (their modal resonators are
 * MIT-licensed; no code from them is used here: they run filter banks, this engine runs decaying partials). The
 * partial and output loops, the sine table and the exp helpers are engines/modal's (same project, MIT).
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "mallet_tables.h"

#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define NM          MALLET_NMODES
#define LIVE        (1 << 16)                   /* a mode quieter than this (-90 dBFS) is skipped */
#define END_SUM     (1 << 18)                   /* the hit is over when its modes sum below this (-78 dBFS) */
#define NYQ_HI      1932735283u                 /* 0.45 fs: modes above are silent */
#define NYQ_LO      1717986918u                 /* 0.40 fs: modes fade out from here */
#define LOW_LO      1460900u                    /* 15 Hz: modes below are silent */
#define LOW_HI      (2u * LOW_LO)               /* 30 Hz: modes fade in up to here */
#define LG_500HZ    1673612                     /* log2 of 500 Hz as an increment (500 / 44100 x 2^32), Q16 */
#define LGT_MIN     (-418174)                   /* log2 of the shortest fundamental T60, 12 ms, Q16 */
#define LGT_MAX     (5 << 16)                   /* and of the longest, 32 s */
#define INC_C_MIN   (1u << 22)                  /* the mallet corner's lowest increment, 43 Hz: keeps f / fc < 2^16 */

/* Levels (Q31). The partials' amplitudes sum to at most STRIKE_SUM x the material's trim (<= 1.5) at full velocity
 * (their sum's peak is lower: the modes start at phase 0 and drift apart), the click to CLICK_PEAK: together below
 * 0.6 of full scale, so the per-voice sum (added without saturation) never wraps. */
#define STRIKE_SUM  0x29000000                  /* 0.32 FS = 1.28 x OPZ_VOICE_PEAK */
#define CLICK_PEAK  0x0c000000                  /* 0.094 FS before the click's low-pass */
_Static_assert((int64_t)STRIKE_SUM * 3 / 2 + CLICK_PEAK < 0x4c000000, "voice sum headroom");

enum { PH_OFF = 0, PH_STRIKE = 1, PH_DECAY = 2, PH_RING = 3 };

typedef struct State {
    uint32_t ph[NM];                            /* phases, Q32 turns */
    uint32_t inc[NM];                           /* phase increments, 0 for a silent mode */
    int32_t amp[NM];                            /* amplitude at the end of the last block, Q31 */
    int32_t dec[NM];                            /* decay multiplier per block, Q31 */
    uint32_t seed;                              /* click noise */
    int32_t ck_env, ck_lp;                      /* click envelope and low-pass state, Q31 */
    int32_t ck_k, ck_dec;                       /* click low-pass coefficient (Q15), decay per block (Q31) */
    int32_t gain;                               /* output level, Q31: the contact rise, then 1; the steal fade */
    int32_t gain_inc;                           /* per block during the rise, Q31 */
    int32_t fade;                               /* per sample while the voice fades out (stolen), 0 otherwise */
    uint16_t p1, p2;                            /* the knobs at the hit, 0 .. 65535 */
    uint8_t note, vel, phase;
} State;

/* The OS's voice record leaves 280 bytes in practice (hardware, 2026-10-19), not the 308 of OPZ_VOICE_STATE_BYTES. */
_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x6d2b79f5u;          /* note-on noise seeds, one draw per hit */
static int32_t s_acc[OPZ_BLOCK_FRAMES + 1];     /* the voice's partials, summed (render runs one voice at a time);
                                                   the last entry is a guard pass_lin reads once */

/* ---- fixed-point helpers (engines/modal) -------------------------------------------------------------------- */

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

/* MIDI note 0 .. 127 -> phase increment (exact to the table's rounding; n / 12 by a multiply, exact below 128). */
static uint32_t note_inc(int n)
{
    int oct = (n * 2731) >> 15;
    return k_note_inc[n - 12 * oct] >> (10 - oct);
}

/* ---- the material: P1 -> zone and position between two anchors ------------------------------------------- */

typedef struct Zone {
    int z;                                      /* material below */
    int32_t t;                                  /* 0 .. 65535 towards material z + 1 */
} Zone;

static Zone zone(int32_t p1)
{
    uint32_t u = (uint32_t)p1 * (MALLET_MATERIALS - 1);
    Zone Z = { (int)(u >> 16), (int32_t)(u & 0xffff) };
    if (Z.z >= MALLET_MATERIALS - 1) { Z.z = MALLET_MATERIALS - 2; Z.t = 65535; }
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

/* The mode's increment at pitch pm (0 above the band) and its band gain, Q15: 0 above 0.45 fs and below 15 Hz, linear
 * fades from 0.40 fs and up to 30 Hz. */
static uint32_t mode_inc(uint32_t pm, uint32_t ratio, int32_t *g)
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

/* ---- per hit: the strike (first block) and the decays (second block) ----------------------------------------- */

__attribute__((noinline)) static void strike(State *s)
{
    Zone Z = zone(s->p1);
    int32_t q = s->p2;
    int32_t vq = s->vel * 258;                                           /* Q15, 127 -> 32766 */
    uint32_t pm = note_inc(s->note);

    /* mallet corner fc = 500 Hz x 2^(5 v) x 2^hardness x 2^(2 q - 1), as an increment */
    int32_t hard = lerp16(k_mat[Z.z].hard, k_mat[Z.z + 1].hard, Z.t) * 16;           /* octaves, Q16 */
    int32_t lgc = LG_500HZ + vq * 10 + hard + q * 2 - 65536;
    uint32_t inc_c = pow2(lgc - (16 << 16));
    if (inc_c > 0x7fffffffu) inc_c = 0x7fffffffu;
    if (inc_c < INC_C_MIN) inc_c = INC_C_MIN;

    /* strike position 0.22 -> 0.15 of the length (Q16): each mode's weight times 0.4 + 0.6 |sin(pi beta r)| */
    uint32_t beta = 14418 - ((uint32_t)(vq * 4588) >> 15);
    int32_t c[NM], S = 0;
    uint32_t ratio[NM];
    for (int k = 0; k < NM; k++) {
        int32_t g;
        ratio[k] = mode_ratio(&Z, k);
        s->inc[k] = mode_inc(pm, ratio[k], &g);
        s->ph[k] = 0;
        s->dec[k] = 0x7fffffff;                                          /* the decays come in the second block */
        uint32_t ph = opz_mul_u32_shr16(beta, ratio[k]) << 15;           /* beta r / 2 turns */
        int32_t e = k_sin[ph >> 24];
        int32_t sn = (int16_t)add_hh(e, mul_ll(e, (int32_t)(ph >> 9)));  /* sin(2 pi ph), Q15 */
        int32_t pos = 13107 + ((sn < 0 ? -sn : sn) * 19661 >> 15);
        int32_t w = lerp16(k_weight[Z.z][k], k_weight[Z.z + 1][k], Z.t);
        c[k] = (w * pos >> 15) * g >> 15;
        S += c[k];
    }
    int32_t trim = lerp16(k_mat[Z.z].trim, k_mat[Z.z + 1].trim, Z.t);            /* Q14, <= 1.5 */
    int32_t total = opz_mul_q31_q15(opz_mul_q31_q15(STRIKE_SUM, vq), trim) << 1;
    uint32_t r = S > 0 ? uquot((uint32_t)total, (uint32_t)S) : 0;          /* per unit of c; c[k] <= S */
    uint32_t qf = uquot(pm, inc_c >> 16);                               /* f0 / fc, Q16 (< 2^26) */
    for (int k = 0; k < NM; k++) {
        int32_t a = 0;
        if (c[k] > 0) {
            uint32_t x = opz_mul_u32_shr16(ratio[k], qf);                /* f / fc, Q16 (< 2^32: ratio < 2^22) */
            if (x > (1u << 22)) x = 1u << 22;
            int32_t h = inv_fast(65536 + opz_mul_u32_shr16(x, x));       /* 1 / (1 + x^2), Q15 */
            if (h > 32767) h = 32767;
            a = (int32_t)((uint32_t)(c[k] * h >> 15) * r);
        }
        if (a < LIVE) {                                                  /* never audible: never rendered */
            a = 0;
            s->inc[k] = 0;
        }
        s->amp[k] = a;
    }

    /* contact time Tc = 176 x 2^(-4 v) samples (4 ms -> 0.25 ms): the output rises linearly over it; shorter than a
     * block, the first block starts at 1 - Tc / 64 */
    int32_t tc = (int32_t)(pow2(488862 - vq * 8) >> 16);
    if (tc < 1) tc = 1;
    uint32_t ri = uquot(64u << 15, (uint32_t)tc);                        /* Q15 per block */
    s->gain_inc = ri > 32767 ? 0x7fffffff : (int32_t)(ri << 16);
    s->gain = tc < 64 ? 0x7fffffff - tc * (1 << 25) : 0;

    /* click: noise through a one-pole at fc, decaying with a time constant of 8 + Tc / 2 samples */
    int32_t camt = lerp16(k_mat[Z.z].click, k_mat[Z.z + 1].click, Z.t);
    s->ck_env = opz_mul_q31_q15(opz_mul_q31_q15(CLICK_PEAK, camt), vq * vq >> 15);
    uint32_t kk = opz_mul_u32_shr16(inc_c >> 16, 205887);               /* 2 pi fc / fs, Q15 */
    s->ck_k = kk > 29491 ? 29491 : (int32_t)kk;
    s->ck_dec = exp2_neg_q31(uquot(1549082005u, (uint32_t)(8 + tc / 2)));   /* per block: 64 / (tau ln 2) octaves */
    s->ck_lp = 0;
}

/* Decay multipliers per block from the material and P2. The fundamental's T60 is the material's natural ring x
 * 2^(-4 .. 0 .. 3) (P2 0, 128, 255; 12 ms .. 32 s); each mode's T60 is that times its relative decay, divided by
 * 1 + h f (f in kHz; h = the material's damping x 2^(2 - 4 q): absolute damping darkens the tail, more at low P2). */
__attribute__((noinline)) static void decays(State *s)
{
    Zone Z = zone(s->p1);
    int32_t q = s->p2;
    int32_t off = q < 32768 ? q * 8 - (4 << 16) : q * 6 - (3 << 16);    /* octaves, Q16 */
    int32_t lgT0 = lerp16(k_mat[Z.z].lgt, k_mat[Z.z + 1].lgt, Z.t) * 16 + off;
    if (lgT0 < LGT_MIN) lgT0 = LGT_MIN;
    if (lgT0 > LGT_MAX) lgT0 = LGT_MAX;
    int32_t lgh = lerp16(k_mat[Z.z].lgh, k_mat[Z.z + 1].lgh, Z.t) * 16 + (32768 - q) * 4;
    uint32_t h = pow2(lgh);                                              /* Q16 per kHz */
    for (int k = 0; k < NM; k++) {
        if (!s->amp[k]) continue;                                        /* silent modes stay silent */
        int32_t ld = lerp16(k_ldecay[Z.z][k], k_ldecay[Z.z + 1][k], Z.t) * 16;   /* Q16 */
        /* octaves per block: 64 x log2(1000) / (44100 T60) = 2^(-6.111 - log2 T60), Q24 */
        uint32_t xb = pow2(-400483 - lgT0 - ld + (8 << 16));
        uint32_t fk = opz_mul_u32_shr16(s->inc[k] >> 8, 11290);         /* kHz, Q16 */
        uint32_t damp = 65536 + opz_mul_u32_shr16(h, fk);               /* Q16 */
        uint32_t x = opz_mul_u32_shr16(xb, damp);
        if (xb > (31u << 24) || x > (31u << 24)) x = 31u << 24;
        s->dec[k] = exp2_neg_q31(x);
    }
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
    (void)params;                                                        /* the knobs are read in the first render */
    State *s = state;
    s->note = (uint8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));      /* the OS folds notes into 0 .. 127 */
    s->vel = (uint8_t)(velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
    s_seed = s_seed * 1664525u + 1013904223u;
    s->seed = s_seed;
    s->ck_env = s->ck_lp = 0;
    s->gain = 0;
    s->fade = 0;
    s->phase = PH_STRIKE;
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                             /* one-shots: the hit rings on */
}

/* One partial into s_acc: a sine from the interpolated table (one load: value and step; the fraction is the signed
 * low half of ph >> 9, see mallet_tables.h), its amplitude ramped by astep per sample. first: store instead of add.
 * On the Blackfin the body is scheduled by hand (engines/modal): the table and accumulator loads and the store issue
 * in parallel with arithmetic, 10 cycles per sample. The C below is the same arithmetic, bit for bit. */
#if defined(__bfin__) && !defined(MALLET_C_LOOP)
ALWAYS_INLINE void mode_pass(uint32_t *php, uint32_t inc, int32_t amp, int32_t astep, int first)
{
    uint32_t ph = *php;
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
}
#else
ALWAYS_INLINE void mode_pass(uint32_t *php, uint32_t inc, int32_t amp, int32_t astep, int first)
{
    uint32_t ph = *php;
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

/* s_acc x the output level, added into both channels of out, the level ramped by ls per sample. On the Blackfin the
 * loop is scheduled by hand (engines/modal), 4 cycles per frame; the C below is the same arithmetic, bit for bit.
 * Reads s_acc[64] (the guard) once. Returns the level. */
#if defined(__bfin__) && !defined(MALLET_C_LOOP)
ALWAYS_INLINE int32_t pass_lin(fract32 *out, int32_t level, int32_t ls)
{
    const int32_t *acc = s_acc;
    fract32 *w = out;
    int32_t x, v, l, r;
    __asm__ volatile("I0 = %[out];\n\t"
                     "I1 = %[out];\n\t"
                     "M0 = 8 (X);\n\t"
                     "I1 += 4;\n\t"
                     "%[x] = [%[acc]++];\n\t"
                     "LSETUP (1f, 2f) LC0 = %[n];\n"
                     "1: %[v] = %[x].H * %[lev].H || %[l] = [I0 ++ M0] || %[r] = [I1 ++ M0];\n\t"
                     "%[l] = %[l] + %[v] (S) || %[x] = [%[acc]++] || NOP;\n\t"
                     "%[r] = %[r] + %[v] (S) || [%[w]++] = %[l] || NOP;\n"
                     "2: %[lev] = %[lev] + %[ls] (S) || [%[w]++] = %[r] || NOP;"
                     : [lev] "+d"(level), [acc] "+a"(acc), [w] "+a"(w), [x] "=&d"(x), [v] "=&d"(v), [l] "=&d"(l),
                       [r] "=&d"(r)
                     : [out] "a"(out), [ls] "d"(ls), [n] "a"(OPZ_BLOCK_FRAMES)
                     : "memory", "I0", "I1", "M0", "LC0", "LT0", "LB0");
    return level;
}
#else
ALWAYS_INLINE int32_t pass_lin(fract32 *out, int32_t level, int32_t ls)
{
    const int32_t *acc = s_acc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        fract32 v = mul_hh(acc[i], level);
        out[2 * i] = opz_add_q31(out[2 * i], v);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], v);
        level = opz_add_q31(level, ls);
    }
    return level;
}
#endif

static void end_hit(State *s, ADSR *env)
{
    s->phase = PH_OFF;
    s->ck_env = 0;
    OPZ_ADSR_LEVEL(env) = 0;
    OPZ_ADSR_STAGE(env) = 0;                                             /* the host frees the voice */
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                                       /* drum tracks: the pitch is the note */
    State *s = state;
    int32_t stage = OPZ_ADSR_STAGE(env);
    if (stage == 0) return;
    if (s->phase == PH_OFF || (stage >= 5 && s->phase == PH_STRIKE)) {   /* over, or stolen before it sounded */
        end_hit(s, env);
        return;
    }

    if (s->phase == PH_STRIKE) {
        int32_t p1 = params ? opz_p16(params[OPZ_P_P1]) : 32768, p2 = params ? opz_p16(params[OPZ_P_P2]) : 32768;
        s->p1 = (uint16_t)(p1 > 65535 ? 65535 : p1);                     /* the host's words are below 2^31 */
        s->p2 = (uint16_t)(p2 > 65535 ? 65535 : p2);
        strike(s);
        s->phase = PH_DECAY;
    } else if (s->phase == PH_DECAY) {
        decays(s);
        s->phase = PH_RING;
    }

    /* The OS wants this voice for a new note (voice steal, stage 5/6) and starts it when we reach stage 0: fade out
     * linearly over two blocks (2.9 ms) instead of ringing on. */
    if (stage >= 5 && !s->fade) {
        s->fade = (s->gain >> 7) + 1;
        s->ck_env = 0;
    }

    /* the partials: exact exponential per block, ramped linearly per sample */
    int first = 1;
    int32_t sum = 0;
    for (int k = 0; k < NM; k++) {
        int32_t a = s->amp[k];
        if (!a) continue;                                                /* silent or out of the band */
        int32_t tgt = opz_mul_q31(a, s->dec[k]);
        if (tgt < LIVE) tgt = 0;
        s->amp[k] = tgt;
        sum += tgt;
        if (first) mode_pass(&s->ph[k], s->inc[k], a, (tgt - a) >> 6, 1);
        else mode_pass(&s->ph[k], s->inc[k], a, (tgt - a) >> 6, 0);
        first = 0;
    }
    if (s->ck_env) {
        if (first) click_pass(s, 1);
        else click_pass(s, 0);
        first = 0;
    }
    if (first) { end_hit(s, env); return; }                              /* nothing left to play */

    /* the output level: the contact rise, then 1; or the fade of a stolen voice */
    int32_t g = s->gain, ls;
    if (s->fade) {
        ls = g > (s->fade << 6) ? -s->fade : -(g >> 6);
    } else {
        int32_t g1 = opz_add_q31(g, s->gain_inc);
        ls = (g1 - g) >> 6;
    }
    s->gain = pass_lin(out, g, ls);

    if ((s->fade && s->gain < 64) || (sum < END_SUM && !s->ck_env)) end_hit(s, env);
}

OPZ_EXPORTS_INSTRUMENT();
