/* Host check: the sequencer clock (opzsim's keys 4-7) and opz_tempo.h. Per block, left channel: the raw keys
 * (step, tick, playing, BPM) in frames 0-3, then OpzClock's pos, jumped and beat phase in frames 4-6. Right
 * channel, frames 0-6: the tempo helpers at the current BPM. Everything else 0. tests/golden.py checks both
 * against its own model of sdk/docs/engine-abi.md ("Sequencer keys"). */
#include "opz_tempo.h"

typedef struct { OpzClock clock; } State;

static const ModuleCallbackInterface *s_cb;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr)
{
    (void)t; (void)a; (void)mem; (void)scr;
    opz_clock_init(&((State *)s)->clock);
}
void onNoteOn(int n, uint8_t v, void *s, const uint32_t *p) { (void)n; (void)v; (void)s; (void)p; }
void onNoteOff(int n, void *s) { (void)n; (void)s; }

void renderFX(const uint32_t *p, fract32 b, void *state, const fract32 *in, fract32 *out, fract32 g)
{
    (void)p; (void)b; (void)in; (void)g;
    State *s = state;
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = 0;
    opz_clock_block(&s->clock, s_cb);
    int bpm = opz_seq_bpm(s_cb);
    int32_t l[7] = { s_cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_STEP, 0)),
                     s_cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_TICK, 0)),
                     s_cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_PLAYING, 0)), bpm,
                     (int32_t)s->clock.pos, s->clock.jumped, (int32_t)(opz_clock_phase(&s->clock, 2) >> 1) };
    int32_t r[7] = { (int32_t)opz_samples_per_beat(bpm), (int32_t)opz_samples_per_step(bpm),
                     (int32_t)opz_samples_per_note(bpm, 3, 16), (int32_t)opz_sync_inc(bpm, 1, 4),
                     (int32_t)opz_steps_per_block_q20(bpm), (int32_t)opz_tempo_pos(-1, 23),
                     (int32_t)opz_clock_phase_inc(&s->clock, 2) };
    for (int i = 0; i < 7; i++) {
        out[2 * i] = l[i];
        out[2 * i + 1] = r[i];
    }
}

OPZ_EXPORTS_FX();
