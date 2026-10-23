/* program.c -- DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE, Elmer and SPARTA projects: what a folder is,
 * what it can run, how to run it, how to read its log as it goes (time, residual-like series, errors), what to check
 * first and where its results are. See program.h. Log formats are from real runs on this machine (heatBench,
 * the SPARTA examples, DEME drivers); the tests feed those logs through prog_parse_line. */
#define _GNU_SOURCE
#include "program.h"
#include "assets.h"

/* run commands hold paths of up to PL_PATH each: a path that long makes a cut-off command, which the shell rejects */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <ctype.h>
#include <stdarg.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static const ProgInfo INFO[PROG_COUNT] = {
    [PROG_OPENFOAM] = { "OpenFOAM",     "openfoam", "Finite-volume CFD and heat transfer",                 "case",       "",             false, true,  false, true  },
    [PROG_DEME]     = { "DEME",         "deme",     "GPU discrete elements: granular flow, regolith",      "program",    "",             true,  false, false, true  },
    [PROG_CHRONO]   = { "Chrono",       "chrono",   "Multibody dynamics, vehicles, FEA",                   "program",    "",             true,  false, false, true  },
    [PROG_CALCULIX] = { "CalculiX",     "calculix", "Finite elements: structures and heat transfer",       "input file", "ccx",          false, false, true,  true  },
    [PROG_ASTER]    = { "code_aster",   "aster",    "Finite elements: structures, thermal, mechanics",     "study",      "run_aster",    false, false, false, true  },
    [PROG_SATURNE]  = { "code_saturne", "saturne",  "Finite-volume CFD (EDF)",                             "case",       "code_saturne", false, true,  false, true  },
    [PROG_MOOSE]    = { "MOOSE",        "moose",    "Multiphysics finite elements (INL)",                  "input file", "",             false, true,  false, true  },
    [PROG_ELMER]    = { "Elmer",        "elmer",    "Multiphysics finite elements (CSC)",                  "input file", "ElmerSolver",  false, false, false, true  },
    [PROG_SPARTA]   = { "SPARTA",       "sparta",   "DSMC: rarefied gas, plumes in vacuum",                "input script", "spa_mpi",    false, true,  false, false },
};

static const ProgInfo COUPLED_INFO = { "preCICE coupling", "coupled", "Solvers coupled through preCICE: conjugate heat transfer, "
                                       "fluid-structure interaction", "participant", "", false, true, false, true };
const ProgInfo *prog_info(int p) { return p == PROG_COUPLED ? &COUPLED_INFO : p >= 0 && p < PROG_COUNT ? &INFO[p] : &INFO[0]; }

int prog_by_key(const char *key)
{
    for (int i = 0; i < PROG_COUNT; i++) if (key && !strcasecmp(key, INFO[i].key)) return i;
    return PROG_NONE;
}

/* ============================================================================ small helpers */
static char *read_some(const char *path, size_t cap)
{
    long long sz = pl_file_size(path);
    if (sz < 0) return NULL;
    if (sz <= (long long)cap * 2) return pl_read_head(path, (size_t)sz + 1);
    /* a big CalculiX deck: the mesh in the middle, the steps at the end; keep the head and the tail */
    char *head = pl_read_head(path, cap);
    if (!head) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return head;
    char *out = malloc(cap * 2 + 4);
    if (!out) { fclose(f); return head; }
    size_t hl = strlen(head);
    memcpy(out, head, hl); out[hl++] = '\n';
    free(head);
    if (fseek(f, -(long)cap, SEEK_END) == 0) { size_t r = fread(out + hl, 1, cap, f); hl += r; }
    out[hl] = 0;
    fclose(f);
    return out;
}

static bool line_starts_ci(const char *l, const char *w)
{
    while (*l == ' ' || *l == '\t') l++;
    return strncasecmp(l, w, strlen(w)) == 0;
}

/* does a line of t start (after blanks) with w, case-insensitively */
static bool has_line_ci(const char *t, const char *w)
{
    for (const char *p = t; p && *p; ) {
        if (line_starts_ci(p, w)) return true;
        p = strchr(p, '\n'); if (p) p++;
    }
    return false;
}

static bool file_has_line(const char *path, const char *w, size_t cap)
{
    char *t = read_some(path, cap);
    bool ok = t && has_line_ci(t, w);
    free(t);
    return ok;
}

static bool file_contains(const char *path, const char *needle, size_t cap)
{
    char *t = read_some(path, cap);
    bool ok = t && strstr(t, needle);
    free(t);
    return ok;
}

static const char *ext_of(const char *n) { const char *d = strrchr(n, '.'); return d ? d : ""; }

static void stem_of(const char *n, char *out, int cap)
{
    const char *b = pl_basename(n);
    if (pl_starts_with(b, "in.")) { pl_strlcpy(out, b + 3, (size_t)cap); return; }      /* SPARTA in.circle -> circle */
    pl_strlcpy(out, b, (size_t)cap);
    char *d = strrchr(out, '.'); if (d && d != out) *d = 0;
}

/* the kind of a top-level file of a folder: which program's input it looks like */
static int input_kind(const char *dir, const char *n)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, n);
    const char *e = ext_of(n);
    if (!strcasecmp(e, ".inp") && file_has_line(p, "*STEP", 65536)) return PROG_CALCULIX;
    if (!strcmp(e, ".export") && (file_contains(p, "F comm", 16384) || file_contains(p, "P actions", 16384))) return PROG_ASTER;
    if (!strcmp(e, ".comm") && (file_contains(p, "DEBUT", 65536) || file_contains(p, "FIN(", 65536) || file_contains(p, "POURSUITE", 65536))) return PROG_ASTER;
    if (!strcmp(e, ".i") && file_has_line(p, "[Executioner]", 65536)) return PROG_MOOSE;
    if (!strcasecmp(e, ".sif") && file_has_line(p, "Simulation", 65536) && file_has_line(p, "Solver", 65536)) return PROG_ELMER;
    if (pl_starts_with(n, "in.") && !pl_ends_with(n, "~") &&
        (file_has_line(p, "create_box", 65536) || file_has_line(p, "read_grid", 65536) || file_has_line(p, "create_grid", 65536))) return PROG_SPARTA;
    return PROG_NONE;
}

static int cmake_kind(const char *dir)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "CMakeLists.txt");
    char *t = pl_read_head(p, 32768);
    int k = PROG_NONE;
    if (t) {
        if (strstr(t, "find_package(DEME")) k = PROG_DEME;
        else if (strstr(t, "find_package(Chrono")) k = PROG_CHRONO;
    }
    free(t);
    return k;
}

static bool is_saturne_case(const char *dir)
{
    char d[PL_PATH], s[PL_PATH], x[PL_PATH], c[PL_PATH];
    pl_join(d, sizeof d, dir, "DATA"); pl_join(s, sizeof s, dir, "SRC");
    pl_join(x, sizeof x, dir, "DATA/setup.xml"); pl_join(c, sizeof c, dir, "DATA/run.cfg");
    return pl_is_dir(d) && (pl_is_file(x) || (pl_is_dir(s) && pl_is_file(c)));
}

int prog_detect(const char *dir)
{
    if (!dir || !*dir || !pl_is_dir(dir)) return PROG_NONE;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "system/controlDict");
    if (pl_is_file(p)) return PROG_OPENFOAM;
    pl_join(p, sizeof p, dir, "precice-config.xml");
    if (pl_is_file(p)) return PROG_COUPLED;
    int k = cmake_kind(dir);
    if (k != PROG_NONE) return k;
    if (is_saturne_case(dir)) return PROG_SATURNE;
    const NameList *l = pl_list_dir_cached(dir, false, true);
    /* by the first input found in this order: a folder with both a mesh .inp and a deck is CalculiX either way */
    static const int ORDER[] = { PROG_CALCULIX, PROG_ASTER, PROG_MOOSE, PROG_ELMER, PROG_SPARTA };
    int found[PROG_COUNT] = { 0 };
    for (int i = 0; i < l->n && i < 400; i++) {
        const char *n = l->names[i];
        if (n[0] == '.') continue;
        const char *e = ext_of(n);
        if (strcasecmp(e, ".inp") && strcmp(e, ".export") && strcmp(e, ".comm") && strcmp(e, ".i") && strcasecmp(e, ".sif") &&
            !pl_starts_with(n, "in.")) continue;
        int kk = input_kind(dir, n);
        if (kk >= 0) found[kk]++;
    }
    for (size_t i = 0; i < sizeof ORDER / sizeof ORDER[0]; i++) if (found[ORDER[i]]) return ORDER[i];
    return PROG_NONE;
}

bool prog_is_project(const char *dir) { return prog_detect(dir) != PROG_NONE; }

/* ============================================================================ inputs */
static void cmake_targets(const char *dir, NameList *out)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "CMakeLists.txt");
    char *t = pl_read_head(p, 65536);
    if (!t) return;
    for (char *q = t; (q = strcasestr(q, "add_executable(")); ) {
        q += strlen("add_executable(");
        while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        char name[128]; int k = 0;
        while (*q && (isalnum((unsigned char)*q) || *q == '_' || *q == '-' || *q == '.') && k < 127) name[k++] = *q++;
        name[k] = 0;
        if (!k || name[0] == '$') continue;
        bool dup = false; for (int i = 0; i < out->n; i++) if (!strcmp(out->names[i], name)) dup = true;
        if (!dup) names_add(out, name);
    }
    free(t);
}

int prog_inputs(int prog, const char *dir, NameList *out)
{
    names_free(out);
    if (prog == PROG_COUPLED) { names_add(out, "precice-config.xml"); return 1; }
    if (prog == PROG_DEME || prog == PROG_CHRONO) { cmake_targets(dir, out); return out->n; }
    if (prog == PROG_SATURNE) {
        char x[PL_PATH]; pl_join(x, sizeof x, dir, "DATA/setup.xml");
        names_add(out, pl_is_file(x) ? "DATA/setup.xml" : "DATA/run.cfg");
        return out->n;
    }
    const NameList *l = pl_list_dir_cached(dir, false, true);
    NameList comm = { 0 };
    for (int i = 0; i < l->n; i++) {
        const char *n = l->names[i];
        if (n[0] == '.') continue;
        if (prog == PROG_ASTER && !strcmp(ext_of(n), ".comm") && input_kind(dir, n) == PROG_ASTER) { names_add(&comm, n); continue; }
        if (input_kind(dir, n) == prog) names_add(out, n);
    }
    if (prog == PROG_ASTER) {                          /* a .comm no .export runs gets its own (written on run) */
        for (int i = 0; i < comm.n; i++) {
            char stem[128]; stem_of(comm.names[i], stem, sizeof stem);
            bool covered = false;
            for (int k = 0; k < out->n; k++) {
                char p[PL_PATH]; pl_join(p, sizeof p, dir, out->names[k]);
                char *t = pl_read_head(p, 16384);
                if (t && strstr(t, comm.names[i])) covered = true;
                free(t);
            }
            if (!covered) names_add(out, comm.names[i]);
        }
        names_free(&comm);
    }
    names_sort(out);
    return out->n;
}

static bool text_file_name(const char *n)
{
    static const char *const EXT[] = { ".inp", ".i", ".sif", ".comm", ".export", ".py", ".sh", ".txt", ".md", ".ini", ".cpp",
                                       ".cc", ".cu", ".cuh", ".hpp", ".h", ".c", ".cmake", ".xml", ".cfg", ".json", ".species",
                                       ".vss", ".grd", ".fbd", ".geo", ".msh", ".tmpl", ".yaml", ".yml", ".csv", NULL };
    if (!strcmp(n, "CMakeLists.txt") || pl_starts_with(n, "in.") || pl_starts_with(n, "data.") || !strcmp(n, "Allrun") ||
        !strcmp(n, "Allclean") || !strcmp(n, "ELMERSOLVER_STARTINFO") || !strcmp(n, "README")) return true;
    const char *e = ext_of(n);
    for (int i = 0; EXT[i]; i++) if (!strcasecmp(e, EXT[i])) return true;
    return false;
}

static void files_in(const char *dir, const char *rel, NameList *out, int depth)
{
    char d[PL_PATH]; if (rel[0]) pl_join(d, sizeof d, dir, rel); else pl_strlcpy(d, dir, sizeof d);
    NameList l = { 0 };
    pl_list_dir(d, &l, false, false);
    names_sort(&l);
    for (int i = 0; i < l.n && out->n < 300; i++) {
        const char *n = l.names[i];
        if (n[0] == '.' || pl_ends_with(n, "~")) continue;
        char r[PL_PATH]; if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, n); else pl_strlcpy(r, n, sizeof r);
        char p[PL_PATH]; pl_join(p, sizeof p, dir, r);
        if (pl_is_dir(p)) {
            if (depth > 0 && strcmp(n, "build") && strcmp(n, "RESU") && strcmp(n, "run") && strcmp(n, "output") &&
                strcmp(n, "mesh") && strcmp(n, "results") && strcmp(n, "__pycache__") && !pl_starts_with(n, "wrk_") &&
                !pl_starts_with(n, "DemoOutput")) files_in(dir, r, out, depth - 1);
            continue;
        }
        if (!text_file_name(n) || pl_file_size(p) > (8 << 20)) continue;
        if (!strcasecmp(ext_of(n), ".csv") && pl_file_size(p) > (256 << 10)) continue;    /* results, not inputs */
        names_add(out, r);
    }
    names_free(&l);
}

int prog_files(int prog, const char *dir, NameList *out)
{
    names_free(out);
    if (prog == PROG_COUPLED) {                              /* the configuration, and each participant's coupling files */
        names_add(out, "precice-config.xml");
        NameList d = { 0 };
        pl_list_dir(dir, &d, true, false);
        for (int i = 0; i < d.n; i++) {
            static const char *const each[] = { "run.sh", "system/preciceDict", "system/controlDict", "clean.sh", NULL };
            for (int k = 0; each[k]; k++) {
                char rel[PL_PATH], p[PL_PATH];
                if (!pl_fmt(rel, sizeof rel, "%s/%s", d.names[i], each[k]) || !pl_fmt(p, sizeof p, "%s/%s", dir, rel)) continue;
                if (pl_is_file(p)) names_add(out, rel);
            }
        }
        names_free(&d);
        return out->n;
    }
    files_in(dir, "", out, prog == PROG_SATURNE ? 1 : (prog == PROG_DEME || prog == PROG_CHRONO ? 1 : 0));
    return out->n;
}

/* ============================================================================ executables */
static bool moose_app(char *out, int cap)
{
    const char *env = getenv("QC_MOOSE_APP");
    if (env && *env && pl_is_exec(env)) { pl_strlcpy(out, env, (size_t)cap); return true; }
    char pat[PL_PATH];
    /* the usual build places first: searching the PATH costs ~50 ms a name under WSL (its /mnt/c folders) */
    static const char *const FIXED[] = { "%s/projects/moose/modules/combined/combined-opt", "%s/projects/moose/modules/heat_transfer/heat_transfer-opt", NULL };
    for (int i = 0; FIXED[i]; i++) { snprintf(pat, sizeof pat, FIXED[i], pl_home()); if (pl_is_exec(pat)) { pl_strlcpy(out, pat, (size_t)cap); return true; } }
    static const char *const NAMES[] = { "combined-opt", "heat_transfer-opt", "moose-opt", "solid_mechanics-opt", NULL };
    for (int i = 0; NAMES[i]; i++) if (pl_which(NAMES[i], out, (size_t)cap)) return true;
    static const char *const PATS[] = { "%s/projects/moose/modules/*/*-opt", "%s/projects/*/*-opt", NULL };
    for (int i = 0; PATS[i]; i++) {
        snprintf(pat, sizeof pat, PATS[i], pl_home());
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; k++)
                if (pl_is_exec(g.gl_pathv[k])) { pl_strlcpy(out, g.gl_pathv[k], (size_t)cap); globfree(&g); return true; }
            globfree(&g);
        }
    }
    return false;
}

static void deme_dir(char *out, int cap)
{
    out[0] = 0;
    const char *pats[] = { "%s/src/DEM-Engine/build/DEME/DEMEConfig.cmake", "%s/DEM-Engine/build/DEME/DEMEConfig.cmake",
                           "%s/src/*/build/DEME/DEMEConfig.cmake", "/usr/local/lib/cmake/DEME/DEMEConfig.cmake", NULL };
    for (int i = 0; pats[i] && !out[0]; i++) {
        char pat[PL_PATH]; snprintf(pat, sizeof pat, pats[i], pl_home());
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) { if (g.gl_pathc) pl_dirname(out, (size_t)cap, g.gl_pathv[0]); globfree(&g); }
    }
}

static void chrono_dir(char *out, int cap)
{
    out[0] = 0;
    const char *pats[] = { "%s/src/chrono/build/cmake/ChronoConfig.cmake", "%s/chrono/build/cmake/ChronoConfig.cmake",
                           "/usr/local/lib/cmake/Chrono/ChronoConfig.cmake", NULL };
    for (int i = 0; pats[i] && !out[0]; i++) {
        char pat[PL_PATH]; snprintf(pat, sizeof pat, pats[i], pl_home());
        if (pl_is_file(pat)) pl_dirname(out, (size_t)cap, pat);
    }
}

static void nvcc_path(char *out, int cap)
{
    if (pl_which("nvcc", out, (size_t)cap)) return;
    out[0] = 0;
    glob_t g;
    if (glob("/usr/local/cuda*/bin/nvcc", 0, NULL, &g) == 0) { if (g.gl_pathc) pl_strlcpy(out, g.gl_pathv[g.gl_pathc - 1], (size_t)cap); globfree(&g); }
}

bool prog_exe(int prog, char *out, int cap, char *why, int whyCap)
{
    out[0] = 0; if (why) why[0] = 0;
    switch (prog) {
    case PROG_MOOSE:
        if (moose_app(out, cap)) return true;
        if (why) snprintf(why, (size_t)whyCap, "No MOOSE application found (combined-opt, heat_transfer-opt ...). Build one under "
                          "~/projects/moose/modules, or set QC_MOOSE_APP to its path.");
        return false;
    case PROG_DEME: {
        char d[PL_PATH]; deme_dir(d, sizeof d);
        if (!d[0]) { if (why) snprintf(why, (size_t)whyCap, "DEME is not built: no DEMEConfig.cmake under ~/src/DEM-Engine/build."); return false; }
        char n[PL_PATH]; nvcc_path(n, sizeof n);
        if (!n[0]) { if (why) snprintf(why, (size_t)whyCap, "No CUDA compiler (nvcc): DEME drivers need the CUDA toolkit."); return false; }
        pl_strlcpy(out, d, (size_t)cap);
        return true;
    }
    case PROG_CHRONO: {
        char d[PL_PATH]; chrono_dir(d, sizeof d);
        if (!d[0]) { if (why) snprintf(why, (size_t)whyCap, "Chrono is not built: no ChronoConfig.cmake under ~/src/chrono/build."); return false; }
        pl_strlcpy(out, d, (size_t)cap);
        return true;
    }
    case PROG_SPARTA:
        if (pl_which("spa_mpi", out, (size_t)cap) || pl_which("spa_serial", out, (size_t)cap) || pl_which("spa_kokkos", out, (size_t)cap)) return true;
        if (why) snprintf(why, (size_t)whyCap, "SPARTA (spa_mpi) is not on the PATH. Build it in ~/src/sparta/src (make mpi).");
        return false;
    default:
        if (prog <= PROG_OPENFOAM || prog >= PROG_COUNT) return false;
        if (pl_which(INFO[prog].exe, out, (size_t)cap)) return true;
        if (why) snprintf(why, (size_t)whyCap, "%s (%s) is not installed or not on the PATH.", INFO[prog].name, INFO[prog].exe);
        return false;
    }
}

/* ============================================================================ end of the run */
static double num_after_key(const char *t, const char *key)   /* "key = 12.5" or "KEY=12.5", case-insensitive key */
{
    const char *p = strcasestr(t, key);
    if (!p) return NAN;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == '\'' || *p == '"') p++;
    char *e; double v = strtod(p, &e);
    return e == p ? NAN : v;
}

/* the text of the top-level block [name] of a MOOSE input (nested blocks included), malloc'd */
static char *moose_block(const char *t, const char *name)
{
    char open[96]; snprintf(open, sizeof open, "[%s]", name);
    for (const char *p = t; p && *p; ) {
        const char *s = p; while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, open, strlen(open))) {
            int depth = 0; const char *q = p;
            while (q && *q) {
                const char *l = q; while (*l == ' ' || *l == '\t') l++;
                if (*l == '[') {
                    if (!strncmp(l, "[]", 2) || !strncmp(l, "[../]", 5)) { if (--depth == 0) { const char *e = strchr(q, '\n'); return pl_strndup(p, (size_t)((e ? e : q + strlen(q)) - p)); } }
                    else depth++;
                }
                q = strchr(q, '\n'); if (q) q++;
            }
            return pl_strdup(p);
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return NULL;
}

/* the value of key in a MOOSE block's own lines (not nested sub-blocks) */
static double moose_key(const char *blk, const char *key)
{
    int depth = 0;
    for (const char *p = blk; p && *p; ) {
        const char *l = p; while (*l == ' ' || *l == '\t') l++;
        if (*l == '[') { if (!strncmp(l, "[]", 2) || !strncmp(l, "[../]", 5)) depth--; else depth++; }
        else if (depth == 1 && !strncmp(l, key, strlen(key))) {
            const char *q = l + strlen(key); while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') { q++; while (*q == ' ' || *q == '\t' || *q == '\'') q++; char *e; double v = strtod(q, &e); if (e != q) return v; }
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return NAN;
}

static double calculix_end(const char *t)
{
    static const char *const PROC[] = { "*HEAT TRANSFER", "*STATIC", "*DYNAMIC", "*COUPLED TEMPERATURE-DISPLACEMENT",
                                        "*UNCOUPLED TEMPERATURE-DISPLACEMENT", "*VISCO", "*CREEP", "*MODAL DYNAMIC", NULL };
    double total = 0; bool any = false;
    for (const char *p = t; p && *p; ) {
        bool proc = false;
        if (*p == '*' && p[1] != '*') for (int i = 0; PROC[i]; i++) if (line_starts_ci(p, PROC[i])) proc = true;
        p = strchr(p, '\n'); if (p) p++;
        if (!proc) continue;
        /* the data line: initial increment, step period, ... (the period defaults to 1) */
        while (p && *p && p[0] == '*' && p[1] == '*') { p = strchr(p, '\n'); if (p) p++; }
        double period = 1;
        if (p && *p && *p != '*') {
            const char *c = strchr(p, ','), *nl = strchr(p, '\n');
            if (c && (!nl || c < nl)) { char *e; double v = strtod(c + 1, &e); if (e != c + 1 && v > 0) period = v; }
        }
        total += period; any = true;
    }
    return any ? total : 0;
}

static double aster_end(const char *dir, const char *input)
{
    /* the .comm the study runs: from the .export's "F comm", else the input itself */
    char comm[PL_PATH] = "";
    char ip[PL_PATH]; pl_join(ip, sizeof ip, dir, input);
    if (pl_ends_with(input, ".comm")) pl_strlcpy(comm, ip, sizeof comm);
    else {
        char *t = pl_read_head(ip, 16384);
        for (char *l = t; l && *l; ) {
            char a[16], b[16], path[PL_PATH];
            if (sscanf(l, "%15s %15s %1023s", a, b, path) == 3 && !strcmp(a, "F") && !strcmp(b, "comm")) {
                if (path[0] == '/') pl_strlcpy(comm, path, sizeof comm); else pl_join(comm, sizeof comm, dir, path);
                if (!pl_is_file(comm)) pl_join(comm, sizeof comm, dir, pl_basename(path));
                break;
            }
            l = strchr(l, '\n'); if (l) l++;
        }
        free(t);
    }
    char *t = comm[0] ? pl_read_head(comm, 1 << 20) : NULL;
    double end = 0;
    for (const char *p = t; p && (p = strstr(p, "JUSQU_A")); p += 7) { double v = num_after_key(p, "JUSQU_A"); if (v > end) end = v; }
    for (const char *p = t; p && (p = strstr(p, "INST_FIN")); p += 8) { double v = num_after_key(p, "INST_FIN"); if (v > end) end = v; }
    free(t);
    return end;
}

double prog_end(int prog, const char *dir, const char *input, bool *isSteps)
{
    *isSteps = false;
    if (!input || !*input) return 0;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, input);
    double end = 0;
    switch (prog) {
    case PROG_CALCULIX: { char *t = read_some(p, 1 << 20); if (t) end = calculix_end(t); free(t); break; }
    case PROG_MOOSE: {
        char *t = pl_read_head(p, 1 << 20);
        char *b = t ? moose_block(t, "Executioner") : NULL;
        if (b) {
            double e = moose_key(b, "end_time"), n = moose_key(b, "num_steps");
            if (isfinite(e)) end = e;
            else if (isfinite(n)) { end = n; *isSteps = true; }
        }
        free(b); free(t);
        break;
    }
    case PROG_ELMER: {
        char *t = pl_read_head(p, 1 << 20);
        if (t) {
            const char *q = strcasestr(t, "Timestep Intervals");
            if (q) {
                q += strlen("Timestep Intervals");
                if (*q == '(') { q = strchr(q, ')'); if (q) q++; }
                if (q) { while (*q == ' ' || *q == '=') q++; double s = 0; char *e;
                         for (double v = strtod(q, &e); e != q; q = e, v = strtod(q, &e)) s += v;
                         end = s; *isSteps = true; }
            }
        }
        free(t);
        break;
    }
    case PROG_ASTER: end = aster_end(dir, input); break;
    case PROG_SATURNE: {
        char x[PL_PATH]; pl_join(x, sizeof x, dir, "DATA/setup.xml");
        char *t = pl_read_head(x, 1 << 20);
        const char *tp = t ? strstr(t, "<time_parameters") : NULL;
        if (tp) { const char *it = strstr(tp, "<iterations>"); if (it) { end = atof(it + 12); *isSteps = true; } }
        free(t);
        break;
    }
    case PROG_SPARTA: {
        char *t = pl_read_head(p, 1 << 20);
        double s = 0;
        for (const char *l = t; l && *l; ) {
            const char *q = l; while (*q == ' ' || *q == '\t') q++;
            if (!strncmp(q, "run", 3) && (q[3] == ' ' || q[3] == '\t')) s += atof(q + 4);
            l = strchr(l, '\n'); if (l) l++;
        }
        free(t);
        end = s; *isSteps = true;
        break;
    }
    default: break;
    }
    return end;
}

/* ============================================================================ building (DEME, Chrono) */
/* the newest source file of a project: its top folder and src/, include/ (the starters keep their drivers in src/) */
static time_t newest_source(const char *dir)
{
    static const char *const SUB[] = { "", "src", "include" };
    time_t t = 0;
    for (int s = 0; s < 3; s++) {
        char d[PL_PATH]; if (SUB[s][0]) pl_join(d, sizeof d, dir, SUB[s]); else pl_strlcpy(d, dir, sizeof d);
        NameList l = { 0 };
        pl_list_dir(d, &l, false, true);
        for (int i = 0; i < l.n; i++) {
            const char *e = ext_of(l.names[i]);
            if (strcmp(e, ".cpp") && strcmp(e, ".cu") && strcmp(e, ".h") && strcmp(e, ".hpp") && strcmp(e, ".cuh") && strcmp(l.names[i], "CMakeLists.txt")) continue;
            char p[PL_PATH]; pl_join(p, sizeof p, d, l.names[i]);
            time_t m = pl_mtime(p); if (m > t) t = m;
        }
        names_free(&l);
    }
    return t;
}

bool prog_built(int prog, const char *dir, const char *target, bool *stale)
{
    (void)prog;
    if (stale) *stale = false;
    char p[PL_PATH], b[PL_PATH]; pl_join(b, sizeof b, dir, "build"); pl_join(p, sizeof p, b, target);
    if (!pl_is_exec(p)) return false;
    if (stale) *stale = newest_source(dir) > pl_mtime(p);
    return true;
}

void prog_run_dir(int prog, const char *dir, char *out, int cap)
{
    if (prog == PROG_DEME || prog == PROG_CHRONO) pl_join(out, (size_t)cap, dir, "run");
    else pl_strlcpy(out, dir, (size_t)cap);
}

void prog_log_name(int prog, const char *input, char *out, int cap)
{
    char stem[128]; stem_of(input ? input : "", stem, sizeof stem);
    if (prog == PROG_SATURNE) pl_strlcpy(stem, "case", sizeof stem);
    for (char *c = stem; *c; c++) if (*c == '/' || *c == ' ') *c = '_';
    snprintf(out, (size_t)cap, "log.%s%s%s", prog_info(prog)->key, stem[0] ? "." : "", stem);
}

/* ============================================================================ run steps */
static void finish_cmd(char *cmd, int cap, const char *body)
{
    /* the exit code goes into the log, so a finished run reads as finished (or failed) without the app watching */
    snprintf(cmd, (size_t)cap, "%s\nrc=$?\necho \"Q-Continuum: exit $rc\"\nexit $rc", body);
}

static void mpi_launcher(const char *exe, char *out, int cap)
{
    out[0] = 0;
    /* MOOSE from its conda environment wants that environment's mpiexec */
    char d[PL_PATH]; snprintf(d, sizeof d, "%s/projects/", pl_home());
    if (pl_starts_with(exe, d)) {
        char m[PL_PATH]; snprintf(m, sizeof m, "%s/miniforge/envs/moose/bin/mpiexec", pl_home());
        if (pl_is_exec(m)) { pl_strlcpy(out, m, (size_t)cap); return; }
    }
    if (!pl_which("mpirun", out, (size_t)cap)) pl_which("mpiexec", out, (size_t)cap);
}

static bool write_export_for(const char *dir, const char *comm, int procs, char *exportRel, int cap)
{
    char stem[128]; stem_of(comm, stem, sizeof stem);
    snprintf(exportRel, (size_t)cap, "%s.export", stem);
    char ep[PL_PATH]; pl_join(ep, sizeof ep, dir, exportRel);
    Str b; str_init(&b);
    char cp[PL_PATH]; pl_join(cp, sizeof cp, dir, comm);
    str_cat(&b, "P actions make_etude\nP memory_limit 2048\nP time_limit 86400\n");
    str_catf(&b, "P mpi_nbcpu 1\nP ncpus %d\n", procs > 0 ? procs : 1);
    str_catf(&b, "F comm %s D 1\n", cp);
    /* the mesh: <stem>.med, else the folder's only .med */
    char med[PL_PATH]; snprintf(med, sizeof med, "%s/%s.med", dir, stem);
    if (!pl_is_file(med)) {
        med[0] = 0;
        const NameList *l = pl_list_dir_cached(dir, false, true);
        int n = 0; for (int i = 0; i < l->n; i++) if (pl_ends_with(l->names[i], ".med")) { n++; pl_join(med, sizeof med, dir, l->names[i]); }
        if (n != 1) med[0] = 0;
    }
    char mail[PL_PATH]; snprintf(mail, sizeof mail, "%s/%s.mail", dir, stem);
    if (med[0]) str_catf(&b, "F mmed %s D 20\n", med);
    else if (pl_is_file(mail)) str_catf(&b, "F mail %s D 20\n", mail);
    str_catf(&b, "F mess %s/%s.mess R 6\nF rmed %s/%s.rmed R 80\n", dir, stem, dir, stem);
    bool ok = pl_write_file(ep, b.s, b.len);
    str_free(&b);
    return ok;
}

int prog_run_steps(int prog, const char *dir, const char *input, int procs, bool rebuild, ProgStep *st, int max, char *err, int errCap)
{
    err[0] = 0;
    if (max < 2) return 0;
    char exe[PL_PATH], why[300];
    if (!prog_exe(prog, exe, sizeof exe, why, sizeof why)) { snprintf(err, (size_t)errCap, "%s", why); return 0; }
    if (!input || !*input) { snprintf(err, (size_t)errCap, "Nothing to run: the project has no %s.", prog_info(prog)->inputWord); return 0; }
    if (procs < 1) procs = 1;
    char qe[PL_PATH + 8], qi[PL_PATH + 8], body[8000], log[96];
    pl_shell_quote(qe, sizeof qe, exe);
    pl_shell_quote(qi, sizeof qi, input);
    prog_log_name(prog, input, log, sizeof log);
    int n = 0;
    char mpi[PL_PATH] = "", qm[PL_PATH + 8] = "";
    if (INFO[prog].mpi && procs > 1) { mpi_launcher(exe, mpi, sizeof mpi); pl_shell_quote(qm, sizeof qm, mpi); }
    bool par = mpi[0] != 0;
    switch (prog) {
    case PROG_DEME: case PROG_CHRONO: {
        bool stale = false, built = prog_built(prog, dir, input, &stale);
        if (!built || stale || rebuild) {
            ProgStep *b = &st[n++];
            memset(b, 0, sizeof *b);
            pl_strlcpy(b->id, "build", sizeof b->id);
            pl_strlcpy(b->label, built ? "Rebuild" : "Build", sizeof b->label);
            pl_strlcpy(b->log, "log.build", sizeof b->log);
            char qd[PL_PATH + 8]; pl_shell_quote(qd, sizeof qd, exe);
            int jobs = (int)sysconf(_SC_NPROCESSORS_ONLN); if (jobs > 4) jobs = 4; if (jobs < 1) jobs = 1;   /* CUDA compiles take ~2 GB each */
            if (prog == PROG_DEME) {
                char nv[PL_PATH], cb[PL_PATH] = "", qc[PL_PATH + 8]; nvcc_path(nv, sizeof nv);
                if (nv[0]) pl_dirname(cb, sizeof cb, nv);
                pl_shell_quote(qc, sizeof qc, cb);
                char arch[16] = "";
                const char *ae = getenv("QC_CUDA_ARCH"); if (ae) pl_strlcpy(arch, ae, sizeof arch);
                snprintf(body, sizeof body,
                         "export PATH=%s${PATH:+:$PATH}\n"
                         "arch=%s; [ -z \"$arch\" ] && arch=$( { nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null || "
                         "/usr/lib/wsl/lib/nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null; } | head -1 | tr -d ' .')\n"
                         "if [ ! -f build/CMakeCache.txt ]; then cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDEME_DIR=%s "
                         "${arch:+-DCMAKE_CUDA_ARCHITECTURES=$arch} || exit 1; fi\n"
                         "cmake --build build -j%d", qc, arch, qd, jobs);
            } else {
                snprintf(body, sizeof body,
                         "if [ ! -f build/CMakeCache.txt ]; then cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DChrono_DIR=%s || exit 1; fi\n"
                         "cmake --build build -j%d", qd, jobs);
            }
            finish_cmd(b->cmd, sizeof b->cmd, body);
        }
        ProgStep *r = &st[n++];
        memset(r, 0, sizeof *r);
        pl_strlcpy(r->id, "run", sizeof r->id);
        pl_strlcpy(r->label, input, sizeof r->label);
        pl_strlcpy(r->log, log, sizeof r->log);
        r->solver = true;
        snprintf(body, sizeof body, "mkdir -p run && cd run || exit 1\necho \"\\$ ../build/%s\"\n../build/%s", input, input);
        finish_cmd(r->cmd, sizeof r->cmd, body);
        return n;
    }
    case PROG_CALCULIX: {
        char job[128]; stem_of(input, job, sizeof job);
        char qj[200]; pl_shell_quote(qj, sizeof qj, job);
        snprintf(body, sizeof body, "export OMP_NUM_THREADS=%d\necho \"\\$ ccx -i %s   (%d thread%s)\"\n%s -i %s", procs, job, procs,
                 procs == 1 ? "" : "s", qe, qj);
        break;
    }
    case PROG_ASTER: {
        char ex[256];
        if (pl_ends_with(input, ".comm")) {
            if (!write_export_for(dir, input, procs, ex, sizeof ex)) { snprintf(err, (size_t)errCap, "Could not write %s.export", input); return 0; }
        } else pl_strlcpy(ex, input, sizeof ex);
        char qx[300]; pl_shell_quote(qx, sizeof qx, ex);
        snprintf(body, sizeof body, "echo \"\\$ run_aster %s\"\n%s %s", ex, qe, qx);
        break;
    }
    case PROG_SATURNE:
        /* code_saturne run waits for the solver, whose log is in RESU/<id>/: follow it into ours as it is written */
        snprintf(body, sizeof body,
                 "id=qc-$(date +%%Y%%m%%d-%%H%%M%%S)\ncd DATA || exit 1\necho \"\\$ code_saturne run --id $id%s\"\n"
                 "%s run --id \"$id\"%s &\npid=$!\n"
                 "( while [ ! -f \"../RESU/$id/run_solver.log\" ] && kill -0 $pid 2>/dev/null; do sleep 1; done\n"
                 "  grep -h \"nt_max:\" \"../RESU/$id/setup.log\" 2>/dev/null\n"
                 "  [ -f \"../RESU/$id/run_solver.log\" ] && exec tail -n +1 -F \"../RESU/$id/run_solver.log\" 2>/dev/null ) &\n"
                 "tp=$!\nwait $pid; rc=$?\nsleep 2; kill $tp 2>/dev/null; wait $tp 2>/dev/null\n"
                 "[ -f \"../RESU/$id/error\" ] && { echo \"Error (code_saturne):\"; head -40 \"../RESU/$id/error\"; }\n(exit $rc)",
                 procs > 1 ? " -n N" : "", qe, procs > 1 ? " -n $QC_PROCS" : "");
        {   /* -n N in the echo and $QC_PROCS in the command, so both read right */
            char tmp[8000]; pl_strlcpy(tmp, body, sizeof tmp);
            char np[16]; snprintf(np, sizeof np, "%d", procs);
            Str s; str_init(&s);
            for (const char *q = tmp; *q; ) {
                if (!strncmp(q, "$QC_PROCS", 9)) { str_cat(&s, np); q += 9; }
                else if (!strncmp(q, " -n N\"", 6)) { str_catf(&s, " -n %s\"", np); q += 6; }
                else { str_catn(&s, q, 1); q++; }
            }
            pl_strlcpy(body, s.s, sizeof body);
            str_free(&s);
        }
        break;
    case PROG_MOOSE:
        if (par) snprintf(body, sizeof body, "echo \"\\$ mpiexec -n %d %s -i %s\"\n%s -n %d %s -i %s --color off", procs, pl_basename(exe), input, qm, procs, qe, qi);
        else snprintf(body, sizeof body, "echo \"\\$ %s -i %s\"\n%s -i %s --color off", pl_basename(exe), input, qe, qi);
        break;
    case PROG_ELMER: {
        /* the mesh folder from a .grd with ElmerGrid when it is missing */
        char grd[256] = "", md[256] = "";
        char ip[PL_PATH]; pl_join(ip, sizeof ip, dir, input);
        char *t = pl_read_head(ip, 65536);
        const char *m = t ? strcasestr(t, "Mesh DB") : NULL;
        if (m) { const char *q = strchr(m, '"'); if (q) q = strchr(q + 1, '"'); if (q) q = strchr(q + 1, '"');
                 if (q) { int k = 0; q++; while (*q && *q != '"' && k < 255) md[k++] = *q++; md[k] = 0; } }
        free(t);
        char hdr[PL_PATH]; snprintf(hdr, sizeof hdr, "%s/%s/mesh.header", dir, md);
        if (md[0] && !pl_is_file(hdr)) {
            char g[PL_PATH]; snprintf(g, sizeof g, "%s/%s.grd", dir, md);
            if (pl_is_file(g)) snprintf(grd, sizeof grd, "%s.grd", md);
        }
        if (grd[0]) snprintf(body, sizeof body, "echo \"\\$ ElmerGrid 1 2 %s\"\nElmerGrid 1 2 %s || exit 1\necho \"\\$ ElmerSolver %s\"\n%s %s",
                             grd, grd, input, qe, qi);
        else snprintf(body, sizeof body, "echo \"\\$ ElmerSolver %s\"\n%s %s", input, qe, qi);
        break;
    }
    case PROG_SPARTA:
        if (par) snprintf(body, sizeof body, "echo \"\\$ mpirun -np %d spa_mpi -in %s\"\n%s -np %d %s -in %s -log none", procs, input, qm, procs, qe, qi);
        else snprintf(body, sizeof body, "echo \"\\$ spa_mpi -in %s\"\n%s -in %s -log none", input, qe, qi);
        break;
    default:
        snprintf(err, (size_t)errCap, "%s runs on the OpenFOAM pages.", prog_info(prog)->name);
        return 0;
    }
    ProgStep *r = &st[n++];
    memset(r, 0, sizeof *r);
    pl_strlcpy(r->id, "run", sizeof r->id);
    pl_strlcpy(r->label, INFO[prog].name, sizeof r->label);
    pl_strlcpy(r->log, log, sizeof r->log);
    r->solver = true;
    finish_cmd(r->cmd, sizeof r->cmd, body);
    return n;
}

/* ============================================================================ checks */
static void add_check(ProgCheck *out, int *n, int max, int sev, const char *file, int line, int fix, const char *fixLabel, const char *fmt, ...)
    __attribute__((format(printf, 9, 10)));
static void add_check(ProgCheck *out, int *n, int max, int sev, const char *file, int line, int fix, const char *fixLabel, const char *fmt, ...)
{
    if (*n >= max) return;
    ProgCheck *c = &out[(*n)++];
    memset(c, 0, sizeof *c);
    c->sev = sev; c->line = line; c->fix = fix;
    pl_strlcpy(c->file, file ? file : "", sizeof c->file);
    pl_strlcpy(c->fixLabel, fixLabel ? fixLabel : "", sizeof c->fixLabel);
    va_list ap; va_start(ap, fmt); vsnprintf(c->text, sizeof c->text, fmt, ap); va_end(ap);
}

/* the file a line of an input names: a relative path from the project, or an absolute one */
static bool exists_rel(const char *dir, const char *name)
{
    char p[PL_PATH];
    if (name[0] == '/') return pl_exists(name);
    pl_join(p, sizeof p, dir, name);
    return pl_exists(p);
}

static const char *skip_tok(const char *q)
{
    while (*q == ' ' || *q == '\t') q++;
    while (*q && !isspace((unsigned char)*q)) q++;
    return q;
}

static void word_after(const char *s, char *out, int cap)          /* the next token: quoted or up to blank/comma */
{
    while (*s == ' ' || *s == '\t' || *s == '=') s++;
    int k = 0;
    if (*s == '"' || *s == '\'') { char q = *s++; while (*s && *s != q && *s != '\n' && k < cap - 1) out[k++] = *s++; }
    else while (*s && !isspace((unsigned char)*s) && *s != ',' && k < cap - 1) out[k++] = *s++;
    out[k] = 0;
}

/* SPARTA's "-h" lists every style built in ("Compute styles: ...", "Dump styles: ..."). It takes a second (MPI starts),
   so it is kept in $XDG_CACHE_HOME/q-continuum/sparta-h.txt until spa_mpi changes. wait = false (the Environment panel)
   never blocks: a background child writes the cache and NULL is returned until it is there; wait = true (the pre-run
   check) runs it in place. */
static const char *sparta_help_ex(bool wait)
{
    static char *help;
    static time_t helpFor;
    static bool started;
    char exe[PL_PATH];
    if (!prog_exe(PROG_SPARTA, exe, sizeof exe, NULL, 0)) return "";
    time_t m = pl_mtime(exe);
    if (help && helpFor == m) return help;
    char cache[PL_PATH], dir[PL_PATH];
    const char *xc = getenv("XDG_CACHE_HOME");
    if (xc && *xc) pl_join(dir, sizeof dir, xc, "q-continuum"); else snprintf(dir, sizeof dir, "%s/.cache/q-continuum", pl_home());
    pl_join(cache, sizeof cache, dir, "sparta-h.txt");
    if (pl_is_file(cache) && pl_mtime(cache) >= m) {
        free(help); help = pl_read_file(cache, NULL); helpFor = m;
        if (help) return help;
    }
    char q[PL_PATH + 8], qc[PL_PATH + 8], cmd[PL_PATH * 3 + 256];
    pl_shell_quote(q, sizeof q, exe); pl_shell_quote(qc, sizeof qc, cache);
    pl_mkdirs(dir);
    snprintf(cmd, sizeof cmd, "cd /tmp && t=%s.tmp.$$; %s -h -log none 2>/dev/null </dev/null > \"$t\"; "   /* -h exits non-zero */
             "if grep -q 'Compute styles' \"$t\"; then mv -f \"$t\" %s; else rm -f \"$t\"; fi", qc, q, qc);
    if (wait) {
        pl_run_wait(cmd, NULL, 0);
        free(help); help = pl_read_file(cache, NULL); helpFor = m;
        return help ? help : "";
    }
    if (!started) {
        started = true;
        pid_t p = fork();
        if (p == 0) {                                   /* a grandchild does it, so nothing is left to reap */
            if (fork() == 0) { setsid(); execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
            _exit(0);
        }
        if (p > 0) waitpid(p, NULL, 0);
    }
    return NULL;
}
static const char *sparta_help(void) { return sparta_help_ex(true); }

/* is this style in SPARTA's list for its kind ("Compute", "Dump", "Fix", "Command")? true when the list can't be read */
bool prog_sparta_has(const char *kind, const char *style)
{
    const char *h = sparta_help();
    char head[64]; snprintf(head, sizeof head, "%s styles", kind);
    const char *l = strstr(h, head);
    if (!l) return true;
    const char *e = strchr(l, '\n'); if (!e) e = l + strlen(l);
    size_t k = strlen(style);
    for (const char *p = strchr(l, ':'); p && p < e; p++)
        if ((p[-1] == ' ' || p[-1] == ':') && !strncmp(p, style, k) && (p[k] == ' ' || p[k] == '\n' || p[k] == 0 || p + k == e)) return true;
    return false;
}

/* the optional SPARTA packages this build lacks, e.g. "FFT, VTK, PYTHON" ("" when all are in or SPARTA isn't found) */
bool prog_sparta_missing(char *out, int cap)
{
    out[0] = 0;
    const char *h = sparta_help_ex(false);
    if (!h) return false;                              /* still being read */
    if (!*h) return true;
    struct { const char *kind, *style, *pkg; } P[] = { { "Compute", "fft/grid", "FFT" }, { "Dump", "particle/vtk", "VTK" },
                                                      { "Command", "python", "PYTHON" }, { "Fix", "emit/face/kk", "KOKKOS" } };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (!prog_sparta_has(P[i].kind, P[i].style)) pl_fmt_ui(out + strlen(out), (size_t)cap - strlen(out), "%s%s", out[0] ? ", " : "", P[i].pkg);
    return true;
}

int prog_checks(int prog, const char *dir, const char *input, ProgCheck *out, int max)
{
    int n = 0;
    char exe[PL_PATH], why[300];
    if (!prog_exe(prog, exe, sizeof exe, why, sizeof why)) add_check(out, &n, max, 2, NULL, 0, 0, NULL, "%s", why);
    if (!input || !*input) { add_check(out, &n, max, 2, NULL, 0, 0, NULL, "The project has no %s to run.", prog_info(prog)->inputWord); return n; }
    char ip[PL_PATH]; pl_join(ip, sizeof ip, dir, input);
    if (prog != PROG_DEME && prog != PROG_CHRONO && !pl_is_file(ip)) { add_check(out, &n, max, 2, input, 0, 0, NULL, "%s is missing.", input); return n; }
    char *t = (prog == PROG_DEME || prog == PROG_CHRONO) ? NULL : pl_read_head(ip, 4 << 20);
    int line = 1;
    switch (prog) {
    case PROG_DEME: case PROG_CHRONO: {
        bool stale = false;
        if (!prog_built(prog, dir, input, &stale))
            add_check(out, &n, max, 1, "CMakeLists.txt", 0, PFIX_BUILD, "Build now", "%s is not built yet: Run builds it first (a few minutes the first time).", input);
        else if (stale) add_check(out, &n, max, 1, NULL, 0, PFIX_BUILD, "Rebuild now", "The source changed since %s was built: Run rebuilds it first.", input);
        char pp[PL_PATH]; pl_join(pp, sizeof pp, dir, "params.ini");
        if (prog == PROG_DEME && !pl_is_file(pp)) add_check(out, &n, max, 1, NULL, 0, 0, NULL, "No params.ini: the driver runs on its built-in values.");
        break;
    }
    case PROG_CALCULIX:
        for (const char *l = t; l && *l; line++) {
            if (line_starts_ci(l, "*INCLUDE")) {
                const char *q = strcasestr(l, "INPUT");
                if (q) { char f[256]; word_after(q + 5, f, sizeof f);
                         if (f[0] && !exists_rel(dir, f)) add_check(out, &n, max, 2, input, line, 0, NULL, "*INCLUDE names %s, which is not in the folder.", f); }
            }
            l = strchr(l, '\n'); if (l) l++;
        }
        break;
    case PROG_ASTER:
        if (pl_ends_with(input, ".export")) {
            bool foreign = false;
            for (const char *l = t; l && *l; line++) {
                char a[8], k[16], path[PL_PATH], io[8];
                if (sscanf(l, "%7s %15s %1023s %7s", a, k, path, io) == 4 && !strcmp(a, "F")) {
                    bool data = strchr(io, 'D') != NULL;
                    char pd[PL_PATH]; pl_dirname(pd, sizeof pd, path);
                    bool ok = data ? exists_rel(dir, path) : (path[0] != '/' || pl_is_dir(pd));
                    if (!ok) {
                        char here[PL_PATH]; pl_join(here, sizeof here, dir, pl_basename(path));
                        bool fixable = !data || pl_is_file(here);
                        if (fixable) foreign = true;
                        add_check(out, &n, max, 2, input, line, fixable ? PFIX_EXPORT_PATHS : 0, fixable ? "Use this folder" : NULL,
                                  "%s (%s) is %s.", path, k, data ? "missing" : "in a folder that does not exist");
                    } else if (path[0] == '/') {
                        char d2[PL_PATH]; pl_dirname(d2, sizeof d2, path);
                        if (strcmp(d2, dir) && !data) foreign = true;
                    }
                }
                l = strchr(l, '\n'); if (l) l++;
            }
            (void)foreign;
        } else add_check(out, &n, max, 1, input, 0, PFIX_WRITE_EXPORT, "Write it now",
                         "No .export runs %s: Run writes %.*s.export next to it (mesh, message and results files).", input,
                         (int)(strlen(input) - 5), input);
        break;
    case PROG_MOOSE: {
        char *b = t ? moose_block(t, "Mesh") : NULL;
        for (const char *l = b; l && *l; ) {
            const char *s = l; while (*s == ' ' || *s == '\t') s++;
            if (!strncmp(s, "file", 4) && (s[4] == ' ' || s[4] == '=')) {
                char f[256]; word_after(s + 4, f, sizeof f);
                if (f[0] && !exists_rel(dir, f)) add_check(out, &n, max, 2, input, 0, 0, NULL, "The mesh file %s is not in the folder.", f);
            }
            l = strchr(l, '\n'); if (l) l++;
        }
        free(b);
        break;
    }
    case PROG_ELMER: {
        const char *m = t ? strcasestr(t, "Mesh DB") : NULL;
        if (m) {
            char a[256] = "", b[256] = "";
            const char *q = m + 7; word_after(q, a, sizeof a);
            const char *q2 = strchr(q + 1, '"'); if (q2) q2 = strchr(q2 + 1, '"');
            if (q2) word_after(q2 + 1, b, sizeof b);
            char md[PL_PATH]; snprintf(md, sizeof md, "%s/%s/%s/mesh.header", dir, a[0] ? a : ".", b);
            if (b[0] && !pl_is_file(md)) {
                bool grd = false; const NameList *l = pl_list_dir_cached(dir, false, true);
                for (int i = 0; i < l->n; i++) if (pl_ends_with(l->names[i], ".grd")) grd = true;
                char g[PL_PATH]; snprintf(g, sizeof g, "%s/%s.grd", dir, b);
                if (pl_is_file(g)) add_check(out, &n, max, 1, input, 0, 0, NULL, "No mesh yet: Run makes it from %s.grd with ElmerGrid first.", b);
                else add_check(out, &n, max, 2, input, 0, 0, NULL, "The mesh folder %s has no mesh.header%s.", b,
                               grd ? ": make it with ElmerGrid from the .grd file" : "");
            }
        }
        break;
    }
    case PROG_SPARTA: {
        static const char *const CMDS[] = { "read_surf", "species", "collide", "read_grid", "react", NULL };
        for (const char *l = t; l && *l; line++) {
            const char *s = l; while (*s == ' ' || *s == '\t') s++;
            for (int i = 0; CMDS[i]; i++) {
                size_t k = strlen(CMDS[i]);
                if (!strncmp(s, CMDS[i], k) && (s[k] == ' ' || s[k] == '\t')) {
                    char f[256]; const char *q = s + k;
                    if (!strcmp(CMDS[i], "collide")) q = skip_tok(skip_tok(q));       /* collide vss <mixture> <file> */
                    word_after(q, f, sizeof f);
                    if (!strcmp(CMDS[i], "react") && (!strcmp(f, "none") || !f[0])) continue;
                    if (f[0] && f[0] != '$' && strchr(f, '.') && !exists_rel(dir, f))
                        add_check(out, &n, max, 2, input, line, 0, NULL, "%s names %s, which is not in the folder.", CMDS[i], f);
                }
            }
            /* a style this SPARTA wasn't built with: "compute ID style", "dump ID style", "fix ID style", python */
            static const char *const KIND[][2] = { { "compute", "Compute" }, { "dump", "Dump" }, { "fix", "Fix" } };
            for (int i = 0; i < 3; i++) {
                size_t k = strlen(KIND[i][0]);
                if (strncmp(s, KIND[i][0], k) || (s[k] != ' ' && s[k] != '\t')) continue;
                char st[64]; word_after(skip_tok(s + k), st, sizeof st);
                if (st[0] && st[0] != '$' && !prog_sparta_has(KIND[i][1], st)) {
                    const char *pkg = !strcmp(st, "fft/grid") ? "FFT" : strstr(st, "vtk") ? "VTK" : strstr(st, "/kk") ? "KOKKOS" : NULL;
                    char lp[16] = ""; for (int j = 0; pkg && pkg[j] && j < 15; j++) { lp[j] = (char)tolower((unsigned char)pkg[j]); lp[j + 1] = 0; }
                    if (pkg) add_check(out, &n, max, 2, input, line, 0, NULL, "This SPARTA was built without the %s package, which the %s style %s needs: "
                                       "rebuild it in ~/src/sparta/src with make yes-%s && make mpi, or take the line out.", pkg, KIND[i][0], st, lp);
                    else add_check(out, &n, max, 2, input, line, 0, NULL, "This SPARTA has no %s style %s (spa_mpi -h lists what it has).", KIND[i][0], st);
                }
            }
            if ((!strncmp(s, "python", 6) && (s[6] == ' ' || s[6] == '\t')) || (!strncmp(s, "variable", 8) && strstr(s, " python ")))
                if (!prog_sparta_has("Command", "python") && !strstr(sparta_help(), "PYTHON"))
                    add_check(out, &n, max, 2, input, line, 0, NULL, "This SPARTA was built without Python embedded (the PYTHON package), which this "
                              "line needs: rebuild it with make yes-python && make mpi, or take the line out.");
            l = strchr(l, '\n'); if (l) l++;
        }
        break;
    }
    case PROG_SATURNE: {
        const char *m = t ? strstr(t, "<mesh name=\"") : NULL;
        if (m) {
            char f[256]; int k = 0; m += 12; while (*m && *m != '"' && k < 255) f[k++] = *m++; f[k] = 0;
            char p1[PL_PATH], p2[PL_PATH];
            snprintf(p1, sizeof p1, "%s/MESH/%s", dir, f); snprintf(p2, sizeof p2, "%s/DATA/%s", dir, f);
            const char *mi = strstr(t, "<meshdir name=\"");
            bool found = pl_exists(p1) || pl_exists(p2) || f[0] == '/' ? pl_exists(f) || pl_exists(p1) || pl_exists(p2) : false;
            if (mi) found = true;              /* a mesh directory set in the GUI: trust it */
            if (!found) add_check(out, &n, max, 1, input, 0, 0, NULL, "The mesh %s was not found in MESH/ (or it comes from a user function).", f);
        }
        break;
    }
    default: break;
    }
    free(t);
    return n;
}

bool prog_apply_fix(int prog, const char *dir, const char *input, int fix, char *msg, int cap)
{
    msg[0] = 0;
    if (fix == PFIX_WRITE_EXPORT && prog == PROG_ASTER) {
        char ex[256];
        if (!write_export_for(dir, input, 1, ex, sizeof ex)) { snprintf(msg, (size_t)cap, "Could not write the .export"); return false; }
        snprintf(msg, (size_t)cap, "Wrote %s", ex);
        return true;
    }
    if (fix == PFIX_EXPORT_PATHS && prog == PROG_ASTER) {
        char absd[PL_PATH];
        if (!realpath(dir, absd)) pl_strlcpy(absd, dir, sizeof absd);
        dir = absd;
        char ip[PL_PATH]; pl_join(ip, sizeof ip, dir, input);
        char *t = pl_read_file(ip, NULL);
        if (!t) { snprintf(msg, (size_t)cap, "Could not read %s", input); return false; }
        Str b; str_init(&b);
        int changed = 0;
        for (char *l = t; *l; ) {
            char *e = strchr(l, '\n'); size_t len = e ? (size_t)(e - l) : strlen(l);
            char line[PL_PATH + 64]; size_t ll = len < sizeof line - 1 ? len : sizeof line - 1;
            memcpy(line, l, ll); line[ll] = 0;
            char a[8], k[16], path[PL_PATH], io[8], rest[64] = "";
            int got = sscanf(line, "%7s %15s %1023s %7s %63[^\n]", a, k, path, io, rest);
            if (got >= 4 && !strcmp(a, "F") && path[0] == '/') {
                char pd[PL_PATH]; pl_dirname(pd, sizeof pd, path);
                bool data = strchr(io, 'D') != NULL;
                bool bad = data ? !pl_exists(path) : !pl_is_dir(pd);
                char here[PL_PATH]; pl_join(here, sizeof here, dir, pl_basename(path));
                if (bad && (!data || pl_is_file(here))) {
                    str_catf(&b, "F %s %s %s%s%s\n", k, here, io, got == 5 ? " " : "", got == 5 ? rest : "");
                    changed++;
                    l = e ? e + 1 : l + len;
                    continue;
                }
            }
            str_catn(&b, l, len); str_cat(&b, "\n");
            l = e ? e + 1 : l + len;
        }
        bool ok = changed && pl_write_file(ip, b.s, b.len);
        str_free(&b); free(t);
        snprintf(msg, (size_t)cap, ok ? "%d path%s now point into this folder" : "Nothing to change", changed, changed == 1 ? "" : "s");
        return ok;
    }
    (void)prog;
    snprintf(msg, (size_t)cap, "No automatic fix for that");
    return false;
}

/* ============================================================================ results */
const char *prog_result_kind_name(int k)
{
    switch (k) {
    case RK_PARAVIEW: return "ParaView";
    case RK_PARTICLES: return "Particles";
    case RK_TABLE: return "Table";
    case RK_TEXT: return "Text";
    case RK_IMAGE: return "Image";
    case RK_CGX: return "CalculiX results";
    case RK_FOLDER: return "Folder";
    case RK_MED: return "MED (SALOME)";
    default: return "File";
    }
}

static bool particle_csv(const char *p)
{
    char *h = pl_read_head(p, 512);
    bool ok = h && (strstr(h, "X,") || strstr(h, "X ,") || strstr(h, ",X")) && (strstr(h, ",Y") || strstr(h, "Y,"));
    free(h);
    return ok;
}

static bool sparta_dump(const char *p)
{
    char *h = pl_read_head(p, 400);
    bool ok = h && pl_starts_with(h, "ITEM: TIMESTEP") && strstr(h, "ITEM: ATOMS") && strstr(h, " x ");
    free(h);
    return ok;
}

static void add_result(ProgResult *out, int *n, int max, const char *rel, int kind, const char *full, int count)
{
    if (*n >= max) return;
    ProgResult *r = &out[(*n)++];
    memset(r, 0, sizeof *r);
    pl_strlcpy(r->rel, rel, sizeof r->rel);
    r->kind = kind; r->count = count;
    struct stat st;
    if (stat(full, &st) == 0) { r->size = st.st_size; r->mtime = st.st_mtime; }
}

static bool is_input_mesh(const char *dir, const char *n)
{
    /* a code_aster .med named in an .export with D (data) is an input, not a result */
    const NameList *l = pl_list_dir_cached(dir, false, true);
    for (int i = 0; i < l->n; i++) {
        if (!pl_ends_with(l->names[i], ".export")) continue;
        char p[PL_PATH]; pl_join(p, sizeof p, dir, l->names[i]);
        char *t = pl_read_head(p, 16384);
        bool in = false;
        for (char *q = t; q && (q = strstr(q, n)); q += strlen(n)) {
            const char *e = q + strlen(n);
            while (*e == ' ') e++;
            if (*e == 'D') in = true;
        }
        free(t);
        if (in) return true;
    }
    return false;
}

static void results_in(int prog, const char *dir, const char *rel, ProgResult *out, int *n, int max, int depth)
{
    char d[PL_PATH]; if (rel[0]) pl_join(d, sizeof d, dir, rel); else pl_strlcpy(d, dir, sizeof d);
    NameList l = { 0 };
    pl_list_dir(d, &l, false, false);
    names_sort(&l);
    int csvParticles = 0, dumps = 0; char firstDump[128] = "";
    for (int i = 0; i < l.n && *n < max; i++) {
        const char *nm = l.names[i];
        if (nm[0] == '.') continue;
        char r[PL_PATH]; if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, nm); else pl_strlcpy(r, nm, sizeof r);
        char p[PL_PATH]; pl_join(p, sizeof p, dir, r);
        if (pl_is_dir(p)) {
            if (depth > 0 && strcmp(nm, "build") && strcmp(nm, "SRC") && strcmp(nm, "DATA") && strcmp(nm, "src") &&
                strcmp(nm, "__pycache__") && strcmp(nm, "mesh") && strcmp(nm, "CMakeFiles") && strcmp(nm, "checkpoint"))
                results_in(prog, dir, r, out, n, max, depth - 1);
            continue;
        }
        const char *e = ext_of(nm);
        if (!strcmp(e, ".csv") && particle_csv(p)) { csvParticles++; continue; }
        if (pl_starts_with(nm, "log.")) continue;
        if (pl_starts_with(nm, "dump") && sparta_dump(p)) { if (!dumps++) pl_strlcpy(firstDump, nm, sizeof firstDump); continue; }
        if (!strcmp(e, ".e") || !strcmp(e, ".exo") || !strcmp(e, ".vtu") || !strcmp(e, ".pvd") || !strcmp(e, ".vtk") ||
            !strcmp(e, ".vtp") || !strcmp(e, ".vtm") || !strcmp(e, ".case") || !strcmp(e, ".xdmf") || !strcmp(e, ".pvtu")) {
            if (!strcmp(e, ".vtu") && strstr(nm, "_t") && pl_is_file(p)) {   /* res_t0001.vtu ... next to res.pvd: list the .pvd only */
                char pvd[PL_PATH]; pl_strlcpy(pvd, p, sizeof pvd); char *u = strstr(pvd + strlen(d), "_t"); if (u) pl_strlcpy(u, ".pvd", sizeof pvd - (size_t)(u - pvd));
                if (pl_is_file(pvd)) continue;
            }
            add_result(out, n, max, r, RK_PARAVIEW, p, 0);
        }
        else if (!strcmp(e, ".frd")) add_result(out, n, max, r, RK_CGX, p, 0);
        else if ((!strcmp(e, ".rmed") || !strcmp(e, ".med")) && !is_input_mesh(d, nm)) add_result(out, n, max, r, RK_MED, p, 0);
        else if (!strcmp(e, ".csv") || (!strcmp(e, ".dat") && prog != PROG_SPARTA)) add_result(out, n, max, r, !strcmp(e, ".csv") ? RK_TABLE : RK_TEXT, p, 0);
        else if (!strcmp(e, ".sta") || !strcmp(e, ".cvg") || !strcmp(e, ".mess") || !strcmp(e, ".resu")) add_result(out, n, max, r, RK_TEXT, p, 0);
        else if (!strcmp(e, ".png") || !strcmp(e, ".jpg") || !strcmp(e, ".ppm") || !strcmp(e, ".gif") || !strcmp(e, ".mp4")) add_result(out, n, max, r, RK_IMAGE, p, 0);
    }
    names_free(&l);
    if (csvParticles >= 1) {
        char p[PL_PATH]; if (rel[0]) pl_join(p, sizeof p, dir, rel); else pl_strlcpy(p, dir, sizeof p);
        add_result(out, n, max, rel[0] ? rel : ".", RK_PARTICLES, p, csvParticles);
    }
    if (dumps >= 1) {
        char p[PL_PATH]; pl_join(p, sizeof p, d, firstDump);
        char pat[PL_PATH]; char *dot = strchr(firstDump, '.'); char pre[128]; pl_strlcpy(pre, firstDump, sizeof pre);
        if (dot) pre[dot - firstDump] = 0;
        if (rel[0]) snprintf(pat, sizeof pat, "%s/%s.*", rel, pre); else snprintf(pat, sizeof pat, "%s.*", pre);
        add_result(out, n, max, pat, RK_PARTICLES, p, dumps);
    }
}

static int by_mtime(const void *a, const void *b)
{
    const ProgResult *x = a, *y = b;
    if (x->kind != y->kind && (x->kind == RK_PARTICLES || y->kind == RK_PARTICLES)) return x->kind == RK_PARTICLES ? -1 : 1;
    return (y->mtime > x->mtime) - (y->mtime < x->mtime);
}

int prog_results(int prog, const char *dir, ProgResult *out, int max)
{
    int n = 0;
    results_in(prog, dir, "", out, &n, max, prog == PROG_SATURNE ? 4 : 3);
    qsort(out, (size_t)n, sizeof *out, by_mtime);
    return n;
}

/* ============================================================================ log lines */
static void strip_ansi(const char *in, char *out, int cap)
{
    int k = 0;
    for (const char *p = in; *p && k < cap - 1; p++) {
        if (*p == 27 && p[1] == '[') { p += 2; while (*p && !isalpha((unsigned char)*p)) p++; if (!*p) break; continue; }
        out[k++] = *p;
    }
    out[k] = 0;
}

enum { PS_NONE = 0, PS_ERROR, PS_TABLE, PS_CONV };

static bool parse_calculix(LogMon *m, const char *l)
{
    if (m->pstate == PS_ERROR) {
        if (!*l) { m->pstate = PS_NONE; m->fatalDone = true; return true; }
        logmon_fatal(m, l); return true;
    }
    if (pl_starts_with(l, "*ERROR")) { logmon_fatal(m, l + 1); m->pstate = PS_ERROR; return true; }
    if (pl_starts_with(l, "*WARNING")) { m->warnings++; pl_strlcpy(m->warnMsg, l + 1, sizeof m->warnMsg); return true; }
    if (pl_starts_with(l, "actual total time=")) { logmon_step(m, atof(l + 18)); return true; }
    static const struct { const char *key, *series; } V[] = {
        { "largest residual force=", "residual force" }, { "largest residual flux=", "residual flux" },
        { "largest increment of disp=", "disp increment" }, { "largest increment of temp=", "temp increment" }, { NULL, NULL } };
    for (int i = 0; V[i].key; i++) if (pl_starts_with(l, V[i].key)) { logmon_value(m, V[i].series, fabs(atof(l + strlen(V[i].key))), false); return true; }
    if (pl_starts_with(l, "Job finished")) { m->ended = true; return true; }
    if (pl_starts_with(l, "number of equations")) return true;
    return false;
}

static bool parse_moose(LogMon *m, const char *raw)
{
    char b[1024]; strip_ansi(raw, b, sizeof b);
    const char *l = b; while (*l == ' ') l++;
    if (m->pstate == PS_ERROR) {
        if (!*l) { if (m->fatalMsg[0]) { m->pstate = PS_NONE; m->fatalDone = true; } return true; }
        if (pl_starts_with(l, "Stack frames") || pl_starts_with(l, "application called MPI_Abort")) { m->pstate = PS_NONE; m->fatalDone = true; return true; }
        logmon_fatal(m, l); return true;
    }
    if (strstr(l, "*** ERROR ***")) { logmon_fatal(m, NULL); m->pstate = PS_ERROR; return true; }
    if (strstr(l, "*** Warning") || strstr(l, "*** WARNING")) { m->warnings++; return true; }
    if (pl_starts_with(l, "Time Step ")) {
        const char *t = strstr(l, "time = ");
        if (t && atoi(l + 10) > 0) logmon_step(m, atof(t + 7));
        return true;
    }
    const char *nr = strstr(l, "Nonlinear |R| = ");
    if (nr && isdigit((unsigned char)l[0])) {
        if (atoi(l) == 0) logmon_value(m, "Nonlinear |R|", atof(nr + 16), false);
        return true;
    }
    if (strstr(l, "Solve Did NOT Converge")) { m->warnings++; pl_strlcpy(m->warnMsg, "A nonlinear solve did not converge (MOOSE cuts the time step).", sizeof m->warnMsg); return true; }
    if (strstr(l, "Solve Converged")) return true;
    if (pl_starts_with(l, "Finished Executing")) { m->ended = true; return true; }
    return false;
}

static bool parse_elmer(LogMon *m, const char *l)
{
    if (pl_starts_with(l, "MAIN: Time: ")) {
        int i = 0, N = 0; double t = 0;
        if (sscanf(l + 12, "%d/%d:%lf", &i, &N, &t) >= 2) { if (N > 0) m->endSteps = N; logmon_step(m, t); }
        return true;
    }
    if (pl_starts_with(l, "ComputeChange: NS (ITER=")) {
        int it = atoi(l + 24);
        const char *p = strstr(l, "(NRM,RELC): (");
        const char *eq = strstr(l, ":: ");
        if (p && it == 1) {
            double nrm = 0, relc = 0;
            if (sscanf(p + 13, "%lf %lf", &nrm, &relc) == 2) logmon_value(m, eq ? eq + 3 : "change", relc, false);
        }
        return true;
    }
    if (strstr(l, "ERROR::")) { logmon_fatal(m, strstr(l, "ERROR::") + 7); m->fatalDone = true; return true; }
    if (strstr(l, "WARNING::")) { m->warnings++; pl_strlcpy(m->warnMsg, strstr(l, "WARNING::") + 9, sizeof m->warnMsg); return true; }
    if (strstr(l, "ALL DONE")) { m->ended = true; return true; }
    return false;
}

static bool parse_aster(LogMon *m, const char *l)
{
    /* messages are framed: "!------!" / "! <F> <ID> !" (older) or box drawing "║ <F> <ID> ║" (newer) */
    const char *s = l;
    if (m->pstate == PS_ERROR) {
        if (pl_starts_with(s, "!--") || pl_starts_with(s, "\xe2\x95\x9a") || (!*s && m->fatalMsg[0])) { m->pstate = PS_NONE; m->fatalDone = true; return true; }
        char t[600]; pl_strlcpy(t, s, sizeof t);
        char *q = t; if (*q == '!') q++; else if (!strncmp(q, "\xe2\x95\x91", 3)) q += 3;
        int k = (int)strlen(q); while (k > 0 && (q[k - 1] == ' ' || q[k - 1] == '!')) q[--k] = 0;
        if (k >= 3 && !strcmp(q + k - 3, "\xe2\x95\x91")) { q[k - 3] = 0; }
        pl_trim_inplace(q);
        if (*q) logmon_fatal(m, q);
        return true;
    }
    if (strstr(s, "<F>") || strstr(s, "<EXCEPTION>") || strstr(s, "<S>")) {
        if (strstr(s, "DIAGNOSTIC JOB")) { if (!m->fatal) logmon_fatal(m, s); m->fatalDone = true; return true; }
        if (!m->fatal) { logmon_fatal(m, NULL); m->pstate = PS_ERROR; }
        return true;
    }
    if (strstr(s, "<A>")) { m->warnings++; return true; }
    const char *in = strstr(s, "Instant de calcul");
    if (!in) in = strstr(s, "Computation time");
    if (in) { const char *c = strchr(in, ':'); if (c) logmon_step(m, atof(c + 1)); return true; }
    const char *at = strstr(s, "l'instant");
    if (at) { double t = atof(at + 9); if (m->steps == 0 || t > m->time) logmon_step(m, t); return true; }
    /* STAT_NON_LINE's Newton table: "|     0     |  1.2E-01 X|..." -> the first iteration's RESI_GLOB_RELA */
    if ((s[0] == '|' || !strncmp(s, "\xe2\x94\x82", 3)) && m->steps > 0) {
        const char *q = s[0] == '|' ? s + 1 : s + 3;
        char *e; long it = strtol(q, &e, 10);
        if (e != q) {
            const char *bar = strchr(e, '|'); if (!bar) bar = strstr(e, "\xe2\x94\x82");
            if (bar) { const char *v = bar + (*bar == '|' ? 1 : 3); char *e2; double r = strtod(v, &e2);
                       if (e2 != v && it == 0) logmon_value(m, "RESI_GLOB_RELA", r, false); }
        }
        return true;
    }
    if (strstr(s, "DIAGNOSTIC JOB : OK")) { m->ended = true; return true; }
    return false;
}

static bool parse_saturne(LogMon *m, const char *l)
{
    if (pl_starts_with(l, "INSTANT ")) {
        double t = atof(l + 8);
        const char *n = strstr(l, "TIME STEP NUMBER");
        logmon_step(m, t);
        if (n) { int k = atoi(n + 16); if (k > 0) m->steps = k; }   /* it logs every Nth step: the number is the count */
        m->pstate = PS_NONE;
        return true;
    }
    if (strstr(l, "Information on convergence")) { m->pstate = PS_CONV; return true; }
    if (pl_starts_with(l, "nt_max:")) { int nm = atoi(l + 7); if (nm > 0) m->endSteps = nm; return true; }
    if (m->pstate == PS_CONV) {
        if (pl_starts_with(l, "** ")) { m->pstate = PS_NONE; return false; }
        if (l[0] == 'c' && l[1] == ' ') {          /* "c  Temperature  2.67e+02  18  1.0e-13  7.9e-02  7.7e-04" */
            char name[64]; double rhs, nres, drift, tres; int nit;
            if (sscanf(l + 1, "%63s %lf %d %lf %lf %lf", name, &rhs, &nit, &nres, &drift, &tres) == 6) logmon_value(m, name, tres, false);
        }
        return true;
    }
    if (pl_starts_with(l, "Error") || strstr(l, "solver script exited with status") || pl_starts_with(l, "SIGFPE") || pl_starts_with(l, "SIGSEGV")) {
        logmon_fatal(m, l);
        if (!pl_starts_with(l, "Error (code_saturne)")) m->fatalDone = strstr(l, "exited") != NULL;
        return true;
    }
    if (m->fatal && !m->fatalDone) { if (!*l) { m->fatalDone = true; return true; } logmon_fatal(m, l); return true; }
    if (strstr(l, "END OF CALCULATION") || pl_starts_with(l, "Post-calculation operations")) { m->ended = true; return true; }
    return false;
}

static bool parse_sparta(LogMon *m, const char *l)
{
    if (pl_starts_with(l, "ERROR")) { logmon_fatal(m, l + 5 + (l[5] == ':')); m->fatalDone = true; return true; }
    if (pl_starts_with(l, "WARNING")) { m->warnings++; pl_strlcpy(m->warnMsg, l + 8, sizeof m->warnMsg); return true; }
    if (pl_starts_with(l, "Step ")) { m->pstate = PS_TABLE; return true; }
    if (m->pstate == PS_TABLE) {
        if (pl_starts_with(l, "Loop time of")) { m->pstate = PS_NONE; return true; }
        double v[8]; int k = 0; const char *q = l; char *e;
        while (k < 8) { v[k] = strtod(q, &e); if (e == q) break; q = e; k++; }
        while (*q == ' ' || *q == '\t') q++;
        if (k >= 3 && !*q) {
            logmon_step(m, v[0]);
            if (v[2] > 0) logmon_value(m, "particles", v[2], false);
            if (k >= 5 && v[4] > 0) logmon_value(m, "collisions", v[4], false);
            return true;
        }
        m->pstate = PS_NONE;
        return false;
    }
    if (pl_starts_with(l, "Total wall time")) { m->ended = true; return true; }
    return false;
}

static bool parse_driver(LogMon *m, const char *l)            /* DEME, Chrono: our drivers print "QC t=..." */
{
    if (pl_starts_with(l, "QC t=")) {
        double t = atof(l + 5);
        const char *e = strstr(l, " end="); if (e) m->endTime = atof(e + 5);
        logmon_step(m, t);
        const char *ke = strstr(l, " ke="); if (ke) logmon_value(m, "kinetic energy", atof(ke + 4), false);
        const char *vm = strstr(l, " vmax="); if (vm) logmon_value(m, "max speed", atof(vm + 6), false);
        return true;
    }
    if (pl_starts_with(l, "QC ")) return true;
    if (strstr(l, "terminate called")) { logmon_fatal(m, NULL); return true; }   /* the reason follows on the what() line */
    if (strstr(l, "what():") || strstr(l, "CUDA error") || strstr(l, "Segmentation fault") || strstr(l, "cudaError") ||
        strstr(l, "invalid force") || strstr(l, "out of memory")) {
        if (m->fatalDone) return true;
        m->fatalMsg[0] = 0;
        if (strstr(l, "invalid force") || strstr(l, "velocity"))
            logmon_fatal(m, "The contact forces blew up: the time step is too big for the stiffest contact (lower the time step on Setup).");
        else if (strstr(l, "out of memory")) logmon_fatal(m, "The GPU ran out of memory: fewer particles or a smaller domain.");
        else logmon_fatal(m, strstr(l, "what():") ? strstr(l, "what():") + 7 : l);
        m->fatalDone = true;
        return true;
    }
    return false;
}

bool prog_parse_line(LogMon *m, const char *line)
{
    switch (m->prog) {
    case PROG_CALCULIX: return parse_calculix(m, line);
    case PROG_MOOSE:    return parse_moose(m, line);
    case PROG_ELMER:    return parse_elmer(m, line);
    case PROG_ASTER:    return parse_aster(m, line);
    case PROG_SATURNE:  return parse_saturne(m, line);
    case PROG_SPARTA:   return parse_sparta(m, line);
    case PROG_DEME: case PROG_CHRONO: return parse_driver(m, line);
    default: return false;
    }
}

/* ============================================================================ templates */
const ProgTemplate PROG_TEMPLATES[] = {
    { PROG_DEME,     "settle",  "Settle a bed",          "Spheres poured into a box settle under gravity (lunar or Earth)." },
    { PROG_DEME,     "drum",    "Rotating drum",         "A drum turns a bed of grains: angle of repose, mixing." },
    { PROG_DEME,     "hopper",  "Hopper discharge",      "Grains drain through an orifice: the discharge rate." },
    { PROG_DEME,     "impact",  "Impact into a bed",     "A projectile hits a settled bed: crater and ejecta." },
    { PROG_CHRONO,   "pendulum", "Swinging pendulum",    "A rod on a hinge released from an angle: its period against theory, energy kept." },
    { PROG_CALCULIX, "beam",    "Cantilever beam",       "Steel beam fixed at one end, a tip load: static stress and deflection." },
    { PROG_CALCULIX, "slab",    "Heated slab",           "Transient heat conduction in a steel slab, one face held hot." },
    { PROG_MOOSE,    "heat",    "Heat conduction",       "2-D transient conduction in a steel plate (heat_transfer module)." },
    { PROG_ELMER,    "heat",    "Heat equation",         "Steady heat conduction in a square with a heat source." },
    { PROG_SPARTA,   "plume",   "Flow over a wedge",     "Rarefied (DSMC) supersonic nitrogen flow over a wedge, 2-D." },
    { PROG_ASTER,    "slab",    "Thermal slab",          "Transient linear heat conduction (THER_LINEAIRE) on a generated mesh." },
};
const int PROG_N_TEMPLATES = (int)(sizeof PROG_TEMPLATES / sizeof PROG_TEMPLATES[0]);

static bool put(const char *dir, const char *name, const char *text, char *err, int cap)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, name);
    if (!pl_write_file(p, text, strlen(text))) { snprintf(err, (size_t)cap, "Could not write %s", p); return false; }
    return true;
}

static const char CCX_BEAM[] =
"** Cantilever beam, 1 m x 0.1 m x 0.1 m, steel, fixed at x = 0, 1 kN down at the free end (25 nodes x 40 N).\n"
"** The mesh: 40 x 4 x 4 C3D8I bricks (beam_mesh.inp). Tip deflection, Euler-Bernoulli: F L^3 / (3 E I) = 0.190 mm.\n"
"** Results: beam.frd (cgx or ParaView), beam.dat (tip displacements).\n"
"*INCLUDE, INPUT=beam_mesh.inp\n"
"*MATERIAL, NAME=STEEL\n*ELASTIC\n210000e6, 0.3\n*DENSITY\n7850.\n"
"*SOLID SECTION, ELSET=EALL, MATERIAL=STEEL\n"
"*STEP\n*STATIC\n"
"*BOUNDARY\nFIXED, 1, 3\n"
"*CLOAD\nTIP, 3, -40.\n"
"*NODE FILE\nU\n*EL FILE\nS\n*NODE PRINT, NSET=TIP\nU\n"
"*END STEP\n";

static const char CCX_SLAB[] =
"** Heated slab: steel 0.1 m thick, starts at 300 K, the face x = 0 held at 400 K for 250 s.\n"
"*INCLUDE, INPUT=slab_mesh.inp\n"
"*MATERIAL, NAME=SS304\n*CONDUCTIVITY\n16.2\n*SPECIFIC HEAT\n500.\n*DENSITY\n8000.\n"
"*SOLID SECTION, ELSET=EALL, MATERIAL=SS304\n"
"*INITIAL CONDITIONS, TYPE=TEMPERATURE\nNALL, 300.\n"
"*STEP, INC=1000\n*HEAT TRANSFER, DIRECT\n1., 250.\n"
"*BOUNDARY\nHOT, 11, 11, 400.\n"
"*NODE FILE\nNT\n*NODE PRINT, NSET=NALL, FREQUENCY=50\nNT\n"
"*END STEP\n";

/* a structured brick mesh for CalculiX: nx x ny x nz C3D8I (incompatible modes: bending without shear locking) */
static void ccx_mesh(Str *b, double lx, double ly, double lz, int nx, int ny, int nz, const char *setLo, const char *setHi, bool hiOneNode)
{
    str_cat(b, "*NODE, NSET=NALL\n");
    for (int k = 0; k <= nz; k++) for (int j = 0; j <= ny; j++) for (int i = 0; i <= nx; i++)
        str_catf(b, "%d, %.6g, %.6g, %.6g\n", 1 + i + (nx + 1) * (j + (ny + 1) * k), lx * i / nx, ly * j / ny, lz * k / nz);
    str_cat(b, "*ELEMENT, TYPE=C3D8I, ELSET=EALL\n");
    int e = 1;
    for (int k = 0; k < nz; k++) for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
        #define N(a, bb, c) (1 + (i + (a)) + (nx + 1) * ((j + (bb)) + (ny + 1) * (k + (c))))
        str_catf(b, "%d, %d, %d, %d, %d, %d, %d, %d, %d\n", e++, N(0,0,0), N(1,0,0), N(1,1,0), N(0,1,0), N(0,0,1), N(1,0,1), N(1,1,1), N(0,1,1));
        #undef N
    }
    str_catf(b, "*NSET, NSET=%s\n", setLo);
    for (int k = 0; k <= nz; k++) for (int j = 0; j <= ny; j++) str_catf(b, "%d,\n", 1 + (nx + 1) * (j + (ny + 1) * k));
    str_catf(b, "*NSET, NSET=%s\n", setHi);
    for (int k = 0; k <= nz; k++) for (int j = 0; j <= ny; j++) if (!hiOneNode || (j == ny / 2 && k == nz)) str_catf(b, "%d,\n", 1 + nx + (nx + 1) * (j + (ny + 1) * k));
}

static const char MOOSE_HEAT[] =
"# 2-D transient heat conduction in a steel plate (0.1 m x 0.05 m): starts at 300 K, the left edge held at 400 K.\n"
"# Runs with the heat_transfer (or combined) MOOSE application. Results: heat_out.e (ParaView), heat_out.csv.\n"
"[Mesh]\n  [gen]\n    type = GeneratedMeshGenerator\n    dim = 2\n    nx = 40\n    ny = 20\n    xmax = 0.1\n    ymax = 0.05\n  []\n[]\n"
"[Variables]\n  [T]\n    initial_condition = 300\n  []\n[]\n"
"[Kernels]\n  [conduction]\n    type = HeatConduction\n    variable = T\n  []\n  [storage]\n    type = HeatConductionTimeDerivative\n    variable = T\n  []\n[]\n"
"[BCs]\n  [hot]\n    type = DirichletBC\n    variable = T\n    boundary = left\n    value = 400\n  []\n[]\n"
"[Materials]\n  [steel]\n    type = GenericConstantMaterial\n    prop_names = 'thermal_conductivity specific_heat density'\n    prop_values = '16.2 500 8000'\n  []\n[]\n"
"[Postprocessors]\n  [T_avg]\n    type = ElementAverageValue\n    variable = T\n  []\n  [T_right]\n    type = PointValue\n    variable = T\n    point = '0.1 0.025 0'\n  []\n[]\n"
"[Executioner]\n  type = Transient\n  solve_type = NEWTON\n  petsc_options_iname = '-pc_type -pc_hypre_type'\n  petsc_options_value = 'hypre boomeramg'\n"
"  dt = 5\n  end_time = 600\n[]\n"
"[Outputs]\n  exodus = true\n  csv = true\n[]\n";

static const char ELMER_HEAT[] =
"! Steady heat conduction in a 1 m square with a uniform heat source; edges at 0 degrees.\n"
"! Make the mesh first: ElmerGrid 1 2 square.grd (Run does it when the mesh folder is missing).\n"
"Header\n  CHECK KEYWORDS Warn\n  Mesh DB \".\" \"square\"\nEnd\n"
"Simulation\n  Coordinate System = Cartesian 2D\n  Simulation Type = Steady state\n  Steady State Max Iterations = 1\n"
"  Output Intervals = 1\n  Post File = \"heat.vtu\"\nEnd\n"
"Body 1\n  Equation = 1\n  Material = 1\n  Body Force = 1\nEnd\n"
"Material 1\n  Heat Conductivity = 1.0\n  Density = 1.0\nEnd\n"
"Body Force 1\n  Heat Source = 1.0\nEnd\n"
"Equation 1\n  Active Solvers(1) = 1\nEnd\n"
"Solver 1\n  Equation = Heat Equation\n  Procedure = \"HeatSolve\" \"HeatSolver\"\n  Variable = Temperature\n"
"  Linear System Solver = Iterative\n  Linear System Iterative Method = BiCGStab\n  Linear System Preconditioning = ILU0\n"
"  Linear System Max Iterations = 500\n  Linear System Convergence Tolerance = 1.0e-10\n  Steady State Convergence Tolerance = 1.0e-6\nEnd\n"
"Boundary Condition 1\n  Target Boundaries(4) = 1 2 3 4\n  Temperature = 0\nEnd\n";

static const char ELMER_GRD[] =
"***** ElmerGrid input: a 1 m square, 40 x 40 elements *****\n"
"Version = 210903\nCoordinate System = Cartesian 2D\n"
"Subcell Divisions in 2D = 1 1\nSubcell Limits 1 = 0 1\nSubcell Limits 2 = 0 1\nMaterial Structure in 2D\n  1\nEnd\n"
"Materials Interval = 1 1\nBoundary Definitions\n! type out int\n  1 -1 1 1\n  2 -2 1 1\n  3 -3 1 1\n  4 -4 1 1\nEnd\n"
"Numbering = Horizontal\nElement Degree = 1\nElement Innernodes = False\nTriangles = False\n"
"Surface Elements = 1600\nElement Ratios 1 = 1\nElement Ratios 2 = 1\nElement Divisions 1 = 40\nElement Divisions 2 = 40\n";

static const char SPARTA_WEDGE[] =
"# 2-D DSMC: rarefied nitrogen at 1500 m/s (Mach 4.4) over a wedge. Particles are dumped every 200 steps (Results).\n"
"seed            12345\ndimension       2\nglobal          gridcut 0.0 comm/sort yes\n"
"boundary        o o p\n"
"create_box      0 1 -0.4 0.4 -0.5 0.5\ncreate_grid     100 80 1\nbalance_grid    rcb cell\n"
"global          nrho 1.0e20 fnum 1.0e15\n"
"species         air.species N2\nmixture         gas N2 vstream 1500.0 0 0 temp 300.0\n"
"read_surf       data.wedge\nsurf_collide    1 diffuse 300.0 1.0\nsurf_modify     all collide 1\n"
"collide         vss gas air.vss\n"
"fix             in emit/face gas xlo\n"
"timestep        2.0e-6\n"
"compute         temp temp\n"
"stats           100\nstats_style     step cpu np nattempt ncoll nscoll c_temp\n"
"dump            1 particle all 200 dump.flow.* id type x y z vx vy vz\n"
"run             2000\n";

static const char SPARTA_SPECIES[] =
"# species ID  Mol_Wt  Mass  Rot_DOF  Rot_Rel  Vib_DOF  Vib_Rel  Vib_Temp  Spec_wt  Charge\n"
"N2 28.0134 46.5e-27 2 1 2 0.0 3371 1 0\n";
static const char SPARTA_VSS[] = "# species  diam  omega  tref  alpha\nN2 4.17e-10 0.74 273.15 1.0\n";
static const char SPARTA_WEDGE_SURF[] =
"# a wedge of 20 degrees half-angle, tip at x = 0.3 (2-D line segments, traversed clockwise like the SPARTA examples)\n\n3 points\n3 lines\n\n"
"Points\n\n1 0.3 0.0\n2 0.7 0.1456\n3 0.7 -0.1456\n\n"
"Lines\n\n1 1 2\n2 2 3\n3 3 1\n";

static const char ASTER_COMM[] =
"# Transient heat conduction in a steel bar (THER_LINEAIRE): 300 K, one end held at 400 K.\n"
"DEBUT(LANG='EN')\n"
"mesh = LIRE_MAILLAGE(UNITE=20, FORMAT='ASTER')\n"
"model = AFFE_MODELE(MAILLAGE=mesh, AFFE=_F(TOUT='OUI', PHENOMENE='THERMIQUE', MODELISATION='3D'))\n"
"steel = DEFI_MATERIAU(THER=_F(LAMBDA=16.2, RHO_CP=4.0e6))\n"
"chmat = AFFE_MATERIAU(MAILLAGE=mesh, AFFE=_F(TOUT='OUI', MATER=steel))\n"
"hot = AFFE_CHAR_CINE(MODELE=model, THER_IMPO=_F(GROUP_MA='LEFT', TEMP=400.0))\n"
"times = DEFI_LIST_REEL(DEBUT=0., INTERVALLE=_F(JUSQU_A=250., PAS=5.))\n"
"res = THER_LINEAIRE(MODELE=model, CHAM_MATER=chmat, EXCIT=_F(CHARGE=hot),\n"
"                    ETAT_INIT=_F(VALE=300.0), INCREMENT=_F(LIST_INST=times), PARM_THETA=1.0)\n"
"IMPR_RESU(FORMAT='MED', UNITE=80, RESU=_F(RESULTAT=res))\n"
"FIN()\n";

bool prog_template_write(const ProgTemplate *t, const char *dir, char *err, int cap)
{
    err[0] = 0;
    if (!pl_mkdirs(dir)) { snprintf(err, (size_t)cap, "Could not make %s", dir); return false; }
    switch (t->prog) {
    case PROG_CHRONO: {                       /* CMakeLists.txt naming the driver, src/<id>.cpp, params.ini (read at start) */
        Str cm; str_init(&cm);
        const char *a = (const char *)asset_chrono_CMakeLists_txt, *e = a + asset_chrono_CMakeLists_txt_len;
        for (const char *q = a; q < e; ) {
            const char *at = memmem(q, (size_t)(e - q), "@DRIVER@", 8);   /* the asset has no NUL at its end */
            if (!at) { str_catn(&cm, q, (size_t)(e - q)); break; }
            str_catn(&cm, q, (size_t)(at - q)); str_cat(&cm, t->id); q = at + 8;
        }
        char p[PL_PATH], src[PL_PATH], f[160];
        pl_join(src, sizeof src, dir, "src");
        pl_join(p, sizeof p, dir, "CMakeLists.txt");
        bool ok = pl_mkdirs(src) && pl_write_file(p, cm.s, cm.len);
        str_free(&cm);
        snprintf(f, sizeof f, "%s.cpp", t->id); pl_join(p, sizeof p, src, f);
        ok = ok && pl_write_file(p, (const char *)asset_pendulum_cpp, asset_pendulum_cpp_len);
        pl_join(p, sizeof p, dir, "params.ini");
        ok = ok && pl_write_file(p, (const char *)asset_params_pendulum_ini, asset_params_pendulum_ini_len);
        if (!ok) snprintf(err, (size_t)cap, "Could not write into %s", dir);
        return ok;
    }
    case PROG_CALCULIX: {
        Str b; str_init(&b);
        bool ok;
        if (!strcmp(t->id, "beam")) {
            ccx_mesh(&b, 1.0, 0.1, 0.1, 40, 4, 4, "FIXED", "TIP", false);
            ok = put(dir, "beam_mesh.inp", b.s, err, cap) && put(dir, "beam.inp", CCX_BEAM, err, cap);
        } else {
            ccx_mesh(&b, 0.1, 0.01, 0.01, 50, 1, 1, "HOT", "COLD", false);
            ok = put(dir, "slab_mesh.inp", b.s, err, cap) && put(dir, "slab.inp", CCX_SLAB, err, cap);
        }
        str_free(&b);
        return ok;
    }
    case PROG_MOOSE: return put(dir, "heat.i", MOOSE_HEAT, err, cap);
    case PROG_ELMER: return put(dir, "case.sif", ELMER_HEAT, err, cap) && put(dir, "square.grd", ELMER_GRD, err, cap);
    case PROG_SPARTA: return put(dir, "in.wedge", SPARTA_WEDGE, err, cap) && put(dir, "air.species", SPARTA_SPECIES, err, cap) &&
                             put(dir, "air.vss", SPARTA_VSS, err, cap) && put(dir, "data.wedge", SPARTA_WEDGE_SURF, err, cap);
    case PROG_ASTER: {
        /* code_aster's own text mesh format: a 0.1 m bar, 20 x 2 x 2 HEXA8, the face x = 0 as group LEFT */
        Str b; str_init(&b);
        int nx = 20, ny = 2, nz = 2;
        str_cat(&b, "TITRE\n bar written by Q-Continuum\nFINSF\nCOOR_3D\n");
        for (int k = 0; k <= nz; k++) for (int j = 0; j <= ny; j++) for (int i = 0; i <= nx; i++)
            str_catf(&b, " N%d %.6g %.6g %.6g\n", 1 + i + (nx + 1) * (j + (ny + 1) * k), 0.1 * i / nx, 0.01 * j / ny, 0.01 * k / nz);
        str_cat(&b, "FINSF\nHEXA8\n");
        int e = 1;
        for (int k = 0; k < nz; k++) for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
            #define N(a, bb, c) (1 + (i + (a)) + (nx + 1) * ((j + (bb)) + (ny + 1) * (k + (c))))
            str_catf(&b, " M%d N%d N%d N%d N%d N%d N%d N%d N%d\n", e++, N(0,0,0), N(1,0,0), N(1,1,0), N(0,1,0), N(0,0,1), N(1,0,1), N(1,1,1), N(0,1,1));
            #undef N
        }
        str_cat(&b, "FINSF\nQUAD4\n");
        int f0 = e;
        for (int k = 0; k < nz; k++) for (int j = 0; j < ny; j++) {
            #define N(bb, c) (1 + (nx + 1) * ((j + (bb)) + (ny + 1) * (k + (c))))
            str_catf(&b, " M%d N%d N%d N%d N%d\n", e++, N(0,0), N(0,1), N(1,1), N(1,0));
            #undef N
        }
        str_cat(&b, "FINSF\nGROUP_MA\n LEFT\n");
        for (int m = f0; m < e; m++) str_catf(&b, " M%d\n", m);
        str_cat(&b, "FINSF\nFIN\n");
        bool ok = put(dir, "slab.mail", b.s, err, cap) && put(dir, "slab.comm", ASTER_COMM, err, cap);
        str_free(&b);
        return ok;
    }
    default: snprintf(err, (size_t)cap, "%s starters are written by the app", prog_info(t->prog)->name); return false;
    }
}

/* ============================================================================ examples on this machine */
/* a MOOSE example that defines objects of its own (a .C file under src/<kind> beyond main.C and the app's base/) needs its own app
   compiled first; the plain MOOSE applications here can't run it, so the New page leaves it out */
static bool moose_needs_own_app(const char *dir)
{
    char pat[PL_PATH]; snprintf(pat, sizeof pat, "%s/src/*/*.C", dir);
    glob_t g;
    if (glob(pat, 0, NULL, &g) != 0) return false;
    bool own = false;
    for (size_t k = 0; k < g.gl_pathc && !own; k++) if (!strstr(g.gl_pathv[k], "/src/base/")) own = true;
    globfree(&g);
    return own;
}

int prog_examples(ProgExample *out, int max)
{
    int n = 0;
    struct { int prog; const char *pat; } P[] = {
        { PROG_SPARTA,   "%s/src/sparta/examples/*" },
        { PROG_MOOSE,    "%s/projects/moose/modules/heat_transfer/tutorials/*/*" },
        { PROG_MOOSE,    "%s/projects/moose/examples/ex*" },
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0] && n < max; i++) {
        char pat[PL_PATH]; snprintf(pat, sizeof pat, P[i].pat, pl_home());
        glob_t g;
        if (glob(pat, GLOB_ONLYDIR, NULL, &g) != 0) continue;
        for (size_t k = 0; k < g.gl_pathc && n < max; k++) {
            if (prog_detect(g.gl_pathv[k]) != P[i].prog) continue;
            if (P[i].prog == PROG_MOOSE && moose_needs_own_app(g.gl_pathv[k])) continue;
            ProgExample *e = &out[n++];
            e->prog = P[i].prog;
            pl_strlcpy(e->path, g.gl_pathv[k], sizeof e->path);
            char par[PL_PATH]; pl_dirname(par, sizeof par, g.gl_pathv[k]);
            if (P[i].prog == PROG_MOOSE && strstr(pat, "tutorials")) snprintf(e->name, sizeof e->name, "%s/%s", pl_basename(par), pl_basename(g.gl_pathv[k]));
            else pl_strlcpy(e->name, pl_basename(g.gl_pathv[k]), sizeof e->name);
        }
        globfree(&g);
    }
    return n;
}

/* an example copied into a project of its own, so it runs there: symlinks resolved (MOOSE's examples link into each
   other), files it names as "../x/y" (a sibling example's) copied in and the path rewritten, and files its README makes
   with "% python ../../tools/..." made (SPARTA's implicit-surface examples) */
bool prog_example_copy(const char *src, const char *dst, char *err, int cap)
{
    char qs[PL_PATH + 8], qd[PL_PATH + 8], cmd[PL_PATH * 3 + 64];
    pl_shell_quote(qs, sizeof qs, src); pl_shell_quote(qd, sizeof qd, dst);
    if (!pl_fmt(cmd, sizeof cmd, "cp -aL %s %s && chmod -R u+w %s", qs, qd, qd) || pl_run_wait(cmd, NULL, 0) != 0) {
        snprintf(err, (size_t)cap, "Copying the example failed."); return false;
    }
    NameList l = { 0 };
    pl_list_dir(dst, &l, false, true);
    for (int i = 0; i < l.n; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, dst, l.names[i]);
        if (!strcmp(l.names[i], "README") || pl_file_size(p) > (1 << 20)) continue;   /* the README's commands run from the source */
        size_t len = 0; char *t = pl_read_file(p, &len);
        if (!t || memchr(t, 0, len)) { free(t); continue; }            /* text only */
        Str o; str_init(&o);
        bool changed = false;
        for (const char *q = t; *q; ) {
            const char *u = strstr(q, "../");
            if (!u) { str_cat(&o, q); break; }
            if (u > t && !isspace((unsigned char)u[-1]) && u[-1] != '"' && u[-1] != '\'') { str_catn(&o, q, (size_t)(u - q) + 3); q = u + 3; continue; }
            const char *e = u; while (*e && !isspace((unsigned char)*e) && *e != '"' && *e != '\'') e++;
            char rel[PL_PATH], abs[PL_PATH], base[256];
            snprintf(rel, sizeof rel, "%.*s", (int)(e - u), u);
            pl_join(abs, sizeof abs, src, rel);
            pl_strlcpy(base, pl_basename(rel), sizeof base);
            char to[PL_PATH]; pl_join(to, sizeof to, dst, base);
            if (base[0] && pl_is_file(abs)) {
                if (!pl_exists(to)) { size_t n2 = 0; char *b = pl_read_file(abs, &n2); if (b) pl_write_file(to, b, n2); free(b); }
                str_catn(&o, q, (size_t)(u - q)); str_cat(&o, base); changed = true;
            } else str_catn(&o, q, (size_t)(e - q));
            q = e;
        }
        if (changed) pl_write_file(p, o.s, o.len);
        str_free(&o); free(t);
    }
    names_free(&l);
    char rd[PL_PATH]; pl_join(rd, sizeof rd, dst, "README");
    char *r = pl_read_file(rd, NULL);
    for (char *line = r ? strtok(r, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        const char *py = strstr(line, "% python ");
        if (!py) continue;
        py += 9;
        char script[PL_PATH]; word_after(py, script, sizeof script);
        char abs[PL_PATH]; pl_join(abs, sizeof abs, src, script);
        if (!pl_is_file(abs)) continue;
        /* a temporary copy of the tool that runs on Python 3 and today's numpy: SPARTA's are Python 2 era (ndarray.tostring,
           gone in numpy 2; bytes written to a file opened 'w') */
        char tmp[PL_PATH]; pl_join(tmp, sizeof tmp, dst, ".qc-tool.py");
        size_t tl = 0; char *tt = pl_read_file(abs, &tl);
        if (!tt) continue;
        Str fx; str_init(&fx);
        static const char *const FIX[][2] = { { ".tostring()", ".tobytes()" }, { ",'w').write(", ",'wb').write(" } };
        for (const char *q = tt; *q; ) {
            const char *m = NULL; int which = 0;
            for (int f = 0; f < 2; f++) { const char *h = strstr(q, FIX[f][0]); if (h && (!m || h < m)) { m = h; which = f; } }
            if (!m) { str_cat(&fx, q); break; }
            str_catn(&fx, q, (size_t)(m - q)); str_cat(&fx, FIX[which][1]); q = m + strlen(FIX[which][0]);
        }
        pl_write_file(tmp, fx.s, fx.len);
        str_free(&fx); free(tt);
        char qa[PL_PATH + 8], c2[PL_PATH * 2 + 600]; pl_shell_quote(qa, sizeof qa, tmp);
        snprintf(c2, sizeof c2, "cd %s && for py in python3 \"$HOME\"/miniforge/bin/python3 \"$HOME\"/miniforge/envs/*/bin/python3; do "
                 "\"$py\" -c 'import numpy' 2>/dev/null && break; done; \"$py\" %s%s", qd, qa, skip_tok(py));   /* the tools want numpy */
        pl_run_wait(c2, NULL, 0);
        unlink(tmp);
    }
    free(r);
    return true;
}

/* ============================================================================ progress */
bool prog_progress(int prog, const LogMon *m, double inputEnd, bool inputSteps, double *done, double *total, bool *steps)
{
    *done = *total = 0; *steps = false;
    if (m->endSteps > 0) { *steps = true; *total = m->endSteps; *done = m->steps; }       /* Elmer, code_saturne say it */
    else if (m->endTime > 0) { *total = m->endTime; *done = m->time; }                    /* DEME / Chrono drivers */
    else if (inputEnd > 0) { *steps = inputSteps; *total = inputEnd; *done = inputSteps ? m->steps : m->time; }
    if (prog == PROG_SPARTA && *steps) *done = m->time;                                     /* its step number */
    if (m->ended && !m->fatal && m->exitCode == 0) *done = *total;                         /* a static step prints no time */
    if (*done > *total && *total > 0) *done = *total;
    return *total > 0;
}
