/*
 * opz_mix.h — the track's level and pan, applied by an instrument engine.
 *
 * The OP-Z does not apply an instrument track's level (params[11], the red dial on the orange page) or pan
 * (params[10], the yellow dial) after the engine: every TE instrument applies both itself in render, and the OS
 * takes the FX1/FX2 sends, the tape feed and the dry mix from what the engine wrote (sdk/docs/engine-abi.md §4,
 * "Level, pan and the sends"). An engine that ignores them has a dead level and pan dial, and its sends are taken at
 * full level. TE's law, used here [code, app Fm::render 0x7400, AnalogA::render 0x1b518; device: analoga, ep, organ,
 * cluster, synthsampler and sampleplayergain all read params +0x28/+0x2c in render]:
 *
 *   gain_L = level * min(2 * (1 - pan), 1)      gain_R = level * min(2 * pan, 1)
 *
 * with level and pan the Q31 parameter words (linear: level 255 = 1, 128 = 0.5; pan 0 = hard left, 255 = hard right,
 * both gains 1 at the centre: a balance law with no centre dip). TE also multiplies in the velocity; our engines
 * already scale by velocity themselves, so this header leaves that out.
 *
 * Use (one 4-byte OpzMix in the voice state):
 *
 *   onNoteOn:  opz_mix_start(&s->mix, params);           // the note starts at the current level, no ramp
 *   render:    fract32 mono[OPZ_BLOCK_FRAMES];            // write the voice's samples here instead of into out
 *              ... mono[i] = y; ...
 *              opz_mix_mono(out, mono, &s->mix, params);  // adds mono x gain_L / gain_R into out, saturating
 *
 * A stereo engine writes 128 interleaved values and calls opz_mix_stereo (gain_L on L, gain_R on R).
 *
 * The gains ramp linearly across the block from the last block's values, so a dial move or an LFO on level/pan
 * (LFO targets "pan" and "volume") never zips; TE's engines smooth them too. Cost on the Blackfin, measured on every
 * engine of ours with tools/opz_cycles.py: with the hand-scheduled loops below, at most 6 (typically ~5) cycles per
 * sample per voice more than adding the same sample to L and R in the engine's own loop (fm2 97 -> 102, dx 173 ->
 * 178; GCC's C loops took 10-15: fm2 111, dx 186), ~35-60 more per note-on, and a 256-byte (mono) or 512-byte
 * (stereo) buffer on render's stack. The loops take 100-230 bytes more code than the C loops did.
 *
 * params may be NULL in an engine's own tests: guard opz_mix_start with `if (params)` there.
 * MIT licence, see CONTRIBUTING.md.
 */
#ifndef OPZ_MIX_H
#define OPZ_MIX_H

#include "opz_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpzMix {
    int16_t l, r; /* gains reached at the end of the last block, Q15 (32767 = 1) */
} OpzMix;

#define OPZ_MIX_UNITY 32767

/* Target gains for the current params, Q15. Per block (two 32 x 32 products): not for per-sample use. */
static inline void opz_mix_targets(const uint32_t *params, int32_t *l, int32_t *r)
{
    fract32 level = (fract32)(params[OPZ_P_LEVEL] & 0x7fffffffu);
    fract32 pan = (fract32)(params[OPZ_P_PAN] & 0x7fffffffu);
    fract32 a = pan >= 0x40000000 ? (fract32)((uint32_t)(0x7fffffff - pan) << 1) : 0x7fffffff;  /* min(2(1-p), 1) */
    fract32 b = pan < 0x40000000 ? pan << 1 : 0x7fffffff;                                       /* min(2p, 1) */
    *l = opz_mul_q31(level, a) >> 16;
    *r = opz_mul_q31(level, b) >> 16;
}

/* At note-on: jump to the current gains (a new note needs no ramp: its envelope starts at 0). */
static inline void opz_mix_start(OpzMix *m, const uint32_t *params)
{
    int32_t l, r;
    opz_mix_targets(params, &l, &r);
    m->l = (int16_t)l;
    m->r = (int16_t)r;
}

#if defined(__bfin__)
/*
 * The Blackfin loops: hand-scheduled, bit-identical to the C loops below (tools/cycles/sdk_check.c checks every one
 * against the host build). x * g >> 15 (g = 0..32767) is done as in opz_mul_q31_q15, split into the low and the high
 * 16 bits of x, but for both channels at once: the two gains sit packed in one register (gain_R in .H, gain_L in .L)
 * and one dual-MAC instruction makes both channels' partial products:
 *
 *   A1 = x.L * g.H, A0 = x.L * g.L (FU)             low parts, unsigned (x.L is 0..65535, the gains are >= 0)
 *   A1 = A1 >> 15;  A0 = A0 >> 15                     (floor; the accumulator shifts are one each)
 *   R3 = (A1 += x.H * g.H), R2 = (A0 += x.H * g.L)    + 2 * x.H * g, extracted saturating: x * g >> 15 exactly
 *
 * then two saturating adds into out. The loads and stores ride in the parallel slots (two I-register loads, or a
 * load and a store, per instruction), loaded one frame ahead; the last frame is peeled so that nothing is read past
 * x or out. A stereo voice's L and R samples are loaded as halves straight into the packed form (lo = xR.L:xL.L,
 * hi = xR.H:xL.H), so stereo costs what mono does. Instructions per frame (the loop bodies): 3 at unity gain, 6 with
 * steady gains, 9 while the gains ramp; GCC's C loops take 15 for steady gains.
 *
 * The ramp keeps each gain as (g * 64 + i * d) << 10 (Q15 in the high half), so the high half is exactly the C
 * loop's (g * 64 + i * d) >> 6 and one PACK makes the packed pair. All the loops use LC0 and I0-I2 (L0-L2 are 0 by
 * the ABI, so the I registers address linearly).
 */
#define OPZ_MIX_BF_SETUP "I0 = %[x];\n\tI1 = %[o];\n\tI2 = %[o];\n\t"
#define OPZ_MIX_BF_CLOBBER "R0", "R1", "R2", "R3", "R4", "R5", "A0", "A1", "I0", "I1", "I2", "LC0", "LT0", "LB0", \
                           "memory"

static inline void opz_mix_bf_mono_unity(fract32 *out, const fract32 *x)
{
    __asm__ volatile(
        OPZ_MIX_BF_SETUP
        "R0 = [I0++];\n\t"
        "R4 = [I1++];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR4 = R4 + R0 (S) || R5 = [I1++];\n\t"
        "R5 = R5 + R0 (S) || [I2++] = R4 || R0 = [I0++];\n"
        "2:\tMNOP || [I2++] = R5 || R4 = [I1++];\n\t"
        "R4 = R4 + R0 (S) || R5 = [I1++];\n\t"
        "R5 = R5 + R0 (S) || [I2++] = R4;\n\t"
        "[I2++] = R5;"
        : : [x] "a"(x), [o] "a"(out), [n] "a"(OPZ_BLOCK_FRAMES - 1) : OPZ_MIX_BF_CLOBBER);
}

static inline void opz_mix_bf_stereo_unity(fract32 *out, const fract32 *x)
{
    __asm__ volatile(
        OPZ_MIX_BF_SETUP
        "R0 = [I0++];\n\t"
        "R4 = [I1++];\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tR4 = R4 + R0 (S) || R5 = [I1++] || R1 = [I0++];\n\t"
        "R5 = R5 + R1 (S) || [I2++] = R4 || R0 = [I0++];\n"
        "2:\tMNOP || [I2++] = R5 || R4 = [I1++];\n\t"
        "R4 = R4 + R0 (S) || R5 = [I1++] || R1 = [I0++];\n\t"
        "R5 = R5 + R1 (S) || [I2++] = R4;\n\t"
        "[I2++] = R5;"
        : : [x] "a"(x), [o] "a"(out), [n] "a"(OPZ_BLOCK_FRAMES - 1) : OPZ_MIX_BF_CLOBBER);
}

/* g = gain_R << 16 | gain_L */
static inline void opz_mix_bf_mono_gain(fract32 *out, const fract32 *x, uint32_t g)
{
    __asm__ volatile(
        OPZ_MIX_BF_SETUP
        "R0 = [I0++];\n\t"
        "A1 = R0.L * %[g].H, A0 = R0.L * %[g].L (FU);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tA1 = A1 >> 15 || R4 = [I1++];\n\t"
        "A0 = A0 >> 15 || R5 = [I1++];\n\t"
        "R3 = (A1 += R0.H * %[g].H), R2 = (A0 += R0.H * %[g].L) || R0 = [I0++];\n\t"
        "R4 = R4 + R2 (S);\n\t"
        "R5 = R5 + R3 (S) || [I2++] = R4;\n"
        "2:\tA1 = R0.L * %[g].H, A0 = R0.L * %[g].L (FU) || [I2++] = R5;\n\t"
        "A1 = A1 >> 15 || R4 = [I1++];\n\t"
        "A0 = A0 >> 15 || R5 = [I1++];\n\t"
        "R3 = (A1 += R0.H * %[g].H), R2 = (A0 += R0.H * %[g].L);\n\t"
        "R4 = R4 + R2 (S);\n\t"
        "R5 = R5 + R3 (S) || [I2++] = R4;\n\t"
        "[I2++] = R5;"
        : : [x] "a"(x), [o] "a"(out), [n] "a"(OPZ_BLOCK_FRAMES - 1), [g] "d"(g) : OPZ_MIX_BF_CLOBBER);
}

/* Stereo x: R0 = xR.L:xL.L and R1 = xR.H:xL.H, loaded as halves in memory order (xL.L, xL.H, xR.L, xR.H). */
static inline void opz_mix_bf_stereo_gain(fract32 *out, const fract32 *x, uint32_t g)
{
    __asm__ volatile(
        OPZ_MIX_BF_SETUP
        "R0.L = W[I0++];\n\t"
        "R1.L = W[I0++];\n\t"
        "R0.H = W[I0++];\n\t"
        "R1.H = W[I0++];\n\t"
        "A1 = R0.H * %[g].H, A0 = R0.L * %[g].L (FU);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tA1 = A1 >> 15 || R4 = [I1++] || R0.L = W[I0++];\n\t"
        "A0 = A0 >> 15 || R5 = [I1++];\n\t"
        "R3 = (A1 += R1.H * %[g].H), R2 = (A0 += R1.L * %[g].L) || R1.L = W[I0++];\n\t"
        "R4 = R4 + R2 (S) || R0.H = W[I0++];\n\t"
        "R5 = R5 + R3 (S) || [I2++] = R4 || R1.H = W[I0++];\n"
        "2:\tA1 = R0.H * %[g].H, A0 = R0.L * %[g].L (FU) || [I2++] = R5;\n\t"
        "A1 = A1 >> 15 || R4 = [I1++];\n\t"
        "A0 = A0 >> 15 || R5 = [I1++];\n\t"
        "R3 = (A1 += R1.H * %[g].H), R2 = (A0 += R1.L * %[g].L);\n\t"
        "R4 = R4 + R2 (S);\n\t"
        "R5 = R5 + R3 (S) || [I2++] = R4;\n\t"
        "[I2++] = R5;"
        : : [x] "a"(x), [o] "a"(out), [n] "a"(OPZ_BLOCK_FRAMES - 1), [g] "d"(g) : OPZ_MIX_BF_CLOBBER);
}

/* r = { d_L << 10, d_R << 10, (s_L * 64 + d_L) << 10, (s_R * 64 + d_R) << 10 }: the steps, and frame 0's gains.
   Mono: R0 = x (then out L), R1 = packed gains (then out R), R4/R5 = steps, R6/R7 = gains L/R. */
static inline void opz_mix_bf_mono_ramp(fract32 *out, const fract32 *x, const int32_t *r)
{
    __asm__ volatile(
        OPZ_MIX_BF_SETUP
        "R4 = [%[r]];\n\t"
        "R5 = [%[r] + 4];\n\t"
        "R6 = [%[r] + 8];\n\t"
        "R7 = [%[r] + 12];\n\t"
        "R0 = [I0++];\n\t"
        "R1 = PACK(R7.H, R6.H);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tA1 = R0.L * R1.H, A0 = R0.L * R1.L (FU);\n\t"
        "A1 = A1 >> 15;\n\t"
        "A0 = A0 >> 15;\n\t"
        "R3 = (A1 += R0.H * R1.H), R2 = (A0 += R0.H * R1.L) || R0 = [I1++];\n\t"
        "R6 = R6 + R4 (NS) || R1 = [I1++];\n\t"
        "R2 = R2 + R0 (S);\n\t"
        "R7 = R7 + R5 (NS) || [I2++] = R2 || R0 = [I0++];\n\t"
        "R3 = R3 + R1 (S);\n"
        "2:\tR1 = PACK(R7.H, R6.H) || [I2++] = R3;\n\t"
        "A1 = R0.L * R1.H, A0 = R0.L * R1.L (FU);\n\t"
        "A1 = A1 >> 15;\n\t"
        "A0 = A0 >> 15;\n\t"
        "R3 = (A1 += R0.H * R1.H), R2 = (A0 += R0.H * R1.L) || R0 = [I1++];\n\t"
        "R1 = [I1++];\n\t"
        "R2 = R2 + R0 (S);\n\t"
        "R3 = R3 + R1 (S) || [I2++] = R2;\n\t"
        "[I2++] = R3;"
        : : [x] "a"(x), [o] "a"(out), [n] "a"(OPZ_BLOCK_FRAMES - 1), [r] "a"(r)
        : OPZ_MIX_BF_CLOBBER, "R6", "R7");
}

/* Stereo: R0 = lo halves (then out L), R1 = hi halves (then out R), R6 = packed gains, R7/R4 = gains L/R, R5 = the
   step, reloaded from r (no register left for both). */
static inline void opz_mix_bf_stereo_ramp(fract32 *out, const fract32 *x, const int32_t *r)
{
    __asm__ volatile(
        OPZ_MIX_BF_SETUP
        "R7 = [%[r] + 8];\n\t"
        "R4 = [%[r] + 12];\n\t"
        "R0.L = W[I0++];\n\t"
        "R1.L = W[I0++];\n\t"
        "R0.H = W[I0++];\n\t"
        "R6 = PACK(R4.H, R7.H) || R1.H = W[I0++];\n\t"
        "A1 = R0.H * R6.H, A0 = R0.L * R6.L (FU);\n\t"
        "LSETUP (1f, 2f) LC0 = %[n];\n"
        "1:\tA1 = A1 >> 15 || R5 = [%[r]] || R0 = [I1++];\n\t"
        "A0 = A0 >> 15;\n\t"
        "R3 = (A1 += R1.H * R6.H), R2 = (A0 += R1.L * R6.L) || R1 = [I1++];\n\t"
        "R2 = R2 + R0 (S);\n\t"
        "R7 = R7 + R5 (NS) || R5 = [%[r] + 4];\n\t"
        "R3 = R3 + R1 (S) || [I2++] = R2 || R0.L = W[I0++];\n\t"
        "R4 = R4 + R5 (NS) || [I2++] = R3 || R1.L = W[I0++];\n\t"
        "R6 = PACK(R4.H, R7.H) || R0.H = W[I0++];\n"
        "2:\tA1 = R0.H * R6.H, A0 = R0.L * R6.L (FU) || R1.H = W[I0++];\n\t"
        "A1 = A1 >> 15 || R0 = [I1++];\n\t"
        "A0 = A0 >> 15;\n\t"
        "R3 = (A1 += R1.H * R6.H), R2 = (A0 += R1.L * R6.L) || R1 = [I1++];\n\t"
        "R2 = R2 + R0 (S);\n\t"
        "R3 = R3 + R1 (S) || [I2++] = R2;\n\t"
        "[I2++] = R3;"
        : : [x] "a"(x), [o] "a"(out), [n] "a"(OPZ_BLOCK_FRAMES - 1), [r] "a"(r)
        : OPZ_MIX_BF_CLOBBER, "R6", "R7");
}

static inline void opz_mix_bf_ramp_steps(int32_t *r, int32_t sl, int32_t sr, int32_t tl, int32_t tr)
{
    r[0] = (tl - sl) * 1024;
    r[1] = (tr - sr) * 1024;
    r[2] = (sl * 64 + tl - sl) * 1024;
    r[3] = (sr * 64 + tr - sr) * 1024;
}
#endif /* __bfin__ */

/* Adds x[i] * gain into out[2i] (L) and out[2i + 1] (R) for the 64 frames, saturating. */
static inline void opz_mix_mono(fract32 *restrict out, const fract32 *restrict x, OpzMix *m, const uint32_t *params)
{
    int32_t tl, tr;
    opz_mix_targets(params, &tl, &tr);
    int32_t sl = m->l, sr = m->r;
#if defined(__bfin__)
    if (sl == tl && sr == tr) {
        if (tl == OPZ_MIX_UNITY && tr == OPZ_MIX_UNITY) opz_mix_bf_mono_unity(out, x);
        else opz_mix_bf_mono_gain(out, x, (uint32_t)tr << 16 | (uint32_t)tl);
    } else {
        int32_t r[4];
        opz_mix_bf_ramp_steps(r, sl, sr, tl, tr);
        opz_mix_bf_mono_ramp(out, x, r);
        m->l = (int16_t)tl;
        m->r = (int16_t)tr;
    }
#else
    if (sl == tl && sr == tr) {
        if (tl == OPZ_MIX_UNITY && tr == OPZ_MIX_UNITY) {
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                out[2 * i] = opz_add_q31(out[2 * i], x[i]);
                out[2 * i + 1] = opz_add_q31(out[2 * i + 1], x[i]);
            }
        } else {
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                out[2 * i] = opz_add_q31(out[2 * i], opz_mul_q31_q15(x[i], tl));
                out[2 * i + 1] = opz_add_q31(out[2 * i + 1], opz_mul_q31_q15(x[i], tr));
            }
        }
    } else {
        /* Q21 ramps (Q15 << 6): after 64 steps of (t - s) they land exactly on t */
        int32_t gl = sl * 64, gr = sr * 64, dl = tl - sl, dr = tr - sr;
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
            gl += dl;
            gr += dr;
            out[2 * i] = opz_add_q31(out[2 * i], opz_mul_q31_q15(x[i], gl >> 6));
            out[2 * i + 1] = opz_add_q31(out[2 * i + 1], opz_mul_q31_q15(x[i], gr >> 6));
        }
        m->l = (int16_t)tl;
        m->r = (int16_t)tr;
    }
#endif
}

/* The same for a stereo voice: x is 64 interleaved frames (L, R, L, R, ...); gain_L scales L, gain_R scales R. */
static inline void opz_mix_stereo(fract32 *restrict out, const fract32 *restrict x, OpzMix *m, const uint32_t *params)
{
    int32_t tl, tr;
    opz_mix_targets(params, &tl, &tr);
    int32_t sl = m->l, sr = m->r;
#if defined(__bfin__)
    if (sl == tl && sr == tr) {
        if (tl == OPZ_MIX_UNITY && tr == OPZ_MIX_UNITY) opz_mix_bf_stereo_unity(out, x);
        else opz_mix_bf_stereo_gain(out, x, (uint32_t)tr << 16 | (uint32_t)tl);
    } else {
        int32_t r[4];
        opz_mix_bf_ramp_steps(r, sl, sr, tl, tr);
        opz_mix_bf_stereo_ramp(out, x, r);
        m->l = (int16_t)tl;
        m->r = (int16_t)tr;
    }
#else
    if (sl == tl && sr == tr) {
        if (tl == OPZ_MIX_UNITY && tr == OPZ_MIX_UNITY) {
            for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) out[i] = opz_add_q31(out[i], x[i]);
        } else {
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                out[2 * i] = opz_add_q31(out[2 * i], opz_mul_q31_q15(x[2 * i], tl));
                out[2 * i + 1] = opz_add_q31(out[2 * i + 1], opz_mul_q31_q15(x[2 * i + 1], tr));
            }
        }
    } else {
        int32_t gl = sl * 64, gr = sr * 64, dl = tl - sl, dr = tr - sr;
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
            gl += dl;
            gr += dr;
            out[2 * i] = opz_add_q31(out[2 * i], opz_mul_q31_q15(x[2 * i], gl >> 6));
            out[2 * i + 1] = opz_add_q31(out[2 * i + 1], opz_mul_q31_q15(x[2 * i + 1], gr >> 6));
        }
        m->l = (int16_t)tl;
        m->r = (int16_t)tr;
    }
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* OPZ_MIX_H */
