/* page_run.c -- run setup, pipeline, live monitor (time ruler, residual chart, log console) */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "app.h"
#include "physics.h"
#include "safety.h"
#include "precheck.h"
#include "monitor.h"
#include "explain.h"
#include "sweep.h"

/* Tutorial scripts are "#!/bin/sh" but some use bash features (foamyQuadMesh copies {a,b}.stl.gz), and /bin/sh is
   dash on Debian and Ubuntu: run sh scripts with bash, which runs every sh script the same, and others as they are */
#define SH_RUN "qc_sh() { case \"$(head -n1 \"$1\")\" in '#!/bin/sh'*|'#!/usr/bin/env sh'*) bash \"$@\";; *) \"$@\";; esac; }\n"

/* Display order. Multi-region cases restore 0.orig after splitMeshRegions when 0.orig holds per-region folders. */
enum { ST_RESTORE = 0, ST_BLOCKMESH, ST_FEATURES, ST_SNAPPY, ST_CHECK, ST_TOPOSET, ST_SPLIT, ST_RESTORE_REGIONS, ST_CHANGEDICT,
       ST_SETFIELDS, ST_DECOMPOSE, ST_SOLVER, ST_RECONSTRUCT, ST_COUNT };
static const char *STEP_LABEL[ST_COUNT] = { "Copy 0.orig to 0", "blockMesh", "surfaceFeatureExtract", "snappyHexMesh", "checkMesh",
                                            "topoSet", "splitMeshRegions", "Copy 0.orig to 0", "changeDictionary",
                                            "setFields", "decomposePar", "Solver", "reconstructPar" };
static const char *STEP_HINT[ST_COUNT] = { "Start from the initial fields you set up", "Build the background mesh",
                                           "Extract feature edges from the geometry", "Fit the mesh to the geometry",
                                           "Report mesh quality", "Create the cell zones that define each region",
                                           "Cut the mesh into one mesh per region", "Per-region initial fields",
                                           "Apply each region's boundary conditions", "Initialise regions, e.g. the water column",
                                           "Split the case for parallel processes", "", "Merge the parallel results" };

static bool multi_region(void) { return A.cs->s.nRegions > 0; }
#define CELLS_PER_RANK 10000          /* below this per rank, MPI traffic outweighs the split (ROADMAP 4.6) */

/* OpenFOAM's own helpers (restore0Dir, remove0DirFields, ...) so steps behave like the tutorials' Allrun scripts */
#define RUNFUNCTIONS ". \"$WM_PROJECT_DIR/bin/tools/RunFunctions\" || exit 1\n"

bool g_force_builtin = false;                          /* testing: --builtin */

/* 0.orig/<region>/ folders mean the fields are already per region: copy them after the split */
static bool zero_orig_by_region(void)
{
    if (!multi_region() || !A.cs->s.hasZeroOrig) return false;
    char rel[128], p[PL_PATH];
    snprintf(rel, sizeof rel, "0.orig/%s", A.cs->s.regions[0]);
    pl_join(p, sizeof p, A.cs->s.path, rel);
    return pl_is_dir(p);
}

static int n_change_dicts(void)
{
    int n = 0;
    for (int r = 0; r < A.cs->s.nRegions; r++) {
        char rel[160], p[PL_PATH];
        snprintf(rel, sizeof rel, "system/%s/changeDictionaryDict", A.cs->s.regions[r]);
        pl_join(p, sizeof p, A.cs->s.path, rel);
        if (pl_is_file(p)) n++;
    }
    return n;
}

static bool case_has(const char *rel)
{
    char p[PL_PATH]; pl_join(p, sizeof p, A.cs->s.path, rel);
    return pl_exists(p);
}

static bool step_available(int s, int procs)
{
    switch (s) {
    case ST_RESTORE: return A.cs->s.hasZeroOrig && !zero_orig_by_region();
    case ST_RESTORE_REGIONS: return zero_orig_by_region();
    case ST_TOPOSET: return multi_region() && case_has("system/topoSetDict");
    case ST_SPLIT: return multi_region();
    case ST_CHANGEDICT: return multi_region() && n_change_dicts() > 0;
    case ST_BLOCKMESH: return case_has("system/blockMeshDict");
    case ST_FEATURES: return case_has("system/surfaceFeatureExtractDict");
    case ST_SNAPPY: return case_has("system/snappyHexMeshDict");
    case ST_CHECK: return true;
    case ST_SETFIELDS: return case_has("system/setFieldsDict");
    case ST_DECOMPOSE: case ST_RECONSTRUCT: return procs > 1;
    case ST_SOLVER: return true;
    default: return false;
    }
}

/* Starts from the case's decomposeParDict (or Settings); then the stepper owns it and start_run writes it back */
static bool app_runs_parallel(const char *app);
/* The decomposeParDict the Allrun's SOLVER step decomposes with, when not system/decomposeParDict: motorBike runs
   "runParallel $decompDict $(getApplication)" with decompDict="-decomposeParDict system/decomposeParDict.6". The
   Processes count belongs there. Other steps keep their own dictionaries: snappyMultiRegionHeater meshes on .6, has a
   hard-coded "runParallel -np 6" redistribution, then solves on system/decomposeParDict. Returns 0 or 1 path. */
static int allrun_decomp_dicts(const Case *c, char (*out)[160], int max)
{
    if (max < 1) return 0;
    char var[64] = "", rel[160] = "";
    bool solverUses = false;
    const char *names[] = { c->s.allrun[0] ? c->s.allrun : "Allrun", "Allrun.pre" };
    for (int f = 0; f < 2; f++) {
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, names[f]);
        size_t len; char *t = pl_read_file(p, &len);
        for (char *line = t ? strtok(t, "\n") : NULL; line; line = strtok(NULL, "\n")) {
            while (*line == ' ' || *line == '\t') line++;
            if (*line == '#') continue;
            char *d = strstr(line, "-decomposeParDict"), *eq = strchr(line, '=');
            if (d && eq && eq < d) {                              /* decompDict="-decomposeParDict <file>" */
                int k = 0;
                while (line + k < eq && k < 63 && (isalnum((unsigned char)line[k]) || line[k] == '_')) { var[k] = line[k]; k++; }
                var[k] = 0;
            }
            if (d && !rel[0]) {
                const char *q = d + strlen("-decomposeParDict");
                while (*q == ' ' || *q == '\t' || *q == '=') q++;
                int k = 0;
                while (*q && !strchr(" \t\n\"';)", *q) && k < 159) rel[k++] = *q++;
                rel[k] = 0;
                if (strchr(rel, '$')) rel[0] = 0;
            }
            bool solverLine = strstr(line, "runParallel") && (strstr(line, "getApplication") || (c->s.app[0] && strstr(line, c->s.app)));
            if (solverLine && (strstr(line, "-decomposeParDict") || (var[0] && strstr(line, var)))) solverUses = true;
        }
        free(t);
    }
    char full[PL_PATH]; pl_join(full, sizeof full, c->s.path, rel);
    if (!solverUses || !rel[0] || !pl_is_file(full)) return 0;
    pl_strlcpy(out[0], rel, 160);
    return 1;
}

static bool allrun_parallel(const Case *c);
static float precheck_panel(Case *c, float x, float y, float w);
static void summary_with_edits(Case *c, CaseSummary *out);
static int procs(void)
{
    if (A.runMode == 0 && A.cs && !app_runs_parallel(A.cs->s.app)) return 1;   /* serial-only application */
    if (A.runProcs < 1) {
        int n = A.set.nProcs < 1 ? 1 : A.set.nProcs;
        /* a tutorial whose Allrun runs it in serial and that has no decomposeParDict may not run in parallel at all
           (angledDuct: SIGFPE on 4 ranks; rotor2DAMI: particles can't cross AMI patches between processors) */
        char dpd[PL_PATH]; pl_join(dpd, sizeof dpd, A.cs->s.path, "system/decomposeParDict");
        if (A.cs->s.hasAllrun && !A.cs->s.groupAllrun && !pl_is_file(dpd) && !allrun_parallel(A.cs)) n = 1;
        char dd[4][160];
        const char *rel = A.runMode == 1 && allrun_decomp_dicts(A.cs, dd, 4) > 0 ? dd[0] : "system/decomposeParDict";
        FDFile *d = case_dict(A.cs, rel);
        char v[32];
        A.runProcsFrom = 0; A.runProcsDict = false;
        long perRank = CELLS_PER_RANK;
        if (d && fd_get(d, "numberOfSubdomains", v, sizeof v) && atoi(v) >= 1) {
            n = atoi(v);
            A.runProcsDict = true;
            perRank = CELLS_PER_RANK / 10;                 /* the case's own count: only overruled for a demo split */
        }
        if (n > 1) {
            long cells = A.cs->s.nCells;
            if (cells <= 0) { BlockMeshInfo bi; FDFile *bm = case_dict(A.cs, "system/blockMeshDict"); if (bm) { blockmesh_info(bm, &bi); if (bi.ok) cells = bi.cells; } }
            int m = (int)procs_for_cells(n, cells, perRank);
            if (m < n) { A.runProcsFrom = n; A.runProcsCells = cells; n = m; }
        }
        A.runProcs = n;
    }
    return A.runProcs;
}
static void set_procs(int n) { A.runProcs = n < 1 ? 1 : n > 1024 ? 1024 : n; A.runProcsFrom = 0; }
int run_procs_for_check(void) { return procs(); }
int run_mode_for_queue(void) { return A.cs && A.cs->s.hasAllrun ? A.runMode : -1; }

/* Does the case's Allrun (or the Allrun.pre it calls) do something the built-in steps don't? Then it has to run as the
   script: another utility (extrudeMesh, overset setup, a finite-area mesh, ...), or files copied in from elsewhere (the
   tutorials' resources/geometry). In the tutorial sweep the built-in steps failed on such cases with "Cannot find file
   points" and missing surfaces. `why` gets the first such command. */
static bool allrun_needs_script(const Case *c, char *why, int cap)
{
    static const char *const builtin[] = { "blockMesh", "surfaceFeatureExtract", "surfaceFeatures", "snappyHexMesh", "setFields",
        "topoSet", "splitMeshRegions", "changeDictionary", "decomposePar", "reconstructPar", "checkMesh", "$(getApplication)",
        "`getApplication`", "restore0Dir", "foamCleanTutorials", "cleanCase", "cleanCase0", NULL };
    static const char *const harmless[] = { "cd", ".", "source", "echo", "exit", "set", "true", "rm", "mkdir", "touch", "if", "then",
        "else", "elif", "fi", "for", "do", "done", "while", "case", "esac", "[", "[[", "{", "}", "#", "getApplication", "return",
        "unset", "export", "local", "isParallel", "notTest", "isTest", NULL };
    why[0] = 0;
    const char *names[] = { c->s.allrun[0] ? c->s.allrun : "Allrun", "Allrun.pre" };
    for (int f = 0; f < 2; f++) {
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, names[f]);
        size_t len; char *t = pl_read_file(p, &len);
        if (!t) continue;
        for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
            while (*line == ' ' || *line == '\t') line++;
            if (!*line || *line == '#') continue;
            if (strstr(line, "resources/") || strstr(line, "$FOAM_TUTORIALS") || strstr(line, "../")) {
                snprintf(why, (size_t)cap, "it brings in files from outside the case (%.60s)", line);
                free(t); return true;
            }
            char w[96] = ""; sscanf(line, "%95s", w);
            if (strchr(w, '=') && w[0] != '=') continue;                       /* a variable assignment */
            const char *app = w;
            char a2[96] = "";
            if (!strcmp(w, "runApplication") || !strcmp(w, "runParallel")) {
                /* skip the RunFunctions options: -s <suffix>, -o, -a, -l <log>, -np <n>, -decomposeParDict <f> */
                char *q = line + strlen(w);
                for (;;) {
                    while (*q == ' ' || *q == '\t') q++;
                    if (sscanf(q, "%95s", a2) != 1) { a2[0] = 0; break; }
                    q += strlen(a2);
                    if (!strcmp(a2, "-s") || !strcmp(a2, "-l") || !strcmp(a2, "-np") || !strcmp(a2, "-decomposeParDict")) {
                        while (*q == ' ') q++;
                        char sk[96];
                        if (sscanf(q, "%95s", sk) == 1) q += strlen(sk);
                        continue;
                    }
                    if (a2[0] == '-') continue;
                    break;
                }
                app = a2;
            }
            if (!*app || !strcmp(app, c->s.app) || !strcmp(app, "./Allrun.pre") || !strcmp(app, "Allrun.pre")) continue;
            bool ok = false;
            for (int i = 0; builtin[i] && !ok; i++) if (!strcmp(app, builtin[i])) ok = true;
            /* the built-in topoSet/split/changeDictionary steps exist only for multi-region cases */
            if (ok && c->s.nRegions == 0 && (!strcmp(app, "topoSet") || !strcmp(app, "splitMeshRegions") || !strcmp(app, "changeDictionary"))) ok = false;
            for (int i = 0; harmless[i] && !ok; i++) if (!strcmp(app, harmless[i])) ok = true;
            if (!ok) { snprintf(why, (size_t)cap, "it also runs %s, which the built-in steps don't", app); free(t); return true; }
        }
        free(t);
    }
    return false;
}

/* serial-only applications (boundaryFoam, chemFoam, a blockMesh-only case, ...) have no -parallel option, and mpirun with
   it stops at "Invalid option: -parallel". The application's -help lists -parallel when it has it. Asking means sourcing
   the OpenFOAM environment (about a second), so it runs in the background, and the answer is kept per installation in
   the cache folder. Until it is known the app is taken to run in parallel; start_run waits for the answer. */
typedef struct { char app[64], bashrc[PL_PATH]; pid_t pid; char out[PL_PATH]; int state; } ParProbe;   /* state: 0 asking, 1 yes, 2 no */
static ParProbe PP[64];
static int nPP;
static bool ppLoaded;

static void pp_cache_path(char *out, size_t cap)
{
    const char *x = getenv("XDG_CACHE_HOME");
    char dir[PL_PATH];
    if (x && *x) snprintf(dir, sizeof dir, "%s/q-continuum", x); else snprintf(dir, sizeof dir, "%s/.cache/q-continuum", pl_home());
    pl_mkdirs(dir);
    if (!pl_fmt(out, cap, "%s/parallel-apps.txt", dir)) out[0] = 0;
}

static void pp_load(void)
{
    ppLoaded = true;
    char p[PL_PATH]; pp_cache_path(p, sizeof p);
    size_t len; char *t = pl_read_file(p, &len);
    for (char *l = t ? strtok(t, "\n") : NULL; l && nPP < 64; l = strtok(NULL, "\n")) {
        char *a = strchr(l, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!a || !b) continue;
        *a = *b = 0;
        ParProbe *q = &PP[nPP++];
        memset(q, 0, sizeof *q);
        pl_strlcpy(q->bashrc, l, sizeof q->bashrc); pl_strlcpy(q->app, a + 1, sizeof q->app);
        q->state = b[1] == 'n' ? 2 : 1;
    }
    free(t);
}

static void pp_save(void)
{
    char p[PL_PATH]; pp_cache_path(p, sizeof p);
    Str b; str_init(&b);
    for (int i = 0; i < nPP; i++) if (PP[i].state) str_catf(&b, "%s\t%s\t%c\n", PP[i].bashrc, PP[i].app, PP[i].state == 2 ? 'n' : 'y');
    pl_write_file(p, b.s ? b.s : "", b.s ? strlen(b.s) : 0);
    str_free(&b);
}

/* finished asking? (wait: block until it has) */
static void pp_poll(ParProbe *q, bool wait)
{
    if (q->state || q->pid <= 0) return;
    int st;
    if (waitpid(q->pid, &st, wait ? 0 : WNOHANG) != q->pid) return;
    char *t = pl_read_file(q->out, NULL);
    q->state = t && t[0] == 'n' ? 2 : 1;                     /* can't tell: assume it can */
    free(t);
    unlink(q->out);
    q->pid = -1;
    pp_save();
}

static bool app_runs_parallel_wait(const char *app, bool wait)
{
    if (!app || !*app) return true;
    if (!ppLoaded) pp_load();
    const char *rc = app_bashrc();
    for (int i = 0; i < nPP; i++)
        if (!strcmp(PP[i].app, app) && !strcmp(PP[i].bashrc, rc)) { pp_poll(&PP[i], wait); return PP[i].state != 2; }
    if (nPP >= 64) return true;
    ParProbe *q = &PP[nPP++];
    memset(q, 0, sizeof *q);
    pl_strlcpy(q->app, app, sizeof q->app); pl_strlcpy(q->bashrc, rc, sizeof q->bashrc);
    char qb[PL_PATH + 8], qa[160], cmd[PL_PATH + 600];
    pl_shell_quote(qb, sizeof qb, rc);
    pl_shell_quote(qa, sizeof qa, app);
    pl_fmt(cmd, sizeof cmd, "source %s >/dev/null 2>&1; command -v %s >/dev/null || { echo u; exit; }; "
           "%s -help 2>&1 | grep -q -e '-parallel' && echo y || echo n", qb, qa, qa);
    snprintf(q->out, sizeof q->out, "/tmp/qc-parprobe-%d-%d.txt", (int)getpid(), nPP);
    q->pid = pl_spawn_shell(cmd, q->out);
    if (q->pid <= 0) { q->state = 1; return true; }
    if (wait) pp_poll(q, true);
    return q->state != 2;
}
static bool app_runs_parallel(const char *app) { return app_runs_parallel_wait(app, false); }
static bool app_parallel_known(const char *app)
{
    app_runs_parallel_wait(app, false);
    for (int i = 0; i < nPP; i++) if (!strcmp(PP[i].app, app) && !strcmp(PP[i].bashrc, app_bashrc())) return PP[i].state != 0;
    return true;
}

static bool allrun_needs_script_cached(const Case *c, char *why, int cap)
{
    static char path[PL_PATH], reason[160]; static time_t mt[2]; static bool val;
    char p[PL_PATH]; time_t m[2];
    pl_join(p, sizeof p, c->s.path, c->s.allrun[0] ? c->s.allrun : "Allrun"); m[0] = pl_mtime(p);
    pl_join(p, sizeof p, c->s.path, "Allrun.pre"); m[1] = pl_mtime(p);
    if (strcmp(path, c->s.path) || m[0] != mt[0] || m[1] != mt[1]) {
        pl_strlcpy(path, c->s.path, sizeof path); mt[0] = m[0]; mt[1] = m[1];
        val = allrun_needs_script(c, reason, sizeof reason);
    }
    pl_strlcpy(why, reason, (size_t)cap);
    return val;
}

static void init_steps(void)
{
    if (A.stepInit) return;
    A.stepInit = true;
    bool meshed = A.cs->s.meshed && !A.cs->s.meshStale;
    for (int s = 0; s < ST_COUNT; s++) A.stepOn[s] = true;
    A.stepOn[ST_BLOCKMESH] = !meshed;
    A.stepOn[ST_FEATURES] = !meshed;
    A.stepOn[ST_SNAPPY] = !meshed;
    A.stepOn[ST_CHECK] = false;
    bool regionsDone = A.cs->s.regionMeshed && !A.cs->s.regionStale && !A.cs->s.meshStale;
    A.stepOn[ST_TOPOSET] = !regionsDone;
    A.stepOn[ST_SPLIT] = !regionsDone;
    A.stepOn[ST_CHANGEDICT] = !regionsDone;
    /* multi-region tutorials encode a lot in Allrun (per-region tweaks); other cases when their Allrun does more than
       the built-in steps can */
    char why[160];
    A.runMode = A.cs->s.hasAllrun && !g_force_builtin && (A.cs->s.groupAllrun || multi_region() || allrun_needs_script(A.cs, why, sizeof why)) ? 1 : 0;
}

/* simple/hierarchical decompositions carry an explicit n (nx ny nz) whose product must equal the
   number of subdomains. When the counts no longer agree, switch to scotch, which needs no coefficients. */
static void fix_decomposition(FDFile *d, int np)
{
    char m[64] = "", w[64] = "";
    if (!fd_get(d, "method", m, sizeof m)) return;
    fd_word(m, w, sizeof w);
    if (strcmp(w, "simple") && strcmp(w, "hierarchical")) return;
    char key[128], v[128] = "";
    snprintf(key, sizeof key, "%sCoeffs/n", w);
    if (!fd_find(d, key) && fd_find(d, "coeffs/n")) snprintf(key, sizeof key, "coeffs/n");
    fd_get(d, key, v, sizeof v);
    long a = 0, b = 0, c = 0;
    const char *p = strchr(v, '(');
    if (p && sscanf(p + 1, "%ld %ld %ld", &a, &b, &c) == 3 && a * b * c == np) return;   /* already consistent */
    /* the method stays; only the split follows the process count */
    char nv[64]; decomp_split(A.cs, np, nv, sizeof nv);
    fd_put(d, key, nv);
    ui_toast(TOAST_INFO, "decomposeParDict: %s split %s for %d processes", w, nv, np);
}

/* does the case's Allrun decompose and run in parallel itself (runParallel, mpirun or decomposePar in Allrun or
   the Allrun.pre it calls)? Then the process count must reach its decomposeParDict, or the spinner lies. */
static bool allrun_parallel(const Case *c)
{
    static char path[PL_PATH]; static time_t mt[2]; static bool val;
    const char *const names[2] = { c->s.allrun[0] ? c->s.allrun : "Allrun", "Allrun.pre" };
    char p[2][PL_PATH]; time_t m[2];
    for (int i = 0; i < 2; i++) { pl_join(p[i], sizeof p[i], c->s.path, names[i]); m[i] = pl_mtime(p[i]); }
    if (!strcmp(path, c->s.path) && m[0] == mt[0] && m[1] == mt[1]) return val;
    pl_strlcpy(path, c->s.path, sizeof path); mt[0] = m[0]; mt[1] = m[1];
    val = false;
    for (int i = 0; i < 2 && !val; i++) {
        size_t len; char *t = m[i] ? pl_read_file(p[i], &len) : NULL;
        if (t && (strstr(t, "runParallel") || strstr(t, "mpirun") || strstr(t, "decomposePar"))) val = true;
        free(t);
    }
    return val;
}

/* the steps of a run of c on np processes, as start_run makes them (A.runMode, A.continueRun, A.stepOn); slurm: srun
   for the solver instead of mpirun (AWAY MISSION's batch script, page_away.c) */
static void build_steps(Case *c, Job *j, int np, bool slurm)
{
    /* continuing on results that live in processor*: they are the run's input, decomposePar would delete them */
    bool procResults = A.continueRun && c->s.procTimes > 0 && (!c->s.nTimes || atof(c->s.procLatest) >= atof(c->s.latest));
    if (A.runMode == 1) {
        char ac[600], al[48];
        if (c->s.groupAllrun) {
            /* RunFunctions skip a step whose log exists, in every folder of the group */
            snprintf(ac, sizeof ac, "find .. -maxdepth 2 -name 'log.*' ! -name 'log.group.Allrun' -delete\nchmod +x ../Allrun 2>/dev/null\n" SH_RUN "qc_sh ../Allrun");
            snprintf(al, sizeof al, "log.group.Allrun");
            job_add(j, "allrun", "group Allrun", ac, al, true);
        } else {
            snprintf(ac, sizeof ac, "find . -maxdepth 1 -name 'log.*' ! -name 'log.%s' -delete\nchmod +x ./%s 2>/dev/null\n" SH_RUN "qc_sh ./%s",
                     c->s.allrun, c->s.allrun, c->s.allrun);
            snprintf(al, sizeof al, "log.%s", c->s.allrun);
            job_add(j, "allrun", c->s.allrun, ac, al, true);
        }
    } else {
        char cmd[2048], log[96];
        bool cont = A.continueRun;
        for (int s = 0; s < ST_COUNT; s++) {
            if (!step_available(s, np)) continue;
            bool on = A.stepOn[s] || s == ST_SOLVER || s == ST_DECOMPOSE || s == ST_RECONSTRUCT;
            if (cont && (s == ST_RESTORE || s == ST_BLOCKMESH || s == ST_SNAPPY || s == ST_FEATURES || s == ST_SETFIELDS ||
                         s == ST_TOPOSET || s == ST_SPLIT || s == ST_RESTORE_REGIONS || s == ST_CHANGEDICT)) on = false;
            if (procResults && s == ST_DECOMPOSE) on = false;
            if (!on) continue;
            switch (s) {
            case ST_RESTORE: job_add(j, "restore0", STEP_LABEL[s], RUNFUNCTIONS "restore0Dir 2>&1", "log.restore0", false); break;
            case ST_BLOCKMESH: job_add(j, "blockMesh", "blockMesh", "blockMesh", "log.blockMesh", false); break;
            case ST_FEATURES: job_add(j, "features", "surfaceFeatureExtract", "surfaceFeatureExtract", "log.surfaceFeatureExtract", false); break;
            case ST_SNAPPY: job_add(j, "snappy", "snappyHexMesh", "snappyHexMesh -overwrite", "log.snappyHexMesh", false); break;
            case ST_CHECK: job_add(j, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false); break;
            case ST_SETFIELDS: job_add(j, "setFields", "setFields", "setFields", "log.setFields", false); break;
            case ST_TOPOSET: job_add(j, "topoSet", "topoSet", "topoSet", "log.topoSet", false); break;
            case ST_SPLIT:
                /* splitMeshRegions copies flat 0/ fields into every region; like the tutorials' Allrun.pre, take the
                   fluid fields out of the solid regions again (per-region 0.orig is restored afterwards instead) */
                job_add(j, "split", "splitMeshRegions", zero_orig_by_region() ? "splitMeshRegions -cellZones -overwrite" :
                        "splitMeshRegions -cellZones -overwrite || exit 1\n" RUNFUNCTIONS
                        "for region in $(foamListRegions solid)\ndo\n"
                        "    remove0DirFields -region \"$region\" -- nut alphat epsilon k omega nuTilda U p_rgh\ndone 2>&1",
                        "log.splitMeshRegions", false);
                break;
            case ST_RESTORE_REGIONS: job_add(j, "restore0", STEP_LABEL[s], RUNFUNCTIONS "restore0Dir 2>&1", "log.restore0", false); break;
            case ST_CHANGEDICT: {
                Str sc; str_init(&sc);
                for (int r = 0; r < c->s.nRegions; r++) {
                    char rel[160], p[PL_PATH];
                    snprintf(rel, sizeof rel, "system/%s/changeDictionaryDict", c->s.regions[r]);
                    pl_join(p, sizeof p, c->s.path, rel);
                    /* one log per region, like runApplication -s <region> */
                    if (pl_is_file(p))
                        str_catf(&sc, "rm -f log.changeDictionary.%1$s\necho 'changeDictionary -region %1$s > log.changeDictionary.%1$s'\n"
                                      "changeDictionary -region %1$s > log.changeDictionary.%1$s 2>&1 || "
                                      "{ echo 'Q-Continuum: could not apply changeDictionary to region %1$s. See log.changeDictionary.%1$s.'; exit 1; }\n",
                                 c->s.regions[r]);
                }
                job_add(j, "changeDict", "changeDictionary", sc.s ? sc.s : "true", "log.changeDictionary", false);
                str_free(&sc);
                break;
            }
            case ST_DECOMPOSE: {
                const char *ar = multi_region() ? " -allRegions" : "";
                snprintf(cmd, sizeof cmd, "decomposePar -force%s%s", ar, cont ? " -latestTime" : "");
                job_add(j, "decompose", "decomposePar", cmd, "log.decomposePar", false);
                break;
            }
            case ST_SOLVER:
                for (int k = 0; k < c->cfg.nPre; k++) {              /* the user's own steps, just before the solver */
                    if (!c->cfg.pre[k][0]) continue;
                    char id[32], lg[64]; snprintf(id, sizeof id, "pre%d", k + 1); snprintf(lg, sizeof lg, "log.step.pre%d", k + 1);
                    job_add(j, id, c->cfg.pre[k], c->cfg.pre[k], lg, false);
                }
                /* the switch lets Pause reach the solver: SIGUSR1 makes it write the time step it is on and end. Only
                   OpenFOAM.com (v2xxx) has -opt-switch; the Foundation's releases pause through controlDict alone. */
                {
                    const FoamInstall *fi = env_active(&A.env, &A.set);
                    const char *sw = fi && fi->version[0] == 'v' ? " " PAUSE_SIGNAL_OPT : "";
                    if (slurm && np > 1) snprintf(cmd, sizeof cmd, "srun %s -parallel", c->s.app);   /* Slurm starts the ranks */
                    else if (slurm) snprintf(cmd, sizeof cmd, "%s", c->s.app);
                    else if (np > 1) snprintf(cmd, sizeof cmd, "OVS=; mpirun --version 2>&1 | grep -q 'Open MPI' && OVS=--oversubscribe\nmpirun $OVS %s-np %d %s -parallel%s", getuid() == 0 ? "--allow-run-as-root " : "", np, c->s.app, sw);
                    else snprintf(cmd, sizeof cmd, "%s%s", c->s.app, sw);
                }
                snprintf(log, sizeof log, "log.%s", c->s.app);
                if (cont) {
                    /* a continued run adds to the log, so the history before it stays readable */
                    char c2[sizeof cmd + 100];
                    snprintf(c2, sizeof c2, "echo; echo '== Q-Continuum: continuing from t = %s =='; echo\n", case_latest_result(&c->s));
                    size_t L = strlen(c2);
                    pl_strlcpy(c2 + L, cmd, sizeof c2 - L);
                    pl_strlcpy(cmd, c2, sizeof cmd);
                }
                job_add(j, "solver", c->s.app, cmd, log, true);
                if (cont) j->steps[j->n - 1].append = true;
                break;
            case ST_RECONSTRUCT:
                /* a run shorter than writeInterval leaves only time 0 in the processor folders: reconstructPar would fail with
                   "No times selected" and mark a good run as failed */
                snprintf(cmd, sizeof cmd, "if ls -d processor0/[0-9]* 2>/dev/null | grep -qv '^processor0/0$'; then %s; else "
                         "echo 'Q-Continuum: nothing to reconstruct. The solver wrote no results: the end time comes before the first write (writeInterval in system/controlDict).'; fi",
                         multi_region() ? "reconstructPar -allRegions" : "reconstructPar");
                job_add(j, "reconstruct", "reconstructPar", cmd, "log.reconstructPar", false);
                break;
            default: break;                                     /* every step has a case; s is an int */
            }
        }
        for (int k = 0; k < c->cfg.nPost; k++) {                     /* ... and after it */
            if (!c->cfg.post[k][0]) continue;
            char id[32], lg[64]; snprintf(id, sizeof id, "post%d", k + 1); snprintf(lg, sizeof lg, "log.step.post%d", k + 1);
            job_add(j, id, c->cfg.post[k], c->cfg.post[k], lg, false);
        }
    }
}

/* a preCICE participant (system/preciceDict and the adapter in controlDict): alone it waits for its partners forever.
   Fills parent with the coupled run's folder (the preciceConfig's folder) when there is one. */
static bool precice_participant(Case *c, char *parent, int cap, char *who, int whoCap)
{
    parent[0] = who[0] = 0;
    FDFile *pd = case_dict(c, "system/preciceDict");
    if (!pd || !pd->ok) return false;
    FDFile *cd = case_dict(c, "system/controlDict");
    if (!cd || !strstr(cd->text, "libpreciceAdapterFunctionObject")) return false;
    fd_get(pd, "participant", who, whoCap);
    char cfg[PL_PATH] = ""; fd_get(pd, "preciceConfig", cfg, sizeof cfg);
    size_t n = strlen(cfg);
    if (n > 1 && cfg[0] == '"' && cfg[n - 1] == '"') { memmove(cfg, cfg + 1, n - 2); cfg[n - 2] = 0; }
    char p[PL_PATH], real[PL_PATH];
    if (cfg[0] && pl_fmt(p, sizeof p, "%s/%s", c->s.path, cfg) && realpath(p, real)) pl_dirname(parent, (size_t)cap, real);
    return true;
}

static void start_run(void)
{
    Case *c = A.cs;
    if (!app_require_foam()) return;
    const char *bad = foam_path_problem(c->s.path);
    if (bad) { ui_toast(TOAST_ERR, "The case path has %s, which OpenFOAM refuses. Copy the case to a folder without it.", bad); return; }
    if (c->s.readOnly) { ui_toast(TOAST_ERR, "Not started: the case folder is read-only, so OpenFOAM can't write results. Copy it to the workspace from the Overview page."); return; }
    if (!app_can_start()) return;
    if (app_review_needed() && !A.bgStart) { review_open(1); return; }   /* 3.5: the user's edits are seen before they are written */
    if (!c->s.app[0] && A.runMode == 0) { ui_toast(TOAST_ERR, "Choose a solver application first"); return; }
    {
        char par[PL_PATH], who[64];
        if (precice_participant(c, par, sizeof par, who, sizeof who) && !A.bgStart) {
            ui_toast(TOAST_WARN, "%s is the %s participant of a preCICE coupling: alone it would wait for its partners. Run the coupled run%s%s.",
                     c->s.name, who[0] ? who : "preCICE", par[0] ? " in " : "", par[0] ? pl_basename(par) : "");
            return;
        }
    }
    if (A.runMode == 0 && !A.bgStart && !app_parallel_known(c->s.app)) { A.runPending = true; return; }   /* asked in the background: next frame */
    if (A.runMode == 0 && !app_runs_parallel_wait(c->s.app, true)) A.runProcs = 1;   /* the answer, not the guess */
    case_rescan(c);
    if (c->s.runningLog[0]) {
        ui_toast(TOAST_ERR, "%s is still being written: this case is running in another process. Stop that run first.", c->s.runningLog);
        return;
    }
    /* never destroy results without asking: a fresh run re-decomposes (decomposePar -force deletes processor*),
       restores 0 and overwrites time directories; Allrun mode clears the logs */
    if (case_has_results(&c->s) && !A.continueRun && !A.runConfirmed) {
        /* never a dialog for a queued start: it had to say continue or start over when it was queued */
        if (A.bgStart) { ui_toast(TOAST_WARN, "%s has results now: open it and choose Continue or Start over", c->s.name); return; }
        A.modal = MD_RUN_RESULTS; return;
    }
    if (A.runConfirmed && !A.continueRun) {
        /* "Delete and start over" / "Delete and run Allrun": with the old times gone, startFrom latestTime means the
           initial time again (left in place, a tutorial Allrun started at the old end time and ran no steps) */
        char msg[160];
        case_trash_results(c, TR_LOGS, false, A.runMode == 1 ? "Delete and run Allrun" : "Delete and start over", msg, sizeof msg);
        ui_toast(TOAST_INFO, "%s", msg);
    }
    /* will it fit in memory, is the time step stable, are there cores: ask before anything is deleted or started */
    if (!A.precheckOk && !A.bgStart) {
        PreCheck pc;
        CaseSummary cs; summary_with_edits(c, &cs);
        precheck_run(&cs, procs(), app_jobs_procs(), A.env.nCpu, &pc);
        if (pc.worst >= PC_WARN || run_sweep(c, true)->ask) { A.modal = MD_PRECHECK; return; }
    }
    A.precheckOk = false;
    A.runConfirmed = false;
    if (case_pause_pending(c->s.path)) case_pause_settle(c->s.path, false, NULL);   /* a pause that never reached a solver */
    /* continuing on a mesh that is about to change would ignore the change */
    char meshWhy[300];
    if (A.continueRun && A.runMode == 0 && mesh_change_pending(c, meshWhy, sizeof meshWhy)) {
        ui_toast(TOAST_WARN, "Can't continue: %s Start over to mesh again.", meshWhy);
        A.continueRun = false;
        return;
    }
    /* continuing past the end time does nothing but restart the log: say so instead */
    if (A.continueRun && A.runMode == 0 && case_has_results(&c->s)) {
        const char *lt = case_latest_result(&c->s);
        double last = atof(lt), end = c->s.endTime;
        if (lt[0] && end > 0 && last >= end * (1 - 1e-9)) {
            ui_toast(TOAST_WARN, "The results already reach the end time (t = %s). Raise End time to run further.", lt);
            A.continueRun = false;
            return;
        }
    }
    /* continuing a decomposed case: keep its decomposition, and never decompose over its processor* results */
    bool procResults = A.continueRun && c->s.procTimes > 0 && (!c->s.nTimes || atof(c->s.procLatest) >= atof(c->s.latest));
    if (procResults && A.runMode == 0 && procs() != c->s.nProcDirs) {
        ui_toast(TOAST_INFO, "Continuing on the existing %d-way decomposition (processor0..%d)", c->s.nProcDirs, c->s.nProcDirs - 1);
        set_procs(c->s.nProcDirs);
    }
    int np = procs();
    FDFile *cd = case_dict(c, "system/controlDict");
    /* Continue needs latestTime. A fresh run keeps the case's own startFrom: forcing startTime broke cases whose
       startTime has no folder (multiRegionHeater: startTime 0.001, fields in 0, startFrom latestTime). */
    if (cd && A.runMode == 0 && A.continueRun) {
        /* for this run only: the case's own startFrom comes back when it ends (bug hunt H-16), noted in .qc-startfrom
           so a crash in between still knows it */
        char was[64] = "";
        fd_get(cd, "startFrom", was, sizeof was);
        if (strcmp(was, "latestTime")) {
            char mk[PL_PATH]; pl_join(mk, sizeof mk, c->s.path, ".qc-startfrom");
            if (!pl_exists(mk)) pl_write_file(mk, was[0] ? was : "-", strlen(was[0] ? was : "-"));
            fd_put(cd, "startFrom", "latestTime");
        }
    }
    if (np > 1 && (A.runMode == 0 || allrun_parallel(c))) {
        FDFile *dp = case_dict(c, "system/decomposeParDict");
        char n[16]; snprintf(n, sizeof n, "%d", np);
        if (!dp) {
            char body[256]; snprintf(body, sizeof body, "numberOfSubdomains %d;\n\nmethod          scotch;\n", np);
            case_create_dict(c, "system/decomposeParDict", "dictionary", body);
        } else { fd_put(dp, "numberOfSubdomains", n); fix_decomposition(dp, np); }
        for (int r = 0; r < c->s.nRegions; r++) {            /* per-region decomposeParDicts must agree */
            char rel[160], p[PL_PATH]; snprintf(rel, sizeof rel, "system/%s/decomposeParDict", c->s.regions[r]);
            pl_join(p, sizeof p, c->s.path, rel);
            FDFile *rd = pl_is_file(p) ? case_dict(c, rel) : NULL;
            if (rd) { fd_put(rd, "numberOfSubdomains", n); fix_decomposition(rd, np); }
        }
        if (A.runMode == 1) {                                 /* ... and the ones the Allrun names itself */
            char dd[4][160];
            int nd = allrun_decomp_dicts(c, dd, 4);
            for (int i = 0; i < nd; i++) {
                FDFile *xd = case_dict(c, dd[i]);
                if (xd) { fd_put(xd, "numberOfSubdomains", n); fix_decomposition(xd, np); }
            }
        }
    }
    if (!app_save_all(true)) return;
    case_rescan(c);
    /* never run the solver on a mesh that no longer matches the dictionaries it was made from */
    if (!A.continueRun && A.runMode == 0 && (c->s.meshStale || c->s.regionStale)) {
        static const int meshSteps[] = { ST_BLOCKMESH, ST_FEATURES, ST_SNAPPY }, regionSteps[] = { ST_TOPOSET, ST_SPLIT, ST_CHANGEDICT };
        bool turned = false;
        for (int i = 0; i < 3 && c->s.meshStale; i++)
            if (step_available(meshSteps[i], np) && !A.stepOn[meshSteps[i]]) A.stepOn[meshSteps[i]] = turned = true;
        for (int i = 0; i < 3; i++)
            if (step_available(regionSteps[i], np) && !A.stepOn[regionSteps[i]]) A.stepOn[regionSteps[i]] = turned = true;
        if (turned)
            ui_toast(TOAST_WARN, "%s is newer than the mesh: meshing again before %s", c->s.meshStale ? c->s.meshStaleDict : "system/topoSetDict", c->s.app);
    }
    /* every patch of the mesh needs a boundary condition in every field, or decomposePar/the solver stop */
    bool remesh = false;
    for (int s = ST_BLOCKMESH; s <= ST_SNAPPY; s++) if (step_available(s, np) && A.stepOn[s]) remesh = true;
    if (A.runMode == 0 && !A.continueRun && c->s.nRegions == 0) {
        char miss[200];
        int n = turb_missing_fields(c, miss, sizeof miss);
        if (n > 0) {
            ui_toast(TOAST_ERR, "%s%sThe turbulence model needs %s in %s/. Create %s on the Physics page.", A.bgStart ? c->s.name : "", A.bgStart ? ": " : "", miss, c->fieldDir, n == 1 ? "it" : "them");
            if (!A.bgStart) app_go(PG_PHYSICS);
            return;
        }
    }
    if (A.runMode == 0 && !A.continueRun && !remesh && c->s.nRegions == 0 && c->patchSource == 1) {
        char miss[600];
        int n = bc_missing_report(c, miss, sizeof miss);
        if (n > 0) {
            ui_toast(TOAST_ERR, "%s%s%d patch%s of the mesh ha%s no boundary condition: %s. Set them on the Boundaries page.",
                     A.bgStart ? c->s.name : "", A.bgStart ? ": " : "", n, n == 1 ? "" : "es", n == 1 ? "s" : "ve", miss);
            if (!A.bgStart) app_go(PG_BC);
            return;
        }
    }
    job_reset(A.cj, c->s.app[0] ? c->s.app : "Allrun");
    build_steps(c, A.cj, np, false);
    int busy = app_jobs_procs();                             /* cores the other runs already use */
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) {
        case_pause_clear(c->s.path);
        A.cj->procs = A.runMode == 1 && !allrun_parallel(c) ? 1 : np;
        A.cj->isRun = true;
        if (A.set.history) {
            char what[160];
            if (A.runMode == 1) snprintf(what, sizeof what, "%s%s", c->s.groupAllrun ? "group " : "", c->s.allrun);
            else snprintf(what, sizeof what, "%s%s, %d process%s", A.continueRun ? "continue " : "", c->s.app, A.cj->procs, A.cj->procs == 1 ? "" : "es");
            hist_run_started_bg(c->s.path, what);
            history_invalidate();
        }
        A.runLog = -1;
        A.runFollow = true;
        if (busy && busy + A.cj->procs > A.env.nCpu)
            ui_toast(TOAST_WARN, "Runs now use %d processes on %d cores: each will be slower. Fewer processes per run is faster overall.",
                     busy + A.cj->procs, A.env.nCpu);
        else if (A.bgStart) ui_toast(TOAST_INFO, "Queue: started %s (%d step%s)", c->s.name, A.cj->n, A.cj->n == 1 ? "" : "s");
        else ui_toast(TOAST_INFO, "Run started: %d step%s%s", A.cj->n, A.cj->n == 1 ? "" : "s", busy ? " (alongside the other runs)" : "");
    } else ui_toast(TOAST_ERR, "Could not start the run");
}

/* ============================================================================ time ruler */
static double nice_step(double range, int target)
{
    double raw = range / target, mag = pow(10, floor(log10(raw)));
    double n = raw / mag;
    return (n < 1.5 ? 1 : n < 3 ? 2 : n < 7 ? 5 : 10) * mag;
}

static void time_ruler(Rectangle r, double t0, double t1, double t, Color fill)
{
    if (!(t1 > t0)) t1 = t0 + 1;
    double frac = (t - t0) / (t1 - t0);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    float bx = r.x, bw = r.width, by = r.y + 34;
    /* track and fill */
    ui_rect((Rectangle){ bx, by, bw, 10 }, 2, TH.lineSoft);
    ui_rect((Rectangle){ bx, by, (float)(bw * frac), 10 }, 2, fill);
    /* scale ticks */
    double st = nice_step(t1 - t0, 8);
    double minor = st / 5;
    for (double v = ceil(t0 / minor) * minor; v <= t1 + minor * 0.01; v += minor) {
        float px = bx + (float)((v - t0) / (t1 - t0)) * bw;
        bool major = fabs(fmod(v + st * 1e-9, st)) < minor * 0.5 || fabs(fmod(v, st) - st) < minor * 0.5;
        DrawRectangleRec((Rectangle){ px, by + 12, 1, major ? 9.f : 5.f }, major ? TH.text3 : TH.line);
        if (major) {
            char lab[32]; snprintf(lab, sizeof lab, "%g", fabs(v) < st * 1e-6 ? 0.0 : v);
            float lw = ui_text_w(F_REG, 11.5f, lab);
            ui_text(F_REG, 11.5f, lab, fminf(fmaxf(px - lw / 2, bx), bx + bw - lw), by + 24, TH.text3);
        }
    }
    /* marker */
    float mx = bx + (float)(bw * frac);
    DrawTriangle((Vector2){ mx - 6, by - 9 }, (Vector2){ mx, by - 1 }, (Vector2){ mx + 6, by - 9 }, fill);
    char cur[64]; snprintf(cur, sizeof cur, "t = %g", t);
    ui_text(F_SEMI, 14, cur, bx, r.y + 4, TH.text);
    char pct[48]; snprintf(pct, sizeof pct, "%.0f%% of end time", frac * 100);
    ui_text(F_REG, 12.5f, pct, bx + bw - ui_text_w(F_REG, 12.5f, pct), r.y + 6, TH.text3);
}

/* ============================================================================ residual chart */
/* "h (topAir)" -> "topAir"; "I (rays)" and plain names have no region */
static bool series_region(const char *name, char *out, int cap)
{
    const char *o = strrchr(name, '(');
    size_t n = strlen(name);
    if (!o || o == name || o[-1] != ' ' || name[n - 1] != ')' || !strcmp(o, "(rays)")) { out[0] = 0; return false; }
    snprintf(out, (size_t)cap, "%.*s", (int)(name + n - 1 - (o + 1)), o + 1);
    return true;
}

/* region the chart shows: the user's pick, or ("-" = not picked) the first region when the series crowd the chart.
   Resolved every frame, because series appear one by one while the first time step is parsed. */
static char chartRegion[64];

static bool series_shown(const Series *s)
{
    if (s->hidden) return false;
    if (!chartRegion[0]) return true;
    char rg[64];
    return series_region(s->name, rg, sizeof rg) && !strcmp(rg, chartRegion);
}

/* the chart's Expand / Restore: the chart takes the whole page (the log's Expand does the same for the log) */
float chart_max_button(float right, float y)
{
    const char *xl = A.chartMax ? "Restore" : "Expand", *xi = A.chartMax ? ICON_CHEVRON_DOWN : ICON_CHEVRON_UP;
    float xw = ui_button_width(xi, xl);
    Rectangle b = { right - xw, y, xw, 28 };
    if (ui_button(ui_id("chart-max"), b, xi, xl, BTN_GHOST)) { A.chartMax = !A.chartMax; A.logMax = false; }
    ui_tooltip(b, A.chartMax ? "Back to the normal layout (Esc)" : "Give the chart the whole page");
    if (A.chartMax && inp_key_pressed(KEY_ESCAPE) && !ui_any_input_focused() && A.modal == MD_NONE) A.chartMax = false;
    return xw;
}

void run_chart(Rectangle r, LogMon *m)
{
    ui_rect(r, 12, TH.chartBg);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    float xw = m && m->nSeries ? chart_max_button(r.x + r.width - 10, r.y + 10) + 8 : 0;   /* the legend wraps before it */
    if (!m || m->nSeries == 0) {
        ui_icon(ICON_CHART_LINE, r.x + r.width / 2, r.y + r.height / 2 - 18, 28, TH.text3);
        ui_text_center(F_REG, 13.5f, "Residuals appear here while a solver runs.", (Rectangle){ r.x, r.y + r.height / 2 + 2, r.width, 22 }, TH.text3);
        return;
    }
    /* multi-region logs: one row of region pills; with many series start on the first region */
    char regions[16][64]; int nReg = 0;
    for (int i = 0; i < m->nSeries && nReg < 16; i++) {
        char rg[64];
        if (!series_region(m->series[i].name, rg, sizeof rg)) continue;
        bool have = false;
        for (int k = 0; k < nReg; k++) if (!strcmp(regions[k], rg)) have = true;
        if (!have) pl_strlcpy(regions[nReg++], rg, 64);
    }
    if (A.chartRegion[0] == '-') pl_strlcpy(chartRegion, nReg && m->nSeries > 8 ? regions[0] : "", sizeof chartRegion);
    else {
        bool ok = !A.chartRegion[0];
        for (int k = 0; k < nReg; k++) if (!strcmp(regions[k], A.chartRegion)) ok = true;
        pl_strlcpy(chartRegion, ok ? A.chartRegion : "", sizeof chartRegion);
    }
    float lx = r.x + 16, ly = r.y + 12;
    if (nReg > 1) {
        for (int k = -1; k < nReg; k++) {
            const char *lab = k < 0 ? ui_cap("All regions") : regions[k];          /* region names keep their case */
            bool on = k < 0 ? !chartRegion[0] : !strcmp(chartRegion, regions[k]);
            float w = ui_text_w(F_MED, 12.5f, lab) + 24;
            if (lx + w > r.x + r.width - 12 - xw) { lx = r.x + 16; ly += 30; }
            Rectangle pill = { lx, ly, w, 24 };
            bool hov = ui_hovered(pill);
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            ui_rect(pill, 12, on ? (TH.lcars ? TH.lc[k < 0 ? 0 : (k + 1) % 4] : TH.accent) : hov ? TH.hover : BLANK);
            if (!on) ui_rect_lines(pill, 12, 1, TH.line);
            ui_text(F_MED, 12.5f, lab, lx + 12, ly + 4, on ? TH.textOnAccent : TH.text2);
            if (ui_clicked(pill)) pl_strlcpy(A.chartRegion, k < 0 ? "" : regions[k], sizeof A.chartRegion);
            lx += w + 6;
        }
        lx = r.x + 16; ly += 34;
    }
    /* legend */
    for (int i = 0; i < m->nSeries; i++) {
        Series *s = &m->series[i];
        if (chartRegion[0]) { char rg[64]; if (!series_region(s->name, rg, sizeof rg) || strcmp(rg, chartRegion)) continue; }
        Color c = TH.series[i % 10];
        char lab[96], nm[48];
        pl_strlcpy(nm, s->name, sizeof nm);
        if (chartRegion[0]) { char *o = strrchr(nm, '('); if (o && o > nm) o[-1] = 0; }     /* region is in the pill */
        snprintf(lab, sizeof lab, "%s  %.1e", nm, s->n ? s->v[s->n - 1] : 0.0);
        float w = ui_text_w(F_MED, 12.5f, lab) + 30;
        if (lx + w > r.x + r.width - 12 - xw) { lx = r.x + 16; ly += 28; }
        Rectangle chip = { lx, ly, w, 24 };
        bool hov = ui_hovered(chip);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(chip)) s->hidden = !s->hidden;
        ui_rect(chip, 12, s->hidden ? BLANK : ui_alpha(c, 0.13f));
        if (s->hidden || hov) ui_rect_lines(chip, 12, 1, ui_alpha(c, 0.4f));
        DrawCircleV((Vector2){ lx + 12, ly + 12 }, 4, s->hidden ? ui_alpha(c, 0.35f) : c);
        ui_text(F_MED, 12.5f, lab, lx + 22, ly + 4, s->hidden ? TH.text3 : TH.text);
        lx += w + 8;
    }
    Rectangle p = { r.x + 58, ly + 40, r.width - 58 - 20, r.y + r.height - (ly + 40) - 34 };
    if (p.height < 40) return;
    /* ranges. The y range ignores zeros and the lowest 1% of points (a 1e-14 first Uy residual would
       otherwise stretch the axis over a dozen empty decades) and spans at most 12 decades. */
    enum { DLO = -40, DHI = 20 };
    int hist[DHI - DLO + 1] = { 0 }, total = 0;
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < m->nSeries; i++) {
        Series *s = &m->series[i];
        if (!series_shown(s)) continue;
        for (int k = 0; k < s->n; k++) {
            tmin = fminf(tmin, s->t[k]); tmax = fmaxf(tmax, s->t[k]);
            if (!(s->v[k] > 0)) continue;
            int d = (int)floorf(log10f(s->v[k]));
            hist[(d < DLO ? DLO : d > DHI ? DHI : d) - DLO]++;
            total++;
        }
    }
    if (tmax < tmin) return;
    if (tmax - tmin < 1e-12f) { tmax = tmin + 1; }
    int d0 = -6, d1 = 0;
    if (total) {
        int lo = 0, hi = DHI - DLO, cum = 0, cut = total / 100;
        while (lo < hi && cum + hist[lo] <= cut) cum += hist[lo++];
        while (hi > lo && !hist[hi]) hi--;
        d0 = lo + DLO; d1 = hi + DLO + 1;
    }
    if (d1 > 3) d1 = 3;
    if (d0 < d1 - 12) d0 = d1 - 12;
    if (d1 <= d0) d1 = d0 + 1;
    /* grid: a line per decade, labels only as often as the height allows */
    float perDec = p.height / (float)(d1 - d0);
    int every = (int)ceilf(18.f / fmaxf(perDec, 1.f));
    if (every < 1) every = 1;
    for (int d = d0; d <= d1; d++) {
        float py = p.y + p.height - (float)(d - d0) / (d1 - d0) * p.height;
        bool lab = (d1 - d) % every == 0;
        DrawRectangleRec((Rectangle){ p.x, py, p.width, 1 }, lab ? TH.grid : ui_alpha(TH.grid, 0.45f));
        if (!lab) continue;
        char t[16]; snprintf(t, sizeof t, "1e%d", d);
        ui_text(F_MONO, 11.5f, t, p.x - 10 - ui_text_w(F_MONO, 11.5f, t), py - 7, TH.text3);
    }
    double st = nice_step(tmax - tmin, 6);
    for (double v = ceil(tmin / st) * st; v <= tmax + st * 1e-6; v += st) {
        float px = p.x + (float)((v - tmin) / (tmax - tmin)) * p.width;
        DrawRectangleRec((Rectangle){ px, p.y, 1, p.height }, ui_alpha(TH.grid, 0.6f));
        char lab[32]; snprintf(lab, sizeof lab, "%g", v);
        float lw = ui_text_w(F_REG, 11.5f, lab);
        ui_text(F_REG, 11.5f, lab, px - lw / 2, p.y + p.height + 8, TH.text3);
    }
    /* series */
    ui_clip_push(ui_inset(p, -2, -2));
    for (int i = 0; i < m->nSeries; i++) {
        Series *s = &m->series[i];
        if (!series_shown(s) || s->n == 0) continue;
        Color c = TH.series[i % 10];
        int step = s->n > (int)(p.width * 2) ? s->n / (int)(p.width * 2) : 1;
        Vector2 prev = { 0 };
        bool have = false;
        for (int k = 0; k < s->n; k += step) {
            float v = s->v[k];
            if (step > 1) for (int q = k; q < k + step && q < s->n; q++) if (s->v[q] > v) v = s->v[q];
            if (!(v > 0)) { have = false; continue; }                 /* no residual this step: break the line */
            float lv = fminf(fmaxf(log10f(v), (float)d0), (float)d1);
            Vector2 pt = { p.x + (s->t[k] - tmin) / (tmax - tmin) * p.width, p.y + p.height - (lv - d0) / (d1 - d0) * p.height };
            if (have) DrawLineEx(prev, pt, 1.7f, c);
            prev = pt; have = true;
        }
        if (s->n == 1) DrawCircleV(prev, 3, c);
    }
    ui_clip_pop();
    /* hover read-out */
    Vector2 mp = ui_mouse();
    if (ui_hovered(p)) {
        float t = tmin + (mp.x - p.x) / p.width * (tmax - tmin);
        DrawRectangleRec((Rectangle){ mp.x, p.y, 1, p.height }, ui_alpha(TH.text3, 0.6f));
        char lines[MAX_SERIES][64]; int nl = 0; float tw = 0;
        char head[48]; snprintf(head, sizeof head, "t = %.5g", t);
        for (int i = 0; i < m->nSeries; i++) {
            Series *s = &m->series[i];
            if (!series_shown(s) || !s->n) continue;
            int lo = 0, hi = s->n - 1;
            while (lo < hi) { int mid = (lo + hi) / 2; if (s->t[mid] < t) lo = mid + 1; else hi = mid; }
            pl_fmt_ui(lines[nl], 64, "%s  %.3e", s->name, s->v[lo]);
            tw = fmaxf(tw, ui_text_w(F_MONO, 12, lines[nl]));
            float lv = fminf(fmaxf(log10f(s->v[lo] > 0 ? s->v[lo] : 1e-30f), (float)d0), (float)d1);
            DrawCircleV((Vector2){ p.x + (s->t[lo] - tmin) / (tmax - tmin) * p.width, p.y + p.height - (lv - d0) / (d1 - d0) * p.height }, 3.5f, TH.series[i % 10]);
            nl++;
        }
        float bw = tw + 40, bh = 30 + nl * 18;
        float bx = mp.x + 14 + bw > p.x + p.width ? mp.x - 14 - bw : mp.x + 14;
        Rectangle box = { bx, p.y + 8, bw, bh };
        ui_rect(box, 8, ui_alpha(TH.raised, 0.97f));
        ui_rect_lines(box, 8, 1, TH.line);
        ui_text(F_MED, 12.5f, head, box.x + 12, box.y + 8, TH.text);
        for (int i = 0, k = 0; i < m->nSeries; i++) {
            if (!series_shown(&m->series[i]) || !m->series[i].n) continue;
            DrawCircleV((Vector2){ box.x + 16, box.y + 34 + k * 18 }, 3.5f, TH.series[i % 10]);
            ui_text(F_MONO, 12, lines[k], box.x + 26, box.y + 27 + k * 18, TH.text2);
            k++;
        }
    }
}

/* ============================================================================ monitors */
static bool mon_hidden(const Case *c, const char *col)
{
    char key[80]; snprintf(key, sizeof key, "|%s|", col);
    char hay[600]; snprintf(hay, sizeof hay, "|%s|", c->cfg.monHide);
    return strstr(hay, key) != NULL;
}

static void mon_toggle(Case *c, const char *col)
{
    char hay[600]; snprintf(hay, sizeof hay, "|%s|", c->cfg.monHide);
    char key[80]; snprintf(key, sizeof key, "|%s|", col);
    char *at = strstr(hay, key);
    if (at) memmove(at, at + strlen(key) - 1, strlen(at + strlen(key) - 1) + 1);
    else { size_t L = strlen(hay); snprintf(hay + L, sizeof hay - L, "%s|", col); }
    /* back to "a|b|c" without the outer bars */
    char *b = hay; while (*b == '|') b++;
    size_t L = strlen(b); while (L && b[L - 1] == '|') b[--L] = 0;
    pl_strlcpy(c->cfg.monHide, b, sizeof c->cfg.monHide);
    casecfg_save(c->s.path, &c->cfg);
}

/* the file shown: the remembered one, else forces/coefficients first, else the first */
static MonFile *mon_current(Case *c)
{
    if (A.mons.n == 0) return NULL;
    int k = c->cfg.monFile[0] ? monset_find(&A.mons, c->cfg.monFile) : -1;
    if (k < 0) {
        for (int i = 0; i < A.mons.n && k < 0; i++) if (strstr(A.mons.f[i].key, "coefficient") || strstr(A.mons.f[i].key, "force")) k = i;
        if (k < 0) k = 0;
    }
    return &A.mons.f[k];
}

static void mon_select(Case *c, MonFile *f)
{
    pl_strlcpy(c->cfg.monFile, f->key, sizeof c->cfg.monFile);
    /* many columns (solver info, min/max with locations): start with the first six */
    c->cfg.monHide[0] = 0;
    for (int i = 6; i < f->nCols; i++) { size_t L = strlen(c->cfg.monHide); snprintf(c->cfg.monHide + L, sizeof c->cfg.monHide - L, "%s%s", L ? "|" : "", f->col[i]); }
    casecfg_save(c->s.path, &c->cfg);
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static void mon_chart(Rectangle r, Case *c)
{
    ui_rect(r, 12, TH.chartBg);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    MonFile *f = mon_current(c);
    if (!f) return;
    /* file picker */
    const char *labels[MON_MAX_FILES];
    for (int i = 0; i < A.mons.n; i++) labels[i] = A.mons.f[i].label;
    char pick[200] = "";
    if (ui_dropdown(ui_id("mon-file"), (Rectangle){ r.x + 14, r.y + 10, fminf(360, r.width * 0.45f), 30 }, f->label, labels, A.mons.n, 0, pick, sizeof pick, "file")) {
        for (int i = 0; i < A.mons.n; i++) if (!strcmp(A.mons.f[i].label, pick)) { mon_select(c, &A.mons.f[i]); f = &A.mons.f[i]; }
    }
    char info[96]; snprintf(info, sizeof info, "%d samples%s", f->n, f->n ? "" : " yet");
    ui_text(F_REG, 12, info, r.x + 26 + fminf(360, r.width * 0.45f), r.y + 17, TH.text3);
    /* legend: one chip per column, click to show/hide */
    float lx = r.x + 16, ly = r.y + 48;
    for (int i = 0; i < f->nCols; i++) {
        Color col = TH.series[i % 10];
        bool hid = mon_hidden(c, f->col[i]);
        float last = NAN;
        for (int k = f->n - 1; k >= 0 && isnan(last); k--) last = f->v[i][k];
        char lab[96]; snprintf(lab, sizeof lab, isnan(last) ? "%s" : "%s  %.4g", f->col[i], last);
        float w = ui_text_w(F_MED, 12, lab) + 30;
        if (lx + w > r.x + r.width - 12) { lx = r.x + 16; ly += 28; }
        Rectangle chip = { lx, ly, w, 24 };
        bool hov = ui_hovered(chip);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(chip)) mon_toggle(c, f->col[i]);
        ui_rect(chip, 12, hid ? BLANK : ui_alpha(col, 0.13f));
        if (hid || hov) ui_rect_lines(chip, 12, 1, ui_alpha(col, 0.4f));
        DrawCircleV((Vector2){ lx + 12, ly + 12 }, 4, hid ? ui_alpha(col, 0.35f) : col);
        ui_text(F_MED, 12, lab, lx + 22, ly + 5, hid ? TH.text3 : TH.text);
        lx += w + 8;
    }
    Rectangle p = { r.x + 70, ly + 40, r.width - 70 - 20, r.y + r.height - (ly + 40) - 34 };
    if (p.height < 40 || f->n == 0) return;
    float tmin = f->t[0], tmax = f->t[f->n - 1];
    /* the y range leaves out the extreme 2 % at either end: a start-up spike (the first iterations' forces, often 100x the
       settled value) would flatten the rest into a line. Clipped lines run off the chart. */
    static float *vals; static int cap;
    int nv = 0, stride = 1, tot = 0;
    for (int i = 0; i < f->nCols; i++) if (!mon_hidden(c, f->col[i])) tot += f->n;
    if (tot > 40000) stride = tot / 40000 + 1;
    for (int i = 0; i < f->nCols; i++) {
        if (mon_hidden(c, f->col[i])) continue;
        for (int k = 0; k < f->n; k += stride) {
            float v = f->v[i][k]; if (isnan(v)) continue;
            if (nv == cap) {
                int nc = cap ? cap * 2 : 4096;
                float *nvv = realloc(vals, sizeof(float) * (size_t)nc);
                if (!nvv) goto collected;
                vals = nvv; cap = nc;
            }
            vals[nv++] = v;
        }
    }
collected:                                                       /* out of memory: chart what fitted */
    if (nv == 0) return;
    qsort(vals, (size_t)nv, sizeof(float), cmpf);
    double vmin = vals[(int)(nv * 0.02)], vmax = vals[(int)(nv * 0.98) < nv ? (int)(nv * 0.98) : nv - 1];
    if (nv < 50) { vmin = vals[0]; vmax = vals[nv - 1]; }
    if (tmax - tmin < 1e-12f) tmax = tmin + 1;
    if (vmax - vmin < 1e-30) { double m = fabs(vmax) > 0 ? fabs(vmax) * 0.05 : 1; vmin -= m; vmax += m; }
    double pad = (vmax - vmin) * 0.06; vmin -= pad; vmax += pad;
    double st = nice_step(vmax - vmin, 6);
    for (double v = ceil(vmin / st) * st; v <= vmax; v += st) {
        float py = p.y + p.height - (float)((v - vmin) / (vmax - vmin)) * p.height;
        DrawRectangleRec((Rectangle){ p.x, py, p.width, 1 }, TH.grid);
        char t[32]; snprintf(t, sizeof t, "%.4g", fabs(v) < st * 1e-6 ? 0.0 : v);
        ui_text(F_MONO, 11.5f, t, p.x - 10 - ui_text_w(F_MONO, 11.5f, t), py - 7, TH.text3);
    }
    double xs = nice_step(tmax - tmin, 6);
    for (double v = ceil(tmin / xs) * xs; v <= tmax + xs * 1e-6; v += xs) {
        float px = p.x + (float)((v - tmin) / (tmax - tmin)) * p.width;
        DrawRectangleRec((Rectangle){ px, p.y, 1, p.height }, ui_alpha(TH.grid, 0.6f));
        char lab[32]; snprintf(lab, sizeof lab, "%g", v);
        ui_text(F_REG, 11.5f, lab, px - ui_text_w(F_REG, 11.5f, lab) / 2, p.y + p.height + 8, TH.text3);
    }
    ui_clip_push(ui_inset(p, -2, -2));
    for (int i = 0; i < f->nCols; i++) {
        if (mon_hidden(c, f->col[i])) continue;
        Color col = TH.series[i % 10];
        int step = f->n > (int)(p.width * 2) ? f->n / (int)(p.width * 2) : 1;
        Vector2 prev = { 0 }; bool have = false;
        for (int k = 0; k < f->n; k += step) {
            float v = f->v[i][k];
            if (isnan(v)) { int q = k; while (q < k + step && q < f->n && isnan(f->v[i][q])) q++; if (q < f->n && q < k + step) v = f->v[i][q]; }
            if (isnan(v)) continue;
            Vector2 pt = { p.x + (f->t[k] - tmin) / (tmax - tmin) * p.width, p.y + p.height - (float)((v - vmin) / (vmax - vmin)) * p.height };
            if (have) DrawLineEx(prev, pt, 1.7f, col);
            prev = pt; have = true;
        }
    }
    ui_clip_pop();
    if (ui_hovered(p)) {                                       /* read-out at the mouse */
        Vector2 mp = ui_mouse();
        float t = tmin + (mp.x - p.x) / p.width * (tmax - tmin);
        DrawRectangleRec((Rectangle){ mp.x, p.y, 1, p.height }, ui_alpha(TH.text3, 0.6f));
        int lo = 0, hi = f->n - 1;
        while (lo < hi) { int mid = (lo + hi) / 2; if (f->t[mid] < t) lo = mid + 1; else hi = mid; }
        char lines[MON_MAX_COLS][80]; int cols[MON_MAX_COLS]; int nl = 0; float tw = 0;
        for (int i = 0; i < f->nCols && nl < MON_MAX_COLS; i++) {
            if (mon_hidden(c, f->col[i])) continue;
            float v = NAN;
            for (int k = lo; k >= 0 && k > lo - 50 && isnan(v); k--) v = f->v[i][k];
            if (isnan(v)) continue;
            snprintf(lines[nl], 80, "%s  %.5g", f->col[i], v);
            tw = fmaxf(tw, ui_text_w(F_MONO, 12, lines[nl]));
            cols[nl++] = i;
        }
        char head[48]; snprintf(head, sizeof head, "t = %.5g", f->t[lo]);
        float bw = fmaxf(tw, ui_text_w(F_MED, 12.5f, head)) + 40, bh = 30 + nl * 18;
        float bx = mp.x + 14 + bw > p.x + p.width ? mp.x - 14 - bw : mp.x + 14;
        Rectangle box = { bx, p.y + 8, bw, bh };
        ui_rect(box, 8, ui_alpha(TH.raised, 0.97f));
        ui_rect_lines(box, 8, 1, TH.line);
        ui_text(F_MED, 12.5f, head, box.x + 12, box.y + 8, TH.text);
        for (int k = 0; k < nl; k++) {
            DrawCircleV((Vector2){ box.x + 16, box.y + 34 + k * 18 }, 3.5f, TH.series[cols[k] % 10]);
            ui_text(F_MONO, 12, lines[k], box.x + 26, box.y + 27 + k * 18, TH.text2);
        }
    }
}

/* residuals, or (when the case writes function-object output) the monitors, on one switch */
static void chart_area(Rectangle r, Case *c, LogMon *pm)
{
    if (A.mons.n == 0) {
        run_chart(r, pm);
        /* nothing to plot and nothing running: the next action is to run it */
        if ((!pm || pm->nSeries == 0) && !A.cj->active && !c->s.runningLog[0]) {
            const char *lab = A.continueRun ? "Continue run" : "Run case";
            float bw = ui_button_width(ICON_PLAY, lab);
            if (ui_button(ui_id("chart-run"), (Rectangle){ r.x + (r.width - bw) / 2, r.y + r.height / 2 + 34, bw, 36 }, ICON_PLAY, lab, BTN_PRIMARY)) start_run();
        }
        return;
    }
    const char *items[] = { "Residuals", "Monitors" };
    char m[32]; snprintf(m, sizeof m, "Monitors (%d)", A.mons.n);
    items[1] = m;
    ui_segmented(ui_id("chart-tab"), (Rectangle){ r.x + r.width - 300, r.y, 300, 30 }, items, 2, &A.chartTab);
    if (A.chartTab == 1) chart_max_button(r.x + r.width - 310, r.y + 1);     /* the residual chart has its own */
    Rectangle body = { r.x, r.y + 38, r.width, r.height - 38 };
    if (A.chartTab == 1) mon_chart(body, c); else run_chart(body, pm);
}

/* stop the run when a monitored value has settled (left panel) */
static float converge_panel(Case *c, float x, float y, float w)
{
    if (A.mons.n == 0) return 0;
    float y0 = y;
    CaseCfg *g = &c->cfg;
    ui_text(F_MED, 13.5f, "Stop when converged", x, y + 2, TH.text);
    if (ui_switch(ui_id("conv-on"), x + w - 46, y, &g->convOn)) {
        if (!g->convFile[0]) { MonFile *f = mon_current(c); if (f) { pl_strlcpy(g->convFile, f->key, sizeof g->convFile); if (f->nCols) pl_strlcpy(g->convCol, f->col[0], sizeof g->convCol); } }
        if (g->convTol <= 0) g->convTol = 0.001;
        if (g->convWin <= 0) g->convWin = 200;
        casecfg_save(c->s.path, g);
    }
    y += 32;
    if (!g->convOn) {
        y += ui_text_wrap(F_REG, 12, "Ends the run (writing the time step it is on) once a monitored value, such as a force coefficient, stops changing.",
                          (Rectangle){ x, y, w, 60 }, 17, TH.text3, true) + 10;
        return y - y0;
    }
    /* which value: "file · column" */
    const char *opts[MON_MAX_FILES * 8]; static char buf[MON_MAX_FILES * 8][320]; int no = 0;
    for (int i = 0; i < A.mons.n && no < MON_MAX_FILES * 8; i++)
        for (int k = 0; k < A.mons.f[i].nCols && k < 8 && no < MON_MAX_FILES * 8; k++) {
            snprintf(buf[no], sizeof buf[no], "%s : %s", A.mons.f[i].key, A.mons.f[i].col[k]);
            opts[no] = buf[no]; no++;
        }
    char cur[400]; snprintf(cur, sizeof cur, "%s : %s", g->convFile, g->convCol);
    char pick[400] = "";
    if (ui_dropdown(ui_id("conv-col"), (Rectangle){ x, y, w, 34 }, cur, opts, no, DD_MONO, pick, sizeof pick, "value")) {
        char *sep = strstr(pick, " : ");
        if (sep) { *sep = 0; pl_strlcpy(g->convFile, pick, sizeof g->convFile); pl_strlcpy(g->convCol, sep + 3, sizeof g->convCol); casecfg_save(c->s.path, g); }
    }
    y += 42;
    float hw = (w - 12) / 2;
    ui_text(F_REG, 12, "Change below (%)", x, y, TH.text3);
    ui_text(F_REG, 12, "Over the last (samples)", x + hw + 12, y, TH.text3);
    y += 18;
    char tv[32], wv[32]; snprintf(tv, sizeof tv, "%g", g->convTol * 100); snprintf(wv, sizeof wv, "%d", g->convWin);
    if (ui_input(ui_id("conv-tol"), (Rectangle){ x, y, hw, 34 }, tv, sizeof tv, NULL, INPUT_NUMERIC)) { double v = atof(tv); if (v > 0) { g->convTol = v / 100; casecfg_save(c->s.path, g); } }
    if (ui_input(ui_id("conv-win"), (Rectangle){ x + hw + 12, y, hw, 34 }, wv, sizeof wv, NULL, INPUT_NUMERIC)) { int v = atoi(wv); if (v >= 5) { g->convWin = v; casecfg_save(c->s.path, g); } }
    y += 42;
    int k = monset_find(&A.mons, g->convFile);
    double ch = -1;
    if (k >= 0) {
        MonFile *f = &A.mons.f[k];
        int ci = -1; for (int i = 0; i < f->nCols; i++) if (!strcmp(f->col[i], g->convCol)) ci = i;
        bool ok = mon_converged(f, ci, g->convWin, g->convTol, &ch);
        char t[160];
        if (ch < 0) snprintf(t, sizeof t, "Waiting for %d samples (%d so far)", g->convWin, f->n);
        else snprintf(t, sizeof t, "Now: %.3g %% over the last %d%s", ch * 100, g->convWin, ok ? "  -  settled" : "");
        ui_text_clip(F_REG, 12.5f, t, x, y, w, ok ? TH.ok : TH.text2);
        y += 24;
    }
    return y - y0 + 8;
}

/* ============================================================================ log console */
/* error messages quote absolute paths; inside the case they read better (and wrap) relative to it */
static void case_relative(char *msg, size_t cap, const char *casePath)
{
    char pre[PL_PATH + 2]; snprintf(pre, sizeof pre, "%s/", casePath);
    size_t pl = strlen(pre);
    char *q;
    while ((q = strstr(msg, pre))) memmove(q, q + pl, strlen(q + pl) + 1);
    (void)cap;
}

/* ============================================================================ customisation */
/* the user's own shell commands around the solver, kept in <case>/.q-continuum.ini */
static float cmd_list(Case *c, char (*cmds)[256], int *n, const char *what, float x, float y, float w, bool locked)
{
    float y0 = y;
    ui_text(F_REG, 12.5f, what, x, y, TH.text3);
    y += 20;
    for (int k = 0; k < *n; k++) {
        char lg[64]; snprintf(lg, sizeof lg, "%s%d", cmds == c->cfg.pre ? "pre" : "post", k + 1);
        ui_disable(locked);
        if (ui_input(ui_idn(ui_id(lg), k), (Rectangle){ x, y, w - 42, 34 }, cmds[k], 256, "e.g. postProcess -func yPlus", INPUT_MONO))
            casecfg_save(c->s.path, &c->cfg);
        if (ui_button(ui_idn(ui_id("cmd-del"), (cmds == c->cfg.pre ? 0 : 100) + k), (Rectangle){ x + w - 36, y, 34, 34 }, ICON_X, NULL, BTN_ICON)) {
            memmove(cmds[k], cmds[k + 1], sizeof cmds[0] * (size_t)(*n - k - 1));
            (*n)--;
            casecfg_save(c->s.path, &c->cfg);
        }
        ui_disable(false);
        y += 40;
    }
    if (*n < CFG_MAX_CMDS) {
        ui_disable(locked);
        float bw = ui_button_width(ICON_PLUS, "Add command");
        if (ui_button(ui_idn(ui_id("cmd-add"), cmds == c->cfg.pre ? 0 : 1), (Rectangle){ x, y, bw, 30 }, ICON_PLUS, "Add command", BTN_GHOST)) { cmds[*n][0] = 0; (*n)++; }
        ui_disable(false);
        y += 36;
    }
    return y - y0;
}

static float user_steps(Case *c, float x, float y, float w, bool locked)
{
    float y0 = y;
    y += 6;
    ui_text(F_MED, 13.5f, "Your steps", x, y, TH.text);
    y += 24;
    y += ui_text_wrap(F_REG, 12, "Shell commands with the OpenFOAM environment loaded, run in the case folder. Each writes "
                      "log.step.<pre|post>N; a failing command stops the run.", (Rectangle){ x, y, w, 80 }, 17, TH.text3, true) + 8;
    char lab[96];
    snprintf(lab, sizeof lab, "Before %s", c->s.app[0] ? c->s.app : "the solver");
    y += cmd_list(c, c->cfg.pre, &c->cfg.nPre, lab, x, y, w, locked);
    snprintf(lab, sizeof lab, "After %s%s", c->s.app[0] ? c->s.app : "the solver", procs() > 1 ? " and reconstructPar" : "");
    y += cmd_list(c, c->cfg.post, &c->cfg.nPost, lab, x, y, w, locked);
    return y - y0 + 10;
}

/* scripts in the case folder (saveNow.sh, pause.sh, Allrun --resume, ...): one click each, arguments remembered */
static bool risky_script(const char *n)
{
    return pl_contains_ci(n, "clean") || !strcmp(n, "Allrun") || pl_contains_ci(n, "reset") || pl_contains_ci(n, "setup");
}

static float case_scripts(Case *c, float x, float y, float w)
{
    const NameList l = *pl_list_dir_cached(c->s.path, false, true);      /* every frame: cached, sorted */
    float y0 = y;
    bool header = false;
    for (int i = 0; i < l.n; i++) {
        const char *n = l.names[i];
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, n);
        if (n[0] == '.' || pl_starts_with(n, "log.") || !pl_is_exec(p)) continue;
        if (!header) {
            ui_divider(x, y, w); y += 18;
            ui_text(F_MED, 13.5f, "Case scripts", x, y, TH.text); y += 22;
            y += ui_text_wrap(F_REG, 12, "Executables in the case folder. Output goes to log.<script>; scripts that start a run in "
                              "the background keep running after this finishes.", (Rectangle){ x, y, w, 80 }, 17, TH.text3, true) + 8;
            header = true;
        }
        char args[128]; snprintf(args, sizeof args, "%s", casecfg_args(&c->cfg, n));
        ui_text_clip(F_MONO_MED, 13, n, x, y + 9, w * 0.38f, TH.text);
        float ax = x + w * 0.4f, aw = w * 0.6f - 44;
        if (ui_input(ui_idn(ui_id("scr-args"), i), (Rectangle){ ax, y, aw, 34 }, args, sizeof args, "arguments", INPUT_MONO)) {
            casecfg_set_args(&c->cfg, n, args);
            casecfg_save(c->s.path, &c->cfg);
        }
        ui_disable(A.cj->active);
        if (ui_button(ui_idn(ui_id("scr-run"), i), (Rectangle){ x + w - 36, y, 36, 34 }, ICON_PLAY, NULL, BTN_SECONDARY)) {
            snprintf(A.mText, sizeof A.mText, "%s", n);
            snprintf(A.mText2, sizeof A.mText2, "%s", args);
            if (risky_script(n) && !args[0]) { A.modal = MD_RUN_SCRIPT; A.mFlag2 = true; }
            else run_case_script(n, args);
        }
        ui_disable(false);
        ui_tooltip((Rectangle){ x + w - 36, y, 36, 34 }, "Run it in the case folder");
        y += 42;
    }
    /* a case inside a tutorial group (overset background + body meshes, precursor + main, ...): the group's own Allrun
       in the folder above runs the parts in order */
    char par[PL_PATH], pc[PL_PATH]; pl_dirname(par, sizeof par, c->s.path);
    pl_join(pc, sizeof pc, par, "system/controlDict");
    static const char *const gnames[] = { "Allrun", "Allclean", NULL };
    for (int g = 0; gnames[g] && !pl_is_file(pc); g++) {
        char gp[PL_PATH]; pl_join(gp, sizeof gp, par, gnames[g]);
        if (!pl_is_exec(gp)) continue;
        if (!header) { ui_divider(x, y, w); y += 18; ui_text(F_MED, 13.5f, "Case scripts", x, y, TH.text); y += 30; header = true; }
        char lab[64]; snprintf(lab, sizeof lab, "group %s", gnames[g]);
        ui_text_clip(F_MONO_MED, 13, lab, x, y + 9, w - 50, TH.text);
        ui_disable(A.cj->active);
        if (ui_button(ui_idn(ui_id("grp-run"), g), (Rectangle){ x + w - 36, y, 36, 34 }, ICON_PLAY, NULL, BTN_SECONDARY)) {
            char cmd[160], lg[64], t[64];
            snprintf(cmd, sizeof cmd, "chmod u+x ../%s 2>/dev/null\n../%s", gnames[g], gnames[g]);
            snprintf(lg, sizeof lg, "log.group.%s", gnames[g]);
            snprintf(t, sizeof t, "Group %s", gnames[g]);
            if (g == 1) { snprintf(A.mText, sizeof A.mText, "../%s", gnames[g]); A.mText2[0] = 0; A.modal = MD_RUN_SCRIPT; A.mFlag2 = true; }
            else app_start_tool(t, "script", t, cmd, lg);
        }
        ui_disable(false);
        char tip[PL_PATH + 64]; snprintf(tip, sizeof tip, "This case belongs to the tutorial group in %s: its %s runs every part in order", pl_basename(par), gnames[g]);
        ui_tooltip((Rectangle){ x + w - 36, y, 36, 34 }, tip);
        y += 42;
    }
    return y - y0 + (header ? 16 : 0);
}

/* the open case's last run was paused, or ended before its end time without an error (stopped, or the computer
   restarted under it): it can be resumed from its latest write */
bool app_resumable(bool *pausedOut)
{
    Case *c = A.cs;
    if (pausedOut) *pausedOut = false;
    if (!c || A.cj->active || c->s.runningLog[0] || !case_has_results(&c->s)) return false;
    if (case_paused(c->s.path, NULL, 0)) { if (pausedOut) *pausedOut = true; return true; }
    int prim = logset_primary(&A.logs, c->s.app);
    const LogMon *pm = prim >= 0 ? &A.logs.logs[prim] : NULL;
    const char *lt = case_latest_result(&c->s);
    if (pm && pm->offset < pl_file_size(pm->path)) return false;      /* a long log still being read: not decided yet */
    return pm && pm->steps > 0 && !pm->ended && !pm->fatal && c->s.endTime > 0 && atof(lt) < c->s.endTime * (1 - 1e-9);
}

/* "Before you run": memory, time step and cores, one line each; the full text on hover. Re-checked every few seconds. */
/* SENSOR SWEEP (3.1), every 2 s on the Run page and before each run; the dictionary cache makes it cheap */
/* the case summary with controlDict's time settings as edited, saved or not: the estimates are of the run about to start */
static void summary_with_edits(Case *c, CaseSummary *out)
{
    *out = c->s;
    FDFile *cd = case_dict(c, "system/controlDict");
    char v[64];
    if (!cd) return;
    if (fd_get(cd, "deltaT", v, sizeof v) && pl_is_number(v)) out->deltaT = atof(v);
    if (fd_get(cd, "endTime", v, sizeof v) && pl_is_number(v)) out->endTime = atof(v);
    if (fd_get(cd, "startTime", v, sizeof v) && pl_is_number(v)) out->startTime = atof(v);
}

const Sweep *run_sweep(Case *c, bool now)
{
    static Sweep sw; static double at; static char path[PL_PATH]; static unsigned long edits;
    if (now || GetTime() - at > 2.0 || strcmp(path, c->s.path) || edits != fd_edit_count) {   /* an edit shows at once (H-13) */
        at = GetTime(); pl_strlcpy(path, c->s.path, sizeof path); edits = fd_edit_count;
        sensor_sweep(c, &sw);
    }
    return &sw;
}

static void open_at(const Explanation *x)
{
    if (!x->file[0]) return;
    pl_strlcpy(A.filesSel, x->file, sizeof A.filesSel); A.filesRaw = true; text_editor_goto(x->line); app_go(PG_FILES);
}

static float precheck_panel(Case *c, float x, float y, float w)
{
    static PreCheck pc; static double at; static char path[PL_PATH]; static int np; static unsigned long edits;
    if (GetTime() - at > 3.0 || strcmp(path, c->s.path) || np != procs() || edits != fd_edit_count) {
        at = GetTime(); pl_strlcpy(path, c->s.path, sizeof path); np = procs(); edits = fd_edit_count;
        CaseSummary cs; summary_with_edits(c, &cs);
        precheck_run(&cs, np, app_jobs_procs(), A.env.nCpu, &pc);
    }
    float y0 = y;
    ui_text(F_MED, 13.5f, "Before you run", x, y, TH.text);
    y += 26;
    const PreItem *it[3] = { &pc.mem, &pc.co, &pc.cores };
    for (int i = 0; i < 3; i++) {
        const PreItem *p = it[i];
        Color col = p->level == PC_BAD ? TH.err : p->level == PC_WARN ? TH.warn : p->level == PC_OK ? TH.ok : TH.text3;
        const char *ic = p->level == PC_BAD ? ICON_CIRCLE_X : p->level == PC_WARN ? ICON_TRIANGLE_ALERT : p->level == PC_OK ? ICON_CIRCLE_CHECK : ICON_INFO;
        ui_icon(ic, x + 8, y + 9, 14, col);
        char line[200]; snprintf(line, sizeof line, "%s: %s", p->title, p->brief);
        ui_text_clip(F_REG, 12.5f, line, x + 24, y + 1, w - 24, p->level == PC_OK || p->level == PC_UNKNOWN ? TH.text2 : col);
        ui_tooltip((Rectangle){ x, y - 2, w, 22 }, p->text);
        y += 24;
    }
    /* the sweep: problems and warnings, each openable and, where safe, fixable */
    const Sweep *sw = run_sweep(c, false);
    int shown = 0, nProb = 0;
    for (int i = 0; i < sw->n; i++) if (sw->f[i].level >= PC_WARN) nProb++;
    for (int i = 0; i < sw->n && shown < 7; i++) {
        const Finding *f = &sw->f[i];
        Color col = f->level == PC_BAD ? TH.err : f->level == PC_WARN ? TH.warn : TH.ok;
        const char *ic = f->level == PC_BAD ? ICON_CIRCLE_X : f->level == PC_WARN ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK;
        float fw = f->x.fix ? ui_button_width(NULL, "Fix") + 4 : 0;
        Rectangle row = { x, y - 2, w - fw, 22 };
        bool hov = ui_hovered(row) && f->x.file[0];
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_icon(ic, x + 8, y + 9, 14, col);
        ui_text_clip(F_REG, 12.5f, f->x.title, x + 24, y + 1, w - 24 - fw - 4, f->level == PC_OK ? TH.text2 : col);
        char tip[1100];
        if (f->x.file[0]) pl_fmt_ui(tip, sizeof tip, "%s\n%s%s%d. Click to open it there.", f->x.text, f->x.file, f->x.line > 0 ? " line " : "", f->x.line);
        else pl_fmt_ui(tip, sizeof tip, "%s", f->x.text);
        if (f->x.line <= 0 && f->x.file[0]) { char *l = strstr(tip, f->x.file); if (l) pl_fmt_ui(l, (size_t)(sizeof tip - (size_t)(l - tip)), "%s. Click to open it.", f->x.file); }
        ui_register(ui_idn(ui_id("sweep"), i), row, f->x.title);
        ui_tooltip(row, tip);
        if (hov && ui_clicked(row)) open_at(&f->x);
        if (f->x.fix) {
            Rectangle fb = { x + w - fw + 4, y - 3, fw - 4, 24 };
            ui_disable(A.cj->active && !strcmp(A.cj->casePath, c->s.path));
            if (ui_button(ui_idn(ui_id("sweep-fix"), i), fb, NULL, "Fix", BTN_SECONDARY)) { Explanation ex = f->x; app_apply_fix(&ex); run_sweep(c, true); }
            ui_disable(false);
            ui_tooltip(fb, f->x.fixLabel);
        }
        y += 24;
        shown++;
    }
    if (!nProb) {
        char t[96]; pl_fmt_ui(t, sizeof t, "%s: %d check%s, all clear", term(T_SWEEP), sw->checks, sw->checks == 1 ? "" : "s");
        ui_icon(ICON_CIRCLE_CHECK, x + 8, y + 9, 14, TH.ok);
        ui_text_clip(F_REG, 12.5f, t, x + 24, y + 1, w - 24, TH.text2);
        ui_tooltip((Rectangle){ x, y - 2, w, 22 }, "Patch conditions, schemes and solvers, pressure units, decomposition, regions, output and leftovers "
                   "were read from the case's dictionaries (unsaved edits included).");
        y += 24;
    }
    return y - y0 + 14;
}

/* start a queued run in its case, open or not, without dialogs: the queue entry says how (mode -1: the case's
   default; cont: continue from the latest time; fresh: old results go to the trash). The open case's page
   state is put back afterwards. */
/* the queue starts a case only once the solver's -help has said whether it runs in parallel (asked in the background):
   waiting for it inside the start froze the window for a second (bug hunt H-02) */
bool run_queue_ready(const char *path)
{
    CaseSummary cs; case_summarize(path, &cs);
    return cs.prog || !cs.app[0] || app_parallel_known(cs.app);
}

bool run_start_queued(const char *path, int mode, int procs, bool cont, bool fresh)
{
    {   /* another program's project: its own run (page_prog.c), on its first input */
        CaseSummary ps; case_summarize(path, &ps);
        if (ps.prog) return prog_start_run(path, ps.input, procs > 0 ? procs : 1, true);
    }
    struct { Case *cs; Job *cj; int runMode; bool stepOn[16]; bool stepInit; int runProcs; bool continueRun, runConfirmed,
             runPending, precheckOk; int runLog; bool runFollow; Modal modal; } sv =
        { A.cs, A.cj, A.runMode, { 0 }, A.stepInit, A.runProcs, A.continueRun, A.runConfirmed, A.runPending, A.precheckOk,
          A.runLog, A.runFollow, A.modal };
    memcpy(sv.stepOn, A.stepOn, sizeof sv.stepOn);
    bool open = A.cs && !strcmp(A.cs->s.path, path);
    Case *c = open ? A.cs : case_open(path);
    if (!c) { ui_toast(TOAST_ERR, "Queue: %s is no longer an OpenFOAM case", path); return false; }
    A.cs = c;
    if (!open) A.cj = job_slot_for(path);
    bool ok = false;
    if (A.cj == &A.jobs[MAX_JOBS] || A.cj->active) ui_toast(TOAST_WARN, "Queue: no free run slot for %s", c->s.name);
    else {
        A.stepInit = false;
        init_steps();
        if (mode >= 0) A.runMode = c->s.hasAllrun ? mode : 0;
        if (cont) A.runMode = 0;                                /* continuing is a built-in run on the existing results */
        A.runProcs = procs > 0 ? procs : 0;
        A.continueRun = cont && case_has_results(&c->s);
        A.runConfirmed = fresh || cont;
        A.precheckOk = true;
        A.bgStart = true;
        A.modal = MD_NONE;
        start_run();
        ok = A.cj->active && !strcmp(A.cj->casePath, path);
        A.bgStart = false;
    }
    if (!open) case_close(c);
    A.cs = sv.cs; A.cj = sv.cj; A.runMode = sv.runMode; memcpy(A.stepOn, sv.stepOn, sizeof sv.stepOn); A.stepInit = sv.stepInit;
    A.runProcs = sv.runProcs; A.continueRun = sv.continueRun; A.runConfirmed = sv.runConfirmed; A.runPending = sv.runPending;
    A.precheckOk = sv.precheckOk; A.runLog = sv.runLog; A.runFollow = sv.runFollow;
    if (A.modal == MD_NONE) A.modal = sv.modal;
    if (open && ok) A.stepInit = false;
    return ok;
}

/* continue the paused or interrupted run in place, with the built-in steps (the mesh and decomposition are there) */
void app_resume_run(void)
{
    if (!A.cs) return;
    A.runMode = 0;
    A.continueRun = true;
    A.runConfirmed = true;
    A.runProcs = 0;
    /* the same process count as before: the decomposition when the results are in processor*, else what the log says */
    int prim = logset_primary(&A.logs, A.cs->s.app);
    if (A.cs->s.procTimes == 0 && prim >= 0 && A.logs.logs[prim].nProcs > 0) A.runProcs = A.logs.logs[prim].nProcs;
    start_run();
}

void run_case_script(const char *name, const char *args)
{
    char q[300], cmd[900], lg[96], title[96];
    pl_shell_quote(q, sizeof q, name);
    snprintf(cmd, sizeof cmd, "chmod u+x ./%s 2>/dev/null\n" SH_RUN "qc_sh ./%s %s", q, q, args ? args : "");
    if (pl_starts_with(name, "../")) snprintf(lg, sizeof lg, "log.group.%s", name + 3);   /* the tutorial group's script */
    else snprintf(lg, sizeof lg, "log.%s", name);
    snprintf(title, sizeof title, "%s", name);
    app_start_tool(title, "script", name, cmd, lg);
}

/* ============================================================================ page */
static void kpi(Rectangle r, const char *label, const char *value, Color vc)
{
    ui_text(F_REG, 12.5f, label, r.x, r.y, TH.text3);
    ui_text_clip(F_SEMI, 19, value, r.x, r.y + 20, r.width - 6, vc);
}

extern bool g_autorun;

/* after a failed run: the log that holds the error (an Allrun's failing tool writes its own log), newest first; else
   the newest log that ended on a warning (reconstructPar's "No times selected" is a warning with a failing exit code) */
static bool allrun_log(const char *n) { return pl_starts_with(n, "log.Allrun") || pl_starts_with(n, "log.group.") || pl_starts_with(n, "log.Allrun-"); }
static int failing_log(void)
{
    int best = -1, warn = -1;
    for (int i = 0; i < A.logs.n; i++) {
        const LogMon *m = &A.logs.logs[i];
        if (m->fatal && (best < 0 || m->mtime >= A.logs.logs[best].mtime)) best = i;
        if (!m->fatal && m->warnMsg[0] && !m->ended && !allrun_log(m->name) && (warn < 0 || m->mtime >= A.logs.logs[warn].mtime)) warn = i;
    }
    return best >= 0 ? best : warn;
}
static const char *log_error(const LogMon *m) { return m->fatal ? m->fatalMsg : m->warnMsg; }

/* the log shown: the one picked, else the current step's while running, else the failing one, else the solver's */
static int run_log_sel(int prim, bool mine)
{
    int fail = !mine && A.runLog < 0 && A.cj->failed && !strcmp(A.cj->casePath, A.cs->s.path) ? failing_log() : -1;
    if (fail >= 0) return fail;
    int sel = A.runLog >= 0 && A.runLog < A.logs.n ? A.runLog : (!mine && prim >= 0 ? prim : logset_newest(&A.logs));
    if (A.runLog < 0 && mine && A.cj->cur < A.cj->n && A.cj->steps[A.cj->cur].log[0]) {
        int k = logset_find(&A.logs, A.cj->steps[A.cj->cur].log);
        if (k >= 0) sel = k;
    }
    return sel;
}

void page_run(Rectangle r)
{
    if (!A.cs) return;
    if (g_autorun && A.cs && !A.cj->active) { g_autorun = false; init_steps(); A.precheckOk = true; start_run(); }
    if (A.runPending && A.cs && !A.cj->active && A.modal == MD_NONE) { A.runPending = false; init_steps(); start_run(); }
    Case *c = A.cs;
    if (!c) return;                                           /* start_run can leave no case open (a failed queue start) */
    init_steps();
    bool mine = A.cj->active && !strcmp(A.cj->casePath, c->s.path);
    bool jobHere = !strcmp(A.cj->casePath, c->s.path) && A.cj->n > 0;
    if (A.chartMax) {                                         /* the chart takes the whole page */
        int prim = logset_primary(&A.logs, c->s.app);
        float x = r.x + 32, y = r.y + 22, w = r.width - 64;
        char t[400];
        if (mine && A.cj->cur < A.cj->n) snprintf(t, sizeof t, "%s  ·  running step %d of %d: %s", c->s.name, A.cj->cur + 1, A.cj->n, A.cj->steps[A.cj->cur].label);
        else snprintf(t, sizeof t, "%s  ·  residuals", c->s.name);
        ui_text(F_SEMI, 16, t, x, y, TH.text);
        if (mine && ui_button(ui_id("max-stop"), (Rectangle){ x + w - 110, y - 6, 110, 32 }, ICON_SQUARE, "Stop", BTN_DANGER)) job_stop(A.cj, GetTime());
        if (mine && app_can_pause()) {
            float pw = ui_button_width(ICON_PAUSE, "Pause");
            if (ui_button(ui_id("max-pause"), (Rectangle){ x + w - 120 - pw, y - 6, pw, 32 }, ICON_PAUSE, "Pause", BTN_SECONDARY)) app_pause_run();
        }
        LogMon *pm = prim >= 0 ? &A.logs.logs[prim] : NULL;
        chart_area((Rectangle){ x, y + 38, w, r.y + r.height - y - 38 - 24 }, c, pm);
        if (!pm || pm->nSeries == 0) chart_max_button(x + w, y + 44);  /* an empty chart has no button of its own: the way back */
        return;
    }
    if (A.logMax) {                                           /* the log takes the whole page */
        int prim = logset_primary(&A.logs, c->s.app);
        float x = r.x + 32, y = r.y + 22, w = r.width - 64;
        char t[400];
        if (mine && A.cj->cur < A.cj->n) snprintf(t, sizeof t, "%s  ·  running step %d of %d: %s", c->s.name, A.cj->cur + 1, A.cj->n, A.cj->steps[A.cj->cur].label);
        else snprintf(t, sizeof t, "%s  ·  logs", c->s.name);
        ui_text(F_SEMI, 16, t, x, y, TH.text);
        if (mine && ui_button(ui_id("max-stop"), (Rectangle){ x + w - 110, y - 6, 110, 32 }, ICON_SQUARE, "Stop", BTN_DANGER)) job_stop(A.cj, GetTime());
        if (mine && app_can_pause()) {
            float pw = ui_button_width(ICON_PAUSE, "Pause");
            if (ui_button(ui_id("max-pause"), (Rectangle){ x + w - 120 - pw, y - 6, pw, 32 }, ICON_PAUSE, "Pause", BTN_SECONDARY)) app_pause_run();
        }
        run_console((Rectangle){ x, y + 38, w, r.y + r.height - y - 38 - 24 }, &A.logs, run_log_sel(prim, mine));
        return;
    }
    bool pausedNow = false, resumeOffer = !mine && app_resumable(&pausedNow);
    float lw = 400;
    Rectangle left = { r.x, r.y, lw, r.height };
    DrawRectangleRec((Rectangle){ r.x + lw - 1, r.y, 1, r.height }, TH.lineSoft);

    /* ---------------- left: setup */
    UiScroll *sc = &A.scr[13];
    float top = ui_scroll_begin(sc, left);
    float x = left.x + 28, w = lw - 56, y = top + 28;
    ui_text(F_SEMI, TH.lcars ? 26 : 20, ui_cap("Run"), x, y - (TH.lcars ? 3 : 0), head_color()); y += 40;
    {   /* a preCICE participant runs with its partners, from the coupled run */
        char par[PL_PATH], who[64];
        if (precice_participant(c, par, sizeof par, who, sizeof who)) {
            char m[400]; pl_fmt_ui(m, sizeof m, "%s is the %s participant of a preCICE coupling: alone it waits for its partners. "
                                   "Run all of them from the coupled run.", c->s.name, who[0] ? who : "preCICE");
            float ch = callout_height(w, NULL, m) + (par[0] ? 44 : 0);
            callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, m);
            if (par[0] && case_is_project(par)) {
                char lab[160]; pl_fmt_ui(lab, sizeof lab, "Go to %s", pl_basename(par));
                float bw = ui_button_width(ICON_ARROW_LEFT, lab);
                if (ui_button(ui_id("run-cpl-parent"), (Rectangle){ x + 42, y + ch - 42, bw, 32 }, ICON_ARROW_LEFT, lab, BTN_SECONDARY)) {
                    char back[PL_PATH]; pl_strlcpy(back, par, sizeof back);
                    app_open_case(back, PG_RUN);
                    ui_scroll_end(sc, 0);
                    return;
                }
            }
            y += ch + 14;
        }
    }
    FDFile *cd = case_dict(c, "system/controlDict");
    if (cd) {
        ui_disable(mine);
        ui_text(F_MED, 13, "Solver", x, y, TH.text2); y += 22;
        if (dv_choice(cd, "application", (Rectangle){ x, y, w, 36 }, SOLVER_APPS, N_SOLVER_APPS, true, "application")) { case_rescan(c); }
        y += 48;
        float hw = (w - 12) / 2;
        ui_text(F_MED, 13, "End time", x, y, TH.text2);
        ui_text(F_MED, 13, "Time step", x + hw + 12, y, TH.text2);
        y += 22;
        if (dv_number(cd, "endTime", (Rectangle){ x, y, hw, 36 }, "1")) case_rescan(c);
        if (dv_number(cd, "deltaT", (Rectangle){ x + hw + 12, y, hw, 36 }, "0.001")) case_rescan(c);
        y += 48;
        ui_text(F_MED, 13, "Write every", x, y, TH.text2);
        ui_text(F_MED, 13, "Counting", x + hw + 12, y, TH.text2);
        y += 22;
        dv_number(cd, "writeInterval", (Rectangle){ x, y, hw, 36 }, "100");
        const char *const *wc; int nwc = suggest_for_key("writeControl", NULL, &wc);
        dv_choice(cd, "writeControl", (Rectangle){ x + hw + 12, y, hw, 36 }, wc, nwc, false, "timeStep");
        y += 50;
        if (fd_find(cd, "adjustTimeStep")) {
            ui_text(F_MED, 13, "Adaptive time step", x, y + 2, TH.text);
            dv_switch(cd, "adjustTimeStep", x + w - 80, y, "no");
            y += 34;
            if (fd_find(cd, "maxCo")) {
                ui_text(F_REG, 13, "Max Courant", x, y + 9, TH.text2);
                dv_number(cd, "maxCo", (Rectangle){ x + w - 120, y, 120, 34 }, "1");
                y += 44;
            }
        }
        ui_disable(false);
    }
    ui_divider(x, y, w); y += 20;

    /* parallel */
    bool allrunSerial = A.runMode == 1 && !allrun_parallel(c);
    bool appSerial = A.runMode == 0 && !app_runs_parallel(c->s.app);
    ui_disable(mine || allrunSerial || appSerial);
    ui_text(F_MED, 13.5f, "Processes", x, y + 8, TH.text);
    char np[16]; snprintf(np, sizeof np, "%d", procs());
    Rectangle pr = { x + w - 132, y, 132, 34 };
    if (ui_button(ui_id("np-dec"), (Rectangle){ pr.x, pr.y, 34, 34 }, ICON_CHEVRON_LEFT, NULL, BTN_SECONDARY) ) set_procs(procs() - 1);
    if (ui_input(ui_id("np-in"), (Rectangle){ pr.x + 40, pr.y, 52, 34 }, np, sizeof np, NULL, INPUT_NUMERIC)) { int v = atoi(np); if (v >= 1) set_procs(v); }
    if (ui_button(ui_id("np-inc"), (Rectangle){ pr.x + 98, pr.y, 34, 34 }, ICON_CHEVRON_RIGHT, NULL, BTN_SECONDARY)) set_procs(procs() + 1);
    y += 40;
    char pinfo[128];
    if (allrunSerial) snprintf(pinfo, sizeof pinfo, "Allrun runs in serial: it doesn't decompose the case");
    else if (appSerial) snprintf(pinfo, sizeof pinfo, "%s runs in serial only (it has no -parallel option)", c->s.app);
    else if (A.runProcsFrom > procs()) snprintf(pinfo, sizeof pinfo, "Set to %d, not %d%s: %ld cells is too few to split (aim for %d+ per process)", procs(), A.runProcsFrom,
                                               A.runProcsDict ? " from decomposeParDict" : "", A.runProcsCells, CELLS_PER_RANK);
    else if (procs() > 1) snprintf(pinfo, sizeof pinfo, "Parallel with mpirun on %d of %d cores", procs(), A.env.nCpu);
    else if (A.runMode == 1) snprintf(pinfo, sizeof pinfo, "Allrun runs in parallel with its own decomposeParDict count");
    else snprintf(pinfo, sizeof pinfo, "Serial run (%d cores available)", A.env.nCpu);
    y += ui_text_wrap(F_REG, 12.5f, pinfo, (Rectangle){ x, y, w, 60 }, 17, TH.text3, true) - 17;
    ui_disable(mine);                                    /* the steps and the Built-in/Allrun choice stay usable */
    y += 32;
    ui_divider(x, y, w); y += 20;
    if (!mine) { y += precheck_panel(c, x, y, w); ui_divider(x, y - 4, w); y += 16; }
    { ui_disable(false); float ch = converge_panel(c, x, y, w); if (ch > 0) { y += ch; ui_divider(x, y - 4, w); y += 16; } ui_disable(mine); }

    /* pipeline */
    ui_text(F_MED, 13.5f, "Steps", x, y, TH.text);
    y += 28;
    if (multi_region()) {
        char body[900] = "";
        int k = snprintf(body, sizeof body, "%d regions:", c->s.nRegions);
        for (int r = 0; r < c->s.nRegions && k < (int)sizeof body - 80; r++)
            k += snprintf(body + k, sizeof body - k, "%s %s", r ? "," : "", c->s.regions[r]);
        snprintf(body + k, sizeof body - k, ". %s",
                 c->s.regionMeshed ? "Region meshes are in place." : "The mesh still has to be split into regions before the solver can run.");
        float ch = callout_height(w, "Multi-region case", body);
        callout((Rectangle){ x, y, w, ch }, c->s.regionMeshed ? TOAST_INFO : TOAST_WARN, ICON_LAYERS, "Multi-region case", body);
        y += ch + 14;
    }
    if (c->s.hasAllrun) {
        const char *modes[] = { "Built-in steps", c->s.groupAllrun ? "Group Allrun" : c->s.allrun[0] && strcmp(c->s.allrun, "Allrun") ? c->s.allrun : "Allrun script" };
        int was = A.runMode;
        ui_segmented(ui_id("run-mode"), (Rectangle){ x, y, w, 34 }, modes, 2, &A.runMode);
        if (A.runMode != was) A.runProcs = 0;                  /* Allrun may decompose with its own dictionary */
        y += 46;
    } else A.runMode = 0;
    char why[160];
    if (A.runMode == 1) {
        y += ui_text_wrap(F_REG, 13, c->s.groupAllrun
                          ? "This case is part of a tutorial group: the group's Allrun in the folder above prepares the other parts and "
                            "then runs this one. Old log files in the group are cleared first so every step runs again."
                          : "Runs the case's own Allrun script, as the tutorial authors intended. Old log files are cleared first "
                            "so every step runs again.", (Rectangle){ x, y, w, 200 }, 19, TH.text2, true) + 12;
    } else if (c->s.hasAllrun && allrun_needs_script_cached(c, why, sizeof why)) {
        char msg[300]; snprintf(msg, sizeof msg, "The built-in steps may not prepare this case: its Allrun %s. Use Allrun script if the run fails.", why + 3);
        float ch = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, msg);
        y += ch + 12;
    } else {
        if (!A.continueRun && (c->s.meshStale || c->s.regionStale)) {
            char body[400], when[32] = "";
            if (c->s.meshTime) { struct tm tmv; localtime_r(&c->s.meshTime, &tmv); strftime(when, sizeof when, "%d %b %H:%M", &tmv); }
            if (c->s.meshStale)
                snprintf(body, sizeof body, "%s was changed after the mesh in constant/polyMesh was written (%s). The mesh steps run again "
                         "so the solver doesn't use the old mesh.", c->s.meshStaleDict, when);
            else
                snprintf(body, sizeof body, "The region meshes are older than constant/polyMesh or system/topoSetDict. topoSet and "
                         "splitMeshRegions run again.");
            float ch = callout_height(w, "Mesh out of date", body);
            callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Mesh out of date", body);
            y += ch + 14;
        }
        int pn = procs();
        for (int s = 0; s < ST_COUNT; s++) {
            if (!step_available(s, pn)) continue;
            bool forced = s == ST_SOLVER || s == ST_DECOMPOSE || s == ST_RECONSTRUCT;
            bool skipCont = A.continueRun && (s == ST_RESTORE || s == ST_BLOCKMESH || s == ST_SNAPPY || s == ST_FEATURES || s == ST_SETFIELDS ||
                                              s == ST_TOPOSET || s == ST_SPLIT || s == ST_RESTORE_REGIONS || s == ST_CHANGEDICT ||
                                              (s == ST_DECOMPOSE && c->s.procTimes > 0));
            /* state from the job, if it ran here */
            StepState stt = JS_PENDING; double dur = -1;
            if (jobHere) {
                const char *lab = s == ST_SOLVER ? c->s.app : STEP_LABEL[s];
                for (int k = 0; k < A.cj->n; k++)
                    if (!strcmp(A.cj->steps[k].label, lab)) {
                        stt = A.cj->steps[k].st;
                        if (A.cj->steps[k].t0 > 0) dur = (A.cj->steps[k].t1 > 0 ? A.cj->steps[k].t1 : GetTime()) - A.cj->steps[k].t0;
                    }
            }
            Rectangle row = { x, y, w, 44 };
            bool on = !skipCont && (forced || A.stepOn[s]);
            ui_disable(forced || skipCont || mine);
            bool b = on;
            if (ui_checkbox(ui_idn(ui_id("step"), s), (Rectangle){ row.x, row.y + 4, 20, 20 }, &b, NULL)) A.stepOn[s] = b;
            ui_disable(false);
            const char *lab = s == ST_SOLVER ? (c->s.app[0] ? c->s.app : "Solver") : STEP_LABEL[s];
            ui_text(s == ST_SOLVER ? F_SEMI : F_MED, 13.5f, lab, row.x + 30, row.y + 3, on ? TH.text : TH.text3);
            const char *hint = s == ST_SOLVER ? (pn > 1 ? "Run in parallel with mpirun" : "Run the solver") : STEP_HINT[s];
            ui_text_clip(F_REG, 12, skipCont ? "Skipped when continuing" : hint, row.x + 30, row.y + 22, w - 110, TH.text3);
            float ix = row.x + w - 12;
            if (stt == JS_RUNNING) ui_spinner(ix, row.y + 12, 7, TH.accent);
            else if (stt == JS_DONE) ui_icon(ICON_CIRCLE_CHECK, ix, row.y + 12, 16, TH.ok);
            else if (stt == JS_FAILED) ui_icon(ICON_CIRCLE_X, ix, row.y + 12, 16, TH.err);
            else if (stt == JS_STOPPED) ui_icon(ICON_OCTAGON_X, ix, row.y + 12, 16, TH.warn);
            if (dur >= 0) { char d[32]; pl_duration(d, sizeof d, dur); ui_text(F_MONO, 12, d, ix - 20 - ui_text_w(F_MONO, 12, d), row.y + 4, TH.text3); }
            y += 46;
        }
    }
    if (A.runMode == 0) y += user_steps(c, x, y, w, mine);
    char contWhy[300] = "";
    bool meshNew = A.runMode == 0 && mesh_change_pending(c, contWhy, sizeof contWhy);
    ui_disable(mine || !case_has_results(&c->s) || meshNew);
    bool cont = A.continueRun && case_has_results(&c->s) && !meshNew;
    ui_checkbox(ui_id("run-cont"), (Rectangle){ x, y, w, 24 }, &cont, "Continue from the latest time");
    A.continueRun = cont;
    ui_disable(false);
    if (meshNew && case_has_results(&c->s)) {
        char tip[400]; snprintf(tip, sizeof tip, "Not possible: %s The run has to start over on the new mesh.", contWhy);
        ui_tooltip((Rectangle){ x, y, w, 24 }, tip);
    }
    y += 40;
    char pausedAt[32] = "";
    bool paused = resumeOffer && pausedNow && case_paused(c->s.path, pausedAt, sizeof pausedAt);
    if (mine) {
        if (app_can_pause()) {
            float hw = (w - 10) / 2;
            ui_disable(A.cj->pausing);
            if (ui_button(ui_id("run-pause"), (Rectangle){ x, y, hw, 42 }, ICON_PAUSE, A.cj->pausing ? "Pausing..." : "Pause", BTN_SECONDARY)) app_pause_run();
            ui_disable(false);
            ui_tooltip((Rectangle){ x, y, hw, 42 }, "Write the time step it is on and stop; Resume continues from there");
            if (ui_button(ui_id("run-stop"), (Rectangle){ x + hw + 10, y, hw, 42 }, ICON_SQUARE, "Stop run", BTN_DANGER)) job_stop(A.cj, GetTime());
        } else if (ui_button(ui_id("run-stop"), (Rectangle){ x, y, w, 42 }, ICON_SQUARE, "Stop run", BTN_DANGER)) job_stop(A.cj, GetTime());
    } else if (resumeOffer) {
        /* the last run was paused, or stopped before its end time: continue it in place */
        char lab[96]; snprintf(lab, sizeof lab, "Resume from t = %s", case_latest_result(&c->s));
        ui_disable(A.cj->active);
        if (ui_button(ui_id("run-resume"), (Rectangle){ x, y, w, 42 }, ICON_PLAY, lab, BTN_PRIMARY)) app_resume_run();
        y += 50;
        if (ui_button(ui_id("run-go"), (Rectangle){ x, y, w, 36 }, ICON_ROTATE_CCW, "Start over instead", BTN_GHOST)) { A.continueRun = false; case_pause_clear(c->s.path); start_run(); }
        ui_disable(false);
    } else {
        ui_disable(A.cj->active);
        if (ui_button(ui_id("run-go"), (Rectangle){ x, y, w, 42 }, ICON_PLAY, A.continueRun ? "Continue run" : "Run case", BTN_PRIMARY)) start_run();
        ui_disable(false);
    }
    if (!mine) {
        y += 50;
        bool queued = app_queue_has(c->s.path);
        ui_disable(queued);
        if (ui_button(ui_id("run-queue"), (Rectangle){ x, y, w, 34 }, ICON_LIST, queued ? "In the queue" : "Add to queue", BTN_GHOST)) {
            if (case_has_results(&c->s)) { A.modal = MD_QUEUE_ADD; }
            else { app_queue_add(c->s.path, c->s.hasAllrun ? A.runMode : -1, procs(), false, false); }
        }
        ui_disable(false);
        ui_tooltip((Rectangle){ x, y, w, 34 }, "Run it when cores are free, after the runs queued before it (Queue page)");
        y -= 8;
    }
    y += 58;
    float hb = ui_button_width(ICON_EXTERNAL_LINK, "Open in ParaView");
    if (ui_button(ui_id("run-pv"), (Rectangle){ x, y, hb, 34 }, ICON_EXTERNAL_LINK, "Open in ParaView", BTN_SECONDARY)) app_open_paraview();
    if (ui_button(ui_id("run-clean"), (Rectangle){ x + hb + 8, y, w - hb - 8, 34 }, ICON_TRASH_2, "Clean results", BTN_GHOST)) { A.modal = MD_CLEAN; A.mFlag = A.mFlag2 = false; }
    y += 44;
    if (ui_button(ui_id("run-pvc"), (Rectangle){ x, y, w, 34 }, ICON_SPARKLES, "ParaView with Claude", BTN_SECONDARY)) { A.modal = MD_PV_CLAUDE; A.mText2[0] = 0; }
    y += 44;
    ui_disable(!case_has_results(&c->s) || A.cj->active);
    if (ui_button(ui_id("run-movie"), (Rectangle){ x, y, w, 34 }, ICON_MONITOR, "Make a movie", BTN_SECONDARY)) A.modal = MD_MOVIE;
    ui_disable(false);
    ui_tooltip((Rectangle){ x, y, w, 34 }, "Every written time rendered with ParaView and encoded to an mp4");
    y += 44;
    if (ui_button(ui_id("run-away"), (Rectangle){ x, y, w, 34 }, ICON_HARD_DRIVE, term(T_AWAY), BTN_SECONDARY)) A.modal = MD_AWAY;   /* 5.1 */
    ui_tooltip((Rectangle){ x, y, w, 34 }, "Pack this case for aerofly (OpenFOAM v2412, Slurm): checked with v2412 here, a batch script, and "
               "the commands to copy it there");
    y += 50;
    y += case_scripts(c, x, y, w);
    ui_scroll_end(sc, y - top);

    /* ---------------- right: monitor */
    Rectangle mon = { r.x + lw, r.y, r.width - lw, r.height };
    float mx = mon.x + 32, mw = mon.width - 64, my = mon.y + 26;
    int prim = logset_primary(&A.logs, c->s.app);
    LogMon *pm = prim >= 0 ? &A.logs.logs[prim] : NULL;
    /* status line */
    const char *stTxt; Color stC; const char *stIc;
    bool failed = false, stopped = false;
    char stBuf[80];
    if (mine && A.cj->pausing) { stTxt = "Pausing"; stC = TH.warn; stIc = NULL; }
    else if (mine) { stTxt = "Running"; stC = TH.accent; stIc = NULL; }
    else if (paused) { snprintf(stBuf, sizeof stBuf, "Paused at t = %s", pausedAt[0] ? pausedAt : case_latest_result(&c->s)); stTxt = stBuf; stC = TH.warn; stIc = ICON_PAUSE; stopped = true; }
    else if (jobHere && A.cj->failed) { stTxt = "Failed"; stC = TH.err; stIc = ICON_CIRCLE_X; failed = true; }
    else if (jobHere && A.cj->stopped) { stTxt = "Stopped"; stC = TH.warn; stIc = ICON_OCTAGON_X; stopped = true; }
    else if (jobHere && A.cj->finished) { stTxt = "Finished"; stC = TH.ok; stIc = ICON_CIRCLE_CHECK; }
    else if (pm && pm->fatal) { stTxt = "Last run failed"; stC = TH.err; stIc = ICON_CIRCLE_X; failed = true; }
    else if (pm && pm->ended) { stTxt = "Last run finished"; stC = TH.ok; stIc = ICON_CIRCLE_CHECK; }
    else if (c->s.runningLog[0]) { stTxt = "Running in another process"; stC = TH.accent; stIc = ICON_CIRCLE_DOT; }
    else if (pm && pm->steps > 0) { stTxt = "Last run stopped"; stC = TH.warn; stIc = ICON_OCTAGON_X; stopped = true; }
    else { stTxt = "Ready"; stC = TH.text2; stIc = ICON_CIRCLE_DOT; }
    if (stIc) ui_icon(stIc, mx + 10, my + 13, 20, stC); else ui_spinner(mx + 10, my + 13, 9, stC);
    ui_text(F_SEMI, TH.lcars ? 24 : 20, ui_cap(stTxt), mx + 30, my - (TH.lcars ? 2 : 0), TH.lcars ? stC : TH.text);
    if (mine && A.cj->cur < A.cj->n) {
        char t[160]; snprintf(t, sizeof t, "Step %d of %d: %s", A.cj->cur + 1, A.cj->n, A.cj->steps[A.cj->cur].label);
        ui_text(F_REG, 13.5f, t, mx + 40 + ui_text_w(F_SEMI, TH.lcars ? 24 : 20, ui_cap(stTxt)), my + 5, TH.text2);
    } else if (failed && failing_log() >= 0) {
        char t[200]; snprintf(t, sizeof t, "The error is in %s", A.logs.logs[failing_log()].name);
        ui_text(F_REG, 13.5f, t, mx + 40 + ui_text_w(F_SEMI, TH.lcars ? 24 : 20, ui_cap(stTxt)), my + 5, TH.err);
    } else if (pm) {
        char t[200]; snprintf(t, sizeof t, "Showing %s", pm->name);
        ui_text(F_REG, 13.5f, t, mx + 40 + ui_text_w(F_SEMI, TH.lcars ? 24 : 20, ui_cap(stTxt)), my + 5, TH.text3);
    }
    my += 44;

    /* time ruler */
    double tNow = pm ? pm->time : (c->s.latest[0] ? atof(c->s.latest) : c->s.startTime);
    Color barC = mine ? TH.accent : failed ? TH.err : stopped ? TH.warn : TH.ok;
    time_ruler((Rectangle){ mx, my, mw, 60 }, c->s.startTime, c->s.endTime, tNow, barC);
    my += 74;

    /* kpis */
    float kw = mw / 6;
    char v[64];
    snprintf(v, sizeof v, "%d", pm ? pm->steps : 0);
    kpi((Rectangle){ mx, my, kw, 50 }, "Time steps", v, TH.text);
    float co = pm && pm->courant.n ? pm->courant.v[pm->courant.n - 1] : 0;
    snprintf(v, sizeof v, pm && pm->courant.n ? "%.3g" : "-", co);
    kpi((Rectangle){ mx + kw, my, kw, 50 }, "Courant max", v, co > 1 ? TH.warn : TH.text);
    snprintf(v, sizeof v, pm && pm->deltaT > 0 ? "%.3g" : "%g", pm && pm->deltaT > 0 ? pm->deltaT : c->s.deltaT);
    kpi((Rectangle){ mx + 2 * kw, my, kw, 50 }, "Time step", v, TH.text);
    pl_duration(v, sizeof v, pm ? pm->execTime : 0);
    kpi((Rectangle){ mx + 3 * kw, my, kw, 50 }, "Solver time", pm && pm->execTime > 0 ? v : "-", TH.text);
    double eta = -1, rate = -1;
    /* wall-clock time when the log has it (ClockTime: what you wait), CPU time early on (ClockTime counts whole seconds) */
    double el = pm && pm->clockTime >= 5 ? pm->clockTime : pm ? pm->execTime : 0;
    if (pm && el > 0.5 && pm->time > pm->startTimeSeen) {
        rate = (pm->time - pm->startTimeSeen) / el;
        eta = (c->s.endTime - pm->time) / rate;
    }
    pl_duration(v, sizeof v, eta);
    kpi((Rectangle){ mx + 4 * kw, my, kw, 50 }, "Remaining", mine && eta >= 0 ? v : "-", TH.text);
    snprintf(v, sizeof v, rate > 0 ? "%.3g /s" : "-", rate);
    kpi((Rectangle){ mx + 5 * kw, my, kw, 50 }, "Sim time per second", v, TH.text);
    my += 66;
    my += flavour_run(c, pm, mine, mx, my, mw);              /* the screen and game themes' instrument */

    /* RED ALERT (3.3): the running solver is diverging, or may be */
    if (mine && A.alert.level > 0 && !A.alertDismissLevel) {
        bool red = A.alert.level == RA_ALERT;
        char adj[16] = ""; if (cd) fd_get(cd, "adjustTimeStep", adj, sizeof adj);
        bool byCo = (!strcmp(adj, "yes") || !strcmp(adj, "on") || !strcmp(adj, "true")) && cd && fd_find(cd, "maxCo");
        char hv[64] = "", hl[96];
        if (cd) fd_get_resolved(cd, byCo ? "maxCo" : "deltaT", hv, sizeof hv);   /* "deltaT $dtBase": its number (H-19) */
        snprintf(hl, sizeof hl, "Halve %s (%s -> %g)", byCo ? "maxCo" : "Δt", hv, atof(hv) / 2);
        const char *sl = app_can_pause() ? "Stop and write" : "Stop";
        float w1 = ui_button_width(ICON_PAUSE, sl), w2 = ui_button_width(ICON_GAUGE, hl), w3 = ui_button_width(NULL, "Continue");
        float bw = fmaxf(w1, fmaxf(w2, w3));
        float cw = mw - bw - 16;
        float ch = fmaxf(callout_height(cw, alert_title(&A.alert), A.alert.text), 3 * 38 + 16);
        Rectangle cr = { mx, my, cw, ch };
        callout(cr, red ? TOAST_ERR : TOAST_WARN, ICON_TRIANGLE_ALERT, alert_title(&A.alert), A.alert.text);
        if (red && TH.lcars) {                                /* a slow pulse (1 Hz, well under the 3 Hz flash limit) */
            float a = 0.5f + 0.5f * sinf((float)GetTime() * 6.2832f);
            ui_rect_lines(ui_inset(cr, -3, -3), 14, 3, ui_alpha(TH.err, a));
            ui_request_frames(2);
        }
        float bx = mx + mw - bw;
        if (ui_button(ui_id("ra-stop"), (Rectangle){ bx, my, bw, 36 }, ICON_PAUSE, sl, BTN_DANGER)) {
            if (app_can_pause()) app_pause_run(); else job_stop(A.cj, GetTime());
        }
        ui_tooltip((Rectangle){ bx, my, bw, 36 }, app_can_pause() ? "The solver writes the time step it is on and stops: you can look at it in ParaView and Resume later"
                                                                 : "Stop the run now");
        ui_disable(!cd || !(atof(hv) > 0));
        if (ui_button(ui_id("ra-halve"), (Rectangle){ bx, my + 42, bw, 36 }, ICON_GAUGE, hl, BTN_SECONDARY)) app_alert_halve();
        if (!cd || !(atof(hv) > 0)) ui_tooltip((Rectangle){ bx, my + 42, bw, 36 }, "system/controlDict has no number for it here (an #include, "
                                                "#calc or a macro this button can't follow): change it on the Run page");
        ui_disable(false);
        ui_tooltip((Rectangle){ bx, my + 42, bw, 36 }, "Saved to system/controlDict now; the running solver reads it at its next time step");
        if (ui_button(ui_id("ra-continue"), (Rectangle){ bx, my + 84, bw, 36 }, NULL, "Continue", BTN_GHOST)) {
            A.alertDismissLevel = A.alert.level;
            int prim2 = logset_primary(&A.logs, c->s.app);
            A.alertDismissSteps = prim2 >= 0 ? A.logs.logs[prim2].steps : 0;
        }
        ui_tooltip((Rectangle){ bx, my + 84, bw, 36 }, "Hide this until it gets worse");
        my += ch + 14;
    }
    /* the last run can be resumed: say why, with the button at hand */
    if (resumeOffer) {
        char body[400], title[80];
        const char *lt = case_latest_result(&c->s);
        if (paused) {
            snprintf(title, sizeof title, "Paused at t = %s", pausedAt[0] ? pausedAt : lt);
            snprintf(body, sizeof body, "The run wrote t = %s and stopped. Resume continues from there to the end time %g, "
                     "adding to %s.", lt, c->s.endTime, pm ? pm->name : "the log");
        } else {
            snprintf(title, sizeof title, "The last run didn't finish");
            snprintf(body, sizeof body, "%s stopped at t = %g, before the end time %g, without an error: it was stopped, or the computer "
                     "restarted under it. The last written time is t = %s; Resume continues from there.", pm ? pm->name : "The solver",
                     pm ? pm->time : 0.0, c->s.endTime, lt);
        }
        char lab[96]; snprintf(lab, sizeof lab, "Resume from t = %s", lt);
        float bw = ui_button_width(ICON_PLAY, lab);
        float cw = mw - bw - 16;
        float ch = fmaxf(callout_height(cw, title, body), 64);
        callout((Rectangle){ mx, my, cw, ch }, paused ? TOAST_INFO : TOAST_WARN, paused ? ICON_PAUSE : ICON_TRIANGLE_ALERT, title, body);
        ui_disable(A.cj->active);
        if (ui_button(ui_id("mon-resume"), (Rectangle){ mx + mw - bw, my + (ch - 38) / 2, bw, 38 }, ICON_PLAY, lab, BTN_PRIMARY)) app_resume_run();
        ui_disable(false);
        my += ch + 14;
    }
    /* fatal error card */
    if (pm && pm->fatal && pm->fatalMsg[0]) {
        char msg[1024];
        snprintf(msg, sizeof msg, "%s", pm->fatalMsg);
        case_relative(msg, sizeof msg, c->s.path);
        /* a missing region mesh is the classic multi-region mistake: say what to do about it */
        const char *mp = strstr(pm->fatalMsg, "/polyMesh\" in times");
        const char *dq = mp ? strstr(pm->fatalMsg, "in directory \"") : NULL;
        if (mp && dq && dq < mp) {
            char region[96]; int n = (int)(mp - (dq + 14));
            snprintf(region, sizeof region, "%.*s", n > 0 && n < 95 ? n : 0, dq + 14);
            if (region[0]) {
                size_t L = strlen(msg);
                snprintf(msg + L, sizeof msg - L, "\nThe mesh for region \"%s\" hasn't been made. Generate the mesh (it runs topoSet and "
                         "splitMeshRegions -cellZones), check that constant/regionProperties lists %s, then run again so decomposePar "
                         "uses -allRegions.", region, region);
            }
        }
        char ttl[160] = "OpenFOAM stopped with an error";
        static Explanation ex; static char exFor[600]; static bool exOk;   /* the catalogue reads the log and dictionaries: once per error */
        char key[600]; pl_fmt_ui(key, sizeof key, "%s|%s|%s", c->s.path, pm->name, pm->fatalMsg);
        if (strcmp(key, exFor)) { exOk = foam_explain(c->s.path, pm->name, pm->fatalMsg, &ex); pl_strlcpy(exFor, key, sizeof exFor); }
        if (exOk) {      /* the catalogue knows it: plain words first */
            pl_fmt_ui(ttl, sizeof ttl, "%s: %s", term(T_EXPLAIN), ex.title);
            if (ex.line > 0) snprintf(msg, sizeof msg, "%s\n%s line %d (%s).", ex.text, ex.file, ex.line, ex.key);
            else snprintf(msg, sizeof msg, "%s", ex.text);
        }
        bool fix = exOk && ex.fix && !A.cj->active;
        char fl[140]; pl_fmt_ui(fl, sizeof fl, "Fix: %s", fix ? ex.fixLabel : "");
        float aw = fmaxf(ui_button_width(ICON_SPARKLES, "Ask Claude"), fix ? fminf(ui_button_width(ICON_WRENCH, fl), 320) : 0);
        float ch = callout_height(mw - aw - 16, ttl, msg);
        if (ch > 150) ch = 150;
        if (fix && ch < 92) ch = 92;
        if (exOk && ex.file[0] && ch < (fix ? 48 : 6) + 76) ch = (fix ? 48 : 6) + 76;
        callout((Rectangle){ mx, my, mw - aw - 16, ch }, TOAST_ERR, ICON_CIRCLE_X, ttl, msg);
        if (fix) {
            if (ui_button(ui_id("err-fix"), (Rectangle){ mx + mw - aw, my + 6, aw, 36 }, ICON_WRENCH, fl, BTN_PRIMARY)) app_apply_fix(&ex);
            ui_tooltip((Rectangle){ mx + mw - aw, my + 6, aw, 36 }, "Changes the file and saves it; Ctrl+Z undoes it. Run again afterwards.");
        }
        float cy2 = my + (fix ? 48 : 6);
        if (ui_button(ui_id("err-claude"), (Rectangle){ mx + mw - aw, cy2, aw, 36 }, ICON_SPARKLES, "Ask Claude", BTN_SECONDARY)) app_claude_error(pm->name, pm->fatalMsg);
        if (exOk && ex.file[0]) {
            char ol[64]; pl_fmt_ui(ol, sizeof ol, ex.line > 0 ? "Open at line %d" : "Open the file", ex.line);
            if (ui_button(ui_id("err-open"), (Rectangle){ mx + mw - aw, cy2 + 42, aw, 34 }, ICON_FILE_TEXT, ol, BTN_GHOST)) {
                pl_strlcpy(A.filesSel, ex.file, sizeof A.filesSel); A.filesRaw = true; text_editor_goto(ex.line); app_go(PG_FILES);
            }
        }
        ui_tooltip((Rectangle){ mx + mw - aw, cy2, aw, 36 }, "Claude Code opens in a terminal with this error, the log and what changed since the last good run");
        my += ch + 14;
    } else {
        int sel = run_log_sel(prim, mine);
        LogMon *lm = sel >= 0 ? &A.logs.logs[sel] : NULL;
        bool failedHere = !mine && A.cj->failed && !strcmp(A.cj->casePath, c->s.path);
        if (lm && ((lm->fatal && lm->fatalMsg[0]) || (failedHere && lm->warnMsg[0] && sel == failing_log()))) {
            char t[160]; snprintf(t, sizeof t, "%s stopped with an error", lm->name);
            char msg[1024]; snprintf(msg, sizeof msg, "%s", log_error(lm));
            case_relative(msg, sizeof msg, c->s.path);
            static Explanation ex; static char exFor[600]; static bool exOk;
            char key[600]; pl_fmt_ui(key, sizeof key, "%s|%s|%s", c->s.path, lm->name, log_error(lm));
            if (strcmp(key, exFor)) { exOk = foam_explain(c->s.path, lm->name, log_error(lm), &ex); pl_strlcpy(exFor, key, sizeof exFor); }
            if (exOk) {
                pl_fmt_ui(t, sizeof t, "%s: %s", lm->name, ex.title);
                if (ex.line > 0) snprintf(msg, sizeof msg, "%s\n%s line %d (%s).", ex.text, ex.file, ex.line, ex.key);
                else snprintf(msg, sizeof msg, "%s", ex.text);
            }
            bool fix = exOk && ex.fix && !A.cj->active;
            char fl[140]; pl_fmt_ui(fl, sizeof fl, "Fix: %s", fix ? ex.fixLabel : "");
            float aw = fmaxf(ui_button_width(ICON_SPARKLES, "Ask Claude"), fix ? fminf(ui_button_width(ICON_WRENCH, fl), 320) : 0);
            float ch = fminf(callout_height(mw - aw - 16, t, msg), 120);
            if (fix && ch < 92) ch = 92;
            if (exOk && ex.file[0] && ch < (fix ? 90 : 48) + 36) ch = (fix ? 90 : 48) + 36;
            callout((Rectangle){ mx, my, mw - aw - 16, ch }, TOAST_ERR, ICON_CIRCLE_X, t, msg);
            if (fix && ui_button(ui_id("err-fix2"), (Rectangle){ mx + mw - aw, my + 6, aw, 36 }, ICON_WRENCH, fl, BTN_PRIMARY)) app_apply_fix(&ex);
            if (ui_button(ui_id("err-claude2"), (Rectangle){ mx + mw - aw, my + (fix ? 48 : 6), aw, 36 }, ICON_SPARKLES, "Ask Claude", BTN_SECONDARY)) app_claude_error(lm->name, log_error(lm));
            if (exOk && ex.file[0]) {
                char ol[64]; pl_fmt_ui(ol, sizeof ol, ex.line > 0 ? "Open at line %d" : "Open the file", ex.line);
                float oy = my + (fix ? 90 : 48);
                if (ui_button(ui_id("err-open"), (Rectangle){ mx + mw - aw, oy, aw, 34 }, ICON_FILE_TEXT, ol, BTN_GHOST)) {
                    pl_strlcpy(A.filesSel, ex.file, sizeof A.filesSel); A.filesRaw = true; text_editor_goto(ex.line); app_go(PG_FILES);
                }
            }
            my += ch + 14;
        }
    }

    /* chart + console share the remaining height */
    /* chart + console share the remaining height. The gap between them drags; dragging the chart below its
       minimum folds it away so the log gets everything; double-click goes back to the default split. */
    float rest = mon.y + mon.height - my - 24 - 16;
    float chMin = fminf(240, rest * 0.6f);
    #define CHART_H(f) ((f) < 0.06f ? 0 : fmaxf(chMin, fminf(rest * (f), rest - 130)))
    float frac = A.set.runSplit > 0 ? A.set.runSplit : 0.56f;
    float chH = CHART_H(frac);
    static bool dragging;
    static float grabOff;
    Rectangle grip = { mx, my + chH, mw, 16 };
    bool hov = ui_hovered(grip);
    if (hov || dragging || chH == 0) {
        if (hov || dragging) ui_cursor(MOUSE_CURSOR_RESIZE_NS);
        DrawRectangleRounded((Rectangle){ mx + mw / 2 - 24, grip.y + 6, 48, 4 }, 1, 4, dragging ? TH.accent : TH.text3);
    }
    if (chH == 0 && !dragging) ui_text(F_REG, 12, "Residuals hidden: drag down to show", mx + 4, grip.y + 1, TH.text3);
    if (hov && ui_double_clicked()) { A.set.runSplit = 0; settings_save(&A.set); }
    else if (hov && ui_pressed(grip)) { dragging = true; grabOff = ui_mouse().y - grip.y; }
    if (dragging) {
        if (ui_mouse_down()) {
            float h = ui_mouse().y - grabOff - my;
            /* below half the minimum the chart folds away */
            A.set.runSplit = h < chMin * 0.5f ? 0.05f : rest > 0 ? fmaxf(0.06f, fminf(0.95f, h / rest)) : 0.56f;
            if (A.set.runSplit >= 0.06f && rest * A.set.runSplit < chMin) A.set.runSplit = chMin / rest;
            chH = CHART_H(A.set.runSplit);
            ui_request_frames(2);
        } else { dragging = false; settings_save(&A.set); }
    }
    #undef CHART_H
    if (chH > 0) chart_area((Rectangle){ mx, my, mw, chH }, c, pm);
    my += chH + 16;
    run_console((Rectangle){ mx, my, mw, mon.y + mon.height - my - 24 }, &A.logs, run_log_sel(prim, mine));
}

/* AWAY MISSION (5.1): the run as a shell script for a Slurm batch job: the same steps the app runs here, the solver
   started by srun. Each step writes its own log and a failing one ends the job. Returns the step count. */
int run_steps_script(Case *c, int np, Str *out)
{
    static Job tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.pid = -1;
    bool cont = A.continueRun; A.continueRun = false;
    int mode = A.runMode;
    build_steps(c, &tmp, np, true);
    A.continueRun = cont; A.runMode = mode;
    str_cat(out, SH_RUN);
    for (int i = 0; i < tmp.n; i++) {
        const JobStep *st = &tmp.steps[i];
        str_catf(out, "\necho \"== $(date +%%H:%%M:%%S) %s\"\n", st->label);
        if (st->log[0]) str_catf(out, "( %s\n) > %s 2>&1 || { echo \"%s failed: see %s\"; exit 1; }\n", st->cmd, st->log, st->label, st->log);
        else str_catf(out, "( %s\n) || exit 1\n", st->cmd);
    }
    return tmp.n;
}
