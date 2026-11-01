/*
 * swirl — stereo phaser / flanger that follows notes on its FX track.
 *
 *   P1        rate        LFO 0.03 .. 10 Hz (exponential)
 *   P2        character   one macro over mode, depth and feedback:
 *                           0 .. 111    6-stage phaser, depth 25 % -> 100 %, feedback 0 -> 0.7
 *                           112 .. 143  crossfade to the flanger, depth and feedback easing back down
 *                           144 .. 255  flanger, depth 25 % -> 100 %, feedback 0.1 -> 0.9
 *                         depth is the sweep width: up to +/-1.5 octaves (phaser), up to the full base delay
 *                         (flanger). Piecewise linear, so an LFO on P2 glides through the modes without steps.
 *   pan       dry / wet   TE's balance law (wet = min(2p, 1), dry = min(2(1-p), 1)), normalised by dry + wet so
 *                         the centre is the classic 50/50 phaser mix at unity gain
 *   level     output level (x gain)
 *   note on the FX track  sweep centre = the note's pitch; the flanger delay becomes one period of it (a comb
 *                         tuned to the note, with the modulation narrowed so it stays in tune); the LFO restarts.
 *                         Note off returns to the free-running centre.
 *
 * The filter and resonance knobs are the OS's own track filter (also on FX tracks), so swirl never reads them.
 *
 * Clean-room design inspired by TE's unreleased Swirl class: only its name and the fact that it follows notes are
 * known. Q31 signals, Q16 control maths, no floating point, no libc. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define STAGES          6
#define DELAY_LEN       2048                    /* samples per channel, int16 */
#define DELAY_MASK      (DELAY_LEN - 1)
#define MEM_BYTES       (DELAY_LEN * 2 * 2)     /* 8 KB: fits FX1 and FX2 */
#define CLEAR_STEP      512                     /* samples cleared per block: init runs with interrupts masked */
#define SUB             16                      /* coefficients are updated every 16 samples */
#define Q31_ONE         0x7fffffff
#define FREE_CENTRE_HZ  700
#define FREE_DELAY_Q16  (132 << 16)             /* 3 ms */
#define P2_PHASER_END   28672                   /* P2 in 0..65535: 7/16 (byte 111) */
#define P2_FLANGER      36864                   /* 9/16 (byte 144) */

typedef struct State {
    int16_t *delay;                             /* this FX track's host buffer (left half, right half) */
    uint32_t lfo_phase;                         /* Q32 turns */
    int32_t z[2][STAGES + 1];                   /* all-pass memories, Q31: stage k's last input is z[k], its last
                                                   output z[k + 1] (which is also stage k + 1's last input) */
    int32_t fb_ph[2], fb_fl[2];                 /* feedback memories, Q31 */
    int32_t centre_q16;                         /* sweep centre, Hz in Q16 */
    int32_t base_delay_q16;                     /* flanger base delay, samples in Q16 */
    uint32_t pan;                               /* the pan word wet and dry were computed for (+1: none yet) */
    int32_t wet, dry;                           /* Q31 */
    uint16_t write;
    uint16_t cleared;                           /* samples of the delay line cleared so far (both halves) */
    uint8_t note_held;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* exp2, the LFO triangle, parameter scaling and the feedback saturation are opz_dsp.h's opz_exp2_q16, opz_tri15,
 * opz_p16 and opz_soft (the same arithmetic as swirl v1/v2's own copies). */
#define exp2_q16 opz_exp2_q16
#define p16 opz_p16
#define tri15 opz_tri15
#define soft opz_soft

/* First-order all-pass coefficient a = (1 - w) / (1 + w), w = pi fc / fs (small-angle form), Q31: exactly
 * floor((1 - w) 2^31 / (1 + w)), without a division. opz_recip_q31 gives 1 / (1 + w) within a few LSB, so
 * 2 / (1 + w) - 1 is within a few of the quotient; the remainder (1 - w) 2^31 - q (1 + w) then fits 32 bits and
 * moves q to the exact value. The coefficient sits inside the feedback loops, where an LSB can grow. */
static int32_t allpass_coef(int32_t fc_q16)
{
    if (fc_q16 < (30 << 16)) fc_q16 = 30 << 16;
    if (fc_q16 > (12000 << 16)) fc_q16 = 12000 << 16;
    int32_t w = (int32_t)opz_mul_u32_shr16((uint32_t)fc_q16, 4781);   /* 4781 = pi / 44100 * 2^26, so w is Q26 */
    int32_t num = (1 << 26) - w, den = (1 << 26) + w;          /* w < 2^26 */
    fract32 h = opz_recip_q31(den << 4);                       /* 1 / (1 + w); (1 + w) / 2 in Q31 is in [0.5, 1) */
    int32_t q = opz_shl_q31(h - 0x40000000, 1);                /* 2 h - 1 */
    /* r = num 2^31 - q den, modulo 2^32 (num 2^31 is (num & 1) << 31 there) */
    int32_t r = (int32_t)(((uint32_t)num << 31) - (uint32_t)q * (uint32_t)den);
    while (r < 0) { q--; r += den; }
    while (r >= den) { q++; r -= den; }
    return q;
}

/* (a * d) >> 17 for a in [-2^17, 2^17) and d in [0, 2^16), without 64-bit maths: a = ah 2^16 + al. */
static inline int32_t mul_shr17(int32_t a, int32_t d)
{
    return ((a >> 16) * d + (int32_t)(opz_mul_u16((uint32_t)a, (uint32_t)d) >> 16)) >> 1;
}

/* (base * mod) >> 16 for base >= 0 and mod in [-2^16, 2^16]: mod = mh 2^16 + ml. */
static inline int32_t mul_shr16(int32_t base, int32_t mod)
{
    return base * (mod >> 16) + (int32_t)opz_mul_u32_shr16((uint32_t)base, (uint32_t)mod & 0xffff);
}

static inline int16_t to16(int32_t x) { return (int16_t)(x >> 16); }

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
    s->delay = (int16_t *)mem;                                 /* cleared progressively in renderFX */
    s->centre_q16 = FREE_CENTRE_HZ << 16;
    s->base_delay_q16 = FREE_DELAY_Q16;
    s->pan = 0x80000000u;                                      /* no pan word has bit 31 set */
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)velocity; (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                       /* 255: plug selected, no note */
    int32_t octaves_q16 = (note - 69) * 65536 / 12;                            /* fits 32 bits: same quotient */
    int32_t hz_q16 = (int32_t)((int64_t)440 * exp2_q16(octaves_q16));         /* 440 * 2^((n-69)/12) */
    s->centre_q16 = hz_q16;
    int64_t period = opz_udiv64_32((uint64_t)44100 << 32, (uint32_t)hz_q16);   /* samples in Q16, < 2^32 */
    if (period > (int64_t)(DELAY_LEN - 4) << 16) period = (int64_t)(DELAY_LEN - 4) << 16;
    if (period < (int64_t)4 << 16) period = (int64_t)4 << 16;
    s->base_delay_q16 = (int32_t)period;
    s->lfo_phase = 0x40000000;                                 /* triangle at its centre, rising */
    s->note_held = 1;
}

void onNoteOff(int note, void *state)
{
    (void)note;
    State *s = state;
    s->note_held = 0;
    s->centre_q16 = FREE_CENTRE_HZ << 16;
    s->base_delay_q16 = FREE_DELAY_Q16;
}

#define ALWAYS_INLINE static inline __attribute__((always_inline))

/* Everything renderFX's sample loop needs, set up once per block. */
typedef struct Block {
    int32_t fbk, morph, wet, dry, out_gain;
} Block;

/* One frame of channel c. */
ALWAYS_INLINE fract32 run_channel(State *s, const Block *b, int32_t x, int c, int32_t a, int32_t d_q16, int16_t *dl,
                          int flanger)
{
    /* phaser: STAGES first-order all-passes, y = x1 + a (y1 - x), with feedback */
    int32_t v = opz_add_q31(x >> 1, opz_mul_q31(s->fb_ph[c], b->fbk));
    int32_t *z = s->z[c], x1 = z[0];
    for (int k = 0; k < STAGES; k++) {
        int32_t y1 = z[k + 1];
        int32_t y = opz_add_q31(x1, opz_mul_q31(a, opz_sub_q31(y1, v)));
        z[k] = v;
        x1 = y1;
        v = y;
    }
    z[STAGES] = v;
    s->fb_ph[c] = soft(v);
    int32_t effect = v;

    /* flanger: fractional delay with linear interpolation, with feedback */
    if (flanger) {
        int16_t *buf = dl + c * DELAY_LEN;
        int32_t di = d_q16 >> 16, frac = (d_q16 & 0xffff) << 15;           /* frac in Q31 */
        int32_t s0 = (int32_t)buf[(s->write - di) & DELAY_MASK] << 16;
        int32_t s1 = (int32_t)buf[(s->write - di - 1) & DELAY_MASK] << 16;
        int32_t fl = opz_add_q31(s0, opz_mul_q31(frac, opz_sub_q31(s1, s0)));
        buf[s->write] = to16(opz_add_q31(x >> 1, opz_mul_q31(s->fb_fl[c], b->fbk)));
        s->fb_fl[c] = soft(fl);
        effect = opz_add_q31(effect, opz_mul_q31(b->morph, opz_sub_q31(fl, effect)));
    }

    /* dry * x + wet * 2 effect: two products rounded down on their own (within an LSB of one 64-bit sum) */
    int32_t mix = opz_add_q31(opz_mul_q31(b->dry, x), opz_mul_q31(b->wet, opz_shl_q31(effect, 1)));
    return opz_mul_q31(mix, b->out_gain);
}

/* SUB frames of both channels with fixed coefficients a[c] and delays d_q16[c]. flanger is a compile-time
 * constant in each copy (morph == 0 runs the phaser alone). */
ALWAYS_INLINE void run_sub(State *s, const Block *b, const fract32 *in, fract32 *out, int i0, const int32_t *a,
                           const int32_t *d_q16, int16_t *dl, uint32_t lfo_inc, int flanger)
{
    int32_t a0 = a[0], a1 = a[1], d0 = d_q16[0], d1 = d_q16[1];
    for (int i = i0; i < i0 + SUB; i++) {
        out[2 * i] = run_channel(s, b, in[2 * i], 0, a0, d0, dl, flanger);           /* interleaved L, R */
        out[2 * i + 1] = run_channel(s, b, in[2 * i + 1], 1, a1, d1, dl, flanger);
        if (dl) s->write = (uint16_t)((s->write + 1) & DELAY_MASK);
        s->lfo_phase += lfo_inc;
    }
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block (32-bit forms of the 64-bit products: the same values) ---------------------------------- */
    int32_t rate_q16 = exp2_q16((int32_t)opz_mul_u32_shr16(545260, (uint32_t)p16(params[OPZ_P_P1])) - (5 << 16));
    uint32_t lfo_inc = opz_mul_u32_shr16((uint32_t)rate_q16, 97391);          /* Hz Q16 -> Q32 turns/sample */
    /* P2 -> amount (0..1 within a mode) and morph (0 phaser .. 1 flanger), both 0..65536 */
    int32_t u = p16(params[OPZ_P_P2]), amount, morph16;
    if (u < P2_PHASER_END) {
        amount = (int32_t)(((uint32_t)u * 149797u) >> 16);                    /* u * 16/7; below 2^32 */
        morph16 = 0;
    } else if (u < P2_FLANGER) {
        morph16 = (u - P2_PHASER_END) * 8;
        amount = 65536 - morph16;
    } else {
        amount = (int32_t)(((uint32_t)(u - P2_FLANGER) * 149797u) >> 16);
        morph16 = 65536;
    }
    if (amount > 65535) amount = 65535;
    int32_t depth = 16384 + (int32_t)(opz_mul_u16((uint32_t)amount, 49151) >> 16);   /* 0.25..1 as 0..65535 */
    int32_t fb_ph = (int32_t)opz_mul_u32_shr16(0x59999999, (uint32_t)amount);         /* 0 .. 0.7, Q31 */
    int32_t fb_fl = 0x0ccccccc + (int32_t)opz_mul_u32_shr16(0x66666666, (uint32_t)amount);   /* 0.1 .. 0.9 */
    Block b;
    b.morph = morph16 >= 65536 ? Q31_ONE : morph16 << 15;
    b.fbk = opz_add_q31(fb_ph, opz_mul_q31(b.morph, fb_fl - fb_ph));
    uint32_t pan = params[OPZ_P_PAN] & Q31_ONE;
    if (pan != s->pan) {                                       /* two 64-bit divisions: only when pan moves */
        int32_t wet = opz_sat32((int64_t)pan * 2), dry = opz_sat32(((int64_t)Q31_ONE - (int32_t)pan) * 2);
        uint32_t sum = (uint32_t)wet + (uint32_t)dry;                          /* 1..2 in Q31 */
        s->wet = (int32_t)opz_udiv64_32((uint64_t)wet << 31, sum);             /* 2^31 at pan 255: -1, as before */
        s->dry = (int32_t)opz_udiv64_32((uint64_t)dry << 31, sum);
        s->pan = pan;
    }
    b.wet = s->wet;
    b.dry = s->dry;
    b.out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & Q31_ONE), gain);
    /* On a note the flanger delay swings +-1.5 % around one period, so the comb stays tuned to it */
    int32_t fl_depth = s->note_held ? depth >> 5 : depth;
    int32_t fl_offset = s->note_held ? 0 : 65536;

    int16_t *dl = s->delay;
    int flanger_ready = 0;
    if (dl) {
        if (s->cleared < 2 * DELAY_LEN) {
            volatile int16_t *z = dl + s->cleared;
            for (int i = 0; i < CLEAR_STEP; i++) z[i] = 0;
            s->cleared += CLEAR_STEP;
        } else {
            flanger_ready = 1;
        }
    }
    if (!flanger_ready) { b.morph = 0; b.fbk = fb_ph; }

    for (int sub = 0; sub < OPZ_BLOCK_FRAMES; sub += SUB) {
        int32_t a[2], d_q16[2];
        for (int c = 0; c < 2; c++) {
            int32_t lfo = tri15(s->lfo_phase + (c ? 0x40000000u : 0));        /* right channel 90 deg ahead */
            int32_t sweep = (lfo * depth) >> 15;                                /* -1..1 in Q16; fits 32 bits */
            a[c] = allpass_coef((int32_t)opz_mul_u32_shr16((uint32_t)s->centre_q16,
                                                           (uint32_t)exp2_q16((sweep * 3) >> 1)));
            int32_t mod = mul_shr17(sweep + fl_offset, fl_depth);              /* 0..1 (note: +-) in Q16 */
            int32_t d = s->base_delay_q16 + mul_shr16(s->base_delay_q16, mod);
            if (d > (DELAY_LEN - 4) << 16) d = (DELAY_LEN - 4) << 16;
            d_q16[c] = d;
        }
        if (b.morph) run_sub(s, &b, in, out, sub, a, d_q16, dl, lfo_inc, 1);
        else run_sub(s, &b, in, out, sub, a, d_q16, dl, lfo_inc, 0);
    }
}

OPZ_EXPORTS_FX();
