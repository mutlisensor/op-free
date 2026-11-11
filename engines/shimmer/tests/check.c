/* Host checks of shimmer, outside opzsim: a fuzz run and a set of measurements.
 *
 *   clang -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -I sdk/include -o /tmp/shimmer_check \
 *       engines/shimmer/tests/check.c
 *   DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib /tmp/shimmer_check fuzz        (and with MALLOC_PROTECT_BEFORE=1)
 *   /tmp/shimmer_check measure
 *
 * (Apple clang 14's AddressSanitizer doesn't start on macOS 26; guard malloc puts each allocation against an
 * unmapped page instead, so a read or write past the buffer or the state area crashes. Add -fsanitize=address
 * where it works.)
 *
 * fuzz: 20 runs of 60 s each with random knob moves every block (jumps, extremes, fast sweeps), notes (any number,
 * 255, the OS's velocity-0 re-send with params NULL, note-offs), random dry/wet and level, and input that switches
 * between silence, full-scale noise, loud saws, sines and clicks. The state has a 256-byte guard after its 204
 * bytes and the buffer is exactly memRequest bytes (guard malloc or ASan catch any access past either), filled with 0x5a
 * as the OS's pool would be. Checks: the guard is intact, the state stays in range, the wet output never sits at
 * full scale (no runaway) and every run ends in silence after its input stops. Then the worst case for 120 s: P1 255,
 * P2 255, a full-scale saw, each interval in turn.
 * measure: decay times per P1 and P2, the shimmer's octave build-up per P2, the interval mapping, the limiter's
 * level at the endless top, aliasing of a high sine, a P1/P2 sweep for clicks.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shimmer.c"

#define FS 44100

typedef struct {
    uint8_t *area;                              /* 204 B of state + 256 B guard */
    Mem *mem;
    uint32_t target[OPZ_NUM_PARAMS], cur[OPZ_NUM_PARAMS];
    int32_t gain;
    int32_t in[2 * BLK], out[2 * BLK];
} Box;

static uint32_t s_seed = 12345;
static uint32_t rnd(void) { s_seed = s_seed * 1664525u + 1013904223u; return s_seed; }
static int rndn(int n) { return (int)((rnd() >> 8) % (uint32_t)n); }

static void box_open(Box *b, uint32_t track)
{
    static uint8_t scratch[1024];
    b->area = malloc(OPZ_FX_STATE_BYTES + 256);
    memset(b->area, 0x5a, OPZ_FX_STATE_BYTES + 256);
    uint32_t req = 0;
    if (!initModule(0, 0, &req) || req != MEM_BYTES) { printf("FAIL initModule\n"); exit(1); }
    if (req > OPZ_FX2_POOL_BYTES) { printf("FAIL memRequest %u > FX2 pool\n", req); exit(1); }
    b->mem = malloc(req);
    memset(b->mem, 0x5a, req);
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) b->target[i] = b->cur[i] = opz_param_from_byte(0);
    b->target[OPZ_P_PAN] = b->cur[OPZ_P_PAN] = opz_param_from_byte(128);
    b->target[OPZ_P_LEVEL] = b->cur[OPZ_P_LEVEL] = opz_param_from_byte(255);
    b->gain = OPZ_Q31_ONE;
    init(b->area, track, 0, b->mem, scratch);
}

static void box_close(Box *b) { free(b->area); free(b->mem); }

static void guard_check(Box *b, const char *where)
{
    for (int i = 0; i < 256; i++)
        if (b->area[OPZ_FX_STATE_BYTES + i] != 0x5a) { printf("FAIL state guard written (%s)\n", where); exit(1); }
    State *s = (State *)b->area;
    if (s->semi < SEMI_MIN || s->semi > SEMI_MAX || s->a < 0 || s->a > A_MAX || s->trim < 0 || s->trim > 32767 ||
        s->agc < 0 || s->cleared > RING + 2 * SB_N || s->sbw >= SB_N || s->w >= RING || s->xf > XF) {
        printf("FAIL state out of range (%s): semi %d a %d trim %d agc %d cleared %u sbw %u w %u xf %u\n", where,
               s->semi, s->a, s->trim, s->agc, s->cleared, s->sbw, s->w, s->xf);
        exit(1);
    }
}

static void set(Box *b, int p, int byte) { b->target[p] = opz_param_from_byte((uint8_t)byte); }
static void jump(Box *b, int p, int byte) { b->target[p] = b->cur[p] = opz_param_from_byte((uint8_t)byte); }

static void block(Box *b)
{
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) b->cur[i] += (uint32_t)((int32_t)(b->target[i] - b->cur[i]) >> 6);
    renderFX(b->cur, 0, b->area, b->in, b->out, b->gain);
}

/* ---- input ------------------------------------------------------------------------------------------------ */

enum { IN_SILENCE, IN_NOISE, IN_SAW, IN_SINE, IN_CLICK, IN_KINDS };
typedef struct { int kind; double amp, f, ph; long n; } Src;

static void fill(Box *b, Src *src)
{
    for (int i = 0; i < BLK; i++, src->n++) {
        double l = 0, r = 0;
        switch (src->kind) {
        case IN_NOISE: l = ((int32_t)rnd() / 2147483648.0); r = ((int32_t)rnd() / 2147483648.0); break;
        case IN_SAW: l = r = 2 * src->ph - 1; break;
        case IN_SINE: l = r = sin(2 * M_PI * src->ph); break;
        case IN_CLICK: l = r = (src->n % 11025) == 0 ? 1 : 0; break;
        default: break;
        }
        src->ph += src->f / FS;
        src->ph -= floor(src->ph);
        l *= src->amp; r *= src->amp;
        b->in[2 * i] = (int32_t)(fmax(-1, fmin(l, 0.9999999)) * 2147483647.0);
        b->in[2 * i + 1] = (int32_t)(fmax(-1, fmin(r, 0.9999999)) * 2147483647.0);
    }
}

static double peak(const Box *b)
{
    double p = 0;
    for (int i = 0; i < 2 * BLK; i++) p = fmax(p, fabs(b->out[i] / 2147483648.0));
    return p;
}

static double db(double x) { return 20 * log10(x > 1e-12 ? x : 1e-12); }

/* ---- fuzz ------------------------------------------------------------------------------------------------- */

static void fuzz(void)
{
    const int runs = 20, secs = 60;
    double worst = 0;
    for (int run = 0; run < runs; run++) {
        Box b;
        box_open(&b, run % 2 ? OPZ_TRACK_FX2 : OPZ_TRACK_FX1);
        Src src = { IN_SAW, 0.5, 110, 0, 0 };
        long nb = (long)secs * FS / BLK, quiet_from = nb - 30 * FS / BLK;   /* the last 30 s get no input */
        int full = 0;                                                       /* blocks in a row at full scale */
        double run_peak = 0;
        for (long k = 0; k < nb; k++) {
            int r = rndn(1000);
            if (r < 40) set(&b, OPZ_P_P1, rndn(256));                      /* a knob move */
            else if (r < 70) set(&b, OPZ_P_P2, rndn(256));
            else if (r < 80) jump(&b, OPZ_P_P1, rndn(2) * 255);             /* a jump to an end */
            else if (r < 90) jump(&b, OPZ_P_P2, rndn(2) * 255);
            else if (r < 110) { set(&b, OPZ_P_P1, rndn(256)); set(&b, OPZ_P_P2, rndn(256)); }
            else if (r < 125) onNoteOn(rndn(128), (uint8_t)(1 + rndn(127)), b.area, b.cur);
            else if (r < 130) onNoteOn(rndn(128), 0, b.area, NULL);         /* the OS's re-send of a held note */
            else if (r < 133) onNoteOn(255, 0, b.area, NULL);
            else if (r < 136) onNoteOn(rndn(2) ? -1 : 1000, 100, b.area, NULL);
            else if (r < 145) onNoteOff(rndn(128), b.area);
            else if (r < 150) set(&b, OPZ_P_PAN, rndn(256));
            else if (r < 152) set(&b, OPZ_P_LEVEL, rndn(256));
            else if (r < 154) b.gain = (int32_t)(rnd() >> 1);
            else if (r < 170 && k < quiet_from) {
                src.kind = rndn(IN_KINDS); src.amp = rndn(3) ? 1.0 : (rndn(1000) / 1000.0);
                src.f = 20.0 * pow(2, rndn(1000) / 100.0);                  /* 20 Hz .. 20 kHz */
            }
            if (k >= quiet_from) src.kind = IN_SILENCE;
            if (k % 64 == 0 && rndn(4) == 0) {                              /* fast sweeps of both knobs */
                for (int j = 0; j < 32 && k < nb; j++, k++) {
                    set(&b, OPZ_P_P1, (j * 16) & 255); set(&b, OPZ_P_P2, 255 - ((j * 24) & 255));
                    fill(&b, &src); block(&b); guard_check(&b, "sweep");
                }
            }
            fill(&b, &src);
            block(&b);
            guard_check(&b, "fuzz");
            /* level and pan scale the wet signal: judge it at the gain it got */
            double p = peak(&b);
            run_peak = fmax(run_peak, p);
            full = p > 0.999 ? full + 1 : 0;
            if (full > 2 * FS / BLK) { printf("FAIL run %d: output at full scale for 2 s (block %ld)\n", run, k); exit(1); }
        }
        /* end: P1 below the endless top, then 20 s more of silence must decay to nothing */
        jump(&b, OPZ_P_P1, 200);
        double tail = 0;
        for (long k = 0; k < 20L * FS / BLK; k++) {
            fill(&b, &src); block(&b);
            if (k > 19L * FS / BLK) tail = fmax(tail, peak(&b));
        }
        guard_check(&b, "end");
        printf("run %2d (%s): peak %6.1f dBFS, tail after 20 s at P1 200 %6.1f dBFS\n", run,
               run % 2 ? "fx2" : "fx1", db(run_peak), db(tail));
        if (tail > 1e-4) { printf("FAIL run %d: tail doesn't decay\n", run); exit(1); }
        worst = fmax(worst, run_peak);
        box_close(&b);
    }

    /* Worst case: endless, full shimmer, a full-scale saw, every interval for 10 s each, wet only at unity */
    Box b;
    box_open(&b, OPZ_TRACK_FX1);
    jump(&b, OPZ_P_P1, 255); jump(&b, OPZ_P_P2, 255); jump(&b, OPZ_P_PAN, 255);
    Src src = { IN_SAW, 1.0, 220, 0, 0 };
    static const int notes[] = { 60, 67, 72, 84, 48, 36, 30, 53, 100, 0 };
    for (int n = 0; n < 10; n++) {
        onNoteOn(notes[n], 100, b.area, b.cur);
        double pk = 0, rms = 0; long cnt = 0;
        for (long k = 0; k < 10L * FS / BLK; k++) {
            src.kind = n == 9 ? IN_NOISE : IN_SAW;
            fill(&b, &src); block(&b); guard_check(&b, "worst");
            if (k > 5L * FS / BLK) {
                pk = fmax(pk, peak(&b));
                for (int i = 0; i < 2 * BLK; i++) { double v = b.out[i] / 2147483648.0; rms += v * v; cnt++; }
            }
        }
        printf("P1 255 P2 255, note %3d (%+d), full-scale %s: last 5 s peak %6.1f dBFS, RMS %6.1f dBFS\n",
               notes[n], ((State *)b.area)->semi, n == 9 ? "noise" : "saw", db(pk), db(sqrt(rms / cnt)));
        if (pk > 0.999) { printf("FAIL: the endless top clips\n"); exit(1); }
    }
    box_close(&b);
    printf("fuzz: OK (worst run peak %.1f dBFS)\n", db(worst));
}

/* ---- measurements ----------------------------------------------------------------------------------------- */

/* Goertzel power of f over n samples of x (mono) */
static double tone(const double *x, int n, double f)
{
    double w = 2 * cos(2 * M_PI * f / FS), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double s0 = x[i] * (0.5 - 0.5 * cos(2 * M_PI * i / n)) + w * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double p = s1 * s1 + s2 * s2 - w * s1 * s2;
    return 2 * sqrt(p) / (n * 0.5);
}

/* The strongest of f's neighbours within +-40 cents: a delay-line shifter pulls a pure tone's pitch by up to
 * +-fs / SW (7 Hz) */
static double band(const double *x, int n, double f)
{
    double best = 0;
    for (double g = f * 0.977; g < f * 1.0234; g *= 1.0006) best = fmax(best, tone(x, n, g));
    return best;
}

/* Render secs of wet output (mono L+R / 2) after setting P1, P2 and a note, with a 0.5 s input burst. */
static double *wet_render(int p1, int p2, int note, int kind, double f, double amp, double burst, double secs, long *n_out)
{
    Box b;
    box_open(&b, OPZ_TRACK_FX1);
    jump(&b, OPZ_P_P1, p1); jump(&b, OPZ_P_P2, p2); jump(&b, OPZ_P_PAN, 255);
    if (note >= 0) onNoteOn(note, 100, b.area, b.cur);
    Src src = { kind, amp, f, 0, 0 };
    long nb = (long)(secs * FS / BLK), nburst = (long)(burst * FS / BLK);
    double *y = malloc(sizeof(double) * nb * BLK);
    for (int k = 0; k < 20; k++) { src.kind = IN_SILENCE; fill(&b, &src); block(&b); }   /* clearing */
    src.n = 0;
    for (long k = 0; k < nb; k++) {
        src.kind = k < nburst ? kind : IN_SILENCE;
        fill(&b, &src); block(&b);
        for (int i = 0; i < BLK; i++) y[k * BLK + i] = (b.out[2 * i] + (double)b.out[2 * i + 1]) / 4294967296.0;
    }
    box_close(&b);
    *n_out = nb * BLK;
    return y;
}

/* T60 from the energy decay curve (Schroeder), fitted from -5 to -35 dB */
static double t60(const double *y, long n)
{
    double *e = malloc(sizeof(double) * n), acc = 0;
    for (long i = n - 1; i >= 0; i--) { acc += y[i] * y[i]; e[i] = acc; }
    long a = -1, c = -1;
    for (long i = 0; i < n; i++) {
        double d = 10 * log10(e[i] / e[0] + 1e-30);
        if (a < 0 && d < -5) a = i;
        if (c < 0 && d < -35) { c = i; break; }
    }
    free(e);
    return a < 0 || c < 0 ? -1 : 2.0 * (c - a) / FS;
}

static void measure(void)
{
    long n;
    printf("decay (click, T60 from -5 to -35 dB):\n  P1 ");
    static const int p1s[] = { 0, 64, 128, 180, 200, 240 };
    static const int p2s[] = { 0, 64, 128, 192, 255 };
    for (int i = 0; i < 6; i++) printf("%8d", p1s[i]);
    printf("\n");
    for (int j = 0; j < 5; j++) {
        printf("  P2 %3d:", p2s[j]);
        for (int i = 0; i < 6; i++) {
            double secs = p1s[i] >= 200 ? 60 : p1s[i] >= 128 ? 20 : 6;
            double *y = wet_render(p1s[i], p2s[j], -1, IN_CLICK, 0, 0.9, BLK / (double)FS, secs, &n);
            printf("%7.2fs", t60(y, n));
            free(y);
        }
        printf("\n");
    }

    printf("shimmer (440 Hz sine, 0.5 s, P1 180, octave): level of 440 / 880 / 1760 / 3520 Hz in dB\n");
    for (int j = 0; j < 5; j++) {
        double *y = wet_render(180, p2s[j], -1, IN_SINE, 440, 0.5, 0.5, 6, &n);
        printf("  P2 %3d:", p2s[j]);
        for (double t = 0.75; t < 5; t += 1.0) {
            const double *w = y + (long)(t * FS);
            int len = 8192;
            printf("  t %.2f s: %5.1f %5.1f %5.1f %5.1f |", t, db(band(w, len, 440)), db(band(w, len, 880)),
                   db(band(w, len, 1760)), db(band(w, len, 3520)));
        }
        printf("\n");
        free(y);
    }

    printf("intervals (440 Hz sine, 0.5 s, P1 180, P2 140), 0.5-1.5 s: the level at 440 Hz x the interval and x the "
           "interval twice (the second lap)\n");
    static const int notes[] = { 60, 67, 65, 72, 55, 53, 36, 48, 79, 20, 62, 63, 50, 58, 70, 66 };
    for (int k = 0; k < 16; k++) {
        double *y = wet_render(180, 140, notes[k], IN_SINE, 440, 0.5, 0.5, 2, &n);
        int d = notes[k] - NOTE_ZERO < -12 ? -12 : notes[k] - NOTE_ZERO > 24 ? 24 : notes[k] - NOTE_ZERO;
        int semi = k_snap[d - SEMI_MIN];                       /* the keys between the intervals snap */
        double f1 = 440 * pow(2, semi / 12.0), f2 = 440 * pow(2, semi / 6.0);
        const double *w = y + FS / 2;
        printf("  note %3d (%+3d): 440 Hz %5.1f dB, %6.1f Hz %5.1f dB, %6.1f Hz %5.1f dB\n", notes[k], semi,
               db(band(w, FS, 440)), f1, db(band(w, FS, f1)), f2, db(band(w, FS, f2)));
        free(y);
    }

    printf("aliasing (7 kHz sine, 0.3 s, P1 180, P2 255), 0.3-0.7 s: 7 kHz, its octave 14 kHz, the next one's fold "
           "(28 -> 16.1 kHz) and that one's (32.2 -> 11.9 kHz)\n");
    {
        double *y = wet_render(180, 255, -1, IN_SINE, 7000, 0.5, 0.3, 2, &n);
        const double *w = y + (long)(0.3 * FS);
        int len = (int)(0.4 * FS);
        printf("  7 kHz %5.1f dB, 14 kHz %5.1f dB, 16.1 kHz %5.1f dB, 11.9 kHz %5.1f dB\n", db(band(w, len, 7000)),
               db(band(w, len, 14000)), db(band(w, len, 16100)), db(band(w, len, 11900)));
        free(y);
    }

    printf("level (110 Hz saw at -12 dBFS, sustained 20 s, wet at unity): RMS of the last 5 s\n");
    for (int i = 0; i < 3; i++) {
        int p1 = (int[]){ 128, 240, 255 }[i];
        for (int j = 0; j < 5; j += 2) {
            double *y = wet_render(p1, p2s[j], -1, IN_SAW, 110, 0.25, 20, 20, &n);
            double r = 0, pk = 0;
            for (long k = n - 5 * FS; k < n; k++) { r += y[k] * y[k]; pk = fmax(pk, fabs(y[k])); }
            printf("  P1 %3d P2 %3d: RMS %6.1f dBFS, peak %6.1f dBFS\n", p1, p2s[j], db(sqrt(r / (5 * FS))), db(pk));
            free(y);
        }
    }

    printf("endless (P1 255, noise burst 0.5 s at -12 dBFS): wet RMS in dBFS around 2, 5, 10, 20 and 40 s\n");
    for (int j = 0; j < 5; j++) {
        double *y = wet_render(255, p2s[j], -1, IN_NOISE, 0, 0.25, 0.5, 41, &n);
        printf("  P2 %3d:", p2s[j]);
        static const double at[] = { 2, 5, 10, 20, 40 };
        for (int k = 0; k < 5; k++) {
            double r = 0;
            for (long i = (long)(at[k] * FS) - FS / 4; i < (long)(at[k] * FS) + FS / 4; i++) r += y[i] * y[i];
            printf("  %5.1f", db(sqrt(r / (FS / 2))));
        }
        printf("\n");
        free(y);
    }

    printf("clicks (P1 and P2 sweep 0..255..0 every 0.4 s / 0.6 s over 6 s, 220 Hz saw at -12 dBFS): "
           "largest second difference / RMS of the 2 ms around it\n");
    {
        Box b;
        box_open(&b, OPZ_TRACK_FX1);
        jump(&b, OPZ_P_PAN, 255);
        Src src = { IN_SAW, 0.25, 220, 0, 0 };
        long nb = 6L * FS / BLK;
        double *y = malloc(sizeof(double) * nb * BLK);
        for (int k = 0; k < 20; k++) { fill(&b, &src); block(&b); }
        for (long k = 0; k < nb; k++) {
            int t1 = (int)(k % 276), t2 = (int)(k % 414);
            set(&b, OPZ_P_P1, t1 < 138 ? t1 * 255 / 138 : (276 - t1) * 255 / 138);
            set(&b, OPZ_P_P2, t2 < 207 ? t2 * 255 / 207 : (414 - t2) * 255 / 207);
            fill(&b, &src); block(&b);
            for (int i = 0; i < BLK; i++) y[k * BLK + i] = b.out[2 * i] / 2147483648.0;
        }
        double worst = 0, ref = 0; long at = 0;
        for (long i = FS; i < nb * BLK - 100; i++) {
            double d2 = fabs(y[i + 1] - 2 * y[i] + y[i - 1]), r = 0;
            for (int j = -44; j < 44; j++) r += y[i + j] * y[i + j];
            r = sqrt(r / 88);
            if (r > 1e-4 && d2 / r > worst) { worst = d2 / r; at = i; }
        }
        for (long i = 100; i < 20000; i++) {                       /* the same measure on the dry saw */
            double s0 = fmod(220.0 * (i - 1) / FS, 1) * 2 - 1, s1 = fmod(220.0 * i / FS, 1) * 2 - 1,
                   s2 = fmod(220.0 * (i + 1) / FS, 1) * 2 - 1;
            ref = fmax(ref, fabs(s2 - 2 * s1 + s0) / (1 / sqrt(3.0)));
        }
        printf("  wet output: %.3f (at %.2f s); the dry saw's own: %.3f\n", worst, at / (double)FS, ref);
        free(y);
        box_close(&b);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "fuzz")) fuzz();
    else if (argc > 1 && !strcmp(argv[1], "measure")) measure();
    else { printf("usage: %s fuzz|measure\n", argv[0]); return 1; }
    return 0;
}
