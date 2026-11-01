/*
 * websim.c — live entry points of the web simulator (sim/web).
 *
 * Each engine is built into its own WebAssembly module together with opzsim's host (sim/src). The browser plays one
 * engine per module instance: keys, knobs and presets arrive between blocks and every ws_render runs the same
 * host_block as opzsim, so a note played here goes through the same smoothing, envelope, voice allocation and ABI
 * checks as a scenario. Scenarios themselves run through opzsim_main (opzsim.c built with -Dmain=opzsim_main) in a
 * fresh instance, which makes their renders bit-identical to opzsim's goldens.
 *
 * Messages (warnings, ABI errors) go to stderr, which the page shows as opzsim's log.
 */
#include <stdio.h>
#include <string.h>

#include "host.h"

#ifndef WS_ENGINE_NAME
#define WS_ENGINE_NAME "engine"
#endif

#define WS_EXPORT(name) __attribute__((export_name(#name)))
#define SWEEP_SECONDS 4                           /* the live `sweep` input repeats every 4 s */

static ModuleCallbackInterface s_cb;
static Engine s_eng;
static Scenario s_sc;
static HostInput s_in;
static int64_t s_t;                               /* frames rendered */
static int s_next_id;
static int s_note_id[SIM_TRACKS][128];            /* note id of the last note on, per key */
static fract32 s_out[2 * OPZ_BLOCK_FRAMES];       /* interleaved stereo, the last block */
static fract32 s_ext[2 * OPZ_BLOCK_FRAMES];       /* IN_EXTERNAL: left block, then right block */

/* Load the engine and set up one track. track: an instrument track 0-7, or the FX track 8-11 for an effect.
 * Returns 0 for an instrument, 1 for an effect, -1 if the engine was refused or the track does not fit it. */
WS_EXPORT(ws_open) int ws_open(int track, int generous)
{
    sim_callbacks_init(&s_cb);
    memset(&s_eng, 0, sizeof s_eng);
    if (engine_load(&s_eng, WS_ENGINE_NAME, &s_cb)) return -1;
    s_eng.slot = s_eng.trackmask = -1;
    scenario_init(&s_sc, s_eng.is_fx ? track : OPZ_TRACK_FX1);
    if (s_eng.is_fx != (track >= SIM_INSTR_TRACKS) || track < 0 || track >= 12) {
        fprintf(stderr, "error   %s: track %s does not take %s\n", s_eng.name, track_name(track),
                s_eng.is_fx ? "an effect" : "an instrument");
        return -1;
    }
    if (!s_eng.is_fx) s_sc.track_used[track] = 1;
    s_sc.input = IN_SILENCE;
    if (host_setup(s_eng.is_fx ? NULL : &s_eng, s_eng.is_fx ? &s_eng : NULL, &s_sc, generous, 1)) return -1;
    host_input_init(&s_sc, &s_in);
    return s_eng.is_fx;
}

WS_EXPORT(ws_mem_request) uint32_t ws_mem_request(void) { return s_eng.mem_request; }

/* Effect input: IN_SILENCE, IN_SINE, IN_SAW, IN_NOISE, IN_SWEEP (f0 -> f1 over 4 s, repeating), IN_IMPULSE (every f0
 * steps) or IN_EXTERNAL (ws_ext_in, e.g. another engine's output). */
WS_EXPORT(ws_input) void ws_input(int kind, double f0, double f1, double amp)
{
    s_sc.input = kind;
    s_sc.in_f0 = f0;
    s_sc.in_f1 = f1;
    s_sc.in_amp = amp;
    host_input_init(&s_sc, &s_in);
}

WS_EXPORT(ws_ext_in) fract32 *ws_ext_in(void) { return s_ext; }

WS_EXPORT(ws_gain) void ws_gain(double g)
{
    s_sc.gain = g >= 1 ? SIM_FULL : (fract32)(int64_t)(g * 2147483647.0 + 0.5);
}

/* sequencerGetValue(4) and the clock (keys 5-7) follow the new tempo from the next tick; play restarts it. */
WS_EXPORT(ws_tempo) void ws_tempo(double bpm)
{
    s_sc.tempo = bpm;
    s_sc.step_samples = OPZ_SAMPLE_RATE * 15.0 / bpm;
    g_seq.tempo = (int)(bpm + 0.5);
    g_seq.clock_bpm = bpm;
    sim_seq_transport(1, s_t);
}

WS_EXPORT(ws_transport) void ws_transport(int play) { sim_seq_transport(play, s_t); }

static void event(int type, int track, int note, int velocity, int id, int value)
{
    SimEvent e = { .time = s_t, .type = type, .track = track, .note = note, .velocity = velocity, .id = id,
                   .value = value };
    host_event(&s_sc, &e, 0);
}

WS_EXPORT(ws_note_on) void ws_note_on(int track, int note, int velocity)
{
    s_note_id[track][note & 127] = ++s_next_id;
    event(EV_NOTE_ON, track, note & 127, velocity, s_next_id, 0);
}

WS_EXPORT(ws_note_off) void ws_note_off(int track, int note)
{
    event(EV_NOTE_OFF, track, note & 127, 0, s_note_id[track][note & 127], 0);
}

WS_EXPORT(ws_bend) void ws_bend(int track, int cents) { event(EV_BEND, track, 0, 0, 0, cents); }

/* Set a parameter byte. immediate: load it unsmoothed, as a preset at step 0 is; else smoothed per block. */
WS_EXPORT(ws_param) void ws_param(int track, int index, int byte, int immediate)
{
    if (track < 0 || track >= SIM_TRACKS || index < 0 || index >= OPZ_NUM_PARAMS) return;
    host_set_param(track, index, byte);
    if (immediate) g_tracks[track].current[index] = g_tracks[track].target[index];
}

/* Render one 64-frame block; returns it as interleaved stereo Q31. */
WS_EXPORT(ws_render) fract32 *ws_render(void)
{
    fract32 l[OPZ_BLOCK_FRAMES] = { 0 }, r[OPZ_BLOCK_FRAMES] = { 0 };
    if (s_sc.input == IN_EXTERNAL) {
        memcpy(l, s_ext, sizeof l);
        memcpy(r, s_ext + OPZ_BLOCK_FRAMES, sizeof r);
    }
    int64_t in_t = s_t;
    if (s_sc.input == IN_SWEEP) {               /* gen_input sweeps over sc->steps: make that one period */
        int64_t period = (int64_t)SWEEP_SECONDS * OPZ_SAMPLE_RATE;
        s_sc.steps = (double)period / s_sc.step_samples;
        in_t = s_t % period;
    }
    sim_seq_clock(s_t);
    host_block(&s_sc, &s_in, in_t, l, r);
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        s_out[2 * i] = l[i];
        s_out[2 * i + 1] = r[i];
    }
    s_t += OPZ_BLOCK_FRAMES;
    return s_out;
}

/* Counters for the page: 0 voices active, 1 state violations, 2 add violations, 3 notes that stole a voice,
 * 4 voices on the track at most, 5 voices on the track, 6 blocks rendered. */
WS_EXPORT(ws_stat) int ws_stat(int which, int track)
{
    switch (which) {
    case 0: return g_host.active;
    case 1: return g_host.state_violations;
    case 2: return g_host.add_violations;
    case 3: return g_host.voices_stolen;
    case 4: return g_tracks[track].max_used;
    case 5: return g_tracks[track].nvoices;
    case 6: return (int)(s_t / OPZ_BLOCK_FRAMES);
    default: return 0;
    }
}

/* Host time so far: 0 ns in render, 1 voice blocks, 2 ns in renderFX, 3 FX blocks. Host CPU, not Blackfin cycles. */
WS_EXPORT(ws_time) double ws_time(int which)
{
    switch (which) {
    case 0: return g_host.inst_ns;
    case 1: return (double)g_host.voice_blocks;
    case 2: return g_host.fx_ns;
    case 3: return (double)g_host.fx_blocks;
    default: return 0;
    }
}
