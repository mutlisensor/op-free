/*
 * shimmer — a lush stereo reverb with a pitch shifter in its loop (Eventide/Strymon-style shimmer) for FX1/FX2: a
 * Dattorro figure-8 tank (as engines/plate) whose loop passes a two-tap crossfading delay-line shifter once per lap,
 * so every lap lifts part of the tail by the interval (an octave by default) and the tail climbs.
 *
 *   P1        size        one macro over size, decay and pre-delay, as plate: 0 = a small, short room (tank 0.3x,
 *                         T60 0.5 s, pre-delay 3 ms) -> 240 = a huge hall (1.36x, T60 ~20 s, 40 ms) -> 255 endless
 *                         (1.5x, 45 ms). Size changes glide (at most 1/8 sample per sample), so turning P1 bends the
 *                         tail like tape instead of clicking
 *   P2        shimmer     how much of the loop goes through the shifter each lap, a = 0.8 (u + u^2) / 2: 0 = a plain
 *                         reverb; 64 a faint halo (2 % of the power per lap shifted); 128 a clear octave above the
 *                         tail (15 %); 192 the octave as loud as the tail (56 %), which climbs on; 255 nearly all of
 *                         it moves up each lap (94 %): a rising cascade that thins out into the highs. The shifted
 *                         path darkens a little as P2 rises (8 -> 5.7 kHz), so the cascade stays smooth
 *   pan       dry / wet   0 dry .. 128 fully wet; 128 .. 255 stay fully wet (as plate)
 *   level     output level (x gain)
 *   notes     on the FX track: the shift interval, the key's distance from C3: C4 = +12 (an octave, the default),
 *             G3 = +7 (a fifth), F3 = +5, C5 = +24, G4 = +19, C3 = 0 (shimmer off: a plain reverb), keys below C3
 *             shift down (C2 = -12). Clamped to -12 .. +24. The newest note wins and the interval stays after the
 *             release (it's a setting, not a gate). Velocity doesn't matter; note 255 (plug selected) and the OS's
 *             velocity-0 re-send of a held note (params NULL) are handled
 *
 * Signal path: input -> DC blocker -> pre-delay -> band limit (10 kHz) -> 3 input diffusers per channel -> tank. The
 * tank's two halves each run a modulated all-pass, a delay, a damping low-pass (6 kHz), a decay gain, an all-pass
 * and a second delay, and feed each other (the figure 8). In half 0, the first delay's output is split: the
 * shifter branch (high-pass 90 Hz, a two-pole low-pass that keeps the faster reads from aliasing, then an
 * 8,192-sample ring read by two taps whose delays sweep across a 6,144-sample window (139 ms) half a window apart,
 * crossfaded by triangles) and the direct path are mixed as (1 - a) direct + a sqrt(3/2) shifted, then go on into
 * the damping. The sqrt(3/2) makes up the power the triangle crossfade of two unrelated taps loses (a third on
 * average), and the decay gain after the mix makes up what the mix of two unrelated signals loses (up to 3 dB at
 * a = 0.5), capped below 1. So a lap keeps the power of everything the shifter shifts, and the T60 holds; what
 * climbs ends in the damping, which drains the tail faster the more P2 shifts. Nothing can build up on one
 * frequency: the shifted part is at a new pitch every lap, and at unison (C3) the shifter is muted. Over that,
 * plate's limiter holds the endless top of P1 (or any build-up) below 0 dBFS.
 *
 * Output: Dattorro's 7 taps per channel from both halves. The ring of 32-bit tank lines is shared, every line in its
 * own region, processed a block at a time on contiguous stretches (see plate.c); the shifter ring (32-bit too) is a
 * power of two and indexed per sample with a mask. The second all-pass of each half keeps its length (Dattorro's
 * at 0.75x) as the size moves: one interpolated line less per half while P1 glides.
 *
 * The filter and resonance knobs are the OS's own track filter; shimmer never reads them. Pitch bend is not used.
 *
 * J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES 45(9), 1997 (tank topology, delay
 * lengths, output taps). The two-tap triangle-crossfaded delay shifter is the classic one, as in Mutable Instruments
 * Clouds' pitch shifter (MIT); no code from it. Q31 signals, Q15 coefficients; no floating point, no libc, no int64
 * or division per sample. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_engine.h"
#include "opz_dsp.h"

#define BLK             OPZ_BLOCK_FRAMES

/* ---- the tank ring (as plate) ---------------------------------------------------------------------------- */

#define S_MIN           19661                   /* size 0.3, Q16 */
#define S_MAX           98304                   /* size 1.5, Q16 */
#define EXC_MAX         34                      /* largest all-pass excursion (samples), with room */
#define PD_MAX          2000                    /* pre-delay (samples) */
#define SPAN(n)         ((n) + BLK + 8)         /* a line of up to n samples: its history and one block of writes */
#define SPAN_S(n)       SPAN((n) * 3 / 2 + 2)   /* a line that scales with the size, up to S_MAX */

/* Dattorro's lengths x 44100 / 29761. Half 0 is his left half (672, 4453, 1800, 3720), half 1 his right. */
#define L0_AP1          996
#define L0_D1           6598
#define L0_AP2          2000                    /* the +0.5 all-passes don't follow the size: Dattorro's 2667 and */
#define L0_D2           5512
#define L1_AP1          1346
#define L1_D1           6249
#define L1_AP2          2952                    /* 3936 at 0.75x (one interpolated line less per half while P1 moves) */
#define L1_D2           4687

enum {
    R0_AP1 = SPAN_S(L0_AP1) + EXC_MAX,
    R0_D1 = R0_AP1 + SPAN_S(L0_D1),
    R0_AP2 = R0_D1 + SPAN(2732),                /* far enough for its output taps (1820 x 1.5) */
    R0_D2 = R0_AP2 + SPAN_S(L0_D2),
    R1_AP1 = R0_D2 + SPAN_S(L1_AP1) + EXC_MAX,
    R1_D1 = R1_AP1 + SPAN_S(L1_D1),
    R1_AP2 = R1_D1 + SPAN(4255),                /* (2835 x 1.5) */
    R1_D2 = R1_AP2 + SPAN_S(L1_D2),
    R_DL0 = R1_D2 + SPAN(210), R_DL1 = R_DL0 + SPAN(562), R_DL2 = R_DL1 + SPAN(410),
    R_DR0 = R_DL2 + SPAN(223), R_DR1 = R_DR0 + SPAN(593), R_DR2 = R_DR1 + SPAN(433),
    R_PDL = R_DR2 + SPAN(PD_MAX),
    R_PDR = R_PDL + SPAN(PD_MAX),
    RING = R_PDR + BLK                          /* 52,795 samples */
};

#define NL              8
#define FIXED(k)        (((k) & 3) == 2)        /* the +0.5 all-passes keep their length */
static const int16_t k_base[NL] = { L0_AP1, L0_D1, L0_AP2, L0_D2, L1_AP1, L1_D1, L1_AP2, L1_D2 };
static const uint16_t k_region[NL] = { R0_AP1, R0_D1, R0_AP2, R0_D2, R1_AP1, R1_D1, R1_AP2, R1_D2 };

#define NT              14
static const int16_t k_tap[NT] = {
    394, 4407, 2835, 2958, 2949, 277, 1580,     /* left:  1.d1 1.d1 1.ap2 1.d2 0.d1 0.ap2 0.d2 */
    523, 5375, 1820, 3961, 3128, 496, 179,      /* right: 0.d1 0.d1 0.ap2 0.d2 1.d1 1.ap2 1.d2 */
};
static const uint16_t k_tap_reg[NT] = {
    R1_D1, R1_D1, R1_AP2, R1_D2, R0_D1, R0_AP2, R0_D2,
    R0_D1, R0_D1, R0_AP2, R0_D2, R1_D1, R1_AP2, R1_D2,
};
#define XF              8                       /* blocks of a tap crossfade (12 ms) */
static const int16_t k_xf[XF + 1] = { 0, 5246, 10365, 15247, 19778, 23862, 27424, 30406, 32767 };

/* Input diffusers: Dattorro's 142, 379, 277 (x 1.4818; his 107 left out) on the left, slightly longer on the right */
#define ND              3
static const int16_t k_dif_len[2 * ND] = { 210, 562, 410, 223, 593, 433 };
static const uint16_t k_dif_reg[2 * ND] = { R_DL0, R_DL1, R_DL2, R_DR0, R_DR1, R_DR2 };
#define DIF_G1          24576                   /* 0.75, Q15 (the first) */
#define DIF_G2          20480                   /* 0.625 (the others) */
#define DD1             22938                   /* 0.70: decay diffusion 1, the modulated all-passes (negative) */
#define DD2             16384                   /* 0.50: decay diffusion 2 */

/* Fixed tone (P2 is the shimmer): input band limit 10 kHz, damping 6 kHz, one-pole 1 - e^(-2 pi fc / fs), Q15;
 * all-pass excursion 12 samples at 0.7 Hz */
#define K_BAND          24887
#define K_DAMP          18828
#define EXC             (12 << 16)
#define LFO_INC         68173u

#define SLEW            79                      /* glide: Q16 samples per block per sample of base length */
#define IN_SCALE        0x40000000              /* the tank runs at -12 dB: input x 0.5, then x 0.5 here */
#define AGC_KNEE        0x0859999a              /* limiter knees (tap sum, Q31 / 8): -6 dBFS wet at full level */
#define TRIM_KNEE       0x0bcccccd              /* -3 dBFS */
#define G_TOP           32767                   /* loop gain cap, Q15 (1 - 2^-15) */
#define WET_SHIFT       5
#define WET_K           0x1eb851ec              /* wet = taps x 0.96 x 8 / 32 */
#define CLEAR_STEP      4096                    /* samples per block while the buffer is cleared (init masks IRQs) */
#define WIN             (BLK + 24)

/* ---- the shifter ------------------------------------------------------------------------------------------ */

#define SB_N            8192                    /* shifter ring, a power of two */
#define SB_MASK         (SB_N - 1)
#define SW              6144                    /* window: the taps' delays sweep 0 .. SW (139 ms, 7 Hz at +12) */
#define SW_INC_K        699051                  /* 2^32 / SW / 2^16: phase per sample = (ratio - 1) (Q16) x this >> 16 */
#define K_HP            420                     /* branch high-pass, one-pole 2 pi 90 / fs, Q15 */
#define LX_BRANCH       12371                   /* log2(2 pi 8 kHz / fs), Q16: the branch low-pass at P2 0, +12 */
#define A_MAX           26214                   /* shimmer mix at P2 255: 0.8, Q15 */
#define SH_GAIN         40132                   /* sqrt(3/2) x 2^15 (above 1: not a Q15 gain): makes up the power the
                                                   triangle crossfade of two unrelated taps loses (2/3 on average), so
                                                   the shifted path keeps the decay; a x SH_GAIN stays below 1 up to
                                                   A_MAX, so it is a Q15 gain again */
#define SEMI_DEFAULT    12
#define SEMI_MIN        (-12)
#define SEMI_MAX        24
#define NOTE_ZERO       48                      /* C3: unison */

typedef struct Mem {
    int32_t cur[NL];                            /* tank lengths, Q16 samples */
    int32_t tap[NT], tap_old[NT];               /* tap positions, samples: now, and before a crossfade */
    int32_t xl[BLK], xr[BLK];                   /* input, then diffused input, Q31; then old taps */
    int32_t u[BLK];                             /* a tank half, Q31; then taps */
    int32_t sh[BLK];                            /* half 0's first delay after the shifter mix */
    int32_t tb[NL][BLK];                        /* line outputs: interpolated, or copied where the ring wraps */
    int32_t tt[4][BLK];                         /* tap stretches that wrap */
    int32_t win[WIN], wtmp[2][BLK];             /* a stretch that wraps; writes that wrap */
    int32_t buf[RING + SB_N];                   /* the tank ring, then the shifter ring: cleared together */
} Mem;

#define MEM_BYTES       ((uint32_t)sizeof(Mem)) /* 249,052 B on the device: fits FX2's 256 KB pool */

typedef struct State {
    Mem *mem;
    uint32_t w;                                 /* ring position of this block's first write */
    uint32_t cleared;                           /* buffer samples cleared so far */
    uint32_t lfo;                               /* LFO phase */
    uint32_t ph;                                /* shifter window phase (tap 1; tap 2 is half a turn on) */
    uint32_t sbw;                               /* shifter ring write position */
    int32_t lp[2], bw[2];                       /* damping (tank halves), band limit (input L, R) */
    int32_t dcx[2], dcy[2];                     /* DC blockers */
    int32_t hp, b1, b2;                         /* shifter branch: high-pass, two low-pass poles */
    int32_t ig;                                 /* input gain reached, Q31 */
    int32_t agc;                                /* input ducking, Q31 */
    int32_t trim;                               /* loop gain trim, Q15 */
    int32_t wog, dog;                           /* wet and dry output gains reached, Q31 */
    int32_t m0, m1;                             /* all-pass modulation reached, Q16 samples */
    int32_t ts;                                 /* size the taps are at, Q16 */
    int32_t a;                                  /* shimmer mix reached, Q15 */
    int8_t semi;                                /* shift interval, semitones */
    uint8_t xf;                                 /* tap crossfade: blocks to go */
    uint8_t started;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* ---- helpers (as plate) ------------------------------------------------------------------------------------ */

#define MQ(x, c)        opz_mul_q31_q15((x), (c))

/* x times a Q15 c from x's top 16 bits only: one instruction instead of three, error < 2^-15. For the input
 * diffusers, which sit before the tank (outside its loop), so the error can't build up. */
#if defined(__bfin__)
static inline fract32 mul_hi(fract32 x, int32_t c) { return __builtin_bfin_mult_fr1x32((short)(x >> 16), (short)c); }
#else
static inline fract32 mul_hi(fract32 x, int32_t c)
{
    int32_t p = (int32_t)(int16_t)(x >> 16) * (int16_t)c;
    return p == 0x40000000 ? INT32_MAX : p * 2;
}
#endif

static inline int32_t wrap(int32_t p)
{
    if (p >= RING) p -= RING;
    if (p < 0) p += RING;
    return p;
}

static const int32_t *seg(const int32_t *r, int32_t pos, int n, int32_t *tmp)
{
    pos = wrap(pos);
    if (pos + n <= RING) return r + pos;
    for (int i = 0; i < n; i++) tmp[i] = r[pos + i < RING ? pos + i : pos + i - RING];
    return tmp;
}

static inline int32_t *wbeg(int32_t *r, int32_t pos, int32_t *tmp)
{
    pos = wrap(pos);
    return pos + BLK <= RING ? r + pos : tmp;
}
static inline void put(int32_t *r, int32_t pos, const int32_t *p, const int32_t *tmp)
{
    if (p != tmp) return;
    pos = wrap(pos);
    for (int i = 0; i < BLK; i++) r[pos + i < RING ? pos + i : pos + i - RING] = tmp[i];
}

/* A line's output for the block, delay d = d0 + (i + 1) dinc (Q16) for sample i: a pointer into the ring when d is
 * whole and still, else linear interpolation into buf (see plate.c). */
static const int32_t *line_out(Mem *m, int32_t pos, int32_t d0, int32_t dinc, int32_t *buf, int interp)
{
    if (!interp) return seg(m->buf, pos - (d0 >> 16), BLK, buf);
    int32_t d1 = d0 + BLK * dinc;
    int32_t hi = (d0 > d1 ? d0 : d1) >> 16, lo = (d0 < d1 ? d0 : d1) >> 16;
    int off = hi + 1;
    const int32_t *p = seg(m->buf, pos - off, BLK + off - lo + 1, m->win);
    int32_t q = ((off - 1) << 16) - d0, step = 65536 - dinc;
    for (int i = 0; i < BLK; i++) {
        q += step;
        const int32_t *r = p + (q >> 16);
        int32_t c = r[0];
        buf[i] = opz_add_q31(c, MQ(opz_sub_q31(r[1], c), (q & 0xffff) >> 1));
    }
    return buf;
}

static void allpass(Mem *m, const int32_t *x, const int32_t *t, int32_t pos, int32_t g, int32_t next)
{
    int32_t *wp = wbeg(m->buf, pos, m->wtmp[0]), *wq = wbeg(m->buf, next, m->wtmp[1]);
    for (int i = 0; i < BLK; i++) {
        int32_t tt = t[i];
        int32_t v = opz_add_q31(x[i], MQ(tt, g));
        wp[i] = v;
        wq[i] = opz_sub_q31(tt, MQ(v, g));
    }
    put(m->buf, pos, wp, m->wtmp[0]);
    put(m->buf, next, wq, m->wtmp[1]);
}

__attribute__((noinline)) static void diffuse(Mem *m, int32_t *x, int32_t pos, int32_t len, int32_t g)
{
    const int32_t *t = seg(m->buf, pos - len, BLK, m->win);
    int32_t *wp = wbeg(m->buf, pos, m->wtmp[0]);
    for (int i = 0; i < BLK; i++) {
        int32_t tt = t[i];
        int32_t v = opz_add_q31(x[i], mul_hi(tt, g));
        wp[i] = v;
        x[i] = opz_sub_q31(tt, mul_hi(v, g));
    }
    put(m->buf, pos, wp, m->wtmp[0]);
}

/* Two input diffusers in a row, in one pass (x read and written once). Both lines are longer than a block, so the
 * second one's reads don't reach into what the first writes. */
__attribute__((noinline)) static void diffuse2(Mem *m, int32_t *x, int32_t pos1, int32_t len1, int32_t g1, int32_t pos2, int32_t len2, int32_t g2)
{
    const int32_t *t1 = seg(m->buf, pos1 - len1, BLK, m->win);
    const int32_t *t2 = seg(m->buf, pos2 - len2, BLK, m->tt[0]);
    int32_t *w1 = wbeg(m->buf, pos1, m->wtmp[0]), *w2 = wbeg(m->buf, pos2, m->wtmp[1]);
    for (int i = 0; i < BLK; i++) {
        int32_t tt = t1[i];
        int32_t v = opz_add_q31(x[i], mul_hi(tt, g1));
        w1[i] = v;
        int32_t y = opz_sub_q31(tt, mul_hi(v, g1));
        tt = t2[i];
        v = opz_add_q31(y, mul_hi(tt, g2));
        w2[i] = v;
        x[i] = opz_sub_q31(tt, mul_hi(v, g2));
    }
    put(m->buf, pos1, w1, m->wtmp[0]);
    put(m->buf, pos2, w2, m->wtmp[1]);
}

static void tapsum(Mem *m, int32_t *y, int32_t w, const uint16_t *reg, const int32_t *pos)
{
    const int32_t *a0 = seg(m->buf, w + reg[0] - pos[0], BLK, m->tt[0]);
    const int32_t *a1 = seg(m->buf, w + reg[1] - pos[1], BLK, m->tt[1]);
    const int32_t *a2 = seg(m->buf, w + reg[2] - pos[2], BLK, m->tt[2]);
    const int32_t *a3 = seg(m->buf, w + reg[3] - pos[3], BLK, m->tt[3]);
    for (int i = 0; i < BLK; i++) y[i] = (a0[i] >> 3) + (a1[i] >> 3) - (a2[i] >> 3) + (a3[i] >> 3);
    a0 = seg(m->buf, w + reg[4] - pos[4], BLK, m->tt[0]);
    a1 = seg(m->buf, w + reg[5] - pos[5], BLK, m->tt[1]);
    a2 = seg(m->buf, w + reg[6] - pos[6], BLK, m->tt[2]);
    for (int i = 0; i < BLK; i++) y[i] -= (a0[i] >> 3) + (a1[i] >> 3) + (a2[i] >> 3);
}

static int32_t log2_int(int32_t x)
{
    int e = opz_norm32(x);
    int32_t f = ((x << e) - 0x40000000) >> 14;
    int32_t l = f + (opz_mul_q31(f * (65536 - f), 736801640) >> 16);
    return l + ((30 - e) << 16);
}

/* One-pole coefficient 1 - e^-x for x = 2 pi fc / fs = 2^(lx / 65536), Q15. Per block. */
static int32_t onepole15(int32_t lx)
{
    int32_t x = opz_exp2_q16(lx);
    int32_t e = opz_exp2_q16(-opz_mul_q31(x, 0x5c551d95) * 2);
    int32_t k = (65536 - e) >> 1;
    return k > 32767 ? 32767 : k;
}

static int32_t seg_gain(int32_t len_q16, int32_t lr, int32_t frac)
{
    int32_t le = lr + log2_int((len_q16 >> 16) + 1);
    if (le > (6 << 16)) le = 6 << 16;
    int32_t e = opz_mul_q31(opz_exp2_q16(le), frac << 16);
    int32_t g = opz_exp2_q16(-e) >> 1;
    return g > G_TOP ? G_TOP : g;
}

/* The shifter on half 0's first delay output t: z = t + a (shifted - t), a ramping from a0 to a1 (Q15) over the
 * block. The branch is filtered (high-pass, two low-pass poles with coefficient k) into the shifter ring; two taps
 * read it at delays that sweep across the window by `inc` (phase per sample), triangle-crossfaded. In three simple
 * passes (tap 1; tap 2 and the crossfade; the mix), which keep everything in registers. tx, tu: scratch. */
__attribute__((noinline)) static void shift_block(State *s, int32_t *sb, const int32_t *t, int32_t *z, int32_t k, uint32_t inc,
                        int32_t a0, int32_t a1, int32_t *tx, int32_t *tu)
{
    int32_t hp = s->hp, b1 = s->b1, b2 = s->b2;
    uint32_t wpos = s->sbw;
    for (int i = 0; i < BLK; i++) {                         /* the branch into the ring, a block ahead of the reads */
        int32_t x = t[i];
        hp = opz_add_q31(hp, MQ(opz_sub_q31(x, hp), K_HP));
        b1 = opz_add_q31(b1, MQ(opz_sub_q31(opz_sub_q31(x, hp), b1), k));
        b2 = opz_add_q31(b2, MQ(opz_sub_q31(b1, b2), k));
        sb[(wpos + i) & SB_MASK] = b2;
    }
    s->hp = hp; s->b1 = b1; s->b2 = b2;

    /* tap 1: its place in the window u (0 .. 65535), its delay u SW / 65536 (Q16 samples), read between samples */
    uint32_t ph = s->ph, n = wpos;
    for (int i = 0; i < BLK; i++) {
        ph -= inc;
        uint32_t u = ph >> 16;
        uint32_t pq = opz_mul_u16(u, SW);
        uint32_t j = n++ - (pq >> 16);
        int32_t c = sb[j & SB_MASK];
        tx[i] = opz_add_q31(c, MQ(opz_sub_q31(sb[(j - 1) & SB_MASK], c), (pq >> 1) & 0x7fff));
        tu[i] = (int32_t)u;
    }
    s->ph = ph;
    /* tap 2, half a window on (SW / 2 samples: the same fraction), and the triangle crossfade: tap 1's weight is 0
     * where its delay jumps, tap 2's where its does */
    n = wpos;
    for (int i = 0; i < BLK; i++) {
        uint32_t u = (uint32_t)tu[i];
        uint32_t pq = opz_mul_u16(u ^ 0x8000u, SW);
        uint32_t j = n++ - (pq >> 16);
        int32_t c = sb[j & SB_MASK];
        int32_t x2 = opz_add_q31(c, MQ(opz_sub_q31(sb[(j - 1) & SB_MASK], c), (pq >> 1) & 0x7fff));
        int32_t g = (int32_t)((u ^ (0u - (u >> 15))) & 0x7fffu);    /* u, or 65535 - u past the middle */
        tx[i] = opz_add_q31(x2, MQ(opz_sub_q31(tx[i], x2), g));
    }
    /* the mix: (1 - a) direct + a SH_GAIN shifted */
    int32_t a = a0 << 16, a_inc = ((a1 - a0) * 65536) >> 6;
    int32_t b = ((a0 * SH_GAIN) >> 15) << 16, b_inc = ((((a1 - a0) * SH_GAIN) >> 15) * 65536) >> 6;
    for (int i = 0; i < BLK; i++) {
        a += a_inc;
        b += b_inc;
        int32_t d = t[i];
        z[i] = opz_add_q31(opz_sub_q31(d, MQ(d, a >> 16)), MQ(tx[i], b >> 16));
    }
    s->sbw = (wpos + BLK) & SB_MASK;
}

/* ---- exports ---------------------------------------------------------------------------------------------- */

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
    /* The buffer fits FX2's 256 KB pool, so FX1 and FX2 are safe; the master pool (16 KB) is not: dry there */
    s->mem = track == OPZ_TRACK_MASTER ? (Mem *)0 : (Mem *)mem;    /* cleared progressively in renderFX */
    s->agc = OPZ_Q31_ONE;
    s->trim = 32767;
    s->semi = SEMI_DEFAULT;
}

/* params may be NULL (a held note re-sent with velocity 0): never read it. The newest note sets the interval. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params; (void)velocity;
    State *s = state;
    if (note < 0 || note > 127) return;                        /* 255: plug selected, no note */
    int semi = note - NOTE_ZERO;
    s->semi = (int8_t)(semi < SEMI_MIN ? SEMI_MIN : semi > SEMI_MAX ? SEMI_MAX : semi);
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                   /* the interval stays */
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;
    Mem *m = s->mem;

    int32_t pan = opz_pq31(params[OPZ_P_PAN]);
    int32_t wet = pan >= 0x40000000 ? OPZ_Q31_ONE : pan * 2;
    int32_t og = opz_mul_q31(opz_pq31(params[OPZ_P_LEVEL]), gain);
    int32_t dog = opz_mul_q31(OPZ_Q31_ONE - wet, og);
    int32_t wog = opz_mul_q31(opz_mul_q31(wet, og), WET_K);

    if (!m || s->cleared < RING + SB_N) {                       /* no buffer, or still clearing it: dry only */
        if (m) {
            volatile int32_t *zp = m->buf + s->cleared;
            int n = RING + SB_N - s->cleared < CLEAR_STEP ? RING + SB_N - s->cleared : CLEAR_STEP;
            for (int i = 0; i < n; i++) zp[i] = 0;
            s->cleared += n;
        }
        for (int j = 0; j < 2 * BLK; j++) out[j] = opz_mul_q31(in[j], dog);
        s->dog = dog;
        return;
    }

    int32_t u1 = opz_p16(params[OPZ_P_P1]), u2 = opz_p16(params[OPZ_P_P2]);
    int32_t inc[NL];

    /* P1: size 0.3 x 5^u (Q16); every length glides to its rounded target */
    int32_t size = opz_exp2_q16(-113837 + opz_mul_q31(152170, u1 << 15));
    if (size < S_MIN) size = S_MIN;
    if (size > S_MAX) size = S_MAX;
    int glide = 0;
    if (!s->started) {                                          /* the buffer's header is garbage until now */
        for (int k = 0; k < NL; k++) m->cur[k] = FIXED(k) ? k_base[k] << 16 : (k_base[k] * size + 0x8000) & ~0xffff;
        for (int k = 0; k < NT; k++) m->tap[k] = (k_tap[k] * size + 0x8000) >> 16;
        s->ts = ((m->cur[1] >> 12) * 2543) >> 12;
    }
    for (int k = 0; k < NL; k++) {
        int32_t target = FIXED(k) ? k_base[k] << 16 : (k_base[k] * size + 0x8000) & ~0xffff;
        int32_t d = target - m->cur[k], lim = k_base[k] * SLEW;
        if (d > lim) d = lim;
        if (d < -lim) d = -lim;
        inc[k] = (d + (d < 0 ? 63 : 0)) >> 6;
        if (inc[k]) glide = 1;
        else m->cur[k] = target;
    }

    int32_t pd = 132 + ((((m->cur[1] >> 16) - 1979) * 959) >> 12);    /* pre-delay 3 .. 45 ms */
    if (pd > PD_MAX) pd = PD_MAX;
    if (pd < 1) pd = 1;

    /* decay: T60 = 0.3 s x 2^(6.81 u) up to P1 240, then the decay rate falls to 0 at 255 */
    int32_t ut = u1 < 61680 ? u1 : 61680;
    int32_t lr = -793722 + 113837 - opz_mul_q31(446260, ut << 15);
    int32_t frac = u1 < 61680 ? 32767 : ((65535 - u1) * 17) >> 1;

    /* P2: shimmer mix a = 0.8 (u + u^2) / 2, none at unison; the branch low-pass 8 kHz -> 5.7 kHz, lower by the
     * interval above an octave (the taps read faster) */
    int semi = s->semi;
    int32_t q = u2 >> 1;                                        /* Q15 */
    int32_t a1 = semi ? (A_MAX * ((q + ((q * q) >> 15)) >> 1)) >> 15 : 0;
    int32_t kbr = onepole15(LX_BRANCH - (u2 >> 1) - (semi > 12 ? (semi - 12) * 5461 : 0));
    int32_t ratio = opz_exp2_q16((semi * 21845) >> 2);         /* 2^(semi / 12), Q16 */
    int32_t dr = ratio - 65536;
    uint32_t sinc = opz_mul_u32_shr16((uint32_t)(dr < 0 ? -dr : dr), SW_INC_K);
    if (dr < 0) sinc = 0u - sinc;

    /* The mix of two unrelated signals keeps (1 - a)^2 + a^2 of the power: half 0's decay gain makes it up,
     * 1 / sqrt(v) by a quadratic on [0.5, 1] (within 0.5 %), Q14, but stays below 1 */
    int32_t am = 32768 - a1;
    int32_t v = ((am * am) >> 15) + ((a1 * a1) >> 15) - 24576;  /* v - 0.75, Q15 */
    int32_t comp = 18919 - ((v * 13573) >> 15) + ((((v * v) >> 15) * 13736) >> 15);

    int32_t g0a = seg_gain(m->cur[0] + m->cur[1], lr, frac), g0b = seg_gain(m->cur[2] + m->cur[3], lr, frac);
    int32_t g1a = seg_gain(m->cur[4] + m->cur[5], lr, frac), g1b = seg_gain(m->cur[6] + m->cur[7], lr, frac);
    g0a = (g0a * comp) >> 14;
    if (g0a > G_TOP) g0a = G_TOP;
    g0a = MQ(g0a, s->trim);
    g0b = MQ(g0b, s->trim);
    g1a = MQ(g1a, s->trim);
    g1b = MQ(g1b, s->trim);

    uint32_t ph = s->lfo + LFO_INC * BLK;
    int32_t m0 = opz_mul_q31(EXC, opz_sine(ph)), m1 = opz_mul_q31(EXC, opz_sine(ph + 0x40000000u));
    s->lfo = ph;

    int32_t ig = opz_mul_q31(IN_SCALE, s->agc);

    if (!s->started) {
        s->ig = ig; s->wog = wog; s->dog = dog; s->m0 = m0; s->m1 = m1; s->a = a1;
        s->started = 1;
    }
    int32_t w = (int32_t)s->w;
    int32_t *r = m->buf;

    /* ---- input: half scale, DC blocker (~35 Hz), gain (ramped), pre-delay, band limit ----------------------- */
    {
        int32_t g = s->ig, g_inc = (ig - s->ig) >> 6;
        int32_t dxl = s->dcx[0], dxr = s->dcx[1], dyl = s->dcy[0], dyr = s->dcy[1];
        int32_t *pl = wbeg(r, w + R_PDL, m->wtmp[0]);
        for (int i = 0; i < BLK; i++) {
            g += g_inc;
            int32_t x = in[2 * i] >> 1;
            dyl = opz_add_q31(x - dxl, MQ(dyl, 32604));
            dxl = x;
            pl[i] = MQ(dyl, g >> 16);
        }
        put(r, w + R_PDL, pl, m->wtmp[0]);
        g = s->ig;
        int32_t *pr = wbeg(r, w + R_PDR, m->wtmp[0]);
        for (int i = 0; i < BLK; i++) {
            g += g_inc;
            int32_t x = in[2 * i + 1] >> 1;
            dyr = opz_add_q31(x - dxr, MQ(dyr, 32604));
            dxr = x;
            pr[i] = MQ(dyr, g >> 16);
        }
        put(r, w + R_PDR, pr, m->wtmp[0]);
        s->ig = ig;
        s->dcx[0] = dxl; s->dcx[1] = dxr; s->dcy[0] = dyl; s->dcy[1] = dyr;

        const int32_t *ql = seg(r, w + R_PDL - pd, BLK, m->win);
        int32_t b = s->bw[0];
        for (int i = 0; i < BLK; i++) {
            b = opz_add_q31(b, MQ(opz_sub_q31(ql[i], b), K_BAND));
            m->xl[i] = b;
        }
        s->bw[0] = b;
        const int32_t *qr = seg(r, w + R_PDR - pd, BLK, m->win);
        b = s->bw[1];
        for (int i = 0; i < BLK; i++) {
            b = opz_add_q31(b, MQ(opz_sub_q31(qr[i], b), K_BAND));
            m->xr[i] = b;
        }
        s->bw[1] = b;
    }

    for (int c = 0; c < 2; c++) {
        int32_t *x = c ? m->xr : m->xl;
        const int16_t *len = k_dif_len + ND * c;
        const uint16_t *reg = k_dif_reg + ND * c;
        diffuse2(m, x, w + reg[0], len[0], DIF_G1, w + reg[1], len[1], DIF_G2);
        diffuse(m, x, w + reg[2], len[2], DIF_G2);
    }

    /* ---- tank: half 0 gets the right input and half 1's output, half 1 the left input and half 0's output ---- */
    for (int h = 0; h < 2; h++) {
        const int32_t *x = h ? m->xl : m->xr;
        int32_t *u = m->u;
        int b = 4 * h, o = 4 - b;
        int32_t gfb = h ? g0b : g1b, ga = h ? g1a : g0a;

        const int32_t *t = line_out(m, w + k_region[o + 3], m->cur[o + 3], inc[o + 3], m->tb[o + 3], inc[o + 3] != 0);
        for (int i = 0; i < BLK; i++) u[i] = opz_add_q31(x[i], MQ(t[i], gfb));

        int32_t mm = h ? s->m1 : s->m0, mt = h ? m1 : m0;
        t = line_out(m, w + k_region[b], m->cur[b] + mm, inc[b] + ((mt - mm) >> 6), m->tb[b], 1);
        allpass(m, u, t, w + k_region[b], -DD1, w + k_region[b + 1]);

        t = line_out(m, w + k_region[b + 1], m->cur[b + 1], inc[b + 1], m->tb[b + 1], inc[b + 1] != 0);
        if (!h) {                                               /* half 0: the shifter, once a lap. It runs at a = 0
                                                                   too, so its ring is never stale */
            shift_block(s, r + RING, t, m->sh, kbr, sinc, s->a, a1, m->tt[0], m->tt[1]);
            t = m->sh;
        }
        int32_t l = s->lp[h];
        for (int i = 0; i < BLK; i++) {
            l = opz_add_q31(l, MQ(opz_sub_q31(t[i], l), K_DAMP));
            u[i] = MQ(l, ga);
        }
        s->lp[h] = l;

        t = line_out(m, w + k_region[b + 2], m->cur[b + 2], inc[b + 2], m->tb[b + 2], inc[b + 2] != 0);
        allpass(m, u, t, w + k_region[b + 2], DD2, w + k_region[b + 3]);
    }
    s->m0 = m0;
    s->m1 = m1;
    s->a = a1;

    /* ---- output taps, stepped and crossfaded as the size moves (see plate.c) --------------------------------- */
    int32_t sz = ((m->cur[1] >> 12) * 2543) >> 12;
    int32_t dsz = sz - s->ts;
    if (!s->xf && (opz_abs32(dsz) > (s->ts >> 4) || (!glide && dsz))) {
        for (int k = 0; k < NT; k++) {
            m->tap_old[k] = m->tap[k];
            m->tap[k] = (k_tap[k] * sz + 0x8000) >> 16;
        }
        s->ts = sz;
        s->xf = XF;
    }
    int32_t wg_inc = (wog - s->wog) >> 6, dg_inc = (dog - s->dog) >> 6;
    int32_t pk = 0;
    for (int c = 0; c < 2; c++) {
        int32_t *y = m->u;
        tapsum(m, y, w, k_tap_reg + 7 * c, m->tap + 7 * c);
        if (s->xf) {
            int32_t *yo = m->xl;
            tapsum(m, yo, w, k_tap_reg + 7 * c, m->tap_old + 7 * c);
            int j = XF - s->xf;
            int32_t gn = k_xf[j] << 16, gn_inc = ((k_xf[j + 1] - k_xf[j]) * 65536) >> 6;
            int32_t go = k_xf[XF - j] << 16, go_inc = ((k_xf[XF - j - 1] - k_xf[XF - j]) * 65536) >> 6;
            for (int i = 0; i < BLK; i++) {
                gn += gn_inc;
                go += go_inc;
                y[i] = MQ(y[i], gn >> 16) + MQ(yo[i], go >> 16);
            }
        }
        const fract32 *xi = in + c;
        fract32 *o = out + c;
        int32_t wg = s->wog, dg = s->dog;
        for (int i = 0; i < BLK; i++) {
            wg += wg_inc;
            dg += dg_inc;
            int32_t y0 = y[i];
            o[2 * i] = opz_add_q31(MQ(xi[2 * i], dg >> 16), opz_shl_q31(MQ(y0, wg >> 16), WET_SHIFT));
            y0 = opz_abs32(y0);
            pk = y0 > pk ? y0 : pk;
        }
    }
    if (s->xf) s->xf--;
    s->wog = wog;
    s->dog = dog;
    if (glide)
        for (int k = 0; k < NL; k++) m->cur[k] += inc[k] * BLK;
    s->w = (uint32_t)wrap(w + BLK);

    if (pk > AGC_KNEE) s->agc -= s->agc >> 2;
    else s->agc += (OPZ_Q31_ONE - s->agc) >> 7;
    if (pk > TRIM_KNEE) s->trim -= s->trim >> 5;
    else s->trim += (32767 - s->trim) >> 7;
}

OPZ_EXPORTS_FX();
