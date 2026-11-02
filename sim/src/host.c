/*
 * host.c — the block loop of the host model, shared by opzsim (sim/src/opzsim.c, driven by a scenario) and the
 * web simulator (sim/wasm/websim.c, driven live from the browser). 44.1 kHz, 64-frame blocks, 18 Q31 parameters
 * smoothed per block, Q32 pitch with a per-sample slope, voices per track, instruments adding into a shared track
 * buffer, effects on separate interleaved stereo buffers. sdk/docs/engine-abi.md; no TE code or data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

/* Voices per instrument track (research/architecture/02-sound-engine-host.md, voicePolyphony). */
static const int k_voices[SIM_INSTR_TRACKS] = { 2, 2, 2, 2, 2, 3, 2, 4 };
static const int k_voices_generous_chord = 6;

/* FX memory pools per FX track: FX1, FX2, tape, master (device table at 0x8156f21c). */
static const uint32_t k_fx_pool[4] = { OPZ_FX1_POOL_BYTES, OPZ_FX2_POOL_BYTES, OPZ_TAPE_POOL_BYTES,
                                       OPZ_MASTER_POOL_BYTES };

/* The OS masks interrupts around an effect's init: flag inits that touch much memory or take long. */
#define FX_INIT_MAX_BYTES 4096
#define FX_INIT_MAX_NS    1e6
#define FX_MEM_FILL       0x5a

#define ADD_PROBE 0x01000000

Track g_tracks[SIM_TRACKS];
HostStats g_host;
static uint8_t g_scratch[SIM_SCRATCH_BYTES];
static int g_half_warned, g_fx_unwritten_warned;
static Engine *s_inst, *s_fx;
static uint8_t *s_fx_state;
static int s_check_add;

double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* ---- state budget guard ------------------------------------------------------------------------------ */

static uint8_t *alloc_state(int bytes)
{
    uint8_t *p = malloc((size_t)bytes + SIM_GUARD_BYTES);
    memset(p, 0x5a, (size_t)bytes);                 /* not zero: the host does not clear state */
    for (int i = 0; i < SIM_GUARD_BYTES; i++) p[bytes + i] = (uint8_t)(0xc3 ^ i);
    return p;
}

static void check_guard(uint8_t *p, int bytes, const char *engine, const char *fn)
{
    for (int i = 0; i < SIM_GUARD_BYTES; i++)
        if (p[bytes + i] != (uint8_t)(0xc3 ^ i)) {
            if (!g_host.state_violations++)
                fprintf(stderr, "error   %s: %s wrote past the %d-byte state area (offset %d)\n",
                        engine, fn, bytes, bytes + i);
            for (int k = 0; k < SIM_GUARD_BYTES; k++) p[bytes + k] = (uint8_t)(0xc3 ^ k);
            return;
        }
}

/* ---- pitch ------------------------------------------------------------------------------------------- */

/* Q32 phase increment at 44.1 kHz for a MIDI note (A4 = 69 = 440 Hz) bent by `cents`. */
/* Drum tracks (0-3) get a fixed "unity rate", not a note frequency: Voice::getTargetFrequency uses base 1024 scaled
 * by the master tuning instead of the note table (app code; proven on hardware: a sine on the kick track is near-DC).
 * The key only reaches the engine as onNoteOn's note number. 1024 at standard tuning: the tuning factor is Q30
 * (`x tune >> 30`, app 0x70fc0, device FUN_8012e058), 1.0 for synth notes whose table is exact Q32 (A4 = 440 Hz),
 * and TE's sampleplayergain plays at original speed with step 65536 x pitch 1024 >> 16 = 1 frame (samples.md §8). */
#define DRUM_UNITY_PITCH 1024u

static uint32_t note_pitch(int note, int cents)
{
    double f = 440.0 * sim_exp2((note - 69) / 12.0 + cents / 1200.0);
    double inc = f * 4294967296.0 / OPZ_SAMPLE_RATE;
    if (inc >= 4294967295.0) inc = 4294967295.0;
    return (uint32_t)(inc + 0.5);
}

static uint32_t track_pitch(const Track *tr, int note, int cents)
{
    return (tr - g_tracks) < 4 ? DRUM_UNITY_PITCH : note_pitch(note, cents);
}

/* ---- inputs for effects ------------------------------------------------------------------------------ */

static fract32 q31(double v)
{
    double s = v * 2147483647.0;
    return s >= 2147483647.0 ? SIM_FULL : (fract32)(int64_t)(s + (s >= 0 ? 0.5 : -0.5));
}

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

void host_input_init(const Scenario *sc, HostInput *in)
{
    memset(in, 0, sizeof *in);
    in->rng_l = 0x12345678u;
    in->rng_r = 0x9e3779b9u;
    in->amp = q31(sc->in_amp);
    in->inc = (uint32_t)(sc->in_f0 * 4294967296.0 / OPZ_SAMPLE_RATE + 0.5);
}

static void gen_input(const Scenario *sc, HostInput *in, int64_t t0, fract32 *l, fract32 *r)
{
    double active = sc->steps * sc->step_samples;
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        int64_t t = t0 + i;
        fract32 a = 0, b = 0;
        switch (sc->input) {
        case IN_SINE:
        case IN_SWEEP:
            if (sc->input == IN_SWEEP) {
                double f = sc->in_f0 + (sc->in_f1 - sc->in_f0) * (t < active ? t / active : 1.0);
                in->inc = (uint32_t)(f * 4294967296.0 / OPZ_SAMPLE_RATE + 0.5);
            }
            a = opz_mul_q31(in->amp, q31(sim_sin(in->phase * (6.283185307179586 / 4294967296.0))));
            b = opz_mul_q31(in->amp, q31(sim_sin((uint32_t)(in->phase + 0x40000000u) * (6.283185307179586 / 4294967296.0))));
            in->phase += in->inc;
            break;
        case IN_SAW:
            a = opz_mul_q31(in->amp, (fract32)in->phase);
            b = opz_mul_q31(in->amp, (fract32)(in->phase + 0x40000000u));
            in->phase += in->inc;
            break;
        case IN_NOISE:
            a = opz_mul_q31(in->amp, (fract32)xorshift(&in->rng_l));
            b = opz_mul_q31(in->amp, (fract32)xorshift(&in->rng_r));
            break;
        case IN_IMPULSE: {
            int64_t every = (int64_t)(sc->in_f0 * sc->step_samples + 0.5);
            if (every > 0 && t % every == 0) a = b = in->amp;
            break;
        }
        default:
            break;
        }
        l[i] = a;
        r[i] = b;
    }
}

/* ---- voices ------------------------------------------------------------------------------------------ */

static void start_note(const Engine *e, Track *tr, Voice *v, int note, int id, int vel, int64_t time)
{
    v->active = 1;
    v->note = note;
    v->note_id = id;
    v->started = time;
    v->fresh = 1;
    v->pending = 0;
    v->pitch_prev = track_pitch(tr, note, tr->bend_cents);
    sim_env_note_on(&v->env);
    e->onNoteOn(note, (uint8_t)vel, v->state, tr->current);
    check_guard(v->state, OPZ_VOICE_STATE_BYTES, e->name, "onNoteOn");
}

static void start_pending(const Engine *e, Track *tr, Voice *v)
{
    start_note(e, tr, v, v->pending_note, v->pending_id, v->pending_vel, v->started);
}

static void render_voice(const Engine *e, Track *tr, Voice *v, int check_add, double *ns)
{
    const int total = OPZ_VOICE_STATE_BYTES + SIM_GUARD_BYTES;
    uint32_t pitch_end = track_pitch(tr, v->note, tr->bend_cents);
    int32_t slope = (int32_t)(pitch_end - v->pitch_prev) >> 6;

    if (check_add) {
        static uint8_t save[OPZ_VOICE_STATE_BYTES + SIM_GUARD_BYTES];
        fract32 probe[2 * OPZ_BLOCK_FRAMES], before[2 * OPZ_BLOCK_FRAMES];
        SimADSR env = v->env;
        memcpy(save, v->state, (size_t)total);
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) probe[i] = ADD_PROBE;
        e->render(probe, v->pitch_prev, slope, v->state, tr->current, (ADSR *)&v->env);
        memcpy(v->state, save, (size_t)total);
        v->env = env;
        memcpy(before, tr->buf, sizeof before);
        double t0 = now_ns();
        e->render(tr->buf, v->pitch_prev, slope, v->state, tr->current, (ADSR *)&v->env);
        *ns += now_ns() - t0;
        int first = 0, second = 0;
        int64_t peak = 0;
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
            int64_t d = (int64_t)probe[i] - ADD_PROBE;
            if (d) { if (i < OPZ_BLOCK_FRAMES) first++; else second++; }
            if (i < OPZ_BLOCK_FRAMES && (d < 0 ? -d : d) > peak) peak = d < 0 ? -d : d;
        }
        /* an audible voice (> -54 dBFS) that leaves the whole second half untouched wrote mono */
        if (v->fresh && first > 48 && second == 0 && peak > (1 << 22) && !g_half_warned) {
            g_half_warned = 1;
            fprintf(stderr, "warning %s: render wrote only the first %d values of out; out is 64 frames of "
                    "interleaved stereo (128 values): write out[2i] and out[2i+1]\n", e->name, OPZ_BLOCK_FRAMES);
        }
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
            if (tr->buf[i] == INT32_MAX || tr->buf[i] == INT32_MIN || probe[i] == INT32_MAX || probe[i] == INT32_MIN)
                continue;
            if ((int64_t)tr->buf[i] - before[i] != (int64_t)probe[i] - ADD_PROBE) {
                if (!g_host.add_violations++)
                    fprintf(stderr, "error   %s: render does not add into out (frame %d: buffer had %d, probe "
                            "showed a different contribution); never overwrite the track buffer\n",
                            e->name, i, before[i]);
                break;
            }
        }
    } else {
        double t0 = now_ns();
        e->render(tr->buf, v->pitch_prev, slope, v->state, tr->current, (ADSR *)&v->env);
        *ns += now_ns() - t0;
    }
    v->fresh = 0;
    check_guard(v->state, OPZ_VOICE_STATE_BYTES, e->name, "render");
    v->pitch_prev = pitch_end;
    if (v->env.stage == ENV_IDLE) {             /* Voice::renderVoice frees the voice, or starts the waiting note */
        if (v->pending) start_pending(e, tr, v);
        else v->active = 0;
    }
}

#define FX_OUT_SENTINEL 0x5a5a5a5au

static Voice *alloc_voice(Track *tr)
{
    Voice *best = NULL;
    for (int i = 0; i < tr->nvoices; i++)
        if (!tr->voices[i].active) return &tr->voices[i];
    for (int i = 0; i < tr->nvoices; i++) /* oldest releasing voice */
        if (!tr->voices[i].env.gate && (!best || tr->voices[i].started < best->started)) best = &tr->voices[i];
    if (!best)
        for (int i = 0; i < tr->nvoices; i++)
            if (!best || tr->voices[i].started < best->started) best = &tr->voices[i];
    g_host.voices_stolen++;
    return best;
}

/* ---- setup ------------------------------------------------------------------------------------------- */

int host_setup(Engine *inst, Engine *fx, const Scenario *sc, int generous, int check_add)
{
    s_inst = inst;
    s_fx = fx;
    s_check_add = check_add;
    g_host.unchanged = 1;

    /* tracks, voices, FX record */
    for (int t = 0; t < SIM_TRACKS; t++) {
        Track *tr = &g_tracks[t];
        for (int i = 0; i < OPZ_NUM_PARAMS; i++) {
            tr->bytes[i] = sc->init_params[t][i];
            tr->target[i] = tr->current[i] = opz_param_from_byte(tr->bytes[i]);
        }
        if (t < SIM_INSTR_TRACKS && inst && sc->track_used[t]) {
            tr->nvoices = (t == OPZ_TRACK_CHORD && generous) ? k_voices_generous_chord : k_voices[t];
            for (int v = 0; v < tr->nvoices; v++) tr->voices[v].state = alloc_state(OPZ_VOICE_STATE_BYTES);
            if (inst->mem_request) {
                static int warned;
                if (!warned++)
                    fprintf(stderr, "warning %s: instrument requests %u B, but instruments get no private large "
                            "buffer on the device; request ignored (use a static voice pool)\n", inst->name,
                            inst->mem_request);
            }
            /* init's arg2 and mem: the plug's sample-pack id, crc32 of its samples path (0 for synth plugs) */
            if (sc->sample_builtin[t] == SAMPLE_UNLOADED) {     /* the plug names a pack the loader never loaded */
                tr->pack = sim_crc32(sc->samples_path[t]);
                fprintf(stderr, "sample  %s -> pack 0x%08x, not loaded (the OS drops its notes)\n",
                        sc->samples_path[t], tr->pack);
            } else if (sc->sample_builtin[t] || sc->sample_file[t][0]) {
                if (sim_pack_attach(sc->sample_builtin[t] ? NULL : sc->sample_file[t], sc->samples_path[t], &tr->pack))
                    return 1;
            }
        }
    }
    if (fx) {
        uint32_t pool = k_fx_pool[sc->fx_track - 8], req = fx->mem_request;
        if (req > pool) {
            fprintf(stderr, "warning %s: requests %u B, more than the %s pool of %u B; capped\n", fx->name, req,
                    track_name(sc->fx_track), pool);
            req = pool;
        }
        /* the pool is not cleared between plugs: start from a pattern, not zeros */
        void *fx_mem = req ? malloc(req) : NULL;
        if (fx_mem) memset(fx_mem, FX_MEM_FILL, req);
        s_fx_state = alloc_state(OPZ_FX_STATE_BYTES);
        memset(g_scratch, 0xa5, sizeof g_scratch);
        double ts = now_ns();
        fx->init(s_fx_state, (uint32_t)sc->fx_track, 0, fx_mem, g_scratch); /* FxManager::noteOn, arg2 = 0 */
        double init_ns = now_ns() - ts;
        check_guard(s_fx_state, OPZ_FX_STATE_BYTES, fx->name, "init");
        uint32_t touched = 0;
        for (uint32_t i = 0; i < req; i++) touched += ((uint8_t *)fx_mem)[i] != FX_MEM_FILL;
        if (touched > FX_INIT_MAX_BYTES)
            fprintf(stderr, "warning %s: init writes %u B of mem with interrupts masked on the device; clear "
                    "delay lines progressively in renderFX\n", fx->name, touched);
        if (init_ns > FX_INIT_MAX_NS)
            fprintf(stderr, "warning %s: init took %.0f us of host time with interrupts masked on the device; "
                    "keep it short\n", fx->name, init_ns / 1000);
    }
    return 0;
}

/* ---- events ------------------------------------------------------------------------------------------ */

void host_set_param(int track, int index, int byte)
{
    g_tracks[track].bytes[index] = (uint8_t)byte;
    g_tracks[track].target[index] = opz_param_from_byte((uint8_t)byte);
}

void host_event(const Scenario *sc, const SimEvent *e, int verbose)
{
    Track *tr = &g_tracks[e->track];
    Engine *inst = s_inst, *fx = s_fx;
    int is_fx_track = e->track == sc->fx_track && fx;
    if (verbose && e->type != EV_PARAM)
        fprintf(stderr, "event   %8lld %-6s %s note %d vel %d value %d\n", (long long)e->time,
                track_name(e->track), e->type == EV_NOTE_ON ? "on " : e->type == EV_NOTE_OFF ? "off" :
                e->type == EV_BEND ? "bend" : "transport",
                e->note, e->velocity, e->value);
    switch (e->type) {
    case EV_PARAM:
        host_set_param(e->track, e->index, e->value);
        break;
    case EV_BEND:
        tr->bend_cents = e->value;
        break;
    case EV_TRANSPORT:
        sim_seq_transport(e->value, e->time);
        break;
    case EV_NOTE_ON:
        if (is_fx_track) {
            fx->onNoteOn(e->note, (uint8_t)e->velocity, s_fx_state, tr->current);
            check_guard(s_fx_state, OPZ_FX_STATE_BYTES, fx->name, "onNoteOn");
        } else if (e->track < SIM_INSTR_TRACKS && inst && tr->nvoices) {
            Voice *v = alloc_voice(tr);
            if (!v->initialized) { /* plug changed on this voice */
                memset(g_scratch, 0xa5, sizeof g_scratch);
                inst->init(v->state, (uint32_t)e->track, tr->pack, (void *)(uintptr_t)tr->pack, g_scratch);
                check_guard(v->state, OPZ_VOICE_STATE_BYTES, inst->name, "init");
                v->initialized = 1;
            }
            if (tr->pack && !sim_pack_loaded(tr->pack)) {
                /* Voice::noteOn locks the plug's pack before onNoteOn (FUN_8012e0a4 -> FUN_80100c40); while
                 * it is not loaded the lock fails and the note is dropped (samples.md §5) */
                if (!tr->dropped++)
                    fprintf(stderr, "note    %s: pack 0x%08x is not loaded: the OS drops the note before "
                            "onNoteOn\n", track_name(e->track), tr->pack);
            } else if (v->active && v->env.stage != ENV_IDLE) {
                /* Voice::noteOn on a busy voice: fade it (stage 5, c = 0.992, s = -level/256) and start the
                 * new note when the fade reaches idle, as the device does. */
                int64_t lvl = v->env.level < 0 ? 0 : v->env.level;
                v->env.stage = ENV_FADE;
                v->env.coef = 0x7f000000;
                v->env.step = -(int32_t)((lvl * 8388607) >> 31);
                v->env.release = 0;
                v->pending = 1;
                v->pending_note = e->note;
                v->pending_id = e->id;
                v->pending_vel = e->velocity;
                v->started = e->time;
            } else {
                start_note(inst, tr, v, e->note, e->id, e->velocity, e->time);
            }
            int used = 0;
            for (int i = 0; i < tr->nvoices; i++) used += tr->voices[i].active;
            if (used > tr->max_used) tr->max_used = used;
        }
        break;
    case EV_NOTE_OFF:
        if (is_fx_track) {
            fx->onNoteOff(e->note, s_fx_state);
            check_guard(s_fx_state, OPZ_FX_STATE_BYTES, fx->name, "onNoteOff");
        } else if (e->track < SIM_INSTR_TRACKS && inst) {
            for (int i = 0; i < tr->nvoices; i++) {
                Voice *v = &tr->voices[i];
                if (v->active && v->note_id == e->id) {
                    inst->onNoteOff(e->note, v->state);
                    check_guard(v->state, OPZ_VOICE_STATE_BYTES, inst->name, "onNoteOff");
                    sim_env_note_off(&v->env);
                }
            }
        }
        break;
    }
}

/* ---- one block --------------------------------------------------------------------------------------- */

/* Smooth the parameters, render every instrument voice and the effect. l and r come in zeroed, or holding the
 * effect's input when sc->input is IN_EXTERNAL (the web simulator's chain). in_t is the generator's clock. */
void host_block(const Scenario *sc, HostInput *in, int64_t in_t, fract32 *l, fract32 *r)
{
    Engine *inst = s_inst, *fx = s_fx;

    /* Synth::interpolateParameters: 1/64 one-pole per block */
    for (int t = 0; t < SIM_TRACKS; t++)
        for (int i = 0; i < OPZ_NUM_PARAMS; i++) {
            Track *tr = &g_tracks[t];
            tr->current[i] += (uint32_t)((int32_t)(tr->target[i] - tr->current[i]) >> 6);
        }

    /* instruments: every voice of a track adds into the same mono buffer */
    int active = 0;
    if (inst)
        for (int t = 0; t < SIM_INSTR_TRACKS; t++) {
            Track *tr = &g_tracks[t];
            if (!tr->nvoices) continue;
            memset(tr->buf, 0, sizeof tr->buf);
            for (int v = 0; v < tr->nvoices; v++)
                if (tr->voices[v].active) {
                    render_voice(inst, tr, &tr->voices[v], s_check_add, &g_host.inst_ns);
                    g_host.voice_blocks++;
                }
            for (int v = 0; v < tr->nvoices; v++) active += tr->voices[v].active;
            for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
                l[i] = opz_add_q31(l[i], tr->buf[2 * i]);
                r[i] = opz_add_q31(r[i], tr->buf[2 * i + 1]);
            }
        }
    g_host.active = active;

    /* effect: in place on the FX track's stereo buffers */
    if (fx) {
        if (sc->input != IN_INSTRUMENT && sc->input != IN_EXTERNAL) gen_input(sc, in, in_t, l, r);
        fract32 dry_l[OPZ_BLOCK_FRAMES], dry_r[OPZ_BLOCK_FRAMES];
        memcpy(dry_l, l, sizeof dry_l);
        memcpy(dry_r, r, sizeof dry_r);
        Track *ft = &g_tracks[sc->fx_track];
        fract32 bend = (fract32)((int64_t)ft->bend_cents * 2147483647 / 200);
        double ts = now_ns();
        /* device ABI (crush, device and app): interleaved stereo in -> separate interleaved stereo out */
        fract32 fx_in[2 * OPZ_BLOCK_FRAMES], fx_out[2 * OPZ_BLOCK_FRAMES];
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
            fx_in[2 * i] = l[i];
            fx_in[2 * i + 1] = r[i];
            fx_out[2 * i] = fx_out[2 * i + 1] = (fract32)FX_OUT_SENTINEL;
        }
        fx->renderFX(ft->current, bend, s_fx_state, fx_in, fx_out, sc->gain);
        g_host.fx_ns += now_ns() - ts;
        g_host.fx_blocks++;
        int unwritten = 0;
        for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) unwritten += fx_out[i] == (fract32)FX_OUT_SENTINEL;
        if (unwritten && !g_fx_unwritten_warned) {
            g_fx_unwritten_warned = 1;
            fprintf(stderr, "warning %s: renderFX left %d of %d output samples unwritten (it must write every "
                    "sample of out; in and out are separate interleaved buffers)\n", fx->name, unwritten,
                    2 * OPZ_BLOCK_FRAMES);
        }
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
            l[i] = fx_out[2 * i];
            r[i] = fx_out[2 * i + 1];
        }
        check_guard(s_fx_state, OPZ_FX_STATE_BYTES, fx->name, "renderFX");
        if (memcmp(dry_l, l, sizeof dry_l) || memcmp(dry_r, r, sizeof dry_r)) g_host.unchanged = 0;
    }
}
