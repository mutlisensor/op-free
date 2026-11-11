/*
 * 8bit — chiptune drums for the kick, snare, perc and sample tracks. Each of the 24 keys plays its own recipe on a
 * console-style voice: one tone channel (4-bit triangle or 12.5/25/50 % pulse) and one noise channel (a 15-bit
 * LFSR, long or short "metallic" mode), each with a stepped 4-bit volume envelope.
 *
 *   keys 1-4   kicks: triangle, pulse, boom, hard      keys 13-16 toms: low, mid, high (triangle), pulse tom
 *   keys 5-8   snares: triangle, tight, metallic; rim  keys 17-20 blip, chirp, coin, zap
 *   keys 9-12  hats: closed, closed metallic,          keys 21-24 laser, power-up, explosion, jump
 *              open, open metallic (9-10 choke 11-12)
 *
 *   P1        tune        -12..+12 semitones in semitone steps (128 = nominal); the pitch sweep's depth x0.5..x2
 *   P2        crunch      0-63 clean (band-limited pulse, smooth triangle, 16-bit) / 64-255 raw chip: 4-bit
 *                         triangle, naive pulse, DAC 8 bits at 64 down to 3 bits at 224; from 144 a sample-and-hold
 *                         down to 5.5 kHz at 240; from 208 every noise channel turns metallic
 *   velocity              level
 *   octave                transposes tone and noise by 12 semitones per press
 *   attack .. release     the OP-Z drum envelope over the hit, as on TE's sample kits: start, attack, hold, decay
 *                         (opz_drumenv in opz_adsr.h); the presets' 0 / 0 / 255 / 0 leave every hit as designed
 *   note style gate       a key release fades the hit (~20 ms); the other styles play one-shots
 *
 * Everything moves in frames, as on a console: every `frame` blocks (1.45 ms each) a recipe steps its pitch sweep
 * (an offset that shrinks by `drag` and moves by `slide`), its arpeggio jumps and its two volume envelopes. Pitch,
 * noise clock and crunch are recomputed every block from the knobs, so P1 and P2 act on a ringing hit. Each voice
 * ends itself when both volumes reach 0.
 *
 * The noise channel steps its register k times per sample in one go (k <= 8, a clock up to 353 kHz): the k new
 * feedback bits are (s ^ (s >> tap)) & (2^k - 1), valid while k <= 15 - tap. tap 1 is the long mode (32,767
 * steps), tap 6 the short one (93 steps), as on the classic console noise channels. Written from scratch on
 * sdk/include/opz_dsp.h after the idea of Mutable Instruments Plaits' chiptune engines; no code taken. MIT licence,
 * see CONTRIBUTING.md.
 */
#include "opz_adsr.h"
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

enum { W_TRI = 0, W_P12 = 1, W_P25 = 2, W_P50 = 3 };              /* tone waveform: flags bits 0-1 */
enum { F_SHORT = 4, F_NSWEEP = 8, F_CHOKER = 16, F_CHOKED = 32 };  /* metallic noise, noise follows the sweep */

/* Envelope byte: frames per step (1-63) and the step, 1 << d volume units of 15 */
#define E(period, d) ((period) | ((d) << 6))

typedef struct Recipe {
    uint8_t note;     /* tone pitch, MIDI note */
    uint8_t flags;
    uint8_t frame;    /* blocks per frame */
    int8_t sweep;     /* pitch offset at the hit, semitones */
    uint8_t drag;     /* the offset shrinks by drag/256 per frame */
    int8_t slide;     /* and moves by slide/8 semitones per frame */
    uint8_t jt;       /* arpeggio: frames between jumps */
    int8_t js;        /* semitones per jump */
    uint8_t jn;       /* number of jumps */
    uint8_t gt, et;   /* tone level (0 = no tone), envelope */
    uint8_t nn;       /* noise clock as a note: 120 = 44.1 kHz, 12 per octave, at most 156 (8 steps per sample) */
    uint8_t gn, en;   /* noise level (0 = no noise), envelope */
} Recipe;

static const Recipe k_rec[24] = {
    /* note  flags                  frame sweep drag slide jt js jn   gt  et         nn   gn   en */
    {   31, W_TRI,                     2,  36,  58,   0,  0, 0, 0, 255, E(6, 0),   140,  60, E(1, 2) },  /* kick tri */
    {   33, W_P50,                     2,  24,  50,   0,  0, 0, 0, 230, E(4, 0),     0,   0, 0 },        /* kick pulse */
    {   28, W_TRI,                     2,  30,  30,   0,  0, 0, 0, 255, E(10, 0),    0,   0, 0 },        /* boom */
    {   36, W_P25,                     1,  36,  90,   0,  0, 0, 0, 220, E(5, 0),   132, 120, E(1, 1) },  /* kick hard */
    {   48, W_TRI,                     2,  12,  60,   0,  0, 0, 0, 200, E(2, 0),   136, 200, E(4, 0) },  /* snare tri */
    {   55, W_P25,                     1,   7,  80,   0,  0, 0, 0, 120, E(2, 0),   144, 220, E(3, 0) },  /* tight */
    {   50, W_TRI | F_SHORT,           2,  12,  70,   0,  0, 0, 0, 120, E(1, 0),   126, 220, E(3, 0) },  /* metallic */
    {   84, W_P12,                     1,   0,   0,   0,  0, 0, 0, 255, E(1, 0),   150, 100, E(1, 2) },  /* rim */
    {    0, F_CHOKER,                  1,   0,   0,   0,  0, 0, 0,   0, 0,         156, 220, E(3, 1) },  /* closed hat */
    {    0, F_SHORT | F_CHOKER,        1,   0,   0,   0,  0, 0, 0,   0, 0,         150, 200, E(3, 1) },  /* closed met */
    {    0, F_CHOKED,                  1,   0,   0,   0,  0, 0, 0,   0, 0,         156, 100, E(18, 0) }, /* open hat */
    {    0, F_SHORT | F_CHOKED,        2,   0,   0,   0,  0, 0, 0,   0, 0,         144, 100, E(12, 0) }, /* open met */
    {   40, W_TRI,                     2,  12,  20,   0,  0, 0, 0, 255, E(5, 0),     0,   0, 0 },        /* low tom */
    {   47, W_TRI,                     2,  12,  20,   0,  0, 0, 0, 255, E(5, 0),     0,   0, 0 },        /* mid tom */
    {   54, W_TRI,                     2,  12,  20,   0,  0, 0, 0, 255, E(4, 0),     0,   0, 0 },        /* high tom */
    {   57, W_P25,                     2,   5,  40,   0,  0, 0, 0, 180, E(3, 0),     0,   0, 0 },        /* pulse tom */
    {   84, W_P50,                     1,   0,   0,   0,  0, 0, 0, 200, E(3, 0),     0,   0, 0 },        /* blip */
    {   81, W_P25,                     4,   0,   0,   0,  6, 12, 1, 150, E(3, 0),    0,   0, 0 },        /* chirp */
    {   83, W_P50,                    11,   0,   0,   0,  5, 5, 1, 160, E(2, 0),     0,   0, 0 },        /* coin */
    {   72, W_P12,                     2,  36,  30,  -8,  0, 0, 0, 220, E(3, 0),     0,   0, 0 },        /* zap */
    {   88, W_P50,                     4,   0,   0,  -6,  0, 0, 0, 150, E(2, 0),     0,   0, 0 },        /* laser */
    {   60, W_P25,                     4,   0,   0,   2,  3, 4, 8, 180, E(3, 0),     0,   0, 0 },        /* power-up */
    {   28, W_TRI | F_NSWEEP,          4,  24,  40,  -1,  0, 0, 0, 150, E(3, 0),   132, 170, E(12, 0) }, /* explosion */
    {   64, W_P25,                     4,   0,   0,   4,  0, 0, 0, 180, E(2, 0),     0,   0, 0 },        /* jump */
};

#define AMP_UNIT 561000        /* volume 15 x level 255 x AMP_UNIT ~ 1.0 in Q31 */
#define INC_A4 42852281u       /* 440 Hz as a Q32 phase increment at 44.1 kHz */
#define OFF_MAX (96 << 8)      /* sweep offset limit, Q8 semitones */

typedef struct State {
    uint32_t ph, nacc, lfsr;
    int32_t off;              /* sweep offset, Q8 semitones */
    int32_t held, gv;         /* sample-and-hold value; velocity x OPZ_VOICE_PEAK, Q31 */
    int16_t jump;             /* semitones from the arpeggio so far */
    int8_t trans;             /* octave buttons, semitones */
    int8_t vt, vn;            /* tone and noise volume, 0-15 */
    OpzMix mix;               /* the track's level and pan (opz_mix.h) */
    OpzDrumEnv denv;          /* the envelope page over the hit */
    uint8_t key, fc, jc, jleft, ct, cn, hc, track, choke, active, fade;
} State;

_Static_assert(sizeof(State) <= 280, "voice state must fit in 280 bytes");

static const ModuleCallbackInterface *s_cb;
static uint8_t s_choke[16];                    /* per track: bumped by closed hats, watched by open hats */
static fract32 s_tone[OPZ_BLOCK_FRAMES];       /* the tone channel of the voice being rendered */

/* render takes params NULL as the knobs at their centre (as it does for P1): level full, pan centre */
static const uint32_t k_mix_centre[OPZ_P_LEVEL + 1] = { [OPZ_P_PAN] = 0x40000000u, [OPZ_P_LEVEL] = 0x7fffffffu };

/* Pulse gain / 2 by duty, 1 / (2 (1 - duty)): a zero-mean pulse at -12 dBFS like the triangle */
static const int32_t k_pulse_gain[4] = { 0, 0x49249249, 0x55555555, 0x7fffffff };

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
    s->lfsr = ((0x2a5bu + 7919u * track) & 0x7fffu) | 1u;
}

/* Blocks until a channel's stepped envelope reaches 0: ceil(15 / step) steps of `period` frames (0: no channel). */
static uint32_t env_blocks(uint8_t g, uint8_t e, uint8_t frame)
{
    uint32_t d = e >> 6;
    return g ? ((15u + (1u << d) - 1u) >> d) * (e & 63u) * frame : 0;
}

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    /* P1 and P2 are read every block in render; the envelope page at the hit */
    State *s = state;
    int d = note - 53, oct = 0;
    if (d < -96) d = -96;
    if (d > 119) d = 119;
    while (d < 0) { d += 24; oct--; }
    while (d >= 24) { d -= 24; oct++; }
    const Recipe *r = &k_rec[d];
    s->key = (uint8_t)d;
    s->trans = (int8_t)(oct * 12);
    s->gv = (int32_t)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u) >> 2;   /* vel/127 x -12 dBFS */
    s->ph = 0;
    s->nacc = 0;
    if ((s->lfsr & 0x7fff) == 0) s->lfsr = 1;
    s->off = r->sweep * 256;
    s->jump = 0;
    s->jc = 0;
    s->jleft = r->jt ? r->jn : 0;
    s->fc = s->ct = s->cn = 0;
    s->vt = r->gt ? 15 : 0;
    s->vn = r->gn ? 15 : 0;
    s->hc = 0;
    s->held = 0;
    s->fade = 0;
    if (r->flags & F_CHOKER) s_choke[s->track]++;
    s->choke = s_choke[s->track];
    if (params) opz_mix_start(&s->mix, params);
    else s->mix.l = s->mix.r = OPZ_MIX_UNITY;
    s->active = 1;

    /* The hit's length for the envelope page: the longer channel's stepped envelope, exactly, plus a frame */
    uint32_t bt = env_blocks(r->gt, r->et, r->frame), bn = env_blocks(r->gn, r->en, r->frame);
    opz_drumenv_start(&s->denv, ((bt > bn ? bt : bn) + r->frame) * OPZ_BLOCK_FRAMES, params);
}

void onNoteOff(int note, void *state)
{
    (void)note;
    opz_drumenv_off(&((State *)state)->denv);     /* gate style only; one-shots decay on their own */
}

/* Q8 semitones above A4 -> Q16 octaves (5461 / 256 ~ 65536 / 12 / 256) */
static inline int32_t semis_to_oct(int32_t q8) { return (q8 * 5461) >> 8; }

static void env_step(int8_t *v, uint8_t *count, uint8_t e)
{
    if (*v > 0 && ++*count >= (e & 63)) {
        *count = 0;
        int n = *v - (1 << (e >> 6));
        *v = (int8_t)(n < 0 ? 0 : n);
    }
}

/* One frame: the sweep, the arpeggio and the envelopes move one step. */
static void frame_step(State *s, const Recipe *r)
{
    int32_t off = s->off - ((s->off * r->drag) >> 8) + r->slide * 32;
    s->off = off > OFF_MAX ? OFF_MAX : off < -OFF_MAX ? -OFF_MAX : off;
    if (s->jleft && ++s->jc >= r->jt) {
        s->jc = 0;
        s->jleft--;
        s->jump += r->js;
    }
    env_step(&s->vt, &s->ct, r->et);
    env_step(&s->vn, &s->cn, r->en);
}

/* The tone channel at gain at into s_tone: 4-bit or smooth triangle, naive or band-limited pulse. */
static void tone_block(State *s, int wave, uint32_t inc, int32_t at, int clean)
{
    uint32_t ph = s->ph;
    if (wave == W_TRI) {
        if (clean) {
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                s_tone[i] = opz_mul_q31(opz_tri(ph), at);
                ph += inc;
            }
        } else {
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                uint32_t v = (ph + 0x40000000u) >> 27;                     /* 32 steps, 0 at phase 0, rising */
                uint32_t t = (v ^ (0u - (v >> 4))) & 15;                   /* 0..15..0 */
                s_tone[i] = opz_mul_q31(((int32_t)t * 2 - 15) * (1 << 27), at);
                ph += inc;
            }
        }
    } else {
        uint32_t d = 0x10000000u << wave;                                 /* duty 1/8, 1/4, 1/2 */
        int32_t ag = opz_mul_q31(at, k_pulse_gain[wave]);                 /* the low part peaks at full scale */
        if (clean) {
            uint32_t rc = opz_blep_recip(inc);
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                fract32 x = (opz_saw_blep(ph, inc, rc) >> 1) - (opz_saw_blep(ph - d, inc, rc) >> 1);
                s_tone[i] = opz_shl_q31(opz_mul_q31(x, ag), 1);
                ph += inc;
            }
        } else {
            /* d - 1 below the duty, d above (no DC), as Q31 halves */
            fract32 hi = opz_shl_q31(opz_mul_q31((int32_t)(d >> 1), ag), 1);
            fract32 lo = opz_shl_q31(opz_mul_q31((int32_t)((d >> 1) - 0x80000000u), ag), 1);
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                s_tone[i] = ph < d ? lo : hi;
                ph += inc;
            }
        }
    }
    s->ph = ph;
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                 /* drum tracks: the pitch comes from the key's recipe */
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;
    if (!s->active || s->key >= 24) { s->active = 0; OPZ_ADSR_STAGE(env) = 0; return; }
    if (!opz_drumenv_block(&s->denv, s_cb, env, s->gv)) return;          /* the envelope page ended the hit */
    const Recipe *r = &k_rec[s->key];

    uint32_t p1 = params ? params[OPZ_P_P1] >> 23 : 128, p2 = params ? params[OPZ_P_P2] >> 23 : 128;
    if (p1 > 255) p1 = 255;
    if (p2 > 255) p2 = 255;

    if (OPZ_DRUMENV_STOLEN(&s->denv, env) && !s->fade) s->fade = 5;   /* voice steal: gone in three blocks (4 ms) */
    if ((r->flags & F_CHOKED) && s->choke != s_choke[s->track]) {
        s->choke = s_choke[s->track];
        if (!s->fade) s->fade = 3;                           /* choked by a closed hat: five blocks */
    }
    if (s->fade) {
        s->vt = (int8_t)(s->vt > s->fade ? s->vt - s->fade : 0);
        s->vn = (int8_t)(s->vn > s->fade ? s->vn - s->fade : 0);
    }

    /* Pitch: key, octave, arpeggio and tune in semitones, plus the sweep scaled by P1. */
    int32_t semis = (((int32_t)p1 * 25) >> 8) - 12 + s->trans + s->jump;
    int32_t off = (s->off * (opz_exp2_q16(((int32_t)p1 - 128) * 512) >> 4)) >> 12;   /* x 2^((P1 - 128) / 128) */
    int32_t o = semis_to_oct((semis + r->note - 69) * 256 + off);
    o = o < -(9 << 16) ? -(9 << 16) : o > (5 << 16) - 1 ? (5 << 16) - 1 : o;
    uint32_t inc = opz_mul_u32_shr16(INC_A4, (uint32_t)opz_exp2_q16(o));            /* below fs/2 */
    o = semis_to_oct((semis + r->nn - 156) * 256 + ((r->flags & F_NSWEEP) ? off : 0));
    o = o < -(12 << 16) ? -(12 << 16) : o > 0 ? 0 : o;
    uint32_t rate = (uint32_t)opz_exp2_q16(o) << 3;            /* noise steps per sample, Q16, at most 8 */

    /* Crunch */
    int clean = p2 < 64;
    int bits = clean ? 16 : 8 - (int)((p2 - 64) >> 5);                    /* 8 bits at 64, one less per 32 */
    uint32_t step = 1u << (32 - bits);
    int32_t half = (int32_t)(step >> 1);
    uint32_t mask = ~(step - 1);
    int hold = p2 < 128 ? 1 : 1 + (int)((p2 - 128) >> 4);                 /* 1 .. 8 samples */
    int tap = ((r->flags & F_SHORT) || p2 >= 208) ? 6 : 1;

    int32_t at = s->vt * r->gt * AMP_UNIT, an = s->vn * r->gn * AMP_UNIT;
    if (at) tone_block(s, r->flags & 3, inc, at, clean);
    else for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) s_tone[i] = 0;

    uint32_t acc = s->nacc, lf = s->lfsr & 0x7fff;
    /* velocity x the envelope page's ramp (exactly the velocity gain while the page holds) */
    int32_t held = s->held, gv = s->denv.g0, dgv = (s->denv.g1 - gv) >> 6;
    int hc = s->hc > hold ? hold : s->hc;
    fract32 mono[OPZ_BLOCK_FRAMES];                              /* the voice; opz_mix_mono adds it into out */
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        acc += rate;
        uint32_t k = acc >> 16;                                  /* 0..8 register steps this sample */
        acc &= 0xffff;
        uint32_t fb = (lf ^ (lf >> tap)) & ((1u << k) - 1);
        lf = (lf >> k) | (fb << (15 - k));
        int32_t m = -(int32_t)(lf & 1);
        fract32 x = opz_add_q31(s_tone[i], (an ^ m) - m);        /* noise +-an */
        x = (int32_t)((uint32_t)opz_add_q31(x, half) & mask);    /* DAC bits, rounded */
        if (--hc <= 0) {                                          /* DAC rate */
            hc = hold;
            held = x;
        }
        mono[i] = opz_mul_q31(held, gv);
        gv += dgv;
    }
    opz_mix_mono(out, mono, &s->mix, params ? params : k_mix_centre);   /* level and pan, into interleaved out */
    s->nacc = acc;
    s->lfsr = lf;
    s->held = held;
    s->hc = (uint8_t)hc;

    if (++s->fc >= r->frame) {
        s->fc = 0;
        frame_step(s, r);
    }
    if (s->vt <= 0 && s->vn <= 0) {
        s->active = 0;
        OPZ_ADSR_STAGE(env) = 0;                                  /* free the voice */
    }
}

OPZ_EXPORTS_INSTRUMENT();
