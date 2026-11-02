/* Host check: an instrument that overwrites the shared track buffer instead of adding. opzsim must exit 4. */
#include "opz_engine.h"
static const ModuleCallbackInterface *s_cb;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)s; (void)t; (void)a; (void)mem; (void)scr; }
void onNoteOn(int n, uint8_t v, void *s, const uint32_t *p) { (void)n; (void)v; (void)s; (void)p; }
void onNoteOff(int n, void *s) { (void)n; (void)s; }
void render(fract32 *out, uint32_t pitch, int32_t slope, void *s, const uint32_t *p, ADSR *env)
{
    (void)pitch; (void)slope; (void)s;
    if (!OPZ_ADSR_STAGE(env)) return;
    s_cb->updateParametersADSR(env, p[2], p[3], p[4], p[5]);
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = 0x10000000;
}
