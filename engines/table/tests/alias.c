/* Aliasing check of table on the host: held notes on the saw and the brightest waves, rendered by the engine's own
 * code (opzsim's envelope, sustain full), 32,768 samples after the attack through a 4-term Blackman-Harris window
 * (sidelobes -92 dB). Reports, per wave and note, the strongest component that is not a harmonic of the note (more
 * than 8 bins from every multiple of f), below 11 kHz and anywhere, in dB relative to the strongest harmonic, and
 * which path plays the note (a table mip, or "add": the additive path above the last mip).
 *
 *   cc -O2 -I sdk/include -I sim/src -o /tmp/table_alias engines/table/tests/alias.c sim/src/callbacks.c -lm && \
 *       /tmp/table_alias [first_note last_note]          (default A5 81 .. F7 101)
 *
 * Exit 1 if a note on the additive path has anything above -70 dB below 11 kHz (the table mips are listed, not
 * judged: their images fold down to -43 dB on the brightest waves in the top mip).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define initModule table_initModule
#define init table_init
#define onNoteOn table_onNoteOn
#define onNoteOff table_onNoteOff
#define render table_render
#include "../table.c"
#include "host.h"

#define LEN 32768
#define SETTLE 8                                     /* blocks before the capture: the attack is over */
#define LIMIT_DB -70.0

static double s_re[LEN], s_im[LEN];

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double *xr = re + i + k, *xi = im + i + k, *yr = xr + len / 2, *yi = xi + len / 2;
                double tr = *yr * wr - *yi * wi, ti = *yr * wi + *yi * wr;
                *yr = *xr - tr; *yi = *xi - ti;
                *xr += tr; *xi += ti;
            }
    }
}

/* worst[0]: below 11 kHz, worst[1]: anywhere below fs / 2; dB re the strongest harmonic */
static void measure(int bank, int wave, int note, double worst[2])
{
    static int ready;
    static ModuleCallbackInterface cb;
    if (!ready) {
        uint32_t mem;
        sim_callbacks_init(&cb);
        table_initModule(0, &cb, &mem);
        ready = 1;
    }
    uint8_t state[OPZ_VOICE_STATE_BYTES];
    SimADSR env;
    memset(&env, 0, sizeof env);
    table_init(state, 4, 0, NULL, NULL);
    uint32_t params[OPZ_NUM_PARAMS];
    const uint8_t bytes[OPZ_NUM_PARAMS] = { 0, 0, 0, 64, 255, 64, 0, 0, 128, 0, 128, 200, 0, 0, 64, 0, 0, 0 };
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte(bytes[i]);
    params[OPZ_P_P1] = (uint32_t)(wave * 65536 / (TW_WAVES - 1)) << 15;   /* on the wave, velocity 100: no offset */
    params[OPZ_P_P2] = (uint32_t)(bank * 65536 / (TW_BANKS - 1)) << 15;
    if (wave == TW_WAVES - 1) params[OPZ_P_P1] = 0x7fffffff;
    if (bank == TW_BANKS - 1) params[OPZ_P_P2] = 0x7fffffff;
    double f = 440.0 * pow(2.0, (note - 69) / 12.0);
    uint32_t pitch = (uint32_t)llround(f / 44100.0 * 4294967296.0);
    f = (pitch > INC_MAX ? INC_MAX : pitch) / 4294967296.0 * 44100.0;          /* the engine caps it at 11 kHz */
    sim_env_note_on(&env);
    table_onNoteOn(note, 100, state, params);
    fract32 out[2 * N];
    for (int b = 0; b < SETTLE + LEN / N; b++) {
        memset(out, 0, sizeof out);
        table_render(out, pitch, 0, state, params, (ADSR *)&env);
        if (b >= SETTLE)
            for (int i = 0; i < N; i++) {
                int k = (b - SETTLE) * N + i;
                double t = 2 * M_PI * k / LEN;
                double w = 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t);
                s_re[k] = out[2 * i] / 2147483648.0 * w;
                s_im[k] = 0;
            }
    }
    fft(s_re, s_im, LEN);
    double bin = 44100.0 / LEN, harm = 0, low = 0, all = 0;
    for (int k = 1; k < LEN / 2; k++) {
        double p = s_re[k] * s_re[k] + s_im[k] * s_im[k];
        double fk = k * bin;
        double m = floor(fk / f + 0.5);
        if (fabs(fk - m * f) <= 8 * bin) {
            if (m >= 1 && p > harm) harm = p;
            continue;
        }
        if (p > all) all = p;
        if (fk < 11000.0 && p > low) low = p;
    }
    worst[0] = 10 * log10((low + 1e-30) / harm);
    worst[1] = 10 * log10((all + 1e-30) / harm);
}

int main(int argc, char **argv)
{
    int lo = argc > 2 ? atoi(argv[1]) : 81, hi = argc > 2 ? atoi(argv[2]) : 101;
    static const int waves[][2] = { { 0, 2 }, { 0, 5 }, { 1, 5 }, { 2, 5 }, { 3, 5 }, { 4, 5 }, { 5, 5 }, { 6, 5 } };
    static const char *names[] = { "analog", "organ", "vocal", "reso", "sync", "fm", "metal" };
    int fail = 0;
    printf("strongest non-harmonic component re the strongest harmonic, dB: below 11 kHz / anywhere\n");
    printf("note path ");
    for (unsigned w = 0; w < sizeof waves / sizeof waves[0]; w++) printf(" %8s%d   ", names[waves[w][0]], waves[w][1]);
    printf("\n");
    double worst_add = -999, worst_mip = -999;
    for (int note = lo; note <= hi; note++) {
        uint32_t inc = (uint32_t)llround(440.0 * pow(2.0, (note - 69) / 12.0) / 44100.0 * 4294967296.0);
        int m = 0;
        while (m < TW_MIPS && inc > k_tw_inc[m]) m++;
        if (m == MIP_ADD) printf("%4d  add ", note);
        else printf("%4d mip%d ", note, m);
        for (unsigned w = 0; w < sizeof waves / sizeof waves[0]; w++) {
            double r[2];
            measure(waves[w][0], waves[w][1], note, r);
            printf(" %5.0f/%5.0f ", r[0], r[1]);
            double *worst = m == MIP_ADD ? &worst_add : &worst_mip;
            if (r[0] > *worst) *worst = r[0];
            if (m == MIP_ADD && r[0] > LIMIT_DB) fail = 1;
        }
        printf("\n");
    }
    if (worst_mip > -999) printf("worst below 11 kHz on the table mips: %.1f dB\n", worst_mip);
    if (worst_add > -999)
        printf("worst below 11 kHz on the additive path: %.1f dB (limit %.0f): %s\n", worst_add, LIMIT_DB,
               fail ? "FAIL" : "ok");
    return fail;
}
