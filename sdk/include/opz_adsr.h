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

#ifdef __cplusplus
}
#endif

#endif /* OPZ_ADSR_H */
