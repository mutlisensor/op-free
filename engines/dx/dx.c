/*
 * dx — six-operator FM voice with the 32 DX7 algorithms and a bank of 32 patches.
 *
 *   P1        patch      32 patches of 8 values each (P1 / 8), read at note-on: a held note keeps its patch, the next
 *                        note plays the new one (no clicks, no jumps inside a note)
 *   P2        brightness 128 plays the patch as written. Below 128 every modulator is turned down, up to 24 dB at 0
 *                        (darker, towards pure carriers); above 128 up, up to 12 dB at 255 (brighter, more feedback
 *                        grit, since an operator's feedback follows its own level). Carriers never change, so the
 *                        level stays put
 *   velocity             each operator's own velocity sensitivity (0-7), as on the DX7
 *   envelopes            every operator runs its own 4-rate/4-level DX-style envelope (timbre and the patch's own
 *                        amplitude contour); the OP-Z envelope (attack .. release) is an overall amplifier on top. Note
 *                        off releases both. The voice ends when the OP-Z envelope ends or, earlier, when every carrier
 *                        has finished (silent and in sustain or release). Stage 5/6 fades follow the OP-Z envelope
 *
 * Per operator: frequency ratio or fixed frequency with fine tune and detune, output level, envelope with rate
 * scaling, keyboard level scaling (break point, left/right depth and curve), velocity sensitivity, amplitude
 * modulation sensitivity. Per patch: algorithm, feedback, oscillator key sync, LFO (6 waves, delay, pitch and amplitude
 * depth), pitch envelope, transpose. The bank is in the DX7 32-voice packed format (s_bank, 4,096 bytes), the size of
 * a file read by getPresetData (callback +0x28): a later version can load a bank sent by an editor (F-04).
 *
 * How a block is made (64 frames): the envelopes, LFO and pitch envelope advance once per block and give each
 * operator a gain at the block's end, ramped linearly across the block; then the operators run as one pass each over
 * the block, from operator 6 down to 1, reading their modulation from a bus and writing into a bus or the carrier sum
 * (dx_tables.h, k_alg); then the carrier sum goes through the OP-Z envelope into the track buffer. Operators that are
 * silent for the whole block are skipped. Sine: a 4,096-entry table without interpolation. Modulation depth: an
 * operator at output level 99 with its envelope at 99 swings its targets' phase by 4 pi rad (2 turns).
 *
 * The filter and resonance knobs are the OS's own track filter, which runs after the engine; dx never reads them.
 *
 * Unit conversions (levels, envelope speeds, LFO rates, pitch envelope levels) follow the published behaviour of the
 * DX7 as modelled by the MIT-licensed 6-op FM code below; the fixed-point code, the bus scheme and the patches are
 * our own. MIT licence, see CONTRIBUTING.md.
 *
 * Portions of the unit conversions (operator level, LFO frequency, pitch envelope level and the envelope's
 * qrate/attack behaviour) are derived from a 6-op FM implementation with this notice:
 *
 *   Copyright 2021 Emilie Gillet.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 *   the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"

#include "dx_bank.h"
#include "dx_tables.h"

#define ALWAYS_INLINE static inline __attribute__((always_inline))

#define NOPS         6
#define NPATCHES     32
#define PATCH_BYTES  128
#define OP_BYTES     17

/* Patch bytes (DX7 packed voice). Operator k (0 = operator 1) starts at (5 - k) * 17. */
enum { O_R1 = 0, O_L1 = 4, O_BP = 8, O_LD, O_RD, O_CURVES, O_RS_DET, O_AMS_KVS, O_OL, O_MODE_COARSE, O_FINE };
enum { P_PR1 = 102, P_PL1 = 106, P_ALG = 110, P_FB_SYNC, P_LFO_SPEED, P_LFO_DELAY, P_LFO_PMD, P_LFO_AMD,
       P_LFO_WAVE, P_TRANSPOSE };

/* Envelope segments */
enum { SEG_FALL, SEG_RISE, SEG_FLAT, SEG_HOLD };

/* Levels: DX steps of 0.75 dB (8 per octave). Envelope levels are Q16 steps (0 .. 127 << 16); static attenuations
 * are Q4 steps; per block everything is added up in Q16 octaves. */
#define LEV_FLOOR      (56 << 16)   /* a rising segment starts no lower than this (-53 dB): the DX attack's jump */
#define ATT_SILENT     (14 << 16)   /* Q16 octaves: at or beyond this a gain is 0 (-84 dB re. twice full scale) */
#define ATT_MIN        (-65536)     /* Q16 octaves: at most twice the level of output level 99 (P2 or a + curve) */
#define BRIGHT_DOWN    512          /* P2 0: modulators 32 steps (24 dB) down */
#define BRIGHT_UP      256          /* P2 255: 16 steps (12 dB) up */
#define NORM_MAX_BOOST (-256)       /* a patch with quiet carriers is raised by at most 12 dB */
#define INC_16K        1558261042u  /* 16 kHz as a phase increment */
#define INC_20K        1947826302u  /* 20 kHz */

typedef struct Op {
    uint32_t ph;         /* phase, Q32 turns */
    uint32_t mul;        /* ratio mode: frequency / note frequency, Q16 (with fine, detune, transpose); fixed: increment */
    int32_t  lev;        /* envelope level, Q16 steps */
    int32_t  from;       /* start of a rising segment */
    int32_t  p, dp;      /* progress through a rising or flat segment and its step per block, Q24 */
    int32_t  g;          /* gain at the end of the last block, Q31 (1.0 = twice output level 99: 4 turns of PM) */
    int16_t  att;        /* static attenuation: output level, velocity, keyboard scaling (Q4 steps) */
    uint8_t  stage;      /* envelope stage: 0..2 towards L1..L3, 3 sustain, 4 release, 5 done */
    uint8_t  seg;        /* SEG_* */
    uint8_t  rs;         /* rate scaling for this note, in qrate steps */
    uint8_t  pad[3];
} Op;

typedef struct State {
    Op       op[NOPS];   /* op[0] is operator 1 */
    int32_t  fb[2];      /* last two outputs of the feedback operator (or loop) */
    uint32_t lfo_ph;     /* LFO phase, Q32 */
    int32_t  lfo_fade;   /* LFO fade-in after note-on, Q24 */
    int32_t  lfo_sh;     /* sample and hold value, Q15 */
    uint32_t seed;       /* S&H noise */
    int32_t  peg;        /* pitch envelope, octaves Q24 */
    int16_t  norm;       /* carrier normalisation, Q4 steps (added to every carrier's attenuation) */
    uint8_t  patch;      /* bank index, latched at note-on */
    uint8_t  peg_stage;  /* 0..2 towards L1..L3, 3 hold, 4 towards L4, 5 done */
    uint8_t  released;
    uint8_t  note, vel;  /* as given to onNoteOn (to restart with another patch, see render) */
    uint8_t  fresh;      /* no block rendered since note-on */
    uint32_t p1_on;      /* params[P1] at note-on */
} State;

_Static_assert(sizeof(Op) == 36, "Op layout");
_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;

/* The bank. Writable on purpose: a later version can fill it from engines/fmedit.preset (getPresetData). Every byte
 * is range-checked where it is used, so any 4,096 bytes play without crashing. */
static uint8_t s_bank[NPATCHES * PATCH_BYTES] = DX_BANK_INIT;

/* Block buffers, shared by all voices (render runs one voice at a time): bus A, bus B, carrier sum. */
static int32_t s_bus[3][OPZ_BLOCK_FRAMES];

/* ---- small helpers ---------------------------------------------------------------------------------------------- */

ALWAYS_INLINE int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
ALWAYS_INLINE int b99(uint8_t b) { return b > 99 ? 99 : b; }

/* a (low 16 bits, Q15) x b (high 16 bits, Q15) as a Q31 fractional product: one Blackfin multiply. */
#if defined(__bfin__)
ALWAYS_INLINE int32_t mul_lh(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.L * %2.H;" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
#else
ALWAYS_INLINE int32_t mul_lh(int32_t a, int32_t b)
{
    int32_t p = (int32_t)(int16_t)a * (int32_t)(int16_t)(b >> 16);
    return p == 0x40000000 ? 0x7fffffff : p * 2;
}
#endif

ALWAYS_INLINE const uint8_t *patch_ptr(unsigned n) { return s_bank + (n & (NPATCHES - 1)) * PATCH_BYTES; }
ALWAYS_INLINE const uint8_t *op_ptr(const uint8_t *P, int k) { return P + (5 - k) * OP_BYTES; }

/* log2(x / 65536) in Q16, x > 0. Per note. */
static int32_t log2_q16(uint32_t x)
{
    int32_t n = 0;
    while (x >= (2u << 16)) { x >>= 1; n++; }
    while (x < (1u << 16)) { x <<= 1; n--; }
    uint32_t f = x - 65536, i = f >> 11, r = f & 2047;        /* 32 segments of 2048 */
    int32_t a = k_log2[i], b = k_log2[i + 1];
    return n * 65536 + a + (int32_t)(((uint32_t)(b - a) * r) >> 11);
}

/* 2^-(att + 1) in Q31 (1.0 = twice output level 99): the gain for an attenuation in Q16 octaves. */
static int32_t gain_of(int32_t att)
{
    if (att >= ATT_SILENT) return 0;
    if (att < ATT_MIN) att = ATT_MIN;
    int32_t g16 = opz_exp2_q16(-(att + 65536));              /* <= 65536 */
    return g16 >= 65536 ? 0x7fffffff : g16 << 15;
}

/* ---- envelopes -------------------------------------------------------------------------------------------------- */

/* Envelope speed for a rate 0-99 and rate scaling: steps per block, Q16. The DX7's qrate: 4 qrates per octave. */
ALWAYS_INLINE int32_t eg_speed(int rate, int rs)
{
    int qr = ((b99((uint8_t)rate) * 41) >> 6) + rs;
    if (qr > 63) qr = 63;
    return (4 + (qr & 3)) << (3 + (qr >> 2));
}

/* Start envelope stage `stage` from the current level: falling segments move at the rate's speed; rising ones jump to
 * LEV_FLOOR at least and then take as long as a 7.2-times-shorter fall over the same distance, on a curve that rises
 * fast and settles; flat ones (same level) hold for the time a fall of 1.33 octaves would take. */
static void eg_enter(Op *op, const uint8_t *o, int stage)
{
    for (;;) {
        op->stage = (uint8_t)stage;
        if (stage == 3 || stage >= 5) {
            op->seg = SEG_HOLD;
            return;
        }
        int li = stage == 4 ? 3 : stage;
        int32_t to = (int32_t)k_tl[b99(o[O_L1 + li])] << 16, from = op->lev;
        int32_t v = eg_speed(o[O_R1 + li], op->rs);
        op->p = 0;
        if (to < from) {
            op->seg = SEG_FALL;
            return;
        }
        if (to > from) {
            if (from < LEV_FLOOR) from = op->lev = LEV_FLOOR;
            int32_t dist = (to - from + 0x8000) >> 16;
            if (dist >= 1) {
                op->seg = SEG_RISE;
                op->from = from;
                uint32_t dp = opz_mul_u32_shr16((uint32_t)v * 1843u, k_recip[dist > 127 ? 127 : dist]);
                op->dp = dp > (1u << 24) ? 1 << 24 : (int32_t)dp;     /* 1843 = 7.2 x 256: Q16 x Q16 -> Q24 */
                return;
            }
            op->lev = to;                                     /* the jump already reached it */
            stage++;
            continue;
        }
        op->seg = SEG_FLAT;
        op->dp = v * 19;                                      /* 0.6 x 8 steps / 8: Q16 x 19.2 -> Q24 */
        if (stage == 0 && o[O_L1] == 0) op->dp *= 20;         /* an attack plateau at level 0 is short */
        return;
    }
}

/* One block of an operator's envelope. */
static void eg_block(Op *op, const uint8_t *o)
{
    int stage = op->stage, li = stage == 4 ? 3 : stage;
    switch (op->seg) {
    case SEG_FALL: {
        int32_t to = (int32_t)k_tl[b99(o[O_L1 + li])] << 16;
        op->lev -= eg_speed(o[O_R1 + li], op->rs);
        if (op->lev > to) return;
        op->lev = to;
        break;
    }
    case SEG_RISE: {
        int32_t to = (int32_t)k_tl[b99(o[O_L1 + li])] << 16;
        op->p += op->dp;
        if (op->p < (1 << 24) && to > op->from) {
            uint32_t p16 = (uint32_t)op->p >> 8;
            uint32_t pp = (p16 * (65536u - p16)) >> 16;               /* p (1 - p) */
            uint32_t sh = p16 + ((pp * 43691u) >> 16);                /* p + 2/3 p (1 - p) = p (2.5 - p) 2/3 */
            op->lev = op->from + (int32_t)opz_mul_u32_shr16((uint32_t)(to - op->from), sh);
            return;
        }
        op->lev = to;
        break;
    }
    case SEG_FLAT:
        op->p += op->dp;
        if (op->p < (1 << 24)) return;
        break;
    default:
        return;
    }
    eg_enter(op, o, stage + 1);
}

/* Keyboard level scaling for a note: attenuation in Q4 steps (negative = louder). Per note. */
static int32_t ks_att(const uint8_t *o, int note)
{
    int x = note - (b99(o[O_BP]) + 21);                       /* break point 0 = A-1 (MIDI 21), 39 = C3 (MIDI 60) */
    if (x == 0) return 0;
    int right = x > 0, ax = right ? x : -x;
    int depth = b99(right ? o[O_RD] : o[O_LD]);
    int curve = right ? (o[O_CURVES] >> 2) & 3 : o[O_CURVES] & 3;
    if (!depth) return 0;
    int32_t t;
    if (curve == 0 || curve == 3) {
        t = (ax * depth * 439) >> 10;                         /* 2.65 steps per semitone at depth 99 */
    } else {
        if (ax > 60) ax = 60;                                 /* 3.7 (2^(x / 10) - 1) steps at depth 99 */
        int32_t e = opz_exp2_q16(ax * 6554) - 65536;
        t = (((e >> 8) * depth) * 153) >> 8;
    }
    if (t > 127 * 16) t = 127 * 16;
    return curve < 2 ? t : -t;                                /* - curves fade away from the break point */
}

/* ---- the operator passes ---------------------------------------------------------------------------------------- */

enum { IN_NONE, IN_BUS, IN_FB };
enum { OUT_SET, OUT_ADD_BUS, OUT_ADD_CAR };

/* One operator over the block: y = sin(phase + modulation) x gain. Bus samples are Q31 outputs; << 3 turns a full-scale
 * output (gain 1.0) into 4 turns of phase. Buses add with wrap-around (phase is modular), the carrier sum saturates. */
ALWAYS_INLINE void op_run(uint32_t *php, uint32_t inc, int32_t dinc, int32_t g, int32_t dg, const int32_t *in,
                          int32_t *out, int32_t *fbm, int fbs, const int IN, const int OUT, const int GLIDE)
{
    uint32_t ph = *php;
    int32_t y1 = 0, y2 = 0;
    if (IN == IN_FB) {
        y1 = fbm[0];
        y2 = fbm[1];
    }
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        uint32_t p = ph;
        if (IN == IN_BUS) p += (uint32_t)in[i] << 3;
        if (IN == IN_FB) p += (uint32_t)(((y1 >> 1) + (y2 >> 1)) >> fbs);
        int32_t y = mul_lh(k_sin[p >> 20], g);
        if (IN == IN_FB) {
            y2 = y1;
            y1 = y;
        }
        if (OUT == OUT_SET) out[i] = y;
        else if (OUT == OUT_ADD_BUS) out[i] = (int32_t)((uint32_t)out[i] + (uint32_t)y);
        else out[i] = opz_add_q31(out[i], y);
        ph += inc;
        if (GLIDE) inc += (uint32_t)dinc;
        g += dg;
    }
    *php = ph;
    if (IN == IN_FB) {
        fbm[0] = y1;
        fbm[1] = y2;
    }
}

#define OP_CASE(n, IN, OUT, GL) \
    case n: op_run(php, inc, dinc, g, dg, in, out, fbm, fbs, IN, OUT, GL); break;

/* mode = input (0 none, 3 bus, 6 feedback) + output (0 replace, 1 add to a bus, 2 add to the carrier sum), + 9 when
 * the pitch glides within the block (the increment ramps per sample; otherwise it is constant, one add cheaper). */
static void op_dispatch(int mode, uint32_t *php, uint32_t inc, int32_t dinc, int32_t g, int32_t dg, const int32_t *in,
                        int32_t *out, int32_t *fbm, int fbs)
{
    switch (mode) {
    OP_CASE(0, IN_NONE, OUT_SET, 0)  OP_CASE(1, IN_NONE, OUT_ADD_BUS, 0)  OP_CASE(2, IN_NONE, OUT_ADD_CAR, 0)
    OP_CASE(3, IN_BUS, OUT_SET, 0)   OP_CASE(4, IN_BUS, OUT_ADD_BUS, 0)   OP_CASE(5, IN_BUS, OUT_ADD_CAR, 0)
    OP_CASE(6, IN_FB, OUT_SET, 0)    OP_CASE(7, IN_FB, OUT_ADD_BUS, 0)    OP_CASE(8, IN_FB, OUT_ADD_CAR, 0)
    OP_CASE(9, IN_NONE, OUT_SET, 1)  OP_CASE(10, IN_NONE, OUT_ADD_BUS, 1) OP_CASE(11, IN_NONE, OUT_ADD_CAR, 1)
    OP_CASE(12, IN_BUS, OUT_SET, 1)  OP_CASE(13, IN_BUS, OUT_ADD_BUS, 1)  OP_CASE(14, IN_BUS, OUT_ADD_CAR, 1)
    OP_CASE(15, IN_FB, OUT_SET, 1)   OP_CASE(16, IN_FB, OUT_ADD_BUS, 1)   default: op_run(php, inc, dinc, g, dg, in,
                                                                             out, fbm, fbs, IN_FB, OUT_ADD_CAR, 1);
    }
}

/* Algorithms 4 and 6: operators 6-5(-4) in one loop, the last one's output fed back into operator 6, into C. The
 * increments are the block's mid-point values here (no per-sample glide): these two algorithms are rare. */
static void loop_run(State *s, int n, const uint32_t *inc, const int32_t *g0, const int32_t *dg, int fbs, int32_t *c)
{
    uint32_t p6 = s->op[5].ph, p5 = s->op[4].ph, p4 = s->op[3].ph;
    uint32_t i6 = inc[5], i5 = inc[4], i4 = inc[3];
    int32_t g6 = g0[5], g5 = g0[4], g4 = g0[3], d6 = dg[5], d5 = dg[4], d4 = dg[3];
    int32_t y1 = s->fb[0], y2 = s->fb[1];
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        uint32_t fb = fbs > 7 ? 0 : (uint32_t)(((y1 >> 1) + (y2 >> 1)) >> fbs);
        int32_t y = mul_lh(k_sin[(p6 + fb) >> 20], g6);
        y = mul_lh(k_sin[(p5 + ((uint32_t)y << 3)) >> 20], g5);
        if (n == 3) y = mul_lh(k_sin[(p4 + ((uint32_t)y << 3)) >> 20], g4);
        y2 = y1;
        y1 = y;
        c[i] = y;
        p6 += i6;
        p5 += i5;
        p4 += i4;
        g6 += d6;
        g5 += d5;
        g4 += d4;
    }
    s->op[5].ph = p6;
    s->op[4].ph = p5;
    if (n == 3) s->op[3].ph = p4;
    s->fb[0] = y1;
    s->fb[1] = y2;
}

/* The OP-Z envelope and level, added into out. step0 (decay, release: the envelope's step is 0) is a compile-time
 * constant in each copy. The carrier sum is Q31 with 0.5 = one carrier at full level, which becomes -12 dBFS. */
ALWAYS_INLINE int32_t out_run(fract32 *out, const int32_t *c, int32_t level, int32_t coef, int32_t step, int step0)
{
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        fract32 v = opz_mul_q31_q15(c[i], level >> 17);
        out[2 * i] = opz_add_q31(out[2 * i], v);                             /* interleaved stereo */
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], v);
        level = step0 ? opz_env_tick0(level, coef) : opz_env_tick(level, coef, step);
    }
    return level;
}

/* ---- per block: LFO, pitch envelope ------------------------------------------------------------------------------ */

/* The LFO's value for this block, Q15 bipolar; *fade15 is its fade-in after note-on (the LFO delay), Q15. */
static int32_t lfo_block(State *s, const uint8_t *P, int32_t *fade15)
{
    uint32_t old = s->lfo_ph;
    uint32_t ph = old + k_lfo_inc[b99(P[P_LFO_SPEED])];
    s->lfo_ph = ph;
    int32_t x;
    switch ((P[P_LFO_WAVE] >> 1) & 7) {
    case 0: x = opz_tri15(ph + 0x40000000u); break;             /* triangle, starting at 0 going up */
    case 1: x = 32767 - (int32_t)(ph >> 16); break;             /* saw down */
    case 2: x = (int32_t)(ph >> 16) - 32768; break;             /* saw up */
    case 3: x = ph < 0x80000000u ? 32767 : -32767; break;       /* square */
    case 5:                                                     /* sample and hold */
        if (ph < old) {
            s->seed = s->seed * 1664525u + 1013904223u;
            s->lfo_sh = (int32_t)(s->seed >> 16) - 32768;
        }
        x = s->lfo_sh;
        break;
    default: x = k_sin[ph >> 20]; break;                        /* sine */
    }
    int32_t f = s->lfo_fade + (int32_t)k_lfo_fade[b99(P[P_LFO_DELAY])];
    if (f > (1 << 24)) f = 1 << 24;
    s->lfo_fade = f;
    *fade15 = f >> 9;
    return x;
}

static void peg_block(State *s, const uint8_t *P)
{
    int st = s->peg_stage;
    if (st == 3 || st >= 5) return;
    int li = st == 4 ? 3 : st;
    int32_t to = k_peg_level[b99(P[P_PL1 + li])], v = (int32_t)k_peg_rate[b99(P[P_PR1 + li])];
    if (s->peg < to) {
        s->peg += v;
        if (s->peg < to) return;
    } else if (s->peg > to) {
        s->peg -= v;
        if (s->peg > to) return;
    }
    s->peg = to;
    s->peg_stage = (uint8_t)(st + 1);
}

/* (pitch x mul) >> 16 for a ratio-mode operator; >= Nyquist gives 0xffffffff (muted by the alias guard). */
ALWAYS_INLINE uint32_t ratio_inc(uint32_t pitch, uint32_t mul)
{
    uint32_t hi = opz_mul_u32_shr16(pitch >> 6, mul);
    return hi >= (1u << 25) ? 0xffffffffu : hi << 6;
}

/* ---- entry points ----------------------------------------------------------------------------------------------- */

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

/* Start a note on patch s->patch. */
static void note_start(State *s, int note, uint32_t velocity)
{
    const uint8_t *P = patch_ptr(s->patch);
    int tr = clampi(P[P_TRANSPOSE], 0, 48) - 24;
    int n = clampi(note + tr, 0, 127);
    uint32_t vel = velocity > 127 ? 127 : velocity;
    uint32_t trm = (uint32_t)opz_exp2_q16(tr * 5461);                   /* 2^(tr / 12), Q16 */
    int sync = (P[P_FB_SYNC] >> 3) & 1;
    uint8_t carriers = k_alg_car[P[P_ALG] & 31];
    uint32_t sum = 0;
    int nd3 = (n * 43) >> 7;                                            /* n / 3 */

    for (int k = 0; k < NOPS; k++) {
        Op *op = &s->op[k];
        const uint8_t *o = op_ptr(P, k);
        if (sync) op->ph = 0;
        int fine = b99(o[O_FINE]), coarse = (o[O_MODE_COARSE] >> 1) & 31, det = (o[O_RS_DET] >> 3) & 15;
        if (det > 14) det = 14;
        uint32_t detf = (uint32_t)opz_exp2_q16((det - 7) * 82);         /* 1.5 cents per detune step */
        if (o[O_MODE_COARSE] & 1) {                                     /* fixed: 10^(coarse mod 4 + fine / 100) Hz */
            int32_t hz = opz_exp2_q16(((coarse & 3) * 100 + fine) * 2177);
            op->mul = opz_hz_to_inc((int32_t)opz_mul_u32_shr16((uint32_t)hz, detf));
        } else {                                                        /* coarse (0 = 0.5) x (1 + fine / 100) */
            uint32_t t = (uint32_t)((coarse ? coarse * 2 : 1) * (100 + fine));
            uint32_t r = (t * 20972u) >> 6;                             /* t / 200 in Q16 */
            op->mul = opz_mul_u32_shr16(opz_mul_u32_shr16(r, detf), trm);
        }
        int ol = b99(o[O_OL]);
        int32_t att_ol = (127 - k_tl[ol]) << 4;
        int32_t att = att_ol + (int32_t)((o[O_AMS_KVS] >> 2) & 7) * k_vel[vel] + ks_att(o, n);
        op->att = (int16_t)clampi(att, -4096, 4095);
        int rs = o[O_RS_DET] & 7, x = nd3 - 7;
        op->rs = (uint8_t)(x > 0 ? (rs * x) >> 3 : 0);
        op->lev = (int32_t)k_tl[b99(o[O_L1 + 3])] << 16;                /* envelopes start from L4 */
        op->g = 0;
        eg_enter(op, o, 0);
        if (carriers >> k & 1) sum += (uint32_t)gain_of(att_ol << 9) >> 14;   /* Q16 of output level 99 x 2 */
    }

    /* carriers at full envelope peak at -12 dBFS together: their attenuation moves by 8 log2(sum) steps */
    int32_t norm = 0;
    if (sum) norm = log2_q16(sum) >> 9;                                 /* 128 log2(sum): Q16 octaves -> Q4 steps */
    s->norm = (int16_t)clampi(norm, NORM_MAX_BOOST, 2048);

    s->fb[0] = s->fb[1] = 0;
    s->released = 0;
    s->peg = k_peg_level[b99(P[P_PL1 + 3])];
    s->peg_stage = 0;
    if (P[P_LFO_WAVE] & 1) s->lfo_ph = 0;                              /* LFO key sync */
    s->lfo_fade = 0;
    s->seed = s->seed * 1664525u + 1013904223u + (uint32_t)note;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    if (params) {
        s->p1_on = params[OPZ_P_P1];
        s->patch = (uint8_t)(s->p1_on >> 26);                           /* P1 byte / 8 */
    }
    s->note = (uint8_t)clampi(note, 0, 127);
    s->vel = velocity > 127 ? 127 : velocity;
    s->fresh = 1;
    note_start(s, note, velocity);
}

void onNoteOff(int note, void *state)
{
    (void)note;
    State *s = state;
    const uint8_t *P = patch_ptr(s->patch);
    s->released = 1;
    for (int k = 0; k < NOPS; k++) eg_enter(&s->op[k], op_ptr(P, k), 4);
    s->peg_stage = 4;
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* First block of a note: the OS smooths P1 (a 1/64 one-pole per block), so a P1 that was just changed (a step
     * lock on the note's own step, a preset, a quick turn) is still on its way. Its first move since note-on shows
     * where it is heading: target = value at note-on + 64 x the move. If that is another patch, restart the note on
     * it, before anything has sounded. */
    if (s->fresh) {
        s->fresh = 0;
        int32_t d = (int32_t)(params[OPZ_P_P1] - s->p1_on);
        if (d) {
            int32_t lim = 1 << 25;                                     /* a move this big already means a full sweep */
            d = d > lim ? lim : (d < -lim ? -lim : d);
            int32_t t = (int32_t)(s->p1_on >> 1) + d * 32;              /* the target / 2 */
            t = t < 0 ? 0 : (t > 0x3fffffff ? 0x3fffffff : t);
            if ((uint8_t)(t >> 25) != s->patch) {
                int released = s->released;
                s->patch = (uint8_t)(t >> 25);
                note_start(s, s->note, s->vel);
                if (released) onNoteOff(s->note, s);
            }
        }
    }

    const uint8_t *P = patch_ptr(s->patch);
    int alg = P[P_ALG] & 31, loop = k_alg_loop[alg];
    uint8_t carriers = k_alg_car[alg];
    const uint8_t *code = k_alg[alg];
    int fb = P[P_FB_SYNC] & 7, fbs = 7 - fb;

    /* P2 brightness on the modulators, Q4 steps */
    int32_t b = opz_p16(params[OPZ_P_P2]) - 32768;
    int32_t bright = b < 0 ? (-b * BRIGHT_DOWN) >> 15 : -((b * BRIGHT_UP) >> 15);

    /* LFO and pitch envelope */
    int32_t fade, lfo = lfo_block(s, P, &fade);
    peg_block(s, P);
    int32_t pmd = (int32_t)(((k_pms[(P[P_LFO_WAVE] >> 4) & 7] >> 8) * b99(P[P_LFO_PMD]) * 662) >> 16);  /* Q16 oct */
    int32_t oct = (s->peg >> 8) + ((((pmd * fade) >> 15) * lfo) >> 15);
    int32_t am = (((((lfo + 32768) >> 1) * fade) >> 15) * (b99(P[P_LFO_AMD]) * 331)) >> 15;   /* 0 .. 32767 */
    uint32_t m = (uint32_t)opz_exp2_q16(oct);
    if (m > 0xfffffu) m = 0xfffffu;
    uint32_t p0 = opz_mul_u32_shr16(pitch >> 4, m), p1 = opz_mul_u32_shr16((pitch + (uint32_t)pitchSlope * 64) >> 4, m);
    p0 = p0 >= (1u << 28) ? 0xfffffff0u : p0 << 4;
    p1 = p1 >= (1u << 28) ? 0xfffffff0u : p1 << 4;

    /* per operator: envelope, gain at the end of the block, increments */
    uint32_t inc[NOPS];
    int32_t dinc[NOPS], g0[NOPS], dg[NOPS];
    int audible = 0;
    for (int k = 0; k < NOPS; k++) {
        Op *op = &s->op[k];
        const uint8_t *o = op_ptr(P, k);
        eg_block(op, o);
        uint32_t i0, i1;
        if (o[O_MODE_COARSE] & 1) {
            i0 = i1 = op->mul;
        } else {
            i0 = ratio_inc(p0, op->mul);
            i1 = ratio_inc(p1, op->mul);
        }
        int car = carriers >> k & 1;
        /* attenuation in Q16 octaves: Q4 steps x 512, envelope Q16 steps / 8 (8 steps of 0.75 dB per octave) */
        int32_t att = ((op->att + (car ? s->norm : bright)) * 512) + ((((int32_t)127 << 16) - op->lev) >> 3);
        att += (k_ams[o[O_AMS_KVS] & 3] * am) >> 6;
        uint32_t ihi = i0 > i1 ? i0 : i1;
        if (ihi > INC_16K) att = ihi >= INC_20K ? ATT_SILENT : att + (int32_t)((((ihi - INC_16K) >> 16) * 10584u) >> 7);
        int32_t g = gain_of(att);
        g0[k] = op->g;
        dg[k] = (g - op->g) >> 6;
        op->g = g;
        inc[k] = i0;
        dinc[k] = (int32_t)(i1 - i0) >> 6;
        if (car && (g || op->stage < 3)) audible = 1;
    }

    /* the passes, operator 6 first */
    int32_t *bus[3] = { s_bus[0], s_bus[1], s_bus[2] };
    int valid[3] = { 0, 0, 0 };
    int j = 0;
    if (loop) {
        int any = 0;
        for (int k = 6 - loop; k < NOPS; k++) any |= g0[k] | s->op[k].g;
        if (any) {
            uint32_t mid[NOPS];
            for (int k = 6 - loop; k < NOPS; k++) mid[k] = inc[k] + (uint32_t)(dinc[k] * 32);
            loop_run(s, loop, mid, g0, dg, fb ? fbs : 8, bus[2]);
            valid[2] = 1;
        } else {
            for (int k = 6 - loop; k < NOPS; k++) s->op[k].ph += inc[k] * 64u;
            s->fb[0] = s->fb[1] = 0;
        }
        j = loop;
    }
    for (; j < NOPS; j++) {
        int k = 5 - j, c = code[j];
        int in = c & 3, outb = (c >> 2) & 3, add = (c >> 4) & 1;
        Op *op = &s->op[k];
        if (!g0[k] && !op->g) {                                      /* silent for the whole block */
            op->ph += inc[k] * 64u + (uint32_t)dinc[k] * 2016u;
            if (!add) valid[outb] = 0;
            if (in == 3) s->fb[0] = s->fb[1] = 0;
            continue;
        }
        int mode;
        if (in == 3 && fb) mode = 6;
        else if ((in == 1 || in == 2) && valid[in - 1]) mode = 3;
        else mode = 0;
        if (add && valid[outb]) mode += outb == 2 ? 2 : 1;
        if (dinc[k]) mode += 9;
        op_dispatch(mode, &op->ph, inc[k], dinc[k], g0[k], dg[k], in == 2 ? bus[1] : bus[0], bus[outb], s->fb, fbs);
        valid[outb] = 1;
    }

    int32_t *c = bus[2];
    if (!valid[2])
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) c[i] = 0;
    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    OPZ_ADSR_LEVEL(env) = step ? out_run(out, c, level, coef, step, 0) : out_run(out, c, level, coef, 0, 1);

    /* every carrier silent and done (sustain or release): free the voice now */
    if (!audible && OPZ_ADSR_STAGE(env) != 0) OPZ_ADSR_STAGE(env) = 0;
}

OPZ_EXPORTS_INSTRUMENT();
