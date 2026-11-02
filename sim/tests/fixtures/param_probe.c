/* Host check: writes the smoothed P1 word (>> 1) and the pitch word (>> 1) into the track buffer, so the
 * test can compare the host's parameter conversion, smoothing and pitch against sdk/docs/engine-abi.md. */
#include "opz_engine.h"
static const ModuleCallbackInterface *s_cb;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)s; (void)t; (void)a; (void)mem; (void)scr; }
void onNoteOn(int n, uint8_t v, void *s, const uint32_t *p) { (void)n; (void)v; (void)s; (void)p; }
void onNoteOff(int n, void *s) { (void)n; (void)s; }
void render(fract32 *out, uint32_t pitch, int32_t slope, void *s, const uint32_t *p, ADSR *env)
{
    (void)slope; (void)s;
    if (!OPZ_ADSR_STAGE(env)) return;
    s_cb->updateParametersADSR(env, p[2], p[3], p[4], p[5]);
    /* out is interleaved stereo: frame 0 (L, R) carries P1, frame 1 (L, R) carries the pitch word */
    out[0] = opz_add_q31(out[0], (fract32)(p[OPZ_P_P1] >> 1));
    out[1] = opz_add_q31(out[1], (fract32)(p[OPZ_P_P1] >> 1));
    out[2] = opz_add_q31(out[2], (fract32)(pitch >> 1));
    out[3] = opz_add_q31(out[3], (fract32)(pitch >> 1));
}
