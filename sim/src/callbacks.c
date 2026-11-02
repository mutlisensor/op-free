/*
 * callbacks.c — opzsim's ModuleCallbackInterface.
 *
 * The five shared tables come from sdk/include/opz_tables_gen.h: the device's formats (Q-15, engine-abi.md §5),
 * generated from our formulas. The envelope implements the model in sdk/include/opz_adsr.h (Q-17). All maths
 * that feeds audio is libm-free and built with -ffp-contract=off so renders are bit-identical across Macs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "opz_tables_gen.h"

SimSeq g_seq = { .tempo = 120, .playing = 1, .clock_bpm = 120 };
char g_preset_path[1024];

#define SIM_PI     3.14159265358979323846
#define SIM_LN2    0.69314718055994530942
#define SAMPLE_N   44100

static OpzTablesGen s_tables;
static int16_t s_sample[SAMPLE_N];

/* ---- deterministic maths ---------------------------------------------------------------------------- */

double sim_sin(double x)
{
    double turns = x / (2.0 * SIM_PI);
    int64_t k = (int64_t)(turns + (turns >= 0 ? 0.5 : -0.5));
    x -= (double)k * 2.0 * SIM_PI;               /* [-pi, pi] */
    if (x > SIM_PI / 2) x = SIM_PI - x;
    if (x < -SIM_PI / 2) x = -SIM_PI - x;        /* [-pi/2, pi/2] */
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 20; n++) {
        term = -term * x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double sim_exp2(double x)
{
    int64_t n = (int64_t)x;
    if ((double)n > x) n--;                      /* floor */
    double f = (x - (double)n) * SIM_LN2;        /* e^f, f in [0, ln2) */
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 30; i++) {
        term = term * f / (double)i;
        sum += term;
    }
    for (; n > 0; n--) sum *= 2.0;
    for (; n < 0; n++) sum *= 0.5;
    return sum;
}

static void build_tables(void)
{
    opz_tables_generate(&s_tables);
    /* test sample (`sample builtin`): one second of a decaying 440 Hz sine, -6 dBFS */
    for (int i = 0; i < SAMPLE_N; i++) {
        double env = sim_exp2(-4.0 * i / SAMPLE_N);
        double v = 0.5 * env * sim_sin(2.0 * SIM_PI * 440.0 * i / OPZ_SAMPLE_RATE);
        s_sample[i] = (int16_t)(int32_t)(v * 32767.0 + (v >= 0 ? 0.5 : -0.5));
    }
}

/* ---- sequencer -------------------------------------------------------------------------------------- */

static void seq_set(int key, int value)
{
    int id = key & 0xffff, index = (key >> 16) & 0xffff;
    if (g_seq.verbose)
        fprintf(stderr, "cb      sequencerSetValue(id %d, index %d, %d)\n", id, index, value);
    switch (id) {
    case 0: break;                                   /* sequencer flag */
    case 1: g_seq.ducker = value; break;             /* tape ducker */
    case 2: if (index < SIM_TRACKS) g_seq.fx_timing[index] = value; break;
    case 3: if (index < SIM_TRACKS) g_seq.track_flag[index] = value > 0; break;
    default: break;
    }
}

static int seq_get(int key)
{
    int id = key & 0xffff, index = (key >> 16) & 0xffff;
    int v;
    if (id < 16 && g_seq.get_set[id])
        v = g_seq.get_override[id];
    else switch (id) {
    case 2: v = 0; break;
    case 3: v = index < SIM_TRACKS ? g_seq.track_flag[index] : 0; break;
    case 4: v = g_seq.tempo; break;
    case 5: v = g_seq.playing; break;
    case 6: v = g_seq.step; break;
    case 7: v = g_seq.tick; break;
    case 8: v = 1; break;                            /* samples available */
    default: v = -1; break;
    }
    if (g_seq.verbose)
        fprintf(stderr, "cb      sequencerGetValue(id %d, index %d) = %d\n", id, index, v);
    return v;
}

/*
 * Sequencer clock, after Sequencer::tick and update_sequencer in the app (sdk/docs/engine-abi.md, "Sequencer
 * keys"): 24 ticks per sixteenth step, tick k at origin + k * 44100 * 60 / (96 * BPM) frames. A tick plays
 * (step, tick) and then advances the counter, which runs on while stopped; the global step wraps at 16 (the
 * pattern length). The sim runs it between blocks: a block at t0 sees every tick due by t0.
 */
void sim_seq_transport(int play, int64_t t)
{
    if (play) {                     /* Sequencer::startPlayback: counter to (0, 0); its first tick plays it */
        g_seq.playing = 1;
        g_seq.step = g_seq.tick = 0;
        g_seq.origin = t;
        g_seq.ticks = 0;
    } else if (g_seq.playing) {     /* stopPlayback -> resetPlaybackState(false, ...) */
        g_seq.step = g_seq.tick < 8 ? 0 : 15;
        g_seq.playing = 0;
    }
}

void sim_seq_clock(int64_t t0)
{
    /* tick k is due when k * 27562.5 <= (t0 - origin) * BPM (exact in double for whole BPM) */
    while ((double)g_seq.ticks * 27562.5 <= (double)(t0 - g_seq.origin) * g_seq.clock_bpm) {
        if (++g_seq.tick == 24) {
            g_seq.tick = 0;
            if (++g_seq.step >= 16) g_seq.step = 0;
        }
        g_seq.ticks++;
    }
}

/* ---- envelopes -------------------------------------------------------------------------------------- */
/*
 * Host side of the OP-Z ADSR, following updateParametersADSR, Voice::noteOn and Voice::noteOff in the desktop
 * app (confirmed on the device: same rate curve in analoga, same per-sample step in synthsampler/cluster/organ).
 * Rate curves are our own fits to the shape of TE's tables (not copies):
 *   attack step  i = 0: 0.0526, else 2^(-7.444 - 16 i / 127) per sample      (i = parameter >> 24, 0..127)
 *   decay/release time constant  i = 0: 33.6 samples, else 250 i + 11.5 * 2^(0.115 i) samples; coef = e^(-1/tau)
 */
#define ENV_THRESHOLD   0x0010624E   /* ~ -66 dB: release/fade ends here and the voice is freed */
#define ENV_ATTACK_DONE 0x7EB85200   /* 0.99 */
#define ENV_IDLE_COEF   0x70000000

static int32_t s_attack_step[128], s_decay_coef[128];

static void env_tables_init(void)
{
    for (int i = 0; i < 128; i++) {
        double step = i == 0 ? 0.0526142940 : sim_exp2(-7.444 - 16.0 * i / 127.0);
        s_attack_step[i] = (int32_t)(step * 2147483648.0);
        double tau = i == 0 ? 33.6 : 250.0 * i + 11.5 * sim_exp2(0.115 * i);
        double coef = sim_exp2(-1.0 / (tau * SIM_LN2));          /* e^(-1/tau) */
        s_decay_coef[i] = coef >= 1.0 ? SIM_FULL : (int32_t)(coef * 2147483648.0);
    }
}

/* Q31 product, saturating at the one overflowing case (-1 * -1), truncated: the arm64 build's fract32 multiply. */
static int32_t mul_q31_trunc(int64_t a, int64_t b)
{
    int64_t p = a * b;
    return p == ((int64_t)1 << 62) ? SIM_FULL : (int32_t)(p >> 31);
}

static int32_t fade_limit(int32_t level)                           /* ~0.95 % of the level */
{
    return (int32_t)(((uint64_t)(uint32_t)level * 0x4D7A7880u) >> 37);
}

static void update_adsr(ADSR *env, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    SimADSR *e = (SimADSR *)env;
    int32_t S = (int32_t)(s & 0x7fffffff);
    int32_t decay = s_decay_coef[(d >> 24) & 127], rel = s_decay_coef[(r >> 24) & 127];
    if (decay < s_decay_coef[1]) decay = s_decay_coef[1];          /* decay is never faster than index 1 */
    e->attack_step = s_attack_step[(a >> 24) & 127];
    if (e->release) {
        if (e->stage != ENV_IDLE) {
            e->coef = rel;
            e->step = 0;
            e->stage = ENV_RELEASE;
        }
        e->release = 0;
    }
    switch (e->stage) {
    case ENV_ATTACK:
        if (e->level < ENV_ATTACK_DONE) {
            e->coef = SIM_FULL;
            e->step = e->attack_step;
            return;
        }
        e->stage = ENV_DECAY;
        /* fall through */
    case ENV_DECAY:
        if (e->level > S) {
            e->coef = decay;
            e->step = 0;
        } else {
            e->stage = ENV_SUSTAIN;
        }
        return;
    case ENV_SUSTAIN: {
        int64_t diff = (int64_t)S - e->level;
        if (diff > SIM_FULL) diff = SIM_FULL;
        if (diff < -(int64_t)SIM_FULL - 1) diff = -(int64_t)SIM_FULL - 1;
        e->coef = SIM_FULL;
        e->step = (int32_t)diff >> 8;
        return;
    }
    case ENV_RELEASE:
        if (e->level >= ENV_THRESHOLD) {
            e->coef = rel;
            e->step = 0;
            return;
        }
        break;
    case ENV_FADE:
    case ENV_FADE2:
        if (e->level >= ENV_THRESHOLD) {
            int32_t lim = fade_limit(e->level);
            int32_t st = e->step == INT32_MIN ? SIM_FULL : (e->step < 0 ? -e->step : e->step);
            e->step = -(st < lim ? st : lim);
            return;
        }
        break;
    default:
        return;
    }
    e->stage = ENV_IDLE;
    e->coef = ENV_IDLE_COEF;
    e->step = 0;
}

/*
 * Host side of the drum envelope, following updateParametersAHD in the desktop app (the device's
 * sampleplayergain makes the same call). The arguments are positions: the engine passes how far it has played
 * and the landmarks from opz_ahd_layout() (sdk/include/opz_adsr.h); the engine steps the level linearly with
 * opz_ahd_tick(). Stage 7 is the hold; stage 4 is the decay; note-off is ignored.
 */
static void update_ahd(ADSR *env, uint32_t pos, uint32_t attack_end, uint32_t hold_end, uint32_t decay_end,
                       fract32 coef, fract32 shape)
{
    SimADSR *e = (SimADSR *)env;
    switch (e->stage) {
    case ENV_ATTACK:
        if (attack_end > pos) {
            e->coef = SIM_FULL;
            e->step = (int32_t)(0x7fffffffu / (attack_end - pos));
            return;
        }
        e->stage = ENV_HOLD;
        /* fall through */
    case ENV_HOLD:
        if (pos < hold_end) {
            e->level = SIM_FULL;
            e->coef = SIM_FULL;
            e->step = 0;
            return;
        }
        e->stage = ENV_RELEASE;
        /* fall through */
    case ENV_RELEASE: {
        if (decay_end <= pos) break;
        uint32_t left = decay_end - pos;
        int32_t span = (int32_t)(decay_end - hold_end);
        int64_t f = (int64_t)(int32_t)left * ((int64_t)1 << 31) / (span ? span : 1);
        if (f > SIM_FULL) f = SIM_FULL;
        if (f < INT32_MIN) f = INT32_MIN;
        int32_t t = mul_q31_trunc(mul_q31_trunc(mul_q31_trunc(f, shape), shape), shape);
        int64_t d = (int64_t)SIM_FULL - t;
        uint32_t u = (uint32_t)(d > SIM_FULL ? SIM_FULL : d < INT32_MIN ? INT32_MIN : d);
        uint64_t u4 = (uint64_t)u * u >> 31;
        u4 = (u4 * u) >> 31;
        u4 = (uint32_t)((u4 * u) >> 31);
        int32_t lin = -(e->level / (int32_t)(left > OPZ_BLOCK_FRAMES ? left : OPZ_BLOCK_FRAMES));
        e->coef = coef;
        e->step = (int32_t)(((int64_t)u4 * lin) >> 31);
        return;
    }
    case ENV_FADE:
        if (e->level >= ENV_THRESHOLD) {
            int32_t lim = fade_limit(e->level);
            int32_t st = e->step == INT32_MIN ? SIM_FULL : (e->step < 0 ? -e->step : e->step);
            e->step = -(st < lim ? st : lim);
            return;
        }
        break;
    case ENV_FADE2:
        e->coef = SIM_FULL;
        if (e->level >= ENV_THRESHOLD) {
            e->step = -fade_limit(e->level);
            return;
        }
        break;
    default:                                                        /* idle; 2 and 3 are not used by drums */
        return;
    }
    e->stage = ENV_IDLE;
    e->coef = ENV_IDLE_COEF;
    e->step = 0;
}

/* Voice::noteOn (fresh voice): level 0, stage 1, step = the last attack step. The device fades a busy voice out
 * first (stage 5) and starts the new note when it reaches 0; the simulator restarts at once. */
void sim_env_note_on(SimADSR *e)
{
    e->level = 0;
    e->stage = ENV_ATTACK;
    e->coef = SIM_FULL;
    if (e->attack_step == 0) e->attack_step = s_attack_step[0];
    e->step = e->attack_step;
    e->new_note = 1;
    e->release = 0;
    e->gate = 1;
}

/* Voice::noteOff: clear the new-note byte, set the release flag; the next updateParametersADSR starts release. */
void sim_env_note_off(SimADSR *e)
{
    e->new_note = 0;
    e->release = 1;
    e->gate = 0;
}

/* ---- samples, presets, IIR -------------------------------------------------------------------------- */

/*
 * Sample packs, as the OS SampleLoader serves them (research/samples.md): a scenario attaches an OP-1 format
 * .aif to an instrument track; its pack id is crc32 of the plug's `samples` path, passed as init's arg2 and mem,
 * and getSample(packId, note) returns one 32-byte OpzSampleSlice per key. The parsing follows tools/opz_samples.py
 * (the host check sample_probe compares every slice word with tools/opz_sampleplay.py). Drum packs: 24 start/end
 * positions with 2^31 = 529,200 frames, the rate word from each key's pitch knob; synth samples ("sampler"): one
 * slice from the 8 knobs and base_freq; a plain AIFF (no op-1 chunk) is one slice, capped at 6 s. getSample does not
 * look at the engine or at sequencerGetValue(8), as on the device (Q-27).
 */
#define PACK_KEYS        24
#define PACK_MAX_FRAMES  529200     /* 12.0 s: one cache slot, and the drum position scale */
#define PLAIN_MAX_FRAMES 264600     /* 6.0 s */
#define SAMPLES_PATH_MAX 95         /* the plug record keeps 96 bytes of the path, NUL included */

typedef struct SimPack {
    uint32_t id;
    int nkeys;
    int16_t *pcm;
    OpzSampleSlice slice[PACK_KEYS];
} SimPack;

static SimPack s_packs[SIM_INSTR_TRACKS];
static int s_npacks;

uint32_t sim_crc32(const char *str)          /* zlib's crc32, as PlugManager's makeHash */
{
    uint32_t c = 0xffffffffu;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        c ^= *p;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return ~c;
}

static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* The value after "key": in the op-1 JSON object, or NULL. Flat objects only, as in TE's packs. */
static const char *json_value(const char *js, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = strchr(js, '"'); p; p = strchr(p + 1, '"')) {
        if (strncmp(p + 1, key, n) || p[n + 1] != '"') continue;
        const char *q = p + n + 2;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') continue;
        for (q++; *q == ' ' || *q == '\t' || *q == '\n' || *q == '\r'; q++) {}
        return q;
    }
    return NULL;
}

/* Up to max numbers of the array "key"; returns how many, -1 if the key is missing or not an array. */
static int json_ints(const char *js, const char *key, int64_t *out, int max)
{
    const char *p = json_value(js, key);
    if (!p || *p != '[') return -1;
    int n = 0;
    for (p++;;) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']' || !*p) break;
        char *end;
        double v = strtod(p, &end);
        if (end == p) return -1;
        if (n < max) out[n] = (int64_t)v;
        n++;
        p = end;
    }
    return n < max ? n : max;
}

static int pack_fail(const char *file, const char *what)
{
    fprintf(stderr, "error   sample %s: %s\n", file, what);
    return -1;
}

/* What the OS computes from a drum position: a Q31 multiply by 529,200, rounded (tools/opz_samples.py). */
static int64_t frame_from_pos(int64_t pos)
{
    return (pos * PACK_MAX_FRAMES * 2 + ((int64_t)1 << 31)) >> 32;
}

/* The loader's rate word: Q31 of ratio / 16 clamped to [2^-8, 1], truncated (fcvtzs #31). Unity = 2^27. */
static uint32_t rate_word(float r16)
{
    if (r16 > 1.0f) r16 = 1.0f;
    else if (!(r16 >= 0.00390625f)) r16 = 0.00390625f;
    double q = (double)r16 * 2147483648.0;
    return q >= 2147483647.0 ? 0x7fffffffu : (uint32_t)(int64_t)q;
}

/* Drum key: 4,800 cents per 24,576 pitch units, rounded away from zero, in single precision (FUN_80107648, app
 * drumPitchToDetune); drum_version < 3 drops the low 9 bits first, so those packs move in whole semitones. */
static uint32_t drum_rate(int64_t pitch, long version)
{
    int64_t p = version < 3 ? pitch & ~(int64_t)0x1ff : pitch;
    float x = (float)((float)(p < 0 ? -p : p) * 4800.0f) / 24576.0f;
    int64_t c = (int64_t)x;
    if ((float)c < x) c++;
    float ratio = (float)sim_exp2((double)(p < 0 ? -c : c) / 1200.0);
    return rate_word(ratio * 0.0625f);
}

/* Synth sample: 261.626 / (16 * base_freq) (FUN_80107760, app op1SynthBaseFreqToDetune). */
static uint32_t synth_rate(double base_freq)
{
    return rate_word(261.6255798339844f / ((float)base_freq * 16.0f));
}

/* A synth knob position (Q31) in frames: truncating Q31 multiply by 264,600 (FUN_80105e94, Mult32 (T)). */
static int64_t synth_frame(int64_t q31) { return (q31 * PLAIN_MAX_FRAMES) >> 31; }

static int pack_parse(SimPack *pk, const char *file, const uint8_t *d, size_t n)
{
    if (n < 12 || memcmp(d, "FORM", 4) || (memcmp(d + 8, "AIFF", 4) && memcmp(d + 8, "AIFC", 4)))
        return pack_fail(file, "not an AIFF file");
    int aifc = !memcmp(d + 8, "AIFC", 4);
    const uint8_t *comm = NULL, *ssnd = NULL, *appl = NULL;
    uint32_t comm_n = 0, ssnd_n = 0, appl_n = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t len = rd_be32(d + p + 4);
        if (len > n - p - 8) len = (uint32_t)(n - p - 8);
        const uint8_t *body = d + p + 8;
        if (!memcmp(d + p, "COMM", 4) && !comm) { comm = body; comm_n = len; }
        if (!memcmp(d + p, "SSND", 4) && !ssnd) { ssnd = body; ssnd_n = len; }
        if (!memcmp(d + p, "APPL", 4) && !appl) { appl = body; appl_n = len; }
        p += 8 + (size_t)len + (len & 1);
    }
    if (!comm || comm_n < 18) return pack_fail(file, "no COMM chunk");
    int channels = (int16_t)rd_be16(comm), bits = (int16_t)rd_be16(comm + 6);
    uint32_t frames = rd_be32(comm + 2);
    /* 44100.0 as an 80-bit extended: exponent 0x400E, mantissa 0xAC44 << 48 (the OS wants exactly 44100) */
    if (rd_be16(comm + 8) != 0x400E || rd_be32(comm + 10) != 0xAC440000u || rd_be32(comm + 14) != 0)
        return pack_fail(file, "sample rate is not 44100 Hz");
    if (channels != 1 || bits != 16) return pack_fail(file, "packs are 16-bit mono");
    int little = 0;
    if (aifc && comm_n >= 22) {
        if (!memcmp(comm + 18, "sowt", 4)) little = 1;
        else if (memcmp(comm + 18, "NONE", 4)) return pack_fail(file, "compressed AIFF-C is not supported");
    }
    if (frames > PACK_MAX_FRAMES) return pack_fail(file, "more than 529,200 frames (12 s)");
    if (!ssnd || ssnd_n < 8) return pack_fail(file, "no SSND chunk");
    uint32_t off = rd_be32(ssnd);
    if (off > ssnd_n - 8 || (ssnd_n - 8 - off) / 2 < frames) return pack_fail(file, "SSND is shorter than COMM says");
    pk->pcm = malloc(frames ? frames * sizeof(int16_t) : 1);
    const uint8_t *raw = ssnd + 8 + off;
    for (uint32_t i = 0; i < frames; i++)
        pk->pcm[i] = (int16_t)(little ? raw[2 * i] | raw[2 * i + 1] << 8 : raw[2 * i] << 8 | raw[2 * i + 1]);

    if (!appl || appl_n < 4 || memcmp(appl, "op-1", 4)) {      /* a plain sample: one slice */
        OpzSampleSlice *sl = &pk->slice[0];
        memset(sl, 0, sizeof *sl);
        sl->data = pk->pcm;
        sl->frames = frames < PLAIN_MAX_FRAMES ? frames : PLAIN_MAX_FRAMES;
        pk->nkeys = 1;
        return 0;
    }
    char *js = malloc(appl_n - 4 + 1);
    memcpy(js, appl + 4, appl_n - 4);
    js[appl_n - 4] = 0;
    const char *type = json_value(js, "type");
    int rc = 0;
    const char *ed = json_value(js, "editable");
    uint8_t editable = ed && (!strncmp(ed, "true", 4) || (*ed >= '1' && *ed <= '9'));
    if (type && !strncmp(type, "\"sampler\"", 9)) {
        /* one slice: knobs[0] start, [3] end, [1] << 16 | [5] << 1 loop start, [2] << 16 | [6] << 1 loop end, [4] play
         * mode (top byte of +0x08), [7] volume; start <= loop start <= loop end <= end <= frames (FUN_80105e94) */
        int64_t k[8];
        if (json_ints(js, "knobs", k, 8) != 8) { rc = pack_fail(file, "synth sample needs exactly 8 knobs"); goto done; }
        for (int i = 0; i < 8; i++) k[i] &= 0xffff;
        const char *bf = json_value(js, "base_freq");
        double base = bf ? strtod(bf, NULL) : 261.6255798339844;
        int64_t len = frames < PLAIN_MAX_FRAMES ? frames : PLAIN_MAX_FRAMES;
        int64_t s = synth_frame(k[0] << 16), e = synth_frame(k[3] << 16);
        int64_t ls = synth_frame(k[1] << 16 | k[5] << 1), le = synth_frame(k[2] << 16 | k[6] << 1);
        if (s > len) s = len;
        if (e > len) e = len;
        if (ls < s) ls = s;
        if (ls > e) ls = e;
        if (le < ls) le = ls;
        if (le > e) le = e;
        OpzSampleSlice *sl = &pk->slice[0];
        memset(sl, 0, sizeof *sl);
        sl->data = pk->pcm + s;
        sl->frames = (uint32_t)(e > s ? e - s : 0);
        sl->knobs = (uint32_t)k[7] | (uint32_t)((k[4] >> 7) & 0xff) << 24;
        sl->rate = synth_rate(base);
        sl->loopStart = (uint32_t)(ls - s);
        sl->loopEnd = (uint32_t)(le - s);
        sl->editable = editable;
        pk->nkeys = 1;
        goto done;
    }
    if (!type || strncmp(type, "\"drum\"", 6)) {
        rc = pack_fail(file, "op-1 type is neither drum nor sampler");
        goto done;
    }
    int64_t start[PACK_KEYS], end[PACK_KEYS], vol[PACK_KEYS], pitch[PACK_KEYS], mode[PACK_KEYS], rev[PACK_KEYS];
    int ns = json_ints(js, "start", start, PACK_KEYS), ne = json_ints(js, "end", end, PACK_KEYS);
    int nk = ns < ne ? ns : ne;
    if (nk <= 0) { rc = pack_fail(file, "drum pack without start/end positions"); goto done; }
    /* knob lists are padded to 24 with the defaults (opz_samples.py) */
    int nv = json_ints(js, "volume", vol, PACK_KEYS), np = json_ints(js, "pitch", pitch, PACK_KEYS);
    int nm = json_ints(js, "playmode", mode, PACK_KEYS), nr = json_ints(js, "reverse", rev, PACK_KEYS);
    for (int i = 0; i < PACK_KEYS; i++) {
        if (i >= nv) vol[i] = 8192;
        if (i >= np) pitch[i] = 0;
        if (i >= nm) mode[i] = 8192;
        if (i >= nr) rev[i] = 8192;
    }
    const char *dv = json_value(js, "drum_version");
    long version = dv ? strtol(dv, NULL, 10) : 2;
    for (int i = 0; i < nk; i++) {
        int64_t s = frame_from_pos(start[i]), e = frame_from_pos(end[i]);
        if (start[i] < 0 || end[i] < 0 || s > e || e > frames) {
            char msg[128];
            snprintf(msg, sizeof msg, "key %d: start %lld / end %lld outside 0..%u frames", i + 1, (long long)s,
                     (long long)e, frames);
            rc = pack_fail(file, msg);
            goto done;
        }
        OpzSampleSlice *sl = &pk->slice[i];
        memset(sl, 0, sizeof *sl);
        sl->data = pk->pcm + s;
        sl->frames = (uint32_t)(e - s);
        sl->knobs = ((uint32_t)vol[i] & 0xffff) | ((uint32_t)(mode[i] >> 7) & 0xff) << 16
                  | ((uint32_t)(rev[i] >> 7) & 0xff) << 24;
        sl->rate = drum_rate(pitch[i], version);
        sl->pitch = (int32_t)(version < 3 ? pitch[i] & ~(int64_t)0x1ff : pitch[i]);
        sl->editable = editable;
    }
    pk->nkeys = nk;
done:
    free(js);
    return rc;
}

/* The old raw test buffer, served as a one-key plain pack (`sample builtin`). */
static int pack_builtin(SimPack *pk)
{
    pk->pcm = s_sample;
    memset(&pk->slice[0], 0, sizeof pk->slice[0]);
    pk->slice[0].data = s_sample;
    pk->slice[0].frames = SAMPLE_N;
    pk->nkeys = 1;
    return 0;
}

int sim_pack_attach(const char *file, const char *samples_path, uint32_t *id)
{
    if (strlen(samples_path) > SAMPLES_PATH_MAX) {
        fprintf(stderr, "error   sample path %s is longer than the %d characters a plug record keeps\n", samples_path,
                SAMPLES_PATH_MAX);
        return -1;
    }
    *id = sim_crc32(samples_path);
    for (int i = 0; i < s_npacks; i++)
        if (s_packs[i].id == *id) return 0;                      /* one cache slot per pack */
    if (s_npacks == SIM_INSTR_TRACKS) return pack_fail(file, "too many packs");
    SimPack *pk = &s_packs[s_npacks];
    memset(pk, 0, sizeof *pk);
    if (!file) {
        pack_builtin(pk);
    } else {
        FILE *f = fopen(file, "rb");
        if (!f) return pack_fail(file, "cannot open");
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
        size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        int rc = n > 0 && got == (size_t)n ? pack_parse(pk, file, buf, (size_t)n) : pack_fail(file, "cannot read");
        free(buf);
        if (rc) return rc;
    }
    pk->id = *id;
    s_npacks++;
    uint32_t total = 0;
    for (int k = 0; k < pk->nkeys; k++) total += pk->slice[k].frames;
    fprintf(stderr, "sample  %s -> pack 0x%08x, %d key%s, %u frames in slices\n", samples_path, *id, pk->nkeys,
            pk->nkeys == 1 ? "" : "s", total);
    return 0;
}

int sim_pack_loaded(uint32_t id)
{
    for (int i = 0; i < s_npacks; i++)
        if (s_packs[i].id == id) return 1;
    return 0;
}

/* FUN_80100d78: the loaded pack by id, slice (note + 19) mod 24 clamped to its keys (a synth sample has one); NULL
 * for an unknown id. Nothing here looks at the engine or at sequencerGetValue(8). */
static const void *get_sample(int packId, uint8_t note)
{
    const OpzSampleSlice *sl = NULL;
    int key = -1;
    for (int i = 0; i < s_npacks; i++) {
        if (s_packs[i].id != (uint32_t)packId) continue;
        key = (note + 19) % PACK_KEYS;
        if (key >= s_packs[i].nkeys) key = s_packs[i].nkeys - 1;
        sl = &s_packs[i].slice[key];
    }
    if (g_seq.verbose) {
        if (sl)
            fprintf(stderr, "cb      getSample(pack 0x%08x, note %u) = key %d, %u frames\n", (uint32_t)packId, note,
                    key + 1, sl->frames);
        else
            fprintf(stderr, "cb      getSample(pack 0x%08x, note %u) = NULL\n", (uint32_t)packId, note);
    }
    return sl;
}

static int get_preset_data(int a, int b, void *out)
{
    if (g_seq.verbose) fprintf(stderr, "cb      getPresetData(%d, %d) <- %s\n", a, b, g_preset_path);
    if (!g_preset_path[0] || !out) return 0;
    FILE *f = fopen(g_preset_path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, SIM_PRESET_BYTES, f);
    fclose(f);
    return n > 0;
}

static void iir_update_peak(IIRFilter *f, uint16_t freq, uint8_t gain)
{
    static int warned;
    (void)f; (void)freq; (void)gain;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "warning IIRFilter_updateCoefficientsPeak: not modelled (Q-17), filter is a pass-through\n");
    }
}

static void iir_filter(IIRFilter *f, fract16 *buf, uint8_t n)
{
    (void)f; (void)buf; (void)n;
}

void sim_callbacks_init(ModuleCallbackInterface *cb)
{
    build_tables();
    env_tables_init();
    memset(cb, 0, sizeof *cb);
    cb->sequencerSetValue = seq_set;
    cb->sequencerGetValue = seq_get;
    opz_tables_install(cb, &s_tables);
    cb->updateParametersADSR = update_adsr;
    cb->updateParametersAHD = update_ahd;
    cb->getSample = get_sample;
    cb->getPresetData = get_preset_data;
    cb->IIRFilter_updateCoefficientsPeak = iir_update_peak;
    cb->IIRFilter_filter = iir_filter;
}

_Static_assert(offsetof(SimADSR, level) == 4, "ADSR level at +4");
_Static_assert(offsetof(SimADSR, coef) == 8, "ADSR coef at +8");
_Static_assert(offsetof(SimADSR, step) == 12, "ADSR step at +12");
_Static_assert(offsetof(SimADSR, stage) == 16, "ADSR stage at +16");
_Static_assert(offsetof(SimADSR, new_note) == 20, "ADSR new-note byte at +20");
_Static_assert(offsetof(SimADSR, release) == 21, "ADSR release flag at +21");
