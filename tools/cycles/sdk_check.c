/*
 * sdk_check.c — the SDK's per-sample helpers (sdk/include/opz_engine.h, opz_dsp.h) on pseudo-random and edge
 * inputs, one FNV-1a checksum line per helper.
 *
 * Built twice by tools/tests/test_opz_cycles.py: for the host (cc, the arithmetic opzsim's engines get) and for the
 * Blackfin (bfin-elf-gcc with the device flags, run in bfin-elf-run). Both must print the same lines: then opzsim
 * renders the bits the device renders, for every helper listed here. opz_mul_q31_rnd (opz_adsr.h) is left out on
 * purpose: its host version rounds, the Blackfin's fractional multiply rounds down.
 * MIT licence, see CONTRIBUTING.md.
 */
#include "opz_dsp.h"
#include "opz_engine.h"

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
    put("done\n", 5);
    return 0;
}
