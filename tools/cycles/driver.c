/*
 * driver.c — bare-metal cycle benchmark for one OP-Z engine, run under the GNU Blackfin simulator.
 *
 * tools/opz_cycles.py builds the engine with tools/opz_build.py (same compiler, flags and slot), copies its image
 * byte for byte to the slot base (image.c, section .engine_image) and generates bench.h with the cases. Like the
 * OS, the driver finds the entry points through the export table at the slot base, calls initModule once, then
 * per case init + onNoteOn for every voice and render/renderFX for N blocks, and reads the CYCLES register around
 * each call. Results go to stdout through the simulator's libgloss write call, one "R" line per case.
 *
 * The host side (ADSR callback, voice handling) is our own model from sim/src/callbacks.c, kept integer-only. Its
 * cycles count inside render, as TE's callback does on the device; they are also reported separately (cb=).
 * With BENCH_DUMP (opz_cycles.py --dump) every rendered block is also printed in hex ("S" lines, outside the timed
 * calls), so that two builds of an engine can be null-tested sample by sample.
 *
 * Crash checks (research/crash-safety.md): every engine call goes through bench_call (checks.S) on a painted
 * stack of its own per entry point, with L0-L3/LC0/LC1 = 0 and known callee-saved registers, as the OS's interrupt
 * entry leaves them. After each call the driver checks the registers, the guards after the voice state (the
 * device's 280/204 bytes), the output buffer, the envelope and the FX buffer, and that params and the FX input are
 * unchanged; after each case the engine's code and constants, the slot window past its .bss and the scratch
 * buffer. Problems are "F" lines, the stack and scratch high-water marks one "K" line at the end. The simulator
 * itself stops on misaligned accesses, the NULL page (opz_cycles.py unmaps it), unmapped memory and illegal
 * instructions; the "C" line before each case says which case was running.
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_engine.h"

#define MAX_VOICES 24
#define Q31_ONE    0x7fffffff
#define DRUM_PITCH_WORD 1024        /* drum tracks: the fixed unity rate (1024 x tune >> 30) */

typedef struct BenchFill { uint32_t addr, len; } BenchFill;

typedef struct BenchCase {
    uint8_t params[OPZ_NUM_PARAMS];   /* parameter bytes, as the knobs send them */
    uint8_t track;
    uint8_t velocity;
    uint8_t nvoices;                  /* instruments: voices rendering into one track buffer; FX: 1 */
    uint8_t notes[MAX_VOICES];        /* one note per voice; FX: note on the FX track, 255 = none */
    uint32_t pitch[MAX_VOICES];       /* render's pitch word (drum tracks: the fixed unity rate) */
    uint16_t blocks;                  /* blocks to render */
    uint16_t warmup;                  /* blocks left out of the mean (FX delay-line clearing) */
    uint16_t release_at;              /* block at which note-off is sent (0 = never) */
    uint16_t p1_to;                   /* from block `warmup` on, P1 moves to this byte, smoothed as the host does
                                         (0xffff = P1 stays) */
    uint16_t knobs;                   /* > 0: from block `warmup` on, P1 and P2 sweep 0..255..0 as triangles with
                                         periods of `knobs` and 3*knobs/2+1 blocks, a new value every block while
                                         the notes play (a fast hand on both knobs; the macro v3 crash) */
    int16_t glide;                    /* != 0: from block `warmup` on, every voice's pitch changes by glide / 4096 of
                                         itself per block, passed to render as a pitch slope (portamento, vibrato) */
    uint16_t glide_flip;              /* > 0: the glide changes direction every glide_flip blocks */
    uint16_t resend;                  /* FX, > 0: from block `warmup` on, every `resend` blocks the next note of
                                         `notes` arrives in turn (with `velocity`), and halfway between, the
                                         current one comes back as the OS re-sends a held note:
                                         onNoteOn(note, 0, state, NULL).
                                         Instruments, > 0: from block `warmup` on, every `resend` blocks the next
                                         voice in turn is struck again with its note (fast retriggers): a busy voice
                                         is faded first (stage 5) and the note starts once the voice is idle, as the
                                         device does */
    uint8_t raw;                      /* 1: params[0] and params[1] are p_raw[], not knob values (any 32-bit word,
                                         e.g. outside the knob range, which OS 1.2.45 never sends: Q-23) */
    uint32_t p_raw[2];
    uint16_t stream_rows;             /* > 0: from block `warmup` on, block b takes all 18 params from row
                                         (b - warmup) % stream_rows of `stream` (the OS's parameter path as
                                         tools/opz_params.py models it: LFO, smoothing, note-on latches), set before
                                         the block's note-ons; words 18-19 of the row: instruments, if not both 0,
                                         the pitch at the block's start and end (pitchSlope = their difference
                                         >> 6, as the OS), voice v's lowered by v/32 so that voices don't add up
                                         in phase; FX, renderFX's pitch bend */
    const uint32_t *stream;           /* stream_rows x STREAM_WORDS words */
} BenchCase;

#define STREAM_WORDS (OPZ_NUM_PARAMS + 2)

static uint8_t knob_triangle(int b, int period)
{
    int t = b % period, half = period / 2;
    if (half < 1) half = 1;
    int v = t <= half ? t * 255 / half : (period - t) * 255 / (period - half);
    return (uint8_t)(v > 255 ? 255 : v);
}

#include "bench.h"                    /* ENGINE_BASE, ENGINE_IS_FX, bench_fills, bench_cases, ADSR and OS tables */

/* ---- simulator I/O (libgloss system calls: EXCPT 0, P0 = number, R0 -> argument block) ---------------- */

static int sim_syscall(int nr, uint32_t *args)
{
    register int r0 __asm__("R0") = (int)args;
    register int p0 __asm__("P0") = nr;
    /* The simulator's handler writes more than R0 (R2 at least: --dump printed every word after a mid-line flush as
     * zeros, the hex digit mask was gone), so everything a call may clobber is declared clobbered. */
    __asm__ volatile("EXCPT 0;" : "+r"(r0), "+r"(p0) : : "R1", "R2", "R3", "P1", "P2", "cc", "memory");
    return r0;
}

static void put_bytes(const char *s, uint32_t n)
{
    uint32_t args[3] = { 1, (uint32_t)s, n };
    sim_syscall(5 /* write */, args);
}

void bench_exit(int code)
{
    uint32_t args[1] = { (uint32_t)code };
    sim_syscall(1 /* exit */, args);
}

static char s_line[512];
static uint32_t s_len;

static void out_flush(void) { put_bytes(s_line, s_len); s_len = 0; }

static void out_str(const char *s)
{
    while (*s) {
        if (s_len == sizeof s_line) out_flush();
        s_line[s_len++] = *s++;
    }
}

static void out_u64(uint64_t v)
{
    char buf[24];
    int i = sizeof buf - 1;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out_str(buf + i);
}

#ifndef BENCH_DUMP
#define BENCH_DUMP 0
#endif

/* One block of out as "S" + 128 eight-digit hex words (two's complement). */
static void out_block(const fract32 *buf)
{
    static const char hex[] = "0123456789abcdef";
    out_str("S ");
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
        uint32_t v = (uint32_t)buf[i];
        if (s_len + 9 > sizeof s_line) out_flush();
        for (int k = 28; k >= 0; k -= 4) s_line[s_len++] = hex[(v >> k) & 15];
    }
    out_str("\n");
    out_flush();
}

static void out_kv(const char *k, uint64_t v) { out_str(" "); out_str(k); out_str("="); out_u64(v); }

/* ---- cycle counter ----------------------------------------------------------------------------------- */

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint32_t s_overhead;           /* cost of one back-to-back read, subtracted from every measurement */

typedef struct Stat { uint32_t n, min, max; uint64_t sum; } Stat;

static void stat_reset(Stat *s) { s->n = 0; s->min = 0xffffffffu; s->max = 0; s->sum = 0; }
static void stat_add(Stat *s, uint32_t v) { s->n++; s->sum += v; if (v < s->min) s->min = v; if (v > s->max) s->max = v; }

static void out_stat(const char *prefix, const Stat *s)
{
    char k[24];
    const char *tail[4] = { "_n", "_min", "_max", "_sum" };
    uint64_t val[4] = { s->n, s->n ? s->min : 0, s->max, s->sum };
    for (int t = 0; t < 4; t++) {
        int i = 0;
        for (const char *p = prefix; *p && i < 16; p++) k[i++] = *p;
        for (const char *p = tail[t]; *p; p++) k[i++] = *p;
        k[i] = 0;
        out_kv(k, val[t]);
    }
}

/* ---- host model: envelope callback (integer port of sim/src/callbacks.c) ----------------------------- */

typedef struct Env {
    int32_t attack_step, level, coef, step, stage;
    uint8_t new_note, release, pad, gate;
} Env;

enum { ST_IDLE = 0, ST_ATTACK = 1, ST_DECAY = 2, ST_SUSTAIN = 3, ST_RELEASE = 4, ST_FADE = 5, ST_FADE2 = 6,
       ST_HOLD = 7 };
#define ENV_THRESHOLD   0x0010624E
#define ENV_ATTACK_DONE 0x7EB85200
#define ENV_IDLE_COEF   0x70000000

static uint32_t s_cb_cycles;

static int32_t fade_limit(int32_t level) { return (int32_t)(((uint64_t)(uint32_t)level * 0x4D7A7880u) >> 37); }

static void update_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    uint32_t t0 = cycles();
    Env *e = (Env *)env;
    int32_t S = (int32_t)(s & 0x7fffffff);
    int32_t decay = bench_decay_coef[(d >> 24) & 127], rel = bench_decay_coef[(r >> 24) & 127];
    if (decay < bench_decay_coef[1]) decay = bench_decay_coef[1];
    e->attack_step = bench_attack_step[(a >> 24) & 127];
    if (e->release) {
        if (e->stage != ST_IDLE) { e->coef = rel; e->step = 0; e->stage = ST_RELEASE; }
        e->release = 0;
    }
    switch (e->stage) {
    case ST_ATTACK:
        if (e->level < ENV_ATTACK_DONE) { e->coef = Q31_ONE; e->step = e->attack_step; goto done; }
        e->stage = ST_DECAY;
        /* fall through */
    case ST_DECAY:
        if (e->level > S) { e->coef = decay; e->step = 0; }
        else e->stage = ST_SUSTAIN;
        goto done;
    case ST_SUSTAIN:
        e->coef = Q31_ONE;
        e->step = (int32_t)(((int64_t)S - e->level) >> 8);
        goto done;
    case ST_RELEASE:
        if (e->level >= ENV_THRESHOLD) { e->coef = rel; e->step = 0; goto done; }
        break;
    case ST_FADE:
    case ST_FADE2:
        if (e->level >= ENV_THRESHOLD) {
            int32_t lim = fade_limit(e->level);
            int32_t st = e->step == INT32_MIN ? Q31_ONE : (e->step < 0 ? -e->step : e->step);
            e->step = -(st < lim ? st : lim);
            goto done;
        }
        break;
    default:
        goto done;
    }
    e->stage = ST_IDLE;
    e->coef = ENV_IDLE_COEF;
    e->step = 0;
done:
    s_cb_cycles += cycles() - t0;
}

/* Q31 product, saturating at the one overflowing case (-1 * -1), truncated (opzsim's mul_q31_trunc). */
static int32_t mul_q31_trunc(int64_t a, int64_t b)
{
    int64_t p = a * b;
    return p == ((int64_t)1 << 62) ? Q31_ONE : (int32_t)(p >> 31);
}

/* The drum envelope (updateParametersAHD): an integer port of update_ahd in sim/src/callbacks.c, stage for stage.
 * pos and the landmarks are positions along the sound (engine-abi.md §3); note-off is ignored. */
static void update_ahd(ADSR *env, uint32_t pos, uint32_t ae, uint32_t he, uint32_t de, fract32 coef, fract32 shape)
{
    uint32_t t0 = cycles();
    Env *e = (Env *)env;
    switch (e->stage) {
    case ST_ATTACK:
        if (ae > pos) { e->coef = Q31_ONE; e->step = (int32_t)(0x7fffffffu / (ae - pos)); goto done; }
        e->stage = ST_HOLD;
        /* fall through */
    case ST_HOLD:
        if (pos < he) { e->level = Q31_ONE; e->coef = Q31_ONE; e->step = 0; goto done; }
        e->stage = ST_RELEASE;
        /* fall through */
    case ST_RELEASE: {
        if (de <= pos) break;
        uint32_t left = de - pos;
        int32_t span = (int32_t)(de - he);
        int64_t f = (int64_t)(int32_t)left * ((int64_t)1 << 31) / (span ? span : 1);
        if (f > Q31_ONE) f = Q31_ONE;
        if (f < INT32_MIN) f = INT32_MIN;
        int32_t t = mul_q31_trunc(mul_q31_trunc(mul_q31_trunc(f, shape), shape), shape);
        int64_t d = (int64_t)Q31_ONE - t;
        uint32_t u = (uint32_t)(d > Q31_ONE ? Q31_ONE : d < INT32_MIN ? INT32_MIN : d);
        uint64_t u4 = (uint64_t)u * u >> 31;
        u4 = (u4 * u) >> 31;
        u4 = (uint32_t)((u4 * u) >> 31);
        int32_t lin = -(e->level / (int32_t)(left > OPZ_BLOCK_FRAMES ? left : OPZ_BLOCK_FRAMES));
        e->coef = coef;
        e->step = (int32_t)(((int64_t)u4 * lin) >> 31);
        goto done;
    }
    case ST_FADE:
        if (e->level >= ENV_THRESHOLD) {
            int32_t lim = fade_limit(e->level);
            int32_t st = e->step == INT32_MIN ? Q31_ONE : (e->step < 0 ? -e->step : e->step);
            e->step = -(st < lim ? st : lim);
            goto done;
        }
        break;
    case ST_FADE2:
        e->coef = Q31_ONE;
        if (e->level >= ENV_THRESHOLD) { e->step = -fade_limit(e->level); goto done; }
        break;
    default:                                    /* idle; 2 and 3 are not used by drums */
        goto done;
    }
    e->stage = ST_IDLE;
    e->coef = ENV_IDLE_COEF;
    e->step = 0;
done:
    s_cb_cycles += cycles() - t0;
}

static void seq_set(int key, int value) { (void)key; (void)value; }
static int seq_get(int key) { return (key & 0xffff) == OPZ_SEQ_GET_TEMPO ? 120 : 0; }

/* The OS's five shared tables in the device formats, generated by tools/opz_tables.py into bench.h. They sit in
 * the driver's memory, not at the OS's addresses (L1/L2/DDR): the simulator does not model memory timing. */
static const fract32 *const s_rf[5] = { bench_rf[0], bench_rf[1], bench_rf[2], bench_rf[3], bench_rf[4] };

/* getSample for a sample engine (plug.json `samples`): init gets BENCH_PACK_ID = crc32 of that path as arg2/mem, and
 * every key of that id is one fake slice of half a second, a full-scale triangle at unity rate and volume, as a drum
 * key of a TE pack looks (samples.md §5, §8). The buffer is ours, not at the OS's cache address (0x82700000), and the
 * simulator does not model SDRAM timing: count the reads in the cycle model. Other ids: NULL. */
#define BENCH_PACK_FRAMES 22050
static int16_t s_pack_pcm[BENCH_PACK_FRAMES];
static OpzSampleSlice s_pack_slice;

static void pack_init(void)
{
    for (uint32_t i = 0; i < BENCH_PACK_FRAMES; i++) {
        uint32_t t = (i * 1311u) & 0xffffu;                 /* ~880 Hz */
        s_pack_pcm[i] = (int16_t)((t < 32768u ? (int32_t)t * 2 : (int32_t)(65535u - t) * 2) - 32767);
    }
    s_pack_slice.data = s_pack_pcm;
    s_pack_slice.frames = BENCH_PACK_FRAMES;
    s_pack_slice.knobs = 8192u | 64u << 16 | 64u << 24;      /* volume 8192, playmode / reverse 8192 */
    s_pack_slice.rate = OPZ_SLICE_RATE_UNITY;
}

static const void *get_sample(int packId, uint8_t note)
{
    (void)note;
    return BENCH_PACK_ID && (uint32_t)packId == BENCH_PACK_ID ? &s_pack_slice : 0;
}

static const ModuleCallbackInterface s_cb = {
    seq_set, seq_get, bench_k_lp_table, bench_g_sine, bench_g_sineTable, s_rf, bench_releaseTable,
    update_adsr, update_ahd, get_sample, 0, 0, 0,
};

/* ---- engine entry points, found through the export table like the OS loader does ------------------------ */

typedef uint32_t (*InitModuleFn)(uint32_t, const ModuleCallbackInterface *, uint32_t *);
typedef void (*InitFn)(void *, uint32_t, uint32_t, void *, void *);
typedef void (*NoteOnFn)(int, uint8_t, void *, const uint32_t *);
typedef void (*NoteOffFn)(int, void *);
typedef void (*RenderFn)(fract32 *, uint32_t, int32_t, void *, const uint32_t *, ADSR *);
typedef void (*RenderFxFn)(const uint32_t *, fract32, void *, const fract32 *, fract32 *, fract32);

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const void *find_export(const char *name)
{
    const uint32_t *t = (const uint32_t *)ENGINE_BASE;
    if (t[0] != OPZ_EXPORT_MAGIC0 || t[1] != OPZ_EXPORT_MAGIC1) return 0;
    for (t += 2; t[0]; t += 2)
        if (str_eq((const char *)t[0], name)) return (const void *)t[1];
    return 0;
}

static InitModuleFn f_init_module;
static InitFn f_init;
static NoteOnFn f_note_on;
static NoteOffFn f_note_off;
static RenderFn f_render;
static RenderFxFn f_render_fx;

/* ---- buffers ---------------------------------------------------------------------------------------- */

#define GUARD_WORDS 16
#define GUARD_PAINT 0xa5c3e1f0u
#define STATE_WORDS (BENCH_STATE_LIMIT / 4)    /* the device's state area: 280 bytes (voice), 204 (FX) */
#define TORN MAX_VOICES                 /* state and envelope slot of the renders on half-written states */
static uint32_t s_state[MAX_VOICES + 1][STATE_WORDS + GUARD_WORDS];
static struct { Env e; uint32_t guard[GUARD_WORDS]; } s_envs[MAX_VOICES + 1];
static uint32_t s_pitch[MAX_VOICES];
static uint8_t s_retrigger[MAX_VOICES];        /* instruments: a retrigger waits for the voice's fade */
static struct { uint32_t pre[GUARD_WORDS]; fract32 v[2 * OPZ_BLOCK_FRAMES]; uint32_t post[GUARD_WORDS]; } s_outb;
#define s_out (s_outb.v)
static fract32 s_in[2 * OPZ_BLOCK_FRAMES], s_in_copy[2 * OPZ_BLOCK_FRAMES];   /* FX input and its copy */
static uint32_t s_params[OPZ_NUM_PARAMS], s_params_copy[OPZ_NUM_PARAMS];
/* FX large buffer (memRequest), as big as FX1's pool: 2 MB at the bottom of the slot region that opz_cycles.py maps,
 * well below the first slot window (0x81B00000). The simulator's memory starts zeroed. */
#define FX_MEM ((void *)0x81000000u)
#define FX_MEM_BYTES OPZ_FX1_POOL_BYTES
static uint32_t s_scratch[1024 / 4];
#define SCRATCH_PAINT 0x3c3c5a5au
static uint32_t s_seed = 1;

static void zero(void *p, uint32_t n)
{
    volatile uint8_t *b = p;
    while (n--) *b++ = 0;
}

static void fill_input(int block)
{
    /* -12 dBFS saw at ~345 Hz plus a little noise, interleaved stereo */
    for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
        uint32_t ph = (uint32_t)(block * OPZ_BLOCK_FRAMES + i) * 33554432u;
        s_seed = s_seed * 1664525u + 1013904223u;
        int32_t x = ((int32_t)ph >> 2) + ((int32_t)s_seed >> 6);
        s_in[2 * i] = x;
        s_in[2 * i + 1] = -x;
    }
}

/* ---- crash checks ------------------------------------------------------------------------------------- */

typedef struct CallRegs { uint32_t ret, l[4], lc[2], saved[8], sp; } CallRegs;   /* filled by checks.S */
extern CallRegs bench_regs;
uint32_t bench_call(const void *fn, const uint32_t *args, uint32_t *stack_top);
void bench_nop(void);

enum { E_MODULE, E_INIT, E_ON, E_OFF, E_RENDER, E_COUNT, E_CASE = E_COUNT };   /* E_CASE: found after a case */
enum {                                  /* finding codes, named in tools/opz_cycles.py FINDINGS */
    F_LREG = 1, F_LC, F_CALLEE, F_SP, F_STATE, F_OUT, F_ENV, F_PARAMS, F_INPUT, F_FXMEM, F_CODE, F_TAIL,
    F_STACK, F_SCRATCH
};
#define STACK_WORDS 2048                /* 8 KB per entry point */
#define STACK_PAINT 0x5ca1ab1eu
static uint32_t s_stack[E_COUNT][STACK_WORDS];
static uint32_t s_call_overhead[E_COUNT];
static uint32_t s_case = 0xffff;        /* 0xffff while initModule runs */
static uint32_t s_reported[E_COUNT + 1];
static uint32_t s_fx_guard;             /* FX buffer guard address (FX_MEM + memRequest) */
static uint32_t s_ro_sum, s_scratch_max;

static const uint32_t k_saved[8] = {    /* R4-R7, P3-P5, FP as checks.S sets them */
    0x0bad0004u, 0x0bad0005u, 0x0bad0006u, 0x0bad0007u, 0x0bad0013u, 0x0bad0014u, 0x0bad0015u, 0x0bad00ffu
};

static void finding(int code, int entry, uint32_t a, uint32_t b)
{
    if (s_reported[entry] & (1u << code)) return;          /* once per case, code and entry point */
    s_reported[entry] |= 1u << code;
    out_str("F");
    out_kv("case", s_case);
    out_kv("code", (uint64_t)code);
    out_kv("entry", (uint64_t)entry);
    out_kv("a", a);
    out_kv("b", b);
    out_str("\n");
    out_flush();
}

static void paint(uint32_t *p, uint32_t words, uint32_t v) { while (words--) *p++ = v; }

static int first_changed(const uint32_t *p, uint32_t words, uint32_t v)
{
    for (uint32_t i = 0; i < words; i++)
        if (p[i] != v) return (int)i;
    return -1;
}

static void check_guards(int e, int voice, int fx_call)
{
    int i;
    if (voice >= 0 && (i = first_changed(&s_state[voice][STATE_WORDS], GUARD_WORDS, GUARD_PAINT)) >= 0)
        finding(F_STATE, e, (uint32_t)voice, (uint32_t)(BENCH_STATE_LIMIT + 4 * i));
    if (voice >= 0 && !ENGINE_IS_FX && (i = first_changed(s_envs[voice].guard, GUARD_WORDS, GUARD_PAINT)) >= 0)
        finding(F_ENV, e, (uint32_t)voice, (uint32_t)(sizeof(Env) + 4 * i));
    if ((i = first_changed(s_outb.pre, GUARD_WORDS, GUARD_PAINT)) >= 0) finding(F_OUT, e, 0, (uint32_t)i);
    if ((i = first_changed(s_outb.post, GUARD_WORDS, GUARD_PAINT)) >= 0) finding(F_OUT, e, 1, (uint32_t)i);
    for (i = 0; i < OPZ_NUM_PARAMS; i++)
        if (s_params[i] != s_params_copy[i]) { finding(F_PARAMS, e, (uint32_t)i, s_params[i]); break; }
    if (fx_call) {
        for (i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++)
            if (s_in[i] != s_in_copy[i]) { finding(F_INPUT, e, (uint32_t)i, 0); break; }
        if ((i = first_changed((const uint32_t *)s_fx_guard, GUARD_WORDS, GUARD_PAINT)) >= 0)
            finding(F_FXMEM, e, (uint32_t)(4 * i), 0);
    }
}

/* One engine call through checks.S. dt (if given) gets the cycles of the call itself (call, body, return): the
 * trampoline's own cost is calibrated out (s_call_overhead, main). Before the checks the timed region also held
 * the argument set-up of a direct call, a few cycles more. */
static uint32_t call_engine(int e, const void *fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, int voice, uint32_t *dt)
{
    uint32_t args[6] = { a0, a1, a2, a3, a4, a5 };
    uint32_t *top = s_stack[e] + STACK_WORDS;
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params_copy[i] = s_params[i];
    uint32_t t0 = cycles();
    uint32_t r = bench_call(fn, args, top);
    uint32_t t = cycles() - t0 - s_call_overhead[e];
    if (dt) *dt = t;
    for (int i = 0; i < 4; i++)
        if (bench_regs.l[i]) finding(F_LREG, e, (uint32_t)i, bench_regs.l[i]);
    for (int i = 0; i < 2; i++)
        if (bench_regs.lc[i]) finding(F_LC, e, (uint32_t)i, bench_regs.lc[i]);
    for (int i = 0; i < 8; i++)
        if (bench_regs.saved[i] != k_saved[i]) finding(F_CALLEE, e, (uint32_t)i, bench_regs.saved[i]);
    if (bench_regs.sp != (uint32_t)top - 24) finding(F_SP, e, bench_regs.sp, (uint32_t)top - 24);
    check_guards(e, voice, ENGINE_IS_FX && e == E_RENDER);
    return r;
}

static uint32_t ro_sum(void)            /* the engine's export table, code and constants (bench_ro in bench.h) */
{
    uint32_t h = 0;
    for (unsigned k = 0; k < sizeof bench_ro / sizeof bench_ro[0]; k++) {
        const uint32_t *p = (const uint32_t *)bench_ro[k].addr;
        for (uint32_t i = 0; i < bench_ro[k].len / 4; i++) h = ((h << 5) | (h >> 27)) ^ p[i];
    }
    return h;
}

static void check_case(void)
{
    int i;
    if (ro_sum() != s_ro_sum) { finding(F_CODE, E_CASE, 0, 0); s_ro_sum = ro_sum(); }
    if (BENCH_TAIL_WORDS && (i = first_changed((const uint32_t *)BENCH_TAIL_START, BENCH_TAIL_WORDS, GUARD_PAINT)) >= 0) {
        finding(F_TAIL, E_CASE, BENCH_TAIL_START + 4 * (uint32_t)i, 0);
        paint((uint32_t *)BENCH_TAIL_START, BENCH_TAIL_WORDS, GUARD_PAINT);
    }
    uint32_t used = 0;
    for (i = sizeof s_scratch / 4 - 1; i >= 0; i--)
        if (s_scratch[i] != SCRATCH_PAINT) { used = 4 * (uint32_t)(i + 1); break; }
    if (used > s_scratch_max) s_scratch_max = used;
    if (used > 256) finding(F_SCRATCH, E_CASE, used, 0);
}

static void report_stacks(void)
{
    static const char *const key[E_COUNT] = { "module", "init", "on", "off", "render" };
    out_str("K");
    for (int e = 0; e < E_COUNT; e++) {
        int i = first_changed(s_stack[e], STACK_WORDS, STACK_PAINT);
        uint32_t below = i < 0 ? 0 : 4 * (STACK_WORDS - (uint32_t)i);   /* bytes from the top to the deepest write */
        uint32_t used = below > 24 ? below - 24 : 0;                     /* minus checks.S's argument frame */
        if (i == 0) finding(F_STACK, e, used, 0);
        out_kv(key[e], used);
    }
    out_kv("scratch", s_scratch_max);
    out_str("\n");
    out_flush();
}

/* ---- renders on half-written states ---------------------------------------------------------------------- */

/* The OS starts a voice (envelope stage 1) before it calls onNoteOn, from the sequencer or UI task, while render
 * runs in the audio interrupt (opz.ldr 0x8012e26c-0x8012e280; research/crash-safety.md). So render can see a
 * state that onNoteOn has only partly written. With BENCH_TORN, every note-on that starts a case (voice 0) and a
 * re-trigger at its end are followed by renders on mixes of the state before and after it: the changed words in
 * address order as a prefix or a suffix, all but one, and only one. A crash in one of them stops the simulator
 * between the "T" line and the "U" line. */
#ifndef BENCH_TORN
#define BENCH_TORN 0
#endif
static uint32_t s_before[STATE_WORDS];

static void torn_render(const BenchCase *c)
{
    uint32_t *st = s_state[TORN];
    if (ENGINE_IS_FX) {
        call_engine(E_RENDER, f_render_fx, (uint32_t)s_params, 0, (uint32_t)st, (uint32_t)s_in, (uint32_t)s_out,
                    Q31_ONE, TORN, 0);
        return;
    }
    Env *e = &s_envs[TORN].e;
    zero(e, sizeof *e);
    e->stage = ST_ATTACK;
    e->coef = Q31_ONE;
    e->attack_step = bench_attack_step[(s_params[OPZ_P_ATTACK] >> 24) & 127];
    e->step = e->attack_step;
    e->new_note = 1;
    e->gate = 1;
    paint(s_envs[TORN].guard, GUARD_WORDS, GUARD_PAINT);
    zero(s_out, sizeof s_out);
    call_engine(E_RENDER, f_render, (uint32_t)s_out, c->pitch[0] ? c->pitch[0] : DRUM_PITCH_WORD, 0, (uint32_t)st,
                (uint32_t)s_params, (uint32_t)e, TORN, 0);
}

static void torn_states(const BenchCase *c, const uint32_t *after, int phase)
{
    int idx[STATE_WORDS], n = 0;
    for (int i = 0; i < STATE_WORDS; i++)
        if (s_before[i] != after[i]) idx[n++] = i;
    out_str("T");
    out_kv("case", s_case);
    out_kv("phase", (uint64_t)phase);
    out_kv("words", (uint64_t)n);
    out_str("\n");
    out_flush();
    for (int mode = 0; mode < 4; mode++)
        for (int k = 0; k < n; k++) {
            uint32_t *st = s_state[TORN];
            for (int i = 0; i < STATE_WORDS; i++) st[i] = mode == 2 ? after[i] : s_before[i];
            for (int j = 0; j < n; j++) {
                int use_after = mode == 0 ? j <= k : mode == 1 ? j >= k : mode == 2 ? j != k : j == k;
                st[idx[j]] = use_after ? after[idx[j]] : s_before[idx[j]];
            }
            paint(&st[STATE_WORDS], GUARD_WORDS, GUARD_PAINT);
            torn_render(c);
        }
    out_str("U\n");                     /* the torn renders are over */
    out_flush();
}

/* ---- one case ---------------------------------------------------------------------------------------- */

static void run_case(int idx, const BenchCase *c)
{
    Stat render_all, render_steady, block_total, note_on, note_off;
    stat_reset(&render_all); stat_reset(&render_steady); stat_reset(&block_total);
    stat_reset(&note_on); stat_reset(&note_off);
    uint32_t active_blocks = 0, last_active = 0, dt;

    s_case = (uint32_t)idx;
    for (int e = 0; e <= E_COUNT; e++) s_reported[e] = 0;
    out_str("C");
    out_kv("case", (uint64_t)idx);
    out_str("\n");
    out_flush();
    for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = opz_param_from_byte(c->params[i]);
    if (c->raw) { s_params[OPZ_P_P1] = c->p_raw[0]; s_params[OPZ_P_P2] = c->p_raw[1]; }
    int nv = ENGINE_IS_FX ? 1 : c->nvoices;
    s_cb_cycles = 0;
    paint(s_scratch, sizeof s_scratch / 4, SCRATCH_PAINT);
    zero(s_retrigger, sizeof s_retrigger);

    for (int v = 0; v < nv; v++) {
        Env *e = &s_envs[v].e;
        zero(s_state[v], 4 * STATE_WORDS);
        paint(&s_state[v][STATE_WORDS], GUARD_WORDS, GUARD_PAINT);
        zero(e, sizeof *e);
        paint(s_envs[v].guard, GUARD_WORDS, GUARD_PAINT);
        call_engine(E_INIT, f_init, (uint32_t)s_state[v], c->track, ENGINE_IS_FX ? 0 : BENCH_PACK_ID,
                    ENGINE_IS_FX ? (uint32_t)FX_MEM : BENCH_PACK_ID, (uint32_t)s_scratch, 0, v, 0);
        if (!ENGINE_IS_FX) {
            e->stage = ST_ATTACK;                       /* host note-on: free voice -> level 0, attack */
            e->coef = Q31_ONE;
            e->attack_step = bench_attack_step[(s_params[OPZ_P_ATTACK] >> 24) & 127];
            e->step = e->attack_step;
            e->new_note = 1;
            e->gate = 1;
        }
        if (c->notes[v] != 255) {
            if (BENCH_TORN && v == 0) for (int i = 0; i < STATE_WORDS; i++) s_before[i] = s_state[0][i];
            /* velocity 0: the OS re-sending a held note to an FX track, with no parameter pointer */
            call_engine(E_ON, f_note_on, c->notes[v], c->velocity, (uint32_t)s_state[v],
                        (uint32_t)(c->velocity ? s_params : 0), 0, 0, v, &dt);
            stat_add(&note_on, dt);
            if (BENCH_TORN && v == 0) torn_states(c, s_state[0], 0);
        }
    }

    for (int v = 0; v < nv; v++) s_pitch[v] = c->pitch[v];
    int glide = c->glide;
    for (int b = 0; b < c->blocks; b++) {
        const uint32_t *row = c->stream_rows && b >= c->warmup
            ? c->stream + (uint32_t)((b - c->warmup) % c->stream_rows) * STREAM_WORDS : 0;
        if (row) for (int i = 0; i < OPZ_NUM_PARAMS; i++) s_params[i] = row[i];
        if (c->glide_flip && b > c->warmup && (b - c->warmup) % c->glide_flip == 0) glide = -glide;
        if (c->p1_to != 0xffff && b >= c->warmup)
            s_params[OPZ_P_P1] += (uint32_t)((int32_t)(opz_param_from_byte((uint8_t)c->p1_to) - s_params[OPZ_P_P1]) >> 6);
        if (c->knobs && b >= c->warmup) {
            s_params[OPZ_P_P1] = opz_param_from_byte(knob_triangle(b - c->warmup, c->knobs));
            s_params[OPZ_P_P2] = opz_param_from_byte(knob_triangle(b - c->warmup + c->knobs / 3, 3 * c->knobs / 2 + 1));
        }
        if (ENGINE_IS_FX && c->resend && b >= c->warmup && c->nvoices) {
            int k = (b - c->warmup) % c->resend, n = c->notes[((b - c->warmup) / c->resend) % c->nvoices];
            if (k == 0 || k == c->resend / 2) {
                if (k == 0) call_engine(E_ON, f_note_on, n, c->velocity, (uint32_t)s_state[0], (uint32_t)s_params, 0, 0, 0, &dt);
                else call_engine(E_ON, f_note_on, n, 0, (uint32_t)s_state[0], 0, 0, 0, 0, &dt);
                stat_add(&note_on, dt);
            }
        }
        if (!ENGINE_IS_FX && c->resend && b >= c->warmup && (b - c->warmup) % c->resend == 0) {
            int v = ((b - c->warmup) / c->resend) % nv;
            Env *e = &s_envs[v].e;
            if (e->stage != ST_IDLE) {                  /* Voice::noteOn on a busy voice: fade it (as opzsim) */
                e->stage = ST_FADE;
                e->coef = 0x7f000000;
                e->step = -(int32_t)(((int64_t)(e->level < 0 ? 0 : e->level) * 8388607) >> 31);
                e->release = 0;
            }
            s_retrigger[v] = 1;
        }
        for (int v = 0; v < nv && !ENGINE_IS_FX; v++) {
            if (!s_retrigger[v] || s_envs[v].e.stage != ST_IDLE) continue;
            Env *e = &s_envs[v].e;                         /* the voice is free: host note-on, then the engine's */
            zero(e, sizeof *e);
            e->stage = ST_ATTACK;
            e->coef = Q31_ONE;
            e->attack_step = bench_attack_step[(s_params[OPZ_P_ATTACK] >> 24) & 127];
            e->step = e->attack_step;
            e->new_note = 1;
            e->gate = 1;
            uint32_t t0 = cycles();
            f_note_on(c->notes[v], c->velocity, s_state[v], s_params);
            stat_add(&note_on, cycles() - t0 - s_overhead);
            s_retrigger[v] = 0;
        }
        if (c->release_at && b == c->release_at) {
            for (int v = 0; v < nv; v++) {
                if (!ENGINE_IS_FX) s_envs[v].e.release = 1;
                call_engine(E_OFF, f_note_off, c->notes[v], (uint32_t)s_state[v], 0, 0, 0, 0, v, &dt);
                stat_add(&note_off, dt);
            }
        }
        uint32_t total = 0;
        int any = 0;
        if (ENGINE_IS_FX) {
            fill_input(b);
            for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) s_in_copy[i] = s_in[i];
            call_engine(E_RENDER, f_render_fx, (uint32_t)s_params, row ? row[OPZ_NUM_PARAMS] : 0,
                        (uint32_t)s_state[0], (uint32_t)s_in, (uint32_t)s_out, Q31_ONE, 0, &dt);
            stat_add(&render_all, dt);
            if (b >= c->warmup) stat_add(&render_steady, dt);
            total = dt;
            any = 1;
        } else {
            zero(s_out, sizeof s_out);
            for (int v = 0; v < nv; v++) {
                if (s_envs[v].e.stage == ST_IDLE) continue;   /* the host skips free voices */
                uint32_t p = s_pitch[v];
                int32_t slope = 0;
                if (glide && b >= c->warmup) {
                    uint32_t p_end = p + (uint32_t)((int32_t)(p >> 12) * glide);
                    slope = (int32_t)(p_end - p) >> 6;
                    s_pitch[v] = p_end;
                }
                if (row && (row[OPZ_NUM_PARAMS] | row[OPZ_NUM_PARAMS + 1])) {
                    uint32_t p0 = row[OPZ_NUM_PARAMS], p1 = row[OPZ_NUM_PARAMS + 1];
                    p = p0 - (uint32_t)v * (p0 >> 5);
                    slope = (int32_t)(p1 - (uint32_t)v * (p1 >> 5) - p) >> 6;
                }
                call_engine(E_RENDER, f_render, (uint32_t)s_out, p, (uint32_t)slope, (uint32_t)s_state[v],
                            (uint32_t)s_params, (uint32_t)&s_envs[v].e, v, &dt);
                stat_add(&render_all, dt);
                if (b >= c->warmup) stat_add(&render_steady, dt);
                total += dt;
                any = 1;
            }
        }
        if (!any) {                                    /* every voice has ended */
            if (!ENGINE_IS_FX && c->resend) continue;  /* until the next retrigger */
            break;
        }
        if (BENCH_DUMP) out_block(s_out);
        active_blocks++;
        last_active = (uint32_t)b;
        stat_add(&block_total, total);
    }
    if (BENCH_TORN && c->velocity) {        /* a new note on a voice that has been playing (re-trigger, steal) */
        int note = c->notes[0] == 255 ? 60 : (c->notes[0] + 5) & 127;
        for (int i = 0; i < STATE_WORDS; i++) s_before[i] = s_state[0][i];
        call_engine(E_ON, f_note_on, (uint32_t)note, c->velocity, (uint32_t)s_state[0], (uint32_t)s_params, 0, 0, 0, 0);
        torn_states(c, s_state[0], 1);
    }
    check_case();

    out_str("R");
    out_kv("case", (uint64_t)idx);
    out_kv("voices", (uint64_t)nv);
    out_kv("blocks", active_blocks);
    out_kv("last", last_active);
    out_stat("render", &render_all);
    out_stat("steady", &render_steady);
    out_stat("block", &block_total);
    out_stat("on", &note_on);
    out_stat("off", &note_off);
    out_kv("cb", s_cb_cycles);
    out_str("\n");
    out_flush();
}

int main(void)
{
    for (unsigned i = 0; i < sizeof bench_fills / sizeof bench_fills[0]; i++)
        zero((void *)bench_fills[i].addr, bench_fills[i].len);   /* the engine's .bss, as the loader's FILL */

    uint32_t a = cycles(), b = cycles();
    s_overhead = b - a;

    /* the trampoline's own cost: bench_call(nop) against a direct call of the same nop. What is left is the
     * engine call itself (call, body, return); the argument set-up of a direct call is no longer counted. */
    {
        uint32_t args[6] = { 0 }, best_t = 0xffffffffu, best_d = 0xffffffffu;
        void (*volatile nop)(void) = bench_nop;
        for (int k = 0; k < 3; k++) {
            uint32_t t0 = cycles();
            bench_call((const void *)bench_nop, args, s_stack[E_MODULE] + STACK_WORDS);
            uint32_t t1 = cycles();
            nop();
            uint32_t t2 = cycles();
            if (t1 - t0 < best_t) best_t = t1 - t0;
            if (t2 - t1 < best_d) best_d = t2 - t1;
        }
        for (int e = 0; e < E_COUNT; e++) s_call_overhead[e] = best_t - best_d + s_overhead;
    }
    for (int e = 0; e < E_COUNT; e++) paint(s_stack[e], STACK_WORDS, STACK_PAINT);
    paint(s_outb.pre, GUARD_WORDS, GUARD_PAINT);
    paint(s_outb.post, GUARD_WORDS, GUARD_PAINT);
    if (BENCH_TAIL_WORDS) paint((uint32_t *)BENCH_TAIL_START, BENCH_TAIL_WORDS, GUARD_PAINT);
    s_ro_sum = ro_sum();
    pack_init();

    f_init_module = (InitModuleFn)find_export("initModule");
    f_init = (InitFn)find_export("init");
    f_note_on = (NoteOnFn)find_export("onNoteOn");
    f_note_off = (NoteOffFn)find_export("onNoteOff");
    f_render = (RenderFn)find_export("render");
    f_render_fx = (RenderFxFn)find_export("renderFX");
    if (!f_init_module || !f_init || !f_note_on || !f_note_off || (ENGINE_IS_FX ? !f_render_fx : !f_render)) {
        out_str("E missing export\n");
        out_flush();
        return 2;
    }

    uint32_t mem_request = 0, dt;
    uint32_t ok = call_engine(E_MODULE, f_init_module, 0, (uint32_t)&s_cb, (uint32_t)&mem_request, 0, 0, 0, -1, &dt);
    out_str("M");
    out_kv("ok", ok != 0);
    out_kv("mem", mem_request);
    out_kv("cycles", dt);
    out_kv("overhead", s_overhead);
    out_str("\n");
    out_flush();
    if (!ok || mem_request > FX_MEM_BYTES) {
        out_str("E initModule failed or memRequest too large\n");
        out_flush();
        return 3;
    }
    s_fx_guard = (uint32_t)FX_MEM + ((mem_request + 3) & ~3u);
    paint((uint32_t *)s_fx_guard, GUARD_WORDS, GUARD_PAINT);

    for (unsigned i = 0; i < sizeof bench_cases / sizeof bench_cases[0]; i++) run_case((int)i, &bench_cases[i]);
    report_stacks();
    out_str("D\n");
    out_flush();
    return 0;
}
