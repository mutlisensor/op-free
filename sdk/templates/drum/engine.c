/*
 * my_engine — a drum voice for the OP-Z's drum tracks (kick, snare, perc, sample), made from the SDK's drum
 * template.
 *
 *   keys  the 24 keys play 24 pitches, C2 (first key) to B3 (last); each octave press moves them 2 octaves
 *   P1    tune    -12 .. +12 semitones (128 = as the key says)
 *   P2    snap    the hit's colour: a sine with a falling pitch (0) -> noise (255), crossfaded
 *   attack, decay, sustain, release   the drum envelope of TE's drum tracks: start, attack, hold, decay
 *
 * Drum tracks differ from synth tracks in two ways, and this template shows both (engine-abi.md §3):
 *   - `pitch` is not a note frequency there but a fixed unity rate (1024 at standard tuning): the sound's pitch
 *     comes from onNoteOn's note number, here key = note - 53 (the 24 keys send MIDI 53..76, octave presses +-24);
 *   - the envelope is the host's drum envelope (updateParametersAHD), which runs on positions along a sound of a
 *     fixed length, not on knob words. The voice is freed when the position passes the decay's end.
 *
 * Same ground rules as every engine: integers only (Q31 audio, Q32 phases), opz_mul_q31 / opz_add_q31 in the
 * loop, no division or 64-bit maths per sample, per-voice data in `state` (at most OPZ_VOICE_STATE_BYTES = 280
 * bytes). MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"   /* OPZ_ADSR_* accessors, opz_ahd_layout, opz_ahd_tick */
#include "opz_dsp.h"    /* opz_sine, opz_noise, opz_exp2_q16, opz_hz_to_inc, ... */
#include "opz_engine.h" /* the ABI */

#define LENGTH      22050       /* the sound is 0.5 s long: the drum envelope's knobs cut and shape it */
#define SWEEP_DROP  0x7c000000  /* per block, the pitch sweep keeps 0.97 of itself (~33 ms to fall to a third) */
#define INC_MAX     0x1fffffffu /* ~5.5 kHz: x4 for the sweep still fits below 2^31 */

typedef struct State {
    OpzAhd ahd;         /* envelope landmarks for this hit (positions in frames) */
    uint32_t pos;       /* how far into the sound this voice has played, frames */
    uint32_t phase;     /* sine, Q32 turns */
    uint32_t inc;       /* the key's increment before tuning, Q32 turns per sample */
    fract32 sweep;      /* pitch sweep: 1.0 at the hit, falling towards 0; adds up to 3x the pitch */
    uint32_t seed;      /* noise generator */
    fract32 amp;        /* velocity x OPZ_VOICE_PEAK */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in OPZ_VOICE_STATE_BYTES (engine-abi.md rule 13)");

static const ModuleCallbackInterface *s_cb;

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
    volatile uint8_t *p = (volatile uint8_t *)state;  /* volatile: stops the compiler turning this into memset() */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
    State *s = state;
    s->seed = 0x2545f491u + 7919u * track;            /* a different noise per track */
}

/* A hit. Work into locals and store them last: render() may run before this returns (rule 18), so each stored
 * field must be safe on its own. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    State *s = state;
    int n = note - 53 + 36;                          /* first key (MIDI 53) -> C2 (36) */
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    /* frequency of note n: 440 * 2^((n - 69) / 12). 5461 ~ 65536 / 12 keeps it in 32-bit maths (no libgcc). */
    uint32_t inc = opz_hz_to_inc(440 * opz_exp2_q16((n - 69) * 5461));
    OpzAhd ahd;
    opz_ahd_layout(&ahd, LENGTH, params);            /* attack/decay/sustain/release knobs -> landmarks */
    fract32 vel = (fract32)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u);

    s->inc = inc > INC_MAX ? INC_MAX : inc;
    s->amp = opz_mul_q31(vel, OPZ_VOICE_PEAK);
    s->phase = 0;
    s->sweep = 0x7fffffff;
    s->ahd = ahd;
    s->pos = ahd.start;                              /* the start knob skips into the sound */
}

/* Drums are one-shots: the drum envelope ignores note-off. */
void onNoteOff(int note, void *state)
{
    (void)note; (void)state;
}

void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    (void)pitch; (void)pitchSlope;                   /* drum tracks: the pitch comes from the note (onNoteOn) */
    State *s = state;
    if (OPZ_ADSR_STAGE(env) == 0) return;

    /* The host moves the drum envelope on from our position; past decay_end it sets stage 0 (voice done). */
    s_cb->updateParametersAHD(env, s->pos, s->ahd.attack_end, s->ahd.hold_end, s->ahd.decay_end, s->ahd.coef,
                              s->ahd.shape);
    if (OPZ_ADSR_STAGE(env) == 0) return;

    /* per block: tuning (P1, +-1 octave as a Q16 exponent), the sweep, the tone/noise balance (P2) */
    int32_t tune_q16 = (opz_p16(params[OPZ_P_P1]) - 32768) * 2;           /* -1 .. +1 octave, Q16 */
    uint32_t inc = opz_mul_u32_shr16(s->inc, (uint32_t)opz_exp2_q16(tune_q16));
    if (inc > INC_MAX) inc = INC_MAX;
    inc += 3 * (uint32_t)opz_mul_q31((fract32)inc, s->sweep);                 /* up to 4x at the hit */
    s->sweep = opz_mul_q31(s->sweep, SWEEP_DROP);
    fract32 snap = opz_pq31(params[OPZ_P_P2]);

    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    uint32_t phase = s->phase, seed = s->seed;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        fract32 x = opz_lerp(opz_sine(phase), opz_noise(&seed), snap);
        x = opz_mul_q31(opz_mul_q31(x, s->amp), level);
        out[2 * i] = opz_add_q31(out[2 * i], x);     /* ADD into the interleaved stereo buffer */
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], x);
        level = opz_ahd_tick(level, coef, step);     /* the drum envelope's linear step (not opz_env_tick) */
        phase += inc;
    }
    OPZ_ADSR_LEVEL(env) = level;
    s->phase = phase;
    s->seed = seed;
    s->pos += OPZ_BLOCK_FRAMES;
}

OPZ_EXPORTS_INSTRUMENT();
