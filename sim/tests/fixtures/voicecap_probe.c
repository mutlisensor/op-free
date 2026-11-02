/* Host check: opz_voices.h with a cap of 2 on the chord track (4 voice records). Each voice follows the host ADSR.
 * Left channel: the sum of the voices' envelope levels >> 3. Right channel: per frame, 1 << 24 for each voice that is
 * sounding and 1 << 16 for each voice in the host fade (stage 5/6) after the envelope update. (The stage, not
 * opz_cap_render's answer: opzsim renders every block twice to check that render adds, and restores only the voice
 * state, so a value that depends on the shared cap table could differ between the two calls.) tests/golden.py checks that never more than
 * two sound, that evicted voices fade without a jump and that the host frees them. */
#include "opz_adsr.h"
#include "opz_voices.h"

static const ModuleCallbackInterface *s_cb;
static OpzVoiceCap s_cap = OPZ_VOICE_CAP_INIT(2);

uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)t; (void)a; (void)mem; (void)scr; opz_cap_forget(&s_cap, s); }
void onNoteOn(int n, uint8_t vel, void *state, const uint32_t *p) { (void)n; (void)vel; (void)p; opz_cap_admit(&s_cap, state); }
void onNoteOff(int n, void *s) { (void)n; (void)s; }

void render(fract32 *out, uint32_t pitch, int32_t slope, void *state, const uint32_t *p, ADSR *env)
{
    (void)pitch; (void)slope;
    opz_cap_render(&s_cap, state, env);
    s_cb->updateParametersADSR(env, p[OPZ_P_ATTACK], p[OPZ_P_DECAY], p[OPZ_P_SUSTAIN], p[OPZ_P_RELEASE]);
    int32_t stage = OPZ_ADSR_STAGE(env);
    int fading = stage == OPZ_ENV_STAGE_FADE || stage == OPZ_ENV_STAGE_FADE + 1;
    fract32 mark = !stage ? 0 : fading ? 1 << 16 : 1 << 24;          /* stage 0: the voice ended this block */
    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        level = opz_env_tick(level, coef, step);
        out[2 * i] = opz_add_q31(out[2 * i], level >> 3);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], mark);
    }
    OPZ_ADSR_LEVEL(env) = level;
    if (!OPZ_ADSR_STAGE(env)) opz_cap_forget(&s_cap, state);
}

OPZ_EXPORTS_INSTRUMENT();
