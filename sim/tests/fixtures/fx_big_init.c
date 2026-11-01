/* Host check: an effect that asks for 64 KB and clears it all in init. On master the request is capped at the
 * 16 KB pool, and clearing it with interrupts masked must be flagged. */
#include "opz_engine.h"
static uint32_t s_bytes;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m)
{
    (void)v; (void)cb;
    *m = s_bytes = 0x10000;
    return 1;
}
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr)
{
    (void)s; (void)a; (void)scr;
    uint32_t n = t == OPZ_TRACK_MASTER ? 0x4000 : s_bytes; /* stay inside the capped buffer */
    volatile uint8_t *p = mem;
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}
void onNoteOn(int n, uint8_t v, void *s, const uint32_t *p) { (void)n; (void)v; (void)s; (void)p; }
void onNoteOff(int n, void *s) { (void)n; (void)s; }
void renderFX(const uint32_t *p, fract32 b, void *s, const fract32 *in, fract32 *out, fract32 g)
{
    (void)p; (void)b; (void)s; (void)g;
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = in[i];
}
