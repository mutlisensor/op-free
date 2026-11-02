/* Host check: a drum voice on the decoded drum envelope (opz_ahd_layout + updateParametersAHD + opz_ahd_tick).
 * Left channel: the envelope level >> 1 every frame. Right channel, first block: params[2..5] at note-on in frames
 * 0-3 and the layout (start, attack_end, hold_end, decay_end, coef, shape) in frames 4-9 (all below 2^31); after
 * that, the stage seen after each updateParametersAHD call. tests/golden.py checks it against its own model. */
#include "opz_adsr.h"

#define LEN 8192

typedef struct { OpzAhd ahd; uint32_t pos; uint32_t p[4]; int first; } Voice;

static const ModuleCallbackInterface *s_cb;
uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)s; (void)t; (void)a; (void)mem; (void)scr; }
void onNoteOff(int n, void *s) { (void)n; (void)s; }

void onNoteOn(int n, uint8_t vel, void *state, const uint32_t *p)
{
    Voice *v = state;
    (void)n; (void)vel;
    opz_ahd_layout(&v->ahd, LEN, p);
    v->pos = v->ahd.start;
    for (int i = 0; i < 4; i++) v->p[i] = p[OPZ_P_ATTACK + i];
    v->first = 1;
}

void render(fract32 *out, uint32_t pitch, int32_t slope, void *state, const uint32_t *p, ADSR *env)
{
    Voice *v = state;
    (void)pitch; (void)slope; (void)p;
    if (!OPZ_ADSR_STAGE(env)) return;
    s_cb->updateParametersAHD(env, v->pos, v->ahd.attack_end, v->ahd.hold_end, v->ahd.decay_end, v->ahd.coef,
                              v->ahd.shape);
    int32_t stage = OPZ_ADSR_STAGE(env);
    if (!stage) return;
    int32_t level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
    uint32_t w[10] = { v->p[0], v->p[1], v->p[2], v->p[3], v->ahd.start, v->ahd.attack_end, v->ahd.hold_end,
                       v->ahd.decay_end, (uint32_t)v->ahd.coef, (uint32_t)v->ahd.shape };
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        level = opz_ahd_tick(level, coef, step);
        out[2 * i] = opz_add_q31(out[2 * i], level >> 1);
        out[2 * i + 1] = opz_add_q31(out[2 * i + 1], v->first && i < 10 ? (fract32)w[i] : stage);
    }
    v->first = 0;
    OPZ_ADSR_LEVEL(env) = level;
    v->pos += OPZ_BLOCK_FRAMES;
}
