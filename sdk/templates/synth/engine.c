/*
 * my_engine — a synth for the OP-Z's synth tracks (bass, lead, arp, chord), made from the SDK's synth template.
 *
 *   P1  shape   sine (0) -> band-limited saw (255), crossfaded
 *   P2  sub     a triangle one octave below, off (0) -> as loud as the main oscillator (255)
 *
 * The template is the smallest complete instrument: one oscillator, the host's ADSR envelope, velocity, pitch
 * glide inside the block, the track's level and pan, and the "add into out" rule. Replace the sound, keep the skeleton. Every rule it follows
 * is in sdk/docs/engine-abi.md (§3 entry points, §6 rules for engine authors).
 *
 * Ground rules for everything in this file (the OP-Z's Blackfin has no FPU and no libc):
 *   - integers only: audio and gains are Q31 (fract32, 0x7fffffff = +1.0), phases are uint32_t turns (Q32);
 *   - multiply and add audio with opz_mul_q31 / opz_add_q31: one saturating Blackfin instruction each, where a plain
 *     (int64_t)a * b becomes a slow libgcc call;
 *   - no division, no 64-bit maths and no table bigger than you need in the per-sample loop;
 *   - no globals per voice: one copy of this code serves every voice of every track, so per-voice data lives in
 *     `state` (at most OPZ_VOICE_STATE_BYTES = 280 bytes).
 *
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_adsr.h"   /* the host envelope: OPZ_ADSR_* accessors and opz_env_tick */
#include "opz_dsp.h"    /* shared fixed-point kernels: opz_sine, opz_saw_blep, opz_lerp, ... */
#include "opz_engine.h" /* the ABI: types, callback table, parameter indices, export table */
#include "opz_mix.h"    /* the track's level and pan dials: the OS leaves them to the engine */

/* ---- per-voice state ------------------------------------------------------------------------------------------
 * The host gives every voice 280 bytes for this struct (bytes 280-307 of the area are the OS's own). It does NOT
 * clear them between notes on the same plug, so init() clears them and onNoteOn() sets what a note needs. */
typedef struct State {
    uint32_t phase;      /* main oscillator, Q32 turns */
    uint32_t sub_phase;  /* sub oscillator, one octave down */
    fract32 amp;         /* velocity x OPZ_VOICE_PEAK, Q31, set at note-on */
    OpzMix mix;          /* level and pan gains of the last block, for opz_mix_mono's ramp */
} State;

_Static_assert(sizeof(State) <= OPZ_VOICE_STATE_BYTES, "voice state must fit in OPZ_VOICE_STATE_BYTES (engine-abi.md rule 13)");

/* The one legitimate global: the host's callback table, shared by every voice. */
static const ModuleCallbackInterface *s_cb;

/* ---- entry points ---------------------------------------------------------------------------------------------- */

/* Once, when the OP-Z loads the engine at boot. Keep the callback pointer; instruments ask for no buffer. */
uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;           /* non-zero: loaded. 0 would make the OS report "Failed to initialize module" */
}

/* When a voice first plays a note on this plug. Clear the whole state: it is full of the last plug's bytes. */
void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;  /* volatile: stops the compiler turning this into memset() */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

/* A note starts on this voice. The host has already reset the envelope to its attack. Don't latch a frequency
 * here: with the legato note style new notes only move `pitch` and never reach onNoteOn, so render() reads the
 * pitch every block. render() may also run before this returns (rule 18): every field written here must be safe
 * to render on its own. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note;
    State *s = state;
    fract32 vel = (fract32)((uint32_t)(velocity > 127 ? 127 : velocity) * 0x01020408u);  /* v / 127 in Q31 */
    s->amp = opz_mul_q31(vel, OPZ_VOICE_PEAK);  /* -12 dBFS at full velocity: the level TE's synths play at */
    s->phase = 0;
    s->sub_phase = 0;
    opz_mix_start(&s->mix, params);             /* the note starts at the dials' level and pan */
}

/* The key was released. The host moves the envelope to its release stage by itself, and this may be called
 * twice, from the audio interrupt (rule 20): nothing to do here. */
void onNoteOff(int note, void *state)
{
    (void)note; (void)state;
}

/* 64 frames for one voice. `out` is the track's buffer, interleaved stereo (L, R, L, R, ... 128 values) that every
 * voice of the track ADDS into: never overwrite it. `pitch` is the phase increment per sample (Q32) at the block's
 * start; add `pitchSlope` every sample to glide to the next block's pitch. `params` are the 18 smoothed parameters
 * (Q31, 0..0x7fffffff): this engine owns P1 and P2; filter and resonance are the OS's own track filter. Level and
 * pan are the engine's job (every TE engine applies them in render; the FX sends are taken after them):
 * opz_mix_mono does it at the end of the block. */
void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env)
{
    State *s = state;

    /* 1. Stage 0 means the voice is idle: do nothing. */
    if (OPZ_ADSR_STAGE(env) == 0) return;

    /* 2. Let the host move the envelope on (attack, decay, sustain, release, fade) with this block's knobs. */
    s_cb->updateParametersADSR(env, params[OPZ_P_ATTACK], params[OPZ_P_DECAY], params[OPZ_P_SUSTAIN],
                               params[OPZ_P_RELEASE]);

    /* 3. Everything that depends only on the knobs and the pitch, once per block. */
    fract32 shape = opz_pq31(params[OPZ_P_P1]);            /* 0 sine .. 1 saw */
    fract32 sub_level = opz_pq31(params[OPZ_P_P2]);        /* 0 .. 1 */
    uint32_t recip = opz_blep_recip(pitch);                /* for the saw's anti-aliasing, no division per sample */
    fract32 amp = s->amp << 1;                              /* velocity, doubled to undo the halving in the loop */

    /* 4. The envelope's level and per-sample coefficients live in locals for the loop, and go back at the end.
     *    Locals, not s->...: the compiler must assume a write to out[] could change *s, and would reload it. */
    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    uint32_t phase = s->phase, sub_phase = s->sub_phase, inc = pitch;
    fract32 mono[OPZ_BLOCK_FRAMES];                         /* the voice, before level and pan */

    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        /* the sound: main oscillator (sine -> saw), plus the sub. Both halved, so the sum never clips. */
        fract32 x = opz_lerp(opz_sine(phase), opz_saw_blep(phase, inc, recip), shape);
        fract32 sub = opz_mul_q31(opz_tri(sub_phase), sub_level);   /* a triangle: cheaper than a sine */
        fract32 y = (x >> 1) + (sub >> 1);
        /* level: velocity and the envelope */
        y = opz_mul_q31(opz_mul_q31(y, amp), level);
        mono[i] = y;
        /* advance: envelope, phases (wrapping is what we want), pitch glide */
        level = opz_env_tick(level, coef, step);
        phase += inc;
        sub_phase += inc >> 1;
        inc += (uint32_t)pitchSlope;
    }

    /* 5. The track's level and pan, ADDED into both channels of the interleaved buffer, saturating. */
    opz_mix_mono(out, mono, &s->mix, params);

    /* 6. Store what the next block continues from. The host frees the voice once the envelope reaches stage 0. */
    OPZ_ADSR_LEVEL(env) = level;
    s->phase = phase;
    s->sub_phase = sub_phase;
}

/* The export table the OS looks up by name: initModule, init, onNoteOn, onNoteOff and render. Last line, always. */
OPZ_EXPORTS_INSTRUMENT();
