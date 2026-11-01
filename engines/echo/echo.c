/*
 * echo — tape echo for FX1 (Echoplex / Space Echo spirit): ping-pong repeats with wow, flutter, tape saturation and
 * tape-age darkening; notes on the FX track tune the tape loop to a pitch and pluck it.
 *
 *   P1        time        echo time 19.8 ms .. 5 s, exponential, 32 steps per octave, exact at 120 BPM:
 *                           85 = 125 ms (1/16), 117 = 250 ms (1/8), 149 = 500 ms (1/4), 181 = 1 s, 213 = 2 s.
 *                         Moving it changes the tape speed: within an octave the delay glides like a tape motor
 *                         (~0.1 s, critically damped) and the echoes bend in pitch on the way (at most 1.3 octaves
 *                         down or up); a jump of more than an octave crossfades to the new time (6 ms)
 *   P2        feedback    one macro: feedback 0 -> 0.9 at 192, then -> 1.4 at 255 (self-oscillation from ~210, held
 *                         by the tape saturator); the tape darkens (low-pass 9 kHz -> 2.5 kHz, 12 dB/oct), thins
 *                         (high-pass 40 -> 110 Hz) and wobbles more (wow and flutter 15 % -> 100 %) as it rises
 *   pan       dry / wet   TE's balance law: wet = min(2p, 1), dry = min(2(1-p), 1); 0 dry, 128 dry + echoes, 255
 *                         echoes only
 *   level     output level (x gain)
 *   pitch bend            bends the tape speed, +-2 semitones at full scale (echo time and note pitch)
 *   notes on the FX track the tape loop becomes one period of the note (Karplus-Strong: the echo rings at the note;
 *                         P2 sets how long, from a short pluck to a few seconds, and from ~204 it drones), and a note with velocity plucks the loop with a burst of tape noise
 *                         (level velocity^2, one period, at least 64 samples; from the echo it waits for the 6 ms
 *                         crossfade). Last note wins; legato notes within an octave glide
 *                         (~12 ms), larger jumps crossfade (6 ms). Releasing every note crossfades back to the
 *                         P1 echo. Notes fold into C1 .. C7 by octaves
 *
 * One mono tape (the input's L + R sum), as on the real machines, with two playback heads: in echo mode the left
 * head is at the echo time and the right head, which also feeds the record head, at twice it, so repeats alternate
 * left, right, left (ping-pong) from one loop. In note mode the right head is one period behind the record head
 * (the loop) and the left head two periods (the same tone, with its own wow: a little width).
 *
 * Record path, per sample: input + feedback -> high-pass -> soft saturator (unity at low level, flat at -6 dBFS) ->
 * two one-pole low-passes -> 16-bit tape. Every pass through the tape is filtered once, so each repeat is darker and
 * more saturated than the one before. Wow (0.5 Hz and 0.83 Hz) and flutter (6.7 Hz) modulate the tape speed: a head
 * D samples behind the record head moves by the speed deviation integrated over those D samples, so a short loop
 * detunes by the speed deviation and a long echo bends in pitch by at most twice it, as on tape.
 *
 * The buffer is not cleared: the engine remembers how much tape it has written since init and points a head that
 * would read older (unwritten) tape at a stretch of silence written in init. The record level fades in over the first
 * 23 ms so the first echo after selecting the plug starts from silence.
 *
 * The filter and resonance knobs are the OS's own track filter (also on FX tracks); echo never reads them.
 *
 * Clean-room design; no code from any tape echo emulation. Q31 audio, Q12 delays, no floating point, no libc, no
 * 64-bit maths. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define TAPE_BITS       19                      /* FX1: 2^19 samples of int16 = 1 MB (10 s loop for a 5 s echo) */
#define TAPE_BITS_SMALL 17                      /* any other track: 256 KB, FX2's whole pool */
#define MEM_BYTES       ((uint32_t)2 << TAPE_BITS)
#define MARGIN          1024                    /* samples kept free behind the longest loop (wow, ramps) */
#define ZERO_LEN        64                      /* silence written in init: heads on unwritten tape read it */
#define ZERO_IDX        32
#define XF_BLOCKS       4                       /* crossfade between head settings: 256 samples, 5.8 ms */
#define FADE_BLOCKS     16                      /* record level fade-in after init */
#define Q31_ONE         0x7fffffff
#define FREE_LOOP_Q12   45158400                /* loop at P1 85: 2 x 125 ms = 11025 samples, Q12 */
#define NOTE_P_Q12      410531                  /* period of A4 (440 Hz) in samples, Q12 */
#define D_MIN_Q12       (8 << 12)
#define SLEW_UP_Q12     157286                  /* per block: the head may move 0.6 samples per sample later ... */
#define SLEW_DN_Q12     393216                  /* ... and 1.5 earlier (pitch -1.3 / +1.3 octaves at most) */
#define SAT_MAX         0x5ff00000              /* saturator input clamp, just below 0.75 (output just below 0.5) */
#define SAT_C3          19418                   /* 16/27, Q15: y = z - 16/27 z^3 is flat at z = 0.75 */
#define NOTE_LO         24                      /* C1 */
#define NOTE_HI         96                      /* C7 */
#define HELD_MAX        8
#define MODE_FREE       0
#define MODE_NOTE       1

/* Wow and flutter: rate (Q32 turns per sample) and the largest speed deviation as a delay scale,
 * S = eps fs / (2 pi f) samples in Q12 (wow 0.35 % and 0.2 %, flutter 0.15 %). */
static const uint32_t k_wow_inc[3] = { 48696, 80835, 652521 };          /* 0.5, 0.83, 6.7 Hz */
static const int32_t k_wow_s[3] = { 201240, 69280, 6437 };

typedef struct Heads {                          /* one setting of the two playback heads */
    int32_t D, T;                               /* loop (right head) delay and its target, samples in Q12 */
    int32_t v;                                  /* how fast D moves, Q12 per block (the tape motor's inertia) */
    int32_t prev[2];                            /* left, right head delay reached at the end of the last block */
    uint8_t mode;                               /* MODE_FREE (left head at D / 2) or MODE_NOTE (left at 2 D) */
    uint8_t zero;                               /* bit c: head c read silence last block */
} Heads;

typedef struct State {
    int16_t *tape;
    uint32_t mask;                              /* tape length - 1 */
    uint32_t w;                                 /* record head */
    uint32_t filled;                            /* samples written since init (saturates at the tape length) */
    int32_t d_max;                              /* longest loop, Q12 */
    Heads a, b;                                 /* a: playing; b: fading in while xf > 0 */
    uint32_t phase[3];                          /* wow and flutter, Q32 turns */
    int32_t hx, hy, l1, l2;                     /* high-pass input / output, low-pass outputs */
    uint32_t seed;
    int32_t dg, wg;                             /* dry and wet output gains of the last block, Q31 */
    int32_t recg;                               /* record gain of the last block, Q31 */
    int32_t g_last;                             /* feedback / 2 of the last block, Q31 */
    uint32_t p2;                                /* the P2 word the coefficients below were made for */
    int32_t g_half;                             /* feedback / 2, Q31 */
    int32_t k_q15;                              /* low-pass coefficient */
    int32_t hp_q15;                             /* high-pass coefficient, echo mode */
    int32_t wow_s[3];                           /* wow and flutter depth, Q12 samples */
    int32_t hiss;                               /* noise level, Q15 */
    int32_t note_key;                           /* note mode, made for (note, bend) and P2 word note_p2: */
    uint32_t note_p2;
    int32_t note_T, note_hp, note_g;            /* loop (Q12), high-pass (Q15), feedback / 2 (Q31) */
    int32_t pluck_level;                        /* Q15 */
    uint8_t xf, fade, started, pluck_blocks, pluck_vel, nheld;
    uint8_t held[HELD_MAX];                     /* held notes, oldest first (folded) */
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in 200 bytes");

static const ModuleCallbackInterface *s_cb;

/* ---- per-note maths: Q30 helpers (32-bit, the divisions are libgcc's 32-bit ones) ------------------------------ */

static inline int32_t mul30(int32_t a, int32_t b) { return opz_mul_q31(a, b) * 2; }

/* num / den in Q30 for 0 <= num <= 2 den (den > 0), ~15 bits. */
static int32_t div30(uint32_t num, uint32_t den)
{
    if (num >= 2 * den) return 0x7fffffff;
    while (den < 0x40000000u) { den <<= 1; num <<= 1; }
    return (int32_t)((num / (den >> 15)) << 15);
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

/* atan2(y, x) / pi in Q31 for y >= 0, x > 0 (Q30 both): the phase lag of 1 / (1 - a e^-jw). */
static int32_t atan2_pi(int32_t y, int32_t x)
{
    if (y <= 0) return 0;
    int32_t t = y <= x ? atan29(div30((uint32_t)y, (uint32_t)x))
                       : 843314857 - atan29(div30((uint32_t)x, (uint32_t)y));          /* pi/2 - atan(x / y) */
    return t + opz_mul_q31(t, 586786526);       /* x 4 / pi: Q29 radians -> Q31 half turns */
}

/* Phase lag / pi of a one-pole with pole a (Q15) at the angle of inc (Q32 turns per sample). */
static int32_t pole_lag(uint32_t inc, int32_t a_q15)
{
    int32_t sn = opz_sine(inc), cs = opz_sine(inc + 0x40000000u);
    int32_t a = a_q15 << 16;
    return atan2_pi(opz_mul_q31(a, sn) >> 1, 0x40000000 - (opz_mul_q31(a, cs) >> 1));
}

/* Octaves of a note (folded, + bend in Q16 semitones) below A4, Q16. */
static int32_t note_oct(int note, int32_t bend_q16)
{
    return (((69 - note) * 349525) >> 6) - opz_mul_q31(bend_q16, 178956971);   /* 2^22 / 12, 2^31 / 12 */
}

/* Note mode, from the newest note, the bend and P2: the line delay (Q12) that makes the loop one period of the note,
 * the high-pass (at 1/64 of the note's frequency: it only stops DC from building up when the feedback is above 1, and
 * its phase lead stays small at every harmonic) and the loop gain.
 *
 * Tuning: the line is the period minus the phase delay of the two low-passes and the high-pass at the note. A
 * low-pass delays by tau = lag / w = (lag / pi) P / 2; the high-pass leads by pi/2 - w/2 - lag, so it adds
 * lag / w - P / 4 + 1/2.
 *
 * Gain: P2 sets how long the note rings, T60 = 60 ms * 2^(7 u) (60 ms at 0, ~0.7 s at 128, ~2.4 s at 192): per pass
 * 2^-(9.97 P / (fs T60)), times the low-passes' loss at the note, 1 / |H|^2 = 1 + 2a (1 - cos w) / k^2 per pole pair,
 * so dark settings still ring at the fundamental (the harmonics die faster; the gain may pass 1, the loop's gain at
 * the note stays below it). Where the echo's own
 * feedback is larger (P2 above ~204, beyond 1) that wins: the note drones. */
static void note_setup(State *s, int note, int32_t bend_q16)
{
    int32_t oct = note_oct(note, bend_q16);
    int32_t P = (int32_t)opz_mul_u32_shr16(NOTE_P_Q12, (uint32_t)opz_exp2_q16(oct));
    uint32_t inc = opz_hz_to_inc((int32_t)opz_mul_u32_shr16(440u << 16, (uint32_t)opz_exp2_q16(-oct)));
    int32_t hw = (int32_t)(opz_mul_u32_shr16(inc, 3217) >> 16);              /* w / 64 in Q15 (3217 = 2 pi 2^9) */
    int32_t hp = 32768 - (hw < 1 ? 1 : hw);
    int32_t k = s->k_q15, a = 32768 - k;
    int32_t lp = opz_mul_q31(pole_lag(inc, a), P) >> 1;
    int32_t hd = (opz_mul_q31(pole_lag(inc, hp), P) >> 1) - (P >> 2) + 2048;
    s->note_T = P - 2 * lp - hd;
    s->note_hp = hp;

    int32_t u = opz_p16(s->p2);
    int32_t e = (int32_t)opz_mul_u32_shr16(opz_mul_u32_shr16((uint32_t)P, 3949),   /* 9.97 / (44100 * 0.06) / 2^12 */
                                           (uint32_t)opz_exp2_q16(-7 * u));       /* octaves per pass, Q16 */
    int32_t g = opz_exp2_q16(-e);                                                 /* Q16 */
    int32_t omc = Q31_ONE - opz_sine(inc + 0x40000000u);                          /* 1 - cos w */
    int32_t num = opz_mul_q31(a << 16, omc);                                      /* a (1 - cos w), Q30 */
    int32_t den = opz_mul_q31(k << 16, k << 16) >> 1;                             /* k^2 / 2, Q30 */
    int32_t comp = (1 << 30) + div30((uint32_t)num, (uint32_t)(den > 0 ? den : 1));
    g = (int32_t)opz_mul_u32_shr16((uint32_t)g, (uint32_t)comp >> 14);            /* g (1 + 2a(1-cos)/k^2), Q16 */
    if (g > 91750) g = 91750;                                  /* 1.4: the loop gain at the note stays below 1 */
    s->note_g = g << 14 > s->g_half ? g << 14 : s->g_half;                        /* g / 2, Q31 */
}

/* ---- exports ------------------------------------------------------------------------------------------------- */

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
    s->tape = (int16_t *)mem;
    /* FX1's pool is 2 MB; anywhere else use no more than FX2's 256 KB (the pools sit back to back) */
    uint32_t len = 1u << (track == OPZ_TRACK_FX1 ? TAPE_BITS : TAPE_BITS_SMALL);
    s->mask = len - 1;
    s->d_max = (int32_t)((len - MARGIN) << 12);
    if (mem) {
        volatile int16_t *t = s->tape;                         /* 128 bytes: the rest is never read unwritten */
        for (int i = 0; i < ZERO_LEN; i++) t[i] = 0;
    }
    s->w = s->filled = ZERO_LEN;
    s->phase[1] = 0x55555555u;
    s->phase[2] = 0xaaaaaaaau;
    s->seed = 0x2545f491u;
    s->p2 = 0x80000000u;                                       /* no P2 word has bit 31 set */
    s->note_key = -1;
}

static int fold(int note)
{
    while (note < NOTE_LO) note += 12;
    while (note > NOTE_HI) note -= 12;
    return note;
}

static void unhold(State *s, uint8_t n)
{
    int j = 0;
    for (int i = 0; i < s->nheld; i++)
        if (s->held[i] != n) s->held[j++] = s->held[i];
    s->nheld = (uint8_t)j;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;                                              /* may be NULL (a held note handed back) */
    State *s = state;
    if (note < 0 || note > 127) return;                        /* 255: plug selected, no note */
    uint8_t n = (uint8_t)fold(note);
    unhold(s, n);
    if (s->nheld == HELD_MAX) unhold(s, s->held[0]);
    s->held[s->nheld++] = n;                                   /* the newest note plays */
    if (velocity) s->pluck_vel = velocity;                     /* velocity 0: the OS re-sending a held note */
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note < 0 || note > 127) return;
    unhold(s, (uint8_t)fold(note));
}

/* ---- render -------------------------------------------------------------------------------------------------- */

#define ALWAYS_INLINE static inline __attribute__((always_inline))

typedef struct Block {
    uint32_t rd[4], rinc[4];                    /* read positions (record head - delay, Q12) and their steps */
    int32_t k, hp, nl;                          /* low-pass, high-pass, noise (Q15) */
    int32_t g, g_inc, recg, recg_inc, dg, dg_inc, wg, wg_inc;   /* Q31 ramps: feedback / 2, gains */
    int32_t xg;                                 /* crossfade position, Q31 */
} Block;

/* Head c of a setting for one block: target delay e (Q12) from the head's base and the wow, ramped from where it
 * was. A head that would read unwritten tape reads the silence written in init instead. */
static void head_block(State *s, Heads *h, int c, int32_t base, const int32_t *cph, Block *b, int slot)
{
    int32_t e = base;
    for (int k = 0; k < 3; k++) {                              /* + sum S (cos phi - cos(phi - w D)) */
        uint32_t th = opz_mul_u32_shr16((uint32_t)base, k_wow_inc[k]) << 4;
        int32_t c1 = opz_sine(s->phase[k] - th + 0x40000000u);
        e += opz_mul_q31(s->wow_s[k], (cph[k] >> 1) - (c1 >> 1)) * 2;
    }
    if (e < D_MIN_Q12) e = D_MIN_Q12;
    int32_t st = (h->zero >> c) & 1 ? e : h->prev[c];
    int32_t hi = st > e ? st : e;
    int32_t inc;
    if ((uint32_t)(hi >> 12) + 2 > s->filled) {                /* unwritten tape: read the silence */
        st = (int32_t)(((s->w - ZERO_IDX) & s->mask) << 12);
        inc = 1 << 12;
        h->zero |= (uint8_t)(1 << c);
    } else {
        inc = (e - st) >> 6;
        h->zero &= (uint8_t)~(1 << c);
    }
    h->prev[c] = st + inc * OPZ_BLOCK_FRAMES;
    b->rd[slot] = (s->w << 12) - (uint32_t)st;
    b->rinc[slot] = (uint32_t)((1 << 12) - inc);
}

/* Glide a setting's loop delay towards its target like a tape motor: the speed it wants is a one-pole (1/2^shift of
 * the distance per block) within the speed limits, and the speed itself follows that over 2^vshift blocks, so the
 * pitch of the echoes bends smoothly instead of jumping (critically damped or slower: no overshoot). */
static void glide(Heads *h, int shift, int vshift)
{
    int32_t want = (h->T - h->D) >> shift;
    if (want > SLEW_UP_Q12) want = SLEW_UP_Q12;
    if (want < -SLEW_DN_Q12) want = -SLEW_DN_Q12;
    h->v += (want - h->v) >> vshift;
    h->D += h->v;
}

static void start_heads(Heads *h, int mode, int32_t T)
{
    h->mode = (uint8_t)mode;
    h->D = h->T = T;
    h->v = 0;
    h->zero = 3;                                               /* no ramp in the first block */
}

/* One tape head: linear interpolation between the two samples around the read position, Q31 (tape = 2 y). */
ALWAYS_INLINE int32_t tap(const int16_t *tape, uint32_t mask, uint32_t rd)
{
    uint32_t i = (rd >> 12) & mask;
    int32_t f = (int32_t)(rd & 0xfff);
    return (tape[i] * (4096 - f) + tape[(i + 1) & mask] * f) << 3;        /* two 16 x 16 multiplies */
}

ALWAYS_INLINE void run(State *s, Block *b, const fract32 *in, fract32 *out, int xfade, int fading)
{
    int16_t *tape = s->tape;
    uint32_t mask = s->mask, w = s->w, seed = s->seed;
    uint32_t r0 = b->rd[0], r1 = b->rd[1], r2 = b->rd[2], r3 = b->rd[3];
    int32_t hx = s->hx, hy = s->hy, l1 = s->l1, l2 = s->l2;
    int32_t g = b->g, recg = b->recg, dg = b->dg, wg = b->wg, xg = b->xg;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t inl = in[2 * i], inr = in[2 * i + 1];
        int32_t L = tap(tape, mask, r0), R = tap(tape, mask, r1);
        r0 += b->rinc[0];
        r1 += b->rinc[1];
        if (xfade) {                                           /* fade from setting a (L, R) to b */
            int32_t L2 = tap(tape, mask, r2), R2 = tap(tape, mask, r3);
            r2 += b->rinc[2];
            r3 += b->rinc[3];
            int32_t x2 = opz_mul_q31(xg, xg);
            int32_t sx = x2 + 2 * opz_mul_q31(x2, Q31_ONE - xg);   /* smoothstep 3x^2 - 2x^3: no kinks */
            L += opz_mul_q31(sx, L2 - L);                      /* |tap| <= 0.5: no overflow */
            R += opz_mul_q31(sx, R2 - R);
            xg += 1 << 23;
        }

        /* record: mono input (+ hiss or pluck) + feedback from the right head */
        seed = seed * 1664525u + 1013904223u;
        int32_t x = (inl >> 1) + (inr >> 1);
        if (fading) {                                          /* the record level fades in after init */
            x = opz_mul_q31_q15(x, recg >> 16);
            recg += b->recg_inc;
        }
        x = opz_add_q31(x, opz_mul_q31_q15((int32_t)seed, b->nl));
        int32_t u = opz_add_q31(x, opz_shl_q31(opz_mul_q31_q15(R, g >> 16), 1));
        g += b->g_inc;
        hy = opz_mul_q31_q15(opz_add_q31(hy, opz_sub_q31(u, hx)), b->hp);     /* high-pass y = a (y + x - x1) */
        hx = u;
        int32_t z = hy > SAT_MAX ? SAT_MAX : (hy < -SAT_MAX ? -SAT_MAX : hy);
        int32_t zh = z >> 16;                                  /* the cubic term to 16 bits (error < -110 dB) */
        int32_t y = z - opz_mul_q31_q15(opz_mul_q31_q15(opz_mul_q31_q15(z, zh), zh), SAT_C3);
        l1 += opz_mul_q31_q15(y - l1, b->k);                   /* tape: two one-pole low-passes */
        l2 += opz_mul_q31_q15(l1 - l2, b->k);
        tape[w] = (int16_t)((l2 + 0x4000) >> 15);              /* |l2| < 0.5: fits 16 bits */
        w = (w + 1) & mask;

        int32_t d16 = dg >> 16, w16 = wg >> 16;                /* the gains to 16 bits, ramped to 31 */
        out[2 * i] = opz_add_q31(opz_mul_q31_q15(inl, d16), opz_mul_q31_q15(L, w16));
        out[2 * i + 1] = opz_add_q31(opz_mul_q31_q15(inr, d16), opz_mul_q31_q15(R, w16));
        dg += b->dg_inc;
        wg += b->wg_inc;
    }
    s->w = w;
    s->seed = seed;
    s->hx = hx; s->hy = hy; s->l1 = l1; s->l2 = l2;
}

/* P2 -> feedback, tape filters, wow and flutter, hiss (only when P2 moves). */
static void p2_coefs(State *s, uint32_t word)
{
    int32_t u = opz_p16(word);                                 /* 0..65535 */
    int32_t g = u < 49152 ? (int32_t)((uint32_t)u * 78643u >> 16) : 58982 + 2 * (u - 49152);   /* Q16: 1.2 u, */
    s->g_half = g << 14;                                       /* then 0.9 + 2 (u - 0.75) -> 1.4; stored as g / 2 */
    /* low-pass 9 kHz * 2^(-1.848 u): k = 1 - e^-w, w log2(e) = 1.85 * 2^(-1.848 u) */
    int32_t e2 = (int32_t)opz_mul_u32_shr16(121242, (uint32_t)opz_exp2_q16(-(int32_t)((uint32_t)u * 121110u >> 16)));
    s->k_q15 = (65536 - opz_exp2_q16(-e2)) >> 1;
    /* high-pass 40 Hz * 2^(1.459 u): a = 1 - 2 pi fc / fs */
    int32_t wq = (int32_t)opz_mul_u32_shr16(187, (uint32_t)opz_exp2_q16((int32_t)((uint32_t)u * 95644u >> 16)));
    s->hp_q15 = 32768 - wq;
    int32_t u2 = (int32_t)(opz_mul_u16((uint32_t)u, (uint32_t)u) >> 16);
    int32_t amt = 9830 + (int32_t)(opz_mul_u16((uint32_t)u2, 55705) >> 16);           /* 0.15 + 0.85 u^2, Q16 */
    for (int k = 0; k < 3; k++) s->wow_s[k] = (int32_t)opz_mul_u32_shr16((uint32_t)k_wow_s[k], (uint32_t)amt);
    s->hiss = (u2 * 5) >> 16;                                  /* 0..4 (Q15): up to -78 dBFS */
    s->p2 = word;
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    State *s = state;
    uint32_t p2 = params[OPZ_P_P2] & Q31_ONE;
    if (p2 != s->p2) p2_coefs(s, p2);

    /* dry / wet (TE's balance law) and level, as output gains ramped over the block */
    int32_t pan = opz_pq31(params[OPZ_P_PAN]);
    int32_t wet = pan >= 0x40000000 ? Q31_ONE : pan * 2;
    int32_t dry = pan <= 0x40000000 ? Q31_ONE : (Q31_ONE - pan) * 2;
    int32_t og = opz_mul_q31(opz_pq31(params[OPZ_P_LEVEL]), gain);
    int32_t dg1 = opz_mul_q31(dry, og), wg1 = opz_mul_q31(wet, og);

    if (!s->tape) {                                            /* no buffer: dry only */
        for (int j = 0; j < 2 * OPZ_BLOCK_FRAMES; j++) out[j] = opz_mul_q31(in[j], dg1);
        return;
    }

    /* targets: the P1 echo (loop = twice the echo time) and, with a note held, one period of the newest note */
    int32_t bend_q16 = pitchBend >> 14;                        /* full scale = 2 semitones */
    int32_t oct = (int32_t)(((uint32_t)opz_p16(params[OPZ_P_P1]) * 255u) >> 5) - 174080
                - opz_mul_q31(bend_q16, 178956971);           /* (P1 - 85) / 32 octaves - bend / 12 */
    int32_t t_free = (int32_t)opz_mul_u32_shr16(FREE_LOOP_Q12, (uint32_t)opz_exp2_q16(oct));
    if (t_free > s->d_max) t_free = s->d_max;
    int mode = s->nheld ? MODE_NOTE : MODE_FREE;
    int32_t t_want = t_free;
    if (mode == MODE_NOTE) {
        int32_t key = (s->held[s->nheld - 1] << 24) ^ (bend_q16 & 0xffffff);
        if (key != s->note_key || s->p2 != s->note_p2) {
            note_setup(s, s->held[s->nheld - 1], bend_q16);
            s->note_key = key;
            s->note_p2 = s->p2;
        }
        t_want = s->note_T;
    }
    /* head settings: follow the target (glide), or crossfade to a new setting for a change of mode or a jump of
     * more than an octave; while a crossfade runs the new setting takes the target when it can */
    if (!s->started) {
        start_heads(&s->a, mode, t_want);
        s->started = 1;
    }
    Heads *cur = s->xf ? &s->b : &s->a;
    int fits = mode == cur->mode && (t_want >> 1) <= cur->D && t_want >= (cur->D >> 1);   /* within an octave */
    if (fits) cur->T = t_want;
    else if (!s->xf) {
        start_heads(&s->b, mode, t_want);
        s->xf = XF_BLOCKS;
    }
    if (s->xf && s->a.mode == MODE_FREE) s->a.T = t_free;      /* the fading echo keeps following P1 */
    if (s->pluck_vel && !s->xf) {                              /* one period of tape noise, velocity^2, once the
                                                                  loop is in (a pluck into a crossfading loop
                                                                  would mostly be lost) */
        s->pluck_level = (s->pluck_vel * s->pluck_vel) >> 1;   /* 127^2 / 2 = 0.25 in Q15 */
        s->pluck_blocks = (uint8_t)(mode == MODE_NOTE ? ((t_want >> 18) + 1 > 16 ? 16 : (t_want >> 18) + 1) : 0);
        s->pluck_vel = 0;
    }
    glide(&s->a, s->a.mode == MODE_FREE ? 6 : 3, s->a.mode == MODE_FREE ? 4 : 1);
    if (s->xf) glide(&s->b, s->b.mode == MODE_FREE ? 6 : 3, s->b.mode == MODE_FREE ? 4 : 1);

    /* wow and flutter at the end of the block, and every head's read ramp */
    int32_t cph[3];
    for (int k = 0; k < 3; k++) {
        s->phase[k] += k_wow_inc[k] * OPZ_BLOCK_FRAMES;
        cph[k] = opz_sine(s->phase[k] + 0x40000000u);
    }
    Block b;
    for (int set = 0; set < (s->xf ? 2 : 1); set++) {
        Heads *h = set ? &s->b : &s->a;
        int32_t left = h->mode == MODE_FREE ? h->D >> 1 : h->D * 2;
        head_block(s, h, 0, left, cph, &b, 2 * set);
        head_block(s, h, 1, h->D, cph, &b, 2 * set + 1);
    }

    b.g = s->g_last;
    int note_newest = (s->xf ? s->b.mode : s->a.mode) == MODE_NOTE;
    int32_t g1 = note_newest ? s->note_g : s->g_half;
    b.g_inc = (g1 - s->g_last) >> 6;
    b.k = s->k_q15;
    b.hp = note_newest ? s->note_hp : s->hp_q15;
    b.nl = s->hiss + (s->pluck_blocks ? s->pluck_level : 0);
    if (s->pluck_blocks) s->pluck_blocks--;
    int32_t rec1 = s->fade >= FADE_BLOCKS ? Q31_ONE : s->fade << 27;   /* fade / 16 */
    if (s->fade <= FADE_BLOCKS) s->fade++;
    b.recg = s->recg;
    b.recg_inc = (rec1 - s->recg) >> 6;
    b.dg = s->dg;
    b.dg_inc = (dg1 - s->dg) >> 6;
    b.wg = s->wg;
    b.wg_inc = (wg1 - s->wg) >> 6;
    b.xg = (XF_BLOCKS - s->xf) << 29;

    if (b.recg < Q31_ONE) {                                    /* still fading in */
        if (s->xf) run(s, &b, in, out, 1, 1);
        else run(s, &b, in, out, 0, 1);
    } else {
        if (s->xf) run(s, &b, in, out, 1, 0);
        else run(s, &b, in, out, 0, 0);
    }

    s->recg = rec1;                                            /* the ramps end within 63 LSB of these */
    s->g_last = g1;
    s->dg = dg1;
    s->wg = wg1;
    s->filled += OPZ_BLOCK_FRAMES;
    if (s->filled > s->mask) s->filled = s->mask + 1;
    if (s->xf && !--s->xf) s->a = s->b;                        /* crossfade done: the new setting plays */
}

OPZ_EXPORTS_FX();
