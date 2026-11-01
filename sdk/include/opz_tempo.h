/*
 * opz_tempo.h — tempo and transport for OP-Z engines, from the sequencer keys (sdk/docs/engine-abi.md §5,
 * "Sequencer keys"; Q-18).
 *
 * What the OS offers through sequencerGetValue (app and device code, firmware 1.2.45):
 *   key 4  tempo in whole BPM, 40..200 (the project tempo, or the tempo lock); nudge and MIDI-clock slip are
 *          not included
 *   key 5  1 while the sequencer plays, 0 when stopped
 *   key 6  global step, 0 .. pattern length - 1 (the longest track's step count x step length, up to 256)
 *   key 7  tick within the step, 0..23: 24 ticks per sixteenth step, 96 per beat
 * The sequencer runs between audio blocks (app; inferred on the device, hardware check H-7 with
 * engines/opz_test_tempo). A tick plays position (step, tick) and then advances the counter, so
 * keys 6 and 7 read one tick ahead of the position just played; play starts at (0, 0) and the first block sees
 * (0, 1). The counter keeps running while stopped. Swing and per-track step lengths are not exposed.
 *
 * Two levels of help:
 *   - pure functions of the tempo (samples per beat, step, note value; phase increments) for delay times and
 *     free-running synced LFOs. TE's delay and tape compute their times the same way;
 *   - OpzClock, a block-accurate musical position: snapped to the tick counter when it moves, interpolated from
 *     the tempo in between, never ahead of the next tick and never backwards except on a jump (start, stop,
 *     pattern change, nudge). Keep one in the FX state (or the voice state) and call opz_clock_block once at the
 *     top of every renderFX (render).
 * Positions are steps in Q20 (12.20 on a 4096-step ring): step = pos >> 20, and the phase of a cycle of 2^n steps
 * is pos << (12 - n), so beat and bar phases are shifts. Everything per block is 32-bit; the divisions run only
 * when the tempo changes. MIT licence, see CONTRIBUTING.md.
 */
#ifndef OPZ_TEMPO_H
#define OPZ_TEMPO_H

#include "opz_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPZ_TICKS_PER_STEP  24
#define OPZ_STEPS_PER_BEAT  4
#define OPZ_TICKS_PER_BEAT  96
#define OPZ_BPM_MIN         40
#define OPZ_BPM_MAX         200   /* the OS clamps the tempo to 40..200; TE's delay allows 220 with pitch bend */
#define OPZ_BPM_DEFAULT     120

#define OPZ_STEP_Q20        (1u << 20)   /* one step in OpzClock positions */

/* Tempo from key 4, in whole BPM. Out-of-range answers (an older OS, a host without the key) give 120; the
 * result is clamped to 40..255 so the helpers below never overflow. */
static inline int opz_seq_bpm(const ModuleCallbackInterface *cb)
{
    int bpm = cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_TEMPO, 0));
    if (bpm <= 0 || bpm > 1000) return OPZ_BPM_DEFAULT;
    return bpm < OPZ_BPM_MIN ? OPZ_BPM_MIN : bpm > 255 ? 255 : bpm;
}

/* ---- tempo -> time (frames at 44.1 kHz, truncated like TE's engines) ----------------------------------- */

static inline uint32_t opz_samples_per_beat(int bpm) { return 2646000u / (uint32_t)bpm; }   /* 44100 * 60 / bpm */
static inline uint32_t opz_samples_per_step(int bpm) { return 661500u / (uint32_t)bpm; }    /* a sixteenth */

/* num/den of a whole note (4 beats), e.g. 3/16 for a dotted eighth, 1/12 for an eighth triplet. num <= 400,
 * den <= 255: the formula of TE's delay, num * 44100 * 240 / (den * bpm). */
static inline uint32_t opz_samples_per_note(int bpm, uint32_t num, uint32_t den)
{
    return num * 10584000u / (den * (uint32_t)bpm);
}

/* Per-sample Q32 phase increment of a cycle num/den whole notes long (a free-running synced LFO). num >= 1,
 * den <= 255. One 64-bit division: call it when the tempo changes. */
static inline uint32_t opz_sync_inc(int bpm, uint32_t num, uint32_t den)
{
    return opz_udiv64_32((uint64_t)(den * (uint32_t)bpm) << 32, num * 10584000u);
}

/* Steps per 64-frame block in Q20: 64 * 2^20 * bpm / 661500 = bpm * 2^24 / 165375, rounded (fits for bpm <= 255). */
static inline uint32_t opz_steps_per_block_q20(int bpm)
{
    return ((uint32_t)bpm * 16777216u + 82687u) / 165375u;
}

/* Position of (step, tick) in Q20 steps: a tick is floor(t * 2^20 / 24), so 24 ticks end exactly on the next
 * step (floor(n / 3) = (n * 171) >> 9 is exact for n < 510; no division). step may be -1 (the step before 0 on the
 * ring); tick 0..24. */
static inline uint32_t opz_tempo_pos(int step, int tick)
{
    uint32_t t = (uint32_t)tick;
    return ((uint32_t)step << 20) + t * 43690u + ((t * 342u) >> 9);
}

/* ---- OpzClock -------------------------------------------------------------------------------------------- */

typedef struct OpzClock {
    uint32_t pos;      /* musical position at the start of this block, steps in Q20 */
    uint32_t inc;      /* steps per block in Q20 at the current tempo */
    int16_t  bpm;      /* tempo inc was computed for (0 before the first block) */
    int16_t  step;     /* last key 6 read; -1 before the first block */
    uint8_t  tick;     /* last key 7 read */
    uint8_t  playing;  /* key 5 */
    uint8_t  ticked;   /* 1 on a block where the tick counter moved (the sequencer played a tick since the last block) */
    uint8_t  jumped;   /* 1 on a block where pos was set rather than advanced: the first block, play, stop,
                          a pattern change, a nudge or missed ticks */
} OpzClock;

static inline void opz_clock_init(OpzClock *c)
{
    c->pos = 0;
    c->inc = 0;
    c->bpm = 0;
    c->step = -1;
    c->tick = 0;
    c->playing = 0;
    c->ticked = 0;
    c->jumped = 0;
}

/* Once per block, before using the position. Five sequencerGetValue calls, no division unless the tempo changed. */
static inline void opz_clock_block(OpzClock *c, const ModuleCallbackInterface *cb)
{
    int bpm = opz_seq_bpm(cb);
    if (bpm != c->bpm) {
        c->bpm = (int16_t)bpm;
        c->inc = opz_steps_per_block_q20(bpm);
    }
    c->playing = cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_PLAYING, 0)) > 0;
    int step = cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_STEP, 0));
    int tick = cb->sequencerGetValue(OPZ_SEQ_KEY(OPZ_SEQ_GET_TICK, 0));
    c->ticked = c->jumped = 0;
    if (step < 0 || step > 4095 || tick < 0 || tick >= OPZ_TICKS_PER_STEP) {   /* no transport keys: free-run */
        c->pos += c->inc;
        return;
    }
    if (step != c->step || tick != c->tick) {
        int prev = c->step;
        int carry = prev >= 0 && tick == 0 && c->tick == 23;   /* the previous step ended */
        int wrap = carry && step == 0 && prev != -1;          /* (len - 1, 23) -> (0, 0): the pattern ended */
        c->ticked = 1;
        c->jumped = !((prev >= 0 && step == prev && tick == c->tick + 1) || (carry && step == prev + 1) || wrap);
        c->step = (int16_t)step;
        c->tick = (uint8_t)tick;
        /* the tick just played: (step, tick - 1); at a step change the last tick of the previous step, which at the
         * end of the pattern is the last step, not step - 1 */
        c->pos = tick ? opz_tempo_pos(step, tick - 1) : opz_tempo_pos(wrap ? prev : step - 1, 23);
    } else {
        uint32_t room = opz_tempo_pos(step, tick) - 1u - c->pos;   /* never reach a tick that has not played */
        c->pos += c->inc < room ? c->inc : room;
    }
}

/* Step at the start of the block, 0 .. pattern length - 1 (signed: -1 only after a jump that lands on step 0,
 * tick 0). To detect step changes compare it modulo your cycle (step & 15), not exactly: after a stop the OS moves
 * the step to the pattern's last one or to 0. */
static inline int opz_clock_step(const OpzClock *c) { return (int32_t)c->pos >> 20; }

/* Q32 phase of a cycle 2^log2_steps steps long, 0..12: 0 = a step, 2 = a beat, 4 = a bar of 16 steps. Multiply
 * by k (wrapping) for k cycles per 2^n steps, e.g. beat phase * 3 for eighth-note triplets. Aligned to the
 * pattern start. */
static inline uint32_t opz_clock_phase(const OpzClock *c, int log2_steps) { return c->pos << (12 - log2_steps); }

/* Per-sample Q32 increment of the same cycle at the current tempo (log2_steps 0..6), to ramp within a block. */
static inline uint32_t opz_clock_phase_inc(const OpzClock *c, int log2_steps) { return c->inc << (6 - log2_steps); }

#ifdef __cplusplus
}
#endif

#endif /* OPZ_TEMPO_H */
