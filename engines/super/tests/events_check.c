/* Host check of super's slope + event integration against a direct per-sample sum of seven opz_saw_blep saws,
 * over random pitches (C0 .. C7), detunes, weights and phases (every 7th trial starts all saws just before a wrap).
 * Fails above 1e-5 of full scale; the difference is the SDK reciprocal's rounding (~1e-6 at high notes).
 *
 *   clang -O2 -I sdk/include -o /tmp/super_events engines/super/tests/events_check.c && /tmp/super_events
 */
#include <stdio.h>
#include <stdlib.h>
#include "../super.c"
int main(void)
{
    double worst = 0; long blocks = 0;
    uint32_t seed = 1;
    for (int trial = 0; trial < 20000; trial++) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t base = 0x184CBB * (1u << (seed >> 29)) + (seed & 0xfffff);   /* C0 .. C7-ish */
        seed = seed * 1664525u + 1013904223u;
        int32_t detune = (int32_t)(seed >> 1);
        int32_t gc = 0x10000000 + (int32_t)(seed & 0xffffff), gs = 0x0c000000;
        const int32_t wm[SAWS] = { gc, gs, gs, gs, gs, gs, gs };
        const int32_t ws[SAWS] = { 0, gs - (gs >> 2), (gs >> 2) - gs, gs, -gs, gs >> 1, -(gs >> 1) };
        uint32_t ph[SAWS], inc[SAWS], rc[SAWS];
        for (int k = 0; k < SAWS; k++) {
            seed = seed * 1664525u + 1013904223u; ph[k] = seed;
            if (trial % 7 == 0) ph[k] = (uint32_t)-(int32_t)(k * 1000);      /* near the wrap */
            inc[k] = base + (uint32_t)opz_mul_q31((int32_t)base, opz_mul_q31(detune, k_offset[k]));
            rc[k] = opz_blep_recip(inc[k]);
        }
        for (int b = 0; b < 4; b++, blocks++) {
            uint32_t m = 0, dm = 0, sd = 0, ds = 0;
            for (int k = 0; k < SAWS; k++) {
                int32_t y = (int32_t)(ph[k] ^ 0x80000000u);
                m += (uint32_t)opz_mul_q31(wm[k], y); sd += (uint32_t)opz_mul_q31(ws[k], y);
                dm += (uint32_t)opz_mul_q31(wm[k], (int32_t)inc[k]); ds += (uint32_t)opz_mul_q31(ws[k], (int32_t)inc[k]);
                saw_events(ph[k], inc[k], rc[k], wm[k], ws[k]);
            }
            m -= dm; sd -= ds;
            for (int i = 0; i < N; i++) {
                m += dm + (uint32_t)s_em[i]; sd += ds + (uint32_t)s_es[i]; s_em[i] = s_es[i] = 0;
                double rm = 0, rs = 0;
                for (int k = 0; k < SAWS; k++) {
                    double v = opz_saw_blep(ph[k] + inc[k] * (uint32_t)i, inc[k], rc[k]) / 2147483648.0;
                    rm += v * wm[k] / 2147483648.0; rs += v * ws[k] / 2147483648.0;
                }
                double e1 = rm - (int32_t)m / 2147483648.0, e2 = rs - (int32_t)sd / 2147483648.0;
                if (e1 < 0) e1 = -e1;
                if (e2 < 0) e2 = -e2;
                if (e1 > worst) worst = e1;
                if (e2 > worst) worst = e2;
                if (e1 > 1e-5 || e2 > 1e-5) {
                    printf("FAIL trial %d block %d sample %d: error %g (mid) %g (side)\n", trial, b, i, e1, e2);
                    return 1;
                }
            }
            for (int k = 0; k < SAWS; k++) ph[k] += inc[k] * N;
        }
    }
    printf("ok: %ld blocks, worst error %.3g (%.1f dBFS)\n", blocks, worst, 20 * __builtin_log10(worst + 1e-30));
    return 0;
}
