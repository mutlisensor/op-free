/* Host checks of chip, built with the sanitizers (out-of-bounds reads, signed overflow, shifts, division):
 *
 *   clang -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o /tmp/chip_fuzz engines/chip/tests/fuzz.c && /tmp/chip_fuzz
 *
 * 1. tables: every running integral starts and ends at 0 (no DC), the steps are within +-16 units, the metallic
 *    loop has 48 ones in 93 steps.
 * 2. averages: integral differences / phase step equal the exact average of the table over random intervals
 *    (pitches 0.67 Hz .. the caps, random phases): exact, integer arithmetic.
 * 3. fuzz: 300,000 blocks of random notes (velocity 0 .. 255, params NULL), knob jumps every block, extreme pitches
 *    (0, 0xffffffff, random) and slopes, random envelope stages, levels, coefficients and steps, voice re-use
 *    without init; checks that render adds into out (prefilled vs zero buffer), that one voice never exceeds the
 *    voice peak (+1 %) at envelope levels in [0, 1], that the state stays in range and that renders are
 *    deterministic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define init chip_init
#define render chip_render
#define initModule chip_initModule
#define onNoteOn chip_onNoteOn
#define onNoteOff chip_onNoteOff
#include "../chip.c"

static uint32_t g_seed = 12345;
static uint32_t r32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed ^ (g_seed >> 15); }

static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r) { (void)env; (void)a; (void)d; (void)s; (void)r; }
static ModuleCallbackInterface g_cb;

typedef struct Env { int32_t attack, level, coef, step, stage; uint8_t newnote, release; } Env;

static int check_tables(void)
{
    for (int w = 0; w < NTAB; w++) {
        const int16_t *c = s_c + k_off[w];
        int n = k_len[w];
        if (c[0] != 0 || c[n] != 0) { printf("tables: wave %d is not zero-sum\n", w); return 1; }
        for (int k = 0; k < n; k++)
            if (c[k + 1] - c[k] > 16 || c[k + 1] - c[k] < -16) { printf("tables: wave %d step %d\n", w, k); return 1; }
    }
    int ones = 0;
    for (int k = 0; k < 93; k++) ones += s_c[OFF_METAL + k + 1] - s_c[OFF_METAL + k] > 0;
    if (ones != 48) { printf("tables: metallic loop has %d ones\n", ones); return 1; }
    printf("tables: ok\n");
    return 0;
}

/* exact average of table w over the phase interval [a, a + d) (turns, d < 8 steps) */
static double ref_avg(int w, double a, double d)
{
    const int16_t *c = s_c + k_off[w];
    int n = k_len[w];
    double x0 = a * n, x1 = (a + d) * n, sum = 0;
    for (double x = x0; x < x1;) {
        double next = (double)((long)x + 1);
        double end = next < x1 ? next : x1;
        int k = (int)((long)x % n);
        sum += (c[k + 1] - c[k]) * (end - x);
        x = end;
    }
    return sum / (x1 - x0);
}

static int check_averages(void)
{
    static const uint32_t caps[NTAB] = { INC_MAX, INC_MAX, INC_MAX, INC_MAX, INC_MAX, INC_METAL };
    double worst = 0;
    for (int trial = 0; trial < 200000; trial++) {
        int w = (int)(r32() % NTAB);
        uint32_t inc = INC_MIN + r32() % (caps[w] - INC_MIN);
        if (trial % 3 == 0) inc = INC_MIN << (r32() % 13);
        uint32_t ph = r32();
        uint32_t d = integral(s_c + k_off[w], k_len[w], ph + inc) - integral(s_c + k_off[w], k_len[w], ph);
        uint32_t span = (((ph + inc) >> 8) - (ph >> 8)) & 0xffffff;     /* the truncated phases' interval, Q24 */
        double got = (double)(int32_t)d / ((double)span * k_len[w]);
        double want = ref_avg(w, (ph >> 8) / 16777216.0, span / 16777216.0);
        double err = got - want;
        if (err < 0) err = -err;
        if (err > 1e-3 && worst <= 1e-3)
            printf("averages: wave %d inc %u ph %u: %.6f vs %.6f\n", w, inc, ph, got, want);
        if (err > worst) worst = err;
    }
    printf("averages: worst error %.2e units\n", worst);
    return worst > 1e-3;
}

static int fuzz(void)
{
    static State st[4], st2[4];
    static Env env[4], env2[4];
    static fract32 out[128], pre[128], zero[128], again[128];
    uint32_t params[OPZ_NUM_PARAMS];
    int32_t peak_bad = 0;
    for (int v = 0; v < 4; v++) { chip_init(&st[v], 4 + v, 0, 0, 0); env[v].stage = 0; }
    for (long blk = 0; blk < 300000; blk++) {
        int v = (int)(r32() & 3);
        State *s = &st[v];
        Env *e = &env[v];
        uint32_t r = r32();
        if ((r & 15) == 0) {                                         /* a note-on, params NULL half the time */
            for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = r32() & 0x7fffffff;
            params[OPZ_P_PAN] = 0x40000000u;                         /* level full, pan exact centre: L == R */
            params[OPZ_P_LEVEL] = 0x7fffffffu;
            chip_onNoteOn((int)(r32() & 127), (uint8_t)r32(), s, (r & 16) ? NULL : params);
            e->stage = 1; e->level = 0; e->coef = OPZ_Q31_ONE; e->step = (int32_t)(r32() >> 4);
        }
        if ((r & 255) == 1) chip_init(s, 4 + (r32() & 3), 0, 0, 0);  /* the plug switched back and forth */
        if ((r & 63) == 2) { e->stage = 0; continue; }
        if ((r & 31) == 3) {                                         /* random envelope state */
            e->stage = 1 + (int32_t)(r32() % 6);
            e->level = (int32_t)(r32() & 0x7fffffff);
            e->coef = (r32() & 1) ? OPZ_Q31_ONE : (int32_t)(r32() & 0x7fffffff);
            e->step = (r32() & 1) ? 0 : (int32_t)r32() >> 6;
        }
        if (e->stage == 0) e->stage = 1;
        for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = (r32() & 7) == 0 ? r32() : r32() & 0x7fffffff;
        params[OPZ_P_PAN] = 0x40000000u;     /* the track's level and pan (opz_mix.h): full, exact centre, so L == R */
        params[OPZ_P_LEVEL] = 0x7fffffffu;
        uint32_t pitch = r32();
        switch (r32() & 7) {
        case 0: pitch = 0; break;
        case 1: pitch = 0xffffffffu; break;
        case 2: pitch = 0x184CBB << (r32() % 10); break;
        default: pitch >>= (r32() % 8); break;
        }
        int32_t slope = (r32() & 3) == 0 ? (int32_t)r32() : (int32_t)r32() >> (8 + r32() % 16);
        for (int i = 0; i < 128; i++) { pre[i] = (int32_t)(r32() >> 4) - 0x08000000; zero[i] = 0; }
        memcpy(out, pre, sizeof out);
        st2[v] = *s; env2[v] = *e;
        chip_render(out, pitch, slope, s, params, (ADSR *)e);
        State sa = st2[v]; Env ea = env2[v];
        chip_render(zero, pitch, slope, &sa, params, (ADSR *)&ea);
        State sb = st2[v]; Env eb = env2[v];
        memset(again, 0, sizeof again);
        chip_render(again, pitch, slope, &sb, params, (ADSR *)&eb);
        if (memcmp(zero, again, sizeof zero) || memcmp(&sa, &sb, sizeof sa)) { printf("fuzz: not deterministic\n"); return 1; }
        for (int i = 0; i < 128; i++) {
            if (out[i] != pre[i] + zero[i]) { printf("fuzz: render does not add into out (block %ld)\n", blk); return 1; }
            if (i & 1 && zero[i] != zero[i - 1]) { printf("fuzz: L != R\n"); return 1; }
        }
        /* the voice's peak: env levels in [0, 1] give at most the voice peak (+1 %) */
        int in_range = env2[v].level >= 0 && e->level >= 0;
        for (int i = 0; i < 128 && in_range; i++)
            if (zero[i] > OPZ_VOICE_PEAK + OPZ_VOICE_PEAK / 100 || zero[i] < -(OPZ_VOICE_PEAK + OPZ_VOICE_PEAK / 100)) {
                if (!peak_bad) printf("fuzz: sample %d = %.4f of full scale (block %ld)\n", i, zero[i] / 2147483648.0, blk);
                peak_bad++;
            }
        if (s->arp >= 4 || s->tick >= 4 || s->lfsr == 0 || s->lfsr > 0x7fff || s->sweep < -(4 << 16) ||
            s->sweep > (2 << 16)) { printf("fuzz: state out of range\n"); return 1; }
        if (e->level < 0) { printf("fuzz: envelope level below 0\n"); return 1; }
    }
    if (peak_bad) { printf("fuzz: %d samples over the voice peak\n", peak_bad); return 1; }
    printf("fuzz: 300000 blocks ok\n");
    return 0;
}

int main(void)
{
    uint32_t mem = 0;
    g_cb.updateParametersADSR = cb_adsr;
    chip_initModule(0, &g_cb, &mem);
    int bad = check_tables();
    bad |= check_averages();
    bad |= fuzz();
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad;
}
