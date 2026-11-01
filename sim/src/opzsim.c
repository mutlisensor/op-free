/*
 * opzsim — run an OP-Z engine (built for macOS) with the call pattern described in sdk/docs/engine-abi.md:
 * 44.1 kHz, 64-frame blocks, 18 Q31 parameters smoothed per block, Q32 pitch with a per-sample slope,
 * voices per track, instruments adding into a shared track buffer, effects in place on stereo buffers.
 *
 *   opzsim -e build/engines/opz_test_sine.dylib --slot 57 -o out.wav scenario.scn
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

typedef struct Options {
    const char *engine, *fx, *out, *stats, *scenario;
    int bits, slot, trackmask, fx_slot, fx_trackmask, generous, check_add, verbose;
} Options;


static void usage(void)
{
    fprintf(stderr,
            "usage: opzsim [options] SCENARIO\n"
            "  -e, --engine PATH     engine dylib (instrument or effect)\n"
            "      --fx PATH         effect dylib placed after the instrument (with -e instrument)\n"
            "  -o, --out PATH        write a stereo WAV\n"
            "      --bits 16|32      WAV sample size (default 32)\n"
            "      --stats PATH      write render statistics as JSON\n"
            "      --slot N          memory slot of -e, for the static-size check\n"
            "      --trackmask M     trackmask of -e (decimal or 0x..), checked against the scenario\n"
            "      --fx-slot N, --fx-trackmask M   the same for --fx\n"
            "      --generous-chords 6 voices on the chord track\n"
            "      --no-check-add    skip the render-adds-into-out probe\n"
            "  -v, --verbose         log callbacks and events\n");
}

static int parse_args(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof *o);
    o->bits = 32;
    o->slot = o->fx_slot = -1;
    o->trackmask = o->fx_trackmask = -1;
    o->check_add = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define VAL() (i + 1 < argc ? argv[++i] : (usage(), exit(1), ""))
        if (!strcmp(a, "-e") || !strcmp(a, "--engine")) o->engine = VAL();
        else if (!strcmp(a, "--fx")) o->fx = VAL();
        else if (!strcmp(a, "-o") || !strcmp(a, "--out")) o->out = VAL();
        else if (!strcmp(a, "--bits")) o->bits = atoi(VAL());
        else if (!strcmp(a, "--stats")) o->stats = VAL();
        else if (!strcmp(a, "--slot")) o->slot = atoi(VAL());
        else if (!strcmp(a, "--trackmask")) o->trackmask = (int)strtol(VAL(), NULL, 0);
        else if (!strcmp(a, "--fx-slot")) o->fx_slot = atoi(VAL());
        else if (!strcmp(a, "--fx-trackmask")) o->fx_trackmask = (int)strtol(VAL(), NULL, 0);
        else if (!strcmp(a, "--generous-chords")) o->generous = 1;
        else if (!strcmp(a, "--no-check-add")) o->check_add = 0;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) o->verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); exit(0); }
        else if (a[0] == '-') { fprintf(stderr, "error   unknown option %s\n", a); usage(); return -1; }
        else o->scenario = a;
#undef VAL
    }
    if (!o->scenario || !o->engine || (o->bits != 16 && o->bits != 32)) {
        usage();
        return -1;
    }
    return 0;
}

static void report_static(const Engine *e, FILE *js, int *first)
{
    const SlotSize *s = &e->size;
    uint64_t est = s->code + s->rodata + s->data + s->bss + 8 + 8 * 6 + 48; /* + export table and names */
    int cls = engine_slot_bytes(e->slot);
    if (s->not_measured) {
        fprintf(stderr, "static  %s: not measured in the wasm build (the engine is linked into the host); "
                "tools/opz_build.py measures the device build\n", e->name);
    } else if (!e->size_ok) {
        fprintf(stderr, "warning %s: could not read Mach-O sections; static size unknown\n", e->name);
    } else {
        fprintf(stderr, "static  %s: ~%llu B (code %llu, rodata %llu, data %llu, bss %llu) [host build: estimate]\n",
                e->name, (unsigned long long)est, (unsigned long long)s->code, (unsigned long long)s->rodata,
                (unsigned long long)s->data, (unsigned long long)s->bss);
        if (cls < 0)
            fprintf(stderr, "warning %s: no valid --slot, static size not checked against a slot class\n", e->name);
        else if (est > (uint64_t)cls)
            fprintf(stderr, "warning %s: ~%llu B exceeds the %d KB window of slot %d\n", e->name,
                    (unsigned long long)est, cls / 1024, e->slot);
        else
            fprintf(stderr, "static  %s: slot %d (%d KB class), %.0f%% used\n", e->name, e->slot, cls / 1024,
                    100.0 * (double)est / cls);
        if (s->has_constructors)
            fprintf(stderr, "warning %s: static constructors present; the OP-Z never runs them\n", e->name);
        if (s->undefined[0])
            fprintf(stderr, "warning %s: imports %s (no libc on the device)\n", e->name, s->undefined);
    }
    fprintf(js, "%s\n    \"%s\": {\"slot\": %d, \"class_bytes\": %d, \"estimate_bytes\": %llu, \"code\": %llu, "
            "\"rodata\": %llu, \"data\": %llu, \"bss\": %llu, \"imports\": \"%s\"}",
            *first ? "" : ",", e->name, e->slot, cls, (unsigned long long)est, (unsigned long long)s->code,
            (unsigned long long)s->rodata, (unsigned long long)s->data, (unsigned long long)s->bss, s->undefined);
    *first = 0;
}
int main(int argc, char **argv)
{
    Options o;
    if (parse_args(argc, argv, &o)) return 1;
    g_seq.verbose = o.verbose;

    static ModuleCallbackInterface cb;
    sim_callbacks_init(&cb);

    Engine eng, fxe;
    memset(&eng, 0, sizeof eng);
    memset(&fxe, 0, sizeof fxe);
    if (engine_load(&eng, o.engine, &cb)) return 2;
    eng.slot = o.slot;
    eng.trackmask = o.trackmask;
    Engine *inst = eng.is_fx ? NULL : &eng;
    Engine *fx = eng.is_fx ? &eng : NULL;
    if (o.fx) {
        if (fx) { fprintf(stderr, "error   --fx needs an instrument as -e\n"); return 1; }
        if (engine_load(&fxe, o.fx, &cb)) return 2;
        if (!fxe.is_fx) { fprintf(stderr, "error   %s is not an effect (no renderFX)\n", fxe.name); return 2; }
        fxe.slot = o.fx_slot;
        fxe.trackmask = o.fx_trackmask;
        fx = &fxe;
    }

    Scenario sc;
    if (scenario_load(&sc, o.scenario, inst ? OPZ_TRACK_LEAD : -1, OPZ_TRACK_FX1)) return 1;
    if (fx && sc.input == IN_INSTRUMENT && !inst) {
        fprintf(stderr, "error   input instrument needs an instrument engine\n");
        return 1;
    }
    if (inst && fx && sc.input == IN_SILENCE) sc.input = IN_INSTRUMENT;

    /* trackmask checks */
    for (int t = 0; t < SIM_TRACKS; t++) {
        if (!sc.track_used[t]) continue;
        if (t >= 12) { fprintf(stderr, "warning track %s is a control track; ignored\n", track_name(t)); continue; }
        Engine *e = t < 8 ? inst : fx;
        if (t >= 8 && t != sc.fx_track) continue;
        if (!e) {
            fprintf(stderr, "warning track %s has no %s engine loaded; its events are ignored\n", track_name(t),
                    t < 8 ? "instrument" : "effect");
            continue;
        }
        if (e->trackmask >= 0 && !(e->trackmask & (1 << t)))
            fprintf(stderr, "warning %s: track %s is outside trackmask 0x%x\n", e->name, track_name(t), e->trackmask);
    }

    if (host_setup(inst, fx, &sc, o.generous, o.check_add)) return 1;

    int64_t frames = sc.frames, nblocks = frames / OPZ_BLOCK_FRAMES;
    fract32 *out = calloc((size_t)frames * 2, sizeof(fract32));
    int *active_per_block = calloc((size_t)nblocks + 1, sizeof(int));
    int ev = 0;
    HostInput in;
    host_input_init(&sc, &in);

    for (int64_t b = 0; b < nblocks; b++) {
        int64_t t0 = b * OPZ_BLOCK_FRAMES;

        /* events due by the start of this block (the sequencer runs between blocks) */
        for (; ev < sc.nevents && sc.events[ev].time <= t0; ev++) host_event(&sc, &sc.events[ev], o.verbose);

        sim_seq_clock(t0);                 /* sequencer ticks due by this block (keys 5-7) */

        /* parameter sweeps, then the block (Synth::interpolateParameters first) */
        for (int s = 0; s < sc.nsweeps; s++) {
            const SimSweep *w = &sc.sweeps[s];
            if (t0 < w->start || t0 >= w->end + OPZ_BLOCK_FRAMES) continue;
            int64_t pos = t0 < w->end ? t0 : w->end;
            double x = w->from + (double)(w->to - w->from) * (double)(pos - w->start) / (double)(w->end - w->start);
            host_set_param(w->track, w->index, (int)(x + 0.5));
        }
        fract32 l[OPZ_BLOCK_FRAMES] = { 0 }, r[OPZ_BLOCK_FRAMES] = { 0 };
        host_block(&sc, &in, t0, l, r);
        active_per_block[b] = g_host.active;
        for (int i = 0; i < OPZ_BLOCK_FRAMES; i++) {
            out[(t0 + i) * 2] = l[i];
            out[(t0 + i) * 2 + 1] = r[i];
        }
    }
    active_per_block[nblocks] = active_per_block[nblocks ? nblocks - 1 : 0];

    /* ---- report -------------------------------------------------------------------------------------- */
    int rc = 0;
    if (o.out && wav_write(o.out, out, frames, o.bits)) {
        fprintf(stderr, "error   cannot write %s\n", o.out);
        rc = 1;
    }
    SimStats st;
    stats_compute(out, frames, &st);

    fprintf(stderr, "render  %s: %lld frames (%.2f s), peak %.1f dBFS, rms %.1f dBFS, %lld clipped\n",
            sc.path, (long long)frames, (double)frames / OPZ_SAMPLE_RATE, st.peak_dbfs, st.rms_dbfs,
            (long long)st.clipped);

    int expect_failed = 0;
    for (int i = 0; i < sc.nexpects; i++) {
        const SimExpect *x = &sc.expects[i];
        int ok = 1;
        double v = 0;
        char what[128];
        int64_t from = x->from < frames ? x->from : frames, to = x->to < frames ? x->to : frames;
        switch (x->type) {
        case EXP_SOUND:
            v = segment_rms_dbfs(out, from, to);
            ok = v > -60.0;
            snprintf(what, sizeof what, "sound in samples %lld-%lld (rms %.1f dBFS > -60)", (long long)from,
                     (long long)to, v);
            break;
        case EXP_SILENCE:
            v = segment_peak_dbfs(out, from, to);
            ok = v < -90.0;
            snprintf(what, sizeof what, "silence in samples %lld-%lld (peak %.1f dBFS < -90)", (long long)from,
                     (long long)to, v);
            break;
        case EXP_IDLE: {
            int64_t blk = (x->from + OPZ_BLOCK_FRAMES - 1) / OPZ_BLOCK_FRAMES;
            if (blk > nblocks) blk = nblocks;
            int n = active_per_block[blk ? blk - 1 : 0];
            ok = n == 0;
            snprintf(what, sizeof what, "all voices free by sample %lld (%d active)", (long long)x->from, n);
            break;
        }
        case EXP_UNCHANGED:
            ok = fx && g_host.unchanged;
            snprintf(what, sizeof what, "effect output identical to its input");
            break;
        case EXP_PEAK_BELOW:
            ok = st.peak_dbfs < x->value;
            snprintf(what, sizeof what, "peak %.1f dBFS below %.1f", st.peak_dbfs, x->value);
            break;
        }
        fprintf(stderr, "expect  line %d: %s: %s\n", x->line, what, ok ? "ok" : "FAILED");
        expect_failed += !ok;
    }

    int active_end = active_per_block[nblocks];
    if (inst)
        for (int t = 0; t < SIM_INSTR_TRACKS; t++)
            if (g_tracks[t].nvoices) {
                int n = 0;
                for (int v = 0; v < g_tracks[t].nvoices; v++) n += g_tracks[t].voices[v].active;
                fprintf(stderr, "voices  %s: %d of %d used at most, %d active at end\n", track_name(t),
                        g_tracks[t].max_used, g_tracks[t].nvoices, n);
            }
    if (g_host.voices_stolen) fprintf(stderr, "voices  %d note(s) stole a voice\n", g_host.voices_stolen);
    if (inst && active_end)
        fprintf(stderr, "warning %s: %d voice(s) still active at the end; the envelope must reach stage 0\n",
                inst->name, active_end);

    double inst_nps = g_host.voice_blocks ? g_host.inst_ns / ((double)g_host.voice_blocks * OPZ_BLOCK_FRAMES) : 0;
    double fx_nps = fx && nblocks ? g_host.fx_ns / ((double)nblocks * OPZ_BLOCK_FRAMES) : 0;
    if (inst) fprintf(stderr, "time    %s: %.1f ns/sample/voice (host CPU, not Blackfin cycles)\n", inst->name, inst_nps);
    if (fx) fprintf(stderr, "time    %s: %.1f ns/sample (host CPU, not Blackfin cycles)\n", fx->name, fx_nps);
    fprintf(stderr, "budget  state %d B/voice, %d B/FX: %s\n", OPZ_VOICE_STATE_BYTES, OPZ_FX_STATE_BYTES,
            g_host.state_violations ? "VIOLATED" : "ok");

    FILE *js = o.stats ? fopen(o.stats, "w") : NULL;
    if (o.stats && !js) {
        fprintf(stderr, "error   cannot write %s\n", o.stats);
        rc = 1;
    }
    FILE *jsf = js ? js : fopen("/dev/null", "w");
    fprintf(jsf, "{\n  \"scenario\": \"%s\",\n  \"frames\": %lld,\n  \"bits\": %d,\n", sc.path, (long long)frames, o.bits);
    fprintf(jsf, "  \"peak_dbfs\": %.3f,\n  \"rms_dbfs\": %.3f,\n  \"dc\": %.6g,\n  \"clipped\": %lld,\n",
            st.peak_dbfs, st.rms_dbfs, st.dc, (long long)st.clipped);
    fprintf(jsf, "  \"bands_db\": [");
    for (int i = 0; i < 10; i++) fprintf(jsf, "%s%.3f", i ? ", " : "", st.bands_db[i]);
    fprintf(jsf, "],\n  \"voices_stolen\": %d,\n  \"voices_active_at_end\": %d,\n", g_host.voices_stolen, active_end);
    fprintf(jsf, "  \"state_violations\": %d,\n  \"add_violations\": %d,\n  \"expectations_failed\": %d,\n",
            g_host.state_violations, g_host.add_violations, expect_failed);
    fprintf(jsf, "  \"host_ns_per_sample\": {\"instrument\": %.2f, \"fx\": %.2f},\n  \"static\": {", inst_nps, fx_nps);
    int first = 1;
    report_static(&eng, jsf, &first);
    if (o.fx) report_static(&fxe, jsf, &first);
    fprintf(jsf, "\n  }\n}\n");
    fclose(jsf);

    if (rc) return rc;
    if (g_host.state_violations || g_host.add_violations) return 4;
    if (expect_failed) return 3;
    return 0;
}
