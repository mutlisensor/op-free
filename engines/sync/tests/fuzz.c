/* Host checks of sync, built with the sanitizers (out-of-bounds reads, signed overflow, division):
 *
 *   clang -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o /tmp/sync_fuzz engines/sync/tests/fuzz.c -lm && /tmp/sync_fuzz
 *
 * 1. reference: the engine's output against a double-precision model of the same oscillator (master and slave
 *    simulated in continuous time from the engine's phases at every block, every wrap and reset found exactly,
 *    the 4-point B-spline BLEP added from a continuous formula, one sample late), over random pitches C0 .. 11 kHz
 *    with slopes, P1 jumping every block, P2 0 .. 127, velocities 1 .. 127: within 6e-5 of full scale (the Q15
 *    residual polynomials; 4e-6 with exact ones).
 * 2. mapping: every P1 byte x velocity x pitch: the ratio is 2^(4x + 4x^2 + velocity offset), clamped to 1 .. 256
 *    and to the slave cap, within 0.05 %; every P2 byte: oscillator + noise power constant within 0.3 dB.
 * 3. fuzz: 300,000 blocks of random notes (velocity 0 .. 255, params NULL or random), knob jumps every block,
 *    extreme pitches (0, 0xffffffff, random) and slopes, random envelope stages, levels, coefficients and steps,
 *    voice re-use without init; checks every event lands inside the block, render adds into out (prefilled vs
 *    zero buffer), is deterministic, leaves the shared event buffer zeroed, keeps the envelope level >= 0, never
 *    wraps (no sample jumps from near +1 to near -1) and stays inside its state.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_bad_event;
#define SYNC_CHECK(T, d) do { if ((T) < 0 || (T) >= N || (d) < 0) g_bad_event = 1; } while (0)
/* plain names like init would interpose on the sanitizer runtime's own */
#define init sync_init
#define render sync_render
#define initModule sync_initModule
#define onNoteOn sync_onNoteOn
#define onNoteOff sync_onNoteOff
#include "../sync.c"

static uint32_t g_seed = 12345;
static uint32_t r32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed ^ (g_seed >> 15); }

static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r) { (void)env; (void)a; (void)d; (void)s; (void)r; }
static ModuleCallbackInterface g_cb;

/* ---- 1. reference ------------------------------------------------------------------------------------------ */

/* the band-limited step minus the ideal one at u samples after the step (the cubic B-spline's integral) */
static double residual(double u)
{
    if (u <= -2 || u >= 2) return 0;
    if (u < -1) return pow(u + 2, 4) / 24;
    if (u < 0) { double e = -u; return 0.5 - 2 * e / 3 + e * e * e / 3 - e * e * e * e / 8; }
    if (u < 1) return -(0.5 - 2 * u / 3 + u * u * u / 3 - u * u * u * u / 8);
    return -pow(2 - u, 4) / 24;
}

#define RB 200                                                  /* blocks per run */
static double g_naive[RB * N], g_evt[RB * 80], g_evh[RB * 80];

static int check_reference(void)
{
    double worst = 0;
    long nev = 0;
    for (int run = 0; run < 200; run++) {
        State st;
        sync_init(&st, 5, 0, 0, 0);
        uint32_t params[OPZ_NUM_PARAMS] = { 0 };
        sync_onNoteOn(60, (uint8_t)(1 + r32() % 127), &st, params);
        int32_t envw[6] = { 0, 0x7fffffff, 0x7fffffff, 0, 3, 0 };
        int ne = 0;
        uint32_t base = (uint32_t)(0x184CBB * exp2((r32() % 1300) / 120.0));   /* C0 .. ~11 kHz */
        for (int b = 0; b < RB; b++) {
            params[0] = opz_param_from_byte((uint8_t)r32());
            params[1] = opz_param_from_byte((uint8_t)(r32() % 128));            /* no noise (128 has a trace of it) */
            uint32_t pitch = base + (uint32_t)((int32_t)(r32() % 2001) - 1000) * (base >> 12);
            int32_t slope = (r32() & 3) == 0 ? (int32_t)(r32() % 2001 - 1000) * (int32_t)(pitch >> 18) : 0;
            Block k;
            setup(&k, &st, pitch, slope, params);
            /* the model, from the engine's phases at sample 0 */
            double fm = k.im / 4294967296.0, fsl = k.is / 4294967296.0;
            double pm = st.pm / 4294967296.0, ps = st.ps / 4294967296.0;
            double gs = k.gs / 2147483648.0, gb = k.gb / 2147483648.0, dc = k.dc / 2147483648.0;
            int sub = st.sub;
            for (int t = 0; t < N; t++) {
                g_naive[b * N + t] = gs * (2 * ps - 1) + sub * gb + dc;
                double left = 1.0;
                for (;;) {
                    double dtm = (1 - pm) / fm, dts = (1 - ps) / fsl;
                    if (dtm >= left && dts >= left) { pm += fm * left; ps += fsl * left; break; }
                    if (dts < dtm) {                                /* the slave wraps */
                        pm += fm * dts; ps = 0; left -= dts;
                        g_evt[ne] = b * N + t + (1 - left); g_evh[ne++] = -2 * gs;
                    } else {                                        /* reset */
                        ps += fsl * dtm; pm = 0; left -= dtm;
                        sub = -sub;
                        g_evt[ne] = b * N + t + (1 - left); g_evh[ne++] = -2 * gs * ps + 2 * gb * sub;
                        ps = 0;
                    }
                }
            }
            static fract32 out[2 * N];
            memset(out, 0, sizeof out);
            sync_render(out, pitch, slope, &st, params, (ADSR *)envw);
            if (b < 2) continue;                                    /* the first reset settles the slave */
            for (int i = 0; i < N; i++) {
                int n = b * N + i - 1;                              /* the sample this output shows */
                double ref = g_naive[n];
                for (int j = ne - 1; j >= 0 && g_evt[j] > n - 2.5; j--) ref += g_evh[j] * residual(n - g_evt[j]);
                double y = out[2 * i] / 2147483648.0 * 32768.0 / 32767.0;
                double err = fabs(y - ref);
                if (err > worst) {
                    worst = err;
                    if (getenv("SYNC_DEBUG"))
                        printf("  run %d block %d i %d: err %.2e im %u is %u ratio %.3f gs %d pitch %u slope %d\n", run, b, i,
                               err, k.im, k.is, (double)k.is / k.im, k.gs, pitch, slope);
                }
                if (out[2 * i] != out[2 * i + 1]) { printf("reference: L != R\n"); return 1; }
            }
        }
        nev += ne;
    }
    printf("reference: %ld events, worst difference to the double-precision model %.2e of full scale\n", nev, worst);
    return worst > 6e-5;                                     /* the Q15 residuals; 4e-6 with exact ones */
}

/* ---- 2. mapping ------------------------------------------------------------------------------------------- */

static int check_mapping(void)
{
    State st;
    sync_init(&st, 5, 0, 0, 0);
    uint32_t params[OPZ_NUM_PARAMS] = { 0 };
    double worst = 0;
    for (int vel = 0; vel <= 127; vel += 7) {
        sync_onNoteOn(60, (uint8_t)vel, &st, params);
        for (int p1 = 0; p1 < 256; p1++)
            for (int note = 0; note < 128; note += 5) {
                uint32_t pitch = (uint32_t)(0x184CBB * exp2((note - 12) / 12.0));
                params[0] = opz_param_from_byte((uint8_t)p1);
                Block k;
                setup(&k, &st, pitch, 0, params);
                double x = opz_p16(params[0]) / 65536.0;
                double oct = 4 * x + 4 * x * x + (vel - 100) / 48.0;
                if (oct < 0) oct = 0;
                if (oct > 8) oct = 8;
                double im = pitch < INC_MIN ? INC_MIN : pitch > INC_MAX ? INC_MAX : pitch;
                double is = im * exp2(oct);
                if (is > SLAVE_MAX) is = SLAVE_MAX;
                if (is < im) is = im;
                double err = fabs(k.is / is - 1);
                if (err > worst) worst = err;
                if (k.im != (uint32_t)im || k.is < k.im || (k.is > SLAVE_MAX && k.is != k.im) || k.dc < 0 || k.dc > k.gs / 5) {
                    printf("mapping: out of range at p1 %d vel %d note %d\n", p1, vel, note);
                    return 1;
                }
            }
    }
    sync_onNoteOn(60, 100, &st, params);
    params[0] = 0;                                              /* ratio 1 at a low note: no level compensation */
    double pmin = 1e9, pmax = 0;
    for (int p2 = 0; p2 < 256; p2++) {
        params[1] = opz_param_from_byte((uint8_t)p2);
        Block k;
        setup(&k, &st, 0x00400000, 0, params);
        double gs = k.gs / 2147483648.0, gb = k.gb / 2147483648.0, gn = k.gn / 32768.0;
        double pw = gs * gs / 3 + gb * gb + gn * gn / 6;            /* saw, square, noise (two-tap sum) */
        if (pw < pmin) pmin = pw;
        if (pw > pmax) pmax = pw;
    }
    printf("mapping: slave increment within %.3f %% of 2^(4x + 4x^2 + vel); P2 power %.2f dB .. %.2f dB re a saw\n",
           100 * worst, 10 * log10(pmin * 3 / (0.197 * 0.197)), 10 * log10(pmax * 3 / (0.197 * 0.197)));
    return worst > 5e-4 || 10 * log10(pmax / pmin) > 0.3;
}

/* ---- 3. fuzz ---------------------------------------------------------------------------------------------- */

typedef struct { uint8_t b[OPZ_VOICE_STATE_BYTES + 64]; } Mem;
typedef struct { int32_t w[6]; } Env;

static int fuzz(void)
{
    enum { V = 6 };
    static Mem mem[V];
    static Env env[V];
    uint32_t params[OPZ_NUM_PARAMS];
    int32_t out_a[2 * N], out_b[2 * N];
    long sat = 0, samples = 0;
    int32_t loud = 0;
    sync_initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int v = 0; v < V; v++) {
        memset(&mem[v], 0x5a, sizeof mem[v]);
        sync_init(mem[v].b, 4 + (uint32_t)(v % 4), 0, 0, 0);
    }
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte((uint8_t)r32());
    for (long blk = 0; blk < 300000; blk++) {
        int v = (int)(r32() % V);
        uint32_t r = r32();
        if ((r & 31) == 0) sync_init(mem[v].b, 4 + (r >> 8) % 4, 0, 0, 0);
        if ((r & 7) == 1) sync_onNoteOn((int)(r32() % 128), (uint8_t)r32(), mem[v].b, (r & 64) ? NULL : params);
        if ((r & 15) == 2) sync_onNoteOff(0, mem[v].b);
        /* knobs: jumps, extremes or the host's smoothing */
        for (int i = 0; i < 2; i++) {
            uint32_t k = r32() % 4;
            params[i] = k == 0 ? opz_param_from_byte((uint8_t)r32()) : k == 1 ? (r32() & 1 ? 0x7fffffffu : 0)
                      : k == 2 ? params[i] + (uint32_t)((int32_t)(opz_param_from_byte((uint8_t)r32()) - params[i]) >> 6)
                      : params[i];
        }
        Env *e = &env[v];
        uint32_t stage = 1 + r32() % 6;
        e->w[4] = (int32_t)stage;
        if (r32() % 8 == 0) e->w[1] = (int32_t)(r32() >> 1);
        e->w[2] = (r32() & 1) ? 0x7fffffff : 0x70000000 + (int32_t)(r32() % 0x10000000);
        uint32_t sk = r32() % 4;
        e->w[3] = sk == 0 ? 0 : sk == 1 ? (int32_t)(r32() >> 4) : sk == 2 ? -(int32_t)(r32() >> 6) : (int32_t)(r32() >> 1);
        uint32_t pk = r32() % 6;
        uint32_t pitch = pk == 0 ? 0 : pk == 1 ? 0xffffffffu : pk == 2 ? r32()
                       : (uint32_t)(0x184CBB * exp2((r32() % 128) / 12.0));
        int32_t slope = (r32() % 4 == 0) ? (int32_t)r32() >> (r32() % 2 ? 6 : 0) : 0;
        Mem save = mem[v];
        Env esave = *e;
        memset(out_a, 0, sizeof out_a);
        sync_render(out_a, pitch, slope, mem[v].b, params, (ADSR *)e);
        Mem after = mem[v];
        Env eafter = *e;
        mem[v] = save;
        *e = esave;
        for (int i = 0; i < 2 * N; i++) out_b[i] = 12345;
        sync_render(out_b, pitch, slope, mem[v].b, params, (ADSR *)e);
        if (g_bad_event) { printf("fuzz: an event outside the block (block %ld)\n", blk); return 1; }
        if (memcmp(&after, &mem[v], sizeof after) || memcmp(&eafter, e, sizeof eafter)) {
            printf("fuzz: render is not deterministic (block %ld)\n", blk);
            return 1;
        }
        for (int i = 0; i < 2 * N; i++) {
            if (out_a[i] > 0x7ff00000 || out_a[i] < -0x7ff00000) { sat++; continue; }
            int32_t a = out_a[i] < 0 ? -out_a[i] : out_a[i];
            if (a > loud) loud = a;
            if (out_b[i] - 12345 != out_a[i]) {
                printf("fuzz: render does not add into out (block %ld, sample %d)\n", blk, i);
                return 1;
            }
            if (i >= 2 && ((out_a[i] > 0x40000000 && out_a[i - 2] < -0x40000000) ||
                           (out_a[i] < -0x40000000 && out_a[i - 2] > 0x40000000))) {
                printf("fuzz: wrap-like jump (block %ld, sample %d): %d -> %d; pitch %u slope %d\n", blk, i,
                       out_a[i - 2], out_a[i], pitch, slope);
                return 1;
            }
        }
        samples += 2 * N;
        for (int i = 0; i < N + 4; i++)
            if (s_e[i]) { printf("fuzz: s_e not cleared (block %ld)\n", blk); return 1; }
        if (e->w[1] < 0) { printf("fuzz: negative envelope level (block %ld)\n", blk); return 1; }
        for (int i = (int)sizeof(State); i < (int)sizeof(Mem); i++)
            if (mem[v].b[i] != 0x5a) { printf("fuzz: state overflow (block %ld)\n", blk); return 1; }
    }
    printf("fuzz: 300000 blocks ok, %.4f %% of samples at full scale, loudest other sample %.1f dBFS "
           "(random envelope levels up to 1.0)\n", 100.0 * sat / samples, 20 * log10(loud / 2147483648.0));
    return sat != 0;
}

int main(void)
{
    g_cb.updateParametersADSR = cb_adsr;
    sync_initModule(0, &g_cb, &(uint32_t){ 0 });
    int fail = check_reference();
    fail |= check_mapping();
    fail |= fuzz();
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
