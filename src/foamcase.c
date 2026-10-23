/* foamcase.c -- case summaries, opened-case model, tutorial index */
#define _GNU_SOURCE
#include "foamcase.h"
#include "program.h"
#include "safety.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <limits.h>
#include <signal.h>

/* ============================================================================ categories */
static const char *INCOMP[] = { "icoFoam", "simpleFoam", "pimpleFoam", "pisoFoam", "SRFSimpleFoam", "SRFPimpleFoam",
    "porousSimpleFoam", "nonNewtonianIcoFoam", "potentialFoam", "boundaryFoam", "adjointOptimisationFoam",
    "adjointShapeOptimizationFoam", "shallowWaterFoam", "overPimpleDyMFoam", "overSimpleFoam", "incompressibleFluid",
    "lumpedPointMotion", NULL };
static const char *HEAT[] = { "buoyantSimpleFoam", "buoyantPimpleFoam", "buoyantBoussinesqSimpleFoam",
    "buoyantBoussinesqPimpleFoam", "chtMultiRegionFoam", "chtMultiRegionSimpleFoam", "chtMultiRegionTwoPhaseEulerFoam",
    "laplacianFoam", "overLaplacianDyMFoam", "solidFoam", "thermoFoam", "solidDisplacementFoam", "fluid", "solid",
    "multiRegion", NULL };
static const char *MULTI[] = { "interFoam", "interIsoFoam", "multiphaseInterFoam", "multiphaseEulerFoam",
    "twoPhaseEulerFoam", "reactingTwoPhaseEulerFoam", "reactingMultiphaseEulerFoam", "driftFluxFoam",
    "compressibleInterFoam", "compressibleMultiphaseInterFoam", "interPhaseChangeFoam", "cavitatingFoam",
    "potentialFreeSurfaceFoam", "icoReactingMultiphaseInterFoam", "overInterDyMFoam", "interCondensatingEvaporatingFoam",
    "incompressibleVoF", "compressibleVoF", "multiphaseEuler", "incompressibleDriftFlux", NULL };
static const char *COMP[] = { "rhoSimpleFoam", "rhoPimpleFoam", "rhoCentralFoam", "sonicFoam", "rhoPorousSimpleFoam",
    "reactingFoam", "fireFoam", "XiFoam", "chemFoam", "coalChemistryFoam", "sprayFoam", "dsmcFoam",
    "compressibleFluid", "multicomponentFluid", NULL };

static bool in_list(const char *s, const char **l) { for (; *l; l++) if (!strcmp(s, *l)) return true; return false; }

Category category_of(const char *app, const char *rel)
{
    if (app && *app) {
        if (in_list(app, INCOMP)) return CAT_INCOMP;
        if (in_list(app, HEAT)) return CAT_HEAT;
        if (in_list(app, MULTI)) return CAT_MULTI;
        if (in_list(app, COMP)) return CAT_COMP;
        if (strstr(app, "buoyant") || strstr(app, "cht")) return CAT_HEAT;
        if (strstr(app, "inter") || strstr(app, "Euler") || strstr(app, "VoF")) return CAT_MULTI;
    }
    if (rel) {
        if (pl_starts_with(rel, "incompressible")) return CAT_INCOMP;
        if (pl_starts_with(rel, "heatTransfer") || pl_starts_with(rel, "basic/laplacianFoam")) return CAT_HEAT;
        if (pl_starts_with(rel, "multiphase")) return CAT_MULTI;
        if (pl_starts_with(rel, "compressible") || pl_starts_with(rel, "combustion")) return CAT_COMP;
    }
    return CAT_OTHER;
}
const char *category_name(Category c)
{
    static const char *n[] = { "Incompressible", "Heat transfer", "Multiphase", "Compressible", "Other" };
    return (c >= 0 && c < CAT_COUNT) ? n[c] : "Other";
}
const char *status_name(CaseStatus s)
{
    static const char *n[] = { "Not meshed", "Ready", "Running", "Finished", "Failed", "Stopped", "Queued", "Paused" };
    return (s >= CS_NEW && s <= CS_PAUSED) ? n[s] : "?";
}

/* ============================================================================ summary */
bool case_is_project(const char *dir) { return prog_detect(dir) != PROG_NONE; }

/* another program's project: what the Cases page and the pages need, from its inputs and its run logs */
/* a coupled run's state from its participants (folders with precice-config.xml beside them): the newest log among
   them; running when one is being written; how many participants it has */
static void coupled_status(CaseSummary *s)
{
    char cfg[PL_PATH]; pl_join(cfg, sizeof cfg, s->path, "precice-config.xml");
    char *t = pl_read_file(cfg, NULL);
    int parts = 0;
    for (const char *q = t; q && (q = strstr(q, "<participant ")); q++) parts++;
    free(t);
    s->nInputs = parts;
    pl_fmt(s->input, sizeof s->input, "%d participant%s", parts, parts == 1 ? "" : "s");
    NameList d = { 0 };
    pl_list_dir(s->path, &d, true, false);
    time_t newest = 0;
    for (int i = 0; i < d.n; i++) {
        char sub[PL_PATH]; pl_join(sub, sizeof sub, s->path, d.names[i]);
        NameList f = { 0 };
        pl_list_dir(sub, &f, false, true);
        for (int k = 0; k < f.n; k++) {
            if (!pl_starts_with(f.names[k], "log.")) continue;
            char p[PL_PATH]; pl_join(p, sizeof p, sub, f.names[k]);
            time_t m = pl_mtime(p);
            if (m > newest) { newest = m; pl_fmt(s->lastLog, sizeof s->lastLog, "%s/%s", d.names[i], f.names[k]); }
        }
        names_free(&f);
    }
    names_free(&d);
    if (newest > s->activity) s->activity = newest;
    if (newest && time(NULL) - newest < 30) s->status = CS_RUNNING;
    else if (newest) s->status = CS_DONE;
}

static void program_summarize(CaseSummary *s, int prog)
{
    s->prog = prog;
    pl_strlcpy(s->app, prog_info(prog)->name, sizeof s->app);
    /* "dem", "moose", "A": a folder named for its role says little alone; with the folder above it does */
    {
        const char *b = pl_basename(s->path);
        static const char *const GENERIC[] = { "dem", "deme", "DEM", "moose", "chrono", "aster", "calculix", "ccx", "saturne", "elmer",
                                               "sparta", "case", "run", "solid", "fluid", NULL };
        bool gen = strlen(b) <= 2;
        for (int i = 0; GENERIC[i] && !gen; i++) if (!strcasecmp(b, GENERIC[i])) gen = true;
        if (gen) {
            char par[PL_PATH]; pl_dirname(par, sizeof par, s->path);
            pl_fmt_ui(s->name, sizeof s->name, "%s/%s", pl_basename(par), b);
        }
    }
    s->cat = CAT_OTHER;
    s->meshed = true;
    s->readOnly = access(s->path, W_OK) != 0;
    NameList in = { 0 };
    s->nInputs = prog_inputs(prog, s->path, &in);
    if (in.n) pl_strlcpy(s->input, in.names[0], sizeof s->input);
    s->activity = pl_mtime(s->path);
    for (int i = 0; i < in.n; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, s->path, in.names[i]);
        time_t m = pl_mtime(p); if (m > s->activity) s->activity = m;
    }
    names_free(&in);
    /* the newest log decides the status: its exit line, or being written right now */
    NameList all = { 0 };
    pl_list_dir(s->path, &all, false, true);
    time_t newest = 0; char nl[64] = "";
    for (int i = 0; i < all.n; i++) {
        if (!pl_starts_with(all.names[i], "log.") || pl_ends_with(all.names[i], "~")) continue;
        char p[PL_PATH]; pl_join(p, sizeof p, s->path, all.names[i]);
        time_t m = pl_mtime(p);
        /* a build log counts only when it is the newest and it failed (or is going): a good build is not a run */
        if (!strcmp(all.names[i], "log.build")) {
            if (m < newest) continue;
            char *bt = pl_read_file(p, NULL);
            const char *ex = bt ? strstr(bt, "Q-Continuum: exit ") : NULL;
            bool failed = ex && atoi(ex + 18) != 0, going = !ex && time(NULL) - m < 30;
            free(bt);
            if (!failed && !going) { if (m > s->activity) s->activity = m; continue; }
        }
        if (m >= newest) { newest = m; pl_strlcpy(nl, all.names[i], sizeof nl); }
    }
    names_free(&all);
    s->status = CS_NEW;
    if (prog == PROG_COUPLED) coupled_status(s);              /* the participants' logs, not the folder's own */
    if (nl[0] && prog != PROG_COUPLED) {
        pl_strlcpy(s->lastLog, nl, sizeof s->lastLog);
        pl_strlcpy(s->lastRun, nl, sizeof s->lastRun);
        if (newest > s->activity) s->activity = newest;
        char p[PL_PATH]; pl_join(p, sizeof p, s->path, nl);
        long long sz = pl_file_size(p);
        char *t = NULL;
        FILE *f = fopen(p, "rb");
        if (f) {
            long off = sz > 4096 ? (long)sz - 4096 : 0;
            if (fseek(f, off, SEEK_SET) == 0) { t = calloc(1, 4097); if (t) { size_t r = fread(t, 1, 4096, f); t[r] = 0; } }
            fclose(f);
        }
        const char *ex = t ? strstr(t, "Q-Continuum: exit ") : NULL;
        if (ex) {
            int rc = atoi(ex + 18);
            s->status = rc == 0 ? CS_DONE : (rc == 143 || rc == 130) ? CS_STOPPED : CS_FAILED;
        } else if (time(NULL) - newest < 30) { s->status = CS_RUNNING; pl_strlcpy(s->runningLog, nl, sizeof s->runningLog); }
        else {                                             /* a run from outside the app: what its own last lines say */
            LogMon *m = calloc(1, sizeof *m);
            if (m && t) {
                m->prog = prog; m->exitCode = -1;
                logmon_feed(m, t, (int)strlen(t));
                s->status = m->fatal ? CS_FAILED : m->ended ? CS_DONE : CS_STOPPED;
                logmon_reset(m);
            }
            free(m);
        }
        free(t);
    }
    ProgResult r[64];
    s->nTimes = prog_results(prog, s->path, r, 64);
    if (prog == PROG_COUPLED) {                               /* results: the time folders its participants wrote */
        s->nTimes = 0;
        NameList d = { 0 };
        pl_list_dir(s->path, &d, true, false);
        for (int i = 0; i < d.n; i++) {
            char sub[PL_PATH]; pl_join(sub, sizeof sub, s->path, d.names[i]);
            NameList t = { 0 };
            pl_list_dir(sub, &t, true, false);
            for (int k = 0; k < t.n; k++) if (pl_is_number(t.names[k]) && atof(t.names[k]) > 0) s->nTimes++;
            names_free(&t);
        }
        names_free(&d);
    }
}

bool case_is_case(const char *dir)
{
    char p[PL_PATH];
    pl_join(p, sizeof p, dir, "system/controlDict");
    return pl_is_file(p);
}

static long note_value(const char *head, const char *key)
{
    const char *p = strstr(head, key);
    return p ? strtol(p + strlen(key), NULL, 10) : -1;
}

static void read_mesh_note(const char *casePath, CaseSummary *s)
{
    char p[PL_PATH];
    const char *cands[] = { "constant/polyMesh/owner", "processor0/constant/polyMesh/owner", NULL };
    for (int i = 0; cands[i]; i++) {
        pl_join(p, sizeof p, casePath, cands[i]);
        if (pl_is_file(p)) {
            char *h = pl_read_head(p, 2048);
            s->meshed = true;
            if (i == 0) s->meshTime = pl_mtime(p);
            if (h) {
                s->nCells = note_value(h, "nCells:");
                s->nPoints = note_value(h, "nPoints:");
                s->nFaces = note_value(h, "nFaces:");
                s->nInternal = note_value(h, "nInternalFaces:");
                if (i == 1) s->nCells = -1;                 /* per-processor counts only */
            }
            free(h);
            return;
        }
        char gz[PL_PATH + 4]; snprintf(gz, sizeof gz, "%s.gz", p);
        if (pl_is_file(gz)) { s->meshed = true; return; }
    }
}

/* regionProperties: regions ( fluid (bottomWater) solid (heater leftSolid) ); */
static void read_regions(const char *casePath, CaseSummary *s)
{
    const char *cands[] = { "constant/regionProperties", "system/regionProperties", NULL };
    for (int i = 0; cands[i]; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, casePath, cands[i]);
        FDFile *f = fd_load(p);
        if (!f) continue;
        char v[2048] = "";
        if (fd_get(f, "regions", v, sizeof v)) {
            const char *q = v;
            while (*q && s->nRegions < 16) {
                while (*q && !(isalnum((unsigned char)*q) || *q == '_')) q++;
                char w[64]; int k = 0;
                while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '.' || *q == '-') && k < 63) w[k++] = *q++;
                w[k] = 0;
                if (!k) break;
                if (!strcmp(w, "fluid") || !strcmp(w, "solid")) continue;     /* region types, not names */
                pl_strlcpy(s->regions[s->nRegions++], w, 64);
            }
        }
        fd_free(f);
        break;
    }
    if (s->nRegions == 0) return;
    /* the mesh counts live in each region's polyMesh */
    long cells = 0; bool all = true, any = false;
    for (int r = 0; r < s->nRegions; r++) {
        char rel[160], p[PL_PATH];
        snprintf(rel, sizeof rel, "constant/%s/polyMesh/owner", s->regions[r]);
        pl_join(p, sizeof p, casePath, rel);
        char gz[PL_PATH + 4]; snprintf(gz, sizeof gz, "%s.gz", p);
        if (!pl_is_file(p) && !pl_is_file(gz)) { all = false; continue; }
        any = true;
        char *h = pl_read_head(p, 2048);
        long n = h ? note_value(h, "nCells:") : -1;
        free(h);
        if (n > 0) cells += n;
    }
    s->regionMeshed = all && any;
    if (s->regionMeshed && cells > 0) { s->nCells = cells; s->nFaces = s->nPoints = s->nInternal = -1; }
}

/* A mesh older than the dictionary that makes it no longer matches it: the solver would run on the
   old mesh. (An unedited blockMeshDict next to a 200x200 polyMesh from an earlier edit diverged the
   cavity at Co ~ 10.) */
static void check_mesh_stale(CaseSummary *s)
{
    if (!s->meshTime) return;
    /* a mesh imported after the last blockMesh/snappy run (SALOME tab) doesn't come from those dictionaries */
    const char *logs[] = { "log.ideasUnvToFoam", "log.blockMesh", "log.snappyHexMesh" };
    time_t lt[3];
    for (int i = 0; i < 3; i++) { char p[PL_PATH]; pl_join(p, sizeof p, s->path, logs[i]); lt[i] = pl_is_file(p) ? pl_mtime(p) : 0; }
    if (lt[0] && lt[0] > lt[1] && lt[0] > lt[2]) return;
    const char *dicts[] = { "system/blockMeshDict", "system/snappyHexMeshDict", NULL };
    for (int i = 0; dicts[i]; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, s->path, dicts[i]);
        time_t m = pl_is_file(p) ? pl_mtime(p) : 0;
        if (m > s->meshTime) { s->meshStale = true; pl_strlcpy(s->meshStaleDict, dicts[i], sizeof s->meshStaleDict); return; }
    }
    if (!s->regionMeshed) return;
    char p[PL_PATH]; pl_join(p, sizeof p, s->path, "system/topoSetDict");
    time_t newest = s->meshTime, ts = pl_is_file(p) ? pl_mtime(p) : 0;
    if (ts > newest) newest = ts;
    for (int r = 0; r < s->nRegions; r++) {
        char rel[160]; snprintf(rel, sizeof rel, "constant/%s/polyMesh/owner", s->regions[r]);
        pl_join(p, sizeof p, s->path, rel);
        time_t m = pl_mtime(p);
        if (m && m < newest) { s->regionStale = true; return; }
    }
}

static CaseStatus log_status(const char *logPath, time_t mtime)
{
    long long sz = pl_file_size(logPath);
    if (sz <= 0) return CS_STOPPED;
    FILE *f = fopen(logPath, "rb");
    if (!f) return CS_STOPPED;
    long take = sz > 6000 ? 6000 : (long)sz;
    fseek(f, -take, SEEK_END);
    char buf[6001];
    size_t r = fread(buf, 1, (size_t)take, f);
    fclose(f);
    buf[r] = 0;
    if (strstr(buf, "FOAM FATAL") || strstr(buf, "FOAM aborting") || strstr(buf, "Q-Continuum: could not") ||
        strstr(buf, "Foam::sigFpe::sigHandler") || strstr(buf, "Foam::sigSegv::sigHandler")) return CS_FAILED;
    /* last non-empty line */
    int e = (int)r;
    while (e > 0 && isspace((unsigned char)buf[e - 1])) e--;
    int s = e;
    while (s > 0 && buf[s - 1] != '\n') s--;
    char last[256]; int n = e - s < 255 ? e - s : 255;
    memcpy(last, buf + s, (size_t)n); last[n] = 0;
    pl_trim_inplace(last);
    if (!strcmp(last, "End") || strstr(last, "Finalising parallel run")) return CS_DONE;
    if (difftime(time(NULL), mtime) < 30) return CS_RUNNING;
    return CS_STOPPED;
}

/* OpenFOAM's fileName rejects quotes, spaces and a few shell characters anywhere in the case path
   (decomposePar: "fileName::stripInvalid() called for invalid fileName"), so such a case can't run */
const char *foam_path_problem(const char *path)
{
    for (const char *p = path; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == ' ' || ch == '\t') return "a space";
        if (ch == '\'' || ch == '"') return "a quote";
        if (ch == '$' || ch == '*' || ch == '?' || ch == ';' || ch == '`' || ch == '\\') return "a shell character ($ * ? ; ` \\)";
    }
    return NULL;
}

/* is the program behind a log ("log.simpleFoam" -> simpleFoam, mpirun for parallel runs, a shell running
   Allrun) running with its working directory in this case? Recent logs alone also match a run that was
   just stopped or crashed. */
static bool log_writer_alive(const char *casePath, const char *logName)
{
    char real[PATH_MAX];
    if (!realpath(casePath, real)) pl_strlcpy(real, casePath, sizeof real);
    size_t rl = strlen(real);
    const char *app = logName + 4;                           /* after "log." */
    char app15[16]; snprintf(app15, sizeof app15, "%.15s", app);   /* /proc/<pid>/comm is cut at 15 */
    bool allrun = pl_starts_with(app, "Allrun");
    DIR *d = opendir("/proc");
    if (!d) return true;                                      /* can't tell: keep the old caution */
    bool alive = false;
    pid_t me = getpid();
    struct dirent *e;
    while (!alive && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) == me) continue;
        char p[300], cwd[PATH_MAX];
        snprintf(p, sizeof p, "/proc/%s/cwd", e->d_name);
        ssize_t n = readlink(p, cwd, sizeof cwd - 1);
        if (n <= 0) continue;
        cwd[n] = 0;
        if (strncmp(cwd, real, rl) || (cwd[rl] && cwd[rl] != '/')) continue;
        char comm[64] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        if (!strcmp(comm, app15) || !strcmp(comm, "mpirun") || !strcmp(comm, "orterun") || !strcmp(comm, "prterun") ||
            !strcmp(comm, "mpiexec") || !strcmp(comm, "mpiexec.hydra")) alive = true;
        else if (allrun) {
            char cmd[512] = ""; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
            f = fopen(p, "r");
            if (f) { size_t k = fread(cmd, 1, sizeof cmd - 1, f); fclose(f); for (size_t j = 0; j < k; j++) if (!cmd[j]) cmd[j] = ' '; cmd[k] = 0; }
            if (strstr(cmd, "Allrun")) alive = true;
        }
    }
    closedir(d);
    return alive;
}

void case_summarize(const char *path, CaseSummary *s)
{
    char src[PL_PATH];
    pl_strlcpy(src, path, sizeof src);                   /* path may point into *s (rescan) */
    memset(s, 0, sizeof *s);
    pl_expand_home(s->path, sizeof s->path, src);
    pl_strlcpy(s->name, pl_basename(s->path), sizeof s->name);
    s->nCells = s->nPoints = s->nFaces = s->nInternal = -1;
    if (!case_is_case(s->path)) {
        int prog = prog_detect(s->path);
        if (prog > PROG_OPENFOAM) { program_summarize(s, prog); return; }
    }

    char p[PL_PATH];
    pl_join(p, sizeof p, s->path, "system/controlDict");
    s->activity = pl_mtime(p);
    FDFile *cd = fd_load(p);
    if (cd) {
        char v[128];
        if (fd_get(cd, "application", v, sizeof v)) fd_word(v, s->app, sizeof s->app);
        if (fd_get(cd, "startTime", v, sizeof v)) s->startTime = atof(v);
        if (fd_get(cd, "endTime", v, sizeof v)) s->endTime = atof(v);
        if (fd_get(cd, "deltaT", v, sizeof v)) s->deltaT = atof(v);
        fd_free(cd);
    }
    s->cat = category_of(s->app, NULL);
    read_mesh_note(s->path, s);
    read_regions(s->path, s);
    check_mesh_stale(s);
    if (s->nRegions > 0 && !s->regionMeshed) s->meshed = false;   /* base mesh alone can't run */

    /* tutorials such as obliqueAirJet/main or snappyMultiRegionHeater have Allrun-parallel / Allrun-serial instead */
    s->allrun[0] = 0;
    static const char *const runNames[] = { "Allrun", "Allrun-parallel", "Allrun-serial", NULL };
    for (int i = 0; runNames[i] && !s->allrun[0]; i++) { pl_join(p, sizeof p, s->path, runNames[i]); if (pl_is_file(p)) pl_strlcpy(s->allrun, runNames[i], sizeof s->allrun); }
    s->hasAllrun = s->allrun[0] != 0;
    /* overset tutorials (cylinder/{cylinderMesh,cylinderAndBackground}), obliqueAirJet/{precursor,main}: the group's
       Allrun prepares the siblings this case reads, so the case can't be run on its own */
    s->groupAllrun = false;
    {
        char par[PL_PATH], pa[PL_PATH], pc[PL_PATH]; pl_dirname(par, sizeof par, s->path);
        pl_join(pc, sizeof pc, par, "system/controlDict");
        pl_join(pa, sizeof pa, par, "Allrun");
        if (!pl_is_file(pc) && access(pa, X_OK) == 0) {
            const char *me = pl_basename(s->path);
            const char *gn[] = { "Allrun", "Allrun.pre" };
            for (int i = 0; i < 2 && !s->groupAllrun; i++) {
                char g[PL_PATH]; pl_join(g, sizeof g, par, gn[i]);
                size_t len; char *t = pl_read_file(g, &len);
                for (const char *q = t; q && (q = strstr(q, me)); q += strlen(me)) {
                    char b = q > t ? q[-1] : ' ', a = q[strlen(me)];
                    if (!isalnum((unsigned char)b) && b != '_' && !isalnum((unsigned char)a) && a != '_') { s->groupAllrun = true; break; }
                }
                free(t);
            }
            if (s->groupAllrun) s->hasAllrun = true;
        }
    }
    pl_join(p, sizeof p, s->path, "0"); s->hasZero = pl_is_dir(p);
    pl_join(p, sizeof p, s->path, "0.orig"); s->hasZeroOrig = pl_is_dir(p);
    pl_join(p, sizeof p, s->path, "processor0"); s->hasProcessors = pl_is_dir(p);
    s->readOnly = access(s->path, W_OK) != 0;

    NameList all = { 0 };
    pl_list_dir(s->path, &all, false, false);
    double latest = -1e300;
    time_t newestLog = 0;
    char newestLogName[64] = "";
    for (int i = 0; i < all.n; i++) {
        const char *n = all.names[i];
        char full[PL_PATH]; pl_join(full, sizeof full, s->path, n);
        if (pl_is_number(n) && pl_is_dir(full)) {
            double t = atof(n);
            if (t > s->startTime + 1e-12) {
                s->nTimes++;
                if (t > latest) { latest = t; pl_strlcpy(s->latest, n, sizeof s->latest); }
                time_t m = pl_mtime(full);
                if (m > s->activity) s->activity = m;
            }
        } else if (pl_starts_with(n, "log.")) {
            time_t m = pl_mtime(full);
            if (m > newestLog) { newestLog = m; pl_strlcpy(newestLogName, n, sizeof newestLogName); }
        }
    }
    names_free(&all);
    if (newestLog > s->activity) s->activity = newestLog;
    /* a solver log (any log.* but the mesh/pre tools) written in the last 30 s: someone is running this case */
    if (newestLog && difftime(time(NULL), newestLog) < 30) {
        static const char *pre[] = { "log.blockMesh", "log.checkMesh", "log.decomposePar", "log.reconstructPar", "log.topoSet",
                                      "log.splitMeshRegions", "log.snappyHexMesh", "log.surfaceFeatureExtract", "log.setFields", NULL };
        bool isPre = false;
        for (int i = 0; pre[i]; i++) if (pl_starts_with(newestLogName, pre[i])) isPre = true;
        if (!isPre && log_writer_alive(s->path, newestLogName)) pl_strlcpy(s->runningLog, newestLogName, sizeof s->runningLog);
    }

    /* decomposed results: processor0/<time> */
    for (;;) {
        char pd[PL_PATH], nm[32];
        snprintf(nm, sizeof nm, "processor%d", s->nProcDirs);
        pl_join(pd, sizeof pd, s->path, nm);
        if (!pl_is_dir(pd) || s->nProcDirs > 100000) break;
        s->nProcDirs++;
    }
    if (s->nProcDirs) {
        char pd[PL_PATH]; pl_join(pd, sizeof pd, s->path, "processor0");
        NameList pt = { 0 };
        pl_list_dir(pd, &pt, true, false);
        double best = -1e300;
        for (int i = 0; i < pt.n; i++) {
            if (!pl_is_number(pt.names[i])) continue;
            double t = atof(pt.names[i]);
            if (t <= s->startTime + 1e-12) continue;
            s->procTimes++;
            if (t > best) { best = t; pl_strlcpy(s->procLatest, pt.names[i], sizeof s->procLatest); }
        }
        names_free(&pt);
    }
    pl_strlcpy(s->lastLog, newestLogName, sizeof s->lastLog);

    /* status: the solver log decides; otherwise mesh presence */
    char solverLog[PL_PATH];
    char ln[96]; snprintf(ln, sizeof ln, "log.%s", s->app);
    pl_join(solverLog, sizeof solverLog, s->path, ln);
    if (s->app[0] && pl_is_file(solverLog)) s->status = log_status(solverLog, pl_mtime(solverLog));
    else if (s->meshed) s->status = CS_MESHED;
    else s->status = CS_NEW;
    pl_join(p, sizeof p, s->path, PAUSE_MARK);
    if ((s->status == CS_DONE || s->status == CS_STOPPED) && pl_is_file(p) && case_has_results(s)) s->status = CS_PAUSED;
}

/* ============================================================================ opened case */
bool case_has_results(const CaseSummary *s) { return s->nTimes > 0 || s->procTimes > 0; }

const char *case_latest_result(const CaseSummary *s)
{
    if (s->procTimes && (!s->nTimes || atof(s->procLatest) > atof(s->latest))) return s->procLatest;
    return s->nTimes ? s->latest : "";
}

static void add_patch(Case *c, const char *name, const char *type, int nFaces, bool pattern)
{
    for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].name, name)) return;
    if (c->nPatches >= MAX_PATCHES) return;
    PatchInfo *p = &c->patches[c->nPatches++];
    pl_strlcpy(p->name, name, sizeof p->name);
    pl_strlcpy(p->type, type ? type : "", sizeof p->type);
    p->nFaces = nFaces;
    p->pattern = pattern;
    p->groups[0] = 0;
}

/* "List<word> 2(wall heater)" or "(wall)": the words, space-separated */
static void read_groups(FDFile *f, FDEntry *e, char *out, int cap)
{
    out[0] = 0;
    FDEntry *g = e ? fd_child(e, "inGroups") : NULL;
    char v[200];
    if (!g || !fd_entry_value(f, g, v, sizeof v)) return;
    char *o = strchr(v, '('), *c = o ? strchr(o, ')') : NULL;
    if (!o || !c) return;
    *c = 0;
    int k = 0;
    for (char *t = strtok(o + 1, " \t\n"); t && k < cap - 2; t = strtok(NULL, " \t\n"))
        k += snprintf(out + k, (size_t)(cap - k), "%s%s", k ? " " : "", t);
}

/* a blockMeshDict patch whose every face repeats a vertex index ((0 1 1 0), the axis of a wedge): blockMesh writes it
   with no faces and drops it, so fields need no entry for it */
static bool faces_collapsed(FDFile *bm, FDEntry *patch)
{
    FDEntry *fe = fd_child(patch, "faces");
    if (!fe) return false;
    char v[4096];
    if (!fd_entry_value(bm, fe, v, sizeof v)) return false;
    const char *p = strchr(v, '(');
    if (!p) return false;
    int faces = 0;
    for (p++; (p = strchr(p, '(')); ) {
        long id[8]; int n = 0;
        const char *q = p + 1;
        while (*q && *q != ')' && n < 8) { char *e; long x = strtol(q, &e, 10); if (e == q) return false; id[n++] = x; q = e; while (*q == ' ' || *q == '\t' || *q == '\n') q++; }
        if (*q != ')') return false;
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) for (int j = i + 1; j < n; j++) if (id[i] == id[j]) dup = true;
        if (!dup) return false;
        faces++;
        p = q + 1;
    }
    return faces > 0;
}

const char *case_rpath(const Case *c, const char *rel)
{
    static char buf[16][PL_PATH];                           /* rotating: callers keep a few results at once */
    static int k;
    if (!c->region[0]) return rel;
    const char *sl = strchr(rel, '/');
    if (!sl || (strncmp(rel, "constant/", 9) && strncmp(rel, "system/", 7))) return rel;
    char *o = buf[k++ & 15];
    snprintf(o, PL_PATH, "%.*s/%s%s", (int)(sl - rel), rel, c->region, sl);
    return o;
}

static void scan_patches(Case *c)
{
    c->nPatches = 0;
    c->patchSource = 0;
    char p[PL_PATH];
    pl_join(p, sizeof p, c->s.path, case_rpath(c, "constant/polyMesh/boundary"));
    if (!pl_is_file(p)) {
        char rel[PL_PATH]; snprintf(rel, sizeof rel, "processor0/%s", case_rpath(c, "constant/polyMesh/boundary"));
        pl_join(p, sizeof p, c->s.path, rel);
    }
    FDFile *b = pl_is_file(p) ? fd_load(p) : NULL;
    if (b) {
        for (int i = 0; i < b->root.nChild; i++) {
            FDEntry *e = &b->root.child[i];
            if (e->kind != FD_DICT || !strcmp(e->key, "FoamFile")) continue;
            char ty[64] = "", nf[32] = "";
            FDEntry *t = fd_child(e, "type"), *n = fd_child(e, "nFaces");
            if (t) fd_entry_value(b, t, ty, sizeof ty);
            if (n) fd_entry_value(b, n, nf, sizeof nf);
            if (strstr(e->key, "procBoundary")) continue;
            add_patch(c, e->key, ty, atoi(nf), false);
            if (c->nPatches && !strcmp(c->patches[c->nPatches - 1].name, e->key)) read_groups(b, e, c->patches[c->nPatches - 1].groups, sizeof c->patches[0].groups);
        }
        fd_free(b);
        if (c->nPatches) { c->patchSource = 1; return; }
    }
    FDFile *bm = c->region[0] ? NULL : case_dict(c, "system/blockMeshDict");      /* region patches come from the split */
    if (!bm && !c->region[0]) bm = case_dict(c, "constant/polyMesh/blockMeshDict");
    FDEntry *bl = bm ? fd_find(bm, "boundary") : NULL;
    if (bl) {
        for (int i = 0; i < bl->nChild; i++) {
            FDEntry *e = &bl->child[i];
            char ty[64] = "";
            FDEntry *t = fd_child(e, "type");
            if (t) fd_entry_value(bm, t, ty, sizeof ty);
            add_patch(c, e->key, ty, -1, false);
            if (c->nPatches && !strcmp(c->patches[c->nPatches - 1].name, e->key)) {
                read_groups(bm, e, c->patches[c->nPatches - 1].groups, sizeof c->patches[0].groups);
                c->patches[c->nPatches - 1].collapsed = faces_collapsed(bm, e);
            }
        }
        if (c->nPatches) { c->patchSource = 2; return; }
    }
    for (int k = 0; k < c->fields.n; k++) {                 /* fall back: the fields' patch names */
        char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, c->fields.names[k]);
        FDFile *ff = case_dict(c, rel);
        FDEntry *bf = ff ? fd_find(ff, "boundaryField") : NULL;
        if (!bf) continue;
        for (int i = 0; i < bf->nChild; i++)
            if (bf->child[i].kind == FD_DICT) add_patch(c, bf->child[i].key, "", -1, bf->child[i].key[0] == '"');
        if (c->nPatches) { c->patchSource = 3; break; }
    }
}

/* a field file: a FoamFile header whose class is a field ("volVectorField", "pointScalarField"); read from the first 4 kB */
static bool is_field_file(const char *dir, const char *name)
{
    char p[PL_PATH];
    if (!pl_fmt(p, sizeof p, "%s/%s", dir, name)) return false;
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    char b[4097]; size_t n = fread(b, 1, 4096, f); fclose(f);
    b[n] = 0;
    const char *h = strstr(b, "FoamFile");
    if (!h) return false;
    const char *cl = strstr(h, "class");
    if (!cl) return true;                                       /* a header without a class: take it */
    const char *e = strchr(cl, ';');
    if (!e) return true;
    char cls[96]; snprintf(cls, sizeof cls, "%.*s", (int)(e - cl) < 95 ? (int)(e - cl) : 95, cl);
    return strstr(cls, "Field") != NULL;
}


void case_rescan(Case *c)
{
    case_summarize(c->s.path, &c->s);
    if (c->s.prog != PROG_OPENFOAM) {                          /* other programs: no fields, patches or time folders */
        names_free(&c->fields); names_free(&c->times); c->nPatches = 0; c->fieldDir[0] = 0;
        return;
    }
    char p[PL_PATH];
    if (c->region[0]) {                                     /* the region must still exist */
        bool ok = false;
        for (int r = 0; r < c->s.nRegions; r++) if (!strcmp(c->s.regions[r], c->region)) ok = true;
        if (!ok) c->region[0] = 0;
    }
    c->rCells = c->rFaces = c->rPoints = -1;
    if (c->region[0]) {
        char rel[160];
        snprintf(rel, sizeof rel, "0.orig/%s", c->region);
        pl_join(p, sizeof p, c->s.path, rel);
        if (!pl_is_dir(p)) snprintf(rel, sizeof rel, "0/%s", c->region);
        pl_strlcpy(c->fieldDir, rel, sizeof c->fieldDir);
        pl_join(p, sizeof p, c->s.path, case_rpath(c, "constant/polyMesh/owner"));
        char *h = pl_is_file(p) ? pl_read_head(p, 2048) : NULL;
        if (h) { c->rCells = note_value(h, "nCells:"); c->rFaces = note_value(h, "nFaces:"); c->rPoints = note_value(h, "nPoints:"); }
        free(h);
    } else {
        pl_join(p, sizeof p, c->s.path, "0.orig");
        pl_strlcpy(c->fieldDir, pl_is_dir(p) ? "0.orig" : "0", sizeof c->fieldDir);
    }
    names_free(&c->fields);
    pl_join(p, sizeof p, c->s.path, c->fieldDir);
    NameList tmp = { 0 };
    pl_list_dir(p, &tmp, false, true);
    for (int i = 0; i < tmp.n; i++) {
        const char *n = tmp.names[i];
        if (pl_ends_with(n, "~") || pl_ends_with(n, ".bak") || pl_ends_with(n, ".gz") || pl_ends_with(n, ".qc-tmp")) continue;
        if (!strcmp(n, "cellToRegion")) continue;               /* written by splitMeshRegions, not a field to set up */
        if (!is_field_file(p, n)) continue;                      /* an #include'd piece (U.bc), a README: not a field */
        names_add(&c->fields, n);
    }
    names_free(&tmp);
    names_free(&c->times);
    NameList all = { 0 };
    pl_list_dir(c->s.path, &all, true, false);
    for (int i = 0; i < all.n; i++) if (pl_is_number(all.names[i])) names_add(&c->times, all.names[i]);
    names_free(&all);
    scan_patches(c);
}

void case_set_region(Case *c, const char *region)
{
    pl_strlcpy(c->region, region ? region : "", sizeof c->region);
    case_rescan(c);
}

Case *case_open(const char *path)
{
    if (!case_is_project(path)) return NULL;
    Case *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    pl_expand_home(c->s.path, sizeof c->s.path, path);
    case_rescan(c);
    casecfg_load(c->s.path, &c->cfg);
    return c;
}

void case_close(Case *c)
{
    if (!c) return;
    for (int i = 0; i < c->nFiles; i++) fd_free(c->files[i].f);
    names_free(&c->fields);
    names_free(&c->times);
    free(c);
}

FDFile *case_dict(Case *c, const char *rel)
{
    for (int i = 0; i < c->nFiles; i++) if (!strcmp(c->files[i].rel, rel)) return c->files[i].f;
    char p[PL_PATH];
    pl_join(p, sizeof p, c->s.path, rel);
    if (!pl_is_file(p) || pl_file_size(p) > (8 << 20)) return NULL;
    FDFile *f = fd_load(p);
    if (!f) return NULL;
    if (c->nFiles >= MAX_CASEFILES) {                        /* evict a clean file */
        int k = -1;
        for (int i = 0; i < c->nFiles; i++) if (!c->files[i].f->dirty) { k = i; break; }
        if (k < 0) return f;                                 /* caller owns nothing; leak avoided below */
        fd_free(c->files[k].f);
        c->files[k] = c->files[--c->nFiles];
    }
    pl_strlcpy(c->files[c->nFiles].rel, rel, sizeof c->files[c->nFiles].rel);
    c->files[c->nFiles].f = f;
    c->nFiles++;
    return f;
}

void case_drop_file(Case *c, const char *rel)
{
    for (int i = 0; i < c->nFiles; i++)
        if (!strcmp(c->files[i].rel, rel)) { fd_free(c->files[i].f); c->files[i] = c->files[--c->nFiles]; return; }
}

void foam_header(Str *b, const char *cls, const char *location, const char *object)
{
    str_cat(b, "/*--------------------------------*- C++ -*----------------------------------*\\\n");
    str_cat(b, "| =========                 |                                                 |\n");
    str_cat(b, "| \\\\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox           |\n");
    str_cat(b, "|  \\\\    /   O peration     | Version:  v2606                                 |\n");
    str_cat(b, "|   \\\\  /    A nd           | Website:  www.openfoam.com                      |\n");
    str_cat(b, "|    \\\\/     M anipulation  |                                                 |\n");
    str_cat(b, "\\*---------------------------------------------------------------------------*/\n");
    str_catf(b, "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       %s;\n", cls);
    if (location && *location) str_catf(b, "    location    \"%s\";\n", location);
    str_catf(b, "    object      %s;\n}\n", object);
    str_cat(b, "// * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * //\n");
    str_cat(b, "// Created by Q-Continuum\n\n");
}

FDFile *case_create_dict(Case *c, const char *rel, const char *cls, const char *body)
{
    char p[PL_PATH], dir[PL_PATH];
    pl_join(p, sizeof p, c->s.path, rel);
    pl_dirname(dir, sizeof dir, p);
    pl_mkdirs(dir);
    char loc[PL_PATH]; pl_dirname(loc, sizeof loc, rel);
    Str b; str_init(&b);
    foam_header(&b, cls, loc, pl_basename(rel));
    str_cat(&b, body);
    str_cat(&b, "\n// ************************************************************************* //\n");
    bool ok = pl_write_file(p, b.s, b.len);
    str_free(&b);
    if (!ok) return NULL;
    case_drop_file(c, rel);
    return case_dict(c, rel);
}

int case_dirty_count(const Case *c)
{
    int n = 0;
    for (int i = 0; i < c->nFiles; i++) if (c->files[i].f->dirty) n++;
    return n;
}

int case_save_all(Case *c)
{
    int n = 0;
    for (int i = 0; i < c->nFiles; i++) {
        if (!c->files[i].f->dirty) continue;
        if (!fd_save(c->files[i].f)) return -1;
        n++;
    }
    return n;
}

/* files changed outside the app (an editor, a script, the app's own pause): clean ones reload, edited ones take the
   change in under their edits, and the rest are marked as conflicts for the app to ask about */
int case_reload_report(Case *c, char *what, int cap)
{
    int n = 0, k = 0;
    if (what && cap > 0) what[0] = 0;
    for (int i = 0; i < c->nFiles; i++) {
        FDFile *f = c->files[i].f;
        if (!fd_changed_on_disk(f)) continue;
        int r = fd_sync(f);
        if (r == FD_SYNC_GONE) {                              /* deleted outside the app: say so */
            n++;
            if (what && k < cap - 1)
                k += snprintf(what + k, (size_t)(cap - k), "%s%s deleted%s", k ? ", " : "", c->files[i].rel,
                              f->dirty ? " (your unsaved edits are kept; Save writes the file again)" : "");
            if (!f->dirty) { fd_free(f); c->files[i] = c->files[--c->nFiles]; i--; }
            else f->gone = true;
            continue;
        }
        if (r != FD_SYNC_RELOADED && r != FD_SYNC_REBASED) continue;
        n++;
        if (what && k < cap - 1)
            k += snprintf(what + k, (size_t)(cap - k), "%s%s%s", k ? ", " : "", c->files[i].rel, r == FD_SYNC_REBASED ? " (kept your unsaved edits)" : "");
    }
    return n;
}

void case_reload_changed(Case *c) { case_reload_report(c, NULL, 0); }

int case_conflicts(const Case *c, int *first)
{
    int n = 0;
    for (int i = 0; i < c->nFiles; i++) if (c->files[i].f->conflict) { if (!n && first) *first = i; n++; }
    return n;
}

/* Results, logs, postProcessing, and (TR_ZERO) a 0/ made from 0.orig, (TR_MESH) a mesh blockMesh can make again.
   They go to the case's trash (History > Deleted results) unless permanent; returns items removed. */
int case_trash_results(Case *c, int what, bool permanent, const char *reason, char *msg, int cap)
{
    NameList all = { 0 }, take = { 0 };
    pl_list_dir(c->s.path, &all, false, false);
    for (int i = 0; i < all.n; i++) {
        const char *n = all.names[i];
        char full[PL_PATH]; pl_join(full, sizeof full, c->s.path, n);
        bool del = false;
        if (pl_is_number(n) && pl_is_dir(full)) {
            double t = atof(n);
            if (t > c->s.startTime + 1e-12) del = true;
            if ((what & TR_ZERO) && fabs(t) < 1e-300 && c->s.hasZeroOrig && !strcmp(n, "0")) del = true;   /* regenerated from 0.orig */
        }
        if (pl_starts_with(n, "processor") && pl_is_dir(full)) del = true;
        if ((what & TR_LOGS) && pl_starts_with(n, "log.") && pl_is_file(full)) del = true;
        if ((what & TR_POST) && !strcmp(n, "postProcessing") && pl_is_dir(full)) del = true;
        if (del) names_add(&take, n);
    }
    names_free(&all);
    if (what & TR_MESH) {
        char bm[PL_PATH], pm[PL_PATH];
        pl_join(bm, sizeof bm, c->s.path, "system/blockMeshDict");
        pl_join(pm, sizeof pm, c->s.path, "constant/polyMesh");
        if (pl_is_file(bm) && pl_is_dir(pm)) names_add(&take, "constant/polyMesh");   /* only regenerable meshes */
    }
    int removed = 0;
    if (permanent) {
        for (int i = 0; i < take.n; i++) {
            char full[PL_PATH]; pl_join(full, sizeof full, c->s.path, take.names[i]);
            if (pl_remove_tree(full) == 0) removed++;
        }
    } else if (take.n) {
        removed = trash_items(c->s.path, reason, case_latest_result(&c->s), (const char *const *)take.names, take.n);
        trash_prune(c->s.path, TRASH_KEEP, TRASH_DAYS);
    }
    names_free(&take);
    if (msg) {
        if (permanent || !removed) snprintf(msg, (size_t)cap, "Removed %d item%s", removed, removed == 1 ? "" : "s");
        else snprintf(msg, (size_t)cap, "Moved %d item%s to the trash (History > Deleted results can restore them)", removed, removed == 1 ? "" : "s");
    }
    case_rescan(c);
    return removed;
}

/* ============================================================================ pause */
bool case_runtime_modifiable(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "system/controlDict");
    FDFile *f = fd_load(p);
    char v[32] = "", w[32] = "";
    if (f) { fd_get(f, "runTimeModifiable", v, sizeof v); fd_free(f); }
    fd_word(v, w, sizeof w);
    return !strcmp(w, "true") || !strcmp(w, "yes") || !strcmp(w, "on") || !strcmp(w, "1");
}

/* processes running in the case folder: the solver app (or, app NULL, any *Foam and mpirun). With needSwitch, only
   solvers started with our "-opt-switch stopAtWriteNowSignal": SIGUSR1 would kill any other. */
int case_processes(const char *casePath, const char *app, pid_t *out, int max, bool needSwitch)
{
    char real[PATH_MAX];
    if (!realpath(casePath, real)) pl_strlcpy(real, casePath, sizeof real);
    size_t rl = strlen(real);
    char app15[16] = ""; if (app) snprintf(app15, sizeof app15, "%.15s", app);
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    pid_t me = getpid();
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) == me) continue;
        char p[300], cwd[PATH_MAX];
        snprintf(p, sizeof p, "/proc/%s/cwd", e->d_name);
        ssize_t k = readlink(p, cwd, sizeof cwd - 1);
        if (k <= 0) continue;
        cwd[k] = 0;
        if (strncmp(cwd, real, rl) || (cwd[rl] && cwd[rl] != '/')) continue;
        char comm[64] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        bool match = app ? !strcmp(comm, app15) : (pl_ends_with(comm, "Foam") || !strcmp(comm, "mpirun"));
        if (!match) continue;
        if (needSwitch) {
            char cmd[2048] = ""; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
            f = fopen(p, "r");
            size_t m = 0;
            if (f) { m = fread(cmd, 1, sizeof cmd - 1, f); fclose(f); }
            for (size_t j = 0; j < m; j++) if (!cmd[j]) cmd[j] = ' ';
            cmd[m] = 0;
            if (!strstr(cmd, "stopAtWriteNowSignal")) continue;
        }
        out[n++] = (pid_t)atoi(e->d_name);
    }
    closedir(d);
    return n;
}

/* the process groups running this case (solvers, mpirun, Allrun scripts, the app's detached step scripts), to follow
   and stop a run started elsewhere or before the app restarted. *nSolver: processes of the solver itself. */
int case_run_groups(const char *casePath, const char *app, pid_t *pg, int max, int *nSolver)
{
    char real[PATH_MAX];
    if (!realpath(casePath, real)) pl_strlcpy(real, casePath, sizeof real);
    size_t rl = strlen(real);
    char app15[16] = ""; if (app) snprintf(app15, sizeof app15, "%.15s", app);
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    if (nSolver) *nSolver = 0;
    pid_t me = getpid(), mine = getpgrp();
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]) || atoi(e->d_name) == me) continue;
        char p[300], cwd[PATH_MAX];
        snprintf(p, sizeof p, "/proc/%s/cwd", e->d_name);
        ssize_t k = readlink(p, cwd, sizeof cwd - 1);
        if (k <= 0) continue;
        cwd[k] = 0;
        if (strncmp(cwd, real, rl) || (cwd[rl] && cwd[rl] != '/')) continue;
        char comm[64] = "";
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        bool solver = app15[0] && !strcmp(comm, app15);
        bool take = solver || pl_ends_with(comm, "Foam") || !strcmp(comm, "mpirun") || !strcmp(comm, "orterun") || !strcmp(comm, "prterun");
        if (!take && !strcmp(comm, "bash")) {
            char cmd[1024] = ""; snprintf(p, sizeof p, "/proc/%s/cmdline", e->d_name);
            f = fopen(p, "r");
            size_t m = 0;
            if (f) { m = fread(cmd, 1, sizeof cmd - 1, f); fclose(f); }
            for (size_t j = 0; j < m; j++) if (!cmd[j]) cmd[j] = ' ';
            cmd[m] = 0;
            take = strstr(cmd, "Allrun") || strstr(cmd, ".qc-step-rc");
        }
        if (!take) continue;
        if (solver && nSolver) (*nSolver)++;
        /* /proc/<pid>/stat: pid (comm) state ppid pgrp ... ; comm may hold spaces, so start after the last ')' */
        snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        char st[512] = "";
        f = fopen(p, "r");
        if (f) { if (!fgets(st, sizeof st, f)) st[0] = 0; fclose(f); }
        char *rp = strrchr(st, ')');
        int ppid = 0, pgrp = 0; char state = 0;
        if (!rp || sscanf(rp + 2, "%c %d %d", &state, &ppid, &pgrp) != 3 || pgrp <= 1 || pgrp == mine) continue;
        bool have = false;
        for (int i = 0; i < n; i++) if (pg[i] == pgrp) have = true;
        if (!have && n < max) pg[n++] = pgrp;
    }
    closedir(d);
    return n;
}

/* Pause: the solver writes the time step it is on and ends as if it had reached the end time. Two ways in:
   SIGUSR1 to solvers the app started with -opt-switch stopAtWriteNowSignal=10, and stopAt writeNow in controlDict,
   which a solver re-reads when runTimeModifiable is on (runs started elsewhere, Allrun). <case>/.qc-pause keeps the
   stopAt to put back once the run has ended. Returns false (err says why) when neither reaches the solver. */
bool case_pause_request(const char *casePath, const char *app, char *err, int cap)
{
    pid_t pids[256];
    int ns = case_processes(casePath, app && *app ? app : NULL, pids, 256, true);
    for (int i = 0; i < ns; i++) kill(pids[i], SIGUSR1);
    bool edited = false;
    if (case_runtime_modifiable(casePath)) {
        char p[PL_PATH], q[PL_PATH];
        pl_join(p, sizeof p, casePath, "system/controlDict");
        pl_join(q, sizeof q, casePath, PAUSE_REQ);
        FDFile *f = fd_load(p);
        if (f) {
            char v[64] = "", w[64] = "";
            fd_get(f, "stopAt", v, sizeof v); fd_word(v, w, sizeof w);
            if (!pl_is_file(q)) {                               /* a second Pause must not record writeNow as the original */
                char b[128]; int L = snprintf(b, sizeof b, "stopAt=%s\n", w[0] && strcmp(w, "writeNow") ? w : "endTime");
                pl_write_file(q, b, (size_t)L);
            }
            edited = fd_put(f, "stopAt", "writeNow") && fd_save(f);
            fd_free(f);
        }
    }
    if (ns > 0 || edited) {
        if (!edited) { char q[PL_PATH]; pl_join(q, sizeof q, casePath, PAUSE_REQ); if (!pl_is_file(q)) pl_write_file(q, "stopAt=\n", 8); }
        return true;
    }
    snprintf(err, (size_t)cap, "This run can't be paused: it wasn't started by Q-Continuum and system/controlDict has runTimeModifiable "
             "off, so the solver never re-reads it. Stop it instead (it keeps the last written time), or turn runTimeModifiable on "
             "for the next run.");
    return false;
}

bool case_pause_pending(const char *casePath)
{
    char q[PL_PATH]; pl_join(q, sizeof q, casePath, PAUSE_REQ);
    return pl_is_file(q);
}

/* the run has ended: put stopAt back; when it ended by the pause, remember where (Resume continues from there) */
bool case_pause_settle(const char *casePath, bool paused, const char *latest)
{
    char q[PL_PATH]; pl_join(q, sizeof q, casePath, PAUSE_REQ);
    char *t = pl_read_file(q, NULL);
    if (!t) return false;
    char orig[64] = "";
    const char *k = strstr(t, "stopAt=");
    if (k) sscanf(k + 7, "%63s", orig);
    free(t);
    if (orig[0]) {
        char p[PL_PATH]; pl_join(p, sizeof p, casePath, "system/controlDict");
        FDFile *f = fd_load(p);
        if (f) {
            char v[64] = "", w[64] = "";
            fd_get(f, "stopAt", v, sizeof v); fd_word(v, w, sizeof w);
            if (!strcmp(w, "writeNow")) { fd_put(f, "stopAt", orig); fd_save(f); }
            fd_free(f);
        }
    }
    unlink(q);
    char m[PL_PATH]; pl_join(m, sizeof m, casePath, PAUSE_MARK);
    if (paused) { char b[96]; int L = snprintf(b, sizeof b, "t=%s\n", latest ? latest : ""); pl_write_file(m, b, (size_t)L); }
    return true;
}

bool case_paused(const char *casePath, char *t, int cap)
{
    char m[PL_PATH]; pl_join(m, sizeof m, casePath, PAUSE_MARK);
    char *b = pl_read_file(m, NULL);
    if (!b) return false;
    if (t) { t[0] = 0; const char *k = strstr(b, "t="); if (k) sscanf(k + 2, "%31s", t); (void)cap; }
    free(b);
    return true;
}

void case_pause_clear(const char *casePath)
{
    char m[PL_PATH]; pl_join(m, sizeof m, casePath, PAUSE_MARK);
    unlink(m);
}

/* ============================================================================ per-case settings */
void casecfg_load(const char *casePath, CaseCfg *c)
{
    memset(c, 0, sizeof *c);
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, ".q-continuum.ini");
    size_t n; char *t = pl_read_file(p, &n);
    if (!t) return;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "pre") && c->nPre < CFG_MAX_CMDS) pl_strlcpy(c->pre[c->nPre++], v, 256);
        else if (!strcmp(k, "post") && c->nPost < CFG_MAX_CMDS) pl_strlcpy(c->post[c->nPost++], v, 256);
        else if (!strcmp(k, "monitor.file")) pl_strlcpy(c->monFile, v, sizeof c->monFile);
        else if (!strcmp(k, "monitor.hide")) pl_strlcpy(c->monHide, v, sizeof c->monHide);
        else if (!strcmp(k, "converge")) {                        /* on|file|column|tolerance|window */
            char tmp[512]; pl_strlcpy(tmp, v, sizeof tmp);
            char *f[5] = { 0 }; int nf = 0;
            for (char *q = tmp; nf < 5; ) { f[nf++] = q; char *b = strchr(q, '|'); if (!b) break; *b = 0; q = b + 1; }
            if (nf == 5) {
                c->convOn = !strcmp(f[0], "on");
                pl_strlcpy(c->convFile, f[1], sizeof c->convFile); pl_strlcpy(c->convCol, f[2], sizeof c->convCol);
                c->convTol = atof(f[3]); c->convWin = atoi(f[4]);
            }
        }
        else if (pl_starts_with(k, "args.") && c->nArgs < CFG_MAX_ARGS) {
            pl_strlcpy(c->argKey[c->nArgs], k + 5, 64); pl_strlcpy(c->argVal[c->nArgs], v, 128); c->nArgs++;
        }
    }
    free(t);
}

bool casecfg_save(const char *casePath, const CaseCfg *c)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, ".q-continuum.ini");
    bool any = c->nPre || c->nPost || c->monFile[0] || c->convFile[0];
    for (int i = 0; i < c->nArgs; i++) if (c->argVal[i][0]) any = true;
    if (!any) { unlink(p); return true; }                   /* nothing to remember: no file */
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum per-case settings (Run page: your steps, case script arguments, monitors)\n");
    for (int i = 0; i < c->nPre; i++) if (c->pre[i][0]) str_catf(&b, "pre=%s\n", c->pre[i]);
    for (int i = 0; i < c->nPost; i++) if (c->post[i][0]) str_catf(&b, "post=%s\n", c->post[i]);
    for (int i = 0; i < c->nArgs; i++) if (c->argVal[i][0]) str_catf(&b, "args.%s=%s\n", c->argKey[i], c->argVal[i]);
    if (c->monFile[0]) str_catf(&b, "monitor.file=%s\nmonitor.hide=%s\n", c->monFile, c->monHide);
    if (c->convFile[0]) str_catf(&b, "converge=%s|%s|%s|%g|%d\n", c->convOn ? "on" : "off", c->convFile, c->convCol, c->convTol, c->convWin);
    bool ok = pl_write_file(p, b.s, b.len);
    str_free(&b);
    return ok;
}

const char *casecfg_args(const CaseCfg *c, const char *script)
{
    for (int i = 0; i < c->nArgs; i++) if (!strcmp(c->argKey[i], script)) return c->argVal[i];
    return "";
}

void casecfg_set_args(CaseCfg *c, const char *script, const char *args)
{
    for (int i = 0; i < c->nArgs; i++) if (!strcmp(c->argKey[i], script)) { pl_strlcpy(c->argVal[i], args, 128); return; }
    if (c->nArgs < CFG_MAX_ARGS) { pl_strlcpy(c->argKey[c->nArgs], script, 64); pl_strlcpy(c->argVal[c->nArgs], args, 128); c->nArgs++; }
}

/* ============================================================================ tutorials */
static void tut_add(TutorialIndex *ix, const char *rel, const char *abs)
{
    if (ix->n == ix->cap) {
        ix->cap = ix->cap ? ix->cap * 2 : 256;
        ix->t = realloc(ix->t, sizeof(Tutorial) * (size_t)ix->cap);
    }
    Tutorial *t = &ix->t[ix->n++];
    memset(t, 0, sizeof *t);
    pl_strlcpy(t->rel, rel, sizeof t->rel);
    pl_strlcpy(t->name, pl_basename(rel), sizeof t->name);
    const char *sl = strchr(rel, '/');
    int g = sl ? (int)(sl - rel) : (int)strlen(rel);
    snprintf(t->group, sizeof t->group, "%.*s", g, rel);
    char p[PL_PATH];
    pl_join(p, sizeof p, abs, "system/controlDict");
    FDFile *cd = fd_load(p);
    if (cd) {
        char v[128];
        if (fd_get(cd, "application", v, sizeof v)) fd_word(v, t->solver, sizeof t->solver);
        fd_free(cd);
    }
    if (!t->solver[0] && sl) {                               /* group/solver/... */
        const char *s2 = strchr(sl + 1, '/');
        snprintf(t->solver, sizeof t->solver, "%.*s", s2 ? (int)(s2 - sl - 1) : (int)strlen(sl + 1), sl + 1);
    }
    pl_join(p, sizeof p, abs, "Allrun");
    t->hasAllrun = pl_is_file(p);
    t->cat = category_of(t->solver, rel);
}

static void tut_scan(TutorialIndex *ix, const char *abs, const char *rel, int depth)
{
    if (depth > 7) return;
    if (rel[0] && case_is_case(abs)) { tut_add(ix, rel, abs); return; }
    NameList l = { 0 };
    pl_list_dir(abs, &l, true, false);
    for (int i = 0; i < l.n; i++) {
        const char *n = l.names[i];
        if (!strcmp(n, "resources") || !strcmp(n, "system") || !strcmp(n, "constant") || pl_is_number(n)) continue;
        char a2[PL_PATH], r2[PL_PATH];
        pl_join(a2, sizeof a2, abs, n);
        if (rel[0]) pl_join(r2, sizeof r2, rel, n); else pl_strlcpy(r2, n, sizeof r2);
        tut_scan(ix, a2, r2, depth + 1);
    }
    names_free(&l);
}

void tutorials_free(TutorialIndex *ix) { free(ix->t); memset(ix, 0, sizeof *ix); }

void tutorials_scan(TutorialIndex *ix, const char *root)
{
    tutorials_free(ix);
    pl_strlcpy(ix->root, root, sizeof ix->root);
    if (pl_is_dir(root)) tut_scan(ix, root, "", 0);
}

int tutorials_find(const TutorialIndex *ix, const char *suffix)
{
    for (int i = 0; i < ix->n; i++) {
        size_t a = strlen(ix->t[i].rel), b = strlen(suffix);
        if (a >= b && !strcmp(ix->t[i].rel + a - b, suffix) && (a == b || ix->t[i].rel[a - b - 1] == '/')) return i;
    }
    return -1;
}

/* Some tutorials live in a group and need it: a sibling feeds them (mapFields from it, a mesh merged from it, files copied
   from ../common), the group prepares them (its Allrun.pre), or the parts run together (a multi-world mpirun). Copied
   alone they fail. Others only sit in a group: the first case of icoFoam/cavity runs on its own and the rest are mapped
   from it, so it is copied alone. Rules:
     1. the case's own scripts or dictionaries refer to ../<sibling>;
     2. the group has an Allrun.pre, or its Allrun runs a multi-world mpirun (-world, mpirun -app);
     3. the group's Allrun names another sibling before this one (this case is a later stage).
   *why says which, in a sentence for the toast. */
static bool word_at(const char *t, const char *p, size_t n)
{
    bool before = p == t || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
    bool after = !(isalnum((unsigned char)p[n]) || p[n] == '_');
    return before && after;
}

/* where a name is first mentioned as a whole word in the script's command lines (not comments), or -1 */
static long first_mention(const char *t, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = strstr(t, name); p; p = strstr(p + 1, name)) {
        const char *ls = p; while (ls > t && ls[-1] != '\n') ls--;
        while (*ls == ' ' || *ls == '\t') ls++;
        if (*ls == '#') continue;
        if (word_at(t, p, n)) return (long)(p - t);
    }
    return -1;
}

bool tutorial_needs_group(const char *src, char *why, int cap)
{
    why[0] = 0;
    char parent[PL_PATH]; pl_dirname(parent, sizeof parent, src);
    const char *me = pl_basename(src);
    NameList sib = { 0 };
    pl_list_dir(parent, &sib, true, false);
    bool need = false;
    /* 1. the case's own files */
    NameList own = { 0 };
    pl_list_dir(src, &own, false, true);
    for (int f = 0; f < own.n + 6 && !need; f++) {
        const char *fixed[] = { "system/blockMeshDict", "system/controlDict", "system/snappyHexMeshDict", "system/topoSetDict",
                                "constant/dynamicMeshDict", "system/mapFieldsDict" };
        char rel[300];
        if (f < own.n) { if (!pl_starts_with(own.names[f], "Allrun") && strcmp(own.names[f], "Allclean")) continue; pl_strlcpy(rel, own.names[f], sizeof rel); }
        else pl_strlcpy(rel, fixed[f - own.n], sizeof rel);
        char p[PL_PATH]; pl_join(p, sizeof p, src, rel);
        char *t = pl_read_file(p, NULL);
        for (int i = 0; t && i < sib.n && !need; i++) {
            if (!strcmp(sib.names[i], me)) continue;
            char ref[300]; snprintf(ref, sizeof ref, "../%s", sib.names[i]);
            const char *q = strstr(t, ref);
            if (q && word_at(t, q + 3, strlen(sib.names[i]))) { need = true; snprintf(why, (size_t)cap, "its %s uses ../%s", rel, sib.names[i]); }
        }
        free(t);
    }
    names_free(&own);
    /* 2. the group prepares it, or runs the parts together */
    char pa[PL_PATH]; pl_join(pa, sizeof pa, parent, "Allrun.pre");
    if (!need && pl_is_file(pa)) { need = true; snprintf(why, (size_t)cap, "the group's Allrun.pre prepares %s", me); }
    pl_join(pa, sizeof pa, parent, "Allrun");
    char *ar = need ? NULL : pl_read_file(pa, NULL);
    if (ar && (strstr(ar, "-world") || strstr(ar, "mpirun -app"))) { need = true; snprintf(why, (size_t)cap, "the group runs its parts together (multi-world mpirun)"); }
    /* 3. a later stage: another part comes first in the group's Allrun */
    if (ar && !need) {
        long mine = first_mention(ar, me);
        for (int i = 0; i < sib.n && mine >= 0 && !need; i++) {
            if (!strcmp(sib.names[i], me)) continue;
            char sc[PL_PATH], rel[300]; snprintf(rel, sizeof rel, "%s/system/controlDict", sib.names[i]);
            pl_join(sc, sizeof sc, parent, rel);
            if (!pl_is_file(sc)) continue;
            long other = first_mention(ar, sib.names[i]);
            if (other >= 0 && other < mine) { need = true; snprintf(why, (size_t)cap, "the group's Allrun runs %s before it", sib.names[i]); }
        }
    }
    free(ar);
    /* 4. fed by a sibling: its scripts name ../<this case> as their target (obliqueAirJet: precursor's
          targetCase="../main") */
    for (int i = 0; i < sib.n && !need; i++) {
        if (!strcmp(sib.names[i], me)) continue;
        char sd[PL_PATH]; pl_join(sd, sizeof sd, parent, sib.names[i]);
        NameList sf = { 0 };
        pl_list_dir(sd, &sf, false, true);
        for (int f = 0; f < sf.n && !need; f++) {
            if (!pl_starts_with(sf.names[f], "Allrun")) continue;
            char p[PL_PATH]; pl_join(p, sizeof p, sd, sf.names[f]);
            char *t = pl_read_file(p, NULL);
            char ref[300]; snprintf(ref, sizeof ref, "../%s", me);
            for (char *line = t ? strtok(t, "\n") : NULL; line && !need; line = strtok(NULL, "\n")) {
                const char *q = strstr(line, ref);
                if (q && word_at(line, q + 3, strlen(me)) && (strcasestr(line, "target") || strstr(line, "-case"))) {
                    need = true; snprintf(why, (size_t)cap, "%s's %s writes into it", sib.names[i], sf.names[f]);
                }
            }
            free(t);
        }
        names_free(&sf);
    }
    names_free(&sib);
    return need;
}

/* how a solver log ends: the last "Time = ", whether it reached "End", and whether the solver reported convergence
   (simpleFoam's residualControl ends a steady run early on purpose). Reads only the end of the log. */
bool log_tail_state(const char *casePath, const char *logName, double *lastTime, bool *ended, bool *converged)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, logName);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = size > (1 << 18) ? size - (1 << 18) : 0;
    if (size < 0) { fclose(f); return false; }
    fseek(f, from, SEEK_SET);
    char *t = malloc((size_t)(size - from) + 1);
    size_t n = t ? fread(t, 1, (size_t)(size - from), f) : 0;
    fclose(f);
    if (!t) return false;
    t[n] = 0;
    *lastTime = -1; *ended = false; *converged = false;
    for (char *l = t; l && *l; ) {
        char *e = strchr(l, '\n');
        if (e) *e = 0;
        if (pl_starts_with(l, "Time = ")) *lastTime = atof(l + 7);
        else if (!strcmp(l, "End") || pl_starts_with(l, "End ")) *ended = true;
        else if (strstr(l, "solution converged in") || strstr(l, "reached convergence criterion")) *converged = true;
        l = e ? e + 1 : NULL;
    }
    free(t);
    return true;
}
