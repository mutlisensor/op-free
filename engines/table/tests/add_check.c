/*
 * add_check.c — table's additive path on pseudo-random and edge inputs, one FNV-1a checksum line per part: the
 * 2 x 16-bit lane helpers, sincos at every 16-bit phase, and add_setup + add_static / add_ramp over random morph
 * positions, bands, phases, slopes and gains.
 *
 * Built twice by tools/tests/test_table.py, like tools/cycles/sdk_check.c: for the host (cc, what opzsim plays) and
 * for the Blackfin (bfin-elf-gcc with the device flags, run in bfin-elf-run). Both must print the same lines.
 * MIT licence, see CONTRIBUTING.md.
 */
#include "../table.c"

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
#define word_of(v) ((uint32_t)(v))
#else
#include <stdio.h>
static void put(const char *s, uint32_t n) { fwrite(s, 1, n, stdout); }
#define word_of(v) (v)
#endif

static uint32_t s_seed = 12345;
static uint32_t rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    uint32_t hi = s_seed & 0xffff0000u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return hi | (s_seed >> 16);
}

/* Random words, with every fourth lane taken from the edges. */
static const int16_t k_edges[] = { 0, 1, -1, 2, -2, 32767, -32768, 32766, -32767, 16384, -16384, 16383 };
static uint32_t lanes(void)
{
    uint32_t w = rnd();
    if ((w & 3) == 0) w = (w & 0xffff0000u) | (uint16_t)k_edges[(w >> 8) % 12];
    if (((w >> 2) & 3) == 0) w = (w & 0xffffu) | ((uint32_t)(uint16_t)k_edges[(w >> 12) % 12] << 16);
    return w;
}

static uint32_t s_hash = 2166136261u;
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
    for (int i = 0; i < 20000; i++) {
        v2 a = v2_of(lanes()), b = v2_of(lanes());
        mix(word_of(vmul(a, b)));
        mix(word_of(vadds(a, b)));
        mix(word_of(vsubs(a, b)));
        mix(word_of(vadd(a, b)));
        mix(word_of(vabs(a)));
        mix(word_of(vdup_lo(a)));
        mix(word_of(vdup_hi(a)));
        mix((uint32_t)vsum(a));
        mix((uint32_t)subs16((int16_t)rnd(), (int16_t)rnd()));
    }
    line("lane helpers");

    for (uint32_t t = 0; t < 65536; t++) mix(word_of(sincos((t << 16) | (rnd() & 0xffff), k_add)));
    line("sincos");

    static State st;
    static fract32 out[2 * N];
    int16_t g[N];
    st.amp = 1 + (int32_t)(rnd() % 32767);
    for (int b = 0; b < 3000; b++) {
        int r = (int)(rnd() % 8);
        int32_t xe = r == 0 ? st.x : (int32_t)(rnd() % (XMAX + 1)), ye = r == 0 ? st.y : (int32_t)(rnd() % (YMAX + 1));
        uint32_t be = r < 2 ? st.bkey : rnd() % (INC_MAX + 1);
        if (r == 7) st.amp = 1 + (int32_t)(rnd() % 32767);
        int ramp = add_setup(&st, xe, ye, be, (int)(rnd() & 1) || r < 3);
        st.x = xe;
        st.y = ye;
        for (int k = 0; k < TW_ADD_H; k++) {
            mix(s_co[0][k]);
            mix(s_co[1][k]);
        }
        mix((uint32_t)ramp);
        for (int i = 0; i < N; i++) g[i] = (int16_t)(rnd() & 0x7fff);
        for (int i = 0; i < 2 * N; i++) out[i] = (fract32)rnd() >> (rnd() & 7);
        uint32_t inc = rnd() % (INC_MAX + 1), ph = rnd();
        int32_t slope = (int32_t)((rnd() % (INC_MAX + 1)) - inc) >> 6;
        if (ramp) add_ramp(out, ph, inc, slope, g);
        else add_static(out, ph, inc, slope, g);
        for (int i = 0; i < 2 * N; i++) mix((uint32_t)out[i]);
    }
    line("add_setup add_static add_ramp");
    put("done\n", 5);
    return 0;
}
