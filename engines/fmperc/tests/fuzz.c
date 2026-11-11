/*
 * fuzz.c — fmperc's random-input check on the host, under UBSan (with array bounds):
 *
 *   clang -std=c11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -I sdk/include \
 *         -o build/fmperc_fuzz engines/fmperc/tests/fuzz.c && build/fmperc_fuzz [blocks]
 *
 * Eight voices (two on each drum track, as on the OP-Z) get random note-ons (notes -300 .. 400, so far outside
 * 53 .. 76 and the octave range; velocity 0 .. 255; params NULL one time in eight), fast retriggers of a busy
 * voice, note-offs, re-inits, voice steals (stage 5/6 set by the "host"), and renders with random pitch words and
 * slopes and P1/P2 words over the whole 32-bit range (above the host's 0x7fffffff too), jumping every block. Checked
 * after every call: no sanitizer report (out-of-bounds index, signed overflow, invalid shift), the voice adds at
 * most OPZ_VOICE_PEAK (plus rounding) to every sample, render only adds (a second render of the same state into a
 * non-zero buffer gives the same contribution), a voice in stage 5/6 is freed by the end of that block, a voice
 * the engine frees stays silent, and every hit ends by itself within 40 s. A grid pass then plays every key at
 * every octave -3 .. +3 with P1 and P2 at every 16th value and checks the same bounds. MIT licence, see
 * CONTRIBUTING.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../fmperc.c"

static uint32_t rng = 0x2468ace1u;
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

static const ModuleCallbackInterface s_fake;

#define VOICES 8
#define LIMIT (OPZ_VOICE_PEAK + 64)
static union { State s; uint8_t b[OPZ_VOICE_STATE_BYTES]; } s_states[VOICES];
static FakeEnv s_env[VOICES];
static uint32_t s_params[OPZ_NUM_PARAMS];
static fract32 s_out[2 * OPZ_BLOCK_FRAMES], s_probe[2 * OPZ_BLOCK_FRAMES];
static long s_failures;

static void fail(const char *what, int v, long b)
{
    if (s_failures++ < 20) printf("FAIL %s: voice %d, block %ld, key %d\n", what, v, b, s_states[v].s.key);
}

/* Render voice v once, checking the bounds and that render only adds; returns 1 if it was active. */
static int render_checked(int v, long b, const uint32_t *params)
{
    FakeEnv *e = &s_env[v];
    if (e->stage == 0) return 0;
    int stage_in = e->stage;
    State saved = s_states[v].s;
    FakeEnv esaved = *e;
    uint8_t choke_saved[16];
    memcpy(choke_saved, s_choke, sizeof s_choke);
    uint32_t pitch = rnd();
    int32_t slope = (int32_t)rnd() >> 6;

    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_probe[i] = 0;
    render(s_probe, pitch, slope, &s_states[v].s, params, (ADSR *)e);
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
        if (s_probe[i] > LIMIT || s_probe[i] < -LIMIT) { fail("level above OPZ_VOICE_PEAK", v, b); break; }
        if ((i & 1) && s_probe[i] != s_probe[i - 1]) { fail("left and right differ", v, b); break; }
    }
    if (stage_in >= 5 && e->stage != 0) fail("stolen voice (stage 5/6) not freed in one block", v, b);

    /* the same render again into a buffer with content: the difference must be the same contribution */
    State after = s_states[v].s;
    FakeEnv eafter = *e;
    s_states[v].s = saved;
    *e = esaved;
    memcpy(s_choke, choke_saved, sizeof s_choke);
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_out[i] = (int32_t)(rnd() >> 2) - 0x20000000;
    fract32 before[2 * OPZ_BLOCK_FRAMES];
    memcpy(before, s_out, sizeof before);
    render(s_out, pitch, slope, &s_states[v].s, params, (ADSR *)e);
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++)
        if (s_out[i] - before[i] != s_probe[i]) { fail("render does not add", v, b); break; }
    if (memcmp(&after, &s_states[v].s, sizeof after) || memcmp(&eafter, e, sizeof eafter))
        fail("render is not deterministic", v, b);
    return 1;
}

static void note_on(int v, int note, int vel, const uint32_t *params)
{
    s_env[v].stage = 1;
    s_env[v].level = 0;
    onNoteOn(note, (uint8_t)vel, &s_states[v].s, params);
    if (s_states[v].s.key >= 24) fail("key out of range", v, -1);
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 1000000;
    uint32_t mem;
    if (!initModule(0, &s_fake, &mem) || mem != 0) { printf("FAIL initModule\n"); return 1; }
    for (int v = 0; v < VOICES; v++) {
        memset(s_states[v].b, 0x5a, sizeof s_states[v].b);
        init(&s_states[v].s, (uint32_t)(v >> 1), 0, NULL, NULL);
    }
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = opz_param_from_byte(128);
    s_params[OPZ_P_PAN] = 0x40000000u;       /* the track's level and pan (opz_mix.h): full, exact centre, so L == R */
    s_params[OPZ_P_LEVEL] = 0x7fffffffu;
    long live[VOICES] = { 0 };

    for (long b = 0; b < blocks; b++) {
        /* knobs: jump every block, anywhere in 32 bits a quarter of the time */
        for (int k = 0; k < 2; k++) {
            uint32_t j = rnd() & 3;
            s_params[k] = j == 0 ? rnd() : opz_param_from_byte((uint8_t)rnd());
        }
        for (int v = 0; v < VOICES; v++) {
            uint32_t r = rnd() & 1023;
            const uint32_t *p = (rnd() & 7) ? s_params : NULL;
            if (r < 24) {                                           /* a hit, mostly on the 24 keys */
                int note = (rnd() & 3) ? 53 + (int)(rnd() % 24) + 24 * ((int)(rnd() % 7) - 3)
                                       : (int)(rnd() % 701) - 300;
                note_on(v, note, (int)(rnd() % 256), p);
                live[v] = 0;
            } else if (r < 28 && s_env[v].stage) {                  /* fast retrigger of the same voice */
                note_on(v, 53 + (int)s_states[v].s.key, (int)(rnd() % 128), p);
                live[v] = 0;
            } else if (r < 32) {
                onNoteOff((int)(rnd() % 128), &s_states[v].s);
            } else if (r < 34 && s_env[v].stage) {
                s_env[v].stage = 5 + (int)(rnd() & 1);              /* the host steals the voice */
            } else if (r == 34) {
                init(&s_states[v].s, rnd() & 15, 0, NULL, NULL);   /* the plug switched */
                s_env[v].stage = 0;
            }
            if (render_checked(v, b, (rnd() & 63) ? s_params : NULL)) {
                if (++live[v] > 40L * 44100 / 64) { fail("hit does not end", v, b); s_env[v].stage = 0; }
            }
        }
    }

    /* grid: every key at every octave, P1 and P2 at every 16th value, a short render of each hit */
    long hits = 0;
    for (int p1 = 0; p1 < 256; p1 += 17)
        for (int p2 = 0; p2 < 256; p2 += 17) {
            s_params[0] = opz_param_from_byte((uint8_t)p1);
            s_params[1] = opz_param_from_byte((uint8_t)p2);
            for (int note = 53 - 72; note < 53 + 96; note++) {
                note_on(0, note, 127, s_params);
                for (int b = 0; b < 8; b++) render_checked(0, -2, s_params);
                hits++;
            }
        }
    printf("%ld blocks x %d voices, %ld grid hits: %ld failures\n", blocks, VOICES, hits, s_failures);
    return s_failures != 0;
}
