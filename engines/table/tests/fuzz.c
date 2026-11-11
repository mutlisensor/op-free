/* Host fuzz of table under UndefinedBehaviorSanitizer, with table.c's TABLE_CHECK bounds checks on every grid
 * corner and scratch-line read (AddressSanitizer would do too, where it runs): eight voices with opzsim's envelope,
 * random note-ons (some with params = NULL), note-offs and steals (the host's stage-5 fade, then the new note),
 * knobs that jump, sweep or sit at 0 / 255 every block, pitch words from 0 to 0xffffffff with random slopes, and
 * velocities 0 .. 255. Fails on any out-of-bounds read or undefined behaviour, on a voice that
 * adds more than -6 dBFS into a block, or on a voice whose envelope never lets it go.
 *
 *   clang -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -I sdk/include -I sim/src \
 *       -o /tmp/table_fuzz engines/table/tests/fuzz.c sim/src/callbacks.c && /tmp/table_fuzz [blocks [seed]]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static long s_checks;
#define TABLE_CHECK(cond) ((cond) ? (void)s_checks++ : (printf("check failed: %s (%s:%d)\n", #cond, __FILE__, \
                                                                 __LINE__), exit(1)))
/* the engine's plain export names clash with the sanitizer runtime on macOS: rename them for this build */
#define initModule table_initModule
#define init table_init
#define onNoteOn table_onNoteOn
#define onNoteOff table_onNoteOff
#define render table_render
#include "../table.c"
#include "host.h"

#define VOICES 8

static uint32_t s_rng = 0x1234567u;          /* argv[2] reseeds */
static uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static int chance(int percent) { return (int)(rnd32() % 100) < percent; }

static uint32_t note_pitch(int note)
{
    /* 2^32 * 440 * 2^((note - 69) / 12) / 44100, by octaves from C0 (0x184CBB) */
    static const uint32_t c0_semi[12] = { 0x184CBB, 0x19BEA3, 0x1B4689, 0x1CE5BD, 0x1E9DA2, 0x206FAE, 0x225D72,
                                          0x246891, 0x2692CC, 0x28DDFC, 0x2B4C16, 0x2DDF2E };
    int o = (note - 12) / 12, k = (note - 12) % 12;
    if (note < 12) return c0_semi[note] >> 1;
    return o < 12 ? c0_semi[k] << o : 0xffffffffu;
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 200000;
    if (argc > 2) s_rng = (uint32_t)strtoul(argv[2], NULL, 0) | 1u;
    ModuleCallbackInterface cb;
    sim_callbacks_init(&cb);
    uint32_t mem = 1;
    if (!initModule(0, &cb, &mem) || mem != 0) { printf("initModule failed\n"); return 1; }

    void *state[VOICES];
    SimADSR env[VOICES];
    uint32_t pitch[VOICES];
    int pending[VOICES];                                     /* note waiting for the fade to end, or -1 */
    long busy_since[VOICES];
    for (int v = 0; v < VOICES; v++) {
        state[v] = malloc(OPZ_VOICE_STATE_BYTES);
        memset(state[v], 0x5a, OPZ_VOICE_STATE_BYTES);
        init(state[v], 4 + (v & 3), 0, NULL, NULL);
        memset(&env[v], 0, sizeof env[v]);
        pending[v] = -1;
        busy_since[v] = -1;
        pitch[v] = note_pitch(60);
    }
    uint8_t target[OPZ_NUM_PARAMS] = { 128, 128, 0, 64, 200, 40, 0, 0, 128, 0, 128, 255, 0, 0, 64, 0, 0, 0 };
    uint32_t params[OPZ_NUM_PARAMS];
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte(target[i]);
    fract32 out[2 * N];
    double worst = 0;
    long notes = 0, steals = 0, renders = 0;

    for (long b = 0; b < blocks; b++) {
        /* knobs: P1/P2 jump, sit at an end or drift; the envelope knobs change now and then */
        for (int k = 0; k < 2; k++) {
            int r = (int)(rnd32() % 100);
            if (r < 10) target[k] = (uint8_t)rnd32();
            else if (r < 15) target[k] = (rnd32() & 1) ? 255 : 0;
            else if (r < 60) target[k] = (uint8_t)(target[k] + (int)(rnd32() % 9) - 4);
        }
        if (chance(2)) target[2 + rnd32() % 4] = (uint8_t)rnd32();
        int jump = chance(5);                                /* sometimes unsmoothed, as a project load would */
        for (int i = 0; i < OPZ_NUM_PARAMS; i++) {
            uint32_t t = opz_param_from_byte(target[i]);
            params[i] = jump ? t : params[i] + (uint32_t)((int32_t)(t - params[i]) >> 6);
        }

        for (int v = 0; v < VOICES; v++) {
            if (chance(3)) {                                 /* note-on: steal a busy voice through the fade */
                int note = (int)(rnd32() % 128);
                if (env[v].stage != 0) {                     /* as opzsim and the device: c 0.992, s -level/256 */
                    if (env[v].stage != 5) {
                        int64_t lvl = env[v].level < 0 ? 0 : env[v].level;
                        env[v].stage = 5;
                        env[v].coef = 0x7f000000;
                        env[v].step = -(int32_t)((lvl * 8388607) >> 31);
                        env[v].release = 0;
                    }
                    pending[v] = note;
                    steals++;
                }
                else pending[v] = note;
            }
            if (pending[v] >= 0 && env[v].stage == 0) {
                int note = pending[v];
                pending[v] = -1;
                sim_env_note_on(&env[v]);
                uint8_t vel = (uint8_t)(chance(10) ? rnd32() : 1 + rnd32() % 127);
                onNoteOn(note, vel, state[v], chance(20) ? NULL : params);
                int r = (int)(rnd32() % 100);
                pitch[v] = r < 85 ? note_pitch(note) : r < 90 ? 0 : r < 95 ? 0xffffffffu : rnd32();
                busy_since[v] = b;
                notes++;
            }
            if (env[v].stage != 0 && env[v].stage != 5 && chance(4)) {
                sim_env_note_off(&env[v]);
                onNoteOff(0, state[v]);
            }
        }

        for (int v = 0; v < VOICES; v++) {
            if (env[v].stage == 0) continue;
            int32_t slope = 0;
            int r = (int)(rnd32() % 100);
            if (r < 10) slope = (int32_t)rnd32() >> (rnd32() % 31);          /* anything */
            else if (r < 40) slope = (int32_t)(pitch[v] >> 12) * ((int)(rnd32() % 33) - 16) >> 6;   /* glides */
            for (int i = 0; i < 2 * N; i++) out[i] = 0;
            render(out, pitch[v], slope, state[v], params, (ADSR *)&env[v]);
            renders++;
            pitch[v] += (uint32_t)slope * N;
            for (int i = 0; i < 2 * N; i++) {
                double a = out[i] < 0 ? -(double)out[i] : out[i];
                if (a > worst) worst = a;
            }
            if (env[v].stage == 0) busy_since[v] = -1;
            else if (busy_since[v] >= 0 && b - busy_since[v] > 200000) {
                printf("voice %d never ends (stage %d)\n", v, env[v].stage);
                return 1;
            }
        }
    }
    double peak_db = 20 * __builtin_log10(worst / 2147483648.0 + 1e-12);
    printf("table fuzz: %ld blocks, %ld renders, %ld notes, %ld steals, %ld bounds checks; loudest sample of one "
           "voice %.1f dBFS\n", blocks, renders, notes, steals, s_checks, peak_db);
    if (peak_db > -6.0) { printf("FAIL: a voice peaked above -6 dBFS\n"); return 1; }
    printf("ok\n");
    return 0;
}
