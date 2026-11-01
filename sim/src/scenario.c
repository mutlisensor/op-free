/*
 * scenario.c — the opzsim scenario format: one directive per line, '#' starts a comment.
 * See sim/README.md for the full list.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host.h"

static const char *k_tracks[SIM_TRACKS] = {
    "kick", "snare", "perc", "sample", "bass", "lead", "arp", "chord",
    "fx1", "fx2", "tape", "master", "perform", "module", "lights", "motion",
};

static const char *k_params[OPZ_NUM_PARAMS] = {
    "p1", "p2", "attack", "decay", "sustain", "release", "fx1", "fx2", "filter",
    "resonance", "pan", "level", "portamento", "lfo_depth", "lfo_speed", "lfo_target", "lfo_shape", "note_style",
};

const char *track_name(int t) { return t >= 0 && t < SIM_TRACKS ? k_tracks[t] : "?"; }

int64_t scenario_step_to_sample(const Scenario *sc, double step)
{
    double s = step * sc->step_samples;
    return (int64_t)(s + 0.5);
}

static int parse_track(const char *s)
{
    for (int i = 0; i < SIM_TRACKS; i++)
        if (!strcasecmp(s, k_tracks[i])) return i;
    char *end;
    long n = strtol(s, &end, 10);
    if (!*end && n >= 1 && n <= SIM_TRACKS) return (int)n - 1;
    return -1;
}

static int parse_param(const char *s)
{
    for (int i = 0; i < OPZ_NUM_PARAMS; i++)
        if (!strcasecmp(s, k_params[i])) return i;
    char *end;
    long n = strtol(s, &end, 10);
    if (!*end && n >= 0 && n < OPZ_NUM_PARAMS) return (int)n;
    return -1;
}

/* MIDI note number or a name: C4 = 60, A#3, Eb2, C-1 = 0. */
static int parse_note(const char *s)
{
    char *end;
    long n = strtol(s, &end, 10);
    if (!*end) return n >= 0 && n <= 127 ? (int)n : -1;
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 }; /* A..G */
    char c = (char)toupper((unsigned char)s[0]);
    if (c < 'A' || c > 'G') return -1;
    int v = base[c - 'A'];
    s++;
    if (*s == '#') { v++; s++; } else if (*s == 'b') { v--; s++; }
    long oct = strtol(s, &end, 10);
    if (end == s || *end) return -1;
    v += (int)(oct + 1) * 12;
    return v >= 0 && v <= 127 ? v : -1;
}

static int parse_num(const char *s, double *out)
{
    char *end;
    *out = strtod(s, &end);
    return end != s && !*end;
}

static int parse_byte(const char *s, int *out)
{
    double v;
    if (!parse_num(s, &v) || v < 0 || v > 255 || v != (int)v) return -1;
    *out = (int)v;
    return 0;
}

/* `name` relative to the scenario's directory unless absolute. */
static void relative_path(const char *scenario, const char *name, char *out, size_t n)
{
    if (name[0] == '/') {
        snprintf(out, n, "%s", name);
        return;
    }
    const char *slash = strrchr(scenario, '/');
    int dir = slash ? (int)(slash - scenario + 1) : 0;
    snprintf(out, n, "%.*s%s", dir, scenario, name);
}

static SimEvent *new_event(Scenario *sc)
{
    if (sc->nevents == sc->cap_events) {
        sc->cap_events = sc->cap_events ? sc->cap_events * 2 : 64;
        sc->events = realloc(sc->events, sizeof(SimEvent) * (size_t)sc->cap_events);
    }
    SimEvent *e = &sc->events[sc->nevents];
    memset(e, 0, sizeof *e);
    e->seq = sc->nevents++;
    return e;
}

static int cmp_event(const void *a, const void *b)
{
    const SimEvent *x = a, *y = b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    if (x->order != y->order) return x->order - y->order;
    return x->seq - y->seq;
}

#define ERR(...) do { fprintf(stderr, "error   %s:%d: ", path, line); fprintf(stderr, __VA_ARGS__); \
                      fputc('\n', stderr); fclose(f); return -1; } while (0)
#define NEED(n) do { if (argc != (n)) ERR("%s expects %d argument(s)", argv[0], (n) - 1); } while (0)

void scenario_init(Scenario *sc, int default_fx_track)
{
    memset(sc, 0, sizeof *sc);
    sc->tempo = 120;
    sc->steps = 16;
    sc->tail = 1.0;
    sc->fx_track = default_fx_track;
    sc->gain = SIM_FULL;
    sc->input = IN_SILENCE;
    for (int t = 0; t < SIM_TRACKS; t++) {
        sc->init_params[t][OPZ_P_SUSTAIN] = 255;
        sc->init_params[t][OPZ_P_PAN] = 128;
        sc->init_params[t][OPZ_P_LEVEL] = 200;
        sc->init_params[t][OPZ_P_FILTER] = 255;
    }
    sc->step_samples = OPZ_SAMPLE_RATE * 15.0 / sc->tempo;
}

int scenario_load(Scenario *sc, const char *path, int default_track, int default_fx_track)
{
    scenario_init(sc, default_fx_track);
    snprintf(sc->path, sizeof sc->path, "%s", path);
    int track = default_track, explicit_track = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error   cannot open scenario %s\n", path);
        return -1;
    }

    /* pass 1 needs tempo before steps are converted: collect lines first */
    char *lines[4096];
    int nlines = 0;
    char buf[1024];
    while (fgets(buf, sizeof buf, f) && nlines < 4096) lines[nlines++] = strdup(buf);
    for (int i = 0; i < nlines; i++) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", lines[i]);
        char *tok = strtok(tmp, " \t\r\n");
        if (tok && !strcmp(tok, "tempo")) {
            char *v = strtok(NULL, " \t\r\n");
            double bpm;
            if (v && parse_num(v, &bpm) && bpm >= 20 && bpm <= 400) sc->tempo = bpm;
        }
    }
    sc->step_samples = OPZ_SAMPLE_RATE * 15.0 / sc->tempo; /* sixteenth notes */
    g_seq.tempo = (int)(sc->tempo + 0.5);
    g_seq.clock_bpm = sc->tempo;

    int note_id = 0;
    for (int line = 1; line <= nlines; line++) {
        char *s = lines[line - 1];
        char *hash = strchr(s, '#');
        if (hash) *hash = 0;
        char *argv[32];
        int argc = 0;
        for (char *t = strtok(s, " \t\r\n"); t && argc < 32; t = strtok(NULL, " \t\r\n")) argv[argc++] = t;
        if (!argc) continue;
        const char *cmd = argv[0];
        double d, d2, d3;

        if (!strcmp(cmd, "tempo")) {
            NEED(2);
            if (!parse_num(argv[1], &d) || d < 20 || d > 400) ERR("tempo must be 20..400 BPM");
        } else if (!strcmp(cmd, "steps")) {
            NEED(2);
            if (!parse_num(argv[1], &d) || d <= 0) ERR("bad step count");
            sc->steps = d;
        } else if (!strcmp(cmd, "tail")) {
            NEED(2);
            if (!parse_num(argv[1], &d) || d < 0 || d > 60) ERR("tail is seconds, 0..60");
            sc->tail = d;
        } else if (!strcmp(cmd, "track")) {
            NEED(2);
            if ((track = parse_track(argv[1])) < 0) ERR("unknown track %s", argv[1]);
            sc->track_used[track] = 1;
            explicit_track = 1;
            if (track >= 8 && track <= 11) sc->fx_track = track;
        } else if (!strcmp(cmd, "preset")) {
            NEED(1 + OPZ_NUM_PARAMS);
            if (track < 0) ERR("no track selected");
            for (int i = 0; i < OPZ_NUM_PARAMS; i++) {
                int b;
                if (parse_byte(argv[1 + i], &b)) ERR("preset values are bytes 0..255");
                sc->init_params[track][i] = (uint8_t)b;
            }
        } else if (!strcmp(cmd, "param")) {
            if (argc != 3 && argc != 4) ERR("param NAME VALUE [STEP]");
            if (track < 0) ERR("no track selected");
            int idx = parse_param(argv[1]), b;
            if (idx < 0) ERR("unknown parameter %s", argv[1]);
            if (parse_byte(argv[2], &b)) ERR("parameter values are bytes 0..255");
            d = 0;
            if (argc == 4 && (!parse_num(argv[3], &d) || d < 0)) ERR("bad step");
            if (d == 0) {
                sc->init_params[track][idx] = (uint8_t)b; /* as loaded with the project: no smoothing */
            } else {
                SimEvent *e = new_event(sc);
                e->type = EV_PARAM; e->time = scenario_step_to_sample(sc, d); e->order = 0;
                e->track = track; e->index = idx; e->value = b;
            }
        } else if (!strcmp(cmd, "sweep")) {
            NEED(6);
            if (track < 0) ERR("no track selected");
            int idx = parse_param(argv[1]), b0, b1;
            if (idx < 0) ERR("unknown parameter %s", argv[1]);
            if (parse_byte(argv[2], &b0) || parse_byte(argv[3], &b1)) ERR("sweep values are bytes 0..255");
            if (!parse_num(argv[4], &d) || !parse_num(argv[5], &d2) || d < 0 || d2 <= d) ERR("sweep needs START < END steps");
            if (sc->nsweeps == sc->cap_sweeps) {
                sc->cap_sweeps = sc->cap_sweeps ? sc->cap_sweeps * 2 : 8;
                sc->sweeps = realloc(sc->sweeps, sizeof(SimSweep) * (size_t)sc->cap_sweeps);
            }
            SimSweep *w = &sc->sweeps[sc->nsweeps++];
            w->track = track; w->index = idx; w->from = b0; w->to = b1;
            w->start = scenario_step_to_sample(sc, d);
            w->end = scenario_step_to_sample(sc, d2);
            if (d == 0) sc->init_params[track][idx] = (uint8_t)b0;
        } else if (!strcmp(cmd, "note")) {
            if (argc != 4 && argc != 5) ERR("note STEP NOTE LENGTH [VELOCITY]");
            if (track < 0) ERR("no track selected");
            int n = parse_note(argv[2]), vel = 100;
            if (!parse_num(argv[1], &d) || d < 0) ERR("bad step");
            if (n < 0) ERR("bad note %s", argv[2]);
            if (!parse_num(argv[3], &d2) || d2 <= 0) ERR("bad length");
            if (argc == 5 && (!parse_num(argv[4], &d3) || d3 < 1 || d3 > 127 || d3 != (int)d3)) ERR("velocity is 1..127");
            if (argc == 5) vel = (int)d3;
            SimEvent *on = new_event(sc);
            on->type = EV_NOTE_ON; on->time = scenario_step_to_sample(sc, d); on->order = 1;
            on->track = track; on->note = n; on->velocity = vel; on->id = ++note_id;
            SimEvent *off = new_event(sc);
            off->type = EV_NOTE_OFF; off->time = scenario_step_to_sample(sc, d + d2); off->order = 0;
            off->track = track; off->note = n; off->id = note_id;
        } else if (!strcmp(cmd, "bend")) {
            NEED(3);
            if (track < 0) ERR("no track selected");
            if (!parse_num(argv[1], &d) || d < 0) ERR("bad step");
            if (!parse_num(argv[2], &d2) || d2 < -200 || d2 > 200) ERR("bend is -200..200 cents");
            SimEvent *e = new_event(sc);
            e->type = EV_BEND; e->time = scenario_step_to_sample(sc, d); e->order = 0;
            e->track = track; e->value = (int)d2;
        } else if (!strcmp(cmd, "transport")) {
            NEED(3);
            if (!parse_num(argv[1], &d) || d < 0) ERR("bad step");
            if (strcmp(argv[2], "play") && strcmp(argv[2], "stop")) ERR("transport STEP play|stop");
            SimEvent *e = new_event(sc);
            e->type = EV_TRANSPORT; e->time = scenario_step_to_sample(sc, d); e->order = 0;
            e->value = !strcmp(argv[2], "play");
        } else if (!strcmp(cmd, "gain")) {
            NEED(2);
            if (!parse_num(argv[1], &d) || d < 0 || d > 1) ERR("gain is 0..1");
            sc->gain = d >= 1 ? SIM_FULL : (fract32)(int64_t)(d * 2147483647.0 + 0.5);
        } else if (!strcmp(cmd, "input")) {
            if (argc < 2) ERR("input KIND ...");
            const char *k = argv[1];
            if (!strcmp(k, "silence")) { NEED(2); sc->input = IN_SILENCE; }
            else if (!strcmp(k, "instrument")) { NEED(2); sc->input = IN_INSTRUMENT; }
            else if (!strcmp(k, "sine") || !strcmp(k, "saw")) {
                NEED(4);
                if (!parse_num(argv[2], &sc->in_f0) || !parse_num(argv[3], &sc->in_amp)) ERR("input %s FREQ AMP", k);
                sc->input = k[1] == 'i' ? IN_SINE : IN_SAW;
            } else if (!strcmp(k, "noise")) {
                NEED(3);
                if (!parse_num(argv[2], &sc->in_amp)) ERR("input noise AMP");
                sc->input = IN_NOISE;
            } else if (!strcmp(k, "sweep")) {
                NEED(5);
                if (!parse_num(argv[2], &sc->in_f0) || !parse_num(argv[3], &sc->in_f1) || !parse_num(argv[4], &sc->in_amp))
                    ERR("input sweep F0 F1 AMP");
                sc->input = IN_SWEEP;
            } else if (!strcmp(k, "impulse")) {
                NEED(4);
                if (!parse_num(argv[2], &sc->in_f0) || sc->in_f0 <= 0 || !parse_num(argv[3], &sc->in_amp))
                    ERR("input impulse EVERY_STEPS AMP");
                sc->input = IN_IMPULSE;
            } else ERR("unknown input %s", k);
            if (sc->in_amp < 0 || sc->in_amp > 1) ERR("input amplitude is 0..1");
        } else if (!strcmp(cmd, "preset_file")) {
            NEED(2);
            relative_path(path, argv[1], g_preset_path, sizeof g_preset_path);
        } else if (!strcmp(cmd, "sample")) {
            if (argc != 2 && argc != 3) ERR("sample FILE|builtin|unloaded [SAMPLES_PATH]");
            if (track < 0 || track >= SIM_INSTR_TRACKS) ERR("sample needs an instrument track");
            sc->sample_builtin[track] = !strcmp(argv[1], "builtin") ? SAMPLE_BUILTIN
                                      : !strcmp(argv[1], "unloaded") ? SAMPLE_UNLOADED : 0;
            sc->sample_file[track][0] = 0;
            if (!sc->sample_builtin[track])
                relative_path(path, argv[1], sc->sample_file[track], sizeof sc->sample_file[track]);
            /* default samples path: samplepacks/user/ + the file name, as our tools install packs */
            const char *base = strrchr(argv[1], '/');
            base = base ? base + 1 : argv[1];
            if (argc == 3)
                snprintf(sc->samples_path[track], sizeof sc->samples_path[track], "%s", argv[2]);
            else
                snprintf(sc->samples_path[track], sizeof sc->samples_path[track], "samplepacks/user/%s",
                         sc->sample_builtin[track] == SAMPLE_BUILTIN ? "opzsim_builtin.aif"
                         : sc->sample_builtin[track] == SAMPLE_UNLOADED ? "opzsim_unloaded.aif" : base);
        } else if (!strcmp(cmd, "seq")) {
            NEED(3);
            if (!parse_num(argv[1], &d) || d < 0 || d > 15 || !parse_num(argv[2], &d2)) ERR("seq ID VALUE (id 0..15)");
            g_seq.get_override[(int)d] = (int)d2;
            g_seq.get_set[(int)d] = 1;
        } else if (!strcmp(cmd, "expect")) {
            if (argc < 2) ERR("expect KIND ...");
            if (sc->nexpects == sc->cap_expects) {
                sc->cap_expects = sc->cap_expects ? sc->cap_expects * 2 : 8;
                sc->expects = realloc(sc->expects, sizeof(SimExpect) * (size_t)sc->cap_expects);
            }
            SimExpect *x = &sc->expects[sc->nexpects];
            memset(x, 0, sizeof *x);
            x->line = line;
            const char *k = argv[1];
            if (!strcmp(k, "sound") || !strcmp(k, "silence")) {
                NEED(4);
                if (!parse_num(argv[2], &d) || !parse_num(argv[3], &d2) || d2 <= d) ERR("expect %s FROM TO (steps)", k);
                x->type = k[1] == 'o' ? EXP_SOUND : EXP_SILENCE;
                x->from = scenario_step_to_sample(sc, d);
                x->to = scenario_step_to_sample(sc, d2);
            } else if (!strcmp(k, "idle")) {
                NEED(3);
                if (!parse_num(argv[2], &d)) ERR("expect idle STEP");
                x->type = EXP_IDLE;
                x->from = scenario_step_to_sample(sc, d);
            } else if (!strcmp(k, "unchanged")) {
                NEED(2);
                x->type = EXP_UNCHANGED;
            } else if (!strcmp(k, "peak_below")) {
                NEED(3);
                if (!parse_num(argv[2], &x->value)) ERR("expect peak_below DBFS");
                x->type = EXP_PEAK_BELOW;
            } else ERR("unknown expectation %s", k);
            sc->nexpects++;
        } else {
            ERR("unknown directive %s", cmd);
        }
    }
    fclose(f);
    for (int i = 0; i < nlines; i++) free(lines[i]);

    if (!explicit_track && default_track >= 0) sc->track_used[default_track] = 1;
    qsort(sc->events, (size_t)sc->nevents, sizeof(SimEvent), cmp_event);
    double total = sc->steps * sc->step_samples + sc->tail * OPZ_SAMPLE_RATE;
    sc->frames = ((int64_t)(total + 0.5) + OPZ_BLOCK_FRAMES - 1) / OPZ_BLOCK_FRAMES * OPZ_BLOCK_FRAMES;
    return 0;
}
