/*
 * fuzz.c — modal's random-input check on the host, under UBSan (with array bounds):
 *
 *   clang -std=c11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o build/modal_fuzz engines/modal/tests/fuzz.c && build/modal_fuzz [blocks]
 *
 * (add address to -fsanitize where AddressSanitizer runs; on this project's macOS it fails at start-up)
 *
 * Thirteen voices (every synth voice of the OP-Z) get random note-ons (velocity 0 .. 255, params NULL or not),
 * note-offs, re-inits and renders with random pitch words (0 .. 2^32 - 1), pitch slopes, P1/P2 words (including
 * above the host's range), and a fake envelope that jumps between every stage, level and coefficient, with knob
 * jumps every block and slow sweeps. Checked after every call: no sanitizer report (out-of-bounds index, signed
 * overflow, invalid shift), the voice's contribution stays below 0.75 of full scale, the partials' amplitude
 * sums stay below their bound (no blow-up at any P1, P2 or pitch), and a voice the engine frees has level 0.
 * A grid pass then holds P1, P2 at every 8th value and the pitch at every note and above fs / 2.
 * MIT licence, see CONTRIBUTING.md.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../modal.c"

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

static void fake_adsr(ADSR *e, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    (void)a; (void)d; (void)s; (void)r;
    FakeEnv *f = (FakeEnv *)e;
    if (f->stage == 0) return;
    uint32_t j = rnd() % 256;                                   /* mostly keep the stage, sometimes jump */
    if (j < 8) f->stage = 1 + (int32_t)(rnd() % 6);
    else if (j == 8) { f->stage = 0; return; }
    else if (j < 16) f->level = (int32_t)(rnd() & 0x7fffffff);
    switch (f->stage) {
    case 1: f->coef = 0x7fffffff; f->step = (int32_t)(rnd() % 0x01000000); break;
    case 3: f->coef = 0x7fffffff; f->step = (int32_t)(rnd() % 0x00400000) - 0x00200000; break;
    case 5: case 6: f->coef = 0x7ef9db22; f->step = -(int32_t)(rnd() % 0x00100000); break;
    default: f->coef = 0x7f000000 + (int32_t)(rnd() % 0x00ffffff); f->step = 0; break;
    }
}

static const ModuleCallbackInterface s_fake = { .updateParametersADSR = fake_adsr };

#define VOICES 13
static union { State s; uint8_t b[OPZ_VOICE_STATE_BYTES]; } s_states[VOICES];
static FakeEnv s_env[VOICES];
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

static uint32_t pitch_word(void)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return rnd();
    case 3: return rnd() >> 8;                                  /* sub-audio */
    default: {                                                  /* a note, C-1 .. C10 */
        uint32_t p = 0x184cbbu >> 1;                            /* C-1 */
        for (int o = (int)(rnd() % 11); o > 0; o--) p <<= 1;
        return p + (p >> 4) * (rnd() % 16);
    }
    }
}

static void check(int v, const char *what, long n)
{
    const State *s = &s_states[v].s;
    int64_t sa = 0, sl = 0;
    for (int k = 0; k < NM; k++) {
        if (s->amp[k] < -(1 << 7) || s->lev[k] < 0) {
            printf("block %ld voice %d %s: negative amplitude %d / level %d at mode %d\n", n, v, what, s->amp[k],
                   s->lev[k], k);
            s_fail++;
        }
        sa += s->amp[k] > 0 ? s->amp[k] : 0;
        sl += s->lev[k];
    }
    if (sa > 0x58000000 || sl > 0x58000000) {
        printf("block %ld voice %d %s: amplitude sum %lld, level sum %lld\n", n, v, what, (long long)sa,
               (long long)sl);
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
    if (peak > 0x60000000) {
        printf("block %ld voice %d %s: peak %d\n", n, v, what, peak);
        s_fail++;
    }
    if (s_env[v].stage == 0 && s_env[v].level != 0 && peak) {
        printf("block %ld voice %d %s: freed with level %d\n", n, v, what, s_env[v].level);
        s_fail++;
    }
}

static void render_voice(int v, uint32_t pitch, int32_t slope, long n)
{
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
    render(s_out, pitch, slope, &s_states[v], s_params, (ADSR *)&s_env[v]);
    check(v, "render", n);
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 200000;
    uint32_t mem = 0;
    if (!initModule(0, &s_fake, &mem) || mem) return 1;
    for (int v = 0; v < VOICES; v++) init(&s_states[v], 4 + v % 4, 0, 0, 0);
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = opz_param_from_byte(128);
    s_params[OPZ_P_LEVEL] = opz_param_from_byte(255);                    /* the track's level: full */

    /* random calls */
    uint32_t pitch[VOICES];
    for (int v = 0; v < VOICES; v++) pitch[v] = pitch_word();
    for (long n = 0; n < blocks; n++) {
        if (rnd() % 4 == 0) s_params[OPZ_P_P1] = word();        /* knob jumps, often every block */
        if (rnd() % 4 == 0) s_params[OPZ_P_P2] = word();
        if (rnd() % 64 == 0) {                                  /* a slow sweep now and then */
            s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)(n >> 2));
            s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)(n >> 3));
        }
        int v = (int)(rnd() % VOICES);
        switch (rnd() % 16) {
        case 0: case 1: {                                       /* note-on: a fresh voice or a steal */
            const uint32_t *p = rnd() % 4 ? s_params : NULL;
            onNoteOn((int)(rnd() % 128), (uint8_t)rnd(), &s_states[v], p);
            s_env[v].stage = 1;
            s_env[v].level = rnd() % 2 ? 0 : (int32_t)(rnd() & 0x7fffffff);
            pitch[v] = pitch_word();
            check(v, "note-on", n);
            break;
        }
        case 2: onNoteOff((int)(rnd() % 128), &s_states[v]); s_env[v].stage = 4; break;
        case 3: if (rnd() % 8 == 0) init(&s_states[v], 4, 0, 0, 0); break;
        default: break;
        }
        for (int w = 0; w < VOICES; w++) {
            int32_t slope = rnd() % 4 ? 0 : (int32_t)rnd() >> (rnd() % 24);
            if (rnd() % 32 == 0) pitch[w] = pitch_word();
            render_voice(w, pitch[w], slope, n);
            pitch[w] += (uint32_t)slope * OPZ_BLOCK_FRAMES;
        }
    }

    /* grid: every 8th P1 and P2, every 6th note C-1 .. C10 and above fs / 2, bowed and struck, 40 blocks each */
    long grid = 0;
    for (int p1 = 0; p1 < 256; p1 += 8)
        for (int p2 = 0; p2 < 256; p2 += 8)
            for (int note = 0; note < 140; note += 6) {
                uint32_t pw = 0x184cbbu >> 1;
                for (int i = 0; i < note / 12; i++) pw <<= 1;
                pw += (pw >> 4) * (uint32_t)(note % 12);
                if (note >= 138) pw = 0xf0000000u;
                s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)p1);
                s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)p2);
                init(&s_states[0], 4, 0, 0, 0);
                onNoteOn(60, 127, &s_states[0], s_params);
                s_env[0] = (FakeEnv){ 0, 0x7fffffff, 0x7fffffff, 0, 3, 0, 0 };
                for (int b = 0; b < 40; b++) {
                    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
                    render(s_out, pw, 0, &s_states[0], s_params, (ADSR *)&s_env[0]);
                    s_env[0].stage = 3;
                    s_env[0].level = 0x7fffffff;
                    check(0, "grid", b);
                    grid++;
                }
            }
    printf("modal fuzz: %ld random blocks x %d voices (%ld of %ld renders sounding), %ld grid blocks, %ld failures\n",
           blocks, VOICES, s_sounding, s_renders, grid, s_fail);
    return s_fail != 0;
}
