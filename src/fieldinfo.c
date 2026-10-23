/* fieldinfo.c -- what an OpenFOAM field means: units, description, typical values, turbulence-model use, and the
 * values the tutorials start each field at (see fieldinfo.h)
 */
#define _GNU_SOURCE
#include <ftw.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fieldinfo.h"
#include "pl_util.h"

/* ---------------------------------------------------------------- units */

static void unit_part(Str *b, const char *sym, int e)
{
    if (b->len) str_cat(b, "·");
    str_cat(b, sym);
    if (e >= 2 && e <= 4) str_cat(b, e == 2 ? "²" : e == 3 ? "³" : "⁴");
    else if (e > 4) str_catf(b, "^%d", e);
}

bool dims_unit(const char *dims, char *out, int cap)
{
    out[0] = 0;
    const char *a = strchr(dims, '[');
    if (!a) return false;
    double d[7] = { 0 };
    int n = 0;
    const char *p = a + 1;
    while (n < 7) {
        char *end;
        double v = strtod(p, &end);
        if (end == p) break;
        d[n++] = v; p = end;
    }
    if (n != 5 && n != 7) return false;
    for (int i = 0; i < 7; i++) if (d[i] != floor(d[i]) || fabs(d[i]) > 9) return false;   /* fractional: leave it */
    int e[7]; for (int i = 0; i < 7; i++) e[i] = (int)d[i];
    /* named units first, as engineers write them */
    static const struct { int e[7]; const char *u; } NAMED[] = {
        { { 0, 0, 0, 0, 0, 0, 0 }, "dimensionless" },
        { { 1, -1, -2, 0, 0, 0, 0 }, "Pa" },
        { { 1, 0, -3, 0, 0, 0, 0 }, "W/m²" },
        { { 1, -1, -3, 0, 0, 0, 0 }, "W/m³" },
        { { 1, 2, -3, 0, 0, 0, 0 }, "W" },
        { { 1, 1, -3, -1, 0, 0, 0 }, "W/(m·K)" },
        { { 1, 0, -3, -1, 0, 0, 0 }, "W/(m²·K)" },
        { { 0, 2, -2, -1, 0, 0, 0 }, "J/(kg·K)" },
        { { 1, 1, -2, 0, 0, 0, 0 }, "N" },
        { { 1, 2, -2, 0, 0, 0, 0 }, "J" },
        { { 1, -3, 0, 0, 0, 0, 0 }, "kg/m³" },
        { { 1, -1, -1, 0, 0, 0, 0 }, "Pa·s" },
        { { 0, 2, -2, 0, 0, 0, 0 }, "m²/s²" },        /* kinematic pressure p/ρ, or k: also J/kg */
    };
    for (size_t k = 0; k < sizeof NAMED / sizeof NAMED[0]; k++)
        if (!memcmp(e, NAMED[k].e, sizeof e)) { pl_strlcpy(out, NAMED[k].u, (size_t)cap); return true; }
    static const char *SYM[7] = { "kg", "m", "s", "K", "mol", "A", "cd" };
    Str num, den; str_init(&num); str_init(&den);
    int nden = 0;
    for (int i = 0; i < 7; i++) {
        if (e[i] > 0) unit_part(&num, SYM[i], e[i]);
        else if (e[i] < 0) { unit_part(&den, SYM[i], -e[i]); nden++; }
    }
    if (!num.len) str_cat(&num, "1");
    if (den.len) snprintf(out, (size_t)cap, nden > 1 ? "%s/(%s)" : "%s/%s", num.s, den.s);
    else snprintf(out, (size_t)cap, "%s", num.s);
    str_free(&num); str_free(&den);
    return true;
}

/* ---------------------------------------------------------------- what the fields are */
static const FieldInfo FIELDS[] = {
    { "U", "Velocity.", "Inlet speeds from mm/s (natural convection) to hundreds of m/s (compressible); 0 at walls." },
    { "p", "Pressure. Incompressible solvers use kinematic pressure p/ρ in m²/s² (gauge, often 0 at an outlet); compressible "
           "solvers use absolute pressure in Pa (1e5 at sea level).", "Incompressible: 0 at the outlet. Compressible: about 1e5 Pa." },
    { "p_rgh", "Pressure minus the hydrostatic part ρgh: what buoyant and multiphase solvers solve for.", "Same order as p: 1e5 Pa for "
               "compressible buoyant cases, 0 for incompressible VoF." },
    { "T", "Temperature.", "Absolute kelvin: 293 K is 20 °C." },
    { "k", "Turbulent kinetic energy per unit mass.", "k = 1.5 (U·I)² for intensity I: 1e-4 to 1 m²/s² for most flows (I of 1 to 10 %)." },
    { "epsilon", "Dissipation rate of turbulent kinetic energy (k-epsilon models).", "ε = Cμ^0.75 k^1.5 / L for a mixing length L: "
                 "1e-4 to 100 m²/s³." },
    { "omega", "Specific dissipation rate (k-omega models).", "ω = √k / (Cμ^0.25 L): 1 to 1e4 1/s; much larger at walls." },
    { "nut", "Turbulent (eddy) viscosity. Computed by the model; the file holds its wall functions.", "internalField 0; "
             "nutkWallFunction at walls, calculated elsewhere." },
    { "nuTilda", "Spalart-Allmaras working variable.", "3 to 5 times the laminar ν at inlets; 0 at walls." },
    { "alphat", "Turbulent thermal diffusivity (heat-transfer cases with turbulence).", "internalField 0; compressible::alphatWallFunction at walls." },
    { "alpha.", "Volume fraction of a phase (VoF and Euler multiphase).", "Between 0 and 1: 1 inside the phase, 0 outside." },
    { "G", "Incident radiation (P1 radiation model).", "0 initially; MarshakRadiation at walls." },
    { "IDefault", "Radiation intensity template for each fvDOM ray.", "0 initially; greyDiffusiveRadiation at walls." },
    { "h", "Specific enthalpy (solids in conjugate heat transfer).", "Computed from T; the file sets the conditions." },
    { "kt", "Turbulent kinetic energy part of kkLOmega.", "Like k." },
    { "kl", "Laminar kinetic energy part of kkLOmega (transition).", "Usually 0 at inlets." },
    { "ReThetat", "Transition momentum-thickness Reynolds number (kOmegaSSTLM).", "From the free-stream turbulence: a few hundred." },
    { "gammaInt", "Intermittency (kOmegaSSTLM): 0 laminar, 1 turbulent.", "1 at inlets." },
    { "v2", "Wall-normal velocity fluctuation (v2f).", "About 2/3 k." },
    { "f", "Elliptic relaxation function (v2f).", "0." },
    { "rho", "Density.", "1.2 kg/m³ air, 1000 kg/m³ water." },
    { "Ydefault", "Template for species mass fractions not given their own file.", "0." },
    { "D", "Displacement (solid mechanics).", "0 at the start." },
    { NULL, NULL, NULL } };

const FieldInfo *field_info(const char *name)
{
    for (int i = 0; FIELDS[i].name; i++) {
        size_t n = strlen(FIELDS[i].name);
        if (FIELDS[i].name[n - 1] == '.' ? !strncmp(name, FIELDS[i].name, n) : !strcmp(name, FIELDS[i].name)) return &FIELDS[i];
    }
    return NULL;
}

int turb_model_fields(const char *model, const char **out)
{
    int n = 0;
    if (!model[0]) return 0;
    /* one-equation LES models first: "dynamicKEqn" contains "KE" and was taken for k-epsilon, so the official LES
       pitzDaily tutorial was refused for a missing epsilon */
    if (strstr(model, "KEqn") || !strcmp(model, "kEqn")) out[n++] = "k";
    else if (!strcmp(model, "dynamicLagrangian")) { out[n++] = "flm"; out[n++] = "fmm"; }
    else if (!strcmp(model, "kkLOmega")) { out[n++] = "kt"; out[n++] = "kl"; out[n++] = "omega"; }
    else if (strstr(model, "Epsilon") || pl_ends_with(model, "KE") || !strcmp(model, "v2f") || !strcmp(model, "LienLeschziner")) { out[n++] = "k"; out[n++] = "epsilon"; }
    else if (!strcmp(model, "kOmegaSSTLM")) { out[n++] = "k"; out[n++] = "omega"; out[n++] = "ReThetat"; out[n++] = "gammaInt"; }
    else if (strstr(model, "Omega") || !strcmp(model, "GEKO")) { out[n++] = "k"; out[n++] = "omega"; }
    else if (strstr(model, "SpalartAllmaras")) out[n++] = "nuTilda";
    /* Smagorinsky, WALE, DeardorffDiffStress and the rest: nut only */
    out[n++] = "nut";
    return n;
}

int turb_field_used(const char *field, const char *simType, const char *model)
{
    static const char *TURB[] = { "k", "epsilon", "omega", "nut", "nuTilda", "kt", "kl", "ReThetat", "gammaInt", "flm", "fmm", NULL };
    bool turb = false;
    for (int i = 0; TURB[i]; i++) if (!strcmp(field, TURB[i])) turb = true;
    if (!turb || !simType || !simType[0]) return -1;
    if (!strcmp(simType, "laminar")) return 0;
    if (!model || !model[0]) return -1;
    const char *need[6]; int n = turb_model_fields(model, need);
    for (int i = 0; i < n; i++) if (!strcmp(need[i], field)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- tutorial ranges */
typedef struct { char name[48]; double *v; int n, cap; } Acc;
static Acc *ACC; static int nAcc, capAcc;

static void acc_add(const char *name, double v)
{
    int i;
    for (i = 0; i < nAcc; i++) if (!strcmp(ACC[i].name, name)) break;
    if (i == nAcc) {
        if (nAcc == capAcc) {
            int nc = capAcc ? capAcc * 2 : 64;
            Acc *na = realloc(ACC, sizeof *ACC * (size_t)nc);
            if (!na) return;
            ACC = na; capAcc = nc;
        }
        memset(&ACC[nAcc], 0, sizeof *ACC);
        pl_strlcpy(ACC[nAcc].name, name, sizeof ACC[0].name);
        nAcc++;
    }
    Acc *a = &ACC[i];
    if (a->n == a->cap) {
        int nc = a->cap ? a->cap * 2 : 16;
        double *nv = realloc(a->v, sizeof(double) * (size_t)nc);
        if (!nv) return;
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = v;
}

/* "internalField   uniform 0.375;" or "uniform (1 0 0);" -> the value (vectors by magnitude) */
static bool uniform_value(const char *t, double *out)
{
    const char *p = strstr(t, "\ninternalField");
    if (!p) return false;
    p += 14;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "uniform", 7)) return false;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    if (*p == '(') {
        double s = 0; int n = 0;
        p++;
        for (;;) {
            while (*p == ' ') p++;
            if (*p == ')') break;
            double v = strtod(p, &end);
            if (end == p) return false;
            s += v * v; n++; p = end;
            if (n > 9) return false;
        }
        if (n != 3) return false;                       /* vectors only, not tensors */
        *out = sqrt(s);
        return true;
    }
    double v = strtod(p, &end);
    if (end == p) return false;
    while (*end == ' ') end++;
    if (*end != ';') return false;                      /* "uniform $Tinit;" and the like */
    *out = v;
    return true;
}

static int visit(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (type != FTW_F) return 0;
    /* <case>/0/<field>, <case>/0.orig/<field>, and one region folder below those */
    char dir[PL_PATH], up[PL_PATH];
    pl_dirname(dir, sizeof dir, path);
    const char *d = pl_basename(dir);
    bool ok = !strcmp(d, "0") || !strcmp(d, "0.orig");
    if (!ok) { pl_dirname(up, sizeof up, dir); const char *u = pl_basename(up); ok = !strcmp(u, "0") || !strcmp(u, "0.orig"); }
    if (!ok) return 0;
    const char *name = pl_basename(path);
    if (strlen(name) >= 48 || pl_ends_with(name, ".orig") || pl_ends_with(name, ".gz") || strchr(name, '~')) return 0;
    char *h = pl_read_head(path, 8192);
    if (!h) return 0;
    double v;
    if ((strstr(h, "volScalarField") || strstr(h, "volVectorField")) && uniform_value(h, &v)) acc_add(name, v);
    free(h);
    return 0;
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

int tut_ranges_scan(const char *tutorialsDir, const char *outFile)
{
    nAcc = 0;
    nftw(tutorialsDir, visit, 32, FTW_PHYS);
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum: tutorial initial values per field: name count min median max\n");
    for (int i = 0; i < nAcc; i++) {
        Acc *a = &ACC[i];
        qsort(a->v, (size_t)a->n, sizeof(double), cmpd);
        str_catf(&b, "%s %d %.6g %.6g %.6g\n", a->name, a->n, a->v[0], a->v[a->n / 2], a->v[a->n - 1]);
        free(a->v);
    }
    int n = nAcc;
    free(ACC); ACC = NULL; nAcc = capAcc = 0;
    bool w = pl_write_file(outFile, b.s, b.len);
    str_free(&b);
    return w ? n : -1;
}

static struct { char file[PL_PATH]; pid_t pid; bool loaded; char *text; } TR = { .pid = -1 };

void tut_ranges_start(const char *tutorialsDir, const char *cacheFile)
{
    if (TR.loaded || TR.pid > 0 || !tutorialsDir[0] || !pl_is_dir(tutorialsDir)) return;
    pl_strlcpy(TR.file, cacheFile, sizeof TR.file);
    /* a week-old scan is still good: the tutorials change with OpenFOAM releases */
    if (pl_is_file(cacheFile) && time(NULL) - pl_mtime(cacheFile) < 7 * 86400 && pl_mtime(cacheFile) >= pl_mtime(tutorialsDir)) return;
    char d[PL_PATH]; pl_dirname(d, sizeof d, cacheFile); pl_mkdirs(d);
    pid_t p = fork();
    if (p == 0) { if (nice(10) < 0) { /* not lowered: scan anyway */ } tut_ranges_scan(tutorialsDir, cacheFile); _exit(0); }
    TR.pid = p;
}

bool tut_range(const char *field, TutRange *out)
{
    if (TR.pid > 0) {
        if (waitpid(TR.pid, NULL, WNOHANG) == 0) return false;
        TR.pid = -1;
    }
    if (!TR.loaded) {
        if (!TR.file[0]) return false;
        TR.text = pl_read_file(TR.file, NULL);
        TR.loaded = true;
    }
    if (!TR.text) return false;
    size_t n = strlen(field);
    for (const char *p = TR.text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, field, n) && p[n] == ' ')
            return sscanf(p + n, "%d %lf %lf %lf", &out->n, &out->min, &out->median, &out->max) == 4;
    }
    return false;
}

/* ============================================================================ checking a typed value */
const char *field_value_problem(const char *cls, const char *raw)
{
    const char *s = raw;
    while (isspace((unsigned char)*s)) s++;
    if (!*s || *s == '$' || *s == '#' || !strncmp(s, "nonuniform", 10)) return NULL;
    int want = strstr(cls, "SymmTensor") ? 6 : strstr(cls, "SphericalTensor") ? 1 : strstr(cls, "Tensor") ? 9 : strstr(cls, "Vector") ? 3 : 0;
    if (!strncmp(s, "uniform", 7) && isspace((unsigned char)s[7])) {        /* a bare value means uniform too */
        s += 8;
        while (isspace((unsigned char)*s)) s++;
    }
    if (*s == '$' || *s == '#') return NULL;
    char *e;
    if (want == 0 || want == 1) {
        strtod(s, &e);
        if (e == s) return "uniform needs a number here, e.g. uniform 0.";
        while (isspace((unsigned char)*e) || *e == ';') e++;
        return *e ? "Only one number belongs after uniform for this field." : NULL;
    }
    if (*s != '(') return want == 3 ? "A vector goes in brackets: uniform (x y z)." : "A tensor goes in brackets: uniform (...).";
    s++;
    int n = 0;
    for (;;) {
        while (isspace((unsigned char)*s)) s++;
        if (*s == ')') break;
        strtod(s, &e);
        if (e == s) return "Only numbers belong inside the brackets.";
        n++; s = e;
    }
    if (n != want) return want == 3 ? "A vector has 3 components: uniform (x y z)." : want == 6 ? "A symmetric tensor has 6 components." : "A tensor has 9 components.";
    return NULL;
}

static bool key_in(const char *k, const char *const *list)
{
    for (int i = 0; list[i]; i++) if (!strcmp(k, list[i])) return true;
    return false;
}

const char *value_problem(const char *key, const char *oldv, const char *newv, const char *cls)
{
    static char why[200];
    const char *k = strrchr(key, '/'); k = k ? k + 1 : key;
    /* structure */
    int brace = 0, paren = 0, brack = 0; bool q = false;
    for (const char *p = newv; *p; p++) {
        if (*p == '"') { q = !q; continue; }
        if (q) continue;
        if (*p == ';') return "A ';' would end the entry there and start another: leave it out.";
        if (*p == '{' || *p == '}') return "Braces belong to dictionaries, not to one value.";
        if (*p == '(') paren++;
        if (*p == ')' && --paren < 0) return "A ')' without its '('.";
        if (*p == '[') brack++;
        if (*p == ']' && --brack < 0) return "A ']' without its '['.";
    }
    (void)brace;
    if (q) return "A quote is not closed.";
    if (paren) return "A '(' is not closed.";
    if (brack) return "A '[' is not closed.";
    char t[1024]; snprintf(t, sizeof t, "%s", newv);
    char *s = t; while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    if (!*s) return "The value is empty.";
    bool macro = *s == '$' || *s == '#';
    /* numbers stay numbers */
    char o[1024]; snprintf(o, sizeof o, "%s", oldv ? oldv : "");
    char *os = o; while (isspace((unsigned char)*os)) os++;
    char *oe = os + strlen(os); while (oe > os && isspace((unsigned char)oe[-1])) *--oe = 0;
    bool wasNum = pl_is_number(os), isNum = pl_is_number(s);
    if (isNum) {
        double v = strtod(s, NULL);
        if (!isfinite(v)) { snprintf(why, sizeof why, "%s is beyond what a double can hold.", s); return why; }
        static const char *const positive[] = { "deltaT", "maxDeltaT", "writeInterval", "maxCo", "maxAlphaCo", "maxDi", "maxFourier", NULL };
        static const char *const counts[] = { "nCorrectors", "nOuterCorrectors", "nNonOrthogonalCorrectors", "nAlphaCorr", "nAlphaSubCycles",
                                              "numberOfSubdomains", "purgeWrite", "writePrecision", "timePrecision", "nSweeps", "maxIter", NULL };
        if (key_in(k, positive) && !(v > 0)) { snprintf(why, sizeof why, "%s must be above 0 (it was %s).", k, os[0] ? os : "not set"); return why; }
        if (key_in(k, counts) && (v < 0 || v != floor(v))) { snprintf(why, sizeof why, "%s is a count: a whole number, 0 or more.", k); return why; }
        if (!strcmp(k, "endTime") && v < 0) return "endTime can't be negative.";
    } else if (wasNum && !macro) {
        snprintf(why, sizeof why, "%s takes a number (it is %s now); %s isn't one.", k, os, s);
        return why;
    }
    /* field values */
    static const char *const fieldKeys[] = { "value", "inletValue", "outletValue", "internalField", "refValue", "uniformValue", NULL };
    if (cls && cls[0] && (strstr(cls, "Field")) && key_in(k, fieldKeys) && strcmp(k, "uniformValue")) {
        const char *fp = field_value_problem(cls, s);
        if (fp) return fp;
    }
    return NULL;
}

/* ============================================================================ the dimensions fields must have */
const char *field_expected_dims(const char *n)
{
    static const struct { const char *n, *d; } T[] = {
        /* not T (a passive scalar in scalarTransportFoam cases is dimensionless) nor alphat (kinematic in incompressible
           solvers, kg/m/s in compressible ones): their units depend on the solver */
        { "U", "[0 1 -1 0 0 0 0]" }, { "k", "[0 2 -2 0 0 0 0]" }, { "epsilon", "[0 2 -3 0 0 0 0]" },
        { "omega", "[0 0 -1 0 0 0 0]" }, { "nut", "[0 2 -1 0 0 0 0]" }, { "nuTilda", "[0 2 -1 0 0 0 0]" },
        { "G", "[1 0 -3 0 0 0 0]" }, { "qr", "[1 0 -3 0 0 0 0]" }, { "kl", "[0 2 -2 0 0 0 0]" }, { "kt", "[0 2 -2 0 0 0 0]" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(n, T[i].n)) return T[i].d;
    if (!strncmp(n, "alpha.", 6)) return "[0 0 0 0 0 0 0]";
    return NULL;
}

bool dims_equal(const char *a, const char *b)
{
    double x[7] = { 0 }, y[7] = { 0 };
    const char *pa = strchr(a, '['), *pb = strchr(b, '[');
    if (!pa || !pb) return false;
    int na = sscanf(pa + 1, "%lf %lf %lf %lf %lf %lf %lf", &x[0], &x[1], &x[2], &x[3], &x[4], &x[5], &x[6]);
    int nb = sscanf(pb + 1, "%lf %lf %lf %lf %lf %lf %lf", &y[0], &y[1], &y[2], &y[3], &y[4], &y[5], &y[6]);
    if (na < 5 || nb < 5) return false;
    for (int i = 0; i < 7; i++) if (x[i] != y[i]) return false;
    return true;
}
