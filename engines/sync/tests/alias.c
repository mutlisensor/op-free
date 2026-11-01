/* Sync aliasing, measured on the engine's own render (host build):
 *
 *   clang -O2 -I sdk/include -o /tmp/sync_alias engines/sync/tests/alias.c -lm && /tmp/sync_alias
 *   add -DSYNC_BLEP2 (2-point PolyBLEP) or -DSYNC_BLEP0 (naive steps) to compare; -DSLAVE_MAX=0x... moves the cap
 *
 * Each tone is a master frequency on an exact FFT bin (inc = k 2^17, 32,768-point FFT), so the synced waveform
 * repeats exactly every 32,768 samples and every harmonic of the master lands on a multiple of bin k: everything
 * else is aliasing. Reported per tone (P2 0: no sub, no noise; velocity 127; constant envelope):
 *   total   alias power over all harmonic power, whole band (dB)
 *   <5k     alias power below 5 kHz (where it is least masked) over all harmonic power (dB)
 *   <11k    the same below 11 kHz
 *   worst   the strongest alias component below 11 kHz over the strongest harmonic (dB), as table measured it
 *   dc      the output's mean, dB re full scale
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define init sync_init
#define render sync_render
#define initModule sync_initModule
#define onNoteOn sync_onNoteOn
#define onNoteOff sync_onNoteOff
#include "../sync.c"

#define NFFT 32768
static int g_vel = 127;

static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r) { (void)env; (void)a; (void)d; (void)s; (void)r; }

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}

static double re_[NFFT], im_[NFFT];

/* one tone: master bin k, P1 and P2 bytes; returns the effective ratio */
static double tone(int k, int p1, int p2, double *total, double *low, double *mid, double *worst, double *dcdb)
{
    ModuleCallbackInterface cb;
    memset(&cb, 0, sizeof cb);
    cb.updateParametersADSR = cb_adsr;
    uint32_t mem;
    sync_initModule(0, &cb, &mem);
    State st;
    sync_init(&st, 5, 0, 0, 0);
    uint32_t params[18] = { 0 };
    params[0] = opz_param_from_byte((uint8_t)p1);
    params[1] = opz_param_from_byte((uint8_t)p2);
    sync_onNoteOn(60, g_vel, &st, params);
    int32_t envw[8] = { 0, 0x7fffffff, 0x7fffffff, 0, 3, 0, 0, 0 };
    uint32_t inc = (uint32_t)k << 17;
    static fract32 out[2 * N];
    double mean = 0;
    for (int b = 0; b < 16 + NFFT / N; b++) {
        memset(out, 0, sizeof out);
        sync_render(out, inc, 0, &st, params, (ADSR *)envw);
        if (b < 16) continue;
        for (int i = 0; i < N; i++) {
            re_[(b - 16) * N + i] = out[2 * i] / 2147483648.0;
            im_[(b - 16) * N + i] = 0;
            mean += out[2 * i] / 2147483648.0;
        }
    }
    mean /= NFFT;
    *dcdb = 20 * log10(fabs(mean) + 1e-12);
    fft(re_, im_, NFFT);
    double harm = 0, alias = 0, alow = 0, amid = 0, hmax = 0, amax = 0;
    for (int i = 1; i < NFFT / 2; i++) {
        double p = re_[i] * re_[i] + im_[i] * im_[i];
        double hz = i * 44100.0 / NFFT;
        if (i % k == 0) { harm += p; if (p > hmax) hmax = p; }
        else {
            alias += p;
            if (hz < 5000) alow += p;
            if (hz < 11000) amid += p;
            if (hz < 11000 && p > amax) amax = p;
        }
    }
    *total = 10 * log10(alias / harm + 1e-30);
    *low = 10 * log10(alow / harm + 1e-30);
    *mid = 10 * log10(amid / harm + 1e-30);
    *worst = 10 * log10(amax / hmax + 1e-30);
    uint32_t x = (uint32_t)opz_p16(params[0]);
    int32_t oct = (int32_t)((x << 2) + ((x * x) >> 14)) + st.voct;
    if (oct < 0) oct = 0;
    if (oct > OCT_MAX) oct = OCT_MAX;
    double is = inc * pow(2.0, oct / 65536.0);
    if (is > SLAVE_MAX) is = SLAVE_MAX;
    if (is < inc) is = inc;
    return is / inc;
}

int main(int argc, char **argv)
{
    if (argc > 1) g_vel = atoi(argv[1]);
    static const int ks[] = { 82, 327, 778, 1308, 2489 };          /* 110, 440, 1047, 1760, 3350 Hz */
    static const int p1s[] = { 0, 40, 80, 120, 160, 200, 255 };
    printf("%-8s %-5s %-7s %-9s %8s %8s %8s %8s %8s\n", "master", "P1", "ratio", "slave", "total", "<5k", "<11k",
           "worst", "dc");
    for (unsigned a = 0; a < sizeof ks / sizeof ks[0]; a++)
        for (unsigned c = 0; c < sizeof p1s / sizeof p1s[0]; c++) {
            double t, l, m, w, dc;
            double r = tone(ks[a], p1s[c], 0, &t, &l, &m, &w, &dc);
            double f0 = ks[a] * 44100.0 / NFFT;
            printf("%6.0f   %-5d %-7.2f %7.0f   %8.1f %8.1f %8.1f %8.1f %8.1f\n", f0, p1s[c], r, f0 * r, t, l, m, w, dc);
        }
    return 0;
}
