/*
 * hats — metallic hi-hats and cymbals for the kick, snare, perc and sample tracks: six detuned square waves (the
 * TR-808 recipe) plus noise, through one band-pass/high-pass filter and a two-stage decay. Each of the 24 keys plays
 * its own hat or cymbal (table k_models below).
 *
 *   P1        tone        dark/trashy (0) .. bright/glassy (255): the squares' pitch (x0.59 .. x1.68) and spread
 *                         (ratios ^0.5 .. ^1.5), the band's centre (-1 .. +1 octave) and resonance (-0.2 .. +0.2)
 *   P2        decay       x1/4 .. x4 on the first decay stage (and a swell's rise), x1/2 .. x2 on the second
 *   velocity              level, and brightness: the band moves down by up to 3/4 octave on soft hits
 *   attack .. release     the OP-Z drum envelope over the hit, as on TE's sample kits: start, attack, hold, decay
 *                         (opz_drumenv in opz_adsr.h); the presets' 0 / 0 / 255 / 0 leave every hit as designed
 *   note style gate       a key release fades the hit (~20 ms); the other styles play one-shots
 *   octave                each octave press moves the squares and the band by 1/3 octave (+-2 presses)
 *
 * Choke: a closed, pedal, tick or half-open hit cuts the open and half-open hats ringing on the same track (in
 * ~28 ms). Knobs are read when a hit starts. Each voice ends itself (envelope stage 0) at -60 dB.
 *
 * Squares without aliasing whistles: each square is a list of edge times (a half period apart, Q16 samples), not
 * a phase. Every edge drops its step into a per-block difference buffer through a quadratic B-spline (three
 * samples, weights (1-f)^2/2, rest, f^2/2 of the fractional edge time f) and one running sum over the block turns
 * the steps into the six squares' sum. That is the ideal square filtered by a triangle two samples wide: images
 * near fs are suppressed by sinc^2 (aliases below 4 kHz are 30-90 dB below a naive square's, tests/alias.c), at a
 * cost of a few cycles per edge instead of per sample. In "ring" models the squares are multiplied in pairs (each
 * edge flips its pair's product), the TR-808 cymbal's ring-modulated clang.
 *
 * Written from scratch on sdk/include/opz_dsp.h. The structure (six squares at the TR-808's 205-800 Hz, band-pass,
 * noise blend, a decay that changes rate part-way) follows the TR-808 circuit as also described by Plaits' hi-hat
 * model (Emilie Gillet, MIT licence); no code is copied. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

enum { F_CLOSE = 1, F_OPEN = 2, F_SWELL = 4 };      /* F_CLOSE cuts F_OPEN on the same track */
enum { S_RING = 0x80 };                               /* ratio set | S_RING: squares multiplied in pairs */
enum { ST_RISE = 0, ST_ONE = 1, ST_TWO = 2, ST_CUT = 3 };

typedef struct Model {
    uint16_t f0;              /* lowest square, log2(Hz) x 4096 */
    uint16_t fc;              /* band centre, log2(Hz) x 4096 */
    uint8_t tr;               /* rise, 4 ms units (0: none) */
    uint16_t t1, t2;          /* decay stages: ms to -60 dB at that stage's rate */
    uint8_t knee;             /* dB below the peak where stage 2 takes over */
    uint8_t set;              /* ratio set | S_RING */
    uint8_t res;              /* band resonance, /256 */
    uint8_t gb, gh;           /* band-pass and high-pass output gains, /64 */
    uint8_t gn;               /* noise into the filter, /256 of half scale */
    uint8_t flags;
} Model;

static const Model k_models[24] = {
    /*  f0     fc    tr    t1    t2  knee  set         res   gb   gh   gn  flags */
    { 31455, 52726,   0,  200,   80, 24, 0         , 140,  93,  67,  40, F_CLOSE           },   /* closed hat    205 Hz, band  7500 Hz */
    { 32387, 54123,   0,   90,   40, 24, 0         , 110,  46,  64,  90, F_CLOSE           },   /* tight hat     240 Hz, band  9500 Hz */
    { 30687, 51408,   1,  220,   80, 20, 0         , 150, 126,  42,  30, F_CLOSE           },   /* pedal hat     180 Hz, band  6000 Hz */
    { 31455, 52726,   0,  600,  200, 24, 0         , 140,  72,  51,  40, F_CLOSE | F_OPEN  },   /* half-open     205 Hz, band  7500 Hz */
    { 31455, 52726,   0, 1500,  400, 30, 0         , 140,  65,  47,  40, F_OPEN            },   /* open hat      205 Hz, band  7500 Hz */
    { 31455, 52319,   0, 3000,  800, 30, 0         , 130,  62,  36,  50, F_OPEN            },   /* open long     205 Hz, band  7000 Hz */
    { 29202, 50562,   0,  220,   80, 24, 0         , 160,  91,  24,  60, F_CLOSE           },   /* dark closed   140 Hz, band  5200 Hz */
    { 29202, 50562,   0, 1600,  400, 30, 0         , 160,  65,  18,  60, F_OPEN            },   /* dark open     140 Hz, band  5200 Hz */
    { 33705, 54990,   0,  180,   70, 24, 0         , 200, 104,  52,  20, F_CLOSE           },   /* glass closed  300 Hz, band 11000 Hz */
    { 33705, 54990,   0, 1400,  400, 30, 0         , 200,  71,  36,  20, F_OPEN            },   /* glass open    300 Hz, band 11000 Hz */
    { 32860, 51881,   0,  200,   80, 24, 0 | S_RING, 170, 105,  38,  30, F_CLOSE           },   /* ring closed   260 Hz, band  6500 Hz */
    { 32860, 51881,   0, 1500,  400, 30, 0 | S_RING, 170,  74,  27,  30, F_OPEN            },   /* ring open     260 Hz, band  6500 Hz */
    { 34268, 54426,   0,   40,   40, 60, 0         , 110, 118, 178,  60, F_CLOSE           },   /* tick          330 Hz, band 10000 Hz */
    { 34268, 50330,   0,  300, 4500, 10, 1         , 130,  63,  57,  25, 0                 },   /* ride          330 Hz, band  5000 Hz */
    { 35694, 47693,   0, 2500, 2500, 60, 2         , 200,  37,   8,  10, 0                 },   /* ride bell     420 Hz, band  3200 Hz */
    { 32387, 48709,   0,  500, 3500,  8, 1 | S_RING, 110,  57,  44,  40, 0                 },   /* dark ride     240 Hz, band  3800 Hz */
    { 33705, 51881,   0,  150, 3000,  9, 1         ,  40,  28,  53,  90, 0                 },   /* crash         300 Hz, band  6500 Hz */
    { 32860, 50330,   0,  250, 4500,  8, 1 | S_RING,  40,  27,  43,  90, 0                 },   /* big crash     260 Hz, band  5000 Hz */
    { 35102, 53466,   0,   80, 1000, 10, 1         ,  50,  32,  76,  80, 0                 },   /* splash        380 Hz, band  8500 Hz */
    { 32387, 47312,   0,  120, 2200,  8, 1 | S_RING,  90,  73,  54,  70, 0                 },   /* china         240 Hz, band  3000 Hz */
    { 32860, 49300,   0,   70,  700, 10, 0 | S_RING,  90,  79,  68,  70, 0                 },   /* china short   260 Hz, band  4200 Hz */
    { 34268, 53804,   0,  500, 4000,  6, 1         ,  60,   7,  23, 140, 0                 },   /* sizzle        330 Hz, band  9000 Hz */
    { 33705, 51881, 225,   60,   60, 60, 1         ,  40,  33,  63,  90, F_SWELL           },   /* swell         300 Hz, band  6500 Hz */
    { 35694, 45993,   0, 1200, 1200, 60, 2 | S_RING, 215,  28,   3,   0, 0                 },   /* bell          420 Hz, band  2400 Hz */
};

/* Square frequency ratios, log2 x 4096: TR-808 hats (205.3, 304.4, 369.6, 522.7, 540, 800 Hz), a wider cymbal
 * set, a near-harmonic bell set. Ring models multiply squares 0-1, 2-3, 4-5. */
static const uint16_t k_ratio[3][6] = {
    { 0, 2327, 3474, 5522, 5715, 8037 },     /* 1, 1.483, 1.800, 2.546, 2.630, 3.897 */
    { 0,  928, 2814, 4356, 6147, 7179 },     /* 1, 1.17, 1.61, 2.09, 2.83, 3.37 */
    { 0, 4096, 5186, 5999, 6492, 8295 },     /* 1, 2, 2.405, 2.76, 3, 4.07 */
};

#define END        (OPZ_BLOCK_FRAMES << 16)  /* a block, in Q16 samples */
#define AMP        0x08000000                /* one square, +-1/16 of full scale (step 2 AMP = 2^28) */
#define THRESHOLD  0x00200000                /* -60 dB: the hit is over */
#define LOG2_HALF_FS 945586                  /* log2(22050) x 65536: a half period is 22050 / f samples */
#define CUT_C64    0x58fa79eb                /* per-block decay of a choked hat: tau 4 ms (-60 dB in ~28 ms) */
#define FADE_C64   0x08000000                /* per-block decay of a stolen voice: 1/16 (-60 dB in 2.5 blocks) */
#define MIN_HP     (2 << 16)                 /* half period limits: 11 kHz .. far below audio */
#define MAX_HP     (1 << 30)
#define MIN_RSTEP  0x00080000                /* rise: at most 4,096 blocks (5.9 s; the longest is 3.6 s) */
#define MAX_AGE    11025u                    /* blocks: a hit ends after 16 s whatever its state (longest 10 s) */

typedef struct State {
    int32_t t[6], hp[6];                      /* next edge (Q16 samples from the block start), half period */
    int32_t carry0, carry1, y;                /* difference buffer spill into the next block, running sum */
    uint32_t seed;
    OpzSvf f;
    OpzSvfCoef fc;
    int32_t gb, gh, gn;                       /* Q15 gains, velocity included */
    int32_t hpa, hpl;                         /* one-pole high-pass an octave below the band: Q15, state */
    uint32_t age;                             /* blocks rendered since the hit */
    int32_t env, c64, c64b, knee, rise, rstep;
    OpzMix mix;                               /* the track's level and pan (opz_mix.h) */
    OpzDrumEnv denv;                          /* the envelope page over the hit */
    uint8_t model, track, choke, stage, signs, ring, active, fading;
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes (the real limit, below the ABI's 308)");

static const ModuleCallbackInterface *s_cb;
static uint8_t s_choke[16];                   /* per track: bumped by closing hats, watched by open ones */
static uint32_t s_d[OPZ_BLOCK_FRAMES + 2];    /* difference buffer: render runs one voice at a time */

/* The last two filter settings and their coefficients (two 64-bit divisions each): a closed/open pattern at
 * steady knobs hits the same two (four until v3; two leave room for the envelope page in the 5 KB slot). lfc is at least 8 octaves, so the zeroed .bss reads as empty. */
static struct { int32_t lfc, res; OpzSvfCoef c; } s_cache[2];
static uint8_t s_cache_next;

uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;
}

void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    State *s = state;
    s->track = (uint8_t)(track & 15);
    s->seed = 0x2545f491u + track * 7919u;
}

/* Per-sample one-pole coefficient for a decay of t ms to -60 dB, scaled by m (Q16), raised to the 64th power: the
 * decay over one block. */
static int32_t block_decay(uint32_t t_ms, uint32_t m)
{
    uint32_t n = opz_mul_u32_shr16(t_ms * 1634u, m) >> 8;              /* tau = t / 6.91 ms = t x 6.384 samples */
    int32_t c = opz_decay_coef((int32_t)n);
    for (int i = 0; i < 6; i++) c = opz_mul_q31(c, c);
    return c;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("Os")))   /* once per hit: size matters more than speed */
#endif
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    int idx = note < -128 ? -128 : note > 255 ? 255 : note;            /* key = (note - 53) mod 24 */
    idx -= 53;
    int oct = 0;                                                         /* octave = floor((note - 53) / 24) */
    while (idx < 0) { idx += 24; oct--; }
    while (idx >= 24) { idx -= 24; oct++; }
    if (oct < -2) oct = -2;
    if (oct > 2) oct = 2;
    const Model *m = &k_models[idx];
    s->model = (uint8_t)idx;

    int32_t p1 = 32768, p2 = 32768;                                      /* no parameters: the knobs at 128 */
    if (params) {
        p1 = opz_p16(params[OPZ_P_P1]);
        p2 = opz_p16(params[OPZ_P_P2]);
        if (p1 > 65535) p1 = 65535;
        if (p2 > 65535) p2 = 65535;
    }
    p1 -= 32768;                                                         /* -0.5 .. 0.5, Q16 */
    p2 -= 32768;
    int v = velocity > 127 ? 127 : velocity;
    int32_t amp = v * 0x01020408;                                        /* velocity, Q31 */

    /* The squares: f = f0 x 2^(1.5 p1 + oct / 3) x ratio^(1 + 2 p1), as half periods. */
    int32_t shift = ((p1 * 3) >> 1) + oct * 21845;
    int32_t lf0 = (m->f0 << 4) + shift;
    int32_t spread = 4096 + (p1 >> 4);                                   /* 0.5 .. 1.5, Q12 */
    const uint16_t *ratio = k_ratio[m->set & 3];
    uint32_t seed = s->seed;
    uint32_t signs = 0;
    int32_t y = 0;
    for (int i = 0; i < 6; i++) {
        int32_t hp = opz_exp2_q16(LOG2_HALF_FS - lf0 - ((ratio[i] * spread) >> 8));
        if (hp < (2 << 16)) hp = 2 << 16;                                /* at most 11 kHz */
        s->hp[i] = hp;
        seed = seed * 1664525u + 1013904223u;
        s->t[i] = (int32_t)opz_mul_u32_shr16((uint32_t)hp, seed >> 16);  /* free-running: a random start */
        signs |= (seed >> 15 & 1u) << i;
    }
    s->ring = (m->set & S_RING) ? 1 : 0;
    for (int i = 0; i < 6; i++) {                                        /* the squares' sum at the start */
        int neg = (signs >> i) & 1;
        if (s->ring) {
            if (i & 1) continue;
            neg ^= (signs >> (i + 1)) & 1;
        }
        y += neg ? -AMP : AMP;
    }
    s->signs = (uint8_t)signs;
    s->y = y;
    s->carry0 = s->carry1 = 0;
    s->seed = seed;

    /* The band: centre fc x 2^(2 p1 + oct / 3 - 3/4 (127 - v) / 127), resonance res + 0.4 p1. */
    int32_t lfc = (m->fc << 4) + p1 * 2 + oct * 21845 - (127 - v) * 387;
    if (lfc > 13 * 65536 + 62000) lfc = 13 * 65536 + 62000;              /* 15.6 kHz */
    if (lfc < 8 * 65536) lfc = 8 * 65536;                                /* 256 Hz */
    int32_t res = (m->res << 22) + p1 * 6553;                            /* Q30: 0.4 p1 is p1 x 6553 */
    if (res < 0) res = 0;
    if (res > 0x3b333333) res = 0x3b333333;                              /* 0.925 */
    res <<= 1;                                                           /* Q31 */
    int c = 0;                                                           /* the coefficient cache (below) */
    while (c < 2 && (s_cache[c].lfc != lfc || s_cache[c].res != res)) c++;
    if (c == 2) {
        c = s_cache_next++ & 1;
        s_cache[c].lfc = lfc;
        s_cache[c].res = res;
        s_cache[c].c = opz_svf_coef(opz_exp2_q16(lfc), OPZ_Q31_ONE - res);
    }
    s->fc = s_cache[c].c;
    s->f.ic1 = s->f.ic2 = 0;
    s->gb = opz_mul_q31(amp, m->gb << 23) >> 16;                         /* Q15 */
    s->gh = opz_mul_q31(amp, m->gh << 23) >> 16;
    s->gn = m->gn << 6;
    /* The one-pole high-pass after the mix, at fc / 2: a = 1 - e^-w, w = 2 pi (fc / 2) / fs. Steepens the band's
     * low skirt (the squares' fundamentals) to 12 dB/octave. */
    uint32_t w = (uint32_t)opz_exp2_q16(lfc - 65536 - 837353);          /* log2(2 pi / 44100) x 65536 */
    s->hpa = (65536 - opz_exp2_q16(-(int32_t)opz_mul_u32_shr16(w, 94548))) >> 1;   /* e^-w = 2^(-w / ln 2) */
    s->hpl = 0;

    /* Decays: stage 1 (and the rise) x 2^(4 p2), stage 2 x 2^(2 p2). */
    uint32_t m1 = (uint32_t)opz_exp2_q16(p2 * 4), m2 = (uint32_t)opz_exp2_q16(p2 * 2);
    s->c64 = block_decay(m->t1, m1);
    s->c64b = block_decay(m->t2, m2);
    s->knee = opz_exp2_q16(-m->knee * 10885) << 15;                      /* 10^(-knee / 20), Q31 */
    s->rise = 0;
    if (m->tr) {
        uint32_t nb = opz_mul_u32_shr16(m->tr * 704u, m1) >> 8;          /* 4 ms x 44.1 / 64 blocks per unit */
        s->rstep = OPZ_Q31_ONE - opz_decay_coef((int32_t)nb);            /* 1 / blocks */
        s->stage = ST_RISE;
        s->env = 0;
    } else {
        s->stage = ST_ONE;
        s->env = OPZ_Q31_ONE;
    }

    if (m->flags & F_CLOSE) s_choke[s->track]++;
    s->choke = s_choke[s->track];
    s->age = 0;
    if (params) opz_mix_start(&s->mix, params);
    else s->mix.l = s->mix.r = OPZ_MIX_UNITY;
    s->active = v != 0;
    s->fading = 0;

    /* The hit's length for the envelope page: the rise, stage 1 down to the knee, stage 2 from the knee to -60 dB
     * (t1, t2 are ms to -60 dB at their rates), in ms x 60, then x 44.1 / 60 (13 / 16: 10 % margin). */
    uint32_t a = opz_mul_u32_shr16((uint32_t)m->t1 * m->knee + m->tr * 240u, m1);
    uint32_t b = opz_mul_u32_shr16((uint32_t)m->t2 * (60u - m->knee), m2);
    opz_drumenv_start(&s->denv, ((a + b) * 13) >> 4, params);
}

void onNoteOff(int note, void *state)
{
    (void)note;
    opz_drumenv_off(&((State *)state)->denv);                 /* gate style only; one-shots decay on their own */
}

/* One edge at time t (Q16 samples from the block start, below END) whose new value has sign mask m (0: +AMP,
 * -1: -AMP): a step of 2 AMP spread over samples k, k+1, k+2 by the quadratic B-spline of the fraction. */
static inline __attribute__((always_inline)) void deposit(uint32_t *d, int32_t t, int32_t m)
{
    int k = t >> 16;
    uint32_t f = ((uint32_t)t & 0xffff) >> 1, g = 32768 - f;           /* Q15 */
    int32_t a0 = (int32_t)(opz_mul_u16(g, g) >> 16), a2 = (int32_t)(opz_mul_u16(f, f) >> 16);
    int32_t a1 = 32768 - a0 - a2;                                       /* the three sum to 1 exactly */
    d[k] += (uint32_t)(((a0 << 13) ^ m) - m);                         /* unsigned: wraps, never traps */
    d[k + 1] += (uint32_t)(((a1 << 13) ^ m) - m);
    d[k + 2] += (uint32_t)(((a2 << 13) ^ m) - m);
}

/* A half period as render may find it: onNoteOn runs from a task while render runs in the audio interrupt, so
 * render can see a state that is only half written (half periods of 0 next to edge times of the new hit, which
 * looped forever and would freeze the OP-Z). Clamped, and with every edge time inside its half period, the edge
 * loop below always ends within a block: at most 33 edges per square. */
static inline __attribute__((always_inline)) int32_t safe_hp(int32_t h)
{
    return h < MIN_HP ? MIN_HP : h > MAX_HP ? MAX_HP : h;
}

/* The six squares' edges in this block into s_d, in pairs, each pair's edges in time order (in ring models a
 * pair's sign is the product of its two squares, so which edge comes first matters). */
static __attribute__((noinline)) void squares(State *s)
{
    uint32_t *d = s_d;
    d[0] = (uint32_t)s->carry0;
    d[1] = (uint32_t)s->carry1;
    for (int i = 2; i < OPZ_BLOCK_FRAMES + 2; i++) d[i] = 0;
    int32_t ring = s->ring ? -1 : 0;
    uint32_t sg = s->signs;
    for (int p = 0; p < 6; p += 2) {
        int32_t ha = safe_hp(s->hp[p]), hb = safe_hp(s->hp[p + 1]), ta = s->t[p], tb = s->t[p + 1];
        if ((uint32_t)ta >= (uint32_t)ha) ta = 0;                       /* an edge is always within a half period */
        if ((uint32_t)tb >= (uint32_t)hb) tb = 0;
        int32_t ma = -(int32_t)((sg >> p) & 1), mb = -(int32_t)((sg >> (p + 1)) & 1);
        for (;;) {
            if (ta <= tb) {
                if (ta >= END) break;
                ma = ~ma;
                deposit(d, ta, ma ^ (mb & ring));
                ta += ha;
            } else {
                if (tb >= END) break;
                mb = ~mb;
                deposit(d, tb, mb ^ (ma & ring));
                tb += hb;
            }
        }
        s->t[p] = ta - END;
        s->t[p + 1] = tb - END;
        sg = (sg & ~(3u << p)) | (uint32_t)(ma & 1) << p | (uint32_t)(mb & 1) << (p + 1);
    }
    s->signs = (uint8_t)sg;
    s->carry0 = (int32_t)d[OPZ_BLOCK_FRAMES];
    s->carry1 = (int32_t)d[OPZ_BLOCK_FRAMES + 1];
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                             /* pitch comes from the key; knobs from the hit */
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    if (!s->active) { OPZ_ADSR_STAGE(env) = 0; return; }
    if (!opz_drumenv_block(&s->denv, s_cb, env, OPZ_Q31_ONE)) return;    /* the envelope page ended the hit */

    if (OPZ_DRUMENV_STOLEN(&s->denv, env) && !s->fading) {
        /* The host wants this voice for a new hit (voice steal, stage 5/6) and starts it at stage 0: fade out
         * in a few milliseconds. */
        s->fading = 1;
        s->stage = ST_CUT;
        s->c64 = FADE_C64;
    }
    /* Every field is checked before it is used as an index or a loop bound: render may see a torn state. */
    int open = s->model < 24 && (k_models[s->model].flags & F_OPEN);
    if (s->stage != ST_CUT && open && s->choke != s_choke[s->track & 15]) {
        s->stage = ST_CUT;                                     /* choked by a closing hat on this track */
        s->c64 = CUT_C64;
    }

    /* The envelope for this block, ramped linearly from e0 to e1 across it. */
    int32_t e0 = s->env < 0 ? 0 : s->env, e1;
    if (s->stage == ST_RISE) {
        int32_t r = opz_add_q31(s->rise, s->rstep < MIN_RSTEP ? MIN_RSTEP : s->rstep);
        s->rise = r;
        int32_t r2 = opz_mul_q31(r, r);
        e1 = opz_mul_q31(r2, r2);                              /* (t / T)^4: a reversed decay's slow start */
        if (r == OPZ_Q31_ONE) s->stage = ST_ONE;
    } else {
        e1 = opz_mul_q31(e0, s->c64);
        if (s->stage == ST_ONE && e1 < s->knee) {
            s->stage = ST_TWO;
            s->c64 = s->c64b;
        }
    }
    if (e1 < 0) e1 = 0;                                        /* (a torn state's coefficient) */
    s->env = e1;

    squares(s);

    const uint32_t *d = s_d;
    /* times the envelope page's ramp (exactly the hit's own envelope while the page holds) */
    int32_t g0 = opz__drumenv_mul(e0, s->denv.g0), g1 = opz__drumenv_mul(e1, s->denv.g1);
    int32_t y = s->y, eg = g0, de = (g1 - g0) >> 6;
    uint32_t seed = s->seed;
    int32_t gb = s->gb, gh = s->gh, gn = s->gn;
    int32_t ic1 = s->f.ic1, ic2 = s->f.ic2, hpl = s->hpl, hpa = s->hpa;
    const int32_t a1 = s->fc.a1, a2 = s->fc.a2, a3 = s->fc.a3, k = s->fc.k >> 16;     /* k / 2 in Q15 */
    fract32 mono[OPZ_BLOCK_FRAMES];                            /* the voice; opz_mix_mono adds it into out */
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        y = opz_add_q31(y, (int32_t)d[i]);                     /* the squares' sum, within +-6 AMP */
        fract32 x = opz_add_q31(y, opz_mul_q31_q15(opz_noise(&seed), gn));
        int32_t v3 = opz_sub_q31(x, ic2);                      /* opz_svf, band- and high-pass outputs */
        int32_t v1 = opz_add_q31(opz_mul_q31(a1, ic1), opz_mul_q31(a2, v3));
        int32_t v2 = opz_add_q31(ic2, opz_add_q31(opz_mul_q31(a2, ic1), opz_mul_q31(a3, v3)));
        ic1 = opz_add_q31(v1, opz_sub_q31(v1, ic1));
        ic2 = opz_add_q31(v2, opz_sub_q31(v2, ic2));
        int32_t hp = opz_sub_q31(opz_sub_q31(x, opz_shl_q31(opz_mul_q31_q15(v1, k), 1)), v2);
        fract32 o = opz_add_q31(opz_mul_q31_q15(v1, gb), opz_mul_q31_q15(hp, gh));
        o = opz_sub_q31(o, hpl);                               /* the one-pole high-pass */
        hpl = opz_add_q31(hpl, opz_mul_q31_q15(o, hpa));
        o = opz_shl_q31(o, 4);
        o = opz_mul_q31_q15(o, eg >> 16) >> 2;                           /* x OPZ_VOICE_PEAK (2^-2): at most -12 dBFS */
        eg += de;
        mono[i] = o;
    }
    opz_mix_mono(out, mono, &s->mix, params);                  /* level and pan, into interleaved out */
    s->y = y;
    s->seed = seed;
    s->f.ic1 = ic1;
    s->f.ic2 = ic2;
    s->hpl = hpl;

    if ((s->stage != ST_RISE && e1 < THRESHOLD) || ++s->age > MAX_AGE) {
        s->active = 0;
        OPZ_ADSR_STAGE(env) = 0;                               /* free the voice */
    }
}

_Static_assert(OPZ_VOICE_PEAK == 0x20000000, "render applies OPZ_VOICE_PEAK as a shift by 2");

OPZ_EXPORTS_INSTRUMENT();
