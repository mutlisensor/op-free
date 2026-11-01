/* Host checks of ensemble, built with the sanitizers (out-of-bounds reads, signed overflow, division):
 *
 *   clang -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o /tmp/ensemble_fuzz engines/ensemble/tests/fuzz.c && /tmp/ensemble_fuzz
 *
 * 1. oscillator: one copy's events, integrated, equal the per-sample band-limited footage mix
 *    a saw(p) + b (saw(p) - saw(p + 1/2)) + c saw(2p), each saw from opz_saw_blep, over random footages,
 *    pitches C0 .. 5 kHz (where the 4' is gone) and phases, across block boundaries.
 * 2. grid: every P1 byte x every third P2 byte, each with its own note 0 .. 127, velocity and synth track: the
 *    taps' increments stay within 6.5 % of the pitch and no sample passes half scale; the footage sum stays
 *    <= 32767 at every P1 word.
 * 3. fuzz: 300,000 blocks (or argv[1], seed argv[2]) of random notes (velocity 0 .. 255, params NULL or random),
 *    knob jumps every block, extreme pitches and slopes, random envelope stages, coefficients and steps, voice
 *    re-use without init, tracks 0 .. 15; checks render adds into out (prefilled vs zero buffer), is
 *    deterministic (the shared LFO table included), leaves the event buffers zeroed, keeps the envelope level
 *    >= 0, writes nothing past 280 bytes of state and never wraps (no sample jumps from near +1 to near -1).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* plain names like init would interpose on the sanitizer runtime's own */
#define init ens_init
#define render ens_render
#define initModule ens_initModule
#define onNoteOn ens_onNoteOn
#define onNoteOff ens_onNoteOff
#include "../ensemble.c"

static uint32_t g_seed = 12345;
static uint32_t r32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed ^ (g_seed >> 15); }

static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    (void)env; (void)a; (void)d; (void)s; (void)r;
}
static ModuleCallbackInterface g_cb;

static int check_oscillator(void)
{
    double worst = 0;
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t inc = 0x184CBB + r32() % (INC_4HI - 0x184CBB);
        if (trial % 3 == 0) inc = 0x184CBB * (1u << (r32() % 8)) + (r32() & 0xffff);
        int32_t a = (int32_t)(r32() % 32768), b = (int32_t)(r32() % (32768 - a)), c = 32767 - a - b;
        if (trial % 4 == 1) c = 0;
        if (trial % 4 == 2) b = 0;
        int32_t wm = (int32_t)(r32() >> 2), ws = (int32_t)(r32() >> 2) - 0x20000000;
        uint32_t ph[COPIES] = { r32(), 0, 0, 0 }, incs[COPIES] = { inc, inc, inc, inc };
        const int32_t wmv[COPIES] = { wm, 0, 0, 0 }, wsv[COPIES] = { ws, 0, 0, 0 };
        uint32_t rc = opz_blep_recip(inc), rc2 = opz_blep_recip(2 * inc);
        for (int blk = 0; blk < 6; blk++) {
            uint32_t p = ph[0], acc[4];
            copies(ph, incs, wmv, wsv, a, b, c, acc);
            uint32_t m = acc[0] - acc[1], sd = acc[2] - acc[3];
            for (int i = 0; i < N; i++) {
                m += acc[1] + s_em[i];
                sd += acc[3] + s_es[i];
                s_em[i] = s_es[i] = 0;
                uint32_t pi = p + (uint32_t)i * inc;
                double y = a / 32768.0 * opz_saw_blep(pi, inc, rc)
                         + b / 32768.0 * ((double)opz_saw_blep(pi, inc, rc) - opz_saw_blep(pi + 0x80000000u, inc, rc))
                         + c / 32768.0 * opz_saw_blep(2 * pi, 2 * inc, rc2);
                double em = ((int32_t)m - y * (wm / 2147483648.0)) / 2147483648.0;
                double es = ((int32_t)sd - y * (ws / 2147483648.0)) / 2147483648.0;
                if (blk > 0) {                               /* block 0 has no crossing before it */
                    if (fabs(em) > worst) worst = fabs(em);
                    if (fabs(es) > worst) worst = fabs(es);
                }
            }
            s_em[N] = s_es[N] = 0;
        }
    }
    printf("oscillator: worst difference to the per-sample BLEP footage mix %.2e of full scale\n", worst);
    return worst > 2e-5;
}

typedef struct { uint8_t b[OPZ_VOICE_STATE_BYTES + 64]; } Mem;
typedef struct { int32_t w[6]; } Env;

static int check_grid(void)
{
    static Mem mem;
    Env e = { { 0, 0x40000000, 0x7fffffff, 0, 3, 0 } };
    uint32_t params[OPZ_NUM_PARAMS];
    int32_t out[2 * N];
    long n = 0;
    initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte(0);
    for (int p1 = 0; p1 < 256; p1++)
        for (int p2 = 0; p2 < 256; p2 += 3) {
            int note = (p1 * 7 + p2) % 128, vel = (p1 + p2 * 3) % 128;
            init(mem.b, 4 + (uint32_t)((p1 + p2) % 4), 0, 0, 0);
            params[0] = opz_param_from_byte((uint8_t)p1);
            params[1] = opz_param_from_byte((uint8_t)p2);
            onNoteOn(note, (uint8_t)vel, mem.b, params);
            uint32_t pitch = (uint32_t)(0x184CBB * exp2((note - 12) / 12.0));
            for (int blk = 0; blk < 3; blk++) {
                memset(out, 0, sizeof out);
                State *st = (State *)mem.b;
                uint32_t ph0[COPIES];
                memcpy(ph0, st->ph, sizeof ph0);
                render(out, pitch, 0, mem.b, params, (ADSR *)&e);
                uint32_t inc = pitch > INC_MAX ? INC_MAX : pitch < INC_MIN ? INC_MIN : pitch;
                for (int k = 1; k < COPIES && blk > 0; k++) {
                    double r = 1 + (double)(int32_t)(st->ph[k] - ph0[k] - inc * N) / N / inc;   /* mod 2^32 */
                    if (r < 0.935 || r > 1.065) {
                        printf("grid: tap %d increment ratio %.4f (p1 %d p2 %d note %d)\n", k, r, p1, p2, note);
                        return 1;
                    }
                }
                for (int i = 0; i < 2 * N; i++)
                    if (out[i] > 0x40000000 || out[i] < -0x40000000) {
                        printf("grid: sample %d at %.3f of full scale (p1 %d p2 %d note %d)\n", i,
                               out[i] / 2147483648.0, p1, p2, note);
                        return 1;
                    }
                n++;
            }
        }
    /* registration sums at every P1 word, including those between bytes */
    for (int32_t x1 = 0; x1 < 65536; x1++) {
        int seg = x1 >> 14;
        int32_t f1 = (x1 & 0x3fff) << 1;
        const Reg *r0 = &k_reg[seg], *r1 = r0 + 1;
        int32_t a = lerp15(r0->a, r1->a, f1), b = lerp15(r0->b, r1->b, f1), c = lerp15(r0->c, r1->c, f1);
        if (a < 0 || b < 0 || c < 0 || a + b + c > 32767) {
            printf("grid: footages %d %d %d at P1 word %d\n", a, b, c, x1);
            return 1;
        }
    }
    printf("grid: %ld blocks (every P1 byte x every third P2 byte, all notes) in range; footage sums ok\n", n);
    return 0;
}

static int fuzz(long blocks)
{
    enum { V = 8 };
    static Mem mem[V];
    static Env env[V];
    uint32_t params[OPZ_NUM_PARAMS];
    int32_t out_a[2 * N], out_b[2 * N];
    long sat = 0, samples = 0;
    int32_t loudest = 0;
    initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int v = 0; v < V; v++) {
        memset(&mem[v], 0x5a, sizeof mem[v]);
        init(mem[v].b, (uint32_t)(r32() % 16), 0, 0, 0);
    }
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte((uint8_t)r32());
    for (long blk = 0; blk < blocks; blk++) {
        int v = (int)(r32() % V);
        uint32_t r = r32();
        if ((r & 31) == 0) init(mem[v].b, (r >> 8) % 16, 0, 0, 0);
        if ((r & 7) == 1) onNoteOn((int)(r32() % 128), (uint8_t)r32(), mem[v].b, (r & 64) ? NULL : params);
        if ((r & 15) == 2) onNoteOff(0, mem[v].b);
        for (int i = 0; i < 2; i++) {                       /* knobs: jumps, extremes or the host's smoothing */
            uint32_t k = r32() % 4;
            params[i] = k == 0 ? opz_param_from_byte((uint8_t)r32()) : k == 1 ? (r32() & 1 ? 0x7fffffffu : 0)
                      : k == 2 ? params[i] + (uint32_t)((int32_t)(opz_param_from_byte((uint8_t)r32()) - params[i]) >> 6)
                      : params[i];
        }
        Env *e = &env[v];
        e->w[4] = (int32_t)(1 + r32() % 6);
        if (r32() % 8 == 0) e->w[1] = (int32_t)(r32() >> 1);
        e->w[2] = (r32() & 1) ? 0x7fffffff : 0x70000000 + (int32_t)(r32() % 0x10000000);
        uint32_t sk = r32() % 4;
        e->w[3] = sk == 0 ? 0 : sk == 1 ? (int32_t)(r32() >> 4) : sk == 2 ? -(int32_t)(r32() >> 6)
                : (int32_t)(r32() >> 1);
        uint32_t pk = r32() % 6;
        uint32_t pitch = pk == 0 ? 0 : pk == 1 ? 0xffffffffu : pk == 2 ? r32()
                       : (uint32_t)(0x184CBB * exp2((r32() % 128) / 12.0));
        int32_t slope = (r32() % 4 == 0) ? (int32_t)r32() >> 6 : 0;
        Mem save = mem[v];
        Env esave = *e;
        uint32_t lfo_save[16][2];
        memcpy(lfo_save, s_lfo, sizeof lfo_save);
        memset(out_a, 0, sizeof out_a);
        render(out_a, pitch, slope, mem[v].b, params, (ADSR *)e);
        Mem after = mem[v];
        Env eafter = *e;
        uint32_t lfo_after[16][2];
        memcpy(lfo_after, s_lfo, sizeof lfo_after);
        mem[v] = save;
        *e = esave;
        memcpy(s_lfo, lfo_save, sizeof lfo_save);
        for (int i = 0; i < 2 * N; i++) out_b[i] = 12345;
        render(out_b, pitch, slope, mem[v].b, params, (ADSR *)e);
        if (memcmp(&after, &mem[v], sizeof after) || memcmp(&eafter, e, sizeof eafter) ||
            memcmp(lfo_after, s_lfo, sizeof lfo_after)) {
            printf("fuzz: render is not deterministic (block %ld)\n", blk);
            return 1;
        }
        for (int i = 0; i < 2 * N; i++) {
            int32_t x = out_a[i] < 0 ? -out_a[i] : out_a[i];
            if (x > loudest && esave.w[1] < 0x7f000000) loudest = x;
            if (x > 0x7ff00000) { sat++; continue; }
            if (out_b[i] - 12345 != out_a[i]) {
                printf("fuzz: render does not add into out (block %ld, sample %d)\n", blk, i);
                return 1;
            }
            if (i >= 2 && ((out_a[i] > 0x40000000 && out_a[i - 2] < -0x40000000) ||
                           (out_a[i] < -0x40000000 && out_a[i - 2] > 0x40000000))) {
                printf("fuzz: wrap-like jump (block %ld, sample %d): %d -> %d; pitch %u slope %d p1 %u p2 %u\n",
                       blk, i, out_a[i - 2], out_a[i], pitch, slope, params[0] >> 23, params[1] >> 23);
                return 1;
            }
        }
        samples += 2 * N;
        for (int i = 0; i <= N; i++)
            if (s_em[i] || s_es[i]) {
                if (i == N) { s_em[N] = s_es[N] = 0; continue; }   /* index N is never read; cleared here only */
                printf("fuzz: event buffers not cleared (block %ld)\n", blk);
                return 1;
            }
        if (e->w[1] < 0) { printf("fuzz: negative envelope level (block %ld)\n", blk); return 1; }
        for (int i = 280; i < (int)sizeof(Mem); i++)
            if (mem[v].b[i] != 0x5a) { printf("fuzz: state beyond 280 bytes written (block %ld)\n", blk); return 1; }
    }
    printf("fuzz: %ld blocks ok, %.4f %% of samples at full scale, loudest %.1f dBFS (random envelope levels)\n",
           blocks, 100.0 * sat / samples, 20 * log10(loudest / 2147483648.0));
    return 0;
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 300000;         /* fuzz [blocks [seed]] */
    if (argc > 2) g_seed = (uint32_t)atol(argv[2]);
    g_cb.updateParametersADSR = cb_adsr;
    int fail = check_oscillator();
    fail |= check_grid();
    fail |= fuzz(blocks);
    printf(fail ? "FAIL\n" : "ok\n");
    return fail;
}
