/*
 * fuzz.c — host fuzz test for clouds: random knobs, notes, bends and inputs, with every grain read checked.
 *
 *   cc -std=c11 -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -I sdk/include \
 *      -o /tmp/clouds_fuzz engines/clouds/tests/fuzz.c && /tmp/clouds_fuzz [blocks-per-run]
 *
 * It includes clouds.c with its test hooks defined and keeps a shadow of the ring: the time at which every sample was
 * recorded. Each grain read must be of audio recorded continuously around it (the sample and the one after it
 * recorded one tick apart: no seam, no half-overwritten stretch), and a grain's successive reads must move forward
 * through the recording at no more than its rate (no jump to newer audio that overwrote what it was playing). The
 * buffer is mapped at exactly the engine's size (FX1: memRequest; FX2: the 128 KB it says it uses there) between two
 * inaccessible pages, so any read or write outside it crashes; the state has a guard after its 204 bytes. Runs include
 * knobs jumping every block, notes and chords, held notes re-sent with velocity 0 and params NULL, plug-select note
 * 255, bends, freeze / thaw at random times, full-scale and silent inputs, gain 0 and 1.
 * MIT licence, see CONTRIBUTING.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int64_t *g_wt;                   /* ring index -> tick recorded (-1: cleared or never) */
static uint8_t *g_faded;                /* ring index -> recorded at under 1/8 of the full level (a fade) */
static uint32_t g_len;
static int64_t g_now;                   /* tick of the current block's first sample */
static int64_t g_last[16];              /* per grain: recording tick of its last audible read */
static int g_cur = -1;
static long g_errors, g_reads;
static const void *g_grain0;

#define CLOUDS_HOOK_GRAIN(g) hook_grain((const void *)(g))
#define CLOUDS_HOOK_READ(q, wl, wr) hook_read((q), (wl), (wr))
#define CLOUDS_HOOK_WRITE(ix, level) (g_wt[(ix)] = g_now + (int64_t)((ix) & 63), g_faded[(ix)] = (level) < 0x10000000)
static void hook_grain(const void *g);
static void hook_read(const int16_t *q, int32_t wl, int32_t wr);

#include "../clouds.c"

static void hook_grain(const void *gp)
{
    const Grain *g = gp;
    g_cur = (int)(g - (const Grain *)g_grain0);
    if (g->ph == 0 && g->w == 0) g_last[g_cur] = -1;        /* a new grain */
}

static const int16_t *g_ring;

static void hook_read(const int16_t *q, int32_t wl, int32_t wr)
{
    g_reads++;
    long off = q - g_ring;
    if (off < 0 || off + 1 >= (long)(g_len + GUARD)) { printf("read at %ld out of %u\n", off, g_len); g_errors++; return; }
    uint32_t ix = (uint32_t)off & (g_len - 1);
    int audible = (wl >> 16) != 0 || (wr >> 16) != 0;          /* silent reads may cross anything */
    uint32_t ix1 = (ix + 1) & (g_len - 1);
    int64_t a = g_wt[ix], b = g_wt[ix1];
    int faded = g_faded[ix] || g_faded[ix1];                  /* a splice between two fades: fine */
    if (audible && !faded && a >= 0 && b >= 0 && b != a + 1) {
        if (g_errors < 20) printf("tick %lld: grain %d reads across a seam at %u (%lld, %lld), gain %d\n",
                                  (long long)g_now, g_cur, ix, (long long)a, (long long)b, (wl >> 16) | (wr >> 16));
        g_errors++;
    }
    if (a >= 0 && g_cur >= 0) {
        if (audible && !faded && g_last[g_cur] >= 0 && (a < g_last[g_cur] || a > g_last[g_cur] + 3)) {
            if (g_errors < 20) printf("tick %lld: grain %d jumps from %lld to %lld\n", (long long)g_now, g_cur,
                                      (long long)g_last[g_cur], (long long)a);
            g_errors++;
        }
        g_last[g_cur] = a;
    }
}

static uint32_t g_seed = 12345;
static uint32_t rnd32(void) { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 1 ^ g_seed << 15; }
static int rndn(int n) { return (int)(rnd32() % (uint32_t)n); }

typedef struct { uint8_t state[OPZ_FX_STATE_BYTES]; uint8_t guard[64]; } StateBox;

static int run(int track, long blocks, uint32_t seed)
{
    uint32_t req = 0;
    static const ModuleCallbackInterface cb;
    initModule(0, &cb, &req);
    int bits = track == OPZ_TRACK_FX1 ? BITS_FX1 : BITS_OTHER;
    uint32_t size = track == OPZ_TRACK_FX1 ? req : MEM_BYTES(BITS_OTHER);
    /* the buffer ends exactly at an inaccessible page and starts right after one */
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), span = (size + pg - 1) / pg * pg;
    uint8_t *map = mmap(0, span + 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    mprotect(map, pg, PROT_NONE);
    mprotect(map + pg + span, pg, PROT_NONE);
    uint8_t *mem = map + pg + span - size;                    /* 4-byte aligned: size is a multiple of 4 */
    memset(mem, 0x5a, size);                                  /* the pool is reused: garbage */
    g_len = 1u << bits;
    g_wt = malloc(sizeof(int64_t) * g_len);
    g_faded = calloc(g_len, 1);
    for (uint32_t i = 0; i < g_len; i++) g_wt[i] = -1;
    StateBox box;
    memset(&box, 0x5a, sizeof box);
    g_seed = seed;
    init(box.state, (uint32_t)track, 0, mem, 0);
    g_grain0 = ((State *)box.state)->m ? ((State *)box.state)->m->g : 0;
    g_ring = ((State *)box.state)->m ? ((State *)box.state)->m->ring : 0;

    uint32_t params[OPZ_NUM_PARAMS];
    uint8_t bytes[OPZ_NUM_PARAMS] = { 128, 128, 0, 0, 255, 0, 0, 0, 128, 0, 128, 255 };
    fract32 in[2 * BLK], out[2 * BLK];
    int held[8], nheld = 0, mode = 0, frozen_blocks = 0;
    fract32 bend = 0, gain = 0x7fffffff;
    int64_t peak = 0;
    for (long b = 0; b < blocks; b++) {
        g_now = b * BLK;
        if (b % 2000 == 0) mode = rndn(6);                    /* how hard the knobs move for a while */
        if (mode == 0 || (mode == 1 && rndn(4) == 0)) {       /* jumps */
            bytes[0] = (uint8_t)rndn(256);
            bytes[1] = (uint8_t)rndn(256);
        } else if (mode == 2) {                               /* extremes */
            bytes[0] = rndn(2) ? 255 : 0;
            bytes[1] = rndn(2) ? 255 : 0;
        } else if (mode == 3) {                               /* sweeps */
            bytes[0] = (uint8_t)(b * 3);
            bytes[1] = (uint8_t)(b * 5 / 3);
        }
        if (rndn(500) == 0) bytes[OPZ_P_PAN] = (uint8_t)rndn(256);
        if (rndn(500) == 0) bytes[OPZ_P_LEVEL] = (uint8_t)rndn(256);
        for (int i = 0; i < OPZ_NUM_PARAMS; i++) params[i] = opz_param_from_byte(bytes[i]);
        if (rndn(300) == 0) bend = (fract32)(rnd32() << 1) * (rndn(4) == 0 ? 1 : 0);
        if (rndn(1000) == 0) gain = rndn(3) ? 0x7fffffff : (rndn(2) ? 0 : 0x40000000);

        /* notes: taps, long holds (freeze), chords, vel-0 re-sends with NULL params, note 255 */
        int r = rndn(1000);
        if (r < 8 && nheld < 8) {
            int n = rndn(128);
            onNoteOn(n, (uint8_t)(1 + rndn(127)), box.state, rndn(2) ? params : NULL);
            held[nheld++] = n;
        } else if (r < 14 && nheld) {
            int k = rndn(nheld);
            onNoteOff(held[k], box.state);
            held[k] = held[--nheld];
            if (nheld && rndn(2)) onNoteOn(held[rndn(nheld)], 0, box.state, NULL);   /* the OS's re-send */
        } else if (r == 14) {
            onNoteOn(255, 0, box.state, NULL);
        } else if (r == 15) {
            onNoteOn(rndn(1000) - 500, (uint8_t)rndn(256), box.state, NULL);       /* nonsense: ignored */
            onNoteOff(rndn(1000) - 500, box.state);
        }

        int kind = (int)((b / 700) % 5);
        for (int i = 0; i < BLK; i++) {
            fract32 v;
            if (kind == 0) v = (fract32)rnd32();
            else if (kind == 1) v = ((g_now + i) / 50) & 1 ? 0x7fffffff : (fract32)0x80000000;
            else if (kind == 2) v = 0;
            else v = (fract32)((uint32_t)(g_now + i) * 40000000u) >> (kind == 3 ? 0 : 3);
            in[2 * i] = v;
            in[2 * i + 1] = kind == 4 ? -v : v;
        }
        renderFX(params, bend, box.state, in, out, gain);
        for (int i = 0; i < 2 * BLK; i++) {
            int64_t a = out[i] < 0 ? -(int64_t)out[i] : out[i];
            if (a > peak) peak = a;
        }
        if (((State *)box.state)->frozen) frozen_blocks++;
        State *st = (State *)box.state;
        if (g_ring && st->cleared >= (st->len + GUARD) / 2 && memcmp(g_ring, g_ring + g_len, GUARD * 2)) {
            printf("block %ld: mirror differs\n", b);
            g_errors++;
        }
        for (unsigned i = 0; i < sizeof box.guard; i++)
            if (box.guard[i] != 0x5a) { printf("state guard written\n"); g_errors++; break; }
    }
    State *s = (State *)box.state;
    printf("track %d seed %u: %ld blocks, %ld reads, %d frozen blocks, peak %.3f, len %u, errors %ld\n", track, seed,
           blocks, g_reads, frozen_blocks, (double)peak / 2147483648.0, s->len, g_errors);
    free(g_wt);
    free(g_faded);
    munmap(map, span + 2 * pg);
    return g_errors != 0;
}

int main(int argc, char **argv)
{
    long blocks = argc > 1 ? atol(argv[1]) : 60000;
    int fail = 0;
    for (uint32_t seed = 1; seed <= 4; seed++) {
        g_errors = g_reads = 0;
        fail |= run(OPZ_TRACK_FX1, blocks, seed);
        g_errors = g_reads = 0;
        fail |= run(OPZ_TRACK_FX2, blocks / 2, seed + 100);
    }
    printf(fail ? "FAIL\n" : "ok\n");
    return fail;
}
