/* Host fuzzer for engines/8bit: random hits, notes over the whole int range, velocities 0-255, parameter words
 * (in range, wild, all ones, NULL), steals and chokes on 8 voices over 4 tracks. Fails on a write past the 280-byte
 * state, L != R, a voice above -12 dBFS, a stolen voice still sounding after 4 blocks, or a voice that never ends.
 *
 *   clang -std=c11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -Dinit=eng_init  *         -Drender=eng_render -DonNoteOn=eng_on -I sdk/include -o /tmp/fuzz8 engines/8bit/tests/fuzz.c engines/8bit/8bit.c
 *   /tmp/fuzz8 400000 1      (iterations, seed)
 *
 * (The renames keep the engine's init/render out of the way of the host's symbols.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opz_engine.h"
void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch);
void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params);
void onNoteOff(int note, void *state);
void render(fract32 *out, uint32_t pitch, int32_t pitchSlope, void *state, const uint32_t *params, ADSR *env);
uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest);
#define NV 8
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
typedef struct { uint8_t st[280]; uint8_t guard[64]; } Slot;
int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    rs = argc > 2 ? (uint32_t)atol(argv[2]) : 12345;
    static Slot v[NV];
    static uint8_t envs[NV][32];
    static int age[NV], stolen[NV];
    uint32_t mem = 0; ModuleCallbackInterface cb; memset(&cb, 0, sizeof cb);
    initModule(0, &cb, &mem);
    for (int i = 0; i < NV; i++) { memset(&v[i], 0xa5, sizeof v[i]); init(v[i].st, i & 3, 0, 0, 0); }
    uint32_t params[18];
    long maxlen = 0, hits = 0;
    double peak = 0;
    for (long it = 0; it < iters; it++) {
        uint32_t r = rnd();
        for (int k = 0; k < 18; k++) params[k] = opz_param_from_byte(128);
        switch (r % 4) {             /* knob words: in range, jumping, wild */
        case 0: break;
        case 1: params[0] = opz_param_from_byte(rnd() & 255); params[1] = opz_param_from_byte(rnd() & 255); break;
        case 2: params[0] = rnd(); params[1] = rnd(); break;
        case 3: params[0] = (rnd() & 1) ? 0xffffffffu : 0; params[1] = (rnd() & 1) ? 0xffffffffu : 0; break;
        }
        if ((r >> 4) % 6 == 0) {     /* a hit on a voice */
            int vi = rnd() % NV;
            int note = (rnd() & 3) == 0 ? (int)rnd() : (int)(rnd() % 200) - 36;
            uint8_t vel = rnd() & 255;
            OPZ_ADSR_STAGE(envs[vi]) = 1;
            onNoteOn(note, vel, v[vi].st, (rnd() & 3) == 0 ? NULL : params);
            age[vi] = 0;
            stolen[vi] = 0;
            hits++;
        }
        if ((r >> 8) % 50 == 0) {    /* the host steals a voice */
            int vi = rnd() % NV;
            if (OPZ_ADSR_STAGE(envs[vi])) { OPZ_ADSR_STAGE(envs[vi]) = 5 + (rnd() & 1); stolen[vi] = 1; }
        }
        if ((r >> 12) % 40 == 0) onNoteOff((int)rnd(), v[rnd() % NV].st);
        for (int vi = 0; vi < NV; vi++) {
            if (!OPZ_ADSR_STAGE(envs[vi])) continue;
            fract32 out[128];
            for (int k = 0; k < 128; k++) out[k] = 0;
            render(out, 2048, 0, v[vi].st, (rnd() & 15) == 0 ? NULL : params, (ADSR *)envs[vi]);
            for (int k = 0; k < 128; k++) {
                double a = abs(out[k]) / 2147483648.0;
                if (a > peak) peak = a;
                if (out[k] != out[k ^ 1]) { printf("L != R\n"); return 1; }
            }
            if (++age[vi] > maxlen) maxlen = age[vi];
            int st = OPZ_ADSR_STAGE(envs[vi]);
            if (st && stolen[vi] && ++stolen[vi] > 4) { printf("stolen voice %d still sounding\n", vi); return 1; }
            if (age[vi] > 20000) { printf("voice %d never ends\n", vi); return 1; }
        }
        for (int vi = 0; vi < NV; vi++)
            for (int k = 0; k < 64; k++)
                if (v[vi].guard[k] != 0xa5) { printf("guard overwritten, voice %d\n", vi); return 1; }
    }
    printf("ok: %ld iterations, %ld hits, longest voice %ld blocks, peak %.4f FS\n", iters, hits, maxlen, peak);
    return peak > 0.2501 ? 1 : 0;
}
