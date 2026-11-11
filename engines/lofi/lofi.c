/*
 * lofi — worn tape and cheap converters for FX1 and FX2: wow and flutter, a narrowing band, saturation, hiss and
 * crackle, then bit and sample-rate reduction; held keys on the FX track stop the tape.
 *
 *   P1     age        one macro, 0 = clean: wow (0.6 Hz, drifting, up to +-3 ms: ~20 cents) and flutter (6.5 Hz,
 *                     up to +-0.2 ms) grow with age (wow as age^2), the band narrows (low-pass 16 -> 2.8 kHz,
 *                     12 dB/oct; high-pass 20 -> 250 Hz) and a soft saturator (x - k x^3 / 4) bends in, its
 *                     amount k 0 -> 1 and drive 1x -> 2x (age^2)
 *   P2     noise      0..127: tape hiss (-66 -> -30 dBFS) and vinyl crackle (0 -> 40 pops/s) fade in;
 *                     128..255: they stay, and the signal is reduced to fewer bits (16 -> 4) at a lower sample
 *                     rate (44.1 -> 3 kHz, no anti-alias filter: the grit is the point)
 *   pan    dry / wet  the SDK's FX law: 128 .. 255 the worn tape only (the OS's dry level keeps the source),
 *                     below 128 the dry blends in at equal power, 0 = dry only; output through the SDK's limiter
 *   level  output level (x gain)
 *   notes  on the FX track: **tape stop**. A held key slows the tape to a halt (pitch and level fall together);
 *          the key sets how fast: F3 (left) 1.2 s .. E5 (right) 0.16 s. Releasing it returns to the live sound
 *          with a 12 ms crossfade. Every new note restarts the stop from the current speed; the OS's velocity-0
 *          re-send of a held note keeps it going
 *
 * Signal path: input -> the tape: a 32,768-frame ring (16-bit stereo) read at a moving delay (12 ms + wow +
 * flutter, linear interpolation; while stopping the delay also grows by 1 - speed per sample) -> drive and
 * x - k x^3/4 saturation, scaled back by 1 / drive -> high-pass, two-pole low-pass -> bits and rate -> + hiss and
 * crackle. Wow and flutter are computed once per block and ramped across it.
 *
 * The filter and resonance knobs are the OS's own track filter; pitch bend is not used. Q31 signals, Q15
 * coefficients, no floating point, no libc, no 64-bit maths or division per sample (one 32-bit division per note).
 * Clean-room design; MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define RING            32768u                  /* stereo int16 frames, packed L | R << 16 */
#define RMASK           (RING - 1)
#define MEM_BYTES       (RING * 4u)             /* 128 KB: fits FX2's 256 KB pool */
#define CLEAR_STEP      4096                    /* words cleared per block (init runs with interrupts masked) */
#define BASE_D          (530 << 16)             /* tape delay at rest, Q16 samples (12 ms) */
#define WOW_MAX         132                     /* wow depth at age 255, samples (3 ms) */
#define FLUTTER_MAX     9                       /* flutter depth, samples (0.2 ms) */
#define WOW_INC         3739835u                /* 0.6 Hz per block, Q32 */
#define FLUTTER_INC     40514884u               /* 6.5 Hz per block */
#define DSTOP_MAX       (26000 << 16)           /* tape-stop delay cap (inside the ring with the wow) */
#define V_ONE           (1 << 24)               /* tape speed 1.0, Q24 */
#define STOP_SLOWEST    52920                   /* frames to stop on the leftmost key (1.2 s) */
#define XF_STEP         64                      /* release crossfade: 32768 / 512 frames */
#define K_HISS          20681                   /* hiss low-pass 7 kHz, Q15 */
#define POP_DECAY       30962                   /* crackle pop decay (0.4 ms), Q15 */
#define POP_RATE_K      3895640u                /* 40 pops/s: 40 x 2^32 / 44100 / 2^16 */
#define HP_MIN          93                      /* high-pass 20 Hz, Q15 (small-angle one-pole) */
#define HP_MAX          1167                    /* 250 Hz */

/* low-pass one-pole coefficients, Q15, for 16 kHz x (2.8 / 16)^(i / 8): the band at age i / 8 */
static const int16_t k_lp[9] = { 29415, 27529, 25267, 22757, 20141, 17549, 15084, 12814, 10779 };

typedef struct State {
    uint32_t *buf;                              /* this FX track's host buffer */
    uint32_t cleared;                           /* words cleared so far */
    uint32_t w;                                 /* ring write position */
    int32_t dl;                                 /* live tape delay at the start of the block, Q16 samples */
    int32_t dstop;                              /* extra delay of the stopping tape, Q16 samples */
    int32_t v;                                  /* tape speed while stopping, Q24 */
    int32_t xf;                                 /* release crossfade: 0 stopped head .. 32768 live, Q15 */
    uint32_t wow_ph, fl_ph;                     /* LFO phases */
    int32_t drift;                              /* random walk on the wow rate */
    int32_t hp[2], lp1[2], lp2[2];              /* band limit */
    int32_t held_l, held_r;                     /* rate reduction: the held sample */
    uint32_t rate_ph;
    int32_t hiss[2];                            /* hiss low-pass */
    int32_t pop;                                /* crackle */
    uint32_t seed;
    uint8_t stopping;                           /* 1 slowing down, 2 crossfading back to live */
    uint8_t started;
    /* written by onNoteOn/onNoteOff (word and byte stores: safe if render runs in the middle) */
    int32_t dv;                                 /* speed step per frame for the newest note, Q24 */
    uint8_t note_held;
    uint8_t note;
    uint8_t trig, trig_seen;
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

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
    s->buf = (track == OPZ_TRACK_FX1 || track == OPZ_TRACK_FX2) ? (uint32_t *)mem : (uint32_t *)0;
    s->seed = 0x9e3779b9u;
    s->v = V_ONE;
    s->dl = BASE_D;
}

/* params may be NULL (the OS re-sends a held note with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                       /* 255: plug selected, no note */
    int k = (note + 19) % 24;                                  /* F3 (53) = key 0 */
    uint32_t frames = ((uint32_t)STOP_SLOWEST * (uint32_t)opz_exp2_q16(-k * 8192)) >> 16;   /* 1.2 s x 2^(-k / 8) */
    s->dv = (int32_t)((uint32_t)V_ONE / frames);
    s->note = (uint8_t)note;
    s->note_held = 1;
    if (velocity) s->trig++;
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note == s->note) s->note_held = 0;
}

static inline uint32_t rnd(uint32_t *seed) { return *seed = *seed * 1664525u + 1013904223u; }

/* The tape at delay d (Q16 samples) behind write position w: both channels, linear interpolation */
static inline void tape_read(const uint32_t *buf, uint32_t w, int32_t d, int32_t *l, int32_t *r)
{
    uint32_t i = (uint32_t)d >> 16;
    int32_t f = (d & 0xffff) >> 1;                             /* Q15 */
    uint32_t a = buf[(w - i) & RMASK], b = buf[(w - i - 1) & RMASK];
    int32_t al = (int32_t)(a << 16), ar = (int32_t)(a & 0xffff0000u);
    int32_t bl = (int32_t)(b << 16), br = (int32_t)(b & 0xffff0000u);
    *l = opz_add_q31(al, opz_mul_q31_q15(opz_sub_q31(bl, al), f));
    *r = opz_add_q31(ar, opz_mul_q31_q15(opz_sub_q31(br, ar), f));
}

typedef struct Block {
    int32_t drive4, inv_drive;                  /* drive / 4 and 1 / drive, Q31 */
    int32_t bend;                               /* saturation amount k, Q31: 0 clean .. 1 */
    int32_t k_lp, k_hp;                         /* Q15 */
    uint32_t mask, round;                       /* bit reduction (mask 0: off) */
    uint32_t rate_inc;                          /* rate reduction phase step (0: off) */
    int32_t hiss_amp, pop_amp;                  /* Q31 */
    uint32_t pop_th;
} Block;

/* Saturation and the band limit of one channel */
static inline int32_t colour(State *s, const Block *b, int c, int32_t x)
{
    int32_t y = opz_limit_half(opz_shl_q31(opz_mul_q31(x, b->drive4), 1));   /* x drive: soft above -3 dBFS,
                                                                                at most 1 (was a hard clip) */
    int32_t y3 = opz_mul_q31(opz_mul_q31(y, y), y);
    y = opz_mul_q31(opz_sub_q31(y, opz_mul_q31(y3 >> 2, b->bend)), b->inv_drive);   /* x - k x^3 / 4 */
    s->hp[c] = opz_add_q31(s->hp[c], opz_mul_q31_q15(opz_sub_q31(y, s->hp[c]), b->k_hp));
    y = opz_sub_q31(y, s->hp[c]);
    s->lp1[c] = opz_add_q31(s->lp1[c], opz_mul_q31_q15(opz_sub_q31(y, s->lp1[c]), b->k_lp));
    s->lp2[c] = opz_add_q31(s->lp2[c], opz_mul_q31_q15(opz_sub_q31(s->lp1[c], s->lp2[c]), b->k_lp));
    return s->lp2[c];
}

static inline int32_t crush(const Block *b, int32_t x)
{
    return (int32_t)((uint32_t)opz_add_q31(x, (int32_t)b->round) & b->mask);
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block --------------------------------------------------------------------------------------- */
    uint32_t a = (uint32_t)opz_p16(params[OPZ_P_P1]), u2 = (uint32_t)opz_p16(params[OPZ_P_P2]);
    uint32_t a2 = opz_mul_u16(a, a) >> 16;
    Block b;
    int32_t drive_half = 0x40000000 + (int32_t)(a2 << 14);    /* drive / 2: 0.5 .. 1, Q31 */
    b.drive4 = drive_half >> 1;
    b.bend = (int32_t)(a << 15);
    b.inv_drive = opz_recip_q31(drive_half < OPZ_Q31_ONE ? drive_half : OPZ_Q31_ONE);   /* 1 / drive */
    int li = (int)(a >> 13), lf = (int)(a & 0x1fff);
    b.k_lp = k_lp[li] + (((k_lp[li + 1 > 8 ? 8 : li + 1] - k_lp[li]) * lf) >> 13);
    b.k_hp = HP_MIN + (int32_t)(((HP_MAX - HP_MIN) * a2) >> 16);
    uint32_t t = u2 < 32768 ? u2 << 1 : 65535;                 /* the noise half, 0..65535 */
    if (u2 >= 32768) {                                         /* bits 16 -> 4, rate 44.1 -> 3 kHz */
        uint32_t c = (u2 - 32768) << 1;
        int bits = 16 - (int)((12 * c) >> 16);
        b.mask = 0xffffffffu << (32 - bits);
        b.round = 1u << (31 - bits);
        uint32_t r16 = (uint32_t)opz_exp2_q16(-(int32_t)opz_mul_u32_shr16(c, 254148));   /* 2^(-3.878 c) */
        b.rate_inc = r16 >= 65536 ? 0 : r16 << 16;
    } else {
        b.mask = 0;
        b.rate_inc = 0;
    }
    if (t < 256) {
        b.hiss_amp = 0;
        b.pop_amp = 0;
        b.pop_th = 0;
    } else {
        b.hiss_amp = opz_exp2_q16(-(11 << 16) + (int32_t)(6 * t)) << 15;   /* 2^(-11 + 6 t): -66 .. -30 dBFS */
        uint32_t t2 = opz_mul_u16(t, t) >> 16;
        b.pop_amp = (int32_t)(t << 14);                                      /* up to -6 dBFS */
        b.pop_th = opz_mul_u32_shr16(t2, POP_RATE_K);
    }

    int32_t dry, wet;                                          /* the SDK's FX law (equal power: the tape is 12 ms */
    opz_fx_drywet(params[OPZ_P_PAN], &dry, &wet);              /* late, so the wet is not the dry) */
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & OPZ_Q31_ONE), gain);
    int32_t dg = opz_mul_q31(dry, out_gain) >> 1, wg = opz_mul_q31(wet, out_gain);   /* dry at half scale; the wet
                                                                                         sum is held at half */

    uint32_t *buf = s->buf;
    if (buf && s->cleared < RING) {
        volatile uint32_t *z = buf + s->cleared;
        for (int i = 0; i < CLEAR_STEP; i++) z[i] = 0;
        s->cleared += CLEAR_STEP;
    }
    if (!buf || s->cleared < RING) {                           /* clearing (8 blocks) or no buffer: dry only */
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = opz_limit_half(opz_mul_q31(dg, in[i]));
        return;
    }

    /* wow (with a random walk on its rate) and flutter at the end of this block */
    s->drift += opz_noise(&s->seed) >> 13;
    if (s->drift > (int32_t)(WOW_INC / 4)) s->drift = (int32_t)(WOW_INC / 4);
    if (s->drift < -(int32_t)(WOW_INC / 4)) s->drift = -(int32_t)(WOW_INC / 4);
    s->wow_ph += WOW_INC + (uint32_t)s->drift;
    s->fl_ph += FLUTTER_INC;
    int32_t d_end = BASE_D + opz_mul_q31(opz_sine(s->wow_ph), (int32_t)(a2 * WOW_MAX))
                           + opz_mul_q31(opz_sine(s->fl_ph), (int32_t)(a * FLUTTER_MAX));
    if (!s->started) { s->dl = d_end; s->started = 1; }
    int32_t dl = s->dl, d_inc = (d_end - dl) >> 6;

    /* the tape stop: start, restart or release at the block's start */
    uint8_t trig = s->trig;
    int retrig = trig != s->trig_seen;
    s->trig_seen = trig;
    if (s->note_held && (s->stopping == 0 || (retrig && s->stopping == 1))) {
        s->stopping = 1;
    } else if (!s->note_held && s->stopping == 1) {
        s->stopping = 2;
        s->xf = 0;
    }
    int32_t dv = s->dv;

    /* ---- per sample -------------------------------------------------------------------------------------- */
    uint32_t w = s->w;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t xl = in[2 * i], xr = in[2 * i + 1];
        buf[w] = ((uint32_t)xl >> 16) | ((uint32_t)xr & 0xffff0000u);
        dl += d_inc;
        int32_t yl, yr;
        if (s->stopping) {
            int32_t v = s->v;
            if (s->stopping == 1) {
                v -= dv;
                if (v < 0) v = 0;
                s->v = v;
            }
            if (s->dstop < DSTOP_MAX) s->dstop += (V_ONE - v) >> 8;   /* the head falls behind by 1 - speed */
            tape_read(buf, w, dl + s->dstop, &yl, &yr);
            int32_t g = v >> 9;                                /* level follows the speed, Q15 */
            yl = opz_mul_q31_q15(yl, g);
            yr = opz_mul_q31_q15(yr, g);
            if (s->stopping == 2) {                            /* back to the live head */
                int32_t ll, lr;
                tape_read(buf, w, dl, &ll, &lr);
                int32_t x = s->xf += XF_STEP;
                yl = opz_add_q31(yl, opz_mul_q31_q15(opz_sub_q31(ll, yl), x));
                yr = opz_add_q31(yr, opz_mul_q31_q15(opz_sub_q31(lr, yr), x));
                if (x >= 32768) {
                    s->stopping = 0;
                    s->dstop = 0;
                    s->v = V_ONE;
                    yl = ll;
                    yr = lr;
                }
            }
        } else {
            tape_read(buf, w, dl, &yl, &yr);
        }
        w = (w + 1) & RMASK;

        yl = colour(s, &b, 0, yl);
        yr = colour(s, &b, 1, yr);
        if (b.mask) {
            if (b.rate_inc) {                                  /* sample and hold at the reduced rate */
                uint32_t ph = s->rate_ph + b.rate_inc;
                if (ph < s->rate_ph) { s->held_l = crush(&b, yl); s->held_r = crush(&b, yr); }
                s->rate_ph = ph;
                yl = s->held_l;
                yr = s->held_r;
            } else {
                yl = crush(&b, yl);
                yr = crush(&b, yr);
            }
        }
        yl >>= 1;                                              /* half scale from here: hiss and pops add on top */
        yr >>= 1;
        if (b.hiss_amp) {
            s->hiss[0] = opz_add_q31(s->hiss[0], opz_mul_q31_q15(opz_sub_q31(opz_noise(&s->seed), s->hiss[0]), K_HISS));
            s->hiss[1] = opz_add_q31(s->hiss[1], opz_mul_q31_q15(opz_sub_q31(opz_noise(&s->seed), s->hiss[1]), K_HISS));
            uint32_t r = rnd(&s->seed);
            if (r < b.pop_th) {                                /* a pop: random level and sign */
                uint32_t q = rnd(&s->seed);
                int32_t p = opz_mul_q31(b.pop_amp, (int32_t)(q >> 1));
                s->pop = (q & 0x10000u) ? p : -p;
            }
            s->pop = opz_mul_q31_q15(s->pop, POP_DECAY);
            int32_t n = s->pop;
            yl = opz_add_q31(yl, (opz_mul_q31(s->hiss[0], b.hiss_amp) >> 1) + (n >> 1));
            yr = opz_add_q31(yr, (opz_mul_q31(s->hiss[1], b.hiss_amp) >> 1) + (n >> 1));
        }
        out[2 * i] = opz_mul_q31(wg, yl);                     /* the wet at half scale */
        out[2 * i + 1] = opz_mul_q31(wg, yr);
    }
    opz_fx_finish(out, in, dg, dg);                            /* + the dry, through the SDK's limiter */
    s->w = w;
    s->dl = dl;
}

OPZ_EXPORTS_FX();
