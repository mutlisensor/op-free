/*
 * fuzz.c — hats' random-input check on the host, under UBSan (with array bounds):
 *
 *   clang -std=c11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o build/hats_fuzz engines/hats/tests/fuzz.c && build/hats_fuzz [blocks]
 *
 * Eight voices on the four drum tracks (two each, as on the OP-Z) get random note-ons (any int note, including
 * INT_MIN and INT_MAX, velocity 0 .. 255, params NULL or not), note-offs, re-inits, host fades (stage 5/6, a steal)
 * and host stage jumps, with P1/P2 words jumping every block (including above the host's range) and slow sweeps.
 * Checked after every call: no sanitizer report (signed overflow, invalid shift, out-of-bounds index), the voice's
 * contribution stays at or below OPZ_VOICE_PEAK, every square's next edge lies less than one half period after
 * the block start (so every difference-buffer index is inside s_d), half periods stay at or above two samples, the
 * squares' running sum stays within +-6 AMP, and a voice the engine frees has stopped sounding. A second pass
 * plays every key at the knob corners (and words above the host's range) and octaves -2 .. +2 until it frees its voice, which must happen within
 * 16 s (no hit rings forever), and a third holds every key under a fast knob sweep. A last pass renders torn
 * states (every mix of the words before and after a note-on, and random garbage) until they free their voice.
 * MIT licence, see CONTRIBUTING.md.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "../hats.c"

static uint32_t rng = 0x9e3779b9u;
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

static const ModuleCallbackInterface s_fake = { 0 };

#define VOICES 8
static union { State s; uint8_t b[OPZ_VOICE_STATE_BYTES]; } s_states[VOICES];
static FakeEnv s_env[VOICES];
static uint32_t s_params[OPZ_NUM_PARAMS];
static fract32 s_out[2 * OPZ_BLOCK_FRAMES];
static long s_fail, s_sounding, s_renders, s_notes, s_frees;

static uint32_t word(void)
{
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return 0x7fffffffu;
    case 2: return rnd();                                       /* above the host's range */
    case 3: return 0xffffffffu;
    default: return opz_param_from_byte((uint8_t)rnd());
    }
}

static int note_word(void)
{
    switch (rnd() % 16) {
    case 0: return INT_MIN;
    case 1: return INT_MAX;
    case 2: return (int)rnd();
    case 3: return (int)(rnd() % 400) - 200;
    default: return (int)(rnd() % 128);
    }
}

static int32_t peak_of(void)
{
    int32_t peak = 0;
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
        int32_t a = s_out[i] < 0 ? -s_out[i] : s_out[i];
        if (a > peak) peak = a;
    }
    return peak;
}

static void check(int v, const char *what, long n, int32_t peak)
{
    const State *s = &s_states[v].s;
    if (!s->active) return;                                     /* a freed or never-started voice: not used */
    for (int i = 0; i < 6; i++) {
        if (s->hp[i] < (2 << 16) || s->t[i] < 0 || s->t[i] >= s->hp[i]) {
            printf("block %ld voice %d %s: square %d edge %d half period %d\n", n, v, what, i, s->t[i], s->hp[i]);
            s_fail++;
        }
    }
    if (s->y > 6 * AMP || s->y < -6 * AMP) {
        printf("block %ld voice %d %s: running sum %d\n", n, v, what, s->y);
        s_fail++;
    }
    if (s->model >= 24 || s->stage > ST_CUT) {
        printf("block %ld voice %d %s: model %d stage %d\n", n, v, what, s->model, s->stage);
        s_fail++;
    }
    if (peak > OPZ_VOICE_PEAK) {
        printf("block %ld voice %d %s: peak %d\n", n, v, what, peak);
        s_fail++;
    }
}

static int render_voice(int v, long n)
{
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
    int was = s_env[v].stage;
    render(s_out, 2048, 0, &s_states[v], s_params, (ADSR *)&s_env[v]);
    int32_t peak = peak_of();
    if (was) {
        s_renders++;
        if (peak) s_sounding++;
    }
    if (was && s_env[v].stage == 0) {
        s_frees++;
        if (peak > 1 << 23) {                                   /* the last block: below -48 dBFS (a steal's fade) */
            printf("block %ld voice %d: freed while at %d\n", n, v, peak);
            s_fail++;
        }
    }
    if (!was && peak) {
        printf("block %ld voice %d: sound from an idle voice\n", n, v);
        s_fail++;
    }
    check(v, "render", n, peak);
    return s_env[v].stage != 0;
}

static void start(int v, int note, uint8_t vel, const uint32_t *p)
{
    onNoteOn(note, vel, &s_states[v], p);
    s_env[v].stage = 1;                                         /* the host's note-on: attack (drums: AHD) */
    s_notes++;
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 1000000;
    uint32_t mem = 0;
    if (!initModule(0, &s_fake, &mem) || mem) return 1;
    for (int v = 0; v < VOICES; v++) init(&s_states[v], (uint32_t)(v / 2), 0, 0, 0);
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = opz_param_from_byte(128);

    for (long n = 0; n < blocks; n++) {
        if (rnd() % 4 == 0) s_params[OPZ_P_P1] = word();        /* knob jumps, often every block */
        if (rnd() % 4 == 0) s_params[OPZ_P_P2] = word();
        if (rnd() % 64 == 0) {                                  /* a slow sweep now and then */
            s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)(n >> 2));
            s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)(n >> 3));
        }
        int hits = rnd() % 4 == 0 ? (int)(rnd() % 9) : 0;      /* sometimes many hits in one block */
        for (int h = 0; h < hits; h++) {
            int v = (int)(rnd() % VOICES);
            start(v, note_word(), (uint8_t)rnd(), rnd() % 4 ? s_params : NULL);
            check(v, "note-on", n, 0);
        }
        int v = (int)(rnd() % VOICES);
        switch (rnd() % 32) {
        case 0: onNoteOff(note_word(), &s_states[v]); break;
        case 1: if (rnd() % 8 == 0) init(&s_states[v], rnd() % 32, 0, 0, 0); break;
        case 2: if (s_env[v].stage) s_env[v].stage = 5 + (int32_t)(rnd() % 2); break;   /* a steal */
        case 3: if (s_env[v].stage) s_env[v].stage = 1 + (int32_t)(rnd() % 7); break;   /* any host stage */
        case 4: s_env[v].stage = 0; break;                     /* the host frees it (killNote) */
        default: break;
        }
        for (int w = 0; w < VOICES; w++) render_voice(w, n);
    }

    /* every key at the knob corners and every octave, rendered until the hit frees its voice */
    long grid = 0, longest = 0;
    for (int key = 0; key < 24; key++)
        for (int c = 0; c < 10; c++)
            for (int oct = -2; oct <= 2; oct++) {
                static const uint8_t k[3] = { 0, 128, 255 };
                s_params[OPZ_P_P1] = c < 9 ? opz_param_from_byte(k[c / 3]) : 0xffffffffu;
                s_params[OPZ_P_P2] = c < 9 ? opz_param_from_byte(k[c % 3]) : 0xffffffffu;
                init(&s_states[0], 2, 0, 0, 0);
                s_env[0] = (FakeEnv){ 0 };
                start(0, 53 + key + 24 * oct, 127, s_params);
                long b = 0;
                while (render_voice(0, b)) {
                    if (++b > 16 * 44100 / OPZ_BLOCK_FRAMES) {
                        printf("key %d corner %d octave %d: still sounding after 16 s\n", key + 1, c, oct);
                        s_fail++;
                        break;
                    }
                }
                if (b > longest) longest = b;
                grid += b;
            }

    /* every key held under a fast knob sweep (the knobs are read at the hit: nothing may break mid-hit) */
    for (int key = 0; key < 24; key++) {
        init(&s_states[1], 3, 0, 0, 0);
        s_env[1] = (FakeEnv){ 0 };
        start(1, 53 + key, 100, s_params);
        for (int b = 0; b < 400; b++) {
            s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)(b * 37));
            s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)(255 - b * 23));
            if (b % 5 == 0) start(1, 53 + (key + b) % 24, (uint8_t)(b * 3), b % 2 ? s_params : NULL);
            render_voice(1, b);
        }
    }

    /* torn states: render on every mix of the state before and after a note-on (a first hit on a cleared voice and
     * re-triggers of a sounding one), as tools/opz_crashcheck.py builds them, then on fully random states. Each
     * must return (a hang shows as this test never finishing), stay at or below OPZ_VOICE_PEAK, raise no sanitizer
     * report and free the voice within MAX_AGE + 1 blocks. */
    long torn = 0;
    for (int trial = 0; trial < 240; trial++) {
        enum { W = sizeof(State) / 4 };
        uint32_t before[W], after[W], *st = (uint32_t *)&s_states[2];
        init(&s_states[2], rnd() % 4, 0, 0, 0);
        s_params[OPZ_P_P1] = word();
        s_params[OPZ_P_P2] = word();
        if (trial % 2) {                                        /* a re-trigger: the voice is sounding */
            start(2, note_word(), (uint8_t)rnd(), s_params);
            for (int b = (int)(rnd() % 20); b > 0; b--) render_voice(2, b);
        }
        for (int i = 0; i < W; i++) before[i] = st[i];
        start(2, note_word(), (uint8_t)(rnd() | 1), rnd() % 4 ? s_params : NULL);
        for (int i = 0; i < W; i++) after[i] = st[i];
        for (int mode = 0; mode < 5; mode++)
            for (int k = 0; k < W; k++) {
                for (int i = 0; i < W; i++) {
                    int use_after = mode == 0 ? i <= k : mode == 1 ? i >= k : mode == 2 ? i != k : mode == 3 ? i == k
                                  : (int)(rnd() & 1);
                    st[i] = use_after ? after[i] : before[i];
                    if (mode == 4 && trial % 8 == 0) st[i] = rnd();     /* garbage */
                }
                s_env[2] = (FakeEnv){ 0 };
                s_env[2].stage = 1;
                long b = 0;
                while (s_env[2].stage) {
                    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
                    render(s_out, 2048, 0, &s_states[2], s_params, (ADSR *)&s_env[2]);
                    if (peak_of() > OPZ_VOICE_PEAK) {
                        printf("torn trial %d mode %d word %d: peak %d\n", trial, mode, k, peak_of());
                        s_fail++;
                    }
                    if (++b > (long)MAX_AGE + 1) {
                        printf("torn trial %d mode %d word %d: not freed after %ld blocks\n", trial, mode, k, b);
                        s_fail++;
                        break;
                    }
                    torn++;
                }
            }
    }

    printf("hats fuzz: %ld random blocks x %d voices, %ld notes, %ld frees (%ld of %ld renders sounding), "
           "%ld grid blocks (longest hit %.1f s), %ld torn-state blocks, %ld failures\n",
           blocks, VOICES, s_notes, s_frees, s_sounding, s_renders, grid,
           longest * OPZ_BLOCK_FRAMES / 44100.0, torn, s_fail);
    return s_fail != 0;
}
