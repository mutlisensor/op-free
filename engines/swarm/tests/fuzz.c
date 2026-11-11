/* Host checks of swarm, built with the sanitizers (out-of-bounds reads, signed overflow, division):
 *
 *   clang -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o /tmp/swarm_fuzz engines/swarm/tests/fuzz.c && /tmp/swarm_fuzz
 *
 * 1. saw events: the slope + event integration (16-bit weights, the reciprocal from the note's x 2^-o, the drift
 *    as 1 - d) equals a per-sample sum of opz_saw_blep saws over random pitches C0 .. 6 kHz, offsets up to the
 *    inharmonic ratios, drifts, weights and phases (every 7th trial starts the saws just before a wrap), within
 *    1e-4 of full scale (the reciprocals are good to ~1e-4; the residual scales with them).
 * 2. grid: every P1 x P2 byte at notes 0 .. 127 (every 7th) and both ends of the keyboard, two blocks after a
 *    note-on and a later block, the envelope at 1.0: the voice's peak stays below 0.95 of full scale and the
 *    shared buffers are left clean.
 * 3. fuzz: 200,000 blocks of random notes (velocity 0 .. 255, params NULL or random), knob jumps every block,
 *    extreme pitches and slopes, random envelope stages, coefficients and steps, voice re-use without init;
 *    checks render is deterministic, adds into out (prefilled vs zero buffer), leaves the event buffers zeroed,
 *    keeps the envelope level >= 0, never wraps (no sample jumps from near +1 to near -1) and never writes past
 *    the state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* plain names like init would interpose on the sanitizer runtime's own */
#define init swarm_init
#define render swarm_render
#define initModule swarm_initModule
#define onNoteOn swarm_onNoteOn
#define onNoteOff swarm_onNoteOff
#include "../swarm.c"

static uint32_t g_seed = 12345;
static uint32_t r32(void) { g_seed = g_seed * 1664525u + 1013904223u; return g_seed ^ (g_seed >> 15); }

static void cb_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r) { (void)env; (void)a; (void)d; (void)s; (void)r; }
static ModuleCallbackInterface g_cb;

static int clean(void)
{
    for (int i = 0; i < N; i++)
        if (s_el[i] || s_er[i]) return 0;
    return 1;
}

static int check_events(void)
{
    double worst = 0;
    long blocks = 0;
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t inc = 0x184CBB * (1u << (r32() % 8)) + (r32() & 0xfffff);
        int32_t lo = log2_q16(inc);
        uint32_t ph[NS], ik[NS], rc[NS];
        int32_t wl[NS], wr[NS];
        uint32_t rc0 = opz_blep_recip(inc);
        int ns = 0;
        for (int k = 0; k < NS; k++) {
            int32_t o = (int32_t)(r32() % (uint32_t)OCT(2.4)) - OCT(0.17);
            if (lo + o >= SAW_HF1) continue;
            uint32_t ib = opz_mul_u32_shr16(inc, (uint32_t)opz_exp2_q16(o));
            int32_t dr = DRIFT * ((int32_t)(r32() % 65535) - 32767);
            ik[ns] = ib + (uint32_t)opz_mul_q31((int32_t)ib, dr);
            uint32_t rb = opz_mul_u32_shr16(rc0, (uint32_t)opz_exp2_q16(-o));
            rc[ns] = rb - (uint32_t)opz_mul_q31((int32_t)rb, dr);
            ph[ns] = trial % 7 == 0 ? (uint32_t)-(int32_t)(k * 1000) : r32();
            wl[ns] = (int32_t)(r32() % 2400);               /* sum of |w| below 1, as in the engine */
            wr[ns] = (int32_t)(r32() % 2400) - 600;
            ns++;
        }
        for (int b = 0; b < 4; b++, blocks++) {
            uint32_t l = 0, dl = 0, r = 0, dr = 0;
            for (int k = 0; k < ns; k++) {
                int32_t y = (int32_t)(ph[k] ^ 0x80000000u);
                l += (uint32_t)opz_mul_q31_q15(y, wl[k]);
                r += (uint32_t)opz_mul_q31_q15(y, wr[k]);
                dl += (uint32_t)opz_mul_q31_q15((int32_t)ik[k], wl[k]);
                dr += (uint32_t)opz_mul_q31_q15((int32_t)ik[k], wr[k]);
                uint32_t p = ph[k];
                if (p < ik[k] || ik[k] >= (1u << 26) || ~p < (ik[k] << 6)) saw_events(p, ik[k], rc[k], wl[k], wr[k]);
            }
            l -= dl;
            r -= dr;
            for (int i = 0; i < N; i++) {
                l += dl + (uint32_t)s_el[i];
                r += dr + (uint32_t)s_er[i];
                s_el[i] = s_er[i] = 0;
                double rl = 0, rr = 0;
                for (int k = 0; k < ns; k++) {
                    double v = opz_saw_blep(ph[k] + ik[k] * (uint32_t)i, ik[k], opz_blep_recip(ik[k])) / 2147483648.0;
                    rl += v * wl[k] / 32768.0;
                    rr += v * wr[k] / 32768.0;
                }
                double e1 = rl - (int32_t)l / 2147483648.0, e2 = rr - (int32_t)r / 2147483648.0;
                if (e1 < 0) e1 = -e1;
                if (e2 < 0) e2 = -e2;
                if (e1 > worst) worst = e1;
                if (e2 > worst) worst = e2;
                if (e1 > 1e-4 || e2 > 1e-4) {
                    printf("events: FAIL trial %d block %d sample %d: error %g / %g\n", trial, b, i, e1, e2);
                    return 1;
                }
            }
            for (int k = 0; k < ns; k++) ph[k] += ik[k] * N;
        }
        s_el[N] = s_er[N] = 0;
    }
    printf("events: %ld blocks, worst difference to the per-sample BLEP saws %.2e of full scale\n", blocks, worst);
    return 0;
}

typedef struct { uint8_t b[OPZ_VOICE_STATE_BYTES + 64]; } Mem;
typedef struct { int32_t w[6]; } Env;

static int check_grid(void)
{
    static Mem m;
    Env e;
    uint32_t params[OPZ_NUM_PARAMS] = { 0 };
    params[OPZ_P_PAN] = opz_param_from_byte(128);           /* the track's level and pan: full, centre */
    params[OPZ_P_LEVEL] = opz_param_from_byte(255);
    int32_t out[2 * N];
    long n = 0;
    double peak = 0;
    initModule(0, &g_cb, &(uint32_t){ 0 });
    for (int p1 = 0; p1 < 256; p1++)
        for (int p2 = 0; p2 < 256; p2++)
            for (int note = (p1 + p2) % 7; note < 128 + 7; note += 7) {
                int nt = note >= 128 ? ((p1 ^ p2) & 1 ? 0 : 127) : note;
                params[0] = opz_param_from_byte((uint8_t)p1);
                params[1] = opz_param_from_byte((uint8_t)p2);
                init(m.b, 4 + (uint32_t)(note % 4), 0, 0, 0);
                onNoteOn(nt, 127, m.b, params);
                uint32_t inc = (uint32_t)(0x184CBB * __builtin_exp2((nt - 12) / 12.0));
                for (int b = 0; b < 3; b++) {
                    memset(&e, 0, sizeof e);
                    e.w[1] = 0x7fffffff;                     /* level 1.0, sustain */
                    e.w[2] = 0x7fffffff;
                    e.w[4] = 3;
                    memset(out, 0, sizeof out);
                    render(out, inc, 0, m.b, params, (ADSR *)&e);
                    for (int i = 0; i < 2 * N; i++) {
                        double v = out[i] / 2147483648.0;
                        if (v < 0) v = -v;
                        if (v > peak) peak = v;
                        if (v > 0.95) {
                            printf("grid: p1 %d p2 %d note %d block %d sample %d at %.3f of full scale\n", p1, p2, nt, b,
                                   i, v);
                            return 1;
                        }
                    }
                    if (!clean()) { printf("grid: event buffers not cleared (p1 %d p2 %d)\n", p1, p2); return 1; }
                    if (b == 1) {                            /* jump ahead: a block a few seconds into the note */
                        State *st = (State *)m.b;
                        st->t += 3000;
                    }
                    n++;
                }
            }
    printf("grid: %ld blocks over every P1 x P2 byte, loudest sample %.3f of full scale\n", n, peak);
    return 0;
}

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
            params[i] = k == 0 ? opz_param_from_byte((uint8_t)r32()) : k == 1 ? (r32() & 1 ? 0xffffffffu : 0)
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
                       : (uint32_t)(0x184CBB * __builtin_exp2((r32() % 128) / 12.0));
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
                printf("fuzz: wrap-like jump (block %ld, sample %d): %d -> %d; pitch %u slope %d p1 %u p2 %u "
                       "stage %d level %d\n", blk, i, out_a[i - 2], out_a[i], pitch, slope, params[0] >> 23,
                       params[1] >> 23, esave.w[4], esave.w[1]);
                return 1;
            }
        }
        samples += 2 * N;
        if (!clean()) { printf("fuzz: event buffers not cleared (block %ld)\n", blk); return 1; }
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
    printf("state: %d bytes of 280\n", (int)sizeof(State));
    int fail = check_events();
    fail |= check_grid();
    fail |= fuzz();
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
