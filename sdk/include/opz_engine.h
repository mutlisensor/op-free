/*
 * opz_engine.h — OP-Z engine module ABI (firmware 1.2.x)
 *
 * DRAFT. Reconstructed from the OP-Z desktop app's symbols and from decompiled device code.
 * Not yet verified by running a custom engine on hardware. See sdk/docs/engine-abi.md.
 * Items marked TBD map to research/OPEN_QUESTIONS.md.
 *
 * The same header compiles for the Blackfin target and for the host simulator (OPZ_HOST_SIM).
 */
#ifndef OPZ_ENGINE_H
#define OPZ_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t fract32; /* Q31 */
typedef int16_t fract16; /* Q15 */

#define OPZ_SAMPLE_RATE         44100 /* pitch words are Q32 phase increments at this rate (app code) */
#define OPZ_BLOCK_FRAMES        64   /* frames per render call (app: loop count and pitch slope >> 6) */
#define OPZ_NUM_PARAMS          18

/* Peak level per voice at full velocity. Calibrated on hardware: the test sine at -12 dBFS matches TE's saw at the
 * same note and knobs. Leave room for overlapping voices. */
#define OPZ_VOICE_PEAK          0x20000000   /* -12 dBFS, Q31 */
/* State areas, from the device OS (opz.ldr 1.2.45, docs/research/p0-device-abi.md sections 3-4). The app's numbers
 * (308 and 204, record size minus state offset) are too big: the OS keeps data at the end of each record. */
#define OPZ_VOICE_STATE_BYTES   280  /* voice record 0x17c: state +0x48..+0x15f, OS pitch state +0x160..+0x17b */
#define OPZ_FX_STATE_BYTES      200  /* FX slot 0xe0: state +0x10..+0xd7, pitch bend +0xd8, plug id +0xdc */

#define OPZ_FX1_POOL_BYTES      0x200000u /* app and device agree */
#define OPZ_FX2_POOL_BYTES      0x040000u
#define OPZ_TAPE_POOL_BYTES     0x200000u
#define OPZ_MASTER_POOL_BYTES   0x004000u /* device: table at 0x8156f21c follows the master pool */

/* Indices into the params array passed to onNoteOn/render/renderFX. */
enum {
    OPZ_P_P1 = 0,
    OPZ_P_P2,
    OPZ_P_ATTACK,
    OPZ_P_DECAY,
    OPZ_P_SUSTAIN,
    OPZ_P_RELEASE,
    OPZ_P_FX1_SEND,
    OPZ_P_FX2_SEND,
    OPZ_P_FILTER,
    OPZ_P_RESONANCE,
    OPZ_P_PAN,
    OPZ_P_LEVEL,
    OPZ_P_PORTAMENTO,
    OPZ_P_LFO_DEPTH,
    OPZ_P_LFO_SPEED,
    OPZ_P_LFO_TARGET,
    OPZ_P_LFO_SHAPE,
    OPZ_P_NOTE_STYLE
};

/* Track indices (bit n of a plug's trackmask). */
enum {
    OPZ_TRACK_KICK = 0, OPZ_TRACK_SNARE, OPZ_TRACK_PERC, OPZ_TRACK_SAMPLE,
    OPZ_TRACK_BASS, OPZ_TRACK_LEAD, OPZ_TRACK_ARP, OPZ_TRACK_CHORD,
    OPZ_TRACK_FX1, OPZ_TRACK_FX2, OPZ_TRACK_TAPE, OPZ_TRACK_MASTER
};

/* Host-owned envelope, advanced by the engine (app + device, sdk/docs/engine-abi.md §3):
 *   +0 attack rate, +4 level (Q31), +8 rate/coefficient, +12 step/target, +16 stage (0 = idle),
 *   +20 new-note byte, +21 release flag. The host frees the voice when stage reaches 0 (opz_adsr.h). */
typedef struct ADSR ADSR;
#define OPZ_ADSR_LEVEL(env)  (*(volatile fract32 *)((uint8_t *)(env) + 4))
#define OPZ_ADSR_STAGE(env)  (*(volatile int32_t *)((uint8_t *)(env) + 16))
typedef struct IIRFilter IIRFilter;

/* Passed to initModule. 13 pointer-sized entries; offsets are for the 32-bit device. */
typedef struct ModuleCallbackInterface {
    void    (*sequencerSetValue)(int key, int value);                                /* +0x00 */
    int     (*sequencerGetValue)(int key);                                           /* +0x04 */
    /* Shared tables (formats, indexing and accessors: opz_tables.h; engine-abi.md §5) */
    const fract32 *k_lp_table;      /* +0x08 129 x Q31 one-pole coefficients, index 0..128 */
    const fract32 *g_sine;          /* +0x0C 8,193 x Q31 sine cycle, peak 0x7FFF0000, index phase >> 19 */
    const int16_t *g_sineTable;     /* +0x10 256 x int16 sine cycle, index phase >> 24 */
    const fract32 *const *rFilterCoeffs; /* +0x14 5 pointers: b0, b1/2, b2, a1/2, a2, each [9][65] Q31 */
    const fract32 *releaseTable;    /* +0x18 128 x Q31 per-sample decay multipliers, index param >> 24 */
    void    (*updateParametersADSR)(ADSR *env, uint32_t a, uint32_t d,
                                    uint32_t s, uint32_t r);                         /* +0x1C */
    /* Drum envelope: arguments are sample positions, not knob words (opz_adsr.h, opz_ahd_layout) */
    void    (*updateParametersAHD)(ADSR *env, uint32_t pos, uint32_t attack_end, uint32_t hold_end,
                                   uint32_t decay_end, fract32 coef, fract32 shape); /* +0x20 */
    /* +0x24 device only; null in the app. The first argument is the plug's sample-pack id, i.e. init's arg2 (crc32
     * of the plug's "samples" path, 0 without one), not the track. Returns an OpzSampleSlice, or 0 for an unknown or
     * unloaded id. Call it in onNoteOn: the OS only calls onNoteOn once it has locked the pack for the voice (a note
     * on an unloaded pack is dropped), and the slice stays valid until the voice ends. Any engine gets it; nothing
     * checks the engine (Q-27). research/samples.md */
    const void *(*getSample)(int packId, uint8_t note);
    int     (*getPresetData)(int a, int b, void *out); /* +0x28 reads engines/fmedit.preset (<= 4096 B) */
    void    (*IIRFilter_updateCoefficientsPeak)(IIRFilter *f, uint16_t freq,
                                                uint8_t gain);                       /* +0x2C */
    void    (*IIRFilter_filter)(IIRFilter *f, fract16 *buf, uint8_t n);              /* +0x30 */
} ModuleCallbackInterface;

/* What getSample returns: one key of a loaded drum pack (key = (note + 19) mod 24, clamped to the keys the pack
 * has) or the one slice of a synth sample. 32 bytes in the OS's sample cache (SDRAM), built by the loader (device
 * FUN_80105e94). The voice that called onNoteOn holds a lock on the pack until it ends, so the pointer is valid until
 * then; fetch it again on every note. What TE's engines do with each word, and a Python model of their playback
 * maths: research/samples.md §8, tools/opz_sampleplay.py (Q-26). */
typedef struct OpzSampleSlice {
    const int16_t *data;   /* +0x00 first frame: 16-bit mono, 44.1 kHz */
    uint32_t frames;       /* +0x04 length in frames (end - start) */
    uint32_t knobs;        /* +0x08 drum: volume (bits 0-15) | playmode >> 7 (bits 16-23) | reverse >> 7 (bits 24-31);
                            *       synth: knobs[7] volume (bits 0-15) | knobs[4] (play mode) >> 7 (bits 24-31).
                            *       Gain = (volume / 8192)^2. Playmode: < 11008 gate, < 21888 trigger, else loop.
                            *       Reverse byte > 127: backwards. TE applies playmode/reverse only when editable */
    uint32_t rate;         /* +0x0C speed, Q31 of ratio / 16: OPZ_SLICE_RATE_UNITY = original speed. Drum: from the
                            * key's pitch knob (512 per semitone); synth: 261.626 Hz / base_freq. +-4 octaves */
    uint32_t loopStart;    /* +0x10 synth: loop start, frames from data; drum: 0 */
    uint32_t loopEnd;      /* +0x14 synth: loop end, frames from data (loops if loopStart < loopEnd < frames) */
    int32_t pitch;         /* +0x18 drum: the key's pitch knob (low 9 bits cleared for drum_version < 3); unused by TE */
    uint8_t editable;      /* +0x1C the pack's "editable" flag */
    uint8_t pad[3];
} OpzSampleSlice;

#define OPZ_SLICE_RATE_UNITY   (1u << 27)
#define OPZ_SLICE_VOLUME(s)    ((s)->knobs & 0xffffu)          /* 8192 = gain 1.0 */
#define OPZ_SLICE_PLAYMODE(s)  (((s)->knobs >> 16) & 0xffu)    /* playmode >> 7: x3 < 256 gate, < 512 trigger */
#define OPZ_SLICE_REVERSE(s)   ((s)->knobs >> 31)              /* reverse (drum) / play mode (synth) >= 16384 */

/* ---- Exports every engine must provide (plain C names) -------------------------------------- */

/* Sequencer keys: key = (index << 16) | id. See sdk/docs/engine-abi.md §5. */
#define OPZ_SEQ_KEY(id, index) ((int)(((uint32_t)(index) << 16) | (uint32_t)(id)))
enum { OPZ_SEQ_GET_TRACK_TIMING = 2 /* FX timing of track index */, OPZ_SEQ_GET_HAS_FX_TIMING = 3,
       OPZ_SEQ_GET_TEMPO = 4 /* whole BPM */, OPZ_SEQ_GET_PLAYING = 5, OPZ_SEQ_GET_STEP = 6,
       OPZ_SEQ_GET_TICK = 7 /* 0..23 */,
       OPZ_SEQ_GET_SAMPLE_EDIT = 8 /* 1 while sample mode edits the pack: TE's engines then re-read the slice
                                    * every block; not a "loaded" flag, playback does not depend on it */ };
                                                                              /* opz_tempo.h */
enum { OPZ_SEQ_SET_FLAG = 0, OPZ_SEQ_SET_TAPE_DUCKER = 1, OPZ_SEQ_SET_FX_TIMING = 2, OPZ_SEQ_SET_HAS_FX_TIMING = 3 };

/* Once at load. Store cb; write the large-buffer size you need to *memRequest; return non-zero. */
uint32_t initModule(uint32_t version, const ModuleCallbackInterface *cb, uint32_t *memRequest);

/* When a voice / FX slot switches to this plug. Clear and set up *state. */
/* arg2 on instruments = mem = the plug's sample-pack id (getSample's first argument; 0 without a pack); 0 on FX. */
void init(void *state, uint32_t track, uint32_t arg2, void *mem, void *scratch);

void onNoteOn(int note, uint8_t velocity, void *state, const uint32_t *params);
void onNoteOff(int note, void *state);

/* Instruments export render: ADD into out, saturating; out is 64 frames of interleaved stereo (128 values, L R L R)
 * — proven on hardware and in TE's engines. Effects export renderFX (interleaved in -> separate interleaved out). */
void render(fract32 *out, uint32_t pitch, int32_t pitchSlope,
            void *state, const uint32_t *params, ADSR *env);

/* Effects: 64 frames of interleaved stereo in (L, R, L, R, ...) -> write all 128 samples of out, also interleaved.
 * in and out are separate buffers (TE's crush, device and app); don't rely on out holding the input. */
void renderFX(const uint32_t *params, fract32 pitchBend, void *state,
              const fract32 *in, fract32 *out, fract32 gain);

/* ---- Export table placed at the slot base by the linker script ---------------------------- */

#define OPZ_EXPORT_MAGIC0 0xfeed0ad1u
#define OPZ_EXPORT_MAGIC1 0xad1facefu

typedef struct OpzExport {
    const char *name;
    const void *target;
} OpzExport;

typedef struct OpzExportTable {
    uint32_t magic0, magic1;
    OpzExport entries[5];
    OpzExport end; /* {0, 0} */
} OpzExportTable;

/* Put exactly one of these at the end of an engine's source. On the Blackfin build it emits the export table into
 * section .opz_exports, which sdk/ld/engine.ld.in places at the slot base. On the host it expands to nothing
 * (opzsim finds the functions with dlsym). */
#if defined(__bfin__)
#define OPZ_EXPORT_TABLE_(RENDER_NAME, RENDER_FN)                                                              \
    __attribute__((section(".opz_exports"), used)) const OpzExportTable opz_exports = {                    \
        OPZ_EXPORT_MAGIC0, OPZ_EXPORT_MAGIC1,                                                                \
        { { "initModule", (const void *)initModule }, { "init", (const void *)init },                        \
          { "onNoteOn", (const void *)onNoteOn }, { "onNoteOff", (const void *)onNoteOff },                  \
          { RENDER_NAME, (const void *)RENDER_FN } },                                                        \
        { 0, 0 } }
#else
#define OPZ_EXPORT_TABLE_(RENDER_NAME, RENDER_FN) typedef int opz_exports_unused_
#endif
#define OPZ_EXPORTS_INSTRUMENT() OPZ_EXPORT_TABLE_("render", render)
#define OPZ_EXPORTS_FX()         OPZ_EXPORT_TABLE_("renderFX", renderFX)

/* Helpers -------------------------------------------------------------------------------------- */

/* Parameter byte b as delivered by the host: b*0x808080 + (b >> 1). */
static inline uint32_t opz_param_from_byte(uint8_t b) { return (uint32_t)b * 0x808080u + (b >> 1); }

/* Saturating Q31 helpers usable on host and target.
 *
 * opz_sat32 on an int64 is 10-15 Blackfin instructions with branches, and 64-bit products call libgcc's
 * ___muldi3: keep both out of per-sample code (per block or per note is fine). Per sample, use the helpers below.
 * Each maps to one to six Blackfin instructions, and the host versions give the same bits as the Blackfin
 * (checked in the simulator by tools/tests/test_opz_cycles.py), so opzsim renders what the device renders. */
static inline fract32 opz_sat32(int64_t x)
{
    return x > INT32_MAX ? INT32_MAX : (x < INT32_MIN ? INT32_MIN : (fract32)x);
}
#if defined(__bfin__)
#if defined(OPZ_BLACKFIN_PLUS)
/* EXPERIMENTAL (Q-47, research/cpu-headroom.md §2.4): the BF70x's single 32 x 32 fractional multiply,
 * "Rd = Ra * Rb (T)", emitted as raw words because GNU as only knows the classic ISA: 0xC231, 0x2000 | d << 6 |
 * a << 3 | b. TE's own code uses exactly this form (136 times in opz.ldr 1.2.45, e.g. the track filter at
 * 0x11a00e52) [code], and the desktop app's build of the same filter truncates ((long)a * b >> 31), so (T) gives
 * floor(a * b / 2^31) like the classic version below [inferred, strong; not yet heard on the device]. Only our
 * patched simulator runs it (~/opt/bfin-elf-bfp, research/toolchain-setup.md); the classic bfin-elf-run treats
 * it as an illegal instruction, so opz_cycles.py refuses such an image unless --blackfin-plus picks that simulator. */
__asm__(".ifndef .Lopz_mult32_defined\n\t.set .Lopz_mult32_defined, 1\n"
        "\t.set .Lopz_R0, 0\n\t.set .Lopz_R1, 1\n\t.set .Lopz_R2, 2\n\t.set .Lopz_R3, 3\n"
        "\t.set .Lopz_R4, 4\n\t.set .Lopz_R5, 5\n\t.set .Lopz_R6, 6\n\t.set .Lopz_R7, 7\n"
        "\t.macro OPZ_MULT32_T d, a, b\n"
        "\t.short 0xC231, 0x2000 | (.Lopz_\\d << 6) | (.Lopz_\\a << 3) | .Lopz_\\b\n"
        "\t.endm\n.endif");
static inline fract32 opz_mul_q31(fract32 a, fract32 b)
{
    fract32 r;
    __asm__("OPZ_MULT32_T %0, %1, %2" : "=d"(r) : "d"(a), "d"(b));
    return r;
}
#else
/* floor(a * b / 2^31), saturated (only -1 * -1): the 6-instruction 32 x 32 fractional multiply. */
static inline fract32 opz_mul_q31(fract32 a, fract32 b) { return __builtin_bfin_mult_fr1x32x32(a, b); }
#endif
/* a + b, a - b, x * 2^n (n >= 0), all saturating: one instruction each. */
static inline fract32 opz_add_q31(fract32 a, fract32 b) { return __builtin_bfin_add_fr1x32(a, b); }
static inline fract32 opz_sub_q31(fract32 a, fract32 b) { return __builtin_bfin_sub_fr1x32(a, b); }
static inline fract32 opz_shl_q31(fract32 x, int n) { return __builtin_bfin_shl_fr1x32(x, n); }
/* floor(x * c / 2^15) for a Q15 c (only the low 16 bits are used, signed), saturated (only -1 * -1): three
 * instructions, for gains and coefficients that need no more than 16 bits. */
static inline fract32 opz_mul_q31_q15(fract32 x, int32_t c)
{
    fract32 r;
    __asm__("A1 = %2.L * %1.L (M), A0 = %2.L * %1.H;\n\tA1 = A1 >>> 15;\n\t%0 = (A0 += A1);"
            : "=d"(r) : "d"(x), "d"(c) : "A0", "A1");
    return r;
}
/* Redundant sign bits of x (x > 0: leading zeros - 1), one instruction. */
static inline int opz_norm32(int32_t x) { return __builtin_bfin_norm_fr1x32(x); }
#else
static inline fract32 opz_mul_q31(fract32 a, fract32 b) { return opz_sat32(((int64_t)a * b) >> 31); }
static inline fract32 opz_add_q31(fract32 a, fract32 b) { return opz_sat32((int64_t)a + b); }
static inline fract32 opz_sub_q31(fract32 a, fract32 b) { return opz_sat32((int64_t)a - b); }
static inline fract32 opz_shl_q31(fract32 x, int n) { return opz_sat32((int64_t)x * ((int64_t)1 << n)); }
static inline fract32 opz_mul_q31_q15(fract32 x, int32_t c) { return opz_sat32(((int64_t)x * (int16_t)c) >> 15); }
static inline int opz_norm32(int32_t x)
{
    if (x == 0 || x == -1) return 31;
    return __builtin_clz((uint32_t)(x < 0 ? ~x : x)) - 1;
}
#endif

/* a * b of the low 16 bits of each, unsigned (one Blackfin instruction). */
static inline uint32_t opz_mul_u16(uint32_t a, uint32_t b) { return (uint32_t)(uint16_t)a * (uint16_t)b; }

#ifdef __cplusplus
}
#endif

#endif /* OPZ_ENGINE_H */
