/* Aliasing check of hats' squares on the host: one square at a time from the engine's own edge code (squares()
 * and the running sum, as render does), 32,768 samples through a 4-term Blackman-Harris window (sidelobes
 * -92 dB), against a naive square (the sign of a phase) at the same frequency. Reports the strongest component
 * that is not a harmonic of the square (more than 8 bins from every multiple of f), below 11 kHz and below 4 kHz,
 * in dB relative to the fundamental.
 *
 *   cc -O2 -I sdk/include -o /tmp/hats_alias engines/hats/tests/alias.c -lm && /tmp/hats_alias
 *
 * Exit 1 if the band-limited square has an alias above -40 dB below 11 kHz or above -55 dB below 4 kHz (where an
 * alias would whistle under a hat's band) at any tested frequency, 2-11 kHz (the brightest squares hats plays).
 * The triangle kernel's sinc^2 is only ~-22 dB at the images that fold to 8-11 kHz (33-36 kHz), so those come out
 * near -43 dB; the bands hats uses there are full of real harmonics ~30 dB louder.
 */
#include <math.h>
#include <stdio.h>
#include "../hats.c"

#define LEN 32768
#define LIMIT_DB -40.0                               /* below 11 kHz */
#define LIMIT_LOW_DB -55.0                           /* below 4 kHz */

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
                double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr;
                im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr;
                im[i + k] += xi;
            }
    }
}

/* Worst non-harmonic component (dB re the fundamental) below lim Hz. */
static double worst(const double *x, double f, double lim)
{
    for (int i = 0; i < LEN; i++) {
        double t = 2 * M_PI * i / (LEN - 1);
        double w = 0.35875 - 0.48829 * cos(t) + 0.14128 * cos(2 * t) - 0.01168 * cos(3 * t);
        s_re[i] = x[i] * w;
        s_im[i] = 0;
    }
    fft(s_re, s_im, LEN);
    double bin = 44100.0 / LEN, fund = 0, bad = 0;
    for (int k = 1; k < LEN / 2; k++) {
        double p = s_re[k] * s_re[k] + s_im[k] * s_im[k], hz = k * bin;
        double h = hz / f, dist = fabs(h - floor(h + 0.5)) * f / bin;
        if (fabs(hz - f) <= 8 * bin) { if (p > fund) fund = p; continue; }
        if (dist <= 8 && floor(h + 0.5) >= 1) continue;
        if (hz < lim && p > bad) bad = p;
    }
    return 10 * log10(bad / fund);
}

static double s_x[LEN], s_n[LEN];

int main(void)
{
    static const double freqs[] = { 2017.0, 3089.0, 4513.0, 6007.0, 8191.0, 11025.0 * 0.99 };
    int fail = 0;
    printf("square Hz   band-limited (<11k, <4k)   naive (<11k, <4k)\n");
    for (unsigned q = 0; q < sizeof freqs / sizeof freqs[0]; q++) {
        double f = freqs[q];
        State s;
        for (unsigned i = 0; i < sizeof s; i++) ((uint8_t *)&s)[i] = 0;
        s.hp[0] = (int32_t)(22050.0 / f * 65536.0 + 0.5);
        for (int i = 1; i < 6; i++) { s.hp[i] = MAX_HP; s.t[i] = MAX_HP - 1; }   /* silent: no edges */
        double hpf = s.hp[0] / 65536.0;                                       /* the frequency it really plays */
        f = 22050.0 / hpf;
        int32_t y = AMP;
        for (int b = 0; b < LEN / OPZ_BLOCK_FRAMES; b++) {
            for (int i = 1; i < 6; i++) s.t[i] = MAX_HP - 1;                 /* (they count down every block) */
            squares(&s);
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                y += (int32_t)s_d[i];
                s_x[b * OPZ_BLOCK_FRAMES + i] = y / (double)AMP;
            }
        }
        for (int i = 0; i < LEN; i++) s_n[i] = fmod(i * f / 44100.0, 1.0) < 0.5 ? 1.0 : -1.0;
        double a11 = worst(s_x, f, 11000), a4 = worst(s_x, f, 4000);
        double n11 = worst(s_n, f, 11000), n4 = worst(s_n, f, 4000);
        printf("%8.0f   %8.1f dB %8.1f dB   %8.1f dB %8.1f dB\n", f, a11, a4, n11, n4);
        if (a11 > LIMIT_DB || a4 > LIMIT_LOW_DB) fail = 1;
    }
    return fail;
}
