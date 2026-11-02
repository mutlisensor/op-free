/* Host check: streams the five shared tables as opzsim passes them through the callback table, then the outputs
 * of the opz_tables.h accessors, into the track buffer (one word per value, 128 per block), so tests/golden.py can
 * compare them with tools/opz_tables.py (formats of sdk/docs/engine-abi.md §5). Stream order: k_lp_table (129),
 * g_sine (8,193), g_sineTable (256), rFilterCoeffs b0, b1/2, b2, a1/2, a2 (5 x 585), releaseTable (128); then
 * opz_host_sine_lerp at 512 phases h(n), opz_host_sine16_lerp at 256 phases h(n), opz_host_rf_lookup and
 * opz_host_rf_lookup_os at 64 (cutoff, res) pairs (h(2p), h(2p+1); 10 words each), 256 samples of opz_host_biquad on (int32) h(n) >> 3 with the OS-rule
 * coefficients of bytes 60 / 100, and opz_host_tables_check's mask. The voice ends after the last word. */
#include "opz_tables.h"

#define N_RAW     (OPZ_KLP_LEN + OPZ_SINE_LEN + 1 + OPZ_SINE16_LEN + 5 * OPZ_RF_ROWS * OPZ_RF_COLS + OPZ_RELEASE_LEN)
#define N_LERP    512
#define N_LERP16  256
#define N_RF      64
#define N_BIQUAD  256
#define N_TOTAL   (N_RAW + N_LERP + N_LERP16 + 10 * N_RF + N_BIQUAD + 1)

typedef struct { uint32_t pos; OpzBiquadState bq; } Voice;

static const ModuleCallbackInterface *s_cb;

uint32_t initModule(uint32_t v, const ModuleCallbackInterface *cb, uint32_t *m) { (void)v; s_cb = cb; *m = 0; return 1; }
void init(void *s, uint32_t t, uint32_t a, void *mem, void *scr) { (void)t; (void)a; (void)mem; (void)scr; ((Voice *)s)->pos = 0; }
void onNoteOn(int n, uint8_t vel, void *s, const uint32_t *p)
{
    Voice *v = s;
    (void)n; (void)vel; (void)p;
    v->pos = 0;
    v->bq.s1 = v->bq.s2 = 0;
}
void onNoteOff(int n, void *s) { (void)n; (void)s; }

static uint32_t h(uint32_t n)
{
    n *= 0x9E3779B1u;
    n ^= n >> 15;
    n *= 0x85EBCA77u;
    return n ^ (n >> 13);
}

static fract32 word(Voice *v, uint32_t k)
{
    const ModuleCallbackInterface *cb = s_cb;
    if (k < OPZ_KLP_LEN) return cb->k_lp_table[k];
    k -= OPZ_KLP_LEN;
    if (k < OPZ_SINE_LEN + 1) return cb->g_sine[k];
    k -= OPZ_SINE_LEN + 1;
    if (k < OPZ_SINE16_LEN) return cb->g_sineTable[k];
    k -= OPZ_SINE16_LEN;
    if (k < 5 * OPZ_RF_ROWS * OPZ_RF_COLS) return cb->rFilterCoeffs[k / (OPZ_RF_ROWS * OPZ_RF_COLS)][k % (OPZ_RF_ROWS * OPZ_RF_COLS)];
    k -= 5 * OPZ_RF_ROWS * OPZ_RF_COLS;
    if (k < OPZ_RELEASE_LEN) return cb->releaseTable[k];
    k -= OPZ_RELEASE_LEN;
    if (k < N_LERP) return opz_host_sine_lerp(cb->g_sine, h(k));
    k -= N_LERP;
    if (k < N_LERP16) return opz_host_sine16_lerp(cb->g_sineTable, h(k));
    k -= N_LERP16;
    if (k < 10 * N_RF) {
        fract32 c[5];
        uint32_t p = k / 10;
        if (k % 10 < 5) opz_host_rf_lookup(cb->rFilterCoeffs, h(2 * p), h(2 * p + 1), c);
        else opz_host_rf_lookup_os(cb->rFilterCoeffs, h(2 * p), h(2 * p + 1), c);
        return c[k % 5];
    }
    k -= 10 * N_RF;
    if (k < N_BIQUAD) {
        fract32 c[5];
        opz_host_rf_lookup_os(cb->rFilterCoeffs, opz_param_from_byte(60), opz_param_from_byte(100), c);
        return opz_host_biquad(&v->bq, c, (fract32)h(k) >> 3);
    }
    return (fract32)opz_host_tables_check(cb);
}

void render(fract32 *out, uint32_t pitch, int32_t slope, void *state, const uint32_t *p, ADSR *env)
{
    Voice *v = state;
    (void)pitch; (void)slope; (void)p;
    if (!OPZ_ADSR_STAGE(env)) return;
    for (int i = 0; i < 2 * OPZ_BLOCK_FRAMES && v->pos < N_TOTAL; i++)
        out[i] = opz_add_q31(out[i], word(v, v->pos++));
    if (v->pos >= N_TOTAL) OPZ_ADSR_STAGE(env) = 0;
}

OPZ_EXPORTS_INSTRUMENT();
