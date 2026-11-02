/*
 * opz_tables_gen.h — the OS's shared DSP tables, generated from their formulas, for hosts that stand in for the OS.
 *
 * opzsim (sim/src/callbacks.c) and engine unit tests use this to fill a ModuleCallbackInterface with tables in the
 * device format (opz_tables.h; sdk/docs/engine-abi.md §5). tools/opz_tables.py implements the same formulas for
 * the cycle driver; sim/tests/golden.py checks that the two agree. Our formulas, no TE data: against OS 1.2.45 they
 * agree to within 1 LSB, k_lp_table to within 11 LSB.
 *
 * Double precision with our own libm-free sin/exp/sqrt (build with -ffp-contract=off), so every Mac gives the same
 * bits. Not for an engine's device build: engines read the OS's tables through the callback table.
 * MIT licence, see CONTRIBUTING.md.
 */
#ifndef OPZ_TABLES_GEN_H
#define OPZ_TABLES_GEN_H

#include "opz_tables.h"

#if defined(__bfin__)
#error "opz_tables_gen.h is for host builds; on the device use the tables the OS passes to initModule"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OpzTablesGen {
    fract32 k_lp[OPZ_KLP_LEN];
    fract32 sine[OPZ_SINE_LEN + 1];
    int16_t sine16[OPZ_SINE16_LEN];
    fract32 rf[5][OPZ_RF_ROWS * OPZ_RF_COLS];
    const fract32 *rf_ptr[5];
    fract32 release[OPZ_RELEASE_LEN];
} OpzTablesGen;

#define OPZ_GEN_PI 3.14159265358979323846

__attribute__((unused)) static double opz_gen_sin(double x)
{
    double turns = x / (2.0 * OPZ_GEN_PI);
    int64_t k = (int64_t)(turns + (turns >= 0 ? 0.5 : -0.5));
    x -= (double)k * 2.0 * OPZ_GEN_PI;                     /* [-pi, pi] */
    if (x > OPZ_GEN_PI / 2) x = OPZ_GEN_PI - x;
    if (x < -OPZ_GEN_PI / 2) x = -OPZ_GEN_PI - x;          /* [-pi/2, pi/2] */
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 20; n++) {
        term = -term * x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

__attribute__((unused)) static double opz_gen_cos(double x)
{
    double turns = x / (2.0 * OPZ_GEN_PI);
    int64_t k = (int64_t)(turns + (turns >= 0 ? 0.5 : -0.5));
    x -= (double)k * 2.0 * OPZ_GEN_PI;                     /* [-pi, pi], cos even */
    if (x < 0) x = -x;
    double sign = 1.0;
    if (x > OPZ_GEN_PI / 2) { x = OPZ_GEN_PI - x; sign = -1.0; }
    double term = 1.0, sum = 1.0, x2 = x * x;
    for (int n = 1; n < 20; n++) {
        term = -term * x2 / (double)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sign * sum;
}

__attribute__((unused)) static double opz_gen_exp(double x)
{
    const double ln2 = 0.69314718055994530942;
    int64_t n = (int64_t)(x / ln2);
    if ((double)n * ln2 > x) n--;                          /* floor */
    double f = x - (double)n * ln2, term = 1.0, sum = 1.0;  /* e^f, f in [0, ln2) */
    for (int i = 1; i < 30; i++) {
        term = term * f / (double)i;
        sum += term;
    }
    for (; n > 0; n--) sum *= 2.0;
    for (; n < 0; n++) sum *= 0.5;
    return sum;
}

__attribute__((unused)) static double opz_gen_sqrt(double a)
{
    if (a <= 0) return 0;
    double x = a > 1 ? a : 1;
    for (int i = 0; i < 200; i++) {
        double y = 0.5 * (x + a / x);
        if (y >= x) break;
        x = y;
    }
    return x;
}

/* round half away from zero */
__attribute__((unused)) static int64_t opz_gen_round(double v)
{
    return v >= 0 ? (int64_t)(v + 0.5) : -(int64_t)(-v + 0.5);
}

/* x * 2^31 rounded to nearest, saturated */
__attribute__((unused)) static fract32 opz_gen_q31(double x)
{
    double v = x * 2147483648.0;
    if (v >= 2147483647.0) return 0x7fffffff;
    if (v <= -2147483648.0) return INT32_MIN;
    int64_t r = opz_gen_round(v);
    return r > 0x7fffffff ? 0x7fffffff : (fract32)r;
}

/* (b0, b1/2, b2, a1/2, a2) of rFilterCoeffs at [row][col]: RBJ low-pass (columns 0-29) / high-pass (35-64) at the
 * ISO third-octaves, flat 30-34. */
__attribute__((unused)) static void opz_gen_rf_column(int col, int row, double c[5])
{
    static const double freqs[30] = { 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                      1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500,
                                      16000, 20000 };
    static const double q_lp[9] = { 0.2, 0.4, 0.6, 0.8, 1.0, 4.0, 8.0, 16.0, 16.0 };
    static const double q_hp[9] = { 0.2, 0.4, 0.6, 0.8, 1.0, 2.0, 3.0, 4.0, 4.0 };
    static const double flat[5][5] = { { 1, .25, .5, .25, .5 }, { 1, 0, 0, 0, 0 }, { 1, 0, 0, 0, 0 },
                                       { 1, 0, 0, 0, 0 }, { 1, -.75, .5, -.75, .5 } };
    if (col >= 30 && col <= 34) {
        for (int k = 0; k < 5; k++) c[k] = flat[col - 30][k];
        return;
    }
    int lp = col < 30;
    double f = freqs[lp ? col : col - 35], q = lp ? q_lp[row] : q_hp[row];
    double w = 2 * OPZ_GEN_PI * f / OPZ_SAMPLE_RATE;
    double cw = opz_gen_cos(w), al = opz_gen_sin(w) / (2 * q), a0 = 1 + al;
    double b0 = lp ? (1 - cw) / 2 : (1 + cw) / 2, b1 = lp ? (1 - cw) : -(1 + cw);
    c[0] = b0 / a0;
    c[1] = b1 / 2 / a0;
    c[2] = b0 / a0;
    c[3] = -cw / a0;
    c[4] = (1 - al) / a0;
}

/* Fill every table. ~12k sines; a few milliseconds on a Mac. */
__attribute__((unused)) static void opz_tables_generate(OpzTablesGen *t)
{
    const double ln2000 = 7.6009024595420822;
    const double g2 = 0.70710678118654752440;             /* each of two poles -1.5 dB: |H|^2 = 2^(-1/2) */
    for (int i = 0; i < OPZ_KLP_LEN; i++) {
        double w = 2 * OPZ_GEN_PI * (10.0 * opz_gen_exp(i / 128.0 * ln2000)) / OPZ_SAMPLE_RATE;
        double a = (1 - g2 * opz_gen_cos(w)) / (1 - g2);
        t->k_lp[i] = opz_gen_q31(1 - (a - opz_gen_sqrt(a * a - 1)));
    }
    for (int i = 0; i <= OPZ_SINE_LEN; i++)
        t->sine[i] = (fract32)opz_gen_round(OPZ_SINE_PEAK * opz_gen_sin(2 * OPZ_GEN_PI * i / OPZ_SINE_LEN));
    for (int k = 0; k < OPZ_SINE16_LEN; k++) {
        int64_t r = opz_gen_round(32768 * opz_gen_sin(2 * OPZ_GEN_PI * k / OPZ_SINE16_LEN));
        t->sine16[k] = (int16_t)(r > 32767 ? 32767 : (r < -32768 ? -32768 : r));
    }
    for (int row = 0; row < OPZ_RF_ROWS; row++)
        for (int col = 0; col < OPZ_RF_COLS; col++) {
            double c[5];
            opz_gen_rf_column(col, row, c);
            for (int k = 0; k < 5; k++) t->rf[k][row * OPZ_RF_COLS + col] = opz_gen_q31(c[k]);
        }
    for (int k = 0; k < 5; k++) t->rf_ptr[k] = t->rf[k];
    /* releaseTable: e^(-1/tau), tau in samples; our least-squares fit of the curve */
    for (int i = 0; i < OPZ_RELEASE_LEN; i++) {
        double tau = i == 0 ? 33.64711 : 0.9581 + 248.8227 * i + 11.72398 * opz_gen_exp(0.079528 * i);
        t->release[i] = opz_gen_q31(opz_gen_exp(-1.0 / tau));
    }
}

/* Point cb's five table entries at t (generate it first). */
__attribute__((unused)) static void opz_tables_install(ModuleCallbackInterface *cb, const OpzTablesGen *t)
{
    cb->k_lp_table = t->k_lp;
    cb->g_sine = t->sine;
    cb->g_sineTable = t->sine16;
    cb->rFilterCoeffs = t->rf_ptr;
    cb->releaseTable = t->release;
}

#ifdef __cplusplus
}
#endif

#endif /* OPZ_TABLES_GEN_H */
