/*
 * opz_adsr.h — the per-sample envelope step every OP-Z instrument engine applies, and the drum (AHD) variant
 * further down.
 *
 * Decoded from the desktop app (Fm::render, updateParametersADSR, Voice::noteOn/noteOff) and confirmed on the
 * device (synthsampler, cluster and organ run the same arithmetic; analoga carries the same release curve). See
 * sdk/docs/engine-abi.md §3.
 *
 * The host owns the ADSR object. Once per block the engine calls cb->updateParametersADSR(env, attack, decay,
 * sustain, release) with params[2..5]; the host moves between stages and writes a coefficient pair:
 *
 *   +8  coef  (Q31)   +12  step (Q31)
 *   per sample:  r = coef * level;   level' = r + (1 - r^3) * step        (Q31, saturating, rounded)
 *
 *   attack   coef 1, step = attack rate      -> curved rise; stage 2 at level >= 0.99
 *   decay    coef = decay rate, step 0       -> exponential fall to the sustain level
 *   sustain  coef 1, step = (S - level)/256  -> settles on S
 *   release  coef = release rate, step 0     -> exponential fall; below ~ -66 dB the host sets stage 0
 *   fade     (voice steal) negative step limited to ~1 % of the level per sample
 *
 * The host frees the voice when stage (+16) is 0, so an engine must keep calling updateParametersADSR and
 * stepping the level, or set stage 0 itself when its own sound has ended (TE's analoga does).
 */
#ifndef OPZ_ADSR_H
#define OPZ_ADSR_H

#include "opz_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPZ_ADSR_COEF(env) (*(volatile int32_t *)((uint8_t *)(env) + 8))
#define OPZ_ADSR_STEP(env) (*(volatile int32_t *)((uint8_t *)(env) + 12))

/* Q31 multiply, saturating. The host version rounds to nearest; on the Blackfin this is
 * __builtin_bfin_mult_fr1x32x32, which rounds down (checked in bfin-elf-run), so opzsim's envelope can differ from
 * the device's in the last bit. tools/cycles/sdk_check.c leaves it out for that reason. */
static inline int32_t opz_mul_q31_rnd(int32_t a, int32_t b)
{
#if defined(__bfin__)
    return __builtin_bfin_mult_fr1x32x32(a, b);
#else
    return opz_sat32(((int64_t)a * b + ((int64_t)1 << 30)) >> 31);
#endif
}

/* One sample of the host envelope. Keep level/coef/step in locals across the 64-frame loop and write the level
 * back with OPZ_ADSR_LEVEL(env) at the end of the block. */
static inline int32_t opz_env_tick(int32_t level, int32_t coef, int32_t step)
{
    int32_t r = opz_mul_q31_rnd(coef, level);
    int32_t r3 = opz_mul_q31_rnd(opz_mul_q31_rnd(r, r), r);
    int32_t t = opz_sub_q31(0x7fffffff, r3);
    return opz_add_q31(r, opz_mul_q31_rnd(t, step));
}

/* The same step when step is 0 (decay and release): (1 - r^3) * 0 adds nothing, so only r = coef * level is left.
 * Bit-identical to opz_env_tick(level, coef, 0) at a quarter of its cost; test step once per block. */
static inline int32_t opz_env_tick0(int32_t level, int32_t coef) { return opz_mul_q31_rnd(coef, level); }

/*
 * ---- Drum envelope: updateParametersAHD -------------------------------------------------------------
 *
 * Decoded from the desktop app (updateParametersAHD, SamplePlayerGain::onNoteOn/render) and confirmed on the
 * device (sampleplayergain.engine: same call, same layout, same per-sample step). See sdk/docs/engine-abi.md §3.
 *
 * The drum envelope runs on *positions*, not knob words: TE's sample player passes its playback position (in
 * sample frames) and three landmarks it derives from the knobs and the sample length. Once per block:
 *
 *   cb->updateParametersAHD(env, pos, attack_end, hold_end, decay_end, coef, shape)
 *
 *   stage 1 attack  pos <  attack_end: coef 1, step = 1 / (attack_end - pos)   (level clips at 1.0)
 *   stage 7 hold    pos <  hold_end:   level = 1.0, coef 1, step 0
 *   stage 4 decay   pos <  decay_end:  coef = coef, step = -(level / max(decay_end - pos, 64)) * (1 - f*shape^3)^4,
 *                                      f = (decay_end - pos) / (decay_end - hold_end)
 *                   pos >= decay_end:  stage 0 (the voice is freed)
 *   stage 5/6 fade  as for the ADSR (voice steal)
 *
 *   per sample:  level' = coef * level + step          (Q31, saturating) — linear, NOT opz_env_tick's r^3 form
 *
 * Note-off does nothing to it: drums are one-shots. With shape 0 the decay is a straight line that reaches zero
 * exactly at decay_end; shape 1 holds the level up and drops it near the end. A coef below 1 adds an exponential
 * fall on top.
 *
 * Use (an engine with a synthesized sound of `len` samples, playing at its own rate):
 *   onNoteOn:  opz_ahd_layout(&s->ahd, len, params); s->pos = s->ahd.start;
 *   render:    if (!OPZ_ADSR_STAGE(env)) return;
 *              cb->updateParametersAHD(env, s->pos, s->ahd.attack_end, s->ahd.hold_end, s->ahd.decay_end,
 *                                      s->ahd.coef, s->ahd.shape);
 *              if (!OPZ_ADSR_STAGE(env)) return;                       // past decay_end: done
 *              level = OPZ_ADSR_LEVEL(env), coef = OPZ_ADSR_COEF(env), step = OPZ_ADSR_STEP(env);
 *              for 64 frames: level = opz_ahd_tick(level, coef, step); out += x * level;
 *              OPZ_ADSR_LEVEL(env) = level; s->pos += 64;
 * TE recomputes the layout every block from the live knobs, so knob moves reshape a ringing hit; doing that is
 * optional. A sample player advances pos by its playback rate instead of 64.
 */
typedef struct OpzAhd {
    uint32_t start;       /* first position (params[2] moves it into the sound) */
    uint32_t attack_end;  /* landmarks, same units as start */
    uint32_t hold_end;
    uint32_t decay_end;
    fract32  coef;        /* Q31 multiplier per sample during decay */
    fract32  shape;       /* Q31 0..1 decay curve */
} OpzAhd;

#define OPZ_AHD_MIN_LENGTH 1024 /* below this the 512/192/64-frame margins of the layout overlap */

/* Q31 product of two non-negative words, truncated (what the layout code does). */
static inline uint32_t opz__ahd_mul(uint32_t a, uint32_t b)
{
    return (uint32_t)(((uint64_t)a * b) >> 31);
}

/*
 * The landmarks TE's drum sample player derives from params[2..5] for a sound `len` frames long (len >=
 * OPZ_AHD_MIN_LENGTH). Our own code; the arithmetic follows SamplePlayerGain::onNoteOn:
 *   params[2]  start:  len * p, at most len - 512
 *   params[3]  attack: up to half of what is left after start (at least 0.2 % when start > 0, to avoid a click)
 *   params[4]  hold:   a fraction of what is left after the attack, at least 512 frames
 *   params[5]  decay:  below 50 % its length grows to the rest of the sound (with a gentle exponential, coef
 *                      0.99995); from 50 % to 100 % the length stays full and the curve bends from a straight
 *                      line (shape 0) to "hold, then drop" (shape 1), with coef 1
 */
static inline void opz_ahd_layout(OpzAhd *a, uint32_t len, const uint32_t *params)
{
    uint32_t pa = params[OPZ_P_ATTACK] & 0x7fffffffu, pb = params[OPZ_P_DECAY] & 0x7fffffffu;
    uint32_t pc = params[OPZ_P_SUSTAIN] & 0x7fffffffu, pd = params[OPZ_P_RELEASE] & 0x7fffffffu;
    uint32_t start, attack, rest, hold, held, dlen;
    if (len < OPZ_AHD_MIN_LENGTH) len = OPZ_AHD_MIN_LENGTH;
    if ((pa & 0xffff8000u) && pb < 0x400000u) pb = 0x400000u;
    start = opz__ahd_mul(pa, len);
    if (start > len - 512) start = len - 512;
    attack = opz__ahd_mul((len - start) >> 1, pb);
    rest = len - start - attack;
    hold = opz__ahd_mul(rest, pc);
    if (hold < 512) hold = 512;
    held = start + attack + hold;
    dlen = (pd < 0x40000000u ? pd : 0x40000000u) << 1;
    if (dlen > 0x7fffffffu) dlen = 0x7fffffffu;
    a->start = start;
    a->attack_end = start + attack < len - 192 ? start + attack : len - 192;
    a->hold_end = held < len - 64 ? held : len - 64;
    a->decay_end = held + (uint32_t)(((int64_t)dlen * (int32_t)(rest - hold)) >> 31);
    a->shape = pd >= 0x40000000u ? (fract32)((pd - 0x40000000u) << 1) : 0;
    a->coef = pd >= 0x40000000u ? 0x7fffffff : 0x7ffe5c92;
}

/* One sample of the drum envelope (linear step, unlike opz_env_tick). */
static inline int32_t opz_ahd_tick(int32_t level, int32_t coef, int32_t step)
{
    return opz_add_q31(opz_mul_q31_rnd(coef, level), step);
}

/*
 * ---- The drum envelope on a synthesized one-shot ----------------------------------------------------------
 *
 * For a drum engine that makes its own sound (its own decays, its own end) and wants the envelope page to act on
 * it as on TE's sample kits. The engine's hit is the "sample": `len` is how long the hit lasts at the knobs it was
 * struck with (to about -60 dB), and the four envelope knobs cut and shape it with TE's drum-envelope arithmetic
 * (opz_ahd_layout): attack = start, decay = attack, sustain = hold, release = decay length and curve. At the
 * presets' envelope bytes 0 / 0 / 255 / 0 the level stays at exactly 1.0 for the whole hit (stage 7, hold), so the
 * hit is unchanged; the voice is in the host's stages 7 / 4 like TE's drum voices (the overload killer can pick
 * it), and the host frees it at decay_end if the engine has not ended it first.
 *
 * Note style "gate" (params[17] >> 29 == 2) is honoured as TE's drum player does it: a key release fades the hit
 * through the host's stage 5 (about 20 ms). Retrigger, mono and loop play one-shots.
 *
 * Unlike opz_ahd_tick, the gain is applied as one linear ramp per block (the start level and a per-frame step):
 * exact while coef is 1 (attack, hold, the curved decay above release 128), and within a fraction of a percent on
 * the slow exponential of release < 128 and the fades. Use:
 *
 *   onNoteOn:   opz_drumenv_start(&s->denv, hit_length_in_frames, params);
 *   onNoteOff:  opz_drumenv_off(&s->denv);
 *   render:     if (!OPZ_ADSR_STAGE(env)) return;
 *               if (!opz_drumenv_block(&s->denv, cb, env, 0x7fffffff)) return;       // the envelope ended
 *               if (OPZ_DRUMENV_STOLEN(&s->denv, env)) ...fade out fast...           // voice steal (stage 5/6)
 *               g = s->denv.g0, dg = (s->denv.g1 - g) >> 6;
 *               per frame: x = opz_mul_q31_rnd(x, g); g += dg;                       // before adding into out
 *
 * opz_mul_q31_rnd by 0x7fffffff leaves |x| < 2^30 unchanged on the host (the Blackfin rounds down: 1 LSB). An
 * engine that already scales its output by a per-model gain can fold the ramp into that gain instead (kit, snap),
 * and skip the work while g0 == g1 == 0x7fffffff.
 */
typedef struct OpzDrumEnv {
    OpzAhd ahd;
    uint32_t pos;           /* frames into the hit, starting at ahd.start */
    int32_t g0, g1;         /* this block's gain ramp, set by opz_drumenv_block */
    uint8_t gate;           /* note style gate: a key release fades the hit */
    uint8_t released;       /* 1: the key was released (gate only); 2: the fade has started */
    uint8_t pad[2];
} OpzDrumEnv;               /* 40 bytes of voice state */

#define OPZ_NOTE_STYLE_GATE 2

/* A voice steal (host stage 5 or 6) that is not this engine's own gate release: fade out in a few ms. */
#define OPZ_DRUMENV_STOLEN(e, env) \
    ((OPZ_ADSR_STAGE(env) == 5 || OPZ_ADSR_STAGE(env) == 6) && !(e)->released)

/* Once per hit or block: small rather than fast (GCC's optimize("Os"); the host build uses plain noinline). */
#if defined(__GNUC__) && !defined(__clang__)
#define OPZ__DRUMENV_COLD __attribute__((noinline, unused, optimize("Os")))
#else
#define OPZ__DRUMENV_COLD __attribute__((noinline, unused))
#endif

/* frames x k / 65536 for frames < 2^28 and k <= 65535, in 32-bit arithmetic (no libgcc call). */
OPZ__DRUMENV_COLD static uint32_t opz__drumenv_frac(uint32_t frames, uint32_t k)
{
    return (frames >> 16) * k + (((frames & 0xffffu) * k) >> 16);
}

/* At the hit: the landmarks for a hit of `len` frames from the envelope knobs (params may be NULL: the presets'
 * neutral 0 / 0 / 255 / 0). opz_ahd_layout's arithmetic with the knobs at 16 bits and 32-bit products (no libgcc),
 * plus one block of margin so that the full-length hold reaches past the engine's own end. len < 2^28 (100 min). */
OPZ__DRUMENV_COLD static void opz_drumenv_start(OpzDrumEnv *e, uint32_t len, const uint32_t *params)
{
    static const uint32_t neutral[OPZ_P_RELEASE + 1] = { 0, 0, 0, 0, 0x7fffffff, 0 };
    const uint32_t *p = params ? params : neutral;
    uint32_t k[4];                                                   /* attack .. release, 0 .. 65535 */
    for (int i = 0; i < 4; i++) k[i] = (p[OPZ_P_ATTACK + i] & 0x7fffffffu) >> 15;
    len += OPZ_BLOCK_FRAMES;
    if (len < OPZ_AHD_MIN_LENGTH) len = OPZ_AHD_MIN_LENGTH;
    if (k[0] && k[1] < 128) k[1] = 128;                              /* a start needs a short attack (no click) */
    uint32_t start = opz__drumenv_frac(len, k[0]);
    if (start > len - 512) start = len - 512;
    uint32_t attack = opz__drumenv_frac((len - start) >> 1, k[1]);
    uint32_t rest = len - start - attack;
    uint32_t hold = opz__drumenv_frac(rest, k[2]);
    if (hold < 512) hold = 512;
    uint32_t held = start + attack + hold, d = k[3], dl = d < 32768 ? d << 1 : 65535;
    e->ahd.start = e->pos = start;
    e->ahd.attack_end = start + attack < len - 192 ? start + attack : len - 192;
    e->ahd.hold_end = held < len - 64 ? held : len - 64;
    /* rest < hold only when the 512-frame minimum hold is longer than the rest, where TE's signed product would end
     * the decay a little before the hold: here they end together */
    e->ahd.decay_end = held + (rest > hold ? opz__drumenv_frac(rest - hold, dl) : 0);
    e->ahd.shape = d >= 32768 ? (fract32)((d - 32768) << 16) : 0;
    e->ahd.coef = d >= 32768 ? 0x7fffffff : 0x7ffe5c92;
    e->gate = params && ((params[OPZ_P_NOTE_STYLE] & 0x7fffffffu) >> 29) == OPZ_NOTE_STYLE_GATE;
    e->released = 0;
}

/* At note-off: fades the hit if it was struck in gate style; one-shots ignore it. */
static inline void opz_drumenv_off(OpzDrumEnv *e)
{
    if (e->gate) e->released = 1;
}

/* a x g (Q31), exactly a when g is 1.0. */
OPZ__DRUMENV_COLD static int32_t opz__drumenv_mul(int32_t a, int32_t g)
{
    return g == 0x7fffffff ? a : opz_mul_q31(a, g);
}

/* Once per block: moves the host envelope on and sets this block's gain ramp times `scale` (Q31; pass 0x7fffffff
 * for the plain gain, or the engine's own output gain to fold the two): e->g0 at the first frame, e->g1 after the
 * last (ramp linearly: g0 + i * ((g1 - g0) >> 6)). Both are exactly `scale` while the envelope holds. Returns 0
 * when the envelope has ended (the host has set stage 0: the voice is free). */
OPZ__DRUMENV_COLD static int opz_drumenv_block(OpzDrumEnv *e, const ModuleCallbackInterface *cb, ADSR *env,
                                               int32_t scale)
{
    if (e->released == 1) {                        /* gate release: TE's drum player fades the hit itself */
        OPZ_ADSR_STAGE(env) = 5;
        OPZ_ADSR_COEF(env) = 0x7f000000;           /* x0.992 per frame: tau 2.9 ms, gone in ~20 ms */
        OPZ_ADSR_STEP(env) = 0;
        e->released = 2;
    }
    cb->updateParametersAHD(env, e->pos, e->ahd.attack_end, e->ahd.hold_end, e->ahd.decay_end, e->ahd.coef,
                            e->ahd.shape);
    if (OPZ_ADSR_STAGE(env) == 0) return 0;
    int32_t level = OPZ_ADSR_LEVEL(env), c = OPZ_ADSR_COEF(env);
    if (c != 0x7fffffff)                                               /* coef^64 (the hold's 1.0 stays 1.0) */
        for (int k = 0; k < 6; k++) c = opz_mul_q31(c, c);
    /* + 64 steps (the coef x step cross terms are left out) */
    int32_t end = opz_add_q31(opz__drumenv_mul(level, c), opz_shl_q31(OPZ_ADSR_STEP(env), 6));
    if (end < 0) end = 0;
    e->g0 = opz__drumenv_mul(scale, level);
    e->g1 = opz__drumenv_mul(scale, end);
    OPZ_ADSR_LEVEL(env) = end;
    e->pos += OPZ_BLOCK_FRAMES;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* OPZ_ADSR_H */
