/*
 * fuzz.c — mallet's random-input check on the host, under UBSan (with array bounds):
 *
 *   clang -std=c11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o build/mallet_fuzz engines/mallet/tests/fuzz.c && build/mallet_fuzz [blocks]
 *
 * (add address to -fsanitize where AddressSanitizer runs; on this project's macOS it fails at start-up)
 *
 * Eight voices (every drum voice of the OP-Z: four tracks x 2) get random note-ons (notes -300 .. 427, velocity
 * 0 .. 255, params NULL or not), note-offs, re-inits, steals (the host's stage 5/6 at any moment, also before the
 * first render), and renders with random pitch words and slopes (drum tracks: ignored), P1/P2 words including above
 * the host's range and knob jumps every block, NULL params in render, and an envelope whose stage the host may set
 * to anything. Checked after every call: no sanitizer report (out-of-bounds index, signed overflow, invalid shift),
 * the voice's contribution stays below 0.6 of full scale, the amplitudes stay non-negative and their sum within its
 * bound (no blow-up at any P1, P2 or note), a voice the engine frees has level 0, a stolen voice is freed within
 * three blocks. Then a grid pass: P1 and P2 every 15 (0 .. 255), notes 0 .. 127, velocities 1, 64, 127, each hit
 * rendered until it frees its voice (every hit must end); and the tuning: note_inc against 440 x 2^((n - 69) / 12)
 * for every note.
 * MIT licence, see CONTRIBUTING.md.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../mallet.c"

static uint32_t rng = 0x12345678u;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

typedef struct FakeEnv {
    int32_t attack, level, coef, step, stage;
    uint8_t newnote, release;
} FakeEnv;

static const ModuleCallbackInterface s_fake = { 0 };     /* mallet calls no host service */

#define VOICES 8
static union { State s; uint8_t b[OPZ_VOICE_STATE_BYTES]; } s_states[VOICES];
static FakeEnv s_env[VOICES];
static int s_steal_age[VOICES];
static uint32_t s_params[OPZ_NUM_PARAMS];
static fract32 s_out[2 * OPZ_BLOCK_FRAMES];
static long s_fail, s_sounding, s_renders;

static uint32_t word(void)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return 0x7fffffffu;
    case 2: return rnd();                                       /* above the host's range */
    default: return opz_param_from_byte((uint8_t)rnd());
    }
}

static void check(int v, const char *what, long n)
{
    const State *s = &s_states[v].s;
    int64_t sa = 0;
    for (int k = 0; k < NM; k++) {
        if (s->amp[k] < 0) {
            printf("block %ld voice %d %s: negative amplitude %d at mode %d\n", n, v, what, s->amp[k], k);
            s_fail++;
        }
        sa += s->amp[k];
    }
    if (sa > (int64_t)STRIKE_SUM * 3 / 2) {
        printf("block %ld voice %d %s: amplitude sum %lld\n", n, v, what, (long long)sa);
        s_fail++;
    }
    int32_t peak = 0;
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
        int32_t a = s_out[i] < 0 ? -s_out[i] : s_out[i];
        if (a > peak) peak = a;
    }
    if (what[0] == 'r') {
        s_renders++;
        if (peak) s_sounding++;
    }
    if (peak > 0x4c000000) {
        printf("block %ld voice %d %s: peak %d\n", n, v, what, peak);
        s_fail++;
    }
    if (s_env[v].stage == 0 && s_env[v].level != 0) {
        printf("block %ld voice %d %s: freed with level %d\n", n, v, what, s_env[v].level);
        s_fail++;
    }
}

static void render_voice(int v, long n, int null_params)
{
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
    int32_t slope = rnd() % 4 ? 0 : (int32_t)rnd();
    render(s_out, rnd() % 2 ? 2048 : rnd(), slope, &s_states[v], null_params ? NULL : s_params, (ADSR *)&s_env[v]);
    check(v, "render", n);
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 200000;
    uint32_t mem = 0;
    if (!initModule(0, &s_fake, &mem) || mem) return 1;
    for (int v = 0; v < VOICES; v++) init(&s_states[v], (uint32_t)(v / 2), 0, 0, 0);
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = opz_param_from_byte(128);
    s_params[OPZ_P_LEVEL] = opz_param_from_byte(255);                    /* the track's level: full */

    /* tuning */
    for (int n = 0; n < 128; n++) {
        double want = 440.0 * pow(2.0, (n - 69) / 12.0) / 44100.0 * 4294967296.0;
        if (fabs(note_inc(n) - want) > want * 1e-6 + 1) {
            printf("note %d: increment %u, want %.1f\n", n, note_inc(n), want);
            s_fail++;
        }
    }

    /* random calls */
    for (long n = 0; n < blocks; n++) {
        if (rnd() % 4 == 0) s_params[OPZ_P_P1] = word();        /* knob jumps, often every block */
        if (rnd() % 4 == 0) s_params[OPZ_P_P2] = word();
        if (rnd() % 64 == 0) {                                  /* a slow sweep now and then */
            s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)(n >> 2));
            s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)(n >> 3));
        }
        int v = (int)(rnd() % VOICES);
        switch (rnd() % 16) {
        case 0: case 1: case 2: {                               /* note-on on a free voice (the host's order) */
            const uint32_t *p = rnd() % 4 ? s_params : NULL;
            if (s_env[v].stage && rnd() % 2) break;             /* busy: the host would fade it first */
            int note = rnd() % 8 ? 53 + (int)(rnd() % 24) + 24 * ((int)(rnd() % 5) - 2) : (int)(rnd() % 728) - 300;
            onNoteOn(note, (uint8_t)rnd(), &s_states[v], p);
            s_env[v] = (FakeEnv){ 0, 0, 0x7fffffff, 0, 1, 1, 0 };
            s_steal_age[v] = 0;
            check(v, "note-on", n);
            break;
        }
        case 3:                                                 /* steal: the host fades the voice */
            if (s_env[v].stage) {
                s_env[v].stage = 5 + (int32_t)(rnd() % 2);
                if (!s_steal_age[v]) s_steal_age[v] = 1;
            }
            break;
        case 4: onNoteOff((int)(rnd() % 128), &s_states[v]); break;
        case 5: if (rnd() % 8 == 0) { init(&s_states[v], rnd() % 16, 0, 0, 0); s_env[v].stage = rnd() % 8; } break;
        case 6: if (rnd() % 16 == 0 && s_env[v].stage) s_env[v].stage = 1 + (int32_t)(rnd() % 7); break;
        default: break;
        }
        for (int w = 0; w < VOICES; w++) {
            render_voice(w, n, rnd() % 64 == 0);
            if (s_steal_age[w] && s_env[w].stage) {
                if (++s_steal_age[w] > 4) {
                    printf("block %ld voice %d: stolen voice still sounding after 3 blocks\n", n, w);
                    s_fail++;
                }
            } else {
                s_steal_age[w] = 0;
            }
        }
    }

    /* grid: P1 and P2 at 0, 15, 30 .. 255, every 11th note (a different set per knob pair, all 128 in turn), three
     * velocities; each hit until it frees its voice */
    long grid = 0, longest = 0;
    for (int p1 = 0; p1 < 256; p1 += 15)
        for (int p2 = 0; p2 < 256; p2 += 15)
            for (int note = 0; note < 128; note++) {
                if ((note + p1 / 15 + 3 * (p2 / 15)) % 11) continue;
                uint8_t vel = (uint8_t)((note % 3 == 0) ? 1 : (note % 3 == 1) ? 64 : 127);
                s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)p1);
                s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)p2);
                init(&s_states[0], 2, 0, 0, 0);
                onNoteOn(note, vel, &s_states[0], s_params);
                s_env[0] = (FakeEnv){ 0, 0, 0x7fffffff, 0, 1, 1, 0 };
                long b = 0;
                for (; s_env[0].stage && b < 40000; b++) {      /* 40,000 blocks = 58 s */
                    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
                    render(s_out, 2048, 0, &s_states[0], s_params, (ADSR *)&s_env[0]);
                    check(0, "grid", b);
                    grid++;
                }
                if (s_env[0].stage) {
                    printf("P1 %d P2 %d note %d: the hit never ends\n", p1, p2, note);
                    s_fail++;
                }
                if (b > longest) longest = b;
            }
    printf("mallet fuzz: %ld random blocks x %d voices (%ld of %ld renders sounding), %ld grid blocks (longest hit "
           "%.1f s), %ld failures\n", blocks, VOICES, s_sounding, s_renders, grid, longest * 64 / 44100.0, s_fail);
    return s_fail != 0;
}
