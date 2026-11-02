/* Host checks of vowel, built with the sanitizers (out-of-bounds reads, signed overflow, division):
 *
 *   clang -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o /tmp/vowel_fuzz engines/vowel/tests/fuzz.c && /tmp/vowel_fuzz
 *
 * 1. pulses: the event source equals the per-sample difference of opz_saw_blep (minus its slope) over random
 *    pitches C0 .. 11 kHz and phases, across block boundaries (carry), within 1e-5 of full scale.
 * 2. coefficients: every P1 x P2 byte, notes 0 .. 127 with +-2 semitone bends and the singers' offsets: filter
 *    coefficients in range and the Chamberlin filter stable (det 1 - f q in (0, 1), trace condition), gains in
 *    range.
 * 3. fuzz: 200,000 blocks of random notes (velocity 0 .. 255, params NULL or random), knob jumps every block,
 *    extreme pitches and slopes, random envelope stages, coefficients and steps, voice re-use without init;
 *    checks render adds into out (prefilled vs zero buffer), leaves the shared source buffer zeroed, keeps the
 *    envelope level in [0, 1) and never wraps (no sample jumps from near +1 to near -1).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* plain names like init would interpose on the sanitizer runtime's own */
#define init vowel_init
#define render vowel_render
#define initModule vowel_initModule
#define onNoteOn vowel_onNoteOn
#define onNoteOff vowel_onNoteOff
#include "../vowel.c"

static uint32_t g_seed = 12345;
static uint32_t r32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed ^ (g_seed >> 15); }

static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r) { (void)env; (void)a; (void)d; (void)s; (void)r; }
static ModuleCallbackInterface g_cb;

static int check_pulses(void)
{
    double worst = 0;
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t inc = 0x184CBB + r32() % (INC_MAX - 0x184CBB);
        if (trial % 3 == 0) inc = 0x184CBB * (1u << (r32() % 9)) + (r32() & 0xffff);
        if (inc > INC_MAX) inc = INC_MAX;
        uint32_t p = r32(), rc = opz_blep_recip(inc);
        int32_t carry[2] = { 0, 0 };
        int32_t prev = opz_saw_blep(p - inc, inc, rc) >> 2;
        /* the reference's first samples need the wrap before the block: start the comparison at block 1 */
        for (int b = 0; b < 6; b++) {
            memset(s_x, 0, sizeof(s_x));
            s_x[0] = carry[0]; s_x[1] = carry[1];
            uint8_t run[MAX_RUNS / 2][2];
            int n = pulses(p, inc, rc, 32767, run);
            for (int k = 1; k < n; k++) if (run[k][0] < run[k - 1][1]) { printf("pulses: runs overlap\n"); return 1; }
            carry[0] = s_x[N]; carry[1] = s_x[N + 1];
            for (int i = 0; i < N; i++) {
                uint32_t pi = p + (uint32_t)i * inc;
                int32_t y = opz_saw_blep(pi, inc, rc) >> 2;
                double ref = (double)(y - prev) - (double)inc / 4.0;
                prev = y;
                double err = (s_x[i] - ref) / 2147483648.0;
                if (b > 0 && err * err > worst * worst) worst = err < 0 ? -err : err;
            }
            p += inc * N;
        }
    }
    memset(s_x, 0, sizeof(s_x));                             /* render expects it zeroed */
    printf("pulses: worst difference to the per-sample BLEP source %.2e of full scale\n", worst);
    return worst > 1e-5;                                     /* the reciprocal's rounding, as in super's check */
}

static int check_coefficients(void)
{
    State st;
    memset(&st, 0, sizeof st);
    long n = 0;
    int32_t fmax = 0, qmin = 1 << 30, qmax = 0;
    for (int track = 4; track <= 7; track += 3)
    for (int p1 = 0; p1 < 256; p1++)
        for (int p2 = 0; p2 < 256; p2++)
            for (int note = (p1 + p2) % 9; note < 128; note += 9) {
                uint32_t inc = (uint32_t)(0x184CBB * __builtin_exp2((note - 12) / 12.0 + ((p1 * 7 + p2) % 5 - 2) / 6.0));
                if (inc > INC_MAX) inc = INC_MAX;
                st.singers = track == 7 ? 2 : 1;
                st.jitter = (int32_t)(r32() % 4589) - 2294;
                st.pan = (int32_t)(r32() % 29493) - 14746;
                st.amp = (int32_t)(r32() % 32767);
                st.tone = OCT(10.2288) + (int32_t)(r32() % 190054);
                compute(&st, opz_param_from_byte((uint8_t)p1), opz_param_from_byte((uint8_t)p2), inc);
                set_gain(&st, st.gt);
                n++;
                for (int k = 0; k < NF; k++) {
                    int32_t f = st.c[k].f, q = st.c[k].q, qa = st.c[k].qa < 0 ? -st.c[k].qa : st.c[k].qa;
                    if (f <= 0 || f >= 32768 || q <= 0 || q > 0x7000 || qa > q) {
                        printf("coef out of range: p1 %d p2 %d note %d k %d f %d q %d qa %d\n", p1, p2, note, k, f, q,
                               st.c[k].qa);
                        return 1;
                    }
                    double fd = f / 32768.0, qd = q / 32768.0, det = 1 - fd * qd, tr = 2 - fd * fd - fd * qd;
                    if (!(det > 0 && det < 1 && tr < 1 + det && tr > -1 - det)) {
                        printf("unstable: p1 %d p2 %d note %d k %d\n", p1, p2, note, k);
                        return 1;
                    }
                    if (f > fmax) fmax = f;
                    if (q < qmin) qmin = q;
                    if (q > qmax) qmax = q;
                }
                if (st.gexp < 0 || st.gexp > 15 || st.gl < 0 || st.gl > 32767 || st.gr < 0 || st.gr > 32767 ||
                    st.nbase < 0 || st.nbase > 16384) {
                    printf("gain out of range: p1 %d p2 %d note %d: e %d gl %d gr %d n %d\n", p1, p2, note, st.gexp,
                           st.gl, st.gr, st.nbase);
                    return 1;
                }
            }
    printf("coefficients: %ld settings stable; f <= %d, q %d .. %d (Q15)\n", n, fmax, qmin, qmax);
    return 0;
}

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
    initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int v = 0; v < V; v++) {
        memset(&mem[v], 0x5a, sizeof mem[v]);
        init(mem[v].b, 4 + (uint32_t)(v % 4), 0, 0, 0);
    }
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte((uint8_t)r32());
    for (long blk = 0; blk < 200000; blk++) {
        int v = (int)(r32() % V);
        uint32_t r = r32();
        if ((r & 31) == 0) init(mem[v].b, 4 + (r >> 8) % 4, 0, 0, 0);
        if ((r & 7) == 1) onNoteOn((int)(r32() % 128), (uint8_t)r32(), mem[v].b, (r & 64) ? NULL : params);
        if ((r & 15) == 2) onNoteOff(0, mem[v].b);
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
        uint32_t pitch;
        uint32_t pk = r32() % 6;
        pitch = pk == 0 ? 0 : pk == 1 ? 0xffffffffu : pk == 2 ? r32() : (uint32_t)(0x184CBB * __builtin_exp2((r32() % 128) / 12.0));
        int32_t slope = (r32() % 4 == 0) ? (int32_t)r32() >> 6 : 0;
        Mem save = mem[v];
        Env esave = *e;
        memset(out_a, 0, sizeof out_a);
        render(out_a, pitch, slope, mem[v].b, params, (ADSR *)e);
        Mem after = mem[v];
        Env eafter = *e;
        mem[v] = save;
        *e = esave;
        for (int i = 0; i < 2 * N; i++) out_b[i] = 12345;
        render(out_b, pitch, slope, mem[v].b, params, (ADSR *)e);
        if (memcmp(&after, &mem[v], sizeof after) || memcmp(&eafter, e, sizeof eafter)) {
            int off = -1;
            for (int i = 0; i < (int)sizeof after; i++) if (after.b[i] != mem[v].b[i]) { off = i; break; }
            printf("fuzz: render is not deterministic (block %ld, state byte %d, env %d)\n", blk, off,
                   memcmp(&eafter, e, sizeof eafter) != 0);
            return 1;
        }
        for (int i = 0; i < 2 * N; i++) {
            if (out_a[i] > 0x7ff00000 || out_a[i] < -0x7ff00000) { sat++; continue; }
            if (out_b[i] - 12345 != out_a[i]) {
                printf("fuzz: render does not add into out (block %ld, sample %d)\n", blk, i);
                return 1;
            }
            if (i >= 2 && ((out_a[i] > 0x40000000 && out_a[i - 2] < -0x40000000) ||
                           (out_a[i] < -0x40000000 && out_a[i - 2] > 0x40000000))) {
                State *st = (State *)mem[v].b;
                printf("fuzz: wrap-like jump (block %ld, sample %d): %d -> %d; pitch %u slope %d p1 %u p2 %u "
                       "stage %d level %d coef %d step %d e %d gl %d gr %d singers %d\n", blk, i, out_a[i - 2],
                       out_a[i], pitch, slope, params[0] >> 23, params[1] >> 23, esave.w[4], esave.w[1], esave.w[2],
                       esave.w[3], st->gexp, st->gl, st->gr, st->singers);
                for (int j = 0; j < 2 * N; j += 2) printf("%d ", out_a[j] >> 20);
                printf("\n");
                return 1;
            }
        }
        samples += 2 * N;
        for (int i = 0; i < N + 2; i++)
            if (s_x[i]) { printf("fuzz: s_x not cleared (block %ld)\n", blk); return 1; }
        if (e->w[1] < 0) { printf("fuzz: negative envelope level (block %ld)\n", blk); return 1; }
        for (int i = (int)sizeof(State); i < (int)sizeof(Mem); i++)
            if (mem[v].b[i] != 0x5a) { printf("fuzz: state overflow (block %ld)\n", blk); return 1; }
    }
    printf("fuzz: 200000 blocks ok, %.4f %% of samples at full scale (random envelope levels up to 1.0)\n",
           100.0 * sat / samples);
    return 0;
}

int main(void)
{
    g_cb.updateParametersADSR = cb_adsr;
    int fail = check_pulses();
    fail |= check_coefficients();
    fail |= fuzz();
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
