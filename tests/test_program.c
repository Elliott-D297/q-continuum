/* test_program: the other programs' layer (program.c).
 *   test_program fixtures <tests/programs> <scratch>
 *       every fixture folder is detected as its program, lists its input, reads its end from the input, and its real
 *       log (from runs on this machine) parses into steps, series and a finished state; starters write and detect.
 *   test_program run <project> [procs] [input]
 *       runs the project for real with the commands the app would use, then parses the log it wrote: prints the
 *       summary, exits 1 unless the run ended cleanly (tests/programs_run.sh drives it over every program). */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "program.h"
#include "runner.h"
#include "pl_util.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void parse_log(int prog, const char *path, LogMon *m)
{
    memset(m, 0, sizeof *m);
    m->prog = prog; m->exitCode = -1;
    size_t len = 0; char *t = pl_read_file(path, &len);
    if (t) logmon_feed(m, t, (int)len);
    logmon_feed(m, "\n", 1);
    free(t);
}

static void summary(const char *what, const LogMon *m)
{
    printf("%-10s steps %d t %g end %g/%d ended %d fatal %d exit %d series %d", what, m->steps, m->time, m->endTime, m->endSteps,
           m->ended, m->fatal, m->exitCode, m->nSeries);
    for (int i = 0; i < m->nSeries; i++) printf(" [%s:%d]", m->series[i].name, m->series[i].n);
    if (m->fatal) printf("\n  fatal: %s", m->fatalMsg);
    printf("\n");
}

typedef struct { const char *dir; int prog; const char *input; double end; bool steps; const char *log; int minSteps; const char *series; } Fx;

static int fixtures(const char *root, const char *scratch)
{
    static const Fx FX[] = {
        { "calculix",     PROG_CALCULIX, "A.inp",          250,  false, "log.calculix.A",    40,  "temp increment" },
        { "moose",        PROG_MOOSE,    "A.i",            250,  false, "log.moose.A",       200, "Nonlinear |R|" },
        { "aster",        PROG_ASTER,    "A.export",       250,  false, "log.aster.A",       200, NULL },
        { "saturne/case", PROG_SATURNE,  "DATA/setup.xml", 0,    true,  "log.saturne.case",  5,   "Temperature" },
        { "elmer",        PROG_ELMER,    "case.sif",       200,  true,  "log.elmer.case",    20,  "heat equation" },
        { "sparta",       PROG_SPARTA,   "in.circle",      1000, true,  "log.sparta.circle", 10,  "particles" },
    };
    for (size_t i = 0; i < sizeof FX / sizeof FX[0]; i++) {
        const Fx *f = &FX[i];
        char d[1024]; pl_join(d, sizeof d, root, f->dir);
        int p = prog_detect(d);
        CHECK(p == f->prog, "%s detected as %d, not %d", f->dir, p, f->prog);
        NameList in = { 0 };
        prog_inputs(f->prog, d, &in);
        CHECK(in.n >= 1 && !strcmp(in.names[0], f->input), "%s inputs: %d, first %s", f->dir, in.n, in.n ? in.names[0] : "-");
        names_free(&in);
        bool isSteps = false;
        double end = prog_end(f->prog, d, f->input, &isSteps);
        CHECK(fabs(end - f->end) < 1e-9 && (f->end == 0 || isSteps == f->steps), "%s end %g (steps %d), want %g", f->dir, end, isSteps, f->end);
        char lp[1024]; pl_join(lp, sizeof lp, d, f->log);
        LogMon m; parse_log(f->prog, lp, &m);
        summary(f->dir, &m);
        CHECK(m.steps >= f->minSteps, "%s: %d steps, want >= %d", f->dir, m.steps, f->minSteps);
        CHECK(!m.fatal, "%s: fatal %s", f->dir, m.fatalMsg);
        CHECK(m.ended || m.exitCode == 0, "%s: not ended", f->dir);
        if (f->series) {
            bool has = false; for (int k = 0; k < m.nSeries; k++) if (!strcmp(m.series[k].name, f->series) && m.series[k].n > 0) has = true;
            CHECK(has, "%s: no series %s", f->dir, f->series);
        }
        /* the run commands come out for each, with the exit marker */
        ProgStep st[4]; char err[400];
        int ns = prog_run_steps(f->prog, d, f->input, 2, false, st, 4, err, sizeof err);
        if (ns > 0) CHECK(strstr(st[ns - 1].cmd, "Q-Continuum: exit"), "%s: no exit marker", f->dir);
        else printf("  (no run steps: %s)\n", err);
        logmon_reset(&m);
    }
    /* specific values from the real logs */
    {
        char lp[1024]; pl_join(lp, sizeof lp, root, "elmer/log.elmer.case");
        LogMon m; parse_log(PROG_ELMER, lp, &m);
        CHECK(m.endSteps == 200, "elmer endSteps %d", m.endSteps);
        logmon_reset(&m);
        pl_join(lp, sizeof lp, root, "saturne/case/log.saturne.case");
        parse_log(PROG_SATURNE, lp, &m);
        CHECK(m.endSteps == 250, "saturne endSteps %d", m.endSteps);
        logmon_reset(&m);
        pl_join(lp, sizeof lp, root, "moose/log.moose.A");
        parse_log(PROG_MOOSE, lp, &m);
        CHECK(fabs(m.time - 250) < 1e-9, "moose t %g", m.time);
        logmon_reset(&m);
    }
    /* errors: what each program prints when it fails (texts from real failed runs) */
    {
        static const struct { int prog; const char *log; const char *want; } E[] = {
            { PROG_CALCULIX, " increment 1 attempt 1\n *ERROR reading *MATERIAL: material STEL\n does not exist\n\n", "STEL" },
            { PROG_MOOSE, "Time Step 1, time = 1, dt = 1\n\n*** ERROR ***\nA.i:12.5: unused parameter 'Kernels/cond/varible'\n\nStack frames: 3\n", "varible" },
            { PROG_ELMER, "MAIN: Time: 1/10:   1.0E+00\nERROR:: ListGetString: Requested property: [Procedure], not found\n", "Procedure" },
            { PROG_SPARTA, "Step CPU Np\n       0 0 0\nERROR: Could not find surf_collide ID (../surf.cpp:410)\n", "surf_collide" },
            { PROG_ASTER, "   !-------------------------------------------------------!\n   ! <F> <SUPERVIS_17>                                     !\n"
                          "   !  The command THER_LINEAIRE has an error in keyword    !\n   !-------------------------------------------------------!\n", "THER_LINEAIRE" },
            { PROG_DEME, "QC t=0.01 end=1 frame=1/100 ke=1e-3 vmax=0.2 n=3200\nterminate called after throwing an instance of 'std::runtime_error'\n"
                         "  what():  invalid force\n", "time step" },
            { PROG_CALCULIX, "actual total time=1\nQ-Continuum: exit 201\n", "exit code 201" },
        };
        for (size_t i = 0; i < sizeof E / sizeof E[0]; i++) {
            LogMon m; memset(&m, 0, sizeof m); m.prog = E[i].prog; m.exitCode = -1;
            logmon_feed(&m, E[i].log, (int)strlen(E[i].log));
            CHECK(m.fatal && strstr(m.fatalMsg, E[i].want), "%s error not caught: fatal %d msg '%s'", prog_info(E[i].prog)->name, m.fatal, m.fatalMsg);
            logmon_reset(&m);
        }
        /* a stopped run (SIGTERM, exit 143) is not an error */
        LogMon m; memset(&m, 0, sizeof m); m.prog = PROG_MOOSE; m.exitCode = -1;
        const char *s = "Time Step 1, time = 1, dt = 1\nQ-Continuum: exit 143\n";
        logmon_feed(&m, s, (int)strlen(s));
        CHECK(!m.fatal && m.exitCode == 143, "stop counted as failure");
        logmon_reset(&m);
    }
    /* starters: written, detected, inputs and checks */
    for (int i = 0; i < PROG_N_TEMPLATES; i++) {
        const ProgTemplate *t = &PROG_TEMPLATES[i];
        if (t->prog == PROG_DEME) continue;                       /* written by the app from embedded files */
        char d[1024]; snprintf(d, sizeof d, "%s/tpl-%s-%s", scratch, prog_info(t->prog)->key, t->id);
        pl_remove_tree(d);
        char err[300];
        CHECK(prog_template_write(t, d, err, sizeof err), "template %s/%s: %s", prog_info(t->prog)->name, t->id, err);
        pl_stat_cache_clear();
        CHECK(prog_detect(d) == t->prog, "template %s/%s detected as %d", prog_info(t->prog)->name, t->id, prog_detect(d));
        NameList in = { 0 }; prog_inputs(t->prog, d, &in);
        CHECK(in.n == 1, "template %s/%s: %d inputs", prog_info(t->prog)->name, t->id, in.n);
        if (in.n) {
            ProgCheck ck[16]; int nc = prog_checks(t->prog, d, in.names[0], ck, 16);
            for (int k = 0; k < nc; k++) {
                printf("  %s/%s check: [%d] %s\n", prog_info(t->prog)->key, t->id, ck[k].sev, ck[k].text);
                CHECK(ck[k].sev < 2 || strstr(ck[k].text, "not installed") || strstr(ck[k].text, "No MOOSE") || strstr(ck[k].text, "not on the PATH"),
                      "template %s/%s has a blocking problem: %s", prog_info(t->prog)->key, t->id, ck[k].text);
            }
        }
        names_free(&in);
    }
    /* a copied code_aster study: absolute paths of another folder, fixed to this one */
    {
        char d[1024]; snprintf(d, sizeof d, "%s/aster-copy", scratch);
        pl_remove_tree(d); pl_mkdirs(d);
        char src[1024]; pl_join(src, sizeof src, root, "aster/A.comm");
        char *c = pl_read_file(src, NULL); char dst[1024]; pl_join(dst, sizeof dst, d, "A.comm");
        pl_write_file(dst, c, strlen(c)); free(c);
        pl_join(dst, sizeof dst, d, "A.med"); pl_write_file(dst, "x", 1);
        const char *ex = "P actions make_etude\nF comm /nowhere/old/A.comm D 1\nF mmed /nowhere/old/A.med D 20\nF mess /nowhere/old/A.mess R 6\n";
        pl_join(dst, sizeof dst, d, "A.export"); pl_write_file(dst, ex, strlen(ex));
        pl_stat_cache_clear();
        ProgCheck ck[16]; int nc = prog_checks(PROG_ASTER, d, "A.export", ck, 16);
        int fixable = 0; for (int k = 0; k < nc; k++) if (ck[k].fix == PFIX_EXPORT_PATHS) fixable++;
        CHECK(fixable == 3, "aster copy: %d fixable path problems, want 3", fixable);
        char msg[200];
        CHECK(prog_apply_fix(PROG_ASTER, d, "A.export", PFIX_EXPORT_PATHS, msg, sizeof msg), "aster fix: %s", msg);
        char *t = pl_read_file(dst, NULL);
        CHECK(t && !strstr(t, "/nowhere") && strstr(t, d), "aster fix result:\n%s", t ? t : "");
        free(t);
        pl_stat_cache_clear();
        nc = prog_checks(PROG_ASTER, d, "A.export", ck, 16);
        int bad = 0; for (int k = 0; k < nc; k++) if (ck[k].sev == 2 && !strstr(ck[k].text, "not installed")) bad++;
        CHECK(bad == 0, "aster copy still has %d problems after the fix", bad);
    }
    /* results: what a finished CalculiX project shows */
    {
        char d[1024]; snprintf(d, sizeof d, "%s/results", scratch);
        pl_remove_tree(d); pl_mkdirs(d);
        static const char *const F[] = { "A.inp", "A.frd", "A.dat", "A.sta", "out/res.pvd", "out/res_t0001.vtu", "frames/p_0001.csv",
                                         "frames/p_0002.csv", "dump.flow.100", "dump.flow.200", NULL };
        for (int i = 0; F[i]; i++) {
            char p[1024]; pl_join(p, sizeof p, d, F[i]);
            char dd[1024]; pl_dirname(dd, sizeof dd, p); pl_mkdirs(dd);
            const char *body = strstr(F[i], "frames/") ? "X,Y,Z,r,absv\n0,0,0,0.1,1\n" :
                               strstr(F[i], "dump") ? "ITEM: TIMESTEP\n100\nITEM: NUMBER OF ATOMS\n1\nITEM: BOX BOUNDS\n0 1\n0 1\n0 1\nITEM: ATOMS id type x y z vx vy vz\n1 1 0.5 0.5 0 1 0 0\n" : "x\n";
            pl_write_file(p, body, strlen(body));
        }
        pl_stat_cache_clear();
        ProgResult r[32]; int n = prog_results(PROG_CALCULIX, d, r, 32);
        int frd = 0, pvd = 0, vtu = 0, parts = 0, text = 0;
        for (int i = 0; i < n; i++) {
            printf("  result %-22s %s (%d)\n", r[i].rel, prog_result_kind_name(r[i].kind), r[i].count);
            if (r[i].kind == RK_CGX) frd++;
            if (strstr(r[i].rel, ".pvd")) pvd++;
            if (strstr(r[i].rel, ".vtu")) vtu++;
            if (r[i].kind == RK_PARTICLES) parts++;
            if (r[i].kind == RK_TEXT) text++;
        }
        CHECK(frd == 1 && pvd == 1 && vtu == 0 && parts == 2 && text == 2, "results: frd %d pvd %d vtu %d particles %d text %d", frd, pvd, vtu, parts, text);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}

static int run(const char *dir, int procs, const char *inputArg)
{
    int prog = prog_detect(dir);
    if (prog <= PROG_OPENFOAM) { printf("%s: not a project of another program (%d)\n", dir, prog); return 2; }
    NameList in = { 0 };
    prog_inputs(prog, dir, &in);
    const char *input = inputArg ? inputArg : in.n ? in.names[0] : "";
    ProgCheck ck[16]; int nc = prog_checks(prog, dir, input, ck, 16);
    for (int k = 0; k < nc; k++) printf("check [%d] %s\n", ck[k].sev, ck[k].text);
    ProgStep st[4]; char err[400];
    int n = prog_run_steps(prog, dir, input, procs, false, st, 4, err, sizeof err);
    if (n <= 0) { printf("no steps: %s\n", err); return 1; }
    for (int i = 0; i < n; i++) {
        char lp[1024]; pl_join(lp, sizeof lp, dir, st[i].log);
        printf("== %s: %s\n", st[i].label, st[i].log);
        char q[1100]; pl_shell_quote(q, sizeof q, dir);
        Str sc; str_init(&sc);
        str_catf(&sc, "cd %s || exit 1\n%s\n", q, st[i].cmd);
        pid_t pid = pl_spawn_shell_log(sc.s, lp, false);
        str_free(&sc);
        int status = 0; waitpid(pid, &status, 0);
        LogMon m; parse_log(prog, lp, &m);
        summary(st[i].label, &m);
        bool ok = !m.fatal && m.exitCode == 0;
        if (st[i].solver) {
            bool isSteps; double end = prog_end(prog, dir, input, &isSteps);
            double done, total; bool steps;                 /* what the Run page shows */
            prog_progress(prog, &m, end, isSteps, &done, &total, &steps);
            printf("progress: %s %g of %g\n", steps ? "step" : "t", done, total);
            ProgResult r[32]; int nr = prog_results(prog, dir, r, 32);
            for (int k = 0; k < nr; k++) printf("result %s (%s)\n", r[k].rel, prog_result_kind_name(r[k].kind));
        }
        logmon_reset(&m);
        if (!ok) { names_free(&in); return 1; }
    }
    names_free(&in);
    return 0;
}

/* every starter the New page offers (DEME's are written by the app: page_deme.c), each in <scratch>/<prog>-<id> */
static int starters(const char *scratch)
{
    int bad = 0;
    for (int i = 0; i < PROG_N_TEMPLATES; i++) {
        const ProgTemplate *t = &PROG_TEMPLATES[i];
        if (t->prog == PROG_DEME) continue;
        char d[1024], err[300]; snprintf(d, sizeof d, "%s/%s-%s", scratch, prog_info(t->prog)->key, t->id);
        pl_remove_tree(d);
        bool ok = prog_template_write(t, d, err, sizeof err) && prog_detect(d) == t->prog;
        printf("%s %s\n", ok ? "made" : "FAILED", d);
        if (!ok) { printf("  %s (detected as %d)\n", err, prog_detect(d)); bad++; }
    }
    return bad;
}

/* the examples the New page lists (SPARTA, MOOSE on this machine): "<prog key>\t<path>" per line */
static int examples(void)
{
    static ProgExample ex[256];
    int n = prog_examples(ex, 256);
    for (int i = 0; i < n; i++) printf("%s\t%s\n", prog_info(ex[i].prog)->key, ex[i].path);
    return n ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "copy-example")) { char e[300] = ""; bool ok = prog_example_copy(argv[2], argv[3], e, sizeof e); if (!ok) puts(e); return ok ? 0 : 1; }
    if (argc >= 2 && !strcmp(argv[1], "examples")) return examples();
    if (argc >= 3 && !strcmp(argv[1], "starters")) return starters(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "fixtures")) return fixtures(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "run")) return run(argv[2], argc > 3 ? atoi(argv[3]) : 1, argc > 4 ? argv[4] : NULL);
    fprintf(stderr, "usage: test_program fixtures <tests/programs> <scratch> | starters <scratch> | examples | copy-example <src> <dst> | run <project> [procs] [input]\n");
    return 2;
}
