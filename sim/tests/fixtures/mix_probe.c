/* Host check for opz_mix.h: every voice is a constant 0x20000000 (-12 dBFS) through opz_mix_mono, so the track
 * buffer shows the level/pan gains block by block and the per-sample ramps between them. tests/golden.py compares it
 * with a model of TE's law written from sdk/docs/engine-abi.md §4. */
#include "opz_engine.h"
#include "opz_mix.h"
typedef struct State { OpzMix mix; } State;
static const ModuleCallbackInterface *s_cb;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)s; (void)t; (void)a; (void)mem; (void)scr; }
void onNoteOn(int n, uint8_t v, void *s, const uint32_t *p) { (void)n; (void)v; opz_mix_start(&((State *)s)->mix, p); }
void onNoteOff(int n, void *s) { (void)n; (void)s; }
void render(fract32 *out, uint32_t pitch, int32_t slope, void *s, const uint32_t *p, ADSR *env)
{
    (void)pitch; (void)slope;
    if (!OPZ_ADSR_STAGE(env)) return;
    s_cb->updateParametersADSR(env, p[2], p[3], p[4], p[5]);
    fract32 mono[OPZ_BLOCK_FRAMES];
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) mono[i] = 0x20000000;
    opz_mix_mono(out, mono, &((State *)s)->mix, p);
}
