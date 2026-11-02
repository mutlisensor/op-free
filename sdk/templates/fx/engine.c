/*
 * my_engine — an effect for the OP-Z's FX1 and FX2 tracks, made from the SDK's effect template.
 *
 *   P1     drive   1x (0) -> 16x (255) into a soft clipper (1.5x - 0.5x^3)
 *   P2     tone    dark (0) -> open (255): a one-pole low-pass after the clipper
 *   pan    dry / wet, TE's balance law: wet = min(2p, 1), dry = min(2(1 - p), 1); the centre is both at full
 *   level  output level, times the FX track's mixer level and mute (`gain`)
 *
 * The template is the smallest complete effect: per-block parameter maths, a per-sample loop over interleaved
 * stereo, state that carries over between blocks, and the pan/level conventions of TE's effects. Rules:
 * sdk/docs/engine-abi.md (§3 renderFX, §6 rules for engine authors).
 *
 * Ground rules: integers only (Q31 audio, 0x7fffffff = +1.0), opz_mul_q31 / opz_add_q31 in the loop, no division
 * or 64-bit maths per sample, per-track data in `state` (at most OPZ_FX_STATE_BYTES = 200 bytes). An effect that
 * needs more (a delay line) asks for a buffer with *memRequest and gets it as `mem` in init(); see engines/swirl for that pattern.
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"    /* opz_clip, opz_pq31, opz_p16, ... */
#include "opz_engine.h" /* the ABI */

/* ---- per-FX-track state: OPZ_FX_STATE_BYTES (200), one per track the effect runs on ------------------------ */
typedef struct State {
    fract32 lp[2];      /* the low-pass filter's memory, left and right, Q31 */
} State;

_Static_assert(sizeof(State) <= OPZ_FX_STATE_BYTES, "FX state must fit in OPZ_FX_STATE_BYTES (engine-abi.md rule 13)");

static const ModuleCallbackInterface *s_cb;

/* Once at boot. memRequest = bytes of large buffer wanted (capped by the track's pool: FX1 2 MB, FX2 256 KB). */
uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest)
{
    (void)version;
    s_cb = cb;
    *memRequest = 0;
    return 1;
}

/* When an FX track switches to this plug. The OS masks interrupts around this call: keep it short (clear big
 * buffers a piece per block in renderFX instead). `track` is 8 (FX1), 9 (FX2), 10 (tape) or 11 (master). */
void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch)
{
    (void)track; (void)arg2; (void)mem; (void)scratch;
    volatile uint8_t *p = (volatile uint8_t *)state;  /* volatile: stops the compiler turning this into memset() */
    for (unsigned i = 0; i < sizeof(State); i++) p[i] = 0;
}

/* Notes played or sequenced on an FX track arrive here as control input. `params` can be NULL (the OS re-sends a
 * held note as onNoteOn(note, 0, state, NULL)): never read it here. This effect ignores notes. */
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params)
{
    (void)note; (void)velocity; (void)state; (void)params;
}

void onNoteOff(int note, void *state)
{
    (void)note; (void)state;
}

/* 64 frames, interleaved stereo: `in` and `out` are separate buffers of 128 values (L, R, L, R, ...). Write all
 * 128 values of `out`; whatever is not written is silence, not the input. On the master track `out` is NULL: this
 * plug's trackmask leaves master out (engine-abi.md §3, routing.md). */
void renderFX(const uint32_t *params, fract32 pitchBend, void *state, const fract32 *in, fract32 *out, fract32 gain)
{
    (void)pitchBend;
    State *s = state;

    /* ---- per block: knobs -> coefficients ---- */
    /* drive gain 1x .. 16x, stored as gain / 16 in Q31 (1x = 1 << 27); the loop multiplies, then shifts left 4 */
    fract32 drive = (1 << 27) + 15 * (opz_p16(params[OPZ_P_P1]) << 11);
    /* low-pass coefficient a in Q31: p^2 from 0.4 % (dark) to 1.0 (no filtering), lp += a (x - lp) */
    fract32 p2 = opz_pq31(params[OPZ_P_P2]);
    fract32 a = opz_mul_q31(p2, p2);
    if (a < 0x00800000) a = 0x00800000;
    /* dry/wet from pan, TE's law; output level from level x gain */
    fract32 pan = opz_pq31(params[OPZ_P_PAN]);
    fract32 wet = opz_shl_q31(pan, 1);                       /* min(2p, 1): the shift saturates */
    fract32 dry = opz_shl_q31(0x7fffffff - pan, 1);          /* min(2(1 - p), 1) */
    fract32 out_gain = opz_mul_q31(opz_pq31(params[OPZ_P_LEVEL]), gain);

    /* ---- per sample ---- */
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        for (int c = 0; c < 2; c++) {                         /* c = 0 left, 1 right */
            fract32 x = in[2 * i + c];
            fract32 y = opz_clip(opz_shl_q31(opz_mul_q31(x, drive), 4));   /* x * gain, saturated, soft-clipped */
            s->lp[c] = opz_add_q31(s->lp[c], opz_mul_q31(a, opz_sub_q31(y, s->lp[c])));
            fract32 mix = opz_add_q31(opz_mul_q31(dry, x), opz_mul_q31(wet, s->lp[c]));
            out[2 * i + c] = opz_mul_q31(mix, out_gain);
        }
    }
}

OPZ_EXPORTS_FX();
