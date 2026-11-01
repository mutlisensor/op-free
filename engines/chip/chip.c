/*
 * chip — 8-bit console voice: pulse waves with the classic duty cycles, the 4-bit stepped triangle, a 4-bit saw,
 * LFSR noise (long hiss and short "metallic" loops) and a fast built-in arpeggio at console frame rates.
 *
 *   P1        wave        8 steps of 32: 0 pulse 12.5 %, 32 pulse 25 %, 64 pulse 50 %, 96 duty cycling
 *                         (12.5 -> 25 -> 50 -> 25 %, one step every 2 frames), 128 triangle (4-bit, 32 steps),
 *                         160 saw (4-bit, 16 steps), 192 noise (15-bit LFSR, clocked at 16 x the note), 224 metallic
 *                         noise (a 93-step LFSR loop, pitched at the note)
 *   P2        arpeggio    0 plain; 1-31 delayed vibrato, depth P2/31 semitone; then 6 arpeggios of 32 steps each:
 *                         32 octave (0 12), 64 major (0 4 7), 96 minor (0 3 7), 128 fifth (0 7 12), 160 sus4 (0 5 7),
 *                         192 seventh (0 4 7 10), each at 4 speeds of 8 steps: 15, 20, 30, 60 Hz (a new note every
 *                         4, 3, 2, 1 frames of 60 Hz); 224-255 pitch sweep: 224 / 228 / 232 / 236 down at 2 / 5 / 12 /
 *                         30 octaves per second, 240 / 244 / 248 / 252 up at the same rates (down to -4, up to +2
 *                         octaves)
 *   velocity              the console's 4-bit volume: 16 linear steps
 *   attack..release       host envelope (advanced per block in closed form)
 *
 * How it sounds clean: every wave except the long noise is a table of N steps (8, 16, 32 or 93), and every output
 * sample is the wave's exact average over that sample's interval (the difference of its running integral, a
 * piecewise-linear table, divided by the phase step: first-order antialiasing). That is what a chip running much
 * faster than 44.1 kHz into a one-sample box filter would give: the steps stay sharp and the 4-bit staircase stays
 * audible, but alias products that fold down into the midrange are 20-40 dB weaker than in a naive emulation, and
 * the top octave rolls off gently (-0.9 dB at 10 kHz, -3.9 dB at 22 kHz). The long noise steps its LFSR at up to
 * one step per sample and averages the same way across the step. Tables are zero-sum (no DC): pulses +14/-2,
 * +12/-4, +8/-8 (units of 1/16), the metallic loop +15/-16 on a 93-step cycle with 48 ones.
 *
 * Arpeggio, duty cycling, vibrato and sweep run at block rate (689 Hz); the 60 Hz frame clock lands on the
 * nearest block (frames of 11 or 12 blocks). Pitch changes keep the phase, as on the real chips.
 *
 * Original code. Waves and behaviour follow the NES 2A03 and Game Boy APUs (published hardware documentation); the
 * idea of a chiptune voice with a built-in arpeggiator comes from Plaits' chiptune model and the OP-1's pulse (no
 * code taken). MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"

#define N OPZ_BLOCK_FRAMES
#define ALWAYS_INLINE static inline __attribute__((always_inline))

enum { W_P12, W_P25, W_P50, W_TRI, W_SAW, W_METAL, NTAB, W_NOISE = NTAB, W_DUTY };

typedef struct State {
    uint32_t ph;                    /* oscillator phase, Q32 turns */
    uint32_t nq;                    /* long noise: LFSR clock phase, Q32 of a step */
    uint32_t frame;                 /* frame clock, Q32 of a 60 Hz frame */
    uint32_t vib;                   /* vibrato phase, Q32 turns */
    int32_t sweep;                  /* pitch sweep, Q16 octaves */
    int32_t vol;                    /* velocity as the 4-bit volume, Q31 */
    uint16_t lfsr;                  /* 15-bit shift register, never 0 */
    uint16_t frames;                /* frames since the note-on (saturating) */
    uint8_t arp, tick;              /* arpeggio step, frames into it */
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in the 280 bytes the OS leaves");

static const ModuleCallbackInterface *s_cb;
static uint32_t s_voices = 0x1234;            /* noise seeds, in init order (deterministic) */

/* The running integral of each table: s_c[off + k] = sum of the first k step values (units of 1/16 of full scale),
 * N + 1 entries per table, the last one 0 (every table is zero-sum). Built in initModule. */
#define OFF_P12   0
#define OFF_P25   9
#define OFF_P50   18
#define OFF_TRI   27
#define OFF_SAW   60
#define OFF_METAL 77
#define TAB_SIZE  171
static int16_t s_c[TAB_SIZE];

static const uint8_t k_off[NTAB] = { OFF_P12, OFF_P25, OFF_P50, OFF_TRI, OFF_SAW, OFF_METAL };
static const uint8_t k_len[NTAB] = { 8, 8, 8, 32, 16, 93 };
/* Output level per unit (Q31): peaks at -12 dBFS (pulse 12.5 / 25 %, triangle, saw) or -15 dBFS (pulse 50 % and
 * the noises, whose RMS equals their peak). */
static const int32_t k_level[NTAB] = { 38347922, 44739243, 47735324, 35791394, 35791394, 23867662 };
#define NOISE_GAIN 1527530382       /* long noise: +-0.25 internally -> -15 dBFS, Q31 */
#define NOISE_V    0x20000000

#define INC_MIN    0x10000u         /* 0.67 Hz */
#define INC_MAX    0x3fffffffu      /* 11 kHz; keeps the triangle under 8 steps per sample */
#define INC_METAL  369000000u       /* 3.79 kHz: the 93-step loop under 8 steps per sample */
#define INC_C5     50960235u       /* 523 Hz; the output stage's 2-tap average is full from C7 (2093 Hz) */
#define FRAME_INC  373983547u       /* 60 Hz frames per block, Q32 */
#define VIB_INC    34281825u        /* 5.5 Hz per block */
#define VIB_DELAY  18               /* frames (0.3 s) */
#define SEMI       5461             /* 1/12 octave, Q16 (21845 / 4) */

/* arpeggios: semitones, 4 per pattern, and their lengths */
static const uint8_t k_arp[6][4] = { { 0, 12 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 7, 12 }, { 0, 5, 7 }, { 0, 4, 7, 10 } };
static const uint8_t k_arp_len[6] = { 2, 3, 3, 3, 3, 4 };
static const uint8_t k_wave[8] = { W_P12, W_P25, W_P50, W_DUTY, W_TRI, W_SAW, W_NOISE, W_METAL };
static const uint8_t k_duty[4] = { W_P12, W_P25, W_P50, W_P25 };
static const int16_t k_sweep[4] = { 190, 476, 1141, 2853 };   /* 2, 5, 12, 30 octaves per second, Q16 per block */

static void build(int off, int n, int (*value)(int k))
{
    int c = 0;
    s_c[off] = 0;
    for (int k = 0; k < n; k++) {
        c += value(k);
        s_c[off + k + 1] = (int16_t)c;
    }
}

static int v_p12(int k) { return k == 1 ? 14 : -2; }
static int v_p25(int k) { return k == 1 || k == 2 ? 12 : -4; }
static int v_p50(int k) { return k >= 1 && k <= 4 ? 8 : -8; }
static int v_tri(int k) { return 2 * (k < 16 ? 15 - k : k - 16) - 15; }    /* 15 14 .. 0 0 1 .. 15 */
static int v_saw(int k) { return 2 * (15 - k) - 15; }
static uint32_t s_metal;                                         /* the loop's register, while building */
static int v_metal(int k)
{
    (void)k;
    int b = s_metal & 1;
    s_metal = (s_metal >> 1) | (((s_metal ^ (s_metal >> 6)) & 1) << 14);   /* short mode: taps 0 and 6 */
    return b ? 15 : -16;
}

/* ---- the envelope ------------------------------------------------------------------------------------------- */

/* The host envelope 64 samples on (sdk/docs/engine-abi.md §3: per sample r = c level, level = r + (1 - r^3) s),
 * in closed form instead of 64 steps:
 *   s = 0 (decay, release):      level c^64, c^64 by six squarings
 *   c = 1 (attack, sustain):     (1 - r^3) s hardly moves within a block: level + 64 (1 - level^3) s
 *   otherwise (a fade with a step, a few blocks per voice steal): the exact 64 steps
 * The audio follows it as a straight line across the block (as in vowel). */
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

/* ---- the oscillators ---------------------------------------------------------------------------------------- */

/* The integral of a table at phase ph, Q24 units x steps, modulo 2^32 (only differences are used). */
ALWAYS_INLINE uint32_t integral(const int16_t *c, uint32_t n, uint32_t ph)
{
    uint32_t pos = (ph >> 8) * n;                              /* Q24 steps */
    uint32_t k = pos >> 24;
    int32_t c0 = c[k];
    return ((uint32_t)c0 << 24) + (uint32_t)((c[k + 1] - c0) * (int32_t)(pos & 0xffffff));
}

/* A table wave: the average over each sample (integral difference x 2^sh), then the output stage's 2-tap average
 * with weight b (Q15, 32767 = a 2-sample box), x g, added into out. d and the last d are below 2^31 in magnitude, and
 * their mix is too. */
__attribute__((noinline)) static uint32_t pass_table(fract32 *out, const int16_t *c, uint32_t n, uint32_t ph,
                                                     uint32_t inc, int sh, int32_t b, int32_t g, int32_t dg)
{
    uint32_t ip = integral(c, n, ph);
    int32_t dp = opz_shl_q31((int32_t)(ip - integral(c, n, ph - inc)), sh);
    for (int i = 0; i < N; i++) {
        ph += inc;
        uint32_t iv = integral(c, n, ph);
        int32_t d = opz_shl_q31((int32_t)(iv - ip), sh);
        ip = iv;
        int32_t dm = d - opz_mul_q31_q15((d >> 1) - (dp >> 1), b);
        dp = d;
        fract32 y = opz_mul_q31_q15(dm, g >> 16);
        g += dg;
        out[2 * i] = opz_add_q31(out[2 * i], y);                 /* interleaved stereo */
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], y);
    }
    return ph;
}

/* The long noise: a 15-bit LFSR (taps 0 and 1, 32,767 steps) clocked by nq at qinc per sample (at most one step),
 * averaged across the step: v_old (1 - t) + v_new t, t = the part of the sample after the step. */
__attribute__((noinline)) static void pass_noise(fract32 *out, State *s, uint32_t qinc, int32_t g, int32_t dg)
{
    int n2 = opz_norm32((int32_t)(qinc >> 1));                 /* (qinc / 2) << n2 in [2^30, 2^31) */
    int32_t h2 = opz_recip_q31((int32_t)((qinc >> 1) << n2));
    uint32_t nq = s->nq, lf = s->lfsr ? s->lfsr : 1;
    int32_t v = (lf & 1) ? -NOISE_V : NOISE_V;
    for (int i = 0; i < N; i++) {
        nq += qinc;
        int32_t y = v;
        if (nq < qinc) {                                           /* a step in this sample */
            lf = (lf >> 1) | (((lf ^ (lf >> 1)) & 1) << 14);
            int32_t vn = (lf & 1) ? -NOISE_V : NOISE_V;
            int32_t t = opz_shl_q31(opz_mul_q31((int32_t)((nq >> 1) << n2), h2), 1);   /* nq / qinc, Q31 */
            y = vn + opz_mul_q31(v - vn, OPZ_Q31_ONE - t);
            v = vn;
        }
        y = opz_mul_q31_q15(y, g >> 16);
        g += dg;
        out[2 * i] = opz_add_q31(out[2 * i], y);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], y);
    }
    s->nq = nq;
    s->lfsr = (uint16_t)lf;
}

/* ---- entry points ------------------------------------------------------------------------------------------- */

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    build(OFF_P12, 8, v_p12);
    build(OFF_P25, 8, v_p25);
    build(OFF_P50, 8, v_p50);
    build(OFF_TRI, 32, v_tri);
    build(OFF_SAW, 16, v_saw);
    s_metal = 7;                                               /* a 93-step cycle with 48 ones: +15 / -16 sum to 0 */
    build(OFF_METAL, 93, v_metal);
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    s_voices = (s_voices + 0x2d4bu) & 0x7fffu;                 /* each voice's noise register starts elsewhere */
    ((State *)state)->lfsr = (uint16_t)(s_voices ? s_voices : 1);
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note; (void)params;                                  /* params may be NULL (engine-abi.md §3) */
    State *s = state;
    uint32_t v = velocity > 127 ? 127 : velocity;
    uint32_t vol = (v * 15 + 64) >> 7;                         /* 4-bit volume, 1 .. 15 */
    if (vol < 1) vol = 1;
    s->vol = (int32_t)(vol * 143165576u);                      /* vol / 15, Q31 */
    s->ph = 0x40000000u;                                       /* the triangle near 0: a soft start */
    s->frame = 0;
    s->frames = 0;
    s->arp = s->tick = 0;
    s->vib = 0;
    s->sweep = 0;
    if (!s->lfsr) s->lfsr = 1;                                 /* the noise register runs on across notes */
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                   /* the host releases the envelope */
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);
    int b1 = (int)(params[OPZ_P_P1] >> 23) & 255, b2 = (int)(params[OPZ_P_P2] >> 23) & 255;

    /* the frame clock */
    uint32_t fr = s->frame + FRAME_INC;
    int tick = fr < s->frame;
    s->frame = fr;
    if (tick && s->frames < 0xffff) s->frames++;

    /* P2: vibrato, arpeggio or sweep, as an offset in Q16 octaves */
    int zone = b2 >> 5;
    int32_t oct = 0;
    if (zone == 0) {
        if (b2 && s->frames >= VIB_DELAY) {
            s->vib += VIB_INC;
            oct = (opz_tri15(s->vib) * (b2 * 176)) >> 15;          /* +-P2/31 semitone */
        }
    } else if (zone == 7) {
        int j = (b2 >> 2) & 7;
        int32_t sw = s->sweep + (j < 4 ? -k_sweep[j] : k_sweep[j - 4]);
        if (sw < -(4 << 16)) sw = -(4 << 16);
        if (sw > (2 << 16)) sw = 2 << 16;
        s->sweep = sw;
        oct = sw;
    } else {
        int pat = zone - 1, len = k_arp_len[pat];
        int fps = 4 - ((b2 >> 3) & 3);                             /* frames per note: 4, 3, 2, 1 */
        if (tick && ++s->tick >= fps) {
            s->tick = 0;
            s->arp++;
        }
        if (s->tick >= fps) s->tick = 0;
        if (s->arp >= len) s->arp = 0;
        oct = k_arp[pat][s->arp] * SEMI;
    }

    /* the block's phase increment: the host pitch at mid-block x 2^oct */
    uint32_t p = pitch > INC_MAX ? INC_MAX : pitch;
    int32_t sl = pitchSlope;
    if (sl > 0x1ffffff) sl = 0x1ffffff;
    if (sl < -0x1ffffff) sl = -0x1ffffff;
    int32_t pm = (int32_t)p + sl * (N / 2);
    if (pm < (int32_t)INC_MIN) pm = (int32_t)INC_MIN;
    if (pm > (int32_t)INC_MAX) pm = (int32_t)INC_MAX;
    uint32_t mul = (uint32_t)opz_exp2_q16(oct);                    /* Q16, below 2^18 */
    if (mul > 0x3ffff) mul = 0x3ffff;
    uint32_t inc = opz_mul_u32_shr16((uint32_t)pm, mul);
    if (inc < INC_MIN) inc = INC_MIN;
    if (inc > INC_MAX) inc = INC_MAX;

    /* P1: the wave */
    int w = k_wave[b1 >> 5];
    if (w == W_DUTY) w = k_duty[(s->frames >> 1) & 3];

    int32_t l0 = OPZ_ADSR_LEVEL(env);
    int32_t l1 = env_block(l0, OPZ_ADSR_COEF(env), OPZ_ADSR_STEP(env));
    OPZ_ADSR_LEVEL(env) = l1;

    if (w == W_NOISE) {
        uint32_t qinc = inc >= 0x10000000u ? 0xffffffffu : inc << 4;   /* 16 steps per note period */
        int32_t gv = opz_mul_q31(NOISE_GAIN, s->vol);
        int32_t g0 = opz_mul_q31(gv, l0), g1 = opz_mul_q31(gv, l1);
        pass_noise(out, s, qinc, g0, (g1 >> 6) - (g0 >> 6));
        return;
    }
    if (w == W_METAL && inc > INC_METAL) inc = INC_METAL;
    uint32_t n = k_len[w];
    /* the phase step in Q24 table steps, normalised: dx << nn = m in [2^30, 2^31); dx < 2^27 keeps nn >= 4 */
    uint32_t dx = (inc >> 8) * n + (((inc & 255) * n) >> 8);
    int nn = opz_norm32((int32_t)dx);
    int32_t h = opz_recip_q31((int32_t)(dx << nn));               /* 2^61 / m */
    /* out = avg x level = d 2^(nn - 4) x g / 2^31 with g = level x h x 2^5 (below 2^31: level <= 47.7e6,
     * h <= 2^31) */
    int32_t gv = opz_mul_q31(opz_mul_q31(k_level[w], s->vol), h) << 5;
    int32_t g0 = opz_mul_q31(gv, l0), g1 = opz_mul_q31(gv, l1);
    /* the output stage: no extra smoothing up to C5, rising linearly in frequency to a 2-sample box at C7 */
    int32_t b = inc <= INC_C5 ? 0 : (int32_t)((((inc - INC_C5) >> 12) * 28767u) >> 15);
    if (b > 32767) b = 32767;
    s->ph = pass_table(out, s_c + k_off[w], n, s->ph, inc, nn - 4, b, g0, (g1 >> 6) - (g0 >> 6));
}

OPZ_EXPORTS_INSTRUMENT();
