/*
 * sdk_check.c — the SDK's per-sample helpers (sdk/include/opz_engine.h, opz_dsp.h) on pseudo-random and edge
 * inputs, one FNV-1a checksum line per helper.
 *
 * Built twice by tools/tests/test_opz_cycles.py: for the host (cc, the arithmetic opzsim's engines get) and for the
 * Blackfin (bfin-elf-gcc with the device flags, run in bfin-elf-run). Both must print the same lines: then opzsim
 * renders the bits the device renders, for every helper listed here (opz_mix.h's hand-written Blackfin loops too). opz_mul_q31_rnd (opz_adsr.h) is left out on
 * purpose: its host version rounds, the Blackfin's fractional multiply rounds down.
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"
#include "opz_mix.h"

#if defined(__bfin__)
static int sim_syscall(int nr, uint32_t *args)
{
    register int r0 __asm__("R0") = (int)args;
    register int p0 __asm__("P0") = nr;
    __asm__ volatile("EXCPT 0;" : "+r"(r0) : "r"(p0) : "memory");
    return r0;
}
static void put(const char *s, uint32_t n)
{
    uint32_t args[3] = { 1, (uint32_t)s, n };
    sim_syscall(5 /* write */, args);
}
void bench_exit(int code)
{
    uint32_t args[1] = { (uint32_t)code };
    sim_syscall(1 /* exit */, args);
}
#else
#include <stdio.h>
static void put(const char *s, uint32_t n) { fwrite(s, 1, n, stdout); }
#endif

#define N 20000

static uint32_t s_seed = 12345;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    uint32_t hi = s_seed & 0xffff0000u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return hi | (s_seed >> 16);
}

/* Random words, with every fifth one taken from the edges. */
static const int32_t k_edges[] = { 0, 1, -1, 2, -2, 0x7fffffff, (int32_t)0x80000000, 0x7fff, 0x8000, 0xffff,
                                   0x10000, -0x8000, -0x10000, 0x40000000, -0x40000000, 0x3fffffff, 0x7fff8000 };
static int32_t pick(int i)
{
    if (i % 5 == 0) return k_edges[(rnd() >> 8) % (sizeof k_edges / sizeof k_edges[0])];
    return (int32_t)rnd();
}

static uint32_t s_hash;
static void mix(uint32_t v)
{
    for (int k = 0; k < 4; k++) {
        s_hash ^= (v >> (8 * k)) & 0xff;
        s_hash *= 16777619u;
    }
}

static void line(const char *name)
{
    static const char hex[] = "0123456789abcdef";
    char buf[64];
    uint32_t n = 0;
    while (*name && n < 40) buf[n++] = *name++;
    buf[n++] = ' ';
    for (int k = 28; k >= 0; k -= 4) buf[n++] = hex[(s_hash >> k) & 15];
    buf[n++] = '\n';
    put(buf, n);
    s_hash = 2166136261u;
}

int main(void)
{
    s_hash = 2166136261u;
    for (int i = 0; i < N; i++) mix((uint32_t)opz_mul_q31(pick(i), pick(i + 1)));
    line("opz_mul_q31");
    for (int i = 0; i < N; i++) {
        int32_t a = pick(i), b = pick(i + 1);
        mix((uint32_t)opz_add_q31(a, b));
        mix((uint32_t)opz_sub_q31(a, b));
    }
    line("opz_add_q31 opz_sub_q31");
    for (int i = 0; i < N; i++) {
        int32_t a = pick(i);
        mix((uint32_t)opz_shl_q31(a, 1));
        mix((uint32_t)opz_shl_q31(a, 2));
    }
    line("opz_shl_q31");
    for (int i = 0; i < N; i++) mix((uint32_t)opz_mul_q31_q15(pick(i), pick(i + 1)));
    line("opz_mul_q31_q15");
    for (int i = 0; i < N; i++) mix((uint32_t)opz_norm32(pick(i)));
    line("opz_norm32");
    for (int i = 0; i < N; i++) {
        uint32_t a = (uint32_t)pick(i), b = (uint32_t)pick(i + 1);
        mix(opz_mul_u16(a, b));
        mix(opz_mul_u32_shr16(a, b));
    }
    line("opz_mul_u16 opz_mul_u32_shr16");
    for (int i = 0; i < N; i++) mix((uint32_t)opz_abs32(pick(i)));
    line("opz_abs32");
    for (int i = 0; i < N; i++) mix((uint32_t)opz_exp2_q16((int32_t)(rnd() % (29u << 16)) - (15 << 16)));
    line("opz_exp2_q16");
    for (int i = 0; i < N; i++) {
        uint32_t ph = (uint32_t)pick(i);
        mix((uint32_t)opz_tri(ph));
        mix((uint32_t)opz_sine(ph));
        mix((uint32_t)opz_tri15(ph));
    }
    line("opz_tri opz_sine opz_tri15");
    for (int i = 0; i < N; i++) {
        int32_t a = pick(i), b = pick(i + 1), w = pick(i + 2);
        mix((uint32_t)opz_soft(a));
        mix((uint32_t)opz_clip(a));
        mix((uint32_t)opz_lerp(a, b, w & 0x7fffffff));
    }
    line("opz_soft opz_clip opz_lerp");
    for (int i = 0; i < N; i++) mix((uint32_t)opz_recip_q31((int32_t)(0x40000000u + (rnd() & 0x3fffffffu))));
    line("opz_recip_q31");
    for (int i = 0; i < N; i++) {
        uint32_t inc = 0x10000u + (rnd() % 0x7fff0000u), rec = opz_blep_recip(inc);
        mix(rec);
        mix((uint32_t)opz_saw_blep((uint32_t)pick(i), inc, rec));
        mix((uint32_t)opz_saw_blep(rnd() % inc, inc, rec));                       /* just after a wrap */
        mix((uint32_t)opz_saw_blep(0u - 1u - rnd() % inc, inc, rec));             /* just before one */
    }
    line("opz_blep_recip opz_saw_blep");
    for (int i = 0; i < 2000; i++) mix((uint32_t)opz_decay_coef((int32_t)(rnd() % (1u << 28))));
    line("opz_decay_coef");
    for (int i = 0; i < 2000; i++) {
        uint32_t d = rnd() | 1u, hi = rnd() % d;
        mix(opz_udiv64_32(((uint64_t)hi << 32) | rnd(), d));
    }
    line("opz_udiv64_32");
    {
        OpzSvf f = { 0, 0 };
        for (int j = 0; j < 40; j++) {
            OpzSvfCoef c = opz_svf_coef((int32_t)(rnd() % (17000u << 16)), (int32_t)(rnd() % 0x80000000u));
            mix((uint32_t)c.a1);
            mix((uint32_t)c.a2);
            mix((uint32_t)c.a3);
            for (int i = 0; i < 500; i++) {
                fract32 lp, bp, hp;
                opz_svf(&f, &c, pick(i) >> 1, &lp, &bp, &hp);
                mix((uint32_t)lp);
                mix((uint32_t)bp);
                mix((uint32_t)hp);
            }
        }
    }
    line("opz_svf_coef opz_svf");
    for (int i = 0; i < N; i++) mix((uint32_t)opz_noise(&s_seed));
    line("opz_noise");
    {
        /* opz_mix_mono / opz_mix_stereo: steady, unity and ramping blocks (each level/pan change ramps for one block),
           random and edge samples, and outputs that saturate. */
        static const uint32_t k_lp[] = { 0, 1, 0x00800000u, 0x40000000u, 0x3fffffffu, 0x7fffffffu, 0x80000000u,
                                         0xffffffffu, 0x7f7f7f7fu };
        static uint32_t p[16];                                                  /* static: no memset */
        OpzMix mm, ms;
        static fract32 xm[OPZ_BLOCK_FRAMES], xs[2 * OPZ_BLOCK_FRAMES], om[2 * OPZ_BLOCK_FRAMES],
            os[2 * OPZ_BLOCK_FRAMES];
        p[OPZ_P_LEVEL] = 0x7fffffffu;
        p[OPZ_P_PAN] = 0x40000000u;
        opz_mix_start(&mm, p);
        opz_mix_start(&ms, p);
        for (int j = 0; j < 600; j++) {
            uint32_t k = rnd() % 8;
            if (k == 0) {                                                       /* unity (level 255, centre) */
                p[OPZ_P_LEVEL] = 0x7fffffffu;
                p[OPZ_P_PAN] = 0x40000000u;
            } else if (k <= 2) {
                p[OPZ_P_LEVEL] = rnd();
                p[OPZ_P_PAN] = rnd();
            } else if (k == 3) {
                p[OPZ_P_LEVEL] = k_lp[rnd() % (sizeof k_lp / sizeof k_lp[0])];
                p[OPZ_P_PAN] = k_lp[rnd() % (sizeof k_lp / sizeof k_lp[0])];
            } else if (k == 4 && (rnd() & 1)) {
                opz_mix_start(&mm, p);                                          /* a note-on: no ramp */
                opz_mix_start(&ms, p);
            }                                                                   /* else: unchanged, steady */
            int quiet = (rnd() % 4) == 0;                                       /* small values: no saturation */
            for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
                xs[i] = quiet ? (int32_t)rnd() >> 4 : pick(i);
                om[i] = quiet ? (int32_t)rnd() >> 4 : pick(i + 1);
                os[i] = quiet ? (int32_t)rnd() >> 4 : pick(i + 2);
                if (i < OPZ_BLOCK_FRAMES) xm[i] = quiet ? (int32_t)rnd() >> 4 : pick(i + 3);
            }
            opz_mix_mono(om, xm, &mm, p);
            opz_mix_stereo(os, xs, &ms, p);
            for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES; i++) {
                mix((uint32_t)om[i]);
                mix((uint32_t)os[i]);
            }
            mix((uint32_t)(uint16_t)mm.l << 16 | (uint16_t)mm.r);
            mix((uint32_t)(uint16_t)ms.l << 16 | (uint16_t)ms.r);
        }
    }
    line("opz_mix_mono opz_mix_stereo");
    put("done\n", 5);
    return 0;
}
