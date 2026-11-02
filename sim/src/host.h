/*
 * host.h — shared types for opzsim, the OP-Z engine host simulator.
 *
 * Everything here is our own model of the host described in sdk/docs/engine-abi.md and
 * research/architecture/02-sound-engine-host.md. No TE code or data.
 */
#ifndef OPZSIM_HOST_H
#define OPZSIM_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "opz_engine.h"

#define SIM_TRACKS        16
#define SIM_INSTR_TRACKS  8
#define SIM_MAX_VOICES    6
#define SIM_GUARD_BYTES   256
#define SIM_SCRATCH_BYTES 4096
#define SIM_PRESET_BYTES  4096
#define SIM_FULL          0x7fffffff

/* ---- envelope: the host side of the OP-Z ADSR (decoded from the app; sdk/docs/engine-abi.md §3) -------- */

enum { ENV_IDLE = 0, ENV_ATTACK = 1, ENV_DECAY = 2, ENV_SUSTAIN = 3, ENV_RELEASE = 4, ENV_FADE = 5, ENV_FADE2 = 6,
       ENV_HOLD = 7 /* drums (updateParametersAHD); their decay uses stage 4 */ };

typedef struct SimADSR {
    int32_t attack_step; /* +0  attack step for the next note on (set by updateParametersADSR) */
    int32_t level;       /* +4  Q31 */
    int32_t coef;        /* +8  Q31: level' = coef*level + (1 - (coef*level)^3) * step */
    int32_t step;        /* +12 Q31 */
    int32_t stage;       /* +16 0 idle (voice freed), 1 attack, 2 decay, 3 sustain, 4 release, 5/6 fade, 7 hold */
    uint8_t new_note;    /* +20 set on note on */
    uint8_t release;     /* +21 set on note off; consumed by updateParametersADSR */
    uint8_t pad;         /* +22 */
    uint8_t gate;        /* +23 sim only: key held (voice-stealing preference) */
} SimADSR;

/* ---- callbacks.c ------------------------------------------------------------------------------------- */

typedef struct SimSeq {
    int tempo;              /* returned for get id 4 unless overridden */
    int get_override[16];   /* value for get ids 0..15 at index 0 */
    uint8_t get_set[16];
    uint8_t track_flag[SIM_TRACKS]; /* get/set id 3 */
    int ducker;             /* last set id 1 */
    int fx_timing[SIM_TRACKS];
    int verbose;
    /* clock (sim_seq_clock, Sequencer::tick in the app): get ids 5, 6, 7 */
    int playing, step, tick;
    double clock_bpm;       /* the scenario tempo, unrounded */
    int64_t origin, ticks;  /* ticks run since origin (scenario start or the last transport play) */
} SimSeq;

extern SimSeq g_seq;
extern char g_preset_path[1024];

void sim_callbacks_init(ModuleCallbackInterface *cb);
void sim_seq_transport(int play, int64_t t);
void sim_seq_clock(int64_t t0);
void sim_env_note_on(SimADSR *env);
void sim_env_note_off(SimADSR *env);
uint32_t sim_crc32(const char *str);
/* Load an OP-1 format .aif (file NULL: the built-in test buffer) as the pack of a plug whose `samples` path is
 * samples_path; *id = crc32(samples_path), the pack id. 0 ok, -1 error (printed). */
int sim_pack_attach(const char *file, const char *samples_path, uint32_t *id);
/* 1 if a pack with this id is attached (loaded), 0 if not: the OS drops notes of a plug whose pack is not loaded. */
int sim_pack_loaded(uint32_t id);
double sim_exp2(double x);  /* deterministic, libm-free */
double sim_sin(double x);

/* ---- scenario.c -------------------------------------------------------------------------------------- */

enum {
    EV_NOTE_ON, EV_NOTE_OFF, EV_PARAM, EV_BEND, EV_TRANSPORT,
};

typedef struct SimEvent {
    int64_t time;     /* sample */
    int order;        /* tie-break: note off before note on */
    int seq;          /* file order */
    int type;
    int track;        /* 0..15 */
    int note;
    int velocity;
    int id;           /* note id, pairs on/off */
    int index;        /* param index */
    int value;        /* param byte, bend cents */
} SimEvent;

typedef struct SimSweep {
    int track, index;
    int from, to;
    int64_t start, end; /* samples */
} SimSweep;

enum { SAMPLE_BUILTIN = 1, SAMPLE_UNLOADED = 2 };
enum { IN_SILENCE, IN_SINE, IN_SAW, IN_NOISE, IN_SWEEP, IN_IMPULSE, IN_INSTRUMENT,
       IN_EXTERNAL /* web simulator: the caller fills the effect's input */ };
enum { EXP_SOUND, EXP_SILENCE, EXP_IDLE, EXP_UNCHANGED, EXP_PEAK_BELOW };

typedef struct SimExpect {
    int type;
    int64_t from, to; /* samples */
    double value;
    int line;
} SimExpect;

typedef struct Scenario {
    char path[1024];
    double tempo;
    double step_samples;
    double steps;
    double tail;
    int64_t frames;
    uint8_t init_params[SIM_TRACKS][OPZ_NUM_PARAMS];
    uint8_t track_used[SIM_TRACKS];
    char sample_file[SIM_INSTR_TRACKS][1024]; /* `sample`: the .aif on disk ("" for builtin/unloaded) */
    char samples_path[SIM_INSTR_TRACKS][128]; /* the plug's samples path; crc32 = pack id = init's arg2/mem */
    uint8_t sample_builtin[SIM_INSTR_TRACKS]; /* SAMPLE_BUILTIN, SAMPLE_UNLOADED (an id, no pack) or 0 */
    int fx_track;
    fract32 gain;
    int input;
    double in_f0, in_f1, in_amp;
    SimEvent *events;
    int nevents, cap_events;
    SimSweep *sweeps;
    int nsweeps, cap_sweeps;
    SimExpect *expects;
    int nexpects, cap_expects;
} Scenario;

/* Defaults of an empty scenario (tempo 120, 16 steps, 1 s tail, sustain 255, pan 128, level 200, filter 255). */
void scenario_init(Scenario *sc, int default_fx_track);
int scenario_load(Scenario *sc, const char *path, int default_track, int default_fx_track);
const char *track_name(int track);
int64_t scenario_step_to_sample(const Scenario *sc, double step);

/* ---- engine.c ---------------------------------------------------------------------------------------- */

typedef struct SlotSize {
    uint64_t code, rodata, data, bss, other;
    int has_constructors;
    int not_measured;     /* the wasm build links the engine in: no separate image to measure */
    char undefined[1024]; /* comma-separated imports other than dyld_stub_binder */
} SlotSize;

typedef struct Engine {
    char path[1024];
    char name[256];
    void *handle;
    int is_fx;
    uint32_t (*initModule)(uint32_t, const ModuleCallbackInterface *, uint32_t *);
    void (*init)(void *, uint32_t, uint32_t, void *, void *);
    void (*onNoteOn)(int, uint8_t, void *, const uint32_t *);
    void (*onNoteOff)(int, void *);
    void (*render)(fract32 *, uint32_t, int32_t, void *, const uint32_t *, ADSR *);
    void (*renderFX)(const uint32_t *, fract32, void *, const fract32 *, fract32 *, fract32);
    uint32_t mem_request;
    int slot;          /* -1 unknown */
    int trackmask;     /* -1 unknown */
    SlotSize size;
    int size_ok;
} Engine;

int engine_load(Engine *e, const char *path, const ModuleCallbackInterface *cb);
int engine_slot_bytes(int slot);
int engine_inspect_size(const char *path, SlotSize *out);

/* ---- host.c: the block loop, shared by opzsim and the web simulator (sim/wasm) ------------------------ */

typedef struct Voice {
    uint8_t *state;      /* OPZ_VOICE_STATE_BYTES + guard */
    SimADSR env;
    int active, note, note_id, initialized, fresh;   /* fresh: no block rendered since note on */
    int pending, pending_note, pending_id, pending_vel; /* note waiting for this voice's fade (device steal) */
    int64_t started;
    uint32_t pitch_prev;
} Voice;

typedef struct Track {
    uint8_t bytes[OPZ_NUM_PARAMS];
    uint32_t target[OPZ_NUM_PARAMS], current[OPZ_NUM_PARAMS];
    int bend_cents;
    int nvoices, max_used;
    Voice voices[SIM_MAX_VOICES];
    fract32 buf[2 * OPZ_BLOCK_FRAMES];   /* interleaved stereo, like the device (proven on hardware) */
    uint32_t pack;                       /* sample-pack id passed as init's arg2/mem, 0 without a pack */
    int dropped;                         /* notes dropped because that pack is not loaded */
} Track;

typedef struct HostInput {               /* the effect-input generator (`input` directive) */
    uint32_t phase, inc, rng_l, rng_r;
    fract32 amp;
} HostInput;

typedef struct HostStats {
    int state_violations, add_violations, voices_stolen;
    int active;                          /* voices active after the last block */
    int unchanged;                       /* effect output identical to its input so far */
    double inst_ns, fx_ns;               /* host time in render / renderFX */
    int64_t voice_blocks, fx_blocks;
} HostStats;

extern Track g_tracks[SIM_TRACKS];
extern HostStats g_host;

double now_ns(void);
/* Tracks, voices (on the scenario's used instrument tracks), sample packs, the effect's mem and init. 0 ok. */
int host_setup(Engine *inst, Engine *fx, const Scenario *sc, int generous, int check_add);
void host_event(const Scenario *sc, const SimEvent *e, int verbose);
void host_set_param(int track, int index, int byte);
void host_input_init(const Scenario *sc, HostInput *in);
/* One block: parameter smoothing, instruments, effect. Call sim_seq_clock (and apply sweeps) first. */
void host_block(const Scenario *sc, HostInput *in, int64_t in_t, fract32 *l, fract32 *r);

/* ---- output.c ---------------------------------------------------------------------------------------- */

int wav_write(const char *path, const fract32 *interleaved, int64_t frames, int bits);

typedef struct SimStats {
    double peak_dbfs, rms_dbfs, dc;
    int64_t clipped;
    double bands_db[10];
} SimStats;

void stats_compute(const fract32 *interleaved, int64_t frames, SimStats *st);
double segment_rms_dbfs(const fract32 *interleaved, int64_t from, int64_t to);
double segment_peak_dbfs(const fract32 *interleaved, int64_t from, int64_t to);

#endif /* OPZSIM_HOST_H */
