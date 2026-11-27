/* chtsetup.c -- conjugate heat transfer from choices (see chtsetup.h; improvement plan A2, 2026-11-26) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "chtsetup.h"
#include "pl_util.h"
#include "simsetup.h"
#include "xsection.h"

/* properties near 300 K (Incropera, Fundamentals of Heat and Mass Transfer, tables A.1, A.3, A.4, A.6) */
const ChtMat CHT_FLUIDS[] = {
    { "Air",   1.16,  1007, 0.0263, 1.846e-5, true,  28.96 },
    { "Water", 997,   4179, 0.613,  8.55e-4,  false, 18.02 },
    { "Engine oil", 884, 1909, 0.145, 0.486,  false, 400   },
};
const int CHT_NFLUIDS = (int)(sizeof CHT_FLUIDS / sizeof CHT_FLUIDS[0]);
const ChtMat CHT_SOLIDS[] = {
    { "Aluminium (6061)",     2700, 896, 167,  0, false, 27 },
    { "Copper",               8933, 385, 401,  0, false, 63.5 },
    { "Carbon steel",         7854, 434, 60.5, 0, false, 55.8 },
    { "Stainless steel (304)", 7900, 477, 14.9, 0, false, 55.8 },
    { "Silicon",              2330, 712, 148,  0, false, 28.1 },
    { "Glass",                2500, 750, 1.4,  0, false, 60 },
    { "Concrete",             2300, 880, 1.4,  0, false, 60 },
    { "Circuit board (FR-4)", 1850, 1100, 0.3, 0, false, 100 },
    { "Plastic (ABS)",        1050, 1400, 0.17, 0, false, 100 },
};
const int CHT_NSOLIDS = (int)(sizeof CHT_SOLIDS / sizeof CHT_SOLIDS[0]);
const char *const CHT_ROLE_NAME[CP__N] = { "Wall", "Inlet", "Outlet" };
const char *const CHT_HEAT_NAME[CT__N] = { "No heat crosses (adiabatic)", "Fixed temperature", "Heat flux", "Convection to surroundings" };
const char *const CHT_KIND_NAME[CR__N] = { "Fluid", "Solid" };

double cht_prandtl(const ChtMat *m) { return m->k > 0 ? m->mu * m->cp / m->k : 0; }
double cht_diffusivity(const ChtMat *m) { return m->rho > 0 && m->cp > 0 ? m->k / (m->rho * m->cp) : 0; }

int cht_find_region(const ChtSetup *s, const char *name)
{
    for (int i = 0; i < s->nR; i++) if (!strcmp(s->r[i].name, name)) return i;
    return -1;
}
int cht_find_patch(const ChtSetup *s, const char *name)
{
    for (int i = 0; i < s->nP; i++) if (!strcmp(s->p[i].name, name)) return i;
    return -1;
}

static const ChtMat *mat_of(const ChtRegion *r)
{
    if (r->kind == CR_FLUID) return &CHT_FLUIDS[r->mat >= 0 && r->mat < CHT_NFLUIDS ? r->mat : 0];
    return &CHT_SOLIDS[r->mat >= 0 && r->mat < CHT_NSOLIDS ? r->mat : 0];
}

/* ---------------------------------------------------------------------------------------------- the mesh */
static long header_count(const char *text, const char *key)        /* "nCells:400" in a polyMesh file's note */
{
    const char *p = text ? strstr(text, key) : NULL;
    return p ? atol(p + strlen(key)) : -1;
}

long cht_rest_cells(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "constant/polyMesh/owner");
    char head[4097] = "";                                    /* the note in owner's header: nCells */
    FILE *f = fopen(p, "rb");
    if (f) { size_t n = fread(head, 1, 4096, f); head[n] = 0; fclose(f); }
    long nc = header_count(head, "nCells:");
    if (nc < 0) return -1;
    pl_join(p, sizeof p, casePath, "constant/polyMesh/cellZones");
    char *z = pl_read_file_max(p, NULL, 1LL << 32);
    if (!z) return nc;
    long inZones = 0;
    /* each zone: "cellLabels List<label> N" then N labels, in ascii or binary; only the counts are read. A cell in two
       zones counts twice, which splitMeshRegions refuses anyway */
    for (const char *q = z; (q = strstr(q, "cellLabels")) != NULL; ) {
        q += 10;
        while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        if (!strncmp(q, "List<label>", 11)) q += 11;
        while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        long k = atol(q);
        if (k > 0) inZones += k;
    }
    free(z);
    return nc - inZones;
}

bool cht_from_mesh(const char *casePath, ChtSetup *s, char *err, int cap)
{
    memset(s, 0, sizeof *s);
    s->g[1] = -9.81; s->endTime = 1000; s->writeEvery = 100;
    char zones[CHT_MAX_R][64];
    int nz = sim_mesh_cell_zones(casePath, zones, CHT_MAX_R - 1);
    if (nz <= 0) {
        snprintf(err, (size_t)cap, "The mesh has no cellZones: each solid (and fluid) part must be a cellZone. In the STL mesher give each closed "
                                   "part's surface a cell zone, or make them with topoSet (Mesh > Tools).");
        return false;
    }
    for (int i = 0; i < nz; i++) {
        ChtRegion *r = &s->r[s->nR++];
        pl_strlcpy(r->name, zones[i], sizeof r->name);
        r->kind = CR_SOLID; r->mat = 0; r->T0 = 300;
    }
    long rest = cht_rest_cells(casePath);
    if (rest > 0) {
        ChtRegion *r = &s->r[s->nR++];
        pl_strlcpy(r->name, cht_find_region(s, CHT_REST) < 0 ? CHT_REST : "rest", sizeof r->name);
        r->rest = true; r->kind = CR_FLUID; r->mat = 0; r->T0 = 300;
    } else if (s->nR) s->r[s->nR - 1].kind = CR_FLUID;        /* every cell zoned: guess the last zone is the fluid */
    char bp[PL_PATH]; pl_join(bp, sizeof bp, casePath, "constant/polyMesh/boundary");
    FrPatch pt[256];
    int np = fr_boundary(bp, pt, 256);
    for (int i = 0; i < np && s->nP < CHT_MAX_P; i++) {
        const char *t = pt[i].type;
        if (!strcmp(t, "empty") || !strncmp(t, "symmetry", 8) || !strcmp(t, "wedge") || !strncmp(t, "cyclic", 6) || !strcmp(t, "processor"))
            continue;
        ChtPatch *p = &s->p[s->nP++];
        pl_strlcpy(p->name, pt[i].name, sizeof p->name);
        p->role = CP_WALL; p->U = 1; p->Tin = 300; p->heat = CT_ADIABATIC; p->T = 300; p->q = 1000; p->h = 10; p->Ta = 300;
        if (pl_contains_ci(p->name, "inlet")) p->role = CP_INLET;
        else if (pl_contains_ci(p->name, "outlet")) p->role = CP_OUTLET;
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------- cht.ini */
bool cht_exists(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "cht.ini");
    return pl_is_file(p);
}

bool cht_save(const char *casePath, const ChtSetup *s)
{
    Str b; str_init(&b);
    str_catf(&b, "# Q-Continuum conjugate heat transfer (Physics > Setup); written again by the app\n[cht]\ntransient=%d\nturbulent=%d\n"
                 "g=%.9g %.9g %.9g\nendTime=%.9g\nwriteEvery=%.9g\n", s->transient, s->turbulent, s->g[0], s->g[1], s->g[2], s->endTime, s->writeEvery);
    for (int i = 0; i < s->nR; i++) {
        const ChtRegion *r = &s->r[i];
        str_catf(&b, "\n[region %s]\nrest=%d\nkind=%s\nmaterial=%s\nT0=%.9g\nU0=%.9g %.9g %.9g\nQ=%.9g\n", r->name, r->rest,
                 r->kind == CR_SOLID ? "solid" : "fluid", mat_of(r)->name, r->T0, r->U0[0], r->U0[1], r->U0[2], r->Q);
    }
    static const char *ROLE[CP__N] = { "wall", "inlet", "outlet" }, *HEAT[CT__N] = { "adiabatic", "fixed", "flux", "convection" };
    for (int i = 0; i < s->nP; i++) {
        const ChtPatch *p = &s->p[i];
        str_catf(&b, "\n[patch %s]\nrole=%s\nU=%.9g\nTin=%.9g\nheat=%s\nT=%.9g\nq=%.9g\nh=%.9g\nTa=%.9g\n", p->name, ROLE[p->role], p->U, p->Tin,
                 HEAT[p->heat], p->T, p->q, p->h, p->Ta);
    }
    char path[PL_PATH]; pl_join(path, sizeof path, casePath, "cht.ini");
    bool ok = b.s && pl_write_file(path, b.s, b.len);
    str_free(&b);
    return ok;
}

static int word_index(const char *v, const char *const *words, int n, int def)
{
    for (int i = 0; i < n; i++) if (!strcmp(v, words[i])) return i;
    return def;
}

bool cht_load(const char *casePath, ChtSetup *s)
{
    char path[PL_PATH]; pl_join(path, sizeof path, casePath, "cht.ini");
    char *t = pl_read_file(path, NULL);
    if (!t) return false;
    memset(s, 0, sizeof *s);
    s->g[1] = -9.81; s->endTime = 1000; s->writeEvery = 100;
    static const char *ROLE[CP__N] = { "wall", "inlet", "outlet" }, *HEAT[CT__N] = { "adiabatic", "fixed", "flux", "convection" };
    ChtRegion *r = NULL; ChtPatch *p = NULL;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line == ' ') line++;
        if (*line == '#' || !*line) continue;
        if (*line == '[') {
            r = NULL; p = NULL;
            char nm[64] = "";
            if (sscanf(line, "[region %63[^]]", nm) == 1 && s->nR < CHT_MAX_R) {
                r = &s->r[s->nR++]; memset(r, 0, sizeof *r); pl_strlcpy(r->name, nm, sizeof r->name); r->T0 = 300;
            } else if (sscanf(line, "[patch %63[^]]", nm) == 1 && s->nP < CHT_MAX_P) {
                p = &s->p[s->nP++]; memset(p, 0, sizeof *p); pl_strlcpy(p->name, nm, sizeof p->name);
                p->U = 1; p->Tin = 300; p->T = 300; p->q = 1000; p->h = 10; p->Ta = 300;
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (r) {
            if (!strcmp(k, "rest")) r->rest = atoi(v) != 0;
            else if (!strcmp(k, "kind")) r->kind = !strcmp(v, "solid") ? CR_SOLID : CR_FLUID;
            else if (!strcmp(k, "material")) {
                const ChtMat *L = r->kind == CR_SOLID ? CHT_SOLIDS : CHT_FLUIDS; int n = r->kind == CR_SOLID ? CHT_NSOLIDS : CHT_NFLUIDS;
                r->mat = 0;
                for (int i = 0; i < n; i++) if (!strcmp(L[i].name, v)) r->mat = i;
            } else if (!strcmp(k, "T0")) r->T0 = atof(v);
            else if (!strcmp(k, "U0")) sscanf(v, "%lf %lf %lf", &r->U0[0], &r->U0[1], &r->U0[2]);
            else if (!strcmp(k, "Q")) r->Q = atof(v);
        } else if (p) {
            if (!strcmp(k, "role")) p->role = word_index(v, ROLE, CP__N, CP_WALL);
            else if (!strcmp(k, "U")) p->U = atof(v);
            else if (!strcmp(k, "Tin")) p->Tin = atof(v);
            else if (!strcmp(k, "heat")) p->heat = word_index(v, HEAT, CT__N, CT_ADIABATIC);
            else if (!strcmp(k, "T")) p->T = atof(v);
            else if (!strcmp(k, "q")) p->q = atof(v);
            else if (!strcmp(k, "h")) p->h = atof(v);
            else if (!strcmp(k, "Ta")) p->Ta = atof(v);
        } else {
            if (!strcmp(k, "transient")) s->transient = atoi(v) != 0;
            else if (!strcmp(k, "turbulent")) s->turbulent = atoi(v) != 0;
            else if (!strcmp(k, "g")) sscanf(v, "%lf %lf %lf", &s->g[0], &s->g[1], &s->g[2]);
            else if (!strcmp(k, "endTime")) s->endTime = atof(v);
            else if (!strcmp(k, "writeEvery")) s->writeEvery = atof(v);
        }
    }
    free(t);
    return true;
}

/* ---------------------------------------------------------------------------------------------- checks */
void cht_check(const ChtSetup *s, char *problems, int pcap, char *warn, int wcap)
{
    Str P, W; str_init(&P); str_init(&W);
    int nf = 0, ns = 0, nin = 0, nout = 0, fixedT = 0;
    double Qtot = 0;
    for (int i = 0; i < s->nR; i++) {
        const ChtRegion *r = &s->r[i];
        if (r->kind == CR_FLUID) nf++; else { ns++; Qtot += r->Q; }
        if (!(r->T0 > 0)) str_catf(&P, "%s: its starting temperature must be above 0 K.\n", r->name);
        if (r->kind == CR_SOLID && r->Q < 0) str_catf(&W, "%s takes heat away (%.3g W): a heat sink, as asked.\n", r->name, r->Q);
    }
    for (int i = 0; i < s->nP; i++) {
        const ChtPatch *p = &s->p[i];
        if (p->role == CP_INLET) { nin++; if (!(p->U > 0)) str_catf(&P, "Inlet %s: give a speed into the domain above 0 m/s.\n", p->name); fixedT++; }
        if (p->role == CP_OUTLET) nout++;
        if (p->role == CP_WALL && (p->heat == CT_FIXED || p->heat == CT_CONVECT)) fixedT++;
        if (p->role == CP_WALL && p->heat == CT_CONVECT && !(p->h > 0)) str_catf(&P, "%s: the heat transfer coefficient must be above 0.\n", p->name);
    }
    if (!s->nR) str_catf(&P, "No regions: the mesh needs cellZones.\n");
    if (nf == 0 && ns > 0 && !fixedT && !s->transient)
        str_catf(&P, "Nothing sets a temperature: with only solids and no fixed-temperature or convection wall, a steady answer doesn't exist "
                     "(any temperature would do). Fix one wall's temperature, or give one convection to surroundings.\n");
    if (nin && !nout) str_catf(&P, "An inlet but no outlet: the fluid that comes in has nowhere to go. Make one patch an outlet.\n");
    if (nout && !nin) str_catf(&W, "An outlet but no inlet: the flow is driven by buoyancy only (natural convection).\n");
    if (nf && !nin && !(fabs(s->g[0]) + fabs(s->g[1]) + fabs(s->g[2]) > 0))
        str_catf(&W, "No inlet and no gravity: the fluid stays still, so heat crosses it by conduction alone.\n");
    if (!s->transient && Qtot > 0 && !fixedT && !nout)
        str_catf(&P, "%.3g W is made but nothing carries it away (no inlet, no fixed-temperature or convection wall): steady, the temperature "
                     "would rise for ever. Add a way out for the heat, or run it transient.\n", Qtot);
    snprintf(problems, (size_t)pcap, "%s", P.s ? P.s : "");
    snprintf(warn, (size_t)wcap, "%s", W.s ? W.s : "");
    str_free(&P); str_free(&W);
}

/* ---------------------------------------------------------------------------------------------- writing */
typedef struct { const char *casePath; char *kept; int kcap; bool ok; char err[300]; } Wr;

static const char *HDR = "/*--------------------------------*- C++ -*----------------------------------*\\\n"
                         "  Written by Q-Continuum (Physics > Setup, conjugate heat transfer) from cht.ini\n"
                         "\\*---------------------------------------------------------------------------*/\n";

static void foam_header(Str *b, const char *cls, const char *loc, const char *obj)
{
    str_catf(b, "%sFoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       %s;\n", HDR, cls);
    if (loc) str_catf(b, "    location    \"%s\";\n", loc);
    str_catf(b, "    object      %s;\n}\n\n", obj);
}

/* a file written from the setup; one changed by hand since the last write (its copy in .qc-cht) is kept, named in kept */
static void put(Wr *w, const char *rel, Str *b)
{
    if (!w->ok) { str_free(b); return; }
    char path[PL_PATH], base[PL_PATH], dir[PL_PATH];
    pl_join(path, sizeof path, w->casePath, rel);
    snprintf(base, sizeof base, "%s/.qc-cht/%s", w->casePath, rel);
    const char *text = b->s ? b->s : "";
    char *now = pl_read_file(path, NULL), *was = pl_read_file(base, NULL);
    bool handEdited = now && was && strcmp(now, was) && strcmp(now, text);
    if (now && !was && strcmp(now, text)) {                /* there before the setup: a copy kept beside it */
        char bak[PL_PATH + 8]; snprintf(bak, sizeof bak, "%s.bak", path);
        pl_write_file(bak, now, strlen(now));
    }
    free(now); free(was);
    if (handEdited) {
        if (w->kept && (int)(strlen(w->kept) + strlen(rel) + 2) < w->kcap) { strcat(w->kept, rel); strcat(w->kept, "\n"); }
        str_free(b);
        return;
    }
    pl_strlcpy(dir, path, sizeof dir); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    pl_mkdirs(dir);
    pl_strlcpy(dir, base, sizeof dir); sl = strrchr(dir, '/'); if (sl) *sl = 0;
    pl_mkdirs(dir);
    if (!pl_write_file(path, text, strlen(text))) { w->ok = false; snprintf(w->err, sizeof w->err, "Could not write %s", rel); }
    else pl_write_file(base, text, strlen(text));
    str_free(b);
}

static void w_region_props(Wr *w, const ChtSetup *s)
{
    Str b; str_init(&b);
    foam_header(&b, "dictionary", "constant", "regionProperties");
    str_catf(&b, "regions\n(\n    fluid       (");
    for (int i = 0, n = 0; i < s->nR; i++) if (s->r[i].kind == CR_FLUID) str_catf(&b, "%s%s", n++ ? " " : "", s->r[i].name);
    str_catf(&b, ")\n    solid       (");
    for (int i = 0, n = 0; i < s->nR; i++) if (s->r[i].kind == CR_SOLID) str_catf(&b, "%s%s", n++ ? " " : "", s->r[i].name);
    str_catf(&b, ")\n);\n");
    put(w, "constant/regionProperties", &b);
    str_init(&b);
    foam_header(&b, "uniformDimensionedVectorField", "constant", "g");
    str_catf(&b, "dimensions      [0 1 -2 0 0 0 0];\nvalue           (%.9g %.9g %.9g);\n", s->g[0], s->g[1], s->g[2]);
    put(w, "constant/g", &b);
}

static void w_thermo(Wr *w, const ChtRegion *r)
{
    const ChtMat *m = mat_of(r);
    char rel[200]; Str b; str_init(&b);
    snprintf(rel, sizeof rel, "constant/%s", r->name);
    foam_header(&b, "dictionary", rel, "thermophysicalProperties");
    if (r->kind == CR_FLUID) {
        str_catf(&b, "// %s near 300 K: rho %.4g kg/m3, cp %.4g J/(kg K), k %.4g W/(m K), mu %.4g Pa s, Pr %.3g\n", m->name, m->rho, m->cp, m->k, m->mu, cht_prandtl(m));
        str_catf(&b, "thermoType\n{\n    type            heRhoThermo;\n    mixture         pureMixture;\n    transport       const;\n    thermo          hConst;\n"
                     "    equationOfState %s;\n    specie          specie;\n    energy          sensibleEnthalpy;\n}\n\n", m->gas ? "perfectGas" : "rhoConst");
        str_catf(&b, "mixture\n{\n    specie\n    {\n        molWeight       %.6g;\n    }\n", m->molW);
        if (!m->gas) str_catf(&b, "    equationOfState\n    {\n        rho             %.6g;\n    }\n", m->rho);
        str_catf(&b, "    thermodynamics\n    {\n        Cp              %.6g;\n        Hf              0;\n    }\n"
                     "    transport\n    {\n        mu              %.6g;\n        Pr              %.6g;\n    }\n}\n", m->cp, m->mu, cht_prandtl(m));
    } else {
        str_catf(&b, "// %s near 300 K: rho %.4g kg/m3, cp %.4g J/(kg K), k %.4g W/(m K)\n", m->name, m->rho, m->cp, m->k);
        str_catf(&b, "thermoType\n{\n    type            heSolidThermo;\n    mixture         pureMixture;\n    transport       constIso;\n    thermo          hConst;\n"
                     "    equationOfState rhoConst;\n    specie          specie;\n    energy          sensibleEnthalpy;\n}\n\n");
        str_catf(&b, "mixture\n{\n    specie\n    {\n        molWeight   %.6g;\n    }\n    transport\n    {\n        kappa   %.6g;\n    }\n"
                     "    thermodynamics\n    {\n        Hf      0;\n        Cp      %.6g;\n    }\n    equationOfState\n    {\n        rho     %.6g;\n    }\n}\n",
                 m->molW, m->k, m->cp, m->rho);
    }
    char rp[240]; snprintf(rp, sizeof rp, "%s/thermophysicalProperties", rel);
    put(w, rp, &b);
    str_init(&b);
    foam_header(&b, "dictionary", rel, "radiationProperties");
    str_catf(&b, "radiation       off;\nradiationModel  none;\n");
    snprintf(rp, sizeof rp, "%s/radiationProperties", rel);
    put(w, rp, &b);
}

static void w_turb(Wr *w, const ChtSetup *s, const ChtRegion *r)
{
    char rel[200], rp[240]; Str b; str_init(&b);
    snprintf(rel, sizeof rel, "constant/%s", r->name);
    foam_header(&b, "dictionary", rel, "turbulenceProperties");
    if (s->turbulent) str_catf(&b, "simulationType  RAS;\n\nRAS\n{\n    RASModel        kOmegaSST;\n    turbulence      on;\n    printCoeffs     on;\n}\n");
    else str_catf(&b, "simulationType  laminar;\n");
    snprintf(rp, sizeof rp, "%s/turbulenceProperties", rel);
    put(w, rp, &b);
}

static void w_system_region(Wr *w, const ChtSetup *s, const ChtRegion *r, double volume)
{
    char rel[200], rp[240]; Str b; str_init(&b);
    snprintf(rel, sizeof rel, "system/%s", r->name);
    foam_header(&b, "dictionary", rel, "fvSchemes");
    if (r->kind == CR_FLUID) {
        str_catf(&b, "ddtSchemes\n{\n    default         %s;\n}\n\ngradSchemes\n{\n    default         Gauss linear;\n}\n\n", s->transient ? "Euler" : "steadyState");
        str_catf(&b, "divSchemes\n{\n    default         none;\n    div(phi,U)      bounded Gauss %s;\n    energy          bounded Gauss upwind;\n"
                     "    div(phi,K)      $energy;\n    div(phi,h)      $energy;\n    turbulence      bounded Gauss upwind;\n    div(phi,k)      $turbulence;\n"
                     "    div(phi,omega)  $turbulence;\n    div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear;\n}\n\n", s->transient ? "linearUpwind grad(U)" : "upwind");
        str_catf(&b, "laplacianSchemes\n{\n    default         Gauss linear corrected;\n}\n\ninterpolationSchemes\n{\n    default         linear;\n}\n\n"
                     "snGradSchemes\n{\n    default         corrected;\n}\n\nwallDist\n{\n    method          meshWave;\n}\n");
    } else {
        str_catf(&b, "ddtSchemes\n{\n    default         %s;\n}\n\ngradSchemes\n{\n    default         Gauss linear;\n}\n\ndivSchemes\n{\n    default         none;\n}\n\n"
                     "laplacianSchemes\n{\n    default             none;\n    laplacian(alpha,h)  Gauss linear corrected;\n}\n\ninterpolationSchemes\n{\n"
                     "    default         linear;\n}\n\nsnGradSchemes\n{\n    default         corrected;\n}\n", s->transient ? "Euler" : "steadyState");
    }
    snprintf(rp, sizeof rp, "%s/fvSchemes", rel);
    put(w, rp, &b);

    str_init(&b);
    foam_header(&b, "dictionary", rel, "fvSolution");
    if (r->kind == CR_FLUID) {
        str_catf(&b, "solvers\n{\n    \"(rho|rhoFinal)\"\n    {\n        solver          PCG;\n        preconditioner  DIC;\n        tolerance       1e-7;\n        relTol          0;\n    }\n\n"
                     "    p_rgh\n    {\n        solver          GAMG;\n        tolerance       1e-7;\n        relTol          0.01;\n        smoother        GaussSeidel;\n    }\n\n"
                     "    p_rghFinal\n    {\n        $p_rgh;\n        relTol          0;\n    }\n\n"
                     "    \"(U|h|k|omega)\"\n    {\n        solver          PBiCGStab;\n        preconditioner  DILU;\n        tolerance       1e-7;\n        relTol          0.1;\n    }\n\n"
                     "    \"(U|h|k|omega)Final\"\n    {\n        $U;\n        relTol          0;\n    }\n}\n\n");
        if (s->transient)
            str_catf(&b, "PIMPLE\n{\n    momentumPredictor   yes;\n    nCorrectors         2;\n    nNonOrthogonalCorrectors 0;\n    pRefCell            0;\n    pRefValue           1e5;\n}\n\n"
                         "relaxationFactors\n{\n    equations\n    {\n        \"h.*\"           1;\n        \"U.*\"           1;\n    }\n}\n");
        else
            str_catf(&b, "SIMPLE\n{\n    momentumPredictor yes;\n    nNonOrthogonalCorrectors 0;\n    pRefCell        0;\n    pRefValue       1e5;\n%s}\n\n"
                         "relaxationFactors\n{\n    fields\n    {\n        rho             1;\n        p_rgh           0.7;\n    }\n    equations\n    {\n"
                         "        U               0.3;\n        h               0.7;\n        \"(k|omega)\"     0.7;\n    }\n}\n",
                     mat_of(r)->gas ? "    rhoMin          0.2;\n    rhoMax          5;\n" : "");
    } else {
        /* steady: the solid solved to the end every iteration, unrelaxed. With the tutorials' relTol 0.1 and 0.7 a 5 W plate
           had passed 0.13 W to the air after 1500 iterations (test cht_openfoam, 2026-11-26); solved fully, all 5 W */
        if (s->transient)
            str_catf(&b, "solvers\n{\n    h\n    {\n        solver           PCG;\n        preconditioner   DIC;\n        tolerance        1e-06;\n        relTol           0.1;\n    }\n\n"
                         "    hFinal\n    {\n        $h;\n        relTol           0;\n    }\n}\n\nPIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n}\n");
        else
            str_catf(&b, "solvers\n{\n    h\n    {\n        solver           PCG;\n        preconditioner   DIC;\n        tolerance        1e-08;\n        relTol           0;\n    }\n}\n\n"
                         "SIMPLE\n{\n    nNonOrthogonalCorrectors 0;\n}\n\nrelaxationFactors\n{\n    equations\n    {\n        h               1;\n    }\n}\n");
    }
    snprintf(rp, sizeof rp, "%s/fvSolution", rel);
    put(w, rp, &b);

    /* a solid's heat: an absolute source on h over the whole region, W (cpuCabinet's v_CPU) */
    snprintf(rp, sizeof rp, "%s/fvOptions", rel);
    if (r->kind == CR_SOLID && r->Q != 0) {
        str_init(&b);
        foam_header(&b, "dictionary", rel, "fvOptions");
        str_catf(&b, "// %.6g W made in %s", r->Q, r->name);
        if (volume > 0) str_catf(&b, " (%.4g m3 at mesh time: %.4g W/m3)", volume, r->Q / volume);
        str_catf(&b, "\n%s_heat\n{\n    type            scalarSemiImplicitSource;\n    active          true;\n    selectionMode   all;\n"
                     "    volumeMode      absolute;\n    sources\n    {\n        h           (%.9g 0);\n    }\n}\n", r->name, r->Q);
        put(w, rp, &b);
    } else {
        char p[PL_PATH], base[PL_PATH]; pl_join(p, sizeof p, w->casePath, rp);
        snprintf(base, sizeof base, "%s/.qc-cht/%s", w->casePath, rp);
        if (pl_is_file(base)) { remove(p); remove(base); }     /* the heat taken away again: only our own file goes */
    }
}

/* ---- fields */
static void field_head(Str *b, const char *cls, const char *region, const char *obj, const char *dims, const char *internal)
{
    char loc[200]; snprintf(loc, sizeof loc, "0/%s", region);
    foam_header(b, cls, loc, obj);
    str_catf(b, "dimensions      %s;\n\ninternalField   %s;\n\nboundaryField\n{\n    #includeEtc \"caseDicts/setConstraintTypes\"\n\n", dims, internal);
}

static void entry(Str *b, const char *name, bool pattern, const char *body)
{
    str_catf(b, "    %s%s%s\n    {\n%s    }\n\n", pattern ? "\"" : "", name, pattern ? "\"" : "", body);
}

static void temperature_bc(char *out, size_t cap, const ChtPatch *p, bool fluid, double T0)
{
    const char *km = fluid ? "fluidThermo" : "solidThermo";
    switch (p->role == CP_INLET ? -1 : p->role == CP_OUTLET ? -2 : p->heat) {
    case -1: snprintf(out, cap, "        type            fixedValue;\n        value           uniform %.9g;\n", p->Tin); break;
    case -2: snprintf(out, cap, "        type            inletOutlet;\n        inletValue      uniform %.9g;\n        value           uniform %.9g;\n", T0, T0); break;
    case CT_FIXED: snprintf(out, cap, "        type            fixedValue;\n        value           uniform %.9g;\n", p->T); break;
    case CT_FLUX:
        snprintf(out, cap, "        type            externalWallHeatFluxTemperature;\n        mode            flux;\n        q               uniform %.9g;\n"
                           "        kappaMethod     %s;\n        value           uniform %.9g;\n", p->q, km, T0);
        break;
    case CT_CONVECT:
        snprintf(out, cap, "        type            externalWallHeatFluxTemperature;\n        mode            coefficient;\n        h               uniform %.9g;\n"
                           "        Ta              constant %.9g;\n        kappaMethod     %s;\n        value           uniform %.9g;\n", p->h, p->Ta, km, T0);
        break;
    default: snprintf(out, cap, "        type            zeroGradient;\n"); break;
    }
}

static void w_fields(Wr *w, const ChtSetup *s, const ChtRegion *r)
{
    bool fluid = r->kind == CR_FLUID;
    char rel[200], body[700], iname[96], internal[128];
    snprintf(iname, sizeof iname, "%s_to_.*", r->name);
    Str b;

    /* T */
    str_init(&b);
    snprintf(internal, sizeof internal, "uniform %.9g", r->T0);
    field_head(&b, "volScalarField", r->name, "T", "[0 0 0 1 0 0 0]", internal);
    entry(&b, ".*", true, "        type            zeroGradient;\n");
    snprintf(body, sizeof body, "        type            compressible::turbulentTemperatureCoupledBaffleMixed;\n        Tnbr            T;\n"
                                "        kappaMethod     %s;\n        value           uniform %.9g;\n", fluid ? "fluidThermo" : "solidThermo", r->T0);
    entry(&b, iname, true, body);
    for (int i = 0; i < s->nP; i++) {
        const ChtPatch *p = &s->p[i];
        if (!fluid && p->role != CP_WALL) continue;              /* an inlet's or outlet's faces are the fluid's */
        temperature_bc(body, sizeof body, p, fluid, r->T0);
        entry(&b, p->name, false, body);
    }
    str_catf(&b, "}\n");
    snprintf(rel, sizeof rel, "0.orig/%s/T", r->name); put(w, rel, &b);

    /* p: every solver reads it; calculated, from p_rgh in a fluid */
    str_init(&b);
    field_head(&b, "volScalarField", r->name, "p", "[1 -1 -2 0 0 0 0]", "uniform 1e5");
    entry(&b, ".*", true, "        type            calculated;\n        value           uniform 1e5;\n");
    str_catf(&b, "}\n");
    snprintf(rel, sizeof rel, "0.orig/%s/p", r->name); put(w, rel, &b);
    if (!fluid) return;

    /* U */
    str_init(&b);
    snprintf(internal, sizeof internal, "uniform (%.9g %.9g %.9g)", r->U0[0], r->U0[1], r->U0[2]);
    field_head(&b, "volVectorField", r->name, "U", "[0 1 -1 0 0 0 0]", internal);
    entry(&b, ".*", true, "        type            noSlip;\n");
    for (int i = 0; i < s->nP; i++) {
        const ChtPatch *p = &s->p[i];
        if (p->role == CP_INLET) {
            snprintf(body, sizeof body, "        type            surfaceNormalFixedValue;\n        refValue        uniform %.9g;   // into the domain\n", -p->U);
            entry(&b, p->name, false, body);
        } else if (p->role == CP_OUTLET)
            entry(&b, p->name, false, "        type            inletOutlet;\n        inletValue      uniform (0 0 0);\n        value           uniform (0 0 0);\n");
    }
    str_catf(&b, "}\n");
    snprintf(rel, sizeof rel, "0.orig/%s/U", r->name); put(w, rel, &b);

    /* p_rgh: walls and inlets take the flux the pressure gradient needs; an outlet holds p with gravity's share taken off */
    str_init(&b);
    field_head(&b, "volScalarField", r->name, "p_rgh", "[1 -1 -2 0 0 0 0]", "uniform 1e5");
    entry(&b, ".*", true, "        type            fixedFluxPressure;\n        value           uniform 1e5;\n");
    for (int i = 0; i < s->nP; i++)
        if (s->p[i].role == CP_OUTLET) entry(&b, s->p[i].name, false, "        type            prghPressure;\n        p               uniform 1e5;\n        value           uniform 1e5;\n");
    str_catf(&b, "}\n");
    snprintf(rel, sizeof rel, "0.orig/%s/p_rgh", r->name); put(w, rel, &b);
    if (!s->turbulent) return;

    /* k-omega SST: an inlet's turbulence from 5 % intensity and a mixing length of a tenth of a typical size (0.01 m) */
    static const struct { const char *name, *dims, *internal, *wall, *in, *out; } TF[] = {
        { "k", "[0 2 -2 0 0 0 0]", "uniform 0.001", "        type            kqRWallFunction;\n        value           uniform 0.001;\n",
          "        type            turbulentIntensityKineticEnergyInlet;\n        intensity       0.05;\n        value           uniform 0.001;\n",
          "        type            inletOutlet;\n        inletValue      uniform 0.001;\n        value           uniform 0.001;\n" },
        { "omega", "[0 0 -1 0 0 0 0]", "uniform 1", "        type            omegaWallFunction;\n        value           uniform 1;\n",
          "        type            turbulentMixingLengthFrequencyInlet;\n        mixingLength    0.01;\n        value           uniform 1;\n",
          "        type            inletOutlet;\n        inletValue      uniform 1;\n        value           uniform 1;\n" },
        { "nut", "[0 2 -1 0 0 0 0]", "uniform 0", "        type            nutkWallFunction;\n        value           uniform 0;\n",
          "        type            calculated;\n        value           uniform 0;\n", "        type            calculated;\n        value           uniform 0;\n" },
        { "alphat", "[1 -1 -1 0 0 0 0]", "uniform 0", "        type            compressible::alphatWallFunction;\n        Prt             0.85;\n        value           uniform 0;\n",
          "        type            calculated;\n        value           uniform 0;\n", "        type            calculated;\n        value           uniform 0;\n" },
    };
    for (size_t f = 0; f < sizeof TF / sizeof TF[0]; f++) {
        str_init(&b);
        field_head(&b, "volScalarField", r->name, TF[f].name, TF[f].dims, TF[f].internal);
        entry(&b, ".*", true, TF[f].wall);
        for (int i = 0; i < s->nP; i++) {
            if (s->p[i].role == CP_INLET) entry(&b, s->p[i].name, false, TF[f].in);
            else if (s->p[i].role == CP_OUTLET) entry(&b, s->p[i].name, false, TF[f].out);
        }
        str_catf(&b, "}\n");
        snprintf(rel, sizeof rel, "0.orig/%s/%s", r->name, TF[f].name); put(w, rel, &b);
    }
}

/* the base dictionaries: controlDict (written whole: a CHT case's run controls are the setup's), fvSchemes, fvSolution;
   reports: per fluid region with an outlet, the flow-weighted outlet temperature; per region the wall heat flow */
static void w_base(Wr *w, const ChtSetup *s)
{
    Str b; str_init(&b);
    foam_header(&b, "dictionary", "system", "controlDict");
    str_catf(&b, "application     %s;\n\nstartFrom       latestTime;\nstartTime       0;\nstopAt          endTime;\nendTime         %.9g;\n",
             s->transient ? "chtMultiRegionFoam" : "chtMultiRegionSimpleFoam", s->endTime);
    if (s->transient)
        str_catf(&b, "deltaT          0.001;\nadjustTimeStep  yes;\nmaxCo           0.5;\nmaxDi           10;\n\nwriteControl    adjustable;\nwriteInterval   %.9g;\n", s->writeEvery);
    else str_catf(&b, "deltaT          1;\n\nwriteControl    timeStep;\nwriteInterval   %.9g;\npurgeWrite      3;\n", s->writeEvery);
    str_catf(&b, "writeFormat     ascii;\nwritePrecision  8;\nwriteCompression off;\ntimeFormat      general;\ntimePrecision   8;\nrunTimeModifiable true;\n\nfunctions\n{\n");
    for (int i = 0; i < s->nR; i++) {
        const ChtRegion *r = &s->r[i];
        str_catf(&b, "    wallHeat_%s\n    {\n        type            wallHeatFlux;\n        libs            (fieldFunctionObjects);\n        region          %s;\n"
                     "        writeControl    writeTime;\n        writeFields     no;\n    }\n\n", r->name, r->name);
        if (r->kind != CR_FLUID) continue;
        Str pat; str_init(&pat);
        int no = 0;
        for (int k = 0; k < s->nP; k++) if (s->p[k].role == CP_OUTLET) str_catf(&pat, "%s%s", no++ ? " " : "", s->p[k].name);
        if (no) {
            str_catf(&b, "    outletT_%s\n    {\n        type            surfaceFieldValue;\n        libs            (fieldFunctionObjects);\n        region          %s;\n"
                         "        writeControl    timeStep;\n        writeInterval   %d;\n        log             yes;\n        writeFields     no;\n"
                         "        regionType      patch;\n        name            %s;\n        operation       weightedAverage;\n        weightField     phi;\n"
                         "        fields          (T);\n    }\n\n", r->name, r->name, s->transient ? 1 : 10, s->p[0].role == CP_OUTLET && no == 1 ? pat.s : strtok(pat.s, " "));
        }
        str_free(&pat);
    }
    str_catf(&b, "}\n");
    put(w, "system/controlDict", &b);
    str_init(&b);
    foam_header(&b, "dictionary", "system", "fvSchemes");
    str_catf(&b, "// each region's schemes are in system/<region>/fvSchemes\nddtSchemes\n{\n}\n\ngradSchemes\n{\n}\n\ndivSchemes\n{\n}\n\n"
                 "laplacianSchemes\n{\n}\n\ninterpolationSchemes\n{\n}\n\nsnGradSchemes\n{\n}\n");
    put(w, "system/fvSchemes", &b);
    str_init(&b);
    foam_header(&b, "dictionary", "system", "fvSolution");
    if (s->transient) str_catf(&b, "PIMPLE\n{\n    nOuterCorrectors 1;\n}\n");
    else str_catf(&b, "// each region's solvers are in system/<region>/fvSolution\n");
    put(w, "system/fvSolution", &b);
}

/* the cells in no zone made a zone for splitMeshRegions (whose own name for them, domainN, follows the cell order) */
static void w_topo(Wr *w, const ChtSetup *s)
{
    const ChtRegion *rest = NULL;
    for (int i = 0; i < s->nR; i++) if (s->r[i].rest) rest = &s->r[i];
    char p[PL_PATH], base[PL_PATH];
    pl_join(p, sizeof p, w->casePath, "system/topoSetDict");
    snprintf(base, sizeof base, "%s/.qc-cht/system/topoSetDict", w->casePath);
    if (!rest) { if (pl_is_file(base)) { remove(p); remove(base); } return; }
    Str b; str_init(&b);
    foam_header(&b, "dictionary", "system", "topoSetDict");
    str_catf(&b, "// the cells in none of the zones become the cellZone %s\nactions\n(\n    {\n        name    %s_cells;\n        type    cellSet;\n"
                 "        action  new;\n        source  zoneToCell;\n        zones   (", rest->name, rest->name);
    for (int i = 0, n = 0; i < s->nR; i++) if (!s->r[i].rest) str_catf(&b, "%s%s", n++ ? " " : "", s->r[i].name);
    str_catf(&b, ");\n    }\n    {\n        name    %s_cells;\n        type    cellSet;\n        action  invert;\n    }\n"
                 "    {\n        name    %s;\n        type    cellZoneSet;\n        action  new;\n        source  setToCellZone;\n        set     %s_cells;\n    }\n);\n",
             rest->name, rest->name, rest->name);
    put(w, "system/topoSetDict", &b);
}

/* a region's volume from the mesh, for the heat density shown beside a solid's source (0 when unknown) */
static double zone_volume(const char *casePath, const char *zone) { (void)casePath; (void)zone; return 0; }

bool cht_write(const char *casePath, const ChtSetup *s, char *kept, int kcap, char *err, int ecap)
{
    if (kept && kcap) kept[0] = 0;
    char probs[1200], warn[800];
    cht_check(s, probs, sizeof probs, warn, sizeof warn);
    if (probs[0]) { snprintf(err, (size_t)ecap, "%s", probs); return false; }
    Wr w = { casePath, kept, kcap, true, "" };
    w_region_props(&w, s);
    for (int i = 0; i < s->nR; i++) {
        const ChtRegion *r = &s->r[i];
        w_thermo(&w, r);
        if (r->kind == CR_FLUID) w_turb(&w, s, r);
        w_system_region(&w, s, r, zone_volume(casePath, r->name));
        w_fields(&w, s, r);
    }
    w_base(&w, s);
    w_topo(&w, s);
    if (!w.ok) { snprintf(err, (size_t)ecap, "%s", w.err); return false; }
    return cht_save(casePath, s);
}
