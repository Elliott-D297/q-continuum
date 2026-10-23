/* test_deme <assets/deme> <scratch>: the DEME page's data layer. Every shipped params_*.ini loads with labels and units,
 * survives an edit + save with its other lines byte-identical, and every key the drivers read is in it; the progress
 * parser reads frames, results and the old starter's lines, incrementally; frames load by their header; the Rayleigh
 * time step matches a hand value. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "demeparams.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* keys the drivers read with P.num(...): each template's params file must carry those its driver uses */
static void keys_in_driver(const char *assets, const char *name, const DemeParams *d)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s_template.cpp", assets, name);
    char *src = pl_read_file(p, NULL);
    CHECK(src, "no driver %s", p);
    if (!src) return;
    for (char *s = src; (s = strstr(s, "P.num(\"")); s += 7) {
        char key[64]; int k = 0;
        for (char *q = s + 7; *q && *q != '"' && k < 63; q++) key[k++] = *q;
        key[k] = 0;
        bool has = false;
        for (int i = 0; i < d->n; i++) if (!strcmp(d->p[i].key, key)) has = true;
        CHECK(has || !strcmp(key, "size_bins") || !strcmp(key, "max_velocity"), "%s reads %s, params_%s.ini lacks it", name, key, name);
    }
    free(src);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *assets = argv[1], *scratch = argv[2];
    pl_mkdirs(scratch);
    static const char *const T[] = { "settle", "drum", "hopper", "impact" };
    for (int t = 0; t < 4; t++) {
        char src[1024], dst[1024];
        snprintf(src, sizeof src, "%s/params_%s.ini", assets, T[t]);
        snprintf(dst, sizeof dst, "%s/params_%s.ini", scratch, T[t]);
        size_t len0 = 0; char *orig = pl_read_file(src, &len0);
        CHECK(orig && pl_write_file(dst, orig, len0), "copy %s", src);
        DemeParams d = { 0 };
        CHECK(deme_params_load(&d, dst), "load %s", dst);
        CHECK(d.n >= 15, "%s: only %d keys", T[t], d.n);
        for (int i = 0; i < d.n; i++) {
            CHECK(d.p[i].section[0], "%s: %s has no section", T[t], d.p[i].key);
            CHECK(strcmp(d.p[i].label, d.p[i].key), "%s: %s has no label", T[t], d.p[i].key);
        }
        DemeParam *g = deme_param(&d, "gravity");
        CHECK(g && !strcmp(g->unit, "m/s²"), "%s: gravity unit '%s'", T[t], g ? g->unit : "?");
        keys_in_driver(assets, T[t], &d);
        /* an edit changes that one line only */
        bool moonAlready = fabs(deme_param_num(&d, "gravity", 0) - 1.62) < 1e-9;    /* impact starts on the Moon */
        CHECK(deme_param_set_num(&d, "density", 1234.5), "set density");
        CHECK(deme_apply_preset(&d, &DEME_GRAVITY[1]) == 1, "moon preset");
        CHECK(deme_params_save(&d), "save");
        DemeParams e = { 0 };
        CHECK(deme_params_load(&e, dst), "reload");
        CHECK(fabs(deme_param_num(&e, "density", 0) - 1234.5) < 1e-9, "density round trip");
        CHECK(fabs(deme_param_num(&e, "gravity", 0) - 1.62) < 1e-9, "gravity round trip");
        CHECK(e.nLines == d.nLines, "line count %d -> %d", d.nLines, e.nLines);
        DemeParams o = { 0 };
        pl_write_file(dst, orig, len0);
        deme_params_load(&o, dst);
        int changed = 0;
        for (int i = 0; i < o.nLines && i < e.nLines; i++) if (strcmp(o.lines[i], e.lines[i])) changed++;
        CHECK(changed == (moonAlready ? 1 : 2), "%s: %d lines changed", T[t], changed);
        deme_params_free(&d); deme_params_free(&e); deme_params_free(&o); free(orig);
    }
    /* materials set the material keys of every template */
    for (int m = 0; m < DEME_N_MATERIALS; m++) {
        char src[1024]; snprintf(src, sizeof src, "%s/params_settle.ini", assets);
        DemeParams d = { 0 }; deme_params_load(&d, src);
        CHECK(deme_apply_preset(&d, &DEME_MATERIALS[m]) == 6, "material %s", DEME_MATERIALS[m].name);
        deme_params_free(&d);
    }
    /* Rayleigh: r 1 mm, glass 2500 kg/m3, E 1e8, nu 0.25 -> G 4e7, pi 1e-3 sqrt(6.25e-5) / 0.9174 = 2.707e-5 s */
    double rd = deme_rayleigh_dt(1e-3, 2500, 1e8, 0.25);
    CHECK(fabs(rd - 2.707e-5) < 0.01e-5, "rayleigh %g", rd);

    /* progress: incremental reads of a growing log */
    char log[1024]; snprintf(log, sizeof log, "%s/log.deme", scratch);
    const char *part1 = "Parameters: ../params.ini\nParticles: 900\nQC t=0 end=0.5 frame=0/10 ke=0 vmax=0 n=900\n"
                        "QC t=0.05 end=0.5 frame=1/10 ke=0.0125 vmax=0.49 n=900\nQC discharged=0.01 kg of 0.2\nQC t=0.1 end=0.5 fr";
    pl_write_file(log, part1, strlen(part1));
    DemeProgress p; deme_progress_reset(&p);
    deme_progress_poll(&p, log, 100.0);
    CHECK(p.any && p.frame == 1 && p.frames == 10 && p.n == 900, "progress 1: frame %d/%d n %ld", p.frame, p.frames, p.n);
    CHECK(fabs(p.ke - 0.0125) < 1e-9 && fabs(p.end - 0.5) < 1e-12 && p.nS == 2, "ke %g end %g nS %d", p.ke, p.end, p.nS);
    CHECK(!strcmp(p.extra, "discharged=0.01 kg of 0.2"), "extra '%s'", p.extra);
    char full[600]; snprintf(full, sizeof full, "%same=2/10 ke=0.02 vmax=0.98 n=900\nQC result penetration=0.0123 m\nQC result froude=0.05 \n", part1);
    pl_write_file(log, full, strlen(full));
    deme_progress_poll(&p, log, 104.0);
    CHECK(p.frame == 2 && p.nS == 3 && fabs(p.t - 0.1) < 1e-12, "progress 2: frame %d nS %d t %g", p.frame, p.nS, p.t);
    CHECK(p.rate > 0 && fabs(p.rate - 0.05 / 4.0) < 1e-9, "rate %g", p.rate);
    CHECK(p.nRes == 2 && !strcmp(p.resName[0], "penetration") && !strcmp(p.resVal[0], "0.0123 m"), "results %d %s %s", p.nRes, p.resName[0], p.resVal[0]);
    /* a new run truncates the log */
    pl_write_file(log, "Frame 3 of 10, t = 0.15 s\n", 26);
    deme_progress_poll(&p, log, 110.0);
    CHECK(p.frame == 3 && p.frames == 10 && p.nRes == 0, "restart: frame %d results %d", p.frame, p.nRes);

    /* a known crash is recognised, the first cause kept */
    deme_progress_reset(&p);
    deme_progress_line(&p, "forceToAcc found invalid force/torque for A: contact 81, owner 99, type 20.", 1);
    deme_progress_line(&p, "[ERROR]   toHost: GPU Error: unspecified launch failure", 1);
    CHECK(p.crash == DEME_CRASH_FORCES && deme_crash_text(p.crash)[0], "crash %d", p.crash);
    deme_progress_reset(&p);
    deme_progress_line(&p, "Parameters: none found, using the defaults", 1);
    CHECK(p.crash == DEME_CRASH_NO_PARAMS, "no params %d", p.crash);

    /* frames: columns by header, any order */
    char csv[1024]; snprintf(csv, sizeof csv, "%s/particles_0000.csv", scratch);
    const char *c = "absv,r,Z,Y,X\n0.5,0.001,0.3,0.2,0.1\n1.5,0.002,-0.3,-0.2,-0.1\n";
    pl_write_file(csv, c, strlen(c));
    DemeFrame f = { 0 };
    CHECK(deme_frame_load(&f, csv) && f.n == 2, "frame load n %d", f.n);
    CHECK(f.n == 2 && fabsf(f.x[0] - 0.1f) < 1e-6f && fabsf(f.z[1] + 0.3f) < 1e-6f && fabsf(f.vmax - 1.5f) < 1e-6f && fabsf(f.rmax - 0.002f) < 1e-9f,
          "frame values");
    CHECK(fabsf(f.lo[2] + 0.3f) < 1e-6f && fabsf(f.hi[0] - 0.1f) < 1e-6f, "frame bounds");
    deme_frame_free(&f);

    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
