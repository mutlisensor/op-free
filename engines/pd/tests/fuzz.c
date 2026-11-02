/* Host checks of pd, built with the sanitizers (out-of-bounds reads, signed overflow, division):
 *
 *   clang -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o /tmp/pd_fuzz engines/pd/tests/fuzz.c && /tmp/pd_fuzz
 *
 * 1. waves: every wave's fixed-point loop against its floating-point definition (the same maths as gen_tables.py)
 *    over one period at 8 amounts and 9 pitches, shape held: within 2e-3 of full scale (the cosine table's
 *    interpolation, the 16-bit buffers and the quantised slopes), double sine's polyBLAMP left out.
 * 2. shapes: every wave x every amount (Q16 steps of 64) x pitches 0 .. 2^32 - 1: knees, slopes, gains and DC in
 *    range, and the saw's two halves meet at the knee within 2e-3 of a turn.
 * 3. smooth: P1 and P2 sweeping (the host's smoothing of a knob turned end to end every 8 or 40 blocks, and a
 *    jump), held notes C1 .. C8: the step from one block into the next is no larger than the largest step inside
 *    a block (x 1.25 + 2^-16 FS): no clicks or zipper at block boundaries.
 * 4. fuzz: 500,000 blocks of random notes (velocity 0 .. 255, params NULL or random), knob jumps every block,
 *    extreme pitches and slopes, random envelope stages, coefficients and steps, voice re-use without init; checks
 *    render adds into out (prefilled vs zero buffer), is deterministic, stays below 0.75 of full scale (the
 *    envelope level is random up to 1.0) and writes nothing past its state.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* plain names like init would interpose on the sanitizer runtime's own */
#define init pd_init
#define render pd_render
#define initModule pd_initModule
#define onNoteOn pd_onNoteOn
#define onNoteOff pd_onNoteOff
#include "../pd.c"

static uint32_t g_seed = 12345;
static uint32_t r32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed ^ (g_seed >> 15); }

/* opzsim's model is not needed here: stages and levels are set by the tests themselves */
static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r) { (void)env; (void)a; (void)d; (void)s; (void)r; }
static ModuleCallbackInterface g_cb;

/* ---- 1. the waves against their definitions -------------------------------------------------------------------- */

static double frac(double x) { return x - floor(x); }

/* The wave at the engine's own shape parameters (knee, s as its loops read it, the saw's slow slope, k, a). */
static double ref_wave(int w, double ph, const Shape *sh)
{
    double th, kn = sh->knee / 4294967296.0, s = (double)(sh->sl >> 16) / 128.0;
    switch (w) {
    case SAW: th = fmin(ph, kn) * s + fmax(ph - kn, 0.0) * (double)(sh->r >> 15) / 32768.0; break;
    case SQUARE: { int h = ph >= 0.5; double u = 2 * ph - h; th = 0.5 * h + 0.5 * fmin(u, kn) * s; break; }
    case PULSE: th = fmin(ph, kn) * s; break;
    case DBL: th = ph + sh->knee / 2147483648.0 * (frac(2 * ph) - ph); break;
    case SAWPULSE: th = fmin(ph, kn) * s + fmin(fmax(ph - kn, 0.0), 0.5); break;
    default: {
        double W, k = (double)(sh->sl >> 16) / 256.0;
        if (w == RES1) W = (1 - ph) * (1 - ph);
        else if (w == RES2) W = pow(4 * ph * (1 - ph), 2);
        else if (ph < 0.5) W = 1;
        else { double t = 2 * (1 - ph); W = t * t * (3 - 2 * t); }
        return 1 - W * (1 - cos(2 * M_PI * frac(ph * k)));
    }
    }
    return cos(2 * M_PI * th);
}

static int check_waves(void)
{
    double worst = 0;
    int ww = 0;
    for (int w = 0; w < NWAVES; w++)
        for (int ai = 0; ai <= 8; ai++)
            for (int note = 24; note <= 120; note += 12) {
                int32_t a = ai * 8192;
                double f = 440.0 * pow(2.0, (note - 69) / 12.0);
                uint32_t inc = (uint32_t)llround(f / 44100.0 * 4294967296.0);
                Shape sh;
                shape(&sh, w, a, log2_q16(inc));
                uint32_t ph = r32();
                int16_t y[N];
                for (int b = 0; b < 16; b++) {
                    if (w == DBL) pass_dbl(y, ph, inc, 0, &sh, &sh, opz_blep_recip(inc), 0);
                    else run(w, y, ph, inc, 0, &sh, &sh, 0, 0);
                    for (int i = 0; i < N; i++) {
                        double r = ref_wave(w, (double)(uint32_t)(ph + (uint32_t)i * inc) / 4294967296.0, &sh);
                        if (w == DBL) r *= 0.5;
                        double e = fabs(y[i] / 32768.0 - r);
                        if (e > worst) {
                            worst = e; ww = w;
                            if (getenv("PD_DEBUG"))
                                printf("  w %d a %d note %d ph %.5f y %.5f ref %.5f knee %.5f\n", w, a, note,
                                       (double)(uint32_t)(ph + (uint32_t)i * inc) / 4294967296.0, y[i] / 32768.0, r,
                                       sh.knee / 4294967296.0);
                        }
                    }
                    ph += inc * N;
                }
            }
    printf("waves: worst error %.2e of full scale (wave %d)\n", worst, ww);
    return worst > 1e-3;
}

/* ---- 2. shape ranges -------------------------------------------------------------------------------------------- */

static int check_shapes(void)
{
    static const uint32_t incs[] = { 0, 1, 0x184CBB, 0x1000000, 0x4000000, 0x10000000, 0x40000000, 0x80000000u,
                                     0xffffffffu };
    double worst_knee = 0;
    for (int w = 0; w < NWAVES; w++)
        for (int32_t a = 0; a <= 65536; a += 64)
            for (unsigned k = 0; k < sizeof incs / sizeof incs[0]; k++) {
                Shape sh;
                shape(&sh, w, a, log2_q16(incs[k]));
                if (sh.gh < 0 || sh.gh > 32767 || sh.dc < -32768 || sh.dc > 32767 || sh.sw < 0 || sh.sw > 32768) {
                    printf("shapes: gain/dc out of range (wave %d, a %d)\n", w, a);
                    return 1;
                }
                if (w != DBL && w < RES1 && (sh.sl < (128 << 16) || sh.sl > (128 << 23) || sh.knee < (1u << 24))) {
                    printf("shapes: slope/knee out of range (wave %d, a %d, inc %u): sl %d knee %u\n", w, a,
                           incs[k], sh.sl, sh.knee);
                    return 1;
                }
                if (w >= RES1 && (sh.sl < (1 << 24) || sh.sl > (32 << 24))) {
                    printf("shapes: k out of range (wave %d, a %d)\n", w, a);
                    return 1;
                }
                if (w == SAW) {                               /* the fast half ends near 1/2, the slow one at 1 */
                    double at = (double)((sh.knee >> 7) * (uint32_t)(sh.sl >> 16)) / 4294967296.0;
                    double slow = (double)(sh.r >> 15) / 32768.0 * (1.0 - sh.knee / 4294967296.0);
                    if (fabs(at - 0.5) > 4e-3 || fabs(at + slow - 1.0) > 1e-4) {
                        printf("shapes: saw halves at %.5f and %.5f (a %d)\n", at, at + slow, a);
                        return 1;
                    }
                    if (fabs(at - 0.5) > worst_knee) worst_knee = fabs(at - 0.5);
                }
            }
    printf("shapes: ok, the saw's knee within %.2e of half a turn, its end at one turn\n", worst_knee);
    return 0;
}

/* ---- 3. no steps at block boundaries ---------------------------------------------------------------------------- */

typedef struct { int32_t w[6]; } Env;

static int check_smooth(void)
{
    int fail = 0;
    double worst_ratio = 0;
    initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int mode = 0; mode < 3; mode++)
        for (int note = 24; note <= 108; note += 12) {
            uint8_t st[OPZ_VOICE_STATE_BYTES];
            Env e = { { 0, 0x40000000, 0x7fffffff, 0, 3, 0 } };   /* sustain at half: a steady envelope */
            uint32_t params[OPZ_NUM_PARAMS];
            for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte(128);
            params[OPZ_P_P1] = params[OPZ_P_P2] = 0;
            params[OPZ_P_DECAY] = opz_param_from_byte(60);
            init(st, 5, 0, 0, 0);
            onNoteOn(note, 100, st, params);
            uint32_t pitch = (uint32_t)llround(440.0 * pow(2.0, (note - 69) / 12.0) / 44100.0 * 4294967296.0);
            double prev[2] = { 0, 0 }, inner = 0, edge = 0;
            int period = mode == 0 ? 8 : 40;
            for (int b = 0; b < 400; b++) {
                /* the target a knob turned end to end (triangles; mode 2: a jump every 50 blocks), then the
                 * host's one-pole smoothing */
                uint32_t t1, t2;
                if (mode < 2) {
                    int ph1 = b % (2 * period), ph2 = b % (3 * period);
                    t1 = opz_param_from_byte((uint8_t)(255 * (ph1 < period ? ph1 : 2 * period - ph1) / period));
                    t2 = opz_param_from_byte((uint8_t)(255 * (ph2 < 3 * period / 2 ? ph2 : 3 * period - ph2) /
                                                       (3 * period / 2)));
                } else {
                    t1 = opz_param_from_byte((b / 50) & 1 ? 255 : 0);
                    t2 = opz_param_from_byte((uint8_t)((b / 50) * 37));
                }
                params[0] += (uint32_t)((int32_t)(t1 - params[0]) >> 6);
                params[1] += (uint32_t)((int32_t)(t2 - params[1]) >> 6);
                e.w[4] = 3;
                e.w[1] = 0x40000000;
                int32_t out[2 * N];
                memset(out, 0, sizeof out);
                render(out, pitch, 0, st, params, (ADSR *)&e);
                for (int i = 0; i < N; i++) {
                    double y = out[2 * i] / 2147483648.0;
                    double d2 = fabs(y - 2 * prev[0] + prev[1]);
                    if (b > 0) {
                        if (i < 2) { if (d2 > edge) edge = d2; }
                        else if (d2 > inner) inner = d2;
                    }
                    prev[1] = prev[0];
                    prev[0] = y;
                }
            }
            double ratio = edge / (inner + 1e-12);
            if (ratio > worst_ratio) worst_ratio = ratio;
            if (edge > inner * 1.25 + 1.0 / 65536) {
                printf("smooth: step at a block boundary, mode %d note %d: %.5f vs %.5f inside\n", mode, note, edge,
                       inner);
                fail = 1;
            }
        }
    printf("smooth: worst boundary / interior second difference %.2f\n", worst_ratio);
    return fail;
}

/* ---- 4. fuzz ---------------------------------------------------------------------------------------------------- */

typedef struct { uint8_t b[OPZ_VOICE_STATE_BYTES + 64]; } Mem;

static int fuzz(void)
{
    enum { V = 6 };
    static Mem mem[V];
    static Env env[V];
    uint32_t params[OPZ_NUM_PARAMS];
    int32_t out_a[2 * N], out_b[2 * N];
    double peak = 0;
    initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int v = 0; v < V; v++) {
        memset(&mem[v], 0x5a, sizeof mem[v]);
        init(mem[v].b, 4 + (uint32_t)(v % 4), 0, 0, 0);
    }
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte((uint8_t)r32());
    for (long blk = 0; blk < 500000; blk++) {
        int v = (int)(r32() % V);
        uint32_t r = r32();
        if ((r & 31) == 0) init(mem[v].b, 4 + (r >> 8) % 4, 0, 0, 0);
        if ((r & 7) == 1) onNoteOn((int)(r32() % 128), (uint8_t)r32(), mem[v].b, (r & 64) ? NULL : params);
        if ((r & 15) == 2) onNoteOff(0, mem[v].b);
        /* knobs: jumps, extremes or the host's smoothing (P1, P2 and the decay, which sets the DCW time) */
        for (int i = 0; i < 4; i++) {
            uint32_t k = r32() % 4;
            params[i] = k == 0 ? opz_param_from_byte((uint8_t)r32()) : k == 1 ? (r32() & 1 ? 0x7fffffffu : 0)
                      : k == 2 ? params[i] + (uint32_t)((int32_t)(opz_param_from_byte((uint8_t)r32()) - params[i]) >> 6)
                      : params[i];
        }
        if (r32() % 64 == 0) params[1] = r32();              /* not even a host value */
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
            printf("fuzz: render is not deterministic (block %ld)\n", blk);
            return 1;
        }
        for (int i = 0; i < 2 * N; i++) {
            if (out_b[i] - 12345 != out_a[i]) {
                printf("fuzz: render does not add into out (block %ld, sample %d)\n", blk, i);
                return 1;
            }
            double y = fabs(out_a[i] / 2147483648.0);
            if (y > peak) peak = y;
            if (y > 0.75) {
                State *st = (State *)mem[v].b;
                printf("fuzz: sample %.3f FS (block %ld): pitch %u slope %d p1 %u p2 %u stage %u level %d a %d x %d\n",
                       y, blk, pitch, slope, params[0] >> 23, params[1] >> 23, stage, esave.w[1], st->a_prev,
                       st->x_prev);
                return 1;
            }
        }
        if (e->w[1] < 0) { printf("fuzz: negative envelope level (block %ld)\n", blk); return 1; }
        for (int i = (int)sizeof(State); i < (int)sizeof(Mem); i++)
            if (mem[v].b[i] != 0x5a) { printf("fuzz: state overflow (block %ld)\n", blk); return 1; }
    }
    printf("fuzz: 500000 blocks ok, loudest sample %.3f FS (random envelope levels up to 1.0)\n", peak);
    return 0;
}

int main(void)
{
    g_cb.updateParametersADSR = cb_adsr;
    int fail = check_waves();
    fail |= check_shapes();
    fail |= check_smooth();
    fail |= fuzz();
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
