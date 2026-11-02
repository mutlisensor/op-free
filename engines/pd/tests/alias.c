/* Aliasing check of pd on the host: held notes on every wave, rendered by the engine's own code (opzsim's envelope,
 * sustain full), 32,768 samples after the attack through a 4-term Blackman-Harris window (sidelobes -92 dB).
 * Reports, per wave and note, the strongest component that is not a harmonic of the note (more than 8 bins from
 * every multiple of f), below 11 kHz and anywhere, in dB relative to the strongest harmonic.
 *
 *   cc -O2 -I sdk/include -I sim/src -o /tmp/pd_alias engines/pd/tests/alias.c sim/src/callbacks.c -lm && \
 *       /tmp/pd_alias [P1]                     (default P1 255; double sine is measured at P1 150, its worst)
 *
 * Add -DPD_NO_BLAMP to hear what double sine does without its polyBLAMP. Exit 1 if anything below 11 kHz is
 * stronger than LIMIT_DB.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define initModule pd_initModule
#define init pd_init
#define onNoteOn pd_onNoteOn
#define onNoteOff pd_onNoteOff
#define render pd_render
#include "../pd.c"
#include "host.h"

#define LEN 32768
#define SETTLE 64                                    /* blocks before the capture: the attack and DCW are over */
#define LIMIT_DB -50.0

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
static void measure(int p1, int p2, int note, double worst[2])
{
    static int ready;
    static ModuleCallbackInterface cb;
    if (!ready) {
        uint32_t mem;
        sim_callbacks_init(&cb);
        pd_initModule(0, &cb, &mem);
        ready = 1;
    }
    uint8_t state[OPZ_VOICE_STATE_BYTES];
    SimADSR env;
    memset(&env, 0, sizeof env);
    pd_init(state, 5, 0, NULL, NULL);
    uint32_t params[OPZ_NUM_PARAMS];
    const uint8_t bytes[OPZ_NUM_PARAMS] = { 0, 0, 0, 0, 255, 64, 0, 0, 128, 0, 128, 200, 0, 128, 100, 255, 0, 0 };
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte(bytes[i]);
    params[OPZ_P_P1] = opz_param_from_byte((uint8_t)p1);
    params[OPZ_P_P2] = opz_param_from_byte((uint8_t)p2);
    double f = 440.0 * pow(2.0, (note - 69) / 12.0);
    uint32_t pitch = (uint32_t)llround(f / 44100.0 * 4294967296.0);
    f = pitch / 4294967296.0 * 44100.0;
    sim_env_note_on(&env);
    pd_onNoteOn(note, 100, state, params);
    fract32 out[2 * N];
    for (int b = 0; b < SETTLE + LEN / N; b++) {
        memset(out, 0, sizeof out);
        pd_render(out, pitch, 0, state, params, (ADSR *)&env);
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
        if (fk < 30.0) continue;                       /* the window's skirt around DC */
        if (p > all) all = p;
        if (fk < 11000.0 && p > low) low = p;
    }
    worst[0] = 10 * log10((low + 1e-30) / harm);
    worst[1] = 10 * log10((all + 1e-30) / harm);
}

int main(int argc, char **argv)
{
    int p1 = argc > 1 ? atoi(argv[1]) : 255;
    static const int p2s[] = { 0, 36, 73, 109, 146, 182, 219, 255 };
    static const char *names[] = { "saw", "square", "pulse", "dbl sine", "saw-pulse", "res I", "res II", "res III" };
    static const int notes[] = { 24, 36, 48, 60, 69, 76, 84, 91, 96, 103, 108 };
    printf("strongest non-harmonic component re the strongest harmonic, dB: below 11 kHz / anywhere (P1 %d; "
           "double sine at P1 %d)\nnote", p1, argc > 1 ? p1 : 150);
    for (int w = 0; w < 8; w++) printf(" %11s", names[w]);
    printf("\n");
    double worst = -999;
    for (unsigned n = 0; n < sizeof notes / sizeof notes[0]; n++) {
        printf("%4d", notes[n]);
        for (int w = 0; w < 8; w++) {
            double r[2];
            measure(w == DBL && argc <= 1 ? 150 : p1, p2s[w], notes[n], r);
            printf("   %4.0f/%4.0f", r[0], r[1]);
            if (r[0] > worst) worst = r[0];
        }
        printf("\n");
    }
    printf("worst below 11 kHz: %.1f dB (limit %.0f): %s\n", worst, LIMIT_DB, worst > LIMIT_DB ? "FAIL" : "ok");
    return worst > LIMIT_DB;
}
