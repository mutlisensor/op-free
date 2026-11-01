/*
 * repeat — beat repeat / stutter for FX1 and FX2, synced to the sequencer's tempo and played from the FX track's keys.
 *
 *   P1     division   0..31 off (the input passes through); then 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/64 (32 each).
 *                     Turning P1 out of "off" starts a repeat; moving it while repeating changes the length at the
 *                     next repeat (no new capture), so a sweep rolls 1/4 -> 1/64 over the same slice
 *   P2     decay      level of each repeat against the one before: 0 = all at full level, 128 = -3 dB, 255 = -12 dB
 *                     (-12 u^2 dB per repeat)
 *   pan    dry / wet  TE's balance law normalised to unity (centre = half and half); the preset is wet only, an
 *                     insert: the stutter replaces the sound
 *   level  output level (x gain)
 *   notes  on the FX track: a held key repeats for as long as it is held, whatever P1 says, and every new note
 *          starts a new capture. The 24 keys from F3 pick the division, long (left) to short (right): 1/4, 1/4T,
 *          1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T, 1/64, 1/64T, 1/128, 1/128T; the upper row (F4..E5) plays the same
 *          divisions in reverse. Other octaves repeat the pattern
 *
 * How it repeats: forward capture. A repeat records the input from the moment it starts and plays it as it goes, so
 * the first pass is the live sound (no delay, no click); at the end of the slice it loops what it recorded (reversed
 * on the upper keys). Each slice is gated with a short linear fade at both ends (32..128 frames by length), the
 * classic stutter, and the first pass has no fade-in. Stopping or re-triggering crossfades from the repeat to the live
 * input over 64 frames (a new capture starts after it). Nothing reads a frame this capture has not written, so the
 * buffer needs no clearing.
 *
 * Tempo: sequencer key 4 (whole BPM, 40..200) every block, as TE's delay does (Q-18 [code]; hardware check H-7 not run
 * yet). Without it (another host, an out-of-range answer) repeat runs at 120 BPM. A slice is 10,584,000 / (den x BPM)
 * frames, capped at the buffer (61,440 frames: a quarter note below 43 BPM is shortened). Repeats start when the
 * note or the knob says, not on the grid: sequenced notes land on steps anyway.
 *
 * The filter and resonance knobs are the OS's own track filter; pitch bend is not used. Q31 signals, 16-bit buffer,
 * no floating point, no libc, no 64-bit maths or division per sample (one 32-bit division when the tempo or the
 * division changes). Clean-room design; MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_tempo.h"

#define BUF_FRAMES      61440u                  /* stereo int16 frames, packed L | R << 16 in one word */
#define MEM_BYTES       (BUF_FRAMES * 4u)       /* 240 KB: fits FX2's 256 KB pool */
#define XF_STEP         512                     /* live <-> repeat crossfade: 32768 / 64 frames */
#define NDIV            12

/* Divisions as the denominator of a whole note: 1/4 .. 1/128 with the triplet of each after it */
static const uint8_t k_den[NDIV] = { 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192 };
/* P1 zones (byte >> 5): off, 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/64 as indices into k_den */
static const int8_t k_p1_div[8] = { -1, 0, 2, 3, 4, 5, 6, 8 };

typedef struct State {
    uint32_t *buf;                              /* this FX track's host buffer */
    uint32_t pos;                               /* play position in the slice, frames */
    uint32_t filled;                            /* frames recorded since the capture started */
    uint32_t len;                               /* slice length of this pass, frames (<= filled after the first) */
    uint32_t target;                            /* slice length the current division asks for */
    uint32_t len_key;                           /* (den << 16) | bpm that target was computed for */
    int32_t lap_gain;                           /* level of this pass, Q31 */
    int32_t mix;                                /* 0 live .. 32768 repeat, Q15 */
    int32_t wet, dry;                           /* normalised dry/wet gains, Q31 */
    uint32_t pan;                               /* the pan word they were computed for (bit 31: none yet) */
    uint16_t laps;                              /* passes completed (saturates) */
    uint8_t capturing;                          /* a repeat is running */
    uint8_t leaving;                            /* fading to live: 1 stop, 2 then start a new capture */
    uint8_t rev;                                /* this pass plays reversed */
    uint8_t div;                                /* division of the current source, index into k_den */
    uint8_t want_rev;                           /* the source asks for reverse (applied at the next pass) */
    /* written by onNoteOn/onNoteOff (single bytes: safe if render runs in the middle) */
    uint8_t note_held;
    uint8_t note;
    uint8_t note_div;
    uint8_t note_rev;
    uint8_t trig;                               /* incremented by every new note */
    uint8_t trig_seen;
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
    /* FX1 and FX2 pools hold the buffer; anywhere else (master: 16 KB) repeat passes the input through */
    s->buf = (track == OPZ_TRACK_FX1 || track == OPZ_TRACK_FX2) ? (uint32_t *)mem : (uint32_t *)0;
    s->pan = 0x80000000u;
    s->lap_gain = OPZ_Q31_ONE;
}

/* params may be NULL (the OS re-sends a held note with velocity 0): never read it. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)params;
    State *s = state;
    if (note < 0 || note > 127) return;                       /* 255: plug selected, no note */
    int k = (note + 19) % 24;                                  /* F3 (53) = key 0, as the OS numbers the keys */
    s->note_div = (uint8_t)(k < NDIV ? k : k - NDIV);
    s->note_rev = (uint8_t)(k >= NDIV);
    s->note = (uint8_t)note;
    s->note_held = 1;
    if (velocity) s->trig++;                                   /* a re-sent held note keeps its capture */
}

void onNoteOff(int note, void *state)
{
    State *s = state;
    if (note == s->note) s->note_held = 0;
}

static inline uint32_t pack(int32_t l, int32_t r) { return ((uint32_t)l >> 16) | ((uint32_t)r & 0xffff0000u); }

/* Fade length for a slice: 128 frames, shorter for short slices (at most 1/8 of it) */
static inline int fade_shift(uint32_t len) { return len >= 1024 ? 7 : len >= 512 ? 6 : 5; }

static void start_capture(State *s)
{
    s->pos = 0;
    s->filled = 0;
    s->laps = 0;
    s->len = s->target;
    s->lap_gain = OPZ_Q31_ONE;
    s->rev = 0;                                                /* the first pass is live, forwards */
    s->mix = 32768;
    s->capturing = 1;
    s->leaving = 0;
}

void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block --------------------------------------------------------------------------------------- */
    int zone = (int)((params[OPZ_P_P1] & OPZ_Q31_ONE) >> 28);
    int held = s->note_held;
    int div = held ? s->note_div : k_p1_div[zone];
    int want = held || zone > 0;
    if (div >= 0) {
        s->div = (uint8_t)div;
        s->want_rev = held ? s->note_rev : 0;
    }
    int bpm = s_cb ? opz_seq_bpm(s_cb) : OPZ_BPM_DEFAULT;
    uint32_t key = ((uint32_t)k_den[s->div] << 16) | (uint32_t)bpm;
    if (key != s->len_key) {                                   /* a 32-bit division, only on a change */
        uint32_t len = 10584000u / ((uint32_t)k_den[s->div] * (uint32_t)bpm);
        s->target = len > BUF_FRAMES ? BUF_FRAMES : len;
        s->len_key = key;
    }
    uint8_t trig = s->trig;
    int retrigger = trig != s->trig_seen;
    s->trig_seen = trig;

    if (!s->buf) want = 0;
    if (s->capturing && !s->leaving) {
        if (!want) s->leaving = 1;
        else if (retrigger) s->leaving = 2;
    } else if (s->capturing && s->leaving == 1 && want) {
        s->leaving = 2;                                        /* back on while fading out: a new capture */
    } else if (!s->capturing && want) {
        start_capture(s);
    }
    if (s->capturing && !s->leaving && s->laps == 0) s->len = s->target;   /* the first pass follows the knob */

    /* P2: per-pass gain 2^(-2 u^2), 0 .. -12 dB, Q31 */
    uint32_t u = (uint32_t)opz_p16(params[OPZ_P_P2]);
    int32_t g16 = opz_exp2_q16(-(int32_t)((opz_mul_u16(u, u) >> 16) << 1));
    int32_t decay = g16 >= 65536 ? OPZ_Q31_ONE : g16 << 15;

    uint32_t pan = params[OPZ_P_PAN] & OPZ_Q31_ONE;
    if (pan != s->pan) {                                       /* TE's law, then / (dry + wet): no division */
        int32_t w = opz_shl_q31((int32_t)pan, 1), d = opz_shl_q31(OPZ_Q31_ONE - (int32_t)pan, 1);
        uint32_t half = ((uint32_t)w + (uint32_t)d) >> 1;      /* (dry + wet) / 2: 0.5 .. 1 */
        fract32 h = opz_recip_q31(half > OPZ_Q31_ONE ? OPZ_Q31_ONE : (int32_t)half);
        s->wet = opz_mul_q31(w, h);
        s->dry = opz_mul_q31(d, h);
        s->pan = pan;
    }
    int32_t wet = s->wet, dry = s->dry;
    int32_t out_gain = opz_mul_q31((int32_t)(params[OPZ_P_LEVEL] & OPZ_Q31_ONE), gain);

    /* ---- per sample -------------------------------------------------------------------------------------- */
    uint32_t *buf = s->buf;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int32_t xl = in[2 * i], xr = in[2 * i + 1];
        int32_t yl = xl, yr = xr;
        if (s->capturing) {
            uint32_t pos = s->pos, len = s->len;
            int32_t rl, rr;
            if (pos >= s->filled) {                            /* the first pass: record and play live */
                buf[pos] = pack(xl, xr);
                s->filled = pos + 1;
                rl = xl;
                rr = xr;
            } else {
                uint32_t w = buf[s->rev ? len - 1 - pos : pos];
                rl = opz_mul_q31((int32_t)(w << 16), s->lap_gain);
                rr = opz_mul_q31((int32_t)(w & 0xffff0000u), s->lap_gain);
            }
            /* the gate: a linear fade at the end of every pass and at the start of every pass but the first */
            int sh = fade_shift(len);
            uint32_t edge = len - pos;
            if (s->laps && pos < edge) edge = pos + 1;
            if (edge < (1u << sh)) {
                int32_t g = (int32_t)(edge << (15 - sh));
                rl = opz_mul_q31_q15(rl, g);
                rr = opz_mul_q31_q15(rr, g);
            }
            /* live <-> repeat */
            if (s->leaving) {
                s->mix -= XF_STEP;
                if (s->mix < 0) s->mix = 0;
            }
            int32_t m = s->mix;
            if (m < 32768) {
                rl = opz_add_q31(xl, opz_mul_q31_q15(opz_sub_q31(rl, xl), m));
                rr = opz_add_q31(xr, opz_mul_q31_q15(opz_sub_q31(rr, xr), m));
            }
            yl = rl;
            yr = rr;
            if (++pos >= len) {                                /* end of a pass: loop what was recorded */
                pos = 0;
                if (s->laps < 0xffff) s->laps++;
                s->lap_gain = opz_mul_q31(s->lap_gain, decay);
                s->rev = s->want_rev;
                uint32_t t = s->target;
                s->len = t < s->filled ? t : s->filled;
            }
            s->pos = pos;
            if (s->leaving && s->mix == 0) {
                if (s->leaving == 2) start_capture(s);
                else s->capturing = 0;
            }
        }
        int32_t ml = opz_add_q31(opz_mul_q31(dry, xl), opz_mul_q31(wet, yl));
        int32_t mr = opz_add_q31(opz_mul_q31(dry, xr), opz_mul_q31(wet, yr));
        out[2 * i] = opz_mul_q31(ml, out_gain);
        out[2 * i + 1] = opz_mul_q31(mr, out_gain);
    }
}

OPZ_EXPORTS_FX();
