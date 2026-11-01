/*
 * opz_tables.h — reading the OS's shared DSP tables (callback entries +0x08 .. +0x18).
 *
 * The OS hands every engine five tables through the ModuleCallbackInterface it passes to initModule. They cost an
 * engine no slot memory, so a 5 KB engine should use them instead of carrying its own sine or filter tables.
 * Formats (OS 1.2.45, device code and the tables themselves; sdk/docs/engine-abi.md §5 "Shared tables"):
 *
 *   +0x08 k_lp_table     129 x Q31 one-pole coefficients c for y += c (x - y); TWO such poles in series are -3 dB at
 *                        10 Hz * 2000^(i/128) (10 Hz .. 20 kHz, 11.67 steps per octave). Integer index, no
 *                        interpolation (TE's delay, dist, chorus, reverb).
 *   +0x0C g_sine         8,193 x Q31, one sine cycle of peak 0x7FFF0000 plus a guard point (= entry 0). Index
 *                        phase >> 19 of a Q32 phase, no interpolation (TE's ep, the OS LFO). Cosine: phase + 2^30.
 *   +0x10 g_sineTable    256 x int16, one sine cycle, peak 32767 (-32768 at 3/4). No TE reader; phase >> 24.
 *   +0x14 rFilterCoeffs  5 pointers to [9][65] Q31 tables b0, b1/2, b2, a1/2, a2 of RBJ biquads: columns 0-29
 *                        low-pass at the ISO third-octaves 25 Hz .. 20 kHz, 30-34 flat, 35-64 high-pass at the same
 *                        frequencies; rows = resonance, Q 0.2 .. 16 (LP) / 0.2 .. 4 (HP). Bilinear lookup (TE's
 *                        analoga, the OS track filter).
 *   +0x18 releaseTable   128 x Q31 per-sample decay multipliers e^(-1/tau) of the ADSR (index parameter >> 24).
 *
 * Reach them only through the callback table: their addresses are OS-version specific (1.2.45: g_sine in L2,
 * g_sineTable in L1 data, the others in DDR). Keep the cb pointer from initModule, as TE's engines do, and load
 * the table pointer into a local once per block. The opz_host_check_*() calls in initModule refuse an OS whose
 * tables differ from these formats. opzsim and the cycle driver supply tables generated from the same formulas
 * (opz_tables_gen.h, tools/opz_tables.py), never TE's data.
 *
 * Header-only, integer only, no libc. MIT licence, see CONTRIBUTING.md.
 */
#ifndef OPZ_TABLES_H
#define OPZ_TABLES_H

#include "opz_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPZ_SINE_BITS     13            /* g_sine: 8,192 points per cycle, + 1 guard */
#define OPZ_SINE_LEN      8192
#define OPZ_SINE_PEAK     0x7FFF0000
#define OPZ_SINE16_LEN    256           /* g_sineTable */
#define OPZ_KLP_LEN       129           /* k_lp_table, index 0..128 */
#define OPZ_RF_ROWS       9             /* rFilterCoeffs: rows (resonance) x columns (cutoff) */
#define OPZ_RF_COLS       65
#define OPZ_RELEASE_LEN   128           /* releaseTable */

/* Columns of rFilterCoeffs and landmarks of k_lp_table (index = 128 log2(f / 10 Hz) / log2(2000)). */
enum { OPZ_RF_COL_LP_25HZ = 0, OPZ_RF_COL_LP_1KHZ = 16, OPZ_RF_COL_LP_20KHZ = 29, OPZ_RF_COL_FLAT = 32,
       OPZ_RF_COL_HP_25HZ = 35, OPZ_RF_COL_HP_1KHZ = 51, OPZ_RF_COL_HP_20KHZ = 64 };
enum { OPZ_KLP_100HZ = 39, OPZ_KLP_1KHZ = 78, OPZ_KLP_10KHZ = 116 };
/* The five coefficient tables, in rFilterCoeffs order. y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2; b1 and a1 halved. */
enum { OPZ_BQ_B0 = 0, OPZ_BQ_B1H, OPZ_BQ_B2, OPZ_BQ_A1H, OPZ_BQ_A2 };

/* ---- sines -------------------------------------------------------------------------------------------- */

/* sin(2 pi phase) * 0x7FFF0000 / 2^31 as the OS and TE's ep read it: one load, phase truncated to 13 bits, no
 * interpolation (error up to 7.7e-4 of full scale, phase-truncation spurs near -78 dBc). */
static inline fract32 opz_host_sine(const fract32 *g_sine, uint32_t phase) { return g_sine[phase >> 19]; }

/* The same, linearly interpolated between g_sine[i] and g_sine[i + 1] (the guard point makes i = 8191 safe) with a
 * 15-bit fraction: two loads, a subtract and a 3-instruction Q31 x Q15 multiply; error < 1e-7 (-140 dB). */
static inline fract32 opz_host_sine_lerp(const fract32 *g_sine, uint32_t phase)
{
    const fract32 *p = g_sine + (phase >> 19);
    return p[0] + opz_mul_q31_q15(p[1] - p[0], (int32_t)((phase >> 4) & 0x7fff));
}

/* cos(2 pi phase), either lookup. */
static inline fract32 opz_host_cos(const fract32 *g_sine, uint32_t phase) { return g_sine[(phase + 0x40000000u) >> 19]; }

/* g_sineTable: sin(2 pi phase) in Q15 (-32768 .. 32767), 8-bit phase, no interpolation. */
static inline int32_t opz_host_sine16(const int16_t *g_sineTable, uint32_t phase) { return g_sineTable[phase >> 24]; }

/* g_sineTable linearly interpolated (no guard point: the next index wraps), as Q31: error ~1e-4 (-80 dB), between
 * opz_host_sine and opz_host_sine_lerp in quality, from a 512-byte table in fast on-chip memory. */
static inline fract32 opz_host_sine16_lerp(const int16_t *g_sineTable, uint32_t phase)
{
    int32_t a = g_sineTable[phase >> 24], b = g_sineTable[(phase + 0x01000000u) >> 24];
    int16_t d = (int16_t)(b - a), f = (int16_t)((phase >> 9) & 0x7fff);   /* |d| <= 805: a 16 x 16 multiply */
    return (int32_t)((uint32_t)a << 16) + 2 * (d * f);
}

/* ---- one-pole low-pass (k_lp_table) -------------------------------------------------------------------- */

/* The coefficient at index i (clamped to 0..128). */
static inline fract32 opz_host_klp(const fract32 *k_lp_table, int32_t i)
{
    return k_lp_table[i < 0 ? 0 : (i > OPZ_KLP_LEN - 1 ? OPZ_KLP_LEN - 1 : i)];
}

/* One pole: y + c (x - y), saturating. Run two in series for the table's -3 dB point (one alone is -1.5 dB there). */
static inline fract32 opz_host_onepole(fract32 y, fract32 x, fract32 c) { return opz_add_q31(y, opz_mul_q31(c, opz_sub_q31(x, y))); }

/* ---- biquads (rFilterCoeffs) --------------------------------------------------------------------------- */

/* Bilinear interpolation of the five tables at column col (0..63) + fraction cf and row row (0..7) + fraction rf
 * (fractions Q31, >= 0), truncating products and saturating sums like TE's lookup. coef[] in OPZ_BQ_* order. */
static inline void opz_host_rf_interp(const fract32 *const *rf, uint32_t col, fract32 cf, uint32_t row, fract32 rfr,
                                      fract32 coef[5])
{
    for (int k = 0; k < 5; k++) {
        const fract32 *t = rf[k] + row * OPZ_RF_COLS + col;
        fract32 lo = opz_add_q31(t[0], opz_mul_q31(opz_sub_q31(t[1], t[0]), cf));
        fract32 hi = opz_add_q31(t[OPZ_RF_COLS], opz_mul_q31(opz_sub_q31(t[OPZ_RF_COLS + 1], t[OPZ_RF_COLS]), cf));
        coef[k] = opz_add_q31(lo, opz_mul_q31(opz_sub_q31(hi, lo), rfr));
    }
}

/* Coefficients for a cutoff word and a resonance word (Q31 parameter words, e.g. params[8], params[9]) as TE's
 * analoga reads them: column cutoff >> 25 (0 = LP 25 Hz, ~32 = flat, 63 = HP 16-20 kHz), the full resonance range
 * over the nine rows. Per block, not per sample (~20 multiplies). */
static inline void opz_host_rf_lookup(const fract32 *const *rf, uint32_t cutoff, uint32_t res, fract32 coef[5])
{
    cutoff &= 0x7fffffffu;
    res &= 0x7fffffffu;
    opz_host_rf_interp(rf, cutoff >> 25, (fract32)((cutoff & 0x1FFFFFFu) << 6), res >> 28,
                       (fract32)((res & 0xFFFFFFFu) << 3), coef);
}

/* The same with the OS track filter's resonance rule: the rows span the lower half of the knob (the top half stays
 * at Q 16 / 4), so an engine-owned filter (plug flag 0x04) answers the knobs like the OS's. */
static inline void opz_host_rf_lookup_os(const fract32 *const *rf, uint32_t cutoff, uint32_t res, fract32 coef[5])
{
    cutoff &= 0x7fffffffu;
    res &= 0x7fffffffu;
    uint32_t r = res > 0x3FFFFFFFu ? 0x3FFFFFFFu : res;
    opz_host_rf_interp(rf, cutoff >> 25, (fract32)((cutoff & 0x1FFFFFFu) << 6), (res >> 27) > 7 ? 7 : res >> 27,
                       (fract32)((r & 0x7FFFFFFu) << 4), coef);
}

/* One sample of a transposed direct form II biquad (the OS track filter's form, without its resonance soft-clip):
 * y = b0 x + s1; s1 = s2 + 2 (b1/2 x - a1/2 y); s2 = b2 x - a2 y. Saturating: at high Q keep x near -6 dBFS or
 * below. 5 multiplies. */
typedef struct OpzBiquadState { fract32 s1, s2; } OpzBiquadState;
static inline fract32 opz_host_biquad(OpzBiquadState *s, const fract32 coef[5], fract32 x)
{
    fract32 y = opz_add_q31(opz_mul_q31(coef[OPZ_BQ_B0], x), s->s1);
    fract32 u = opz_sub_q31(opz_mul_q31(coef[OPZ_BQ_B1H], x), opz_mul_q31(coef[OPZ_BQ_A1H], y));
    s->s1 = opz_add_q31(s->s2, opz_shl_q31(u, 1));
    s->s2 = opz_sub_q31(opz_mul_q31(coef[OPZ_BQ_B2], x), opz_mul_q31(coef[OPZ_BQ_A2], y));
    return y;
}

/* ---- envelope curve (releaseTable) --------------------------------------------------------------------- */

/* The OS's per-sample decay multiplier for a knob word (params[3] decay, params[5] release): level *= c each
 * sample. The OS uses max(c, releaseTable[1]) for decay. */
static inline fract32 opz_host_release_coef(const fract32 *releaseTable, uint32_t param)
{
    return releaseTable[(param >> 24) & (OPZ_RELEASE_LEN - 1)];
}

/* ---- format check, once in initModule ------------------------------------------------------------------ */

enum { OPZ_HT_KLP = 1, OPZ_HT_SINE = 2, OPZ_HT_SINE16 = 4, OPZ_HT_RF = 8, OPZ_HT_RELEASE = 16, OPZ_HT_ALL = 31 };

/* |v - want| <= tol, without signed overflow. */
static inline int opz_host_near_(int32_t v, int32_t want, uint32_t tol)
{
    return (uint32_t)v - (uint32_t)want + tol <= 2 * tol;
}

/* Per-table checks: present and matching the formats above at a few points (values of our formulas; 64 LSB
 * tolerance for the Q31 tables, 1 for g_sineTable). Call the ones for the tables you read, once in initModule,
 * and return 0 if one fails: the OS then reports "Failed to initialize module" instead of the engine playing
 * garbage on an OS whose tables changed. Each is 4-8 loads and compares (~60-100 bytes of code). */
static inline int opz_host_check_klp(const ModuleCallbackInterface *cb)
{
    const fract32 *k = cb->k_lp_table;
    return k && opz_host_near_(k[0], 4748738, 64) && opz_host_near_(k[64], 202308859, 64)
           && opz_host_near_(k[128], 1958517252, 64);
}
static inline int opz_host_check_sine(const ModuleCallbackInterface *cb)
{
    const fract32 *s = cb->g_sine;
    return s && opz_host_near_(s[0], 0, 64) && opz_host_near_(s[1024], 1518453909, 64)
           && opz_host_near_(s[2048], OPZ_SINE_PEAK, 64) && opz_host_near_(s[6144], -OPZ_SINE_PEAK, 64)
           && opz_host_near_(s[8192], 0, 64);
}
static inline int opz_host_check_sine16(const ModuleCallbackInterface *cb)
{
    const int16_t *s = cb->g_sineTable;
    return s && opz_host_near_(s[0], 0, 1) && opz_host_near_(s[32], 23170, 1) && opz_host_near_(s[64], 32767, 1)
           && opz_host_near_(s[192], -32768, 1);
}
static inline int opz_host_check_rf(const ModuleCallbackInterface *cb)
{
    const fract32 *const *rf = cb->rFilterCoeffs;
    return rf && rf[0] && rf[1] && rf[2] && rf[3] && rf[4]
           && opz_host_near_(rf[OPZ_BQ_B0][4 * OPZ_RF_COLS + OPZ_RF_COL_FLAT], 0x7fffffff, 64)
           && opz_host_near_(rf[OPZ_BQ_A2][4 * OPZ_RF_COLS + OPZ_RF_COL_FLAT], 0, 64)
           && opz_host_near_(rf[OPZ_BQ_B0][4 * OPZ_RF_COLS + OPZ_RF_COL_LP_1KHZ], 10158494, 64)
           && opz_host_near_(rf[OPZ_BQ_A2][8 * OPZ_RF_COLS + OPZ_RF_COL_HP_20KHZ], 1998267468, 64);
}
static inline int opz_host_check_release(const ModuleCallbackInterface *cb)
{
    const fract32 *r = cb->releaseTable;
    return r && opz_host_near_(r[1], 2139317552, 64) && opz_host_near_(r[64], 2147363203, 64)
           && opz_host_near_(r[127], 2147476874, 64);
}

/* All five as a bit mask (OPZ_HT_ALL when every table is as documented): for tests and tools; engines call only
 * the checks they need. */
static inline uint32_t opz_host_tables_check(const ModuleCallbackInterface *cb)
{
    if (!cb) return 0;
    return (opz_host_check_klp(cb) ? OPZ_HT_KLP : 0) | (opz_host_check_sine(cb) ? OPZ_HT_SINE : 0)
           | (opz_host_check_sine16(cb) ? OPZ_HT_SINE16 : 0) | (opz_host_check_rf(cb) ? OPZ_HT_RF : 0)
           | (opz_host_check_release(cb) ? OPZ_HT_RELEASE : 0);
}

#ifdef __cplusplus
}
#endif

#endif /* OPZ_TABLES_H */
