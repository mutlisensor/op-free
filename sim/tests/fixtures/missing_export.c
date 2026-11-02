/* Host check: no onNoteOff export. opzsim must refuse it (exit 2), like the OS loader ("Could not lookup"). */
#include "opz_engine.h"
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; (void)cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)s; (void)t; (void)a; (void)mem; (void)scr; }
void onNoteOn(int n, uint8_t v, void *s, const uint32_t *p) { (void)n; (void)v; (void)s; (void)p; }
void renderFX(const uint32_t *p, fract32 b, void *s, const fract32 *l, fract32 *r, fract32 g)
{
    (void)p; (void)b; (void)s; (void)l; (void)r; (void)g;
}
