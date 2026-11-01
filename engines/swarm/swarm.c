/*
 * swarm — tonal noise and oscillator swarms: from sparse particles and crackle tuned to the note, through fizzing
 * tonal noise, to a dense cloud of twelve detuned saws (a big unison drone), spread across the stereo field.
 *
 *   P1        density     0 .. 112: particles only, from ~1.5 a second (a ping on the note, now and then another)
 *                         to ~290 a second (a sparkling cloud); the pings ring shorter as they get denser (Q 250
 *                         at 0 -> 25 at 192, between 40 ms and 1.5 s). 112 .. 224: the saws come in one after the
 *                         other (all twelve by 255) while the particles thicken into tonal noise (up to 5,500 a
 *                         second) and fade out, equal power; from 160 the particle cloud thins to one partial a
 *                         side. 224 .. 255: the saw swarm alone, its slow swells settling (depth 0.85 -> 0.35)
 *   P2        spread      0 .. 128: unison -> chorus (the cluster's half-width 0 -> 50 cents, quadratic)
 *                         128 .. 192: wide cluster (50 -> 200 cents: a tone cluster around the note)
 *                         192 .. 255: inharmonic: every saw and particle glides from its place in the cluster to a
 *                         circular membrane's mode ratio (1, 1.59, 2.14 ... 4.15), the cluster closing to 30 cents;
 *                         the centre saw and three in eight particles stay on the note, the others get quieter
 *                         towards 1 / sqrt(ratio)
 *   velocity              level, and the crackle: each particle's click, none below 24, then velocity squared
 *   attack..release       host envelope
 *
 * The saws: as in engines/super, a weighted sum of naive saws is one straight ramp plus a downward step wherever
 * one of them wraps, so per block each saw costs its starting value, its slope and its few wraps (each wrap writes
 * its step and two-sample PolyBLEP residual into an event buffer); per sample the left and right signals are only
 * integrated. The cost hardly depends on how many saws play. Each saw has its own cluster position, stereo position
 * (hard left to hard right, narrower below 150 Hz), slow swell (0.13 .. 0.63 Hz) and drift (+-1.5 cents); the
 * weights are normalised by 1 / sqrt(sum of squares), so the level stays even while saws come in, swell and spread.
 * Saws fade out between 4 and 6 kHz, and a voice spends at most ~16 saw wraps a block (on high notes and wide
 * ratios the last saws fade out), which bounds the cost.
 *
 * The particles: random impulses (a Poisson-like count per block) excite four sine partials, each decaying like a
 * resonator of quality Q (exact exponential per block, ramped per sample: no recursion, nothing can blow up). A
 * particle that is louder than what still rings on its partial re-pitches it to a new random place in the cluster
 * (or to an inharmonic ratio), so sparse particles scatter in pitch while dense ones settle into tonal noise
 * around four pitches. The particle level is normalised for the overlap (RMS) and capped for single pings (peak).
 * Each particle also adds a one-sample click (the crackle) at its own sample. Partials 0 and 2 sit left, 1 and 3
 * right (a quarter crosses over); a note starts with a centred ping on the note itself, or, where the cloud is
 * dense, with its partials already ringing.
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine.
 *
 * Ideas from the OP-XY's dissolve, the OP-1's DNA and Mutable Instruments Plaits' particle and swarm models
 * (MIT); no code from them is used. The saw events follow engines/super, the sine table and its one-instruction
 * multiplies engines/modal, log2 and the closed-form envelope engines/vowel (all this project, MIT). MIT licence,
 * see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "swarm_tables.h"

#define N OPZ_BLOCK_FRAMES
#define NS SWARM_SAWS
#define NP 4
#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define OCT(x) ((int32_t)((x) * 65536.0 + ((x) < 0 ? -0.5 : 0.5)))   /* octaves, Q16 (constants only) */

typedef struct Part {
    uint32_t ph;            /* phase, Q32 */
    int32_t amp;            /* amplitude at the end of the last block, Q31 (signed) */
    int16_t u;              /* place in the cluster, -1..1, Q15 */
    uint8_t k;              /* inharmonic ratio, index into k_part_l2r */
    uint8_t pad;
} Part;

typedef struct State {
    uint32_t ph[NS];        /* saw phases, Q32 */
    Part pt[NP];            /* particle partials */
    int32_t vel;            /* velocity, Q15 */
    int32_t hard;           /* click amount, Q15 */
    uint32_t seed;          /* particles */
    uint32_t sw;            /* swell and drift phases of this note */
    uint32_t t;             /* blocks since the note-on */
    int32_t norm;           /* the saw weights' normalisation of the last block, Q15 */
    uint32_t ib[NS];        /* the saws' increments without drift (0: above 6 kHz), for key_* */
    uint32_t rb[NS];        /* and 2^47 / ib (0: not computed yet) */
    uint32_t key_inc;       /* the pitch, spread and inharmonic share ib was computed for */
    int32_t key_sp, key_ih;
    uint8_t nb;             /* how many of ib are valid */
    uint8_t fresh;
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in the 280 bytes the OS leaves (docs/STATUS.md)");

/* Block buffers, shared by all voices (render runs one voice at a time): the saw events (left, right; differences,
 * integrated per sample; index N takes events after the block and is never read) and the two particle groups.
 * pass_out leaves the event buffers zeroed. .bss keeps them inside the slot window. */
static uint32_t s_el[N + 1], s_er[N + 1];   /* modulo 2^32, like the integration */
static int32_t s_pa[N], s_pb[N];

static const ModuleCallbackInterface *s_cb;
static uint32_t s_seed = 0x3c6ef372u;

#define INC_MIN 0x100000u               /* 10.7 Hz */
#define INC_MAX 0x40000000u             /* 11 kHz */
#define SAW_HF1 OCT(29.1221)            /* log2 of the increment of 6 kHz: saws above are silent */
#define PART_HF OCT(30.1221)            /* 12 kHz: a particle partial above is silent */
#define PI_HF 1168702000u               /* its increment, for the block it fades in */
#define K_DECAY OCT(8.1802 - 32.0)      /* log2(decay per block, octaves) = log2(inc) + K_DECAY - log2(Q) */
#define LK_MIN OCT(-9.4848)             /* the longest ring: 1.5 s */
#define LK_MAX OCT(-4.2560)             /* the shortest: 40 ms (high notes ping instead of tick) */
#define LO_WIDE OCT(23.8003)            /* log2 of the increment of 150 Hz: the stereo width narrows below */
#define PARTS_TWO (160 * 257)           /* P1 from where particles go to two partials only (the saws take over) */
#define SAW_START (112 * 257)           /* P1 where the first saw comes in (opz_p16 units) */
#define DRIFT 57                        /* +-1.5 cents as a frequency deviation per unit of tri15, Q31 */
#define INC_4K 389566198u               /* 4 kHz: saws fade out from here */
#define INC_6K 584349297u               /* 6 kHz: silent */
#define WRAP_BUDGET (16u << 16)         /* saw wraps per block and voice (Q16): later saws are left out ... */
#define WRAP_FADE (4u << 16)            /* ... fading over the last four */
#define PART_JIT 109                    /* +-2 cents of pitch scatter for particles even at P2 0 */
#define LIVE 0x80000                    /* a partial quieter than this (-72 dBFS) is silent (ramps to 0) */
#define ACAP 0x1c000000                 /* a partial's amplitude is clamped here (0.22 FS) */
#define CLICK_SUM 0x20000000            /* at most this much click per block (keeps the sums in range) */

/* Levels at velocity 127, before the envelope (calibrated in the simulator, README): the saw layer (its weights
 * sum to at most sqrt(12) x GS x (1 + pan), normalised), the particles' RMS target and a single ping's peak. */
#define GS 4380                         /* 0.134, Q15: the saw layer at -20.5 dBFS RMS per channel */
#define LOG2_RT OCT(-3.06)              /* particle RMS target 0.12 */
#define LOG2_CAP OCT(-1.84)             /* a single ping's peak 0.28 */
#define RT_PRE 4813                     /* 0.12 sqrt(3 / 2), Q15: a pre-charged partial (uniform), RMS 0.12 / sqrt 2 */

static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return s_seed;
}

/* ---- fixed-point helpers ------------------------------------------------------------------------------------ */

#if defined(__bfin__)
/* Fractional a.H x b.L, a.H x b.H and a.L x b.L -> Q31 (saturating -1 x -1): one instruction each. */
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

/* a x b of two values that fit 16 bits signed (Q15 gains): one 16 x 16 multiply instead of a 32-bit one. */
ALWAYS_INLINE int32_t m16(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int16_t)b; }

/* log2(x) in Q16 for 0 < x < 2^31: exponent from norm, log2(1 + m) ~ m (1.3466 - 0.3466 m) (0.002 octaves). */
static int32_t log2_q16(uint32_t x)
{
    int n = opz_norm32((int32_t)x);                          /* x << n in [2^30, 2^31) */
    uint32_t m = ((x << n) - 0x40000000u) >> 14;             /* Q16 */
    uint32_t t = 88252u - opz_mul_u32_shr16(22714u, m);
    return ((30 - n) << 16) + (int32_t)opz_mul_u32_shr16(m, t);
}

/* 2^(x / 65536) as Q15, clamped to 0 .. 32767 (x <= 0 octaves for a gain below 1). */
static int32_t exp2_q15(int32_t x)
{
    if (x < OCT(-14.9)) return 0;
    if (x > 0) x = 0;
    int32_t v = opz_exp2_q16(x) >> 1;
    return v > 32767 ? 32767 : v;
}

/* A 17-point knob curve (bytes 0, 16 .. 256) at p (opz_p16 units), linearly interpolated. */
ALWAYS_INLINE int32_t curve32(const int32_t *t, int32_t p)
{
    int i = p >> 12;
    return t[i] + (((t[i + 1] - t[i]) * (p & 0xfff)) >> 12);
}
ALWAYS_INLINE int32_t curve16(const int16_t *t, int32_t p)
{
    int i = p >> 12;
    return t[i] + (((t[i + 1] - t[i]) * (p & 0xfff)) >> 12);
}

/* ---- the saws ------------------------------------------------------------------------------------------------ */

/* One saw's wraps in this block, with left weight wm and right weight ws (Q15), into the event buffers: its steps
 * and BLEP residuals as differences (engines/super's saw_events with 16-bit weights; the slope uses the same
 * products, so ramp and steps match). p is the phase at sample 0, recip ~2^47 / inc (to 1e-4: x is clamped). */
static void saw_events(uint32_t p, uint32_t inc, uint32_t recip, int32_t wm, int32_t ws)
{
    if (p < inc) {                                           /* sample 0 follows a wrap in the last block */
        uint32_t xu = opz_mul_u32_shr16(p, recip);
        fract32 y = xu >= OPZ_Q31_ONE ? 0 : OPZ_Q31_ONE - (int32_t)xu;
        int32_t bm = opz_mul_q31(opz_mul_q31_q15(y, wm), y), bs = opz_mul_q31(opz_mul_q31_q15(y, ws), y);
        s_el[0] += (uint32_t)bm; s_el[1] -= (uint32_t)bm;
        s_er[0] += (uint32_t)bs; s_er[1] -= (uint32_t)bs;
    }
    int32_t wm31 = wm * 65536, ws31 = ws * 65536;            /* the weights below 0.5: 2 w fits */
    int j = 0;                                               /* p is the phase at sample j */
    for (;;) {
        uint32_t np = ~p;                                    /* the wrap comes after floor(np / inc) more samples */
        if (inc < 66076419u && np >= inc * 65u) return;    /* not in this block; keeps the estimate in range */
        uint32_t q = opz_mul_u32_shr16(np >> 7, recip) >> 24;   /* floor(np / inc), give or take one */
        uint32_t rem = np - q * inc;                         /* exact modulo 2^32 */
        if (rem >= inc) {
            if (rem >= 0x80000000u) { q--; rem += inc; }
            else { q++; rem -= inc; }
        }
        int jb = j + (int)q;                                 /* last sample before the wrap; its phase is ~rem */
        if (jb >= N) return;
        uint32_t xu = opz_mul_u32_shr16(rem, recip);
        fract32 x = xu > OPZ_Q31_ONE ? OPZ_Q31_ONE : (fract32)xu;
        int32_t am = opz_mul_q31_q15(x, wm), as = opz_mul_q31_q15(x, ws);
        int32_t bm = opz_mul_q31(am, x), bs = opz_mul_q31(as, x);
        uint32_t cm = (uint32_t)(wm31 - 2 * am + bm), cs = (uint32_t)(ws31 - 2 * as + bs);   /* w (1 - x)^2 */
        s_el[jb] -= cm; s_el[jb + 1] += cm;
        s_er[jb] -= cs; s_er[jb + 1] += cs;
        if (jb + 1 >= N) return;                             /* the wrap itself falls in the next block */
        s_el[jb + 1] += (uint32_t)(bm - 2 * wm31); s_el[jb + 2] -= (uint32_t)bm;
        s_er[jb + 1] += (uint32_t)(bs - 2 * ws31); s_er[jb + 2] -= (uint32_t)bs;
        p = inc - 1u - rem;                                  /* phase at sample jb + 1, after the wrap */
        j = jb + 1;
    }
}

/* ---- the particles ------------------------------------------------------------------------------------------- */

/* One partial into y over the block: a sine from the interpolated table (one load: value and step; the fraction is
 * the signed low half of ph >> 9, see swarm_tables.h), its amplitude ramped by astep per sample; first: store
 * instead of add. On the Blackfin the loop is engines/modal's mode_pass, scheduled by hand (the table and buffer
 * loads and the store issue in parallel with arithmetic: 9 and 10 cycles per sample); the C below is the same
 * arithmetic, bit for bit. */
#if defined(__bfin__) && !defined(SWARM_C_LOOP)
__attribute__((noinline)) static void part_pass(int32_t *y, uint32_t ph, uint32_t inc, int32_t amp, int32_t astep,
                                                int first)
{
    int32_t *p = y;
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
                         : [inc] "d"(inc), [st] "d"(astep), [tab] "a"(k_sin), [n] "a"(N)
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
                         : [inc] "d"(inc), [st] "d"(astep), [tab] "a"(k_sin), [n] "a"(N)
                         : "memory", "LC0", "LT0", "LB0");
}
#else
__attribute__((noinline)) static void part_pass(int32_t *y, uint32_t ph, uint32_t inc, int32_t amp, int32_t astep,
                                                int first)
{
    for (int i = 0; i < N; i++) {
        int32_t e = k_sin[ph >> 24];
        int32_t v = mul_hl(amp, add_hh(e, mul_ll(e, (int32_t)(ph >> 9))));     /* low half: sin, Q15 */
        y[i] = first ? v : y[i] + v;
        ph += inc;
        amp += astep;
    }
}
#endif

/* ---- output -------------------------------------------------------------------------------------------------- */

/* The host envelope 64 samples on, in closed form (engines/vowel's env_block): decay and release level x c^64;
 * attack and sustain (c = 1) with (1 - r^3) s held for the block; a fade with a step, the exact 64 steps. */
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

/* One channel: integrate slope + events (e, left zeroed), plus the particle groups (own side a, the other side b
 * a quarter), times the envelope (a ramp from lv by dl per sample), added into out (every second value). One
 * channel at a time keeps everything in registers. On the Blackfin the loops are scheduled by hand (5 and 9
 * instructions per sample, loads and stores in parallel with arithmetic); the C below is the same arithmetic, bit
 * for bit. */
#if defined(__bfin__) && !defined(SWARM_C_LOOP)
__attribute__((noinline)) static void pass_out(fract32 *out, uint32_t *e, uint32_t y, uint32_t dy, int32_t lv,
                                               int32_t dl, const int32_t *a, const int32_t *b, int parts)
{
    int32_t t0, t1, z = 0;
    if (parts)
        __asm__ volatile("I0 = %[out];\n\t"
                         "M0 = 8 (X);\n\t"
                         "LSETUP (1f, 2f) LC0 = %[n];\n"
                         "1: %[y] = %[y] + %[dy] (NS) || %[t0] = [%[e]] || NOP;\n\t"
                         "%[y] = %[y] + %[t0] (NS) || %[t1] = [%[b]++] || NOP;\n\t"
                         "%[t1] >>>= 2;\n\t"
                         "%[t0] = [%[a]++];\n\t"
                         "%[t1] = %[t1] + %[t0] (NS);\n\t"
                         "%[t0] = %[y] + %[t1] (S) || %[t1] = [I0] || NOP;\n\t"
                         "%[t0] = %[t0].H * %[lv].H || [%[e]++] = %[z] || NOP;\n\t"
                         "%[t1] = %[t1] + %[t0] (S);\n"
                         "2: %[lv] = %[lv] + %[dl] (NS) || [I0 ++ M0] = %[t1] || NOP;"
                         : [y] "+d"(y), [lv] "+d"(lv), [e] "+a"(e), [a] "+a"(a), [b] "+a"(b), [t0] "=&d"(t0),
                           [t1] "=&d"(t1)
                         : [dy] "d"(dy), [dl] "d"(dl), [z] "d"(z), [out] "a"(out), [n] "a"(N)
                         : "memory", "I0", "M0", "LC0", "LT0", "LB0");
    else
        __asm__ volatile("I0 = %[out];\n\t"
                         "M0 = 8 (X);\n\t"
                         "LSETUP (1f, 2f) LC0 = %[n];\n"
                         "1: %[y] = %[y] + %[dy] (NS) || %[t0] = [%[e]] || NOP;\n\t"
                         "%[y] = %[y] + %[t0] (NS) || %[t1] = [I0] || NOP;\n\t"
                         "%[t0] = %[y].H * %[lv].H || [%[e]++] = %[z] || NOP;\n\t"
                         "%[t1] = %[t1] + %[t0] (S);\n"
                         "2: %[lv] = %[lv] + %[dl] (NS) || [I0 ++ M0] = %[t1] || NOP;"
                         : [y] "+d"(y), [lv] "+d"(lv), [e] "+a"(e), [t0] "=&d"(t0), [t1] "=&d"(t1)
                         : [dy] "d"(dy), [dl] "d"(dl), [z] "d"(z), [out] "a"(out), [n] "a"(N)
                         : "memory", "I0", "M0", "LC0", "LT0", "LB0");
}
#else
ALWAYS_INLINE void pass_body(fract32 *out, uint32_t *e, uint32_t y, uint32_t dy, int32_t lv, int32_t dl,
                             const int32_t *a, const int32_t *b, int parts)
{
    for (int i = 0; i < N; i++) {
        y += dy + e[i];                                      /* wraps modulo 2^32; in range at every sample */
        e[i] = 0;
        fract32 v = (fract32)y;
        if (parts) v = opz_add_q31(v, a[i] + (b[i] >> 2));
        out[2 * i] = opz_add_q31(out[2 * i], mul_hh(v, lv));
        lv += dl;
    }
}

__attribute__((noinline)) static void pass_out(fract32 *out, uint32_t *e, uint32_t y, uint32_t dy, int32_t lv,
                                               int32_t dl, const int32_t *a, const int32_t *b, int parts)
{
    if (parts) pass_body(out, e, y, dy, lv, dl, a, b, 1);
    else pass_body(out, e, y, dy, lv, dl, a, b, 0);
}
#endif

/* ---- entry points -------------------------------------------------------------------------------------------- */

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
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note; (void)params;                                            /* may be NULL (engine-abi.md §3); not needed */
    State *s = state;
    for (int k = 0; k < NS; k++) s->ph[k] = rnd();           /* free-running oscillators: any phase at note-on */
    for (int r = 0; r < NP; r++) {
        s->pt[r].ph = rnd();
        s->pt[r].amp = 0;
        s->pt[r].u = 0;
        s->pt[r].k = 0;
    }
    s->seed = rnd();
    s->sw = rnd();
    s->t = 0;
    int32_t v = velocity > 127 ? 127 : velocity;
    s->vel = v * 258;                                        /* Q15, 32766 at 127 */
    s->hard = v < 24 ? 0 : (v * v * 39) >> 6;                /* (v / 127)^2 x 0.3, Q15; none below 24 */
    s->fresh = 1;
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                 /* the host releases the envelope */
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* ---- per block ------------------------------------------------------------------------------------------ */
    uint32_t inc = pitch + (uint32_t)(pitchSlope * (N / 2));  /* the block's middle: glides move in 1.5 ms steps */
    if ((int32_t)inc < (int32_t)INC_MIN) inc = INC_MIN;
    if (inc > INC_MAX) inc = INC_MAX;
    int32_t lo = log2_q16(inc);
    int32_t p1 = opz_p16(params[OPZ_P_P1]), p2 = opz_p16(params[OPZ_P_P2]);
    if (p1 > 65535) p1 = 65535;                              /* the host sends at most 0x7fffffff */
    if (p2 > 65535) p2 = 65535;
    int32_t spread = curve32(k_p2_spread, p2);
    int32_t inh = curve16(k_p2_inh, p2);
    int32_t vel = s->vel;
    uint32_t t = s->t++;

    /* ---- the saws: weights (count, swell, inharmonic tilt, high-frequency fade), increments and wraps ---- */
    int32_t n16 = p1 > SAW_START ? ((p1 - SAW_START) * 21932) >> 10 : 0;   /* saws in, Q16: 12 at P1 255 */
    int ns = (n16 + 65535) >> 16;                            /* saws with a weight */
    if (ns > NS) ns = NS;
    /* the weights are normalised by 1 / sqrt(max(1, sum of their squares)) of the last block (they move slowly;
     * the first block estimates it from the count) x the layer gain x velocity x GS */
    int32_t norm = s->norm;
    if (!t) norm = n16 > 65536 ? exp2_q15(-((log2_q16((uint32_t)n16) - OCT(16)) >> 1)) : 32767;
    int32_t gt = m16(m16(m16(norm, curve16(k_p1_gs, p1)) >> 15, vel) >> 15, GS) >> 15;   /* Q15 */
    /* stereo width, narrowed on low notes: full above 150 Hz, mono two octaves below */
    int32_t width = curve16(k_p2_width, p2);
    if (lo < LO_WIDE) width = lo <= LO_WIDE - OCT(2) ? 0 : (width * ((lo - (LO_WIDE - OCT(2))) >> 2)) >> 15;
    int32_t depth = curve16(k_p1_depth, p1);
    if (inc != s->key_inc || spread != s->key_sp || inh != s->key_ih) {   /* the saws' frequencies moved */
        s->key_inc = inc;
        s->key_sp = spread;
        s->key_ih = inh;
        s->nb = 0;
    }
    int32_t sum = 0;
    uint32_t wraps = 0;                                      /* wraps per block of the saws so far, Q16 */
    uint32_t acl = 0, dcl = 0, acr = 0, dcr = 0, rc0 = 0;   /* left, right: value at sample 0, slope */
    uint32_t lph = s->sw;                                    /* this note's LFO phases, a different one per saw */
    for (int k = 0; k < ns && wraps < WRAP_BUDGET; k++, lph += 0x9e3779b9u) {
        if (k >= s->nb) {                                    /* base increment: inc x 2^(cluster + inharmonic) */
            int32_t o = ((spread * k_saw_pos[k]) >> 15) + opz_mul_q31_q15(k_saw_l2r[k], inh);
            s->ib[k] = lo + o >= SAW_HF1 ? 0 : opz_mul_u32_shr16(inc, (uint32_t)opz_exp2_q16(o));
            s->rb[k] = 0;
        }
        uint32_t ib = s->ib[k];
        if (!ib) continue;                                   /* silent above 6 kHz (its phase waits) */
        int32_t dr = DRIFT * opz_tri15(t * k_saw_drift[k] + (lph << 9));    /* frequency deviation, Q31 */
        uint32_t p = s->ph[k], ikk = ib + (uint32_t)opz_mul_q31((int32_t)ib, dr);
        s->ph[k] = p + ikk * N;
        int32_t wc = n16 - (k << 16);
        int32_t g = wc >= 65535 ? 32767 : wc >> 1;           /* Q15 */
        g = m16(g, 32767 - (m16(depth, (opz_tri15(t * k_saw_swell[k] + lph) + 32767) >> 1) >> 15)) >> 15;
        if (inh) g = m16(g, 32767 - (m16(k_saw_tilt[k], inh) >> 15)) >> 15;   /* towards 1 / sqrt(ratio) */
        if (ikk > INC_4K)                                    /* 4 .. 6 kHz fade */
            g = ikk >= INC_6K ? 0 : m16(g, m16((int32_t)((INC_6K - ikk) >> 13), 1411) >> 10) >> 15;
        uint32_t left = WRAP_BUDGET - wraps;                 /* the last saws within the budget fade */
        if (left < WRAP_FADE) g = m16(g, (int32_t)(left >> 3) - 1) >> 15;
        wraps += ikk >> 10;
        sum += m16(g, g) >> 14;                              /* Q16 */
        int32_t wm = m16(g, gt) >> 15;                       /* Q15 */
        if (!wm) continue;
        int32_t pw = m16(wm, m16(k_saw_pan[k], width) >> 15) >> 15;
        int32_t wl = wm + pw, wr = wm - pw;                  /* L = M + S, R = M - S */
        int32_t y = (int32_t)(p ^ 0x80000000u);              /* the naive saw at sample 0 */
        acl += (uint32_t)opz_mul_q31_q15(y, wl);
        acr += (uint32_t)opz_mul_q31_q15(y, wr);
        dcl += (uint32_t)opz_mul_q31_q15((int32_t)ikk, wl);
        dcr += (uint32_t)opz_mul_q31_q15((int32_t)ikk, wr);
        if (p < ikk || ikk >= (1u << 26) || ~p < (ikk << 6)) {   /* a wrap just before sample 0 or in the block */
            uint32_t rb = s->rb[k];
            if (!rb) {                                       /* 2^47 / ib = 2^47 / inc x 2^-o */
                if (!rc0) rc0 = opz_blep_recip(inc);
                int32_t o = ((spread * k_saw_pos[k]) >> 15) + opz_mul_q31_q15(k_saw_l2r[k], inh);
                s->rb[k] = rb = opz_mul_u32_shr16(rc0, (uint32_t)opz_exp2_q16(-o));
            }
            saw_events(p, ikk, rb - (uint32_t)opz_mul_q31((int32_t)rb, dr), wl, wr);   /* 1 / (1 + d) ~ 1 - d */
        }
    }
    if (ns > s->nb) s->nb = (uint8_t)ns;
    s->norm = sum > 65536 ? exp2_q15(-((log2_q16((uint32_t)sum) - OCT(16)) >> 1)) : 32767;

    /* ---- the particles ---- */
    int32_t gp = curve16(k_p1_gp, p1);
    int32_t lq = curve32(k_p1_lq, p1);
    int32_t lmu = curve32(k_p1_lmu, p1);
    int32_t ps = spread + PART_JIT;
    /* the level of one particle: RMS target over the overlap (sum of A^2 / 2 = mu E[X^2] / (2 (1 - d^2)), with
     * X uniform in +-sig: sig = RT sqrt(6 (1 - d^2) / mu)), capped for single pings; d from the note's decay */
    int32_t lk0 = lo + K_DECAY - lq;
    if (lk0 < LK_MIN) lk0 = LK_MIN;
    if (lk0 > LK_MAX) lk0 = LK_MAX;
    int32_t k0 = opz_exp2_q16(lk0);                          /* decay per block, octaves Q16 */
    int32_t om = 65536 - (k0 >= OCT(7.5) ? 0 : opz_exp2_q16(-2 * k0));     /* 1 - d^2, Q16 */
    if (om < 1) om = 1;
    int32_t lsig = LOG2_RT + ((OCT(2.585) + log2_q16((uint32_t)om) - OCT(16) - lmu) >> 1);
    if (lsig > LOG2_CAP) lsig = LOG2_CAP;
    /* pings ringing at once, mu / (1 - d) ~ 2 mu / (1 - d^2): eight or more is a cloud (the note starts in it) */
    int dense = lmu - (log2_q16((uint32_t)om) - OCT(16)) + OCT(1) >= OCT(3);
    int32_t sig = (((exp2_q15(lsig) * gp) >> 15) * vel) >> 15;            /* Q15 */

    int32_t a0[NP], a1[NP];
    uint32_t pi[NP];
    for (int r = 0; r < NP; r++) {                           /* decay what rings, at its current pitch */
        Part *pt = &s->pt[r];
        a0[r] = pt->amp;
        a1[r] = 0;
        pi[r] = PI_HF;
        if (!pt->amp) continue;
        int32_t o = ((ps * pt->u) >> 15) + opz_mul_q31_q15(k_part_l2r[pt->k], inh);
        int32_t lk = lo + o;
        if (lk >= PART_HF) continue;                         /* bent or spread out of range: fades this block */
        pi[r] = opz_mul_u32_shr16(inc, (uint32_t)opz_exp2_q16(o));
        int32_t kl = lk + K_DECAY - lq;
        if (kl < LK_MIN) kl = LK_MIN;
        if (kl > LK_MAX) kl = LK_MAX;
        a1[r] = opz_mul_q31(pt->amp, opz_exp2_q16(-opz_exp2_q16(kl)) << 15);   /* x 2^-k, k < 1 */
        if (r >= 2 && p1 >= PARTS_TWO) a1[r] >>= 1;          /* the thinned cloud: the other two fade in ~17 ms */
    }
    unsigned moved = 0;
    if (s->fresh) {
        s->fresh = 0;
        if (gp && dense) {                                   /* dense: start with every partial already ringing */
            int32_t a = (((RT_PRE * gp) >> 15) * vel) >> 15;
            int nr = p1 < PARTS_TWO ? NP : 2;
            if (nr == 2) a = (a * 46341) >> 15;              /* the same level on two partials: x sqrt(2) */
            for (int r = 0; r < nr; r++) {
                uint32_t x = s->seed = s->seed * 1664525u + 1013904223u;   /* the voice's own random numbers */
                s->pt[r].u = (int16_t)(x >> 16);
                s->pt[r].k = (uint8_t)((x >> 5) & 7);
                a1[r] = a * (int16_t)x * 2;
            }
            moved = (1u << nr) - 1;
        } else if (gp) {                                     /* sparse: a ping on the note, both sides, a click */
            int32_t a = (((exp2_q15(LOG2_CAP) * gp) >> 15) * vel) >> 15;
            a = (a * 23170) >> 15;                           /* each side -3 dB: centred, as loud as one ping */
            for (int r = 0; r < 2; r++) {
                s->pt[r].u = 0;
                s->pt[r].k = 0;
                a1[r] = a * 65536;
            }
            moved = 3;
            int32_t c = opz_mul_q31_q15(a * 65536, s->hard >> 1);
            s_el[0] += (uint32_t)c; s_el[1] -= (uint32_t)c;
            s_er[0] += (uint32_t)c; s_er[1] -= (uint32_t)c;
        }
    }
    /* this block's particles: a count with the right mean (the whole part and a coin for the fraction), each at a
     * random sample on a random partial with a random level; its click goes into the saw events as an impulse */
    int32_t mu = opz_exp2_q16(lmu);
    uint32_t seed = s->seed * 1664525u + 1013904223u;
    int np = sig ? (mu >> 16) + ((int32_t)(seed >> 16) < (mu & 0xffff)) : 0;
    int32_t csum = 0;
    int32_t hard = s->hard;
    int rmask = p1 < PARTS_TWO ? 3 : 1;                      /* from P1 160 the cloud thins to one partial a side */
    for (int n = 0; n < np; n++) {
        uint32_t r1 = seed = seed * 1664525u + 1013904223u;
        int j = (int)(r1 >> 26);                             /* the top bits: sample, partial, level */
        int r = (int)(r1 >> 24) & rmask;
        int32_t x = sig * (int16_t)(r1 >> 8) * 2;            /* Q31, |x| <= sig */
        if (opz_abs32(a1[r]) < opz_abs32(x)) {               /* louder than the ring: a new place in the swarm */
            uint32_t r2 = seed = seed * 1664525u + 1013904223u;
            s->pt[r].u = (int16_t)(r2 >> 16);
            s->pt[r].k = (uint8_t)((r2 >> 13) & 7);
            moved |= 1u << r;
        }
        a1[r] = opz_add_q31(a1[r], x);
        if (hard && csum < CLICK_SUM) {
            int32_t c = opz_mul_q31_q15(x, hard);
            uint32_t *e = r & 1 ? s_er : s_el, *f = r & 1 ? s_el : s_er;  /* its side, and a quarter across */
            csum += opz_abs32(c);
            e[j] += (uint32_t)c; e[j + 1] -= (uint32_t)c;
            f[j] += (uint32_t)(c >> 2); f[j + 1] -= (uint32_t)(c >> 2);
        }
    }
    s->seed = seed;
    for (int r = 0; r < NP; r++) {
        Part *pt = &s->pt[r];
        if ((moved >> r) & 1) {                              /* re-pitched: the new increment */
            int32_t o = ((ps * pt->u) >> 15) + opz_mul_q31_q15(k_part_l2r[pt->k], inh);
            if (lo + o >= PART_HF) {
                pi[r] = PI_HF;
                a1[r] = 0;
            } else {
                pi[r] = opz_mul_u32_shr16(inc, (uint32_t)opz_exp2_q16(o));
            }
        }
        if (a1[r] > ACAP) a1[r] = ACAP;
        if (a1[r] < -ACAP) a1[r] = -ACAP;
        if (opz_abs32(a1[r]) < LIVE) a1[r] = 0;
        pt->amp = a1[r];
    }
    /* the two groups: partials 0 and 2 left (s_pa), 1 and 3 right (s_pb) */
    int live[2];
    for (int g = 0; g < 2; g++) {
        int32_t *y = g ? s_pb : s_pa;
        int first = 1;
        for (int r = g; r < NP; r += 2) {
            if (!(a0[r] | a1[r])) continue;
            part_pass(y, s->pt[r].ph, pi[r], a0[r], (a1[r] - a0[r]) >> 6, first);
            first = 0;
        }
        live[g] = !first;
    }
    for (int r = 0; r < NP; r++) s->pt[r].ph += pi[r] * N;
    int parts = live[0] | live[1];
    if (parts && !live[0]) for (int i = 0; i < N; i++) s_pa[i] = 0;
    if (parts && !live[1]) for (int i = 0; i < N; i++) s_pb[i] = 0;

    /* The integration adds slope + event before each sample, sample 0 included: start one slope early. */
    acl -= dcl;
    acr -= dcr;
    int32_t l0 = OPZ_ADSR_LEVEL(env);
    int32_t l1 = env_block(l0, OPZ_ADSR_COEF(env), OPZ_ADSR_STEP(env));
    pass_out(out, s_el, acl, dcl, l0, (l1 - l0) >> 6, s_pa, s_pb, parts);
    pass_out(out + 1, s_er, acr, dcr, l0, (l1 - l0) >> 6, s_pb, s_pa, parts);
    OPZ_ADSR_LEVEL(env) = l1;
}

OPZ_EXPORTS_INSTRUMENT();
