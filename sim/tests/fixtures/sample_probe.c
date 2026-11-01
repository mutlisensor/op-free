/* Host check: what getSample serves. On each note the voice asks for the slice of its pack (init's arg2) and, in its
 * first block, adds 13 words into out (wrapping, so the "adds" probe sees them exactly): arg2, frames, knobs, rate,
 * loopStart, loopEnd, pitch, editable, data[0], data[frames - 1], the wrapping sum of the slice's samples, the
 * note, and mem. Then it ends. tests/golden.py checks them against tools/opz_samples.py and zlib.crc32. */
#include "opz_adsr.h"

typedef struct { uint32_t pack, mem, note; const OpzSampleSlice *s; } Voice;

static const ModuleCallbackInterface *s_cb;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void onNoteOff(int n, void *s) { (void)n; (void)s; }

void init(void *state, uint32_t t, uint32_t arg2, void *mem, void *scr)
{
    Voice *v = state;
    (void)t; (void)scr;
    v->pack = arg2;
    v->mem = (uint32_t)(uintptr_t)mem;
    v->s = 0;
}

void onNoteOn(int n, uint8_t vel, void *state, const uint32_t *p)
{
    Voice *v = state;
    (void)vel; (void)p;
    v->note = (uint32_t)n;
    v->s = s_cb->getSample ? s_cb->getSample((int)v->pack, (uint8_t)n) : 0;
}

void render(fract32 *out, uint32_t pitch, int32_t slope, void *state, const uint32_t *p, ADSR *env)
{
    Voice *v = state;
    const OpzSampleSlice *s = v->s;
    (void)pitch; (void)slope; (void)p;
    if (!OPZ_ADSR_STAGE(env)) return;
    uint32_t w[13] = { v->pack, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, v->note, v->mem };
    if (s) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < s->frames; i++) sum += (uint32_t)(int32_t)s->data[i];
        w[1] = s->frames; w[2] = s->knobs; w[3] = s->rate; w[4] = s->loopStart; w[5] = s->loopEnd;
        w[6] = (uint32_t)s->pitch; w[7] = s->editable;
        w[8] = s->frames ? (uint32_t)(int32_t)s->data[0] : 0;
        w[9] = s->frames ? (uint32_t)(int32_t)s->data[s->frames - 1] : 0;
        w[10] = sum;
    }
    for (int i = 0; i < 13; i++) out[i] = (fract32)((uint32_t)out[i] + w[i]);
    OPZ_ADSR_STAGE(env) = 0;
}
