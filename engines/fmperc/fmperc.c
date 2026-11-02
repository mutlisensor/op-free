/*
 * fmperc — two-operator FM percussion for the kick, snare, perc and sample tracks. One sine modulator (with
 * feedback) phase-modulates one sine carrier; each of the 24 keys plays its own recipe of pitch, ratio, index,
 * pitch sweep and decays, in six families of four that follow kit's key map:
 *
 *   keys 1-4   FM kicks: deep, punch, boom, hard           keys 13-16 toms: low, mid, high, syn tom
 *   keys 5-8   snare, tight snare, crack, rim               keys 17-20 low wood, high wood, clave, cowbell
 *   keys 9-12  closed hat, pedal hat (choke 11-12),         keys 21-24 bell, glock, zap, laser
 *              open hat, cymbal
 *
 *   P1        ratio       modulator : carrier ratio, read every block: 0 = the key's harmonic ratio (whole or
 *                         half numbers: tonal, woody), 128 = its designed ratio, 255 = its inharmonic ratio
 *                         (metallic, clangorous); linear in between
 *   P2        sweep/decay read at the hit: pitch-sweep depth x0 (P2 0) .. x1 (128) .. x2 (255), and the index and
 *                         amplitude decays x1/4 (0) .. x1 (128) .. x2.8 (255)
 *   velocity              level (linear) and modulation index x0.3 (soft) .. x1 (hard): harder is brighter
 *   octave                the octave buttons transpose every pitch of the key by whole octaves (+-3 at most)
 *
 * Per hit: a pitch envelope sweeps the carrier (and with it the modulator) down linearly in frequency from up to
 * the recipe's sweep depth above the key's pitch; the index and the modulator feedback decay together (bright
 * attack, darker tail; feedback turns the modulator into noise for snares and hats); the amplitude decays on its
 * own. All three are exact exponentials per block, ramped linearly over the 64 samples. The filter and resonance
 * knobs are the OS's own track filter, which runs after the engine; fmperc never reads them.
 *
 * The key index is (note - 53) mod 24, the octave floor((note - 53) / 24) (engine-abi.md, render's pitch). Each hit
 * ends itself (envelope stage 0) when its level has died away, like kit.
 *
 * Ideas from the classic FM drum voices: Peaks' FM drum (Mutable Instruments, MIT) and the DX7's percussion
 * patches; written from scratch on sdk/include/opz_dsp.h, no code ported. MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

#define ALWAYS_INLINE static inline __attribute__((always_inline))
#define NOINLINE static __attribute__((noinline))

enum { F_CHOKER = 1, F_CHOKED = 2 };

/* One key's recipe. Ratios are modulator : carrier in Q12 (4096 = 1). Index and feedback in units of 128 index
 * units (index units: 16384 = one turn = 2 pi rad of phase deviation, so byte 20 ~ 1 rad, 255 = 12.5 rad). Times
 * are log bytes: T60 (time to -60 dB) = 2^(b / 16) ms, so 0 = 1 ms, 64 = 16 ms, 128 = 256 ms, 160 = 1 s, 176 = 2 s. */
typedef struct Recipe {
    uint16_t hz;              /* carrier, Hz, at the key's own octave */
    uint16_t r_harm, r_nom, r_metal;  /* ratio at P1 0, 128, 255 */
    uint8_t idx, fb;          /* peak modulation index and feedback at full velocity */
    uint8_t sweep;            /* pitch-sweep depth at P2 128, 1/16 octave (0 = none) */
    uint8_t tp, ti, ta;       /* T60 log bytes: pitch sweep, index (and feedback), amplitude */
    uint8_t trim;             /* level, /256, to balance the keys against kit */
    uint8_t flags;
} Recipe;

#define R(x) ((uint16_t)((x) * 4096 + 0.5))   /* a ratio in Q12, folded at compile time (no float at run time) */

static const Recipe k_recipes[24] = {
    /*  hz       harm      nom    metal   idx   fb  swp   tp   ti   ta  trim  flags */
    {   50,  R(0.5),  R(1.0), R(1.41),   50,   0,  40, 116, 101, 178,  255, 0 },         /*  1 deep kick */
    {   60,  R(2.0),  R(1.0), R(1.73),   80,   0,  56, 104,  85, 160,  255, 0 },         /*  2 punch kick */
    {   44,  R(1.0),  R(0.5), R(0.71),   30,   0,  24, 127, 116, 186,  255, 0 },         /*  3 boom kick */
    {   64,  R(1.0),  R(3.0), R(3.53),  110,  40,  64,  90,  62, 154,  255, 0 },         /*  4 hard kick */
    {  185,  R(1.0), R(1.41), R(2.27),  100, 180,  12,  78, 120, 148,  255, 0 },         /*  5 snare */
    {  235,  R(1.5), R(2.13), R(3.17),  120, 230,  10,  62, 112, 138,  255, 0 },         /*  6 tight snare */
    {  900,  R(1.0), R(3.71), R(5.33),  160, 255,   8,  53, 132, 135,  230, 0 },         /*  7 crack */
    {  500,  R(1.0), R(2.66), R(3.41),   70,   0,   4,  37,  48, 101,  255, 0 },         /*  8 rim */
    { 6000,  R(2.0), R(3.36), R(4.73),  200, 220,   0,   0, 127, 110,  220, F_CHOKER },  /*  9 closed hat */
    { 5600,  R(2.0), R(3.36), R(4.73),  190, 220,   0,   0, 140, 127,  170, F_CHOKER },  /* 10 pedal hat */
    { 6000,  R(2.0), R(3.36), R(4.73),  200, 220,   0,   0, 170, 164,  100, F_CHOKED },  /* 11 open hat */
    { 4200,  R(2.0), R(2.53), R(3.73),  230, 160,   0,   0, 185, 185,   80, F_CHOKED },  /* 12 cymbal */
    {   82,  R(2.0),  R(1.0), R(1.41),   30,   0,  16, 106,  94, 170,  255, 0 },         /* 13 low tom */
    {  120,  R(2.0),  R(1.0), R(1.41),   30,   0,  16, 106,  94, 167,  255, 0 },         /* 14 mid tom */
    {  170,  R(2.0),  R(1.0), R(1.41),   30,   0,  16, 106,  94, 164,  255, 0 },         /* 15 high tom */
    {  150,  R(2.0),  R(1.0), R(1.41),   45,  20,  32, 132, 110, 164,  255, 0 },         /* 16 syn tom */
    {  520,  R(1.0), R(1.66),  R(2.4),   60,   0,   6,  48,  57, 122,  255, 0 },         /* 17 low wood */
    { 1050,  R(1.0), R(1.66),  R(2.4),   60,   0,   6,  48,  57, 116,  255, 0 },         /* 18 high wood */
    { 2350,  R(2.0),  R(1.0), R(1.41),   25,   0,   0,   0,  53, 110,  255, 0 },         /* 19 clave */
    {  560,  R(1.0), R(1.48), R(2.12),   60,  10,   0,   0, 110, 148,  230, 0 },         /* 20 cowbell */
    {  600,  R(2.0), R(1.41), R(2.76),   90,   0,   0,   0, 157, 181,  150, 0 },         /* 21 bell */
    { 1320,  R(4.0),  R(3.5), R(5.19),   40,   0,   0,   0, 132, 167,  150, 0 },         /* 22 glock */
    {  220,  R(2.0),  R(1.0), R(1.41),   40,   0,  80,  94, 106, 138,  255, 0 },         /* 23 zap */
    {  330,  R(1.0),  R(0.5), R(0.71),   60,  60,  96, 116, 122, 148,  240, 0 },         /* 24 laser */
};

#define INC_MAX   0x40000000u   /* carrier increment limit: fs / 4, 11 kHz */
#define THRESHOLD 0x00080000    /* amplitude (Q31, gain included) below which the hit is over: -72 dBFS */

typedef struct State {
    uint32_t pc, pm;          /* carrier and modulator phase, Q32 turns */
    uint32_t ic0, dic;        /* carrier increment at rest, and the sweep's extra increment at env_p = 1 */
    int32_t ep, cp;           /* pitch envelope (Q31) and its per-block multiplier */
    int32_t ei, ef, ci;       /* index and feedback (index units, Q16) and their per-block multiplier */
    int32_t ea, ca;           /* amplitude (Q31, velocity, trim and OPZ_VOICE_PEAK included), per-block multiplier */
    int32_t y0, y1;           /* last two modulator outputs, Q15 (the feedback averages them) */
    uint8_t key, track, choke, active, fading;
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes (the device's real limit)");

static const ModuleCallbackInterface *s_cb;
static uint8_t s_choke[16];                   /* per track: bumped by closed hats, watched by open hats */
static int16_t s_mod[OPZ_BLOCK_FRAMES];       /* render's scratch: the modulator, Q15 (one voice at a time) */

/* sin(2 pi i / 256) in Q15 with a guard point; linear interpolation is within 8e-5 (-82 dB) of the sine.
 * Generated by round(32767 * sin(2 * pi * i / 256)) for i = 0 .. 256. */
static const int16_t k_sin[257] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
    0
};

/* Signed 16 x 16 -> 32 multiply of the low halves: one Blackfin instruction. */
#if defined(__bfin__)
ALWAYS_INLINE int32_t mul_s16(int32_t a, int32_t b)
{
    int32_t r;
    __asm__("%0 = %1.L * %2.L (IS);" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
#else
ALWAYS_INLINE int32_t mul_s16(int32_t a, int32_t b) { return (int32_t)(int16_t)a * (int16_t)b; }
#endif

/* sin(2 pi phase) in Q15 from the table, linearly interpolated on the next 15 bits of phase. */
ALWAYS_INLINE int32_t sin15(uint32_t ph)
{
    uint32_t i = ph >> 24;
    int32_t f = (int32_t)((ph >> 9) & 0x7fff);
    int32_t a = k_sin[i];
    return a + (mul_s16(k_sin[i + 1] - a, f) >> 15);
}

/* The per-block multiplier (Q31) of an exponential decay with T60 = 2^(b / 16) ms, b clamped to 0..240: 2^-x with
 * x = 64 samples / (T60 / ln 1000) / ln 2 = 14.46 / T60_ms octaves of level per block. No division. */
static int32_t block_coef(int32_t b)
{
    if (b < 0) b = 0;
    if (b > 240) b = 240;
    int32_t x = opz_exp2_q16(252580 - b * 4096);          /* log2(14.46) = 3.854 -> 252580 in Q16 */
    int32_t c = opz_exp2_q16(-x);                          /* Q16, 0 < c < 1 */
    if (c > 65535) c = 65535;
    return c << 15;
}

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
    ((State *)state)->track = (uint8_t)(track & 15);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("Os")))   /* once per hit: size matters more than speed */
#endif
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    int key = (note < -1000 ? -1000 : note > 1000 ? 1000 : note) - 53, oct = 0;
    while (key < 0) { key += 24; oct--; }                   /* key = (note - 53) mod 24, oct = floor(/ 24) */
    while (key >= 24) { key -= 24; oct++; }                 /* (no division: no libgcc) */
    if (oct < -3) oct = -3;
    if (oct > 3) oct = 3;
    const Recipe *k = &k_recipes[key];
    s->key = (uint8_t)key;

    int32_t p2 = params ? opz_p16(params[OPZ_P_P2]) : 32768;  /* params can be NULL: P2 at its centre */
    if (p2 > 65535) p2 = 65535;                                 /* words above the host's 0x7fffffff */
    int32_t u = p2 - 32768;
    int32_t dshift = u < 0 ? (u * 32) >> 15 : (u * 24) >> 15;    /* decays: -2 .. +1.5 octaves of time */

    /* pitch: the key's carrier, transposed by the octave, below INC_MAX */
    uint32_t ic = opz_hz_to_inc((int32_t)k->hz << 16);
    if (oct < 0) ic >>= -oct;
    for (int i = 0; i < oct && ic < INC_MAX; i++) ic <<= 1;
    if (ic > INC_MAX) ic = INC_MAX;
    s->ic0 = ic;

    /* sweep: start sweep x P2 / 128 sixteenths of an octave above (at most INC_MAX), decaying in frequency */
    int32_t depth = (int32_t)k->sweep * (p2 >> 3);              /* Q16 octaves: sweep / 16 x p2 / 32768 */
    uint32_t start = ic;
    while (depth >= 65536 && start < INC_MAX) {
        start <<= 1;
        depth -= 65536;
    }
    if (start < INC_MAX) start = opz_mul_u32_shr16(start, (uint32_t)opz_exp2_q16(depth));   /* < 2^32 */
    if (start > INC_MAX) start = INC_MAX;
    s->dic = start - ic;
    s->ep = k->sweep ? OPZ_Q31_ONE : 0;
    s->cp = block_coef(k->tp);

    /* velocity: level, and index x (0.3 + 0.7 v) */
    int32_t v31 = (int32_t)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u);
    int32_t bright = 0x26666666 + opz_mul_q31(0x59999999, v31);
    s->ei = opz_mul_q31((int32_t)k->idx << 23, bright);           /* idx x 128 units, Q16: idx << 23 */
    s->ef = opz_mul_q31((int32_t)k->fb << 23, bright);
    s->ci = block_coef(k->ti + dshift);
    s->ea = opz_mul_q31(opz_mul_q31(v31, (int32_t)k->trim << 23), OPZ_VOICE_PEAK);
    s->ca = block_coef(k->ta + dshift);

    s->pc = s->pm = 0;
    s->y0 = s->y1 = 0;
    if (k->flags & F_CHOKER) s_choke[s->track]++;
    s->choke = s_choke[s->track];
    s->active = 1;
    s->fading = 0;
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;                                   /* one-shots: the hit decays on its own */
}

/* ---- render: two passes over the block, each a short loop that fits the Blackfin's registers ------------------- */

/* Pass 1: the modulator into s_mod, stepping by im (+ sm per sample), with the averaged feedback ef (+ def) if fb. */
ALWAYS_INLINE void mod_pass(State *s, uint32_t im, int32_t sm, int32_t ef, int32_t def, int fb)
{
    uint32_t pm = s->pm;
    int32_t y0 = s->y0, y1 = s->y1;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        uint32_t p = pm;
        if (fb) p += (uint32_t)mul_s16((y0 + y1) >> 1, ef >> 16) << 3;
        int32_t m = sin15(p);
        y1 = y0;
        y0 = m;
        s_mod[i] = (int16_t)m;
        pm += im;
        im += (uint32_t)sm;
        if (fb) ef += def;
    }
    s->pm = pm;
    s->y0 = y0;
    s->y1 = y1;
}

NOINLINE void mod_plain(State *s, uint32_t im, int32_t sm) { mod_pass(s, im, sm, 0, 0, 0); }
NOINLINE void mod_fb(State *s, uint32_t im, int32_t sm, int32_t ef, int32_t def) { mod_pass(s, im, sm, ef, def, 1); }

/* Pass 2: the carrier, phase-modulated by s_mod at index ei (+ dei), times the amplitude ea (+ dea), added into the
 * interleaved stereo out with saturation. */
NOINLINE void car_pass(fract32 *out, State *s, uint32_t ic, int32_t sc, int32_t ei, int32_t dei, int32_t ea,
                       int32_t dea)
{
    uint32_t pc = s->pc;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t c = sin15(pc + ((uint32_t)mul_s16(s_mod[i], ei >> 16) << 3));
        fract32 v = mul_s16(c, ea >> 16) * 2;                     /* Q15 x Q15 -> Q31, at most OPZ_VOICE_PEAK */
        out[2 * i] = opz_add_q31(out[2 * i], v);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], v);
        pc += ic;
        ic += (uint32_t)sc;
        ei += dei;
        ea += dea;
    }
    s->pc = pc;
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                             /* drum tracks: the pitch comes from the key */
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    if (!s->active) { OPZ_ADSR_STAGE(env) = 0; return; }
    const Recipe *k = &k_recipes[s->key];

    /* The host wants this voice for a new note (voice steal, stage 5/6) and starts it when we reach stage 0: fade
     * out over this block instead of ringing on. A closed hat on the same track chokes an open one: ~30 ms. */
    if (OPZ_ADSR_STAGE(env) >= 5) s->fading = 1;
    if ((k->flags & F_CHOKED) && s->choke != s_choke[s->track]) {
        s->ca = block_coef(78);
        s->choke = s_choke[s->track];
    }

    /* ratio from P1 (live), Q16: harm (0) -> nom (128) -> metal (255), linear */
    int32_t t = params ? opz_p16(params[OPZ_P_P1]) : 32768;
    if (t > 65535) t = 65535;
    int32_t r;
    if (t < 32768) r = k->r_harm + (((k->r_nom - k->r_harm) * (t >> 1)) >> 14);
    else r = k->r_nom + (((k->r_metal - k->r_nom) * ((t - 32768) >> 1)) >> 14);
    uint32_t r16 = (uint32_t)r << 4;

    /* carrier increment at the block's start and end (sweep), modulator = carrier x ratio */
    int32_t ep1 = opz_mul_q31(s->ep, s->cp);
    uint32_t ic_a = s->ic0 + (uint32_t)opz_mul_q31((int32_t)s->dic, s->ep);
    uint32_t ic_b = s->ic0 + (uint32_t)opz_mul_q31((int32_t)s->dic, ep1);
    int32_t sc = -(int32_t)((ic_a - ic_b) >> 6);                 /* the sweep only falls: ic_a >= ic_b */
    uint32_t im = opz_mul_u32_shr16(ic_a, r16);
    int32_t sm = -(int32_t)opz_mul_u32_shr16((uint32_t)-sc, r16);
    s->ep = ep1;

    /* index, feedback and amplitude ramps to their values at the block's end */
    int32_t ei1 = opz_mul_q31(s->ei, s->ci), ef1 = opz_mul_q31(s->ef, s->ci);
    int32_t ea1 = s->fading ? 0 : opz_mul_q31(s->ea, s->ca);

    if (s->ef | ef1) mod_fb(s, im, sm, s->ef, (ef1 - s->ef) >> 6);
    else mod_plain(s, im, sm);
    car_pass(out, s, ic_a, sc, s->ei, (ei1 - s->ei) >> 6, s->ea, (ea1 - s->ea) >> 6);

    s->ei = ei1;
    s->ef = ef1;
    s->ea = ea1;
    if (s->fading || ea1 < THRESHOLD) {
        s->active = 0;
        OPZ_ADSR_STAGE(env) = 0;                                 /* free the voice */
    }
}

OPZ_EXPORTS_INSTRUMENT();
