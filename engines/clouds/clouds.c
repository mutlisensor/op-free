/*
 * clouds — granular texture for FX1: the input is recorded into a long ring and played back as a cloud of short,
 * windowed, panned grains; notes on the FX track tune the grains, and a held note freezes the ring.
 *
 *   P1        size / density   one macro from sparse long grains to dense short clouds: grain length 600 ms -> 15 ms
 *                              and mean overlap 0.7 -> 8 grains, both exponential (P1 0: a 600 ms swell every
 *                              ~0.86 s, gaps between them; 128: 94 ms grains, 2.4 overlapping, a smooth cloud; 255:
 *                              15 ms grains, 8 overlapping, ~530 grains a second, a dense buzzing haze)
 *   P2        texture          0 = tight: grains read the last 10 ms, all in the centre. Up to 192 the reach grows
 *                              exponentially to 5 s (the grains pick their start anywhere in that stretch of the
 *                              past); the stereo spread grows to full width at 128. From 128 to 255 the cloud feeds
 *                              back into the ring (0 -> 0.8) and each grain is detuned at random (0 -> +-40 cents)
 *   pan       dry / wet        0 dry, rising linearly to fully wet at 128; 128 .. 255 stay fully wet (as plate)
 *   level     output level (x gain)
 *   pitch bend                 transposes new grains, +-2 semitones at full scale
 *   notes on the FX track      a note sets the pitch of every new grain relative to C4 (C3 = -1 octave, C5 = +1
 *                              octave; other octaves fold into that range); releasing it returns to the newest key
 *                              still held, and after the last key the pitch stays until the next note (C4 = back to
 *                              the input's pitch). A note held for 0.4 s or more freezes the ring: recording stops
 *                              and the grains keep playing what it holds, at the newest note's pitch (play other
 *                              keys over it to play the frozen cloud). Releasing every key unfreezes it
 *
 * The ring is mono (the input's L + R sum), 16-bit at half scale (so every multiply in the grain loop is 16 x 16):
 * 2^19 samples (11.9 s, 1 MB) on FX1. Grains read it at any speed with linear interpolation. A grain's position is
 * Q(32 - bits), so the whole uint32 range is one turn of the ring and it wraps by itself; within a block the grain
 * reads from a pointer and a Q16 offset, and the ring's first 192 samples are mirrored after its end so that a block's
 * reads (at most 150 samples) never need wrapping. A grain starts at a random delay behind the record head within the
 * reach set by P2, never closer than its pitch allows (a grain pitched up must not catch up with the record head
 * before it ends, a grain pitched down must not be overtaken by it) and, while frozen, never across the seam where the
 * newest and the oldest audio meet.
 *
 * Grains (16, in the FX buffer: the 204-byte state is too small) are rendered one at a time over the block into a
 * stereo accumulator. The window (Hann, a 129-point table) is evaluated at block boundaries and ramped linearly in
 * between together with the grain's pan gain, so a grain costs one interpolated read and two multiply-adds per sample.
 * Grain onsets are sample-accurate, with random intervals of 0.5 .. 1.5 x the mean.
 *
 * Record path: input / 2 + feedback (the last block's wet sum, high-passed at 50 Hz, low-passed at 5 kHz, x P2's
 * feedback gain) -> soft saturator (unity when quiet, at most 0 dBFS) -> ring. Output: grain sum, one block late,
 * x a limiter that looks one block ahead (gain = knee / peak, ramped between block ends, ~90 ms release, so dense
 * coherent clouds and runaway feedback stay at or below -1 dBFS without a gain step) -> dry / wet mix.
 *
 * Freezing and unfreezing take 8 blocks (12 ms): grains that would read across the seam (freezing) or audio about to
 * be overwritten (unfreezing) fade out meanwhile, the recording fades out (or, after a thaw, in) so that the ring has
 * no splice where it stopped, and new grains start only where they can play on in either mode.
 *
 * Init runs with interrupts masked: renderFX clears the grain table in its first block and the ring 32 KB a block
 * after that (34 blocks, 49 ms on FX1); the output is dry only until then.
 *
 * The filter and resonance knobs are the OS's own track filter (also on FX tracks); clouds never reads them.
 *
 * Clean-room design after the ideas of granular processors such as Mutable Instruments Clouds and Beads (MIT); no
 * code from either. Q15 ring, Q31 signals, no floating point, no libc, no 64-bit maths and no division (per-block
 * reciprocals by table and Newton steps). MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define BLK             OPZ_BLOCK_FRAMES
#define Q31_ONE         0x7fffffff
#define BITS_FX1        19                      /* 2^19 int16 = 1 MB of FX1's 2 MB pool (11.9 s) */
#define BITS_OTHER      16                      /* any other track: 128 KB (1.49 s), well inside FX2's 256 KB */
#define NG              16                      /* grain pool */
#define MAX_SPAWN       3                       /* grain starts per block at most */
#define PH_ONE          (1 << 24)               /* window phase: Q24, 128 table segments of 2^17 */
#define FREEZE_BLOCKS   276                     /* a note held 0.4 s freezes */
#define PEND_BLOCKS     8                       /* freeze / thaw hand-over (12 ms): grains that would break fade
                                                   out, the recording fades out or in */
#define CLEAR_WORDS     8192                    /* 32 KB of ring cleared per block */
#define HELD_MAX        8
#define N_MAX           26460                   /* longest grain, 600 ms */
#define MARGIN          192                     /* samples kept between a grain and the record head */
#define KNEE            0x1c7ae148              /* limiter knee: 0.89 (-1 dBFS) in the accumulator's Q29 */
#define GUARD           192                     /* ring samples mirrored after its end (3 blocks) */
#define LIM_REL         6                       /* limiter release: 1/64 of the way per block (~90 ms) */
#define FB_HP           32534                   /* feedback high-pass pole, 50 Hz (Q15) */
#define FB_LP           16700                   /* feedback low-pass coefficient, 5 kHz (Q15) */
#define SAT_MAX         0x5ff00000              /* saturator input clamp, just below 0.75 */
#define SAT_C3          19418                   /* 16/27 (Q15): z - 16/27 z^3 is flat at z = 0.75, 0.5 there */
#define RATE_MIN        16384                   /* grain rate limits, Q16: 0.25 .. 2.3 */
#define RATE_MAX        150733

/* Hann window sin^2(pi x), 129 points over the grain and a guard, Q15 */
static const int16_t k_win[130] = {
    0, 20, 79, 177, 315, 491, 705, 958, 1247, 1573, 1935, 2331, 2761, 3224, 3719, 4244, 4799, 5381, 5990, 6624,
    7281, 7961, 8660, 9379, 10114, 10864, 11628, 12403, 13187, 13980, 14778, 15580, 16383, 17187, 17989, 18787,
    19580, 20364, 21139, 21903, 22653, 23388, 24107, 24806, 25486, 26143, 26777, 27386, 27968, 28523, 29048, 29543,
    30006, 30436, 30832, 31194, 31520, 31809, 32062, 32276, 32452, 32590, 32688, 32747, 32767, 32747, 32688, 32590,
    32452, 32276, 32062, 31809, 31520, 31194, 30832, 30436, 30006, 29543, 29048, 28523, 27968, 27386, 26777, 26143,
    25486, 24806, 24107, 23388, 22653, 21903, 21139, 20364, 19580, 18787, 17989, 17187, 16384, 15580, 14778, 13980,
    13187, 12403, 11628, 10864, 10114, 9379, 8660, 7961, 7281, 6624, 5990, 5381, 4799, 4244, 3719, 3224, 2761, 2331,
    1935, 1573, 1247, 958, 705, 491, 315, 177, 79, 20, 0, 0
};

/* (k - 1) / k in Q15: a grain fading out with k blocks to go (a linear fade over the hand-over) */
static const int16_t k_fade[PEND_BLOCKS + 1] = { 0, 0, 16384, 21845, 24576, 26214, 27307, 28087, 28672 };

/* 1 / n in Q31 for n = 1 .. 64 (n = 1 just below 1): the slope of a grain's first, partial block */
static const int32_t k_rcp[64] = {
    2147483647, 1073741824, 715827883, 536870912, 429496730, 357913941, 306783378, 268435456, 238609294,
    214748365, 195225786, 178956971, 165191050, 153391689, 143165577, 134217728, 126322568, 119304647, 113025455,
    107374182, 102261126, 97612893, 93368854, 89478485, 85899346, 82595525, 79536431, 76695845, 74051160, 71582788,
    69273666, 67108864, 65075262, 63161284, 61356676, 59652324, 58040099, 56512728, 55063683, 53687091, 52377650,
    51130563, 49941480, 48806447, 47721859, 46684427, 45691141, 44739243, 43826197, 42949673, 42107523, 41297762,
    40518559, 39768216, 39045157, 38347922, 37675152, 37025580, 36398028, 35791394, 35204650, 34636833, 34087042,
    33554432
};

typedef struct Grain {
    uint32_t rd, inc;                           /* read position and step, Q(32 - bits) */
    int32_t ph, dph;                            /* window phase and step, Q24 */
    int32_t w;                                  /* window at the block start, Q31 */
    int32_t left;                               /* samples still to play (for the safety checks) */
    int16_t gl, gr;                             /* pan x level, Q14 */
    uint8_t on, kill, start, pad;               /* kill: blocks of fade left; start: onset in its first block */
} Grain;

typedef struct Mem {                            /* the FX buffer: garbage until the first renderFX */
    Grain g[NG];
    int32_t acc[2][2 * BLK];                    /* grain sums, interleaved, Q29: this block's and the last one's */
    int32_t fbk[BLK];                           /* last block's wet sum (mono, Q31): the feedback */
    int16_t ring[];                             /* len + GUARD: ring[len + i] mirrors ring[i] */
} Mem;

#define MEM_BYTES(bits) ((uint32_t)sizeof(Mem) + (((uint32_t)1 << (bits)) + GUARD) * 2)

typedef struct State {
    Mem *m;
    uint32_t len;                               /* ring samples, a power of two; 0 = no buffer (dry only) */
    uint32_t w;                                 /* record head (a multiple of 64) */
    uint32_t cleared;                           /* 32-bit words of the ring cleared so far */
    int32_t next;                               /* samples from this block's start to the next grain onset */
    uint32_t seed;
    int32_t lim;                                /* limiter gain, Q31 */
    int32_t need;                               /* the gain the last block's grain sum needs, Q31 */
    int32_t dg, wg;                             /* dry and wet output gains of the last block, Q31 */
    int32_t gfb;                                /* feedback gain of the last block, Q31 */
    int32_t rec;                                /* record level at the end of the last block, Q31 */
    int32_t hx, hy, ly;                         /* feedback high-pass input / output, low-pass output */
    int32_t semis;                              /* latched note interval, Q16 semitones */
    uint16_t hold;                              /* blocks the newest note has been held (up to FREEZE_BLOCKS) */
    uint8_t fb;                                 /* 32 - log2(len): fraction bits of a read position */
    uint8_t frozen, pend, started, nheld, ab;   /* ab: the grain sum being made */
    uint8_t held[HELD_MAX];
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = MEM_BYTES(BITS_FX1);          /* 1,050,176 B on the device */
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)arg2; (void)scratch;
    State *s = state;
    volatile uint8_t *p = (volatile uint8_t *)s;               /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    /* FX1's pool is 2 MB. The OS does not enforce the trackmask and the FX pools sit back to back: anywhere else use
     * 128 KB (FX2's pool is 256 KB), and nothing on master (16 KB) */
    int bits = track == OPZ_TRACK_FX1 ? BITS_FX1 : BITS_OTHER;
    if (mem && track != OPZ_TRACK_MASTER) {
        s->m = (Mem *)mem;                                     /* header and ring are set up in renderFX */
        s->len = (uint32_t)1 << bits;
    }
    s->fb = (uint8_t)(32 - bits);
    s->seed = 0x2545f491u;
    s->lim = s->need = Q31_ONE;
}

static int fold(int note)
{
    int d = note - 60;                                         /* relative to C4, into -12 .. +12 */
    while (d > 12) d -= 12;
    while (d < -12) d += 12;
    return d;
}

static void unhold(State *s, uint8_t n)
{
    int j = 0;
    for (int i = 0; i < s->nheld; i++)
        if (s->held[i] != n) s->held[j++] = s->held[i];
    s->nheld = (uint8_t)j;
}

/* params may be NULL (a held note handed back with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                        /* 255: plug selected, no note */
    uint8_t n = (uint8_t)note;
    if (velocity == 0) s->nheld = 0;                           /* the OS hands back the one note still held */
    else if (!s->frozen && !s->pend) s->hold = 0;              /* a new note: the freeze count starts again */
    unhold(s, n);
    if (s->nheld == HELD_MAX) unhold(s, s->held[0]);
    s->held[s->nheld++] = n;
    s->semis = fold(note) * 65536;                              /* the newest note sets the pitch, and it stays */
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note < 0 || note > 127) return;
    unhold(s, (uint8_t)note);
    if (s->nheld) s->semis = fold(s->held[s->nheld - 1]) * 65536;   /* back to the newest key still held */
}

/* ---- grains ------------------------------------------------------------------------------------------------- */

#define ALWAYS_INLINE static inline __attribute__((always_inline))

/* Test hooks (engines/clouds/tests/fuzz.c defines them before including this file); nothing on the device. */
#ifndef CLOUDS_HOOK_GRAIN
#define CLOUDS_HOOK_GRAIN(g) ((void)0)
#define CLOUDS_HOOK_READ(q, wl, wr) ((void)0)
#define CLOUDS_HOOK_WRITE(ix, level) ((void)0)
#endif

/* The grain loop's arithmetic on 16-bit halves (the host versions give the same bits):
 *   mul_lh(x, w)        x.L * w.H * 2: a ring sample by the high half of a Q31 gain ramp, one instruction (only
 *                       -1 * -1 saturates, which the half-scale ring never gives)
 *   lerp_lo(a, b, rel)  a + (b - a) f in the low half (the high half is not defined), f = the Q16 fraction of rel
 *                       to 15 bits: four instructions (a 16-bit truncating multiply, as (d f) >> 15). b - a fits 16
 *                       bits because the ring is at half scale */
#if defined(__bfin__)
static inline int32_t mul_lh(int32_t x, int32_t w)
{
    int32_t r;
    __asm__("%0 = %1.L * %2.H;" : "=d"(r) : "d"(x), "d"(w));
    return r;
}
static inline int32_t lerp_lo(int32_t a, int32_t b, uint32_t rel)
{
    int32_t f, d;
    __asm__("%1 = %2 - %3;\n\t%0.L = %4.L >> 1;\n\t%1.L = %1.L * %0.L (T);\n\t%1.L = %1.L + %3.L (NS);"
            : "=&d"(f), "=&d"(d) : "d"(b), "d"(a), "d"(rel));
    return d;
}
#else
static inline int32_t mul_lh(int32_t x, int32_t w) { return (int32_t)(int16_t)x * (int16_t)(w >> 16) * 2; }
static inline int32_t lerp_lo(int32_t a, int32_t b, uint32_t rel)
{
    return (int16_t)(a + (((b - a) * (int32_t)((rel & 0xffff) >> 1)) >> 15));
}
#endif

typedef struct Spawn {                          /* this block's grain settings, from P1, P2, the bend and the notes */
    int32_t n;                                  /* grain length, samples */
    int32_t dph;                                /* window step, Q24 */
    int32_t gain;                               /* level per grain (overlap normalisation x sqrt 2), Q30 */
    int32_t reach;                              /* position spread, samples */
    int32_t spread;                             /* stereo spread, Q15 */
    int32_t semis;                              /* note + bend, Q16 semitones */
    int32_t jitter;                             /* random detune, Q16 semitones (+-) */
} Spawn;

/* Window at phase ph (Q24, 0 .. PH_ONE), Q31. */
static int32_t window(int32_t ph)
{
    int32_t i = ph >> 17, f = (ph >> 1) & 0xffff;
    int32_t a = k_win[i], b = k_win[i + 1];
    return (a << 16) + (b - a) * f;
}

static uint32_t rnd(State *s)
{
    s->seed = s->seed * 1664525u + 1013904223u;
    return s->seed >> 16;                                      /* 0 .. 65535 */
}

/* Will grain g, once the ring is frozen (fz) or recording, be able to play to its end? Until the change the grain
 * still reads `reads` samples and, unless the ring is frozen now, `writes` more samples are recorded. d: the grain's
 * delay behind the record head at the change. Recording, a grain faster than 1 closes in on the head by (r - 1) per
 * sample, a slower one falls back by (1 - r) towards audio about to be overwritten; frozen, every grain closes in on
 * the seam behind the head by r per sample. Spawning keeps every new grain within these limits, so they only need
 * checking when the mode changes. */
static int grain_ok(const State *s, const Grain *g, int fz, uint32_t reads, uint32_t writes)
{
    uint32_t one = 1u << s->fb;
    uint32_t d = ((s->w << s->fb) - g->rd) >> s->fb;
    uint32_t move = (g->inc * reads) >> s->fb;                 /* how far it reads meanwhile */
    if (!s->frozen) d += writes;
    if (d < move + 2) return 0;
    d -= move;
    /* + two blocks: a grain ends at the end of the block in which its window does, a little after `left` says */
    uint32_t left = (g->left > (int32_t)reads ? (uint32_t)g->left - reads : 0) + 2 * BLK;
    if (fz) return d >= ((g->inc * left) >> s->fb) + BLK + 2;
    if (g->inc > one) {
        if (d < (((g->inc - one) * left) >> s->fb) + BLK + 2) return 0;
    } else if (d < BLK + 2) return 0;
    uint32_t grow = g->inc < one ? ((one - g->inc) * left) >> s->fb : 0;
    return d + grow + 2 * BLK + 2 <= s->len;
}

/* A grain starting k samples into this block (after its recording). During a hand-over (pend blocks left, this one
 * included) it must also be able to play on in the mode that follows (want). */
static void spawn(State *s, const Spawn *p, int k, int want)
{
    Mem *m = s->m;
    Grain *g = 0;
    for (int i = 0; i < NG; i++)
        if (!m->g[i].on) { g = &m->g[i]; break; }
    if (!g) return;                                            /* pool full: skip this one */

    uint32_t r1 = rnd(s), r2 = rnd(s), r3 = rnd(s);
    int32_t semis = p->semis + (int32_t)(((int32_t)r1 - 32768) * (p->jitter >> 1) >> 14);
    int32_t rate = opz_exp2_q16(opz_mul_q31(semis, 178956971));     /* 2^(semis / 12), Q16 */
    if (rate < RATE_MIN) rate = RATE_MIN;
    if (rate > RATE_MAX) rate = RATE_MAX;
    int32_t n = p->n;
    int32_t rn = (int32_t)opz_mul_u32_shr16((uint32_t)rate, (uint32_t)n);  /* samples the grain reads */

    int32_t lo, hi;                                            /* allowed delay at the onset */
    if (s->frozen) {
        lo = rn + MARGIN;
        hi = (int32_t)s->len - BLK;
    } else {
        lo = (rn > n ? rn - n : 0) + MARGIN;
        hi = (int32_t)s->len - 4 * BLK - (rn < n ? n - rn : 0);
    }
    if (lo > hi) return;
    int32_t reach = p->reach < hi - lo ? p->reach : hi - lo;
    int32_t d = lo + (int32_t)opz_mul_u32_shr16((uint32_t)reach, r2);

    g->inc = s->fb >= 16 ? (uint32_t)rate << (s->fb - 16) : (uint32_t)rate >> (16 - s->fb);
    g->rd = (s->w - (uint32_t)d) << s->fb;
    g->ph = 0;
    g->dph = p->dph;
    g->w = 0;
    g->left = n;
    /* equal-power pan: 0.5 +- spread / 2 of a quarter turn */
    int32_t pan = 0x20000000 + ((int32_t)r3 - 32768) * (p->spread >> 1);              /* Q30, 0 .. 2^30 */
    g->gl = (int16_t)(opz_mul_q31(opz_sine((uint32_t)(0x40000000 - pan)), p->gain) >> 16);
    g->gr = (int16_t)(opz_mul_q31(opz_sine((uint32_t)pan), p->gain) >> 16);
    g->kill = 0;
    g->start = (uint8_t)k;
    g->on = s->pend == 0 || grain_ok(s, g, want, (uint32_t)s->pend * BLK, (uint32_t)(s->pend - 1) * BLK);
}

/* One grain over the block: an interpolated read at p + rel (Q16; or a plain one at rate 1 on a whole sample), x the
 * ramped window-and-pan gains (Q30, used to 16 bits), added into the accumulator. Ring samples are at half scale, so
 * the difference of two fits 16 bits and all three multiplies are 16 x 16. */
ALWAYS_INLINE void grain_run(const int16_t *p, int32_t *acc, int i0, uint32_t rel, uint32_t inc,
                             int32_t wl, int32_t wr, int32_t dl, int32_t dr, int interp)
{
    if (!interp) {                                             /* rate 1 on a whole sample: walk the ring */
        const int16_t *q = p;
        for (int i = i0; i < BLK; i++) {
            CLOUDS_HOOK_READ(q, wl, wr);
            int32_t x = *q++;
            acc[2 * i] = opz_add_q31(acc[2 * i], mul_lh(x, wl));
            acc[2 * i + 1] = opz_add_q31(acc[2 * i + 1], mul_lh(x, wr));
            wl += dl;
            wr += dr;
        }
        return;
    }
    for (int i = i0; i < BLK; i++) {
        const int16_t *q = p + (rel >> 16);
        CLOUDS_HOOK_READ(q, wl, wr);
        int32_t x = lerp_lo(q[0], q[1], rel);
        acc[2 * i] = opz_add_q31(acc[2 * i], mul_lh(x, wl));
        acc[2 * i + 1] = opz_add_q31(acc[2 * i + 1], mul_lh(x, wr));
        rel += inc;
        wl += dl;
        wr += dr;
    }
}

static void grain_block(State *s, Grain *g)
{
    CLOUDS_HOOK_GRAIN(g);
    int k = g->start, n = BLK - k;
    int32_t ph = g->ph + n * g->dph;
    if (ph > PH_ONE) ph = PH_ONE;
    g->ph = ph;
    int32_t w1 = window(ph);
    if (g->kill) {                                             /* fading out: linearly to 0 over the blocks left, */
        int32_t f = opz_mul_q31_q15(g->w, k_fade[g->kill]);    /* or sooner if the window ends first */
        if (f < w1) w1 = f;
        g->kill--;
    }
    /* window x pan, Q30 (the gains are Q14): ramps over the n samples */
    int32_t l0 = opz_mul_q31(g->w, g->gl << 16), l1 = opz_mul_q31(w1, g->gl << 16);
    int32_t r0 = opz_mul_q31(g->w, g->gr << 16), r1 = opz_mul_q31(w1, g->gr << 16);
    int32_t dl, dr;
    if (n == BLK) {
        dl = (l1 - l0) >> 6;
        dr = (r1 - r0) >> 6;
    } else {
        dl = opz_mul_q31(l1 - l0, k_rcp[n - 1]);
        dr = opz_mul_q31(r1 - r0, k_rcp[n - 1]);
    }
    /* this block's reads: from the sample at the position, with the fraction and the step in Q16 (fb <= 16) */
    const int16_t *p = s->m->ring + (g->rd >> s->fb);
    uint32_t rel = (g->rd << (32 - s->fb)) >> 16, inc = g->inc << (16 - s->fb);
    int32_t *acc = s->m->acc[s->ab];
    if (inc == 0x10000 && rel == 0) grain_run(p, acc, k, rel, inc, l0, r0, dl, dr, 0);
    else grain_run(p, acc, k, rel, inc, l0, r0, dl, dr, 1);
    g->rd += g->inc * (uint32_t)n;
    g->w = w1;
    g->left -= n;
    g->start = 0;
    if (w1 == 0) g->on = 0;                                    /* window done, or the fade has ended */
}

/* ---- render ------------------------------------------------------------------------------------------------- */

/* Record one block (input / 2 + filtered feedback, soft-saturated) at the record head; fading: the record level
 * ramps (into and out of a freeze, and after the clearing), so the recording has no splice where it stopped. */
ALWAYS_INLINE void record_run(State *s, const fract32 *in, int32_t g0, int32_t g1, int32_t r0, int32_t r1, int fading,
                              int feedback)
{
    int16_t *t = s->m->ring + s->w;
    const int32_t *fbk = s->m->fbk;
    int32_t hx = s->hx, hy = s->hy, ly = s->ly;
    int32_t g = g0, ginc = (g1 - g0) >> 6;
    int32_t rg = r0, rinc = (r1 - r0) >> 6;
    (void)rg; (void)rinc;
    for (int i = 0; i < BLK; i++) {
        int32_t x = (in[2 * i] >> 2) + (in[2 * i + 1] >> 2);  /* (L + R) / 4: the ring holds half scale */
        int32_t z = x;
        if (feedback) {
            ly += opz_mul_q31_q15((fbk[i] >> 1) - ly, FB_LP);  /* the feedback filters run at half scale too */
            hy = opz_mul_q31_q15(opz_add_q31(hy, opz_sub_q31(ly, hx)), FB_HP);
            hx = ly;
            z = opz_add_q31(x, opz_mul_q31_q15(hy, g >> 16));
            g += ginc;
        }
        if (fading) {
            z = opz_mul_q31_q15(z, rg >> 16);
            rg += rinc;
        }
        z = z > SAT_MAX ? SAT_MAX : (z < -SAT_MAX ? -SAT_MAX : z);
        int32_t zh = z >> 16;
        int32_t y = z - opz_mul_q31_q15(opz_mul_q31_q15(opz_mul_q31_q15(z, zh), zh), SAT_C3);    /* |y| < 0.5 */
        t[i] = (int16_t)(y >> 16);                             /* half scale: -16384 .. 16383 */
        CLOUDS_HOOK_WRITE(s->w + i, fading ? rg : Q31_ONE);
    }
    if (s->w < GUARD) {                                        /* the mirror after the end */
        int16_t *u = t + s->len;
        for (int i = 0; i < BLK; i++) u[i] = t[i];
    }
    if (!feedback) hx = hy = ly = 0;
    s->hx = hx; s->hy = hy; s->ly = ly;
    s->w = (s->w + BLK) & (s->len - 1);
}

static void record(State *s, const fract32 *in, int32_t g0, int32_t g1, int32_t r0, int32_t r1)
{
    int fading = r0 != Q31_ONE || r1 != Q31_ONE, feedback = g0 != 0 || g1 != 0;
    if (fading) {
        if (feedback) record_run(s, in, g0, g1, r0, r1, 1, 1);
        else record_run(s, in, g0, g1, r0, r1, 1, 0);
    } else {
        if (feedback) record_run(s, in, g0, g1, r0, r1, 0, 1);
        else record_run(s, in, g0, g1, r0, r1, 0, 0);
    }
}

/* Output: dry x in + wet x the (limited) grain sum acc; the wet sum before level and pan is the next block's
 * feedback. */
ALWAYS_INLINE void output_run(Mem *m, const int32_t *acc, const fract32 *in, fract32 *out, int32_t lim, int32_t linc,
                              int32_t dg, int32_t dinc, int32_t wg, int32_t winc, int limiting)
{
    for (int i = 0; i < BLK; i++) {
        int32_t l = acc[2 * i], r = acc[2 * i + 1];
        if (limiting) {
            l = opz_mul_q31_q15(l, lim >> 16);
            r = opz_mul_q31_q15(r, lim >> 16);
            lim += linc;
        }
        l = opz_shl_q31(l, 2);                                 /* Q29 -> Q31, saturating */
        r = opz_shl_q31(r, 2);
        m->fbk[i] = (l >> 1) + (r >> 1);
        int32_t d16 = dg >> 16, w16 = wg >> 16;
        out[2 * i] = opz_add_q31(opz_mul_q31_q15(in[2 * i], d16), opz_mul_q31_q15(l, w16));
        out[2 * i + 1] = opz_add_q31(opz_mul_q31_q15(in[2 * i + 1], d16), opz_mul_q31_q15(r, w16));
        dg += dinc;
        wg += winc;
    }
}

static void output(Mem *m, const int32_t *acc, const fract32 *in, fract32 *out, int32_t lim, int32_t linc,
                   int32_t dg, int32_t dinc, int32_t wg, int32_t winc)
{
    if (lim == Q31_ONE && linc == 0) output_run(m, acc, in, out, lim, linc, dg, dinc, wg, winc, 0);
    else output_run(m, acc, in, out, lim, linc, dg, dinc, wg, winc, 1);
}

/* This block's grain sum, and the limiter gain to reach by the end of the block that plays now (the last one's sum,
 * one block of look-ahead): knee / peak for the block that plays and for the one after it, else the release (~90 ms).
 * The gain ramps linearly between block ends, so it is at or below what each block needs at every sample. */
static int32_t grains(State *s)
{
    Mem *m = s->m;
    int32_t *acc = m->acc[s->ab];
    for (int i = 0; i < 2 * BLK; i++) acc[i] = 0;
    for (int i = 0; i < NG; i++)
        if (m->g[i].on) grain_block(s, &m->g[i]);
    int32_t pk = 0;
    for (int i = 0; i < 2 * BLK; i++) {
        int32_t a = opz_abs32(acc[i]);
        if (a > pk) pk = a;
    }
    int32_t need = Q31_ONE;
    if (pk > KNEE) {
        int nn = opz_norm32(pk);                               /* pk << nn in [2^30, 2^31) */
        need = opz_mul_q31(KNEE, opz_recip_q31(pk << nn)) << (nn + 1);
    }
    int32_t lim1 = s->lim + ((Q31_ONE - s->lim) >> LIM_REL);
    if (Q31_ONE - lim1 < 0x10000) lim1 = Q31_ONE;               /* released: back to the plain path */
    if (s->need < lim1) lim1 = s->need;
    if (need < lim1) lim1 = need;
    s->need = need;
    return lim1;
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    State *s = state;
    Mem *m = s->m;

    /* dry / wet (0 dry .. 128 wet, then wet) and level, ramped over the block */
    int32_t pan = opz_pq31(params[OPZ_P_PAN]);
    int32_t wet = pan >= 0x40000000 ? Q31_ONE : pan * 2;
    int32_t og = opz_mul_q31(opz_pq31(params[OPZ_P_LEVEL]), gain);
    int32_t dg1 = opz_mul_q31(Q31_ONE - wet, og), wg1 = opz_mul_q31(wet, og);
    int32_t dinc = (dg1 - s->dg) >> 6, winc = (wg1 - s->wg) >> 6;

    if (!m || !s->started || s->cleared < (s->len + GUARD) / 2) {  /* no buffer, or still clearing it: dry only */
        if (m) {
            if (!s->started) {                                 /* the header is garbage until now */
                volatile uint8_t *h = (volatile uint8_t *)m;
                for (unsigned i = 0; i < sizeof(Mem); i++) h[i] = 0;
                s->started = 1;
            } else {
                volatile uint32_t *z = (volatile uint32_t *)m->ring + s->cleared;
                uint32_t n = (s->len + GUARD) / 2 - s->cleared;
                if (n > CLEAR_WORDS) n = CLEAR_WORDS;
                for (uint32_t i = 0; i < n; i++) z[i] = 0;
                s->cleared += n;
            }
        }
        int32_t dg = s->dg;
        for (int i = 0; i < BLK; i++) {
            out[2 * i] = opz_mul_q31(in[2 * i], dg);
            out[2 * i + 1] = opz_mul_q31(in[2 * i + 1], dg);
            dg += dinc;
        }
        s->dg = dg1;
        s->wg = 0;
        return;
    }

    /* notes: the newest one held 0.4 s freezes; releasing every note thaws. A change waits PEND_BLOCKS while the
     * grains that could not survive it fade out (linearly), and new grains must be able to live in both modes. */
    if (s->nheld) {
        if (s->hold < FREEZE_BLOCKS) s->hold++;
    } else s->hold = 0;
    int want = s->nheld && s->hold >= FREEZE_BLOCKS;
    if (want == s->frozen) s->pend = 0;
    else if (!s->pend) {
        s->pend = PEND_BLOCKS;
        for (int i = 0; i < NG; i++) {
            Grain *g = &m->g[i];
            if (g->on && !g->kill && !grain_ok(s, g, want, PEND_BLOCKS * BLK, PEND_BLOCKS * BLK))
                g->kill = PEND_BLOCKS;
        }
    } else if (!--s->pend) s->frozen = (uint8_t)want;

    /* P1: grain length 600 ms x 2^(-5.32 u), overlap 0.7 x 2^(3.51 u), so the mean interval is 857 ms x 2^(-8.83 u);
     * the level per grain is sqrt(2) x min(1, 1.6 / sqrt(overlap)) */
    uint32_t u1 = (uint32_t)opz_p16(params[OPZ_P_P1]), u2 = (uint32_t)opz_p16(params[OPZ_P_P2]);
    Spawn p;
    int32_t e1 = (int32_t)opz_mul_u32_shr16(u1, 348652);       /* 5.32 u, Q16 octaves */
    p.n = (int32_t)opz_mul_u32_shr16(N_MAX, (uint32_t)opz_exp2_q16(-e1));
    p.dph = (int32_t)(opz_mul_u32_shr16(41553727u, (uint32_t)opz_exp2_q16(e1)) >> 16);   /* 2^24 / n */
    int32_t lo2 = -33723 + (int32_t)opz_mul_u32_shr16(u1, 230331);                      /* log2 overlap, Q16 */
    int32_t eg = 44433 - (lo2 > 0 ? lo2 >> 1 : 0);             /* log2(1.6 / sqrt overlap), up to 0.68 */
    if (eg > 0) eg = 0;
    p.gain = opz_exp2_q16(eg + 32768) << 14;                   /* x sqrt 2, Q30 */
    int32_t mean = (int32_t)opz_mul_u32_shr16(37800, (uint32_t)opz_exp2_q16(-(int32_t)opz_mul_u32_shr16(u1, 578983)));
    if (mean < 48) mean = 48;

    /* P2: reach 10 ms x 2^(9 min(u, 0.75) / 0.75), stereo spread 0 -> 1 at 128; from 128 feedback 0 -> 0.8 and detune
     * 0 -> +-40 cents */
    uint32_t ur = u2 < 49152 ? u2 : 49152;
    p.reach = (int32_t)opz_mul_u32_shr16(441, (uint32_t)opz_exp2_q16((int32_t)(ur * 12)));
    p.spread = u2 < 32768 ? (int32_t)u2 : 32767;
    int32_t up = u2 > 32896 ? (int32_t)u2 - 32896 : 0;        /* 0 at P2 128 .. 32639 at 255 */
    int32_t gfb1 = s->frozen ? 0 : up * 52428;                 /* 0.8 (Q31) at 255 */
    p.jitter = (up * 26214) >> 15;                             /* 0.4 semitones */
    p.semis = s->semis + (pitchBend >> 14);                    /* bend: full scale = 2 semitones */

    /* record, unless frozen: the level fades out over the blocks before a freeze and in after a thaw (and after the
     * clearing), 1/8 a block */
    if (!s->frozen) {
        int32_t rec1 = want && s->pend ? (s->pend - 1) << 28                                  /* 1/8 a block */
                                       : (s->rec >= 0x70000000 ? Q31_ONE : s->rec + 0x10000000);
        record(s, in, s->gfb, gfb1, s->rec, rec1);
        s->rec = rec1;
    } else s->rec = 0;
    s->gfb = gfb1;

    /* grain onsets: random intervals of 0.5 .. 1.5 x the mean; a long wait shortens when P1 asks for more */
    if (s->next > mean + (mean >> 1)) s->next = mean + (mean >> 1);
    int spawned = 0;
    while (s->next < BLK) {
        if (spawned < MAX_SPAWN) spawn(s, &p, s->next, want);
        spawned++;
        s->next += (mean >> 1) + (int32_t)opz_mul_u32_shr16((uint32_t)mean, rnd(s));
    }
    s->next -= BLK;

    /* the grains, one after the other, then the limiter and the output */
    int32_t lim1 = grains(s);
    output(m, m->acc[s->ab ^ 1], in, out, s->lim, (lim1 - s->lim) >> 6, s->dg, dinc, s->wg, winc);
    s->ab ^= 1;
    s->lim = lim1;
    s->dg = dg1;
    s->wg = wg1;
}

OPZ_EXPORTS_FX();
