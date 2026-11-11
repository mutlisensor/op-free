/*
 * plate — lush stereo plate reverb for FX1/FX2: a Dattorro figure-8 tank with modulated all-passes; notes freeze it.
 *
 *   P1        size        one macro over size, decay and pre-delay: 0 = a small, short room (tank at 0.3x, T60 0.3 s,
 *                         pre-delay 3 ms) -> 240 = a huge, long plate (1.36x, T60 ~20 s, 40 ms) -> 255 (1.5x, 45 ms).
 *                         Size is exponential (x5 over the range), decay exponential (by design 0.3 s x 2^(6.8 P1/255)
 *                         up to 240). Above 240 the decay runs on towards endless at 255; a limiter keeps the build-up
 *                         from clipping. Size
 *                         changes glide (all delays together, at most 1/8 sample per sample, ~1.5 s from end to end),
 *                         so turning P1 bends the tail's pitch like tape instead of clicking
 *   P2        tone        dark and warm -> bright and shimmering: the input band limit opens from 2 to 18 kHz, the
 *                         damping in the tank from 1.4 to 14 kHz, and the chorus in the tail deepens (all-pass
 *                         excursion 4 -> 32 samples, LFO 0.5 -> 1 Hz)
 *   pan       dry / wet   the SDK's FX law: 128 .. 255 the reverb only (the OS's dry level keeps the source);
 *                         below 128 the dry blends in at equal power, 0 = dry only. Output through opz_limit_half
 *   level     output level (x gain)
 *   notes     held on the FX track: freeze. While any note is held the input is shut out of the tank (~12 ms fade)
 *             and the loop's decay and damping open up, so what was ringing hangs as a pad (it fades only 0.2-0.5 dB
 *             a second, the top end first: the interpolation in the modulated all-passes); the dry signal still
 *             passes. Releasing the last note lets the tail decay at P1 again. Note number and velocity don't
 *             matter. With no notes it is a plain reverb.
 *
 * Signal path: input -> DC blocker -> pre-delay -> band limit (one-pole) -> 4 input diffusers per channel (each
 * channel its own lengths: true stereo) -> tank. The tank is Dattorro's: two halves, each a modulated all-pass, a
 * delay, a damping low-pass, a decay gain, an all-pass, a delay and a decay gain, each half feeding the other (the
 * figure 8). The left input enters the half whose taps make up most of the left output, so the image holds. Output: 7
 * taps per channel from both halves, as in the paper. The lines are 32-bit, in one 220 KB ring where every line has
 * its own region (one write position for all of them). 16-bit lines would halve that, but
 * rounding to 16 bits in the loops leaves limit cycles (faint tones that never decay) and truncating instead cuts
 * the last 20 dB of every tail.
 *
 * Every line is at least one block long (the shortest, a modulated all-pass at size 0.3, is ~265 samples), so the
 * whole network runs a block at a time, one line after the other, on contiguous stretches of the ring: no index
 * wrapping per sample. A line's reads interpolate only while it moves (the all-pass modulation, a size glide).
 *
 * Limiter: the wet signal at full level is watched per block; above -6 dBFS the input to the tank ducks, above -3 dBFS
 * the loop gain trims too, so the endless top of P1 (or a loud sustained input at a long setting) settles below
 * 0 dBFS instead of building up.
 *
 * The filter and resonance knobs are the OS's own track filter; plate never reads them. Pitch bend is not used.
 *
 * J. Dattorro, "Effect Design, Part 1: Reverberator and Other Filters", JAES 45(9), 1997 (the topology, delay lengths
 * and output taps, scaled to 44.1 kHz). Q31 signals, Q15 coefficients; no floating point, no libc, no int64 or
 * division per sample. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_engine.h"
#include "opz_dsp.h"

#define BLK             OPZ_BLOCK_FRAMES

/* ---- the ring ------------------------------------------------------------------------------------------- */

#define S_MIN           19661                   /* size 0.3, Q16 */
#define S_MAX           98304                   /* size 1.5, Q16 */
#define EXC_MAX         34                      /* largest all-pass excursion (samples), with room */
#define PD_MAX          2000                    /* pre-delay (samples) */
#define SPAN(n)         ((n) + BLK + 8)         /* a line of up to n samples: its history and one block of writes */
#define SPAN_S(n)       SPAN((n) * 3 / 2 + 2)   /* a line that scales with the size, up to S_MAX */

/* Dattorro's lengths x 44100 / 29761. Half 0 is his left half (672, 4453, 1800, 3720), half 1 his right. */
#define L0_AP1          996
#define L0_D1           6598
#define L0_AP2          2667
#define L0_D2           5512
#define L1_AP1          1346
#define L1_D1           6249
#define L1_AP2          3936
#define L1_D2           4687

/* Region of each line: the ring position (relative to the block's write position) of its newest sample. Its
 * history lies below, its writes for the block above. */
enum {
    R0_AP1 = SPAN_S(L0_AP1) + EXC_MAX,
    R0_D1 = R0_AP1 + SPAN_S(L0_D1),
    R0_AP2 = R0_D1 + SPAN_S(L0_AP2),
    R0_D2 = R0_AP2 + SPAN_S(L0_D2),
    R1_AP1 = R0_D2 + SPAN_S(L1_AP1) + EXC_MAX,
    R1_D1 = R1_AP1 + SPAN_S(L1_D1),
    R1_AP2 = R1_D1 + SPAN_S(L1_AP2),
    R1_D2 = R1_AP2 + SPAN_S(L1_D2),
    R_DL0 = R1_D2 + SPAN(210), R_DL1 = R_DL0 + SPAN(159), R_DL2 = R_DL1 + SPAN(562), R_DL3 = R_DL2 + SPAN(410),
    R_DR0 = R_DL3 + SPAN(223), R_DR1 = R_DR0 + SPAN(167), R_DR2 = R_DR1 + SPAN(593), R_DR3 = R_DR2 + SPAN(433),
    R_PDL = R_DR3 + SPAN(PD_MAX),
    R_PDR = R_PDL + SPAN(PD_MAX),
    RING = R_PDR + BLK                          /* 56,186 samples */
};

/* The 8 tank lines, whose lengths follow the size */
#define NL              8
static const int16_t k_base[NL] = { L0_AP1, L0_D1, L0_AP2, L0_D2, L1_AP1, L1_D1, L1_AP2, L1_D2 };
static const uint16_t k_region[NL] = { R0_AP1, R0_D1, R0_AP2, R0_D2, R1_AP1, R1_D1, R1_AP2, R1_D2 };

/* Output: 7 taps per channel (Dattorro's, x 1.4818), summed + + - + - - -, at positions that follow the size in
 * steps (see renderFX) */
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
/* crossfade gain of the incoming taps, (x + sin(pi x / 2)) / 2 at x = j / 8, Q15: between linear (right for
 * correlated sets) and equal power (right for uncorrelated ones), within 1.6 dB of both */
static const int16_t k_xf[XF + 1] = { 0, 5246, 10365, 15247, 19778, 23862, 27424, 30406, 32767 };

/* Input diffusers: Dattorro's 142, 107, 379, 277 (x 1.4818) on the left, slightly longer on the right */
static const int16_t k_dif_len[8] = { 210, 159, 562, 410, 223, 167, 593, 433 };
static const uint16_t k_dif_reg[8] = { R_DL0, R_DL1, R_DL2, R_DL3, R_DR0, R_DR1, R_DR2, R_DR3 };
#define DIF_G1          24576                   /* 0.75, Q15 (diffusers 1, 2) */
#define DIF_G2          20480                   /* 0.625 (diffusers 3, 4) */
#define DD1             22938                   /* 0.70: decay diffusion 1, the modulated all-passes (negative) */
#define DD2             16384                   /* 0.50: decay diffusion 2 */

#define SLEW            79                      /* glide: Q16 samples per block per sample of base length, so the
                                                   longest line moves 8 samples per 64 (1/8 sample per sample) */
#define IN_SCALE        0x40000000              /* the tank runs at -12 dB: input x 0.5, then x 0.5 here */
#define AGC_KNEE        0x0859999a              /* tap sum (Q31 / 8) at which the wet output at full level would
                                                   reach -6 dBFS (x 7.68): above it the input ducks */
#define TRIM_KNEE       0x0bcccccd              /* at -3 dBFS: the loop gain trims too */
#define G_TOP           32767                   /* loop gain cap, Q15 (1 - 2^-15) */
#define WET_SHIFT       5
#define WET_K           0x1eb851ec              /* wet = taps x 0.6 x 1.6 = 0.96 x 8 / 32: a sustained sound's tail
                                                   sits a few dB under it */
#define CLEAR_STEP      4096                    /* samples per block while the ring is cleared (init masks IRQs) */
#define WIN             (BLK + 24)              /* an interpolated read's window: a block, the glide, the wobble */

typedef struct Mem {
    int32_t cur[NL];                            /* tank lengths, Q16 samples (integers once a glide has ended) */
    int32_t tap[NT], tap_old[NT];               /* tap positions, samples: now, and before a crossfade */
    int32_t xl[BLK], xr[BLK];                   /* input, then diffused input, Q31; then old taps */
    int32_t u[BLK];                             /* a tank half, Q31; then taps */
    int32_t tb[NL][BLK];                        /* line outputs: interpolated, or copied where the ring wraps */
    int32_t tt[4][BLK];                         /* tap stretches that wrap */
    int32_t win[WIN], wtmp[2][BLK];             /* a stretch that wraps; writes that wrap */
    int32_t ring[RING];
} Mem;

#define MEM_BYTES       ((uint32_t)sizeof(Mem)) /* 229,592 B on the device: FX1 and FX2 (256 KB), not master */

typedef struct State {
    Mem *mem;
    uint32_t w;                                 /* ring position of this block's first write */
    uint32_t cleared;                           /* ring samples cleared so far */
    uint32_t lfo;                               /* LFO phase */
    int32_t lp[2], bw[2];                       /* damping (tank halves), band limit (input L, R) */
    int32_t dcx[2], dcy[2];                     /* DC blockers */
    int32_t ig;                                 /* input gain reached, Q31 */
    int32_t agc;                                /* input ducking, Q31 */
    int32_t fz;                                 /* freeze 0..1, Q31 */
    int32_t trim;                               /* loop gain trim, Q15 */
    int32_t wog, dog;                           /* wet and dry output gains reached, Q31 */
    int32_t m0, m1;                             /* all-pass modulation reached, Q16 samples */
    int32_t ts;                                 /* size the taps are at, Q16 */
    uint32_t held[4];                           /* notes held (bit per MIDI note) */
    uint8_t xf;                                 /* tap crossfade: blocks to go */
    uint8_t started;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* ---- helpers --------------------------------------------------------------------------------------------- */

#define MQ(x, c)        opz_mul_q31_q15((x), (c))

/* A ring position into 0 .. RING - 1 (from -RING .. 2 RING) */
static inline int32_t wrap(int32_t p)
{
    if (p >= RING) p -= RING;
    if (p < 0) p += RING;
    return p;
}

/* n ring samples from pos: a pointer into the ring, or a copy in tmp where the stretch wraps. */
static const int32_t *seg(const int32_t *r, int32_t pos, int n, int32_t *tmp)
{
    pos = wrap(pos);
    if (pos + n <= RING) return r + pos;
    for (int i = 0; i < n; i++) tmp[i] = r[pos + i < RING ? pos + i : pos + i - RING];
    return tmp;
}

/* Where a block of writes at pos goes: the ring, or tmp (then put() copies it round the end). */
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

/* A line's output for the block: the sample written d before each one, d = d0 + (i + 1) dinc (Q16) for sample i.
 * interp = 0: d is a whole number of samples and stays put, so this is a pointer into the ring (or a copy in buf
 * where the stretch wraps). interp = 1: linear interpolation into buf. The window is a block plus how far d moves
 * in it (at most 9 samples: a glide and the all-pass wobble). */
static const int32_t *line_out(Mem *m, int32_t pos, int32_t d0, int32_t dinc, int32_t *buf, int interp)
{
    if (!interp) return seg(m->ring, pos - (d0 >> 16), BLK, buf);
    int32_t d1 = d0 + BLK * dinc;
    int32_t hi = (d0 > d1 ? d0 : d1) >> 16, lo = (d0 < d1 ? d0 : d1) >> 16;
    int off = hi + 1;
    const int32_t *p = seg(m->ring, pos - off, BLK + off - lo + 1, m->win);
    /* q = (i + off) - d in Q16 for sample i: the window index of the newer neighbour a minus the fraction of d, so
     * its whole part indexes the older neighbour c (or a itself when d is whole; then the weight of the next one is
     * 0) and its fraction g weighs a: c + (a - c) g. It moves by 1 - dinc per sample */
    int32_t q = ((off - 1) << 16) - d0, step = 65536 - dinc;
    for (int i = 0; i < BLK; i++) {
        q += step;
        const int32_t *r = p + (q >> 16);
        int32_t c = r[0];                                       /* c + (a - c) g (a - c saturates only for a full-
                                                                   scale step, which the tank never holds) */
        buf[i] = opz_add_q31(c, MQ(opz_sub_q31(r[1], c), (q & 0xffff) >> 1));
    }
    return buf;
}

/* All-pass on x with the line at pos, whose output is t: v = x + g t goes into the line, t - g v into the line at
 * next (the delay that follows it). */
static void allpass(Mem *m, const int32_t *x, const int32_t *t, int32_t pos, int32_t g, int32_t next)
{
    int32_t *wp = wbeg(m->ring, pos, m->wtmp[0]), *wq = wbeg(m->ring, next, m->wtmp[1]);
    for (int i = 0; i < BLK; i++) {
        int32_t tt = t[i];
        int32_t v = opz_add_q31(x[i], MQ(tt, g));
        wp[i] = v;
        wq[i] = opz_sub_q31(tt, MQ(v, g));
    }
    put(m->ring, pos, wp, m->wtmp[0]);
    put(m->ring, next, wq, m->wtmp[1]);
}

/* An input diffuser: a fixed-length all-pass, in place on x. */
static void diffuse(Mem *m, int32_t *x, int32_t pos, int32_t len, int32_t g)
{
    const int32_t *t = seg(m->ring, pos - len, BLK, m->win);
    int32_t *wp = wbeg(m->ring, pos, m->wtmp[0]);
    for (int i = 0; i < BLK; i++) {
        int32_t tt = t[i];
        int32_t v = opz_add_q31(x[i], MQ(tt, g));
        wp[i] = v;
        x[i] = opz_sub_q31(tt, MQ(v, g));
    }
    put(m->ring, pos, wp, m->wtmp[0]);
}

/* y = the 7 taps of one side at 1/8 each (7 of them fit), + + - + - - -, read straight from the ring. Two passes
 * of up to four streams, so the pointers stay in registers. */
static void tapsum(Mem *m, int32_t *y, int32_t w, const uint16_t *reg, const int32_t *pos)
{
    const int32_t *a0 = seg(m->ring, w + reg[0] - pos[0], BLK, m->tt[0]);
    const int32_t *a1 = seg(m->ring, w + reg[1] - pos[1], BLK, m->tt[1]);
    const int32_t *a2 = seg(m->ring, w + reg[2] - pos[2], BLK, m->tt[2]);
    const int32_t *a3 = seg(m->ring, w + reg[3] - pos[3], BLK, m->tt[3]);
    for (int i = 0; i < BLK; i++) y[i] = (a0[i] >> 3) + (a1[i] >> 3) - (a2[i] >> 3) + (a3[i] >> 3);
    a0 = seg(m->ring, w + reg[4] - pos[4], BLK, m->tt[0]);
    a1 = seg(m->ring, w + reg[5] - pos[5], BLK, m->tt[1]);
    a2 = seg(m->ring, w + reg[6] - pos[6], BLK, m->tt[2]);
    for (int i = 0; i < BLK; i++) y[i] -= (a0[i] >> 3) + (a1[i] >> 3) + (a2[i] >> 3);
}

/* log2(x) in Q16 for an integer x > 0 (glue's quadratic mantissa fit, error < 0.01 octave). Per block. */
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
    int32_t x = opz_exp2_q16(lx);                                   /* Q16 */
    int32_t e = opz_exp2_q16(-opz_mul_q31(x, 0x5c551d95) * 2);      /* e^-x = 2^(-1.4427 x) */
    int32_t k = (65536 - e) >> 1;
    return k > 32767 ? 32767 : k;
}

/* Decay gain for a segment of len samples (Q16) at log2(octaves per sample) lr (Q16), times frac (Q15; 1 below
 * P1 240, then down to 0 for an endless tail), Q15 and capped below 1. Per block. */
static int32_t seg_gain(int32_t len_q16, int32_t lr, int32_t frac)
{
    int32_t le = lr + log2_int((len_q16 >> 16) + 1);                /* log2(octaves per pass) */
    if (le > (6 << 16)) le = 6 << 16;
    int32_t e = opz_mul_q31(opz_exp2_q16(le), frac << 16);          /* octaves per pass, Q16 */
    int32_t g = opz_exp2_q16(-e) >> 1;                              /* Q15 */
    return g > G_TOP ? G_TOP : g;
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
    (void)track; (void)arg2; (void)scratch;
    State *s = state;
    volatile uint8_t *p = (volatile uint8_t *)s;               /* volatile: keep the compiler from calling memset */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    s->mem = (Mem *)mem;                                        /* the ring is cleared progressively in renderFX */
    s->agc = OPZ_Q31_ONE;
    s->trim = 32767;
}

/* params may be NULL (a held note re-sent with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                        /* 255: plug selected, no note */
    if (velocity == 0)                                         /* the OS hands back the one note still held */
        s->held[0] = s->held[1] = s->held[2] = s->held[3] = 0;
    s->held[note >> 5] |= 1u << (note & 31);
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
    Mem *m = s->mem;

    /* dry / wet and level */
    int32_t dry, wet;                                           /* the SDK's FX law; dry at half scale */
    opz_fx_drywet(params[OPZ_P_PAN], &dry, &wet);
    int32_t og = opz_mul_q31(opz_pq31(params[OPZ_P_LEVEL]), gain);
    int32_t dog = opz_mul_q31(dry, og) >> 1;
    int32_t wog = opz_mul_q31(opz_mul_q31(wet, og), WET_K);

    if (!m || s->cleared < RING) {                              /* no buffer, or still clearing it: dry only */
        if (m) {
            volatile int32_t *z = m->ring + s->cleared;
            int n = RING - s->cleared < CLEAR_STEP ? RING - s->cleared : CLEAR_STEP;
            for (int i = 0; i < n; i++) z[i] = 0;
            s->cleared += n;
        }
        for (int j = 0; j < 2 * BLK; j++) out[j] = opz_limit_half(opz_mul_q31(in[j], dog));
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
        for (int k = 0; k < NL; k++) m->cur[k] = (k_base[k] * size + 0x8000) & ~0xffff;
        for (int k = 0; k < NT; k++) m->tap[k] = (k_tap[k] * size + 0x8000) >> 16;
        s->ts = ((m->cur[1] >> 12) * 2543) >> 12;              /* as measured below: no crossfade to start with */
    }
    for (int k = 0; k < NL; k++) {
        int32_t target = (k_base[k] * size + 0x8000) & ~0xffff;
        int32_t d = target - m->cur[k], lim = k_base[k] * SLEW;
        if (d > lim) d = lim;
        if (d < -lim) d = -lim;
        inc[k] = (d + (d < 0 ? 63 : 0)) >> 6;                   /* towards zero */
        if (inc[k]) glide = 1;
        else m->cur[k] = target;                                /* within 64/65536 of it: snap (integer again) */
    }

    /* pre-delay follows the size: 3 ms at 0.3 .. 45 ms at 1.5 */
    int32_t pd = 132 + ((((m->cur[1] >> 16) - 1979) * 959) >> 12);    /* 1979 = 0.3 L0_D1; 959 / 4096 = 1544 / L0_D1 */
    if (pd > PD_MAX) pd = PD_MAX;
    if (pd < 1) pd = 1;

    /* decay: T60 = 0.3 s x 2^(6.81 u) up to P1 240 (~25 s), then the decay rate falls to 0 at 255.
     * log2(octaves per sample) = log2(9.966 / 44100) - log2 T60 */
    int32_t ut = u1 < 61680 ? u1 : 61680;
    int32_t lr = -793722 + 113837 - opz_mul_q31(446260, ut << 15);
    int32_t frac = u1 < 61680 ? 32767 : ((65535 - u1) * 17) >> 1;       /* 3855 x 8.5 = 32767 */

    /* freeze: 0 -> 1 while a note is held (~12 ms) */
    int held = (s->held[0] | s->held[1] | s->held[2] | s->held[3]) != 0;
    s->fz += ((held ? OPZ_Q31_ONE : 0) - s->fz) >> 3;
    int32_t fz15 = s->fz >> 16;

    int32_t g0a = seg_gain(m->cur[0] + m->cur[1], lr, frac), g0b = seg_gain(m->cur[2] + m->cur[3], lr, frac);
    int32_t g1a = seg_gain(m->cur[4] + m->cur[5], lr, frac), g1b = seg_gain(m->cur[6] + m->cur[7], lr, frac);
    g0a = MQ(g0a + MQ(G_TOP - g0a, fz15), s->trim);
    g0b = MQ(g0b + MQ(G_TOP - g0b, fz15), s->trim);
    g1a = MQ(g1a + MQ(G_TOP - g1a, fz15), s->trim);
    g1b = MQ(g1b + MQ(G_TOP - g1b, fz15), s->trim);

    /* P2: band limit 2 .. 18 kHz, damping 1.4 .. 14 kHz (open while frozen), chorus depth and rate */
    int32_t t31 = u2 << 15;
    int32_t kb = onepole15(-118700 + opz_mul_q31(209715, t31));     /* log2(2 pi 2000 / fs) + 3.2 t */
    int32_t kd = onepole15(-152424 + opz_mul_q31(216269, t31));     /* log2(2 pi 1400 / fs) + 3.3 t */
    kd += MQ(32767 - kd, fz15);
    int32_t exc = (4 << 16) + opz_mul_q31(28 << 16, t31);           /* excursion, Q16 samples */
    uint32_t lfo_inc = 48694u + (uint32_t)opz_mul_q31(48694, t31);  /* 0.5 .. 1 Hz */
    uint32_t ph = s->lfo + lfo_inc * BLK;
    int32_t m0 = opz_mul_q31(exc, opz_sine(ph)), m1 = opz_mul_q31(exc, opz_sine(ph + 0x40000000u));
    s->lfo = ph;

    /* input gain: x 0.5, ducked by the limiter, shut while frozen */
    int32_t ig = opz_mul_q31(opz_mul_q31(IN_SCALE, s->agc), OPZ_Q31_ONE - s->fz);

    if (!s->started) {
        s->ig = ig; s->wog = wog; s->dog = dog; s->m0 = m0; s->m1 = m1;
        s->started = 1;
    }
    int32_t w = (int32_t)s->w;
    int32_t *r = m->ring;

    /* ---- input: half scale, DC blocker (~35 Hz), gain (ramped), pre-delay, band limit ----------------------- */
    {
        int32_t g = s->ig, g_inc = (ig - s->ig) >> 6;
        int32_t dxl = s->dcx[0], dxr = s->dcx[1], dyl = s->dcy[0], dyr = s->dcy[1];
        int32_t *pl = wbeg(r, w + R_PDL, m->wtmp[0]);
        for (int i = 0; i < BLK; i++) {                         /* left */
            g += g_inc;
            int32_t x = in[2 * i] >> 1;
            dyl = opz_add_q31(x - dxl, MQ(dyl, 32604));          /* |x| <= 1/2: the difference fits */
            dxl = x;
            pl[i] = MQ(dyl, g >> 16);
        }
        put(r, w + R_PDL, pl, m->wtmp[0]);
        g = s->ig;
        int32_t *pr = wbeg(r, w + R_PDR, m->wtmp[0]);
        for (int i = 0; i < BLK; i++) {                         /* right */
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
            b = opz_add_q31(b, MQ(opz_sub_q31(ql[i], b), kb));
            m->xl[i] = b;
        }
        s->bw[0] = b;
        const int32_t *qr = seg(r, w + R_PDR - pd, BLK, m->win);
        b = s->bw[1];
        for (int i = 0; i < BLK; i++) {
            b = opz_add_q31(b, MQ(opz_sub_q31(qr[i], b), kb));
            m->xr[i] = b;
        }
        s->bw[1] = b;
    }

    /* ---- input diffusers ----------------------------------------------------------------------------------- */
    for (int k = 0; k < 4; k++) {
        int32_t g = k < 2 ? DIF_G1 : DIF_G2;
        diffuse(m, m->xl, w + k_dif_reg[k], k_dif_len[k], g);
        diffuse(m, m->xr, w + k_dif_reg[k + 4], k_dif_len[k + 4], g);
    }

    /* ---- tank: half 0 gets the right input and half 1's output, half 1 the left input and half 0's output ---- */
    for (int h = 0; h < 2; h++) {
        const int32_t *x = h ? m->xl : m->xr;
        int32_t *u = m->u;
        int b = 4 * h, o = 4 - b;                               /* this half's lines, the other half's */
        int32_t gfb = h ? g0b : g1b, ga = h ? g1a : g0a;

        const int32_t *t = line_out(m, w + k_region[o + 3], m->cur[o + 3], inc[o + 3], m->tb[o + 3], glide);
        for (int i = 0; i < BLK; i++) u[i] = opz_add_q31(x[i], MQ(t[i], gfb));    /* + the other's delay 2 */

        int32_t mm = h ? s->m1 : s->m0, mt = h ? m1 : m0;       /* modulated all-pass (-0.7) into delay 1 */
        t = line_out(m, w + k_region[b], m->cur[b] + mm, inc[b] + ((mt - mm) >> 6), m->tb[b], 1);
        allpass(m, u, t, w + k_region[b], -DD1, w + k_region[b + 1]);

        t = line_out(m, w + k_region[b + 1], m->cur[b + 1], inc[b + 1], m->tb[b + 1], glide);
        int32_t l = s->lp[h];                                   /* delay 1, damping, decay */
        for (int i = 0; i < BLK; i++) {
            l = opz_add_q31(l, MQ(opz_sub_q31(t[i], l), kd));
            u[i] = MQ(l, ga);
        }
        s->lp[h] = l;

        t = line_out(m, w + k_region[b + 2], m->cur[b + 2], inc[b + 2], m->tb[b + 2], glide);
        allpass(m, u, t, w + k_region[b + 2], DD2, w + k_region[b + 3]);           /* all-pass (+0.5) into delay 2 */
    }
    s->m0 = m0;
    s->m1 = m1;

    /* ---- output taps. They read the tank after every write (the shortest tap at size 0.3 reaches into this
     * block). Their positions follow the size the tank has reached, in steps: when it has moved 3 % (or stopped
     * somewhere new), the taps jump to the new positions and the output crossfades from the old set to the new one
     * over 8 blocks. So the taps never interpolate; a tap left behind a shrinking line still reads that line's own
     * past samples. -------------------------------------------------------------------------------------------- */
    int32_t sz = ((m->cur[1] >> 12) * 2543) >> 12;              /* size reached: delay 1 / L0_D1, Q16 */
    int32_t dsz = sz - s->ts;
    if (!s->xf && (opz_abs32(dsz) > (s->ts >> 5) || (!glide && dsz))) {
        for (int k = 0; k < NT; k++) {
            m->tap_old[k] = m->tap[k];
            m->tap[k] = (k_tap[k] * sz + 0x8000) >> 16;
        }
        s->ts = sz;
        s->xf = XF;
    }
    int32_t wg_inc = (wog - s->wog) >> 6;
    int32_t pk = 0;                                             /* peak of the tap sums, for the limiter */
    for (int c = 0; c < 2; c++) {
        int32_t *y = m->u;
        tapsum(m, y, w, k_tap_reg + 7 * c, m->tap + 7 * c);
        if (s->xf) {                                            /* crossfade from the old taps */
            int32_t *yo = m->xl;
            tapsum(m, yo, w, k_tap_reg + 7 * c, m->tap_old + 7 * c);
            int j = XF - s->xf;
            int32_t gn = k_xf[j] << 16, gn_inc = ((k_xf[j + 1] - k_xf[j]) << 16) >> 6;
            int32_t go = k_xf[XF - j] << 16, go_inc = ((k_xf[XF - j - 1] - k_xf[XF - j]) << 16) >> 6;
            for (int i = 0; i < BLK; i++) {
                gn += gn_inc;
                go += go_inc;
                y[i] = MQ(y[i], gn >> 16) + MQ(yo[i], go >> 16);
            }
        }
        fract32 *o = out + c;
        int32_t wg = s->wog;
        for (int i = 0; i < BLK; i++) {
            wg += wg_inc;
            int32_t v = y[i];
            o[2 * i] = opz_shl_q31(MQ(v, wg >> 16), WET_SHIFT - 1);   /* the wet at half scale */
            v = opz_abs32(v);
            pk = v > pk ? v : pk;
        }
    }
    opz_fx_finish(out, in, s->dog, dog);                        /* + the dry, through the SDK's limiter */
    if (s->xf) s->xf--;
    s->wog = wog;
    s->dog = dog;
    if (glide)
        for (int k = 0; k < NL; k++) m->cur[k] += inc[k] * BLK;
    s->w = (uint32_t)wrap(w + BLK);

    /* limiter on the tank, from the wet signal it gives at full level (whatever level and pan do after it): above
     * -6 dBFS the input ducks (-2.5 dB a block), above -3 dBFS the loop gain trims too (-0.3 dB a block); both
     * recover in ~0.2 s. A sustained tone into the endless tail beats, so its peaks swing well above its average */
    if (pk > AGC_KNEE) s->agc -= s->agc >> 2;
    else s->agc += (OPZ_Q31_ONE - s->agc) >> 7;
    if (pk > TRIM_KNEE) s->trim -= s->trim >> 5;
    else s->trim += (32767 - s->trim) >> 7;
}

OPZ_EXPORTS_FX();
