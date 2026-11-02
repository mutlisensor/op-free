/*
 * fuzz.c — snap's random-input check on the host, under UBSan (with array bounds):
 *
 *   clang -std=c11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o build/snap_fuzz engines/snap/tests/fuzz.c && build/snap_fuzz [blocks]
 *
 * (add address to -fsanitize where AddressSanitizer runs; on this project's macOS it fails at start-up)
 *
 * Eight voices (a drum track's worth and more) get random note-ons (any int note, velocity 0 .. 255, params NULL
 * or words from 0 to 2^32 - 1, knob jumps between hits), retriggers of sounding voices, note-offs, re-inits and
 * renders with a host envelope whose stage the fuzzer moves to fade (5/6) or idle at random. Checked after every
 * call: no sanitizer report (out-of-bounds index, signed overflow, invalid shift), a voice adds at most
 * OPZ_VOICE_PEAK to each output sample, the model index stays in range, render returns (the burst timer always
 * moves on), and every hit ends: with no new notes, each voice frees itself within 4,500 blocks (6.5 s, longer
 * than the longest tail at P2 255) and within 12 blocks once the host asks for a fade. A grid pass then plays
 * every key at every octave with P1 and P2 at every 16th value and checks the same bounds plus the end.
 * MIT licence, see CONTRIBUTING.md.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../snap.c"

static uint32_t rng = 0x9e3779b9u;
static uint32_t rnd32(void)
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
static long s_fail;

static void fail(const char *what, int v, long block)
{
    if (s_fail++ < 20) fprintf(stderr, "FAIL %s: voice %d, block %ld\n", what, v, block);
}

/* One render of voice v into a zeroed buffer, with the bound checks. */
static void render_voice(int v, long block)
{
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = 0;
    render(s_out, (uint32_t)rnd32(), (int32_t)rnd32(), &s_states[v].s, s_params, (ADSR *)&s_env[v]);
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++)
        if (s_out[i] > OPZ_VOICE_PEAK || s_out[i] < -OPZ_VOICE_PEAK) { fail("sample above OPZ_VOICE_PEAK", v, block); break; }
    if (s_states[v].s.model >= 24) fail("model index", v, block);
}

static void note_on(int v, int note, int vel, int null_params)
{
    if (s_env[v].stage == 0) s_env[v].stage = 1;
    onNoteOn(note, (uint8_t)vel, &s_states[v].s, null_params ? NULL : s_params);
}

/* Render voice v until it frees itself; returns the blocks it took (or limit + 1). */
static long run_out(int v, long limit)
{
    long b = 0;
    while (s_env[v].stage != 0 && b <= limit) render_voice(v, b++);
    return b;
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 1000000;
    uint32_t mem = 0;
    initModule(0, &s_fake, &mem);
    for (int v = 0; v < VOICES; v++) init(&s_states[v].s, (uint32_t)v & 3, 0, NULL, NULL);

    /* random pass */
    for (long b = 0; b < blocks; b++) {
        if (rnd32() % 4 == 0)                                         /* knob jumps, sometimes past the host range */
            for (int i = 0; i < OPZ_NUM_PARAMS; i++)
                s_params[i] = rnd32() % 8 ? opz_param_from_byte((uint8_t)rnd32()) : rnd32();
        for (int v = 0; v < VOICES; v++) {
            uint32_t r = rnd32() % 1024;
            if (r < 24) {                                             /* a hit, often on a sounding voice */
                int note = rnd32() % 8 == 0 ? (int)rnd32() : 53 + (int)(rnd32() % 24) + 24 * ((int)(rnd32() % 7) - 3);
                note_on(v, note, (int)(rnd32() % 256), rnd32() % 8 == 0);
            } else if (r < 28) {
                onNoteOff((int)rnd32(), &s_states[v].s);
            } else if (r < 30) {
                init(&s_states[v].s, rnd32() % 16, 0, NULL, NULL);
                s_env[v].stage = 0;
            } else if (r < 34 && s_env[v].stage) {
                s_env[v].stage = 5 + (int32_t)(rnd32() % 2);           /* the host wants the voice back */
            } else if (r < 36) {
                s_env[v].stage = (int32_t)(rnd32() % 8);              /* any stage */
            }
            if (s_env[v].stage) render_voice(v, b);
        }
    }

    /* every hit ends: at the longest settings, and fast when faded */
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = opz_param_from_byte(255);
    for (int k = 0; k < 24; k++) {
        for (int v = 0; v < 2; v++) {
            init(&s_states[v].s, 1, 0, NULL, NULL);
            s_env[v].stage = 0;
            note_on(v, 53 + k, 127, 0);
        }
        if (run_out(0, 4500) > 4500) fail("hit at P1/P2 255 never ends", k, 0);
        render_voice(1, 0);
        s_env[1].stage = 5;
        if (run_out(1, 12) > 12) fail("faded hit takes more than 12 blocks", k, 0);
    }

    /* grid: every key and octave, P1 and P2 at every 16th value */
    long grid = 0;
    for (int p1 = 0; p1 < 256; p1 += 17)
        for (int p2 = 0; p2 < 256; p2 += 17) {
            s_params[OPZ_P_P1] = opz_param_from_byte((uint8_t)p1);
            s_params[OPZ_P_P2] = opz_param_from_byte((uint8_t)p2);
            for (int note = 5; note < 125; note++) {
                init(&s_states[0].s, 0, 0, NULL, NULL);
                s_env[0].stage = 0;
                note_on(0, note, 1 + (note * 37) % 127, 0);
                long n = run_out(0, 4500);
                if (n > 4500) fail("grid hit never ends", note, p1 * 256 + p2);
                grid += n;
            }
        }

    printf("snap fuzz: %ld random blocks x %d voices, %ld grid blocks, %ld failures\n", blocks, VOICES, grid, s_fail);
    return s_fail != 0;
}
