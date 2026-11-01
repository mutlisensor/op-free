/*
 * opz_voices.h — cap how many voices of one engine sound at once, on every track that uses it together.
 *
 * Why: the OS times all instrument voices against one shared pool of cycles per block (research/cpu-budget.md,
 * cpu-headroom.md §2.2). The OS's own voice counts (2, 2, 2, 2, 2, 3, 2, 4 per track) are fixed, so an engine that
 * costs more than the ~167 cycles/sample/voice target can still fit by never letting more than `cap` of its voices
 * sound. With cap 4, an engine may spend roughly 19/4 times the per-voice target.
 *
 * How: every voice is admitted at note-on; when more than `cap` are sounding, the oldest admitted voice is evicted
 * and handed the host's own fade, the one the OS uses for a voice steal and for a CPU-overload kill (envelope stage
 * 5, coefficient 0x7F000000, step -level/256; cpu-budget.md §2). It is gone in ~10 ms, without a click, and the host
 * frees it. An engine that applies the host envelope fades with no further work; a drum engine with its own
 * envelope must already honour stage 5 (engine-abi.md rule 12).
 *
 * Use (one OpzVoiceCap per engine, static, shared by all voices and tracks):
 *
 *   static OpzVoiceCap s_cap = OPZ_VOICE_CAP_INIT(4);
 *   init:      opz_cap_forget(&s_cap, state);
 *   onNoteOn:  opz_cap_admit(&s_cap, state);                        // first thing, before the rest of the set-up
 *   render:    opz_cap_render(&s_cap, state, env);                  // before cb->updateParametersADSR
 *              ... render as usual ...
 *              if (!OPZ_ADSR_STAGE(env)) opz_cap_forget(&s_cap, state);   // at the end, when the voice has ended
 *
 * Interrupt safety: render (and onNoteOff) run in the audio interrupt, possibly in the middle of onNoteOn (rules 18
 * and 20). Pointers are written with single 32-bit stores, last; render only fades voices that onNoteOn put on the
 * evicted list, so a voice rendered before its own onNoteOn admitted it plays normally. A stale entry (a voice the
 * OS gave to another plug without telling us) stops being rendered, falls behind on the render clock and is the first
 * to be replaced.
 *
 * [inferred] Writing stage 5 from an engine is what the OS itself does in Voice::killNote (0x8012dfae); the next
 * updateParametersADSR call continues that fade and frees the voice at ~-66 dB. Proven in opzsim
 * (sim/tests/fixtures/voicecap_probe.c), not yet on the device.
 */
#ifndef OPZ_VOICES_H
#define OPZ_VOICES_H

#include "opz_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPZ_CAP_MAX    8   /* largest cap; more than 8 of a heavy engine's voices would not fit anyway */
#define OPZ_CAP_STALE  64  /* render calls behind the clock before an admitted voice counts as gone (> 38 voices) */
#define OPZ_ENV_STAGE_FADE 5

typedef struct OpzVoiceCap {
    const void *volatile voice[OPZ_CAP_MAX];   /* admitted voices (state pointers), 0 = free */
    uint32_t born[OPZ_CAP_MAX];                /* admission order */
    volatile uint32_t seen[OPZ_CAP_MAX];       /* render clock at each admitted voice's last render */
    const void *volatile evicted[OPZ_CAP_MAX]; /* recently evicted voices: render fades these */
    uint32_t notes;                            /* admissions so far */
    volatile uint32_t clock;                   /* render calls so far */
    uint8_t cap;                               /* 1..OPZ_CAP_MAX */
    uint8_t next_evicted;
} OpzVoiceCap;

#define OPZ_VOICE_CAP_INIT(n) { { 0 }, { 0 }, { 0 }, { 0 }, 0, 0, \
                                (uint8_t)((n) < 1 ? 1 : (n) > OPZ_CAP_MAX ? OPZ_CAP_MAX : (n)), 0 }

/* The host's fade, as Voice::killNote starts it: stage 5, coefficient 0x7F000000, step -level * 0x7FFFFF / 2^31. */
static inline void opz_env_start_fade(ADSR *env)
{
    int32_t level = OPZ_ADSR_LEVEL(env);
    if (level < 0) level = 0;
    *(volatile int32_t *)((uint8_t *)env + 8) = 0x7F000000;
    *(volatile int32_t *)((uint8_t *)env + 12) = -(int32_t)(((uint64_t)(uint32_t)level * 0x7FFFFFu) >> 31);
    OPZ_ADSR_STAGE(env) = OPZ_ENV_STAGE_FADE;
}

static inline int opz__cap_find(const void *volatile const *list, const void *voice)
{
    for (int i = 0; i < OPZ_CAP_MAX; i++)
        if (list[i] == voice) return i;
    return -1;
}

/* A voice of this engine ended or was handed to another plug: free its place. */
static inline void opz_cap_forget(OpzVoiceCap *c, const void *voice)
{
    int i = opz__cap_find(c->voice, voice);
    if (i >= 0) c->voice[i] = 0;
    i = opz__cap_find(c->evicted, voice);
    if (i >= 0) c->evicted[i] = 0;
}

/* onNoteOn: let this voice sound; if that makes cap + 1, evict the oldest (a stale entry first). Returns the evicted
 * voice's state pointer, or 0. The evicted voice fades at its next render. */
static inline const void *opz_cap_admit(OpzVoiceCap *c, const void *voice)
{
    int cap = c->cap < 1 ? 1 : (c->cap > OPZ_CAP_MAX ? OPZ_CAP_MAX : c->cap);
    int i = opz__cap_find(c->evicted, voice);
    if (i >= 0) c->evicted[i] = 0;                        /* re-triggered after an eviction: sounds again */
    uint32_t born = ++c->notes, clock = c->clock;
    i = opz__cap_find(c->voice, voice);
    if (i >= 0) {                                         /* re-trigger of an admitted voice: now the newest */
        c->born[i] = born;
        c->seen[i] = clock;
        return 0;
    }
    int slot = -1, used = 0, stale = -1, oldest = -1;
    for (int k = 0; k < OPZ_CAP_MAX; k++) {
        if (!c->voice[k]) {
            if (slot < 0) slot = k;
            continue;
        }
        used++;
        if (clock - c->seen[k] > OPZ_CAP_STALE && (stale < 0 || c->seen[k] < c->seen[stale])) stale = k;
        if (oldest < 0 || (int32_t)(c->born[k] - c->born[oldest]) < 0) oldest = k;
    }
    const void *out = 0;
    if (stale >= 0) {                                     /* not rendered for a while: gone, reuse quietly */
        c->voice[stale] = 0;
        used--;
        if (slot < 0) slot = stale;
    }
    if (used >= cap) {
        out = c->voice[oldest];
        c->voice[oldest] = 0;
        c->evicted[c->next_evicted] = out;
        c->next_evicted = (uint8_t)((c->next_evicted + 1) % OPZ_CAP_MAX);
        if (slot < 0) slot = oldest;
    }
    if (slot < 0) slot = 0;                               /* unreachable: cap <= OPZ_CAP_MAX */
    c->born[slot] = born;
    c->seen[slot] = clock;
    c->voice[slot] = voice;                               /* last: render may be looking */
    return out;
}

/* render, before the envelope update: count the call, and start the host fade on an evicted voice (again, if a
 * note-off moved it to release meanwhile). Returns 1 while the voice may sound, 0 once it is fading out. */
static inline int opz_cap_render(OpzVoiceCap *c, const void *voice, ADSR *env)
{
    uint32_t clock = ++c->clock;
    int i = opz__cap_find(c->voice, voice);
    if (i >= 0) {
        c->seen[i] = clock;
        return 1;
    }
    if (opz__cap_find(c->evicted, voice) < 0) return 1;   /* not ours to judge (not admitted yet, or stale) */
    int32_t stage = OPZ_ADSR_STAGE(env);
    if (stage != 0 && stage != OPZ_ENV_STAGE_FADE && stage != OPZ_ENV_STAGE_FADE + 1) opz_env_start_fade(env);
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* OPZ_VOICES_H */
