/*
 * output.c — WAV writer and render statistics (level, DC, clipping, octave-band spectrum).
 * The statistics feed the spectral tolerance check of the golden tests; they need not be bit-exact.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static void put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

int wav_write(const char *path, const fract32 *x, int64_t frames, int bits)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ch = 2, bytes = bits / 8;
    uint32_t data = (uint32_t)(frames * ch * bytes);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1); /* PCM */
    put16(f, ch);
    put32(f, OPZ_SAMPLE_RATE);
    put32(f, OPZ_SAMPLE_RATE * ch * bytes);
    put16(f, ch * bytes);
    put16(f, bits);
    fwrite("data", 1, 4, f);
    put32(f, data);
    for (int64_t i = 0; i < frames * ch; i++) {
        if (bits == 16) {
            int64_t v = ((int64_t)x[i] + 0x8000) >> 16;
            put16(f, (uint16_t)(int16_t)(v > 32767 ? 32767 : v));
        } else {
            put32(f, (uint32_t)x[i]);
        }
    }
    return fclose(f);
}

static double to_db(double v) { return v > 1e-12 ? 20.0 * log10(v) : -240.0; }

double segment_rms_dbfs(const fract32 *x, int64_t from, int64_t to)
{
    double acc = 0;
    if (to <= from) return -240.0;
    for (int64_t i = from * 2; i < to * 2; i++) {
        double v = x[i] / 2147483648.0;
        acc += v * v;
    }
    return to_db(sqrt(acc / (double)((to - from) * 2)));
}

double segment_peak_dbfs(const fract32 *x, int64_t from, int64_t to)
{
    double pk = 0;
    for (int64_t i = from * 2; i < to * 2; i++) {
        double v = fabs(x[i] / 2147483648.0);
        if (v > pk) pk = v;
    }
    return to_db(pk);
}

#define FFT_N 2048

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2.0 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
    }
}

void stats_compute(const fract32 *x, int64_t frames, SimStats *st)
{
    memset(st, 0, sizeof *st);
    double sum = 0;
    for (int64_t i = 0; i < frames * 2; i++) {
        sum += x[i] / 2147483648.0;
        if (x[i] == INT32_MAX || x[i] == INT32_MIN) st->clipped++;
    }
    st->dc = frames ? sum / (double)(frames * 2) : 0;
    st->rms_dbfs = segment_rms_dbfs(x, 0, frames);
    st->peak_dbfs = segment_peak_dbfs(x, 0, frames);

    double band[10] = { 0 };
    double *re = malloc(sizeof(double) * FFT_N), *im = malloc(sizeof(double) * FFT_N);
    int64_t nframes = 0;
    for (int64_t start = 0; start + FFT_N <= frames; start += FFT_N, nframes++) {
        for (int i = 0; i < FFT_N; i++) {
            double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (FFT_N - 1));
            re[i] = w * (x[(start + i) * 2] + (double)x[(start + i) * 2 + 1]) / 4294967296.0;
            im[i] = 0;
        }
        fft(re, im, FFT_N);
        for (int k = 1; k < FFT_N / 2; k++) {
            double f = (double)k * OPZ_SAMPLE_RATE / FFT_N;
            int b = (int)floor(log2(f / 31.25) + 0.5);
            if (b < 0) b = 0;
            if (b > 9) b = 9;
            band[b] += re[k] * re[k] + im[k] * im[k];
        }
    }
    for (int b = 0; b < 10; b++) {
        double p = nframes ? band[b] / (double)nframes : 0;
        st->bands_db[b] = p > 1e-24 ? 10.0 * log10(p) : -240.0;
    }
    free(re);
    free(im);
}
