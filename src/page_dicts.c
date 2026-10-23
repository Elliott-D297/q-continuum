/* page_dicts.c -- Physics, Numerics and Files pages (curated cards + generic dictionary editors) */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "program.h"
#include "fieldinfo.h"
#include "physics.h"

static bool has_field(const char *name)
{
    for (int i = 0; i < A.cs->fields.n; i++) if (!strcmp(A.cs->fields.names[i], name)) return true;
    return false;
}

static void create_turb_field(const char *name);

/* "(nx ny nz)" with nx*ny*nz = np for simple/hierarchical decomposition; a 2-D case (empty patches) keeps z whole */
void decomp_split(Case *c, long np, char *out, int cap)
{
    int dirs = 3;
    for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].type, "empty")) dirs = 2;
    long d[3]; decomp_split_n(np, dirs, d);
    snprintf(out, (size_t)cap, "(%ld %ld %ld)", d[0], d[1], d[2]);
}

/* list fields a turbulence model needs and flag the missing ones, each with a button that creates it */
static float required_fields(float x, float y, float w, const char *const *need, int n)
{
    float bx = x;
    ui_text(F_REG, 12.5f, "Needs", bx, y + 4, TH.text3);
    bx += 44;
    int missing = 0;
    for (int i = 0; i < n; i++) {
        bool ok = has_field(need[i]);
        if (!ok) missing++;
        bx += ui_badge(bx, y, ok ? ICON_CHECK : ICON_X, need[i], ok ? TH.ok : TH.err, ok ? TH.okSoft : TH.errSoft) + 6;
    }
    if (!missing) return 32;
    float h = 32;
    bx = x;
    for (int i = 0; i < n; i++) {
        if (has_field(need[i])) continue;
        char lab[64]; snprintf(lab, sizeof lab, "Create %s", need[i]);
        float bw = ui_button_width(ICON_PLUS, lab);
        if (ui_button(ui_idn(ui_id("mk-turb"), i), (Rectangle){ bx, y + 32, bw, 32 }, ICON_PLUS, lab, BTN_SECONDARY)) create_turb_field(need[i]);
        bx += bw + 8;
        h = 74;
    }
    (void)w;
    return h;
}


/* the "uniform X" value of a scalar field's internalField, or -1 */
static double uniform_of(const char *field)
{
    char rel[300], v[160] = "";
    snprintf(rel, sizeof rel, "%s/%s", A.cs->fieldDir, field);
    FDFile *f = has_field(field) ? case_dict(A.cs, rel) : NULL;
    if (!f || !fd_get(f, "internalField", v, sizeof v)) return -1;
    const char *s = v; while (isspace((unsigned char)*s)) s++;
    if (!strncmp(s, "uniform", 7)) s += 7;
    char *e; double d = strtod(s, &e);
    return e == s ? -1 : d;
}

/* write a turbulence field with the usual conditions: wall functions on walls, the U inlet/outlet split
   elsewhere, constraint patches as they are. Its value comes from the other turbulence fields when they
   exist (omega = epsilon / (Cmu k) and back), else a modest guess the toast asks to check. */
static void create_turb_field(const char *name)
{
    Case *c = A.cs;
    double k = uniform_of("k"), eps = uniform_of("epsilon"), om = uniform_of("omega");
    const char *dims = "[0 2 -1 0 0 0 0]", *wallT = NULL;
    double v = 0; bool guess = false;
    if (!strcmp(name, "k"))            { dims = "[0 2 -2 0 0 0 0]"; wallT = "kqRWallFunction";
                                         if (eps > 0 && om > 0) v = eps / (0.09 * om); else { v = 0.375; guess = true; } }
    else if (!strcmp(name, "epsilon")) { dims = "[0 2 -3 0 0 0 0]"; wallT = "epsilonWallFunction";
                                         if (k > 0 && om > 0) v = 0.09 * k * om; else { v = 14.855; guess = true; } }
    else if (!strcmp(name, "omega"))   { dims = "[0 0 -1 0 0 0 0]"; wallT = "omegaWallFunction";
                                         if (k > 0 && eps > 0) v = eps / (0.09 * k); else { v = 440.15; guess = true; } }
    else if (!strcmp(name, "nut"))     { wallT = "nutkWallFunction"; }
    else if (!strcmp(name, "nuTilda")) { wallT = NULL; v = 0; }
    char val[64]; snprintf(val, sizeof val, "%.6g", v);
    Str b; str_init(&b);
    str_catf(&b, "dimensions      %s;\n\ninternalField   uniform %s;\n\nboundaryField\n{\n", dims, val);
    for (int i = 0; i < c->nPatches; i++) {
        const PatchInfo *p = &c->patches[i];
        if (p->pattern) continue;
        const char *t = p->type;
        str_catf(&b, "    %s\n    {\n", p->name);
        if (!strcmp(t, "empty") || !strcmp(t, "symmetryPlane") || !strcmp(t, "symmetry") || !strcmp(t, "wedge") ||
            !strcmp(t, "cyclic") || !strcmp(t, "cyclicAMI"))
            str_catf(&b, "        type            %s;\n", t);
        else if (!strcmp(t, "wall"))
            str_catf(&b, wallT ? "        type            %s;\n        value           $internalField;\n" : "        type            fixedValue;\n        value           uniform 0;\n", wallT);
        else if (!strcmp(name, "nut"))
            str_cat(&b, "        type            calculated;\n        value           $internalField;\n");
        else {
            char ut[96]; bc_type_on(c, "U", i, ut, sizeof ut);
            bool inlet = strstr(ut, "fixedValue") || strstr(ut, "flowRate") || strstr(ut, "Inlet") || strstr(ut, "surfaceNormal");
            if (inlet && !strstr(ut, "inletOutlet")) str_cat(&b, "        type            fixedValue;\n        value           $internalField;\n");
            else str_cat(&b, "        type            inletOutlet;\n        inletValue      $internalField;\n        value           $internalField;\n");
        }
        str_cat(&b, "    }\n");
    }
    str_cat(&b, "}\n");
    char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, name);
    if (case_create_dict(c, rel, "volScalarField", b.s)) {
        if (guess) ui_toast(TOAST_WARN, "Created %s with a guessed value %s: set it for your flow on the Boundaries page.", rel, val);
        else ui_toast(TOAST_OK, "Created %s (%s, from the other turbulence fields) with wall functions on the walls.", rel, val);
        case_rescan(c);
    } else ui_toast(TOAST_ERR, "Could not write %s", rel);
    str_free(&b);
}

/* fields the case's turbulence model needs that fieldDir doesn't have; 0 when none or no model */
void case_turbulence(Case *c, char *st, int stCap, char *model, int mCap)
{
    st[0] = model[0] = 0;
    FDFile *t = case_dict(c, case_rpath(c, "constant/turbulenceProperties"));
    if (!t) t = case_dict(c, case_rpath(c, "constant/momentumTransport"));
    if (!t) return;
    fd_get(t, "simulationType", st, stCap);
    if (!strcmp(st, "RAS")) { if (!fd_get(t, "RAS/RASModel", model, mCap)) fd_get(t, "RAS/model", model, mCap); }
    else if (!strcmp(st, "LES")) { if (!fd_get(t, "LES/LESModel", model, mCap)) fd_get(t, "LES/model", model, mCap); }
}

int turb_missing_fields(Case *c, char *msg, int cap)
{
    msg[0] = 0;
    FDFile *t = case_dict(c, case_rpath(c, "constant/turbulenceProperties"));
    if (!t) t = case_dict(c, case_rpath(c, "constant/momentumTransport"));
    if (!t) return 0;
    char st[32] = "", m[64] = "";
    fd_get(t, "simulationType", st, sizeof st);
    if (!strcmp(st, "RAS")) { if (!fd_get(t, "RAS/RASModel", m, sizeof m)) fd_get(t, "RAS/model", m, sizeof m); }
    else if (!strcmp(st, "LES")) { if (!fd_get(t, "LES/LESModel", m, sizeof m)) fd_get(t, "LES/model", m, sizeof m); }
    else return 0;
    const char *need[6]; int nn = turb_model_fields(m, need), miss = 0, k = 0;
    for (int i = 0; i < nn; i++) {
        bool have = false;
        for (int j = 0; j < c->fields.n; j++) if (!strcmp(c->fields.names[j], need[i])) have = true;
        if (!have && k < cap - 1) { k += snprintf(msg + k, (size_t)(cap - k), "%s%s", miss ? ", " : "", need[i]); miss++; }
    }
    return miss;
}

/* create a radiation field file with sensible conditions on every patch */
static void create_radiation_field(const char *name)
{
    Case *c = A.cs;
    bool isG = !strcmp(name, "G");
    Str b; str_init(&b);
    str_catf(&b, "dimensions      [1 0 -3 0 0 0 0];\n\ninternalField   uniform 0;\n\nboundaryField\n{\n");
    for (int i = 0; i < c->nPatches; i++) {
        const PatchInfo *p = &c->patches[i];
        if (!strcmp(p->type, "empty") || !strcmp(p->type, "symmetryPlane") || !strcmp(p->type, "wedge"))
            str_catf(&b, "    %s\n    {\n        type            %s;\n    }\n", p->name, p->type);
        else if (isG)
            str_catf(&b, "    %s\n    {\n        type            MarshakRadiation;\n        emissivityMode  lookup;\n"
                         "        emissivity      uniform 1;\n        value           uniform 0;\n    }\n", p->name);
        else
            str_catf(&b, "    %s\n    {\n        type            greyDiffusiveRadiation;\n        emissivityMode  lookup;\n"
                         "        emissivity      uniform 1;\n        value           uniform 0;\n    }\n", p->name);
    }
    str_cat(&b, "}\n");
    char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, name);
    if (case_create_dict(c, rel, "volScalarField", b.s)) {
        ui_toast(TOAST_OK, "Created %s with %s on every wall. Review the emissivities.", rel, isG ? "MarshakRadiation" : "greyDiffusiveRadiation");
        case_rescan(c);
    } else ui_toast(TOAST_ERR, "Could not write %s", rel);
    str_free(&b);
}

/* does any entry of dict `parent` in f apply to `name` (exact keyword or "regex")? */
static bool dict_has_key_for(FDFile *f, const char *parent, const char *name)
{
    FDEntry *d = f ? fd_find(f, parent) : NULL;
    if (!d) return false;
    for (int i = 0; i < d->nChild; i++) if (pl_foam_key_matches(d->child[i].key, name)) return true;
    return false;
}

/* entries OpenFOAM reads for a radiation model but a case made for another model (or none) lacks:
   fvDOM stops on a missing fvDOMCoeffs/nPhi/nTheta, div(Ji,Ii_h) or Ii solver, P1 on a missing G solver,
   both on a missing absorption/scatter/soot model */
static void radiation_model_defaults(Case *c, FDFile *rp)
{
    char model[32] = ""; fd_get(rp, "radiationModel", model, sizeof model);
    bool fv = !strcmp(model, "fvDOM");
    if (!fv && strcmp(model, "P1")) return;
    if (fv) {
        static const char *const kv[][2] = { { "nPhi", "3" }, { "nTheta", "5" }, { "maxIter", "10" }, { "tolerance", "1e-3" } };
        for (int i = 0; i < 4; i++) {
            char path[64]; snprintf(path, sizeof path, "fvDOMCoeffs/%s", kv[i][0]);
            if (!fd_find(rp, path)) fd_put(rp, path, kv[i][1]);
        }
    }
    if (!fd_find(rp, "absorptionEmissionModel")) {
        fd_put(rp, "absorptionEmissionModel", "constantAbsorptionEmission");
        fd_put(rp, "constantAbsorptionEmissionCoeffs/absorptivity", "absorptivity [0 -1 0 0 0 0 0] 0.5");
        fd_put(rp, "constantAbsorptionEmissionCoeffs/emissivity", "emissivity [0 -1 0 0 0 0 0] 0.5");
        fd_put(rp, "constantAbsorptionEmissionCoeffs/E", "E [1 -1 -3 0 0 0 0] 0");
    }
    if (!fd_find(rp, "scatterModel")) fd_put(rp, "scatterModel", "none");
    if (!fd_find(rp, "sootModel")) fd_put(rp, "sootModel", "none");

    FDFile *sch = case_dict(c, case_rpath(c, "system/fvSchemes"));
    FDFile *sol = case_dict(c, case_rpath(c, "system/fvSolution"));
    int added = 0;
    if (fv && sch && fd_find(sch, "divSchemes") && !dict_has_key_for(sch, "divSchemes", "div(Ji,Ii_h)")) {
        char ddt[64] = ""; fd_get(sch, "ddtSchemes/default", ddt, sizeof ddt);
        fd_put(sch, "divSchemes/div(Ji,Ii_h)", strstr(ddt, "steadyState") ? "bounded Gauss linearUpwind grad(Ii_h)" : "Gauss linearUpwind grad(Ii_h)");
        added++;
    }
    const char *fld = fv ? "Ii" : "G";
    if (sol && fd_find(sol, "solvers") && !dict_has_key_for(sol, "solvers", fld)) {
        char base[32]; snprintf(base, sizeof base, "solvers/%s", fld);
        char path[64];
        #define PUT(k, v) (snprintf(path, sizeof path, "%s/%s", base, k), fd_put(sol, path, v))
        if (fv) { PUT("solver", "GAMG"); PUT("smoother", "symGaussSeidel"); PUT("tolerance", "1e-4"); PUT("relTol", "0"); PUT("maxIter", "5"); }
        else    { PUT("solver", "PCG"); PUT("preconditioner", "DIC"); PUT("tolerance", "1e-5"); PUT("relTol", "0.1"); }
        #undef PUT
        added++;
    }
    if (added) ui_toast(TOAST_INFO, "%s also needs %s: added to system/. Save to keep them.", model, fv ? "div(Ji,Ii_h) and an Ii solver" : "a G solver");
}

/* ============================================================================ physics */
static float card_begin(float x, float y, float w, const char *icon, const char *title, const char *file)
{
    if (!TH.lcars) ui_icon(icon, x + 10, y + 11, 18, TH.text2);
    if (TH.lcars) {
        Color c = lc_for(title);
        const char *T = ui_upper(title);
        ui_rect((Rectangle){ x, y + 1, 26, 20 }, 10, c);
        DrawRectangleRec((Rectangle){ x + 16, y + 1, 10, 20 }, c);
        ui_text(F_MED, 18, T, x + 34, y + 1, c);
        float bx = x + 34 + ui_text_w(F_MED, 18, T) + 10, fx = file ? x + w - ui_text_w(F_MONO, 12, file) - 12 : x + w;
        if (fx - bx > 20) DrawRectangleRec((Rectangle){ bx, y + 8, fx - bx, 6 }, ui_alpha(c, 0.8f));
        if (file) ui_text(F_MONO, 12, file, x + w - ui_text_w(F_MONO, 12, file), y + 4, TH.text3);
        return 38;
    }
    ui_text(F_SEMI, 16, title, x + 30, y, TH.text);
    if (file) ui_text(F_MONO, 12, file, x + w - ui_text_w(F_MONO, 12, file), y + 4, TH.text3);
    return 34;
}

static float physics_models(float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    /* ---------------- turbulence */
    const char *tf = case_rpath(c, "constant/turbulenceProperties");
    FDFile *t = case_dict(c, tf);
    if (!t) { tf = case_rpath(c, "constant/momentumTransport"); t = case_dict(c, tf); }
    if (t) {
        y += card_begin(x, y, w, ICON_WIND, "Turbulence", tf);
        static const char *const sim[] = { "laminar", "RAS", "LES" };
        Rectangle cr = prop_row(x, &y, w, "Simulation type", "laminar resolves everything; RAS models all turbulence; LES resolves large eddies.", 36);
        if (dv_choice(t, "simulationType", (Rectangle){ cr.x, cr.y, 260, 36 }, sim, 3, false, NULL)) return y - y0;
        char st[32] = ""; fd_get(t, "simulationType", st, sizeof st);
        if (!strcmp(st, "RAS") || !strcmp(st, "LES")) {
            char key[64];
            snprintf(key, sizeof key, "%s/%s", st, fd_find(t, !strcmp(st, "RAS") ? "RAS/RASModel" : "LES/LESModel") ? (!strcmp(st, "RAS") ? "RASModel" : "LESModel") : "model");
            const char *const *items; int n = suggest_for_key(!strcmp(st, "RAS") ? "RASModel" : "LESModel", NULL, &items);
            cr = prop_row(x, &y, w, "Model", !strcmp(st, "RAS") ? "kOmegaSST is a good default for walls; kEpsilon for free shear flows. GEKO is new in v2606."
                                                               : "WALE behaves well near walls; Smagorinsky is the simplest.", 36);
            if (dv_choice(t, key, (Rectangle){ cr.x, cr.y, 260, 36 }, items, n, true, "model")) return y - y0;
            char m[64] = ""; fd_get(t, key, m, sizeof m);
            const char *need[6]; int nn = turb_model_fields(m, need);
            if (nn) y += required_fields(cr.x, y + 4, w, need, nn) + 8;
            char sw[64]; snprintf(sw, sizeof sw, "%s/turbulence", st);
            cr = prop_row(x, &y, w, "Turbulence on", "Off runs the model's equations without their effect on the flow.", 22);
            dv_switch(t, sw, cr.x, cr.y + 1, "on");
        }
        y += 32;
    }
    /* ---------------- transport */
    const char *pf = case_rpath(c, "constant/transportProperties");
    FDFile *tp = case_dict(c, pf);
    if (!tp) {
        pf = case_rpath(c, "constant/physicalProperties"); tp = case_dict(c, pf);
        if (tp && fd_find(tp, "thermoType")) tp = NULL;          /* that's a thermo file, handled below */
    }
    if (tp) {
        float yRows = y;
        FDEntry *phases = fd_find(tp, "phases");
        y += card_begin(x, y, w, ICON_DROPLETS, phases ? "Phases" : "Fluid properties", pf);
        if (fd_find(tp, "nu")) {
            Rectangle cr = prop_row(x, &y, w, "Kinematic viscosity", "nu in m^2/s. Water at 20 C is about 1e-6, air about 1.5e-5.", 36);
            dv_number(tp, "nu", (Rectangle){ cr.x, cr.y, 300, 36 }, "1e-05");
        }
        if (fd_find(tp, "transportModel")) {
            const char *const *items; int n = suggest_for_key("transportModel", NULL, &items);
            Rectangle cr = prop_row(x, &y, w, "Viscosity model", "Newtonian for most fluids; the others model shear thinning.", 36);
            if (dv_choice(tp, "transportModel", (Rectangle){ cr.x, cr.y, 260, 36 }, items, n, true, NULL)) return y - y0;
        }
        if (fd_find(tp, "sigma")) {
            Rectangle cr = prop_row(x, &y, w, "Surface tension", "sigma in N/m. Water and air: 0.07.", 36);
            dv_number(tp, "sigma", (Rectangle){ cr.x, cr.y, 300, 36 }, "0.07");
        }
        if (phases) {
            char pl[256]; fd_entry_value(tp, phases, pl, sizeof pl);
            char name[64];
            const char *p = strchr(pl, '(');
            while (p && *p) {
                while (*p && (*p == '(' || isspace((unsigned char)*p))) p++;
                int k = 0;
                while (*p && !isspace((unsigned char)*p) && *p != ')' && k < 63) name[k++] = *p++;
                name[k] = 0;
                if (!k) break;
                char path[128];
                snprintf(path, sizeof path, "%s/nu", name);
                if (fd_find(tp, path)) {
                    char lab[96]; snprintf(lab, sizeof lab, "%s: viscosity", name);
                    Rectangle cr = prop_row(x, &y, w, lab, "nu in m^2/s", 36);
                    dv_number(tp, path, (Rectangle){ cr.x, cr.y, 300, 36 }, "1e-06");
                }
                snprintf(path, sizeof path, "%s/rho", name);
                if (fd_find(tp, path)) {
                    char lab[96]; snprintf(lab, sizeof lab, "%s: density", name);
                    Rectangle cr = prop_row(x, &y, w, lab, "rho in kg/m^3", 36);
                    dv_number(tp, path, (Rectangle){ cr.x, cr.y, 300, 36 }, "1000");
                }
                if (*p == ')') break;
            }
        }
        if (y <= yRows + 34.5f) {                       /* nothing recognised: edit the raw entries */
            bool ed = false;
            y += 6 + dict_tree(tp, &tp->root, "", x, y + 6, w, 0, &ed);
            if (ed) return y - y0;
        }
        y += 32;
    }
    /* ---------------- thermophysical */
    const char *thf = case_rpath(c, "constant/thermophysicalProperties");
    FDFile *th = case_dict(c, thf);
    if (!th) { thf = case_rpath(c, "constant/physicalProperties"); th = case_dict(c, thf); if (th && !fd_find(th, "thermoType")) th = NULL; }
    if (th) {
        y += card_begin(x, y, w, ICON_THERMOMETER, "Thermophysical model", thf);
        static const struct { const char *k, *label, *hint; } tt[] = {
            { "type", "Thermo type", "heRhoThermo for most buoyant flows; hePsiThermo for compressible gases." },
            { "equationOfState", "Equation of state", "perfectGas for gases; rhoConst for liquids; Boussinesq for small density changes." },
            { "transport", "Transport", "const: fixed viscosity; sutherland: viscosity rising with temperature." },
            { "thermo", "Heat capacity", "hConst: constant cp; janaf: temperature-dependent polynomials." },
            { "energy", "Energy variable", "sensibleEnthalpy is the usual choice." } };
        for (size_t i = 0; i < sizeof tt / sizeof tt[0]; i++) {
            char path[96]; snprintf(path, sizeof path, "thermoType/%s", tt[i].k);
            if (!fd_find(th, path)) continue;
            const char *const *items; int n = suggest_for_key(tt[i].k, "thermoType", &items);
            Rectangle cr = prop_row(x, &y, w, tt[i].label, tt[i].hint, 36);
            if (dv_choice(th, path, (Rectangle){ cr.x, cr.y, 300, 36 }, items, n, true, NULL)) return y - y0;
        }
        FDEntry *mix = fd_find(th, "mixture");
        if (mix) {
            ui_text(F_MED, 13.5f, "Mixture properties", x, y + 14, TH.text);
            y += 40;
            bool ed = false;
            y += dict_tree(th, mix, "mixture", x, y, w, 0, &ed);
            if (ed) return y - y0;
        }
        y += 32;
    }
    /* ---------------- gravity */
    FDFile *g = case_dict(c, "constant/g");
    if (g) {
        y += card_begin(x, y, w, ICON_ORBIT, "Gravity", "constant/g");
        Rectangle cr = prop_row(x, &y, w, "Acceleration", "Vector in m/s^2. (0 -9.81 0) points down the y axis.", 36);
        dv_text(g, "value", (Rectangle){ cr.x, cr.y, 300, 36 }, 0, "(0 -9.81 0)");
        y += 32;
    }
    /* ---------------- radiation */
    FDFile *rp = case_dict(c, case_rpath(c, "constant/radiationProperties"));
    if (rp) {
        y += card_begin(x, y, w, ICON_SUN, "Thermal radiation", case_rpath(c, "constant/radiationProperties"));
        Rectangle cr;
        if (fd_find(rp, "radiation")) {
            cr = prop_row(x, &y, w, "Radiation", "Couple radiative heat transfer to the energy equation.", 22);
            if (dv_switch(rp, "radiation", cr.x, cr.y + 1, "off")) return y - y0;
        }
        const char *const *items; int n = suggest_for_key("radiationModel", NULL, &items);
        cr = prop_row(x, &y, w, "Model", "P1 is cheap and suits optically thick media. fvDOM traces rays and is accurate for thin media and shadows.", 36);
        if (dv_choice(rp, "radiationModel", (Rectangle){ cr.x, cr.y, 260, 36 }, items, n, true, NULL)) { radiation_model_defaults(c, rp); return y - y0; }
        char model[32] = ""; fd_get(rp, "radiationModel", model, sizeof model);
        cr = prop_row(x, &y, w, "Solve every", "solverFreq: radiation is solved once per this many flow iterations.", 36);
        dv_number(rp, "solverFreq", (Rectangle){ cr.x, cr.y, 160, 36 }, "1");
        if (!strcmp(model, "fvDOM")) {
            cr = prop_row(x, &y, w, "Azimuthal divisions", "nPhi per octant. Rays in 2-D = 4 x nPhi; 3-D = 4 x nPhi x nTheta.", 36);
            dv_number(rp, "fvDOMCoeffs/nPhi", (Rectangle){ cr.x, cr.y, 160, 36 }, "3");
            cr = prop_row(x, &y, w, "Polar divisions", "nTheta (3-D only).", 36);
            dv_number(rp, "fvDOMCoeffs/nTheta", (Rectangle){ cr.x, cr.y, 160, 36 }, "5");
            cr = prop_row(x, &y, w, "Ray sweeps", "maxIter: sweeps over all rays per solve.", 36);
            dv_number(rp, "fvDOMCoeffs/maxIter", (Rectangle){ cr.x, cr.y, 160, 36 }, "10");
            cr = prop_row(x, &y, w, "Tolerance", "Stop sweeping when the incident radiation changes less than this.", 36);
            dv_number(rp, "fvDOMCoeffs/tolerance", (Rectangle){ cr.x, cr.y, 160, 36 }, "1e-3");
        }
        if (fd_find(rp, "absorptionEmissionModel")) {
            n = suggest_for_key("absorptionEmissionModel", NULL, &items);
            cr = prop_row(x, &y, w, "Absorption", "How the medium absorbs and emits.", 36);
            if (dv_choice(rp, "absorptionEmissionModel", (Rectangle){ cr.x, cr.y, 300, 36 }, items, n, true, NULL)) return y - y0;
            if (fd_find(rp, "constantAbsorptionEmissionCoeffs/absorptivity")) {
                cr = prop_row(x, &y, w, "Absorption coefficient", "a in 1/m. Optical thickness of the domain is a x its size.", 36);
                dv_number(rp, "constantAbsorptionEmissionCoeffs/absorptivity", (Rectangle){ cr.x, cr.y, 300, 36 }, "0.5");
                cr = prop_row(x, &y, w, "Emission coefficient", "e in 1/m. Equal to a for a grey medium in equilibrium.", 36);
                dv_number(rp, "constantAbsorptionEmissionCoeffs/emissivity", (Rectangle){ cr.x, cr.y, 300, 36 }, "0.5");
            }
        }
        const char *needF = !strcmp(model, "P1") ? "G" : !strcmp(model, "fvDOM") ? "IDefault" : NULL;
        if (needF && !has_field(needF)) {
            char msg[200]; pl_fmt_ui(msg, sizeof msg, "%s needs a %s field file with wall conditions in %s/.", model, needF, c->fieldDir);
            float ch = callout_height(w, "Missing radiation field", msg) + 44;
            callout((Rectangle){ x, y + 8, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Missing radiation field", msg);
            char lab[64]; snprintf(lab, sizeof lab, "Create %s", needF);
            if (ui_button(ui_id("rad-create"), (Rectangle){ x + 42, y + 8 + ch - 42, ui_button_width(ICON_PLUS, lab), 30 }, ICON_PLUS, lab, BTN_SECONDARY))
                create_radiation_field(needF);
            y += ch + 16;
        }
        y += 32;
    }
    if (y == y0) {
        const char *msg = "No turbulence, transport, thermophysical or radiation dictionaries were found in constant/. Use the All files tab.";
        float ch = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, msg);
        y += ch + 20;
    }
    return y - y0;
}

static float files_in(const char *dir, float x, float y, float w, const char *const *skip)
{
    float y0 = y;
    char p[PL_PATH]; pl_join(p, sizeof p, A.cs->s.path, dir);
    const NameList l = *pl_list_dir_cached(p, false, true);   /* every frame: cached, sorted */
    for (int i = 0; i < l.n; i++) {
        bool sk = false;
        for (int k = 0; skip && skip[k]; k++) if (!strcmp(l.names[i], skip[k])) sk = true;
        if (sk || pl_ends_with(l.names[i], "~") || pl_ends_with(l.names[i], ".qc-tmp")) continue;
        char rel[300]; snprintf(rel, sizeof rel, "%s/%s", dir, l.names[i]);
        FDFile *f = case_dict(A.cs, rel);
        if (!f || f->root.nChild == 0) continue;
        UiId id = ui_id(rel);
        bool open = tree_expanded(id, false);
        if (ui_row(id, (Rectangle){ x - 8, y, w + 16, 40 }, false)) tree_toggle(id, false);
        ui_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + 6, y + 20, 14, TH.text3);
        ui_text(F_MONO_MED, 13.5f, rel, x + 24, y + 11, TH.text);
        if (f->dirty) DrawCircleV((Vector2){ x + 32 + ui_text_w(F_MONO_MED, 13.5f, rel), y + 20 }, 3.5f, TH.accent);
        y += 42;
        if (open) {
            if (!f->ok) { ui_text(F_REG, 12.5f, f->err, x + 24, y, TH.warn); y += 24; }
            bool ed = false;
            y += dict_tree(f, &f->root, "", x + 16, y, w - 16, 0, &ed);
            if (ed) return y - y0;
            y += 12;
        }
        ui_divider(x, y, w);
        y += 4;
    }
    return y - y0;
}

void page_physics(Rectangle r)
{
    UiScroll *sc = &A.scr[7];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX);
    float y = top + 30;
    char sub[200], dir[128];
    snprintf(dir, sizeof dir, A.cs->region[0] ? "constant/%s" : "constant", A.cs->region);
    pl_fmt_ui(sub, sizeof sub, "Models and material properties from %s/. Changes are written when you save.", dir);
    y += page_header(x, y, w, "Physics", sub);
    y += region_bar(x, y, w);
    static const char *tabs[] = { "Models", "All files" };
    float th;
    ui_tabs(ui_id("phys-tabs"), x, y, tabs, 2, &A.physicsTab, &th);
    ui_divider(x, y + th, w);
    y += th + 26;
    if (A.physicsTab == 0) y += physics_models(x, y, w);
    else {
        static const char *const skip[] = { NULL };
        y += files_in(dir, x, y, w, skip);
    }
    ui_scroll_end(sc, y - top + 40);
}

/* ============================================================================ numerics */
typedef struct { const char *k, *t, *d; } SecInfo;
static const SecInfo SECTIONS[] = {
    { "ddtSchemes", "Time derivative", "Euler is first order and robust; backward is second order; steadyState for SIMPLE solvers." },
    { "gradSchemes", "Gradients", "Gauss linear is standard. Limited variants keep gradients bounded on poor meshes." },
    { "divSchemes", "Convection", "Upwind is stable but diffusive; linearUpwind and limitedLinear are second order." },
    { "laplacianSchemes", "Diffusion", "Add 'limited corrected 0.33' for strongly non-orthogonal meshes." },
    { "interpolationSchemes", "Interpolation", "Cell-to-face interpolation. linear almost always." },
    { "snGradSchemes", "Surface-normal gradients", "Should match the Laplacian correction." },
    { "wallDist", "Wall distance", "Used by turbulence models near walls." },
    { "solvers", "Linear solvers", "One entry per equation; keys can be patterns such as \"(U|k|epsilon)\". GAMG suits pressure, smoothSolver the rest." },
    { "SIMPLE", "SIMPLE algorithm", "Steady pressure-velocity coupling." },
    { "PISO", "PISO algorithm", "Transient pressure-velocity coupling for small time steps." },
    { "PIMPLE", "PIMPLE algorithm", "Transient coupling that tolerates larger time steps through outer correctors." },
    { "relaxationFactors", "Under-relaxation", "Smaller is more stable but slower. Typical: p 0.3, U 0.7." },
    { "potentialFlow", "Potential flow", "Settings for potentialFoam initialisation." },
    { NULL, NULL, NULL } };

static float dict_sections(FDFile *f, float x, float y, float w)
{
    float y0 = y;
    bool ed = false;
    for (int i = 0; i < f->root.nChild && !ed; i++) {
        FDEntry *e = &f->root.child[i];
        if (!strcmp(e->key, "FoamFile")) continue;
        if (e->kind != FD_DICT) {
            /* top-level values: one tree row each */
            FDEntry tmp = f->root; tmp.child = e; tmp.nChild = 1;
            y += dict_tree(f, &tmp, "", x, y, w, 0, &ed);
            continue;
        }
        const char *title = e->key, *desc = NULL;
        for (int k = 0; SECTIONS[k].k; k++) if (!strcmp(SECTIONS[k].k, e->key)) { title = SECTIONS[k].t; desc = SECTIONS[k].d; }
        char tt[160];
        if (title != e->key) snprintf(tt, sizeof tt, "%s", title); else snprintf(tt, sizeof tt, "%s", e->key);
        y += 10;
        float hh = section_head_r(x, y, w, tt, desc, title != e->key ? e->key : NULL);
        y += hh;
        char key[128]; snprintf(key, sizeof key, "%s", e->key);
        y += dict_tree(f, e, key, x, y, w, 0, &ed);
        if (ed) break;
        Rectangle add = { x + 22, y + 2, ui_text_w(F_MED, 12.5f, "Add entry") + 30, 26 };
        bool ah = ui_hovered(add);
        if (ah) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_icon(ICON_PLUS, add.x + 8, add.y + 13, 13, ah ? TH.accent : TH.text3);
        ui_text(F_MED, 12.5f, "Add entry", add.x + 20, add.y + 5, ah ? TH.accent : TH.text3);
        if (ui_clicked(add)) {
            A.modal = MD_ADD_ENTRY; A.mFlag = false;
            snprintf(A.mPath, sizeof A.mPath, "%s", key);
            snprintf(A.mFile, sizeof A.mFile, "%s", f->path);
            A.mText[0] = A.mText2[0] = A.mErr[0] = 0;
        }
        y += 44;
    }
    return y - y0;
}

static void create_decompose(void)
{
    char body[512];
    snprintf(body, sizeof body, "numberOfSubdomains %d;\n\nmethod          scotch;\n", A.set.nProcs > 1 ? A.set.nProcs : 4);
    if (case_create_dict(A.cs, "system/decomposeParDict", "dictionary", body)) ui_toast(TOAST_OK, "Created system/decomposeParDict");
}

void page_numerics(Rectangle r)
{
    UiScroll *sc = &A.scr[8];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX);
    float y = top + 30;
    char sub[220];
    snprintf(sub, sizeof sub, "Discretisation schemes and linear solvers from %s and %s.", case_rpath(A.cs, "system/fvSchemes"), case_rpath(A.cs, "system/fvSolution"));
    y += page_header(x, y, w, "Numerics", sub);
    y += region_bar(x, y, w);
    static const char *tabs[] = { "Schemes", "Solvers", "Parallel" };
    float th;
    ui_tabs(ui_id("num-tabs"), x, y, tabs, 3, &A.numericsTab, &th);
    ui_divider(x, y + th, w);
    y += th + 20;
    const char *rel = case_rpath(A.cs, A.numericsTab == 0 ? "system/fvSchemes" : A.numericsTab == 1 ? "system/fvSolution" : "system/decomposeParDict");
    FDFile *f = case_dict(A.cs, rel);
    if (!f) {
        char msg[200]; snprintf(msg, sizeof msg, "%s does not exist in this case.", rel);
        float ch = callout_height(w, NULL, msg) + (A.numericsTab == 2 ? 44 : 0);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, msg);
        if (A.numericsTab == 2 && ui_button(ui_id("mk-decomp"), (Rectangle){ x + 42, y + ch - 42, 230, 30 }, ICON_PLUS, "Create decomposeParDict", BTN_SECONDARY))
            create_decompose();
        y += ch + 20;
    } else {
        if (!f->ok) {
            float ch = callout_height(w, "Could not fully read this file", f->err);
            callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Could not fully read this file", f->err);
            y += ch + 16;
        }
        if (A.numericsTab == 2) {
            Rectangle cr = prop_row(x, &y, w, "Processes", "numberOfSubdomains: one MPI rank per subdomain. Use at most your physical core count.", 36);
            bool resplit = dv_number(f, "numberOfSubdomains", (Rectangle){ cr.x, cr.y, 160, 36 }, "4");
            char cpu[64]; snprintf(cpu, sizeof cpu, "%d cores here", A.env.nCpu);
            ui_text(F_REG, 12.5f, cpu, cr.x + 176, cr.y + 10, TH.text3);
            const char *const *items; int n = suggest_for_key("method", NULL, &items);
            cr = prop_row(x, &y, w, "Method", "scotch balances the load automatically; simple and hierarchical split along axes.", 36);
            if (dv_choice(f, "method", (Rectangle){ cr.x, cr.y, 240, 36 }, items, n, true, NULL)) resplit = true;
            /* simple and hierarchical need a split (nx ny nz) whose product is the process count */
            char m[64] = "", mw[64] = "", ns[32] = "";
            fd_get(f, "method", m, sizeof m); fd_word(m, mw, sizeof mw);
            fd_get(f, "numberOfSubdomains", ns, sizeof ns);
            int np = atoi(ns);
            if ((!strcmp(mw, "simple") || !strcmp(mw, "hierarchical")) && np > 0) {
                char key[128], v[128] = "";
                snprintf(key, sizeof key, "%sCoeffs/n", mw);
                if (!fd_find(f, key) && fd_find(f, "coeffs/n")) snprintf(key, sizeof key, "coeffs/n");
                fd_get(f, key, v, sizeof v);
                long a = 0, b = 0, cc = 0;
                const char *q = strchr(v, '(');
                bool ok = q && sscanf(q + 1, "%ld %ld %ld", &a, &b, &cc) == 3 && a * b * cc == np;
                if (resplit && !ok) {
                    char nv[64]; decomp_split(A.cs, np, nv, sizeof nv);
                    fd_put(f, key, nv);
                    ok = true;
                }
                cr = prop_row(x, &y, w, "Split", "Subdomains along x, y and z. Their product must equal the process count.", 36);
                dv_text(f, key, (Rectangle){ cr.x, cr.y, 240, 36 }, 0, "(2 2 1)");
                if (!ok) ui_text(F_REG, 12.5f, "The product doesn't match the process count.", cr.x + 256, cr.y + 10, TH.err);
            }
            y += 20;
        } else y += dict_sections(f, x, y, w);
    }
    ui_scroll_end(sc, y - top + 40);
}

/* ============================================================================ files */
/* text files worth editing in the case root: scripts and notes, not logs, results or ParaView stubs */
static bool root_file_listed(const char *dir, const char *n)
{
    if (n[0] == '.' || pl_starts_with(n, "log.") || pl_ends_with(n, ".foam") || pl_ends_with(n, ".OpenFOAM") ||
        pl_ends_with(n, "~") || pl_ends_with(n, ".qc-tmp") || pl_ends_with(n, ".pyc")) return false;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, n);
    if (pl_file_size(p) > (2 << 20)) return false;
    FILE *fp = fopen(p, "rb");
    if (!fp) return false;
    unsigned char h[512];
    size_t got = fread(h, 1, sizeof h, fp);
    fclose(fp);
    for (size_t i = 0; i < got; i++) if (h[i] < 9) return false;   /* NUL or control bytes: binary (images, archives) */
    return true;
}

static float file_group(Rectangle r, float y, const char *label, int colorIdx)
{
    if (TH.lcars) {
        Color fc = TH.lc[colorIdx % 8];
        ui_rect((Rectangle){ r.x + 14, y + 1, 18, 18 }, 9, fc);
        DrawRectangleRec((Rectangle){ r.x + 23, y + 1, 9, 18 }, fc);
        ui_text_clip(F_MED, 15, label, r.x + 38, y + 2, r.width - 52, fc);
    } else {
        ui_icon(ICON_FOLDER, r.x + 22, y + 10, 15, TH.text3);
        ui_text_clip(F_SEMI, 13, label, r.x + 38, y + 2, r.width - 52, TH.text2);
    }
    return 28;
}

/* one folder's files (relDir "" = case root); returns the height used */
static float file_rows(Rectangle r, float y, const char *relDir, int indent)
{
    Case *c = A.cs;
    float y0 = y;
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, relDir[0] ? relDir : ".");
    const NameList l = *pl_list_dir_cached(p, false, true);   /* every frame: cached, sorted */
    for (int i = 0; i < l.n; i++) {
        const char *n = l.names[i];
        if (!relDir[0] ? !root_file_listed(p, n) : (pl_ends_with(n, "~") || pl_ends_with(n, ".qc-tmp") || pl_ends_with(n, ".gz"))) continue;
        char rel[300]; snprintf(rel, sizeof rel, relDir[0] ? "%s/%s" : "%s%s", relDir, n);
        Rectangle rr = { r.x + 10 + indent, y, r.width - 24 - indent, 32 };
        if (ui_row(ui_id(rel), rr, !strcmp(A.filesSel, rel))) snprintf(A.filesSel, sizeof A.filesSel, "%s", rel);
        ui_text_clip(F_MONO, 13, n, rr.x + 30, rr.y + 8, rr.width - 50, TH.text);
        FDFile *f = NULL;
        for (int k = 0; k < c->nFiles; k++) if (!strcmp(c->files[k].rel, rel)) f = c->files[k].f;
        if (f && f->dirty) DrawCircleV((Vector2){ rr.x + rr.width - 14, rr.y + 16 }, 3.5f, TH.accent);
        y += 34;
    }
    return y - y0;
}

static void file_list(Rectangle r)
{
    Case *c = A.cs;
    UiScroll *sc = &A.scr[9];
    float y = ui_scroll_begin(sc, r) + 10;
    float bw = ui_button_width(ICON_PLUS, "New file");
    if (ui_button(ui_id("f-new"), (Rectangle){ r.x + 14, y, bw, 30 }, ICON_PLUS, "New file", BTN_GHOST)) {
        A.modal = MD_NEW_FILE; A.mErr[0] = 0;
        snprintf(A.mText, sizeof A.mText, "%s", c->s.prog ? "" : c->region[0] ? case_rpath(c, "system/") : "system/");
    }
    y += 42;
    if (c->s.prog) {                                          /* another program: its text files, folder by folder */
        static NameList files; static double at = -10; static char forPath[PL_PATH];
        if (GetTime() - at > 2 || strcmp(forPath, c->s.path)) { prog_files(c->s.prog, c->s.path, &files); at = GetTime(); pl_strlcpy(forPath, c->s.path, sizeof forPath); }
        char cur[PL_PATH] = "-";
        for (int i = 0; i < files.n; i++) {
            const char *rel = files.names[i];
            char dir[PL_PATH]; pl_strlcpy(dir, rel, sizeof dir);
            char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else dir[0] = 0;
            if (strcmp(dir, cur)) {
                if (cur[0] != '-') y += 8;
                pl_strlcpy(cur, dir, sizeof cur);
                y += file_group(r, y, dir[0] ? dir : c->s.name, 1 + (int)(strlen(dir) % 6));
            }
            Rectangle rr = { r.x + 10, y, r.width - 24, 32 };
            if (ui_row(ui_id(rel), rr, !strcmp(A.filesSel, rel))) snprintf(A.filesSel, sizeof A.filesSel, "%s", rel);
            ui_text_clip(F_MONO, 13, sl ? rel + (sl - dir) + 1 : rel, rr.x + 30, rr.y + 8, rr.width - 50, TH.text);
            FDFile *f = NULL;
            for (int k = 0; k < c->nFiles; k++) if (!strcmp(c->files[k].rel, rel)) f = c->files[k].f;
            if (f && f->dirty) DrawCircleV((Vector2){ rr.x + rr.width - 14, rr.y + 16 }, 3.5f, TH.accent);
            y += 34;
        }
        if (!files.n) { ui_text(F_REG, 13, "No text files found.", r.x + 20, y, TH.text3); y += 30; }
        ui_scroll_end(sc, y - (r.y - roundf(sc->y)) + 10);
        return;
    }
    /* the case folder's own files (scripts, notes), when it has any */
    {
        const NameList *root = pl_list_dir_cached(c->s.path, false, true);
        int shown = 0;
        for (int i = 0; i < root->n; i++) if (root_file_listed(c->s.path, root->names[i])) shown++;
        if (shown) { y += file_group(r, y, "case", 0); y += file_rows(r, y, "", 0) + 12; }
    }
    /* system and constant, with their sub-folders (regions); the fields folder of the selected region */
    const char *tops[2] = { "system", "constant" };
    for (int d = 0; d < 2; d++) {
        y += file_group(r, y, tops[d], d * 3 + 1);
        y += file_rows(r, y, tops[d], 0);
        NameList sub = { 0 };
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, tops[d]);
        pl_list_dir(p, &sub, true, false);
        names_sort(&sub);
        for (int i = 0; i < sub.n; i++) {
            const char *n = sub.names[i];
            if (!strcmp(n, "polyMesh") || !strcmp(n, "triSurface") || !strcmp(n, "extendedFeatureEdgeMesh") || !strcmp(n, "geometry") ||
                !strcmp(n, "sets") || n[0] == '.') continue;
            char rel[200]; snprintf(rel, sizeof rel, "%s/%s", tops[d], n);
            ui_text(F_MONO_MED, 12.5f, rel, r.x + 40, y + 6, TH.text2);
            y += 26;
            y += file_rows(r, y, rel, 14);
        }
        names_free(&sub);
        y += 12;
    }
    y += file_group(r, y, c->fieldDir, 7);
    y += file_rows(r, y, c->fieldDir, 0);
    ui_scroll_end(sc, y - (r.y - roundf(sc->y)) + 10);
}

void page_files(Rectangle r)
{
    float lw = 300;
    Rectangle list = { r.x, r.y, lw, r.height };
    ui_rect(list, 0, TH.sidebar);
    DrawRectangleRec((Rectangle){ r.x + lw - 1, r.y, 1, r.height }, TH.lineSoft);
    file_list(list);
    Rectangle main = { r.x + lw, r.y, r.width - lw, r.height };
    float x = main.x + PAGE_PAD, w = fminf(main.width - 2 * PAGE_PAD, CONTENT_MAX);
    if (!A.filesSel[0]) {
        ui_icon(ICON_FILE_TEXT, main.x + main.width / 2, main.y + main.height / 2 - 40, 32, TH.text3);
        ui_text_center(F_REG, 14, "Pick a file to edit its entries.", (Rectangle){ main.x, main.y + main.height / 2 - 10, main.width, 24 }, TH.text2);
        return;
    }
    FDFile *f = case_dict(A.cs, A.filesSel);
    bool isDict = f && strstr(f->text, "FoamFile");                 /* scripts and notes: text only */
    float y = main.y + 26;

    /* a name that doesn't fit beside the buttons gets its own line above them */
    float btns = ui_button_width(ICON_EXTERNAL_LINK, "Open in editor") + ui_button_width(ICON_REFRESH_CW, "Reload") + 190 +
                 ui_button_width(ICON_SAVE, "Save") + 30;
    bool ownLine = ui_text_w(F_SEMI, 20, A.filesSel) > w - btns;
    if (ownLine) { ui_text_clip(F_SEMI, 20, A.filesSel, x, y, w, TH.text); y += 42; }

    float rx = x + w;
    float b1 = ui_button_width(ICON_EXTERNAL_LINK, "Open in editor");
    rx -= b1;
    char full[PL_PATH]; pl_join(full, sizeof full, A.cs->s.path, A.filesSel);
    if (ui_button(ui_id("f-ext"), (Rectangle){ rx, y - 4, b1, 34 }, ICON_EXTERNAL_LINK, "Open in editor", BTN_SECONDARY)) launch_editor(&A.env, &A.set, full);
    float b2 = ui_button_width(ICON_REFRESH_CW, "Reload");
    rx -= b2 + 8;
    if (f && ui_button(ui_id("f-reload"), (Rectangle){ rx, y - 4, b2, 34 }, ICON_REFRESH_CW, "Reload", BTN_GHOST)) { fd_reload(f); ui_toast(TOAST_INFO, "Reloaded from disk"); }
    static const char *modes[] = { "Entries", "Text" };
    int m = A.filesRaw || !isDict ? 1 : 0;
    rx -= 190;
    if (isDict && ui_segmented(ui_id("f-mode"), (Rectangle){ rx, y - 4, 180, 34 }, modes, 2, &m)) A.filesRaw = m == 1;
    float b3 = ui_button_width(ICON_SAVE, "Save");
    rx -= b3 + 8;
    ui_disable(!f || !f->dirty);
    if (ui_button(ui_id("f-save"), (Rectangle){ rx, y - 4, b3, 34 }, ICON_SAVE, "Save", BTN_PRIMARY)) {
        if (fd_save(f)) ui_toast(TOAST_OK, "Saved %s", A.filesSel); else ui_toast(TOAST_ERR, "Could not write %s", A.filesSel);
    }
    ui_disable(false);
    if (!ownLine) ui_text_clip(F_SEMI, 20, A.filesSel, x, y, rx - x - 14, TH.text);
    y += 50;
    if (!f) {
        ui_text(F_REG, 14, "This file is not a dictionary Q-Continuum can read (binary, compressed or too large).", x, y, TH.text2);
        return;
    }
    if (f->dirty) { ui_badge(x, y - 8, ICON_PENCIL, "Edited, not saved", TH.accent, TH.accentSoft); y += 26; }
    if (!f->ok && isDict && !A.filesRaw) {
        float ch = callout_height(w, "Parts of this file could not be read", f->err);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Parts of this file could not be read", f->err);
        y += ch + 14;
    }
    Rectangle body = { main.x, y, main.width, main.y + main.height - y };
    if (A.filesRaw || !isDict) {
        float tw2 = fminf(main.width - 2 * PAGE_PAD, 1400);            /* code wants width */
        text_editor(f, (Rectangle){ x, y, tw2, body.height - 30 });
        return;
    }
    UiScroll *sc = &A.scr[10];
    float top = ui_scroll_begin(sc, body);
    bool ed = false;
    float h = dict_tree(f, &f->root, "", x, top + 4, w, 0, &ed);
    if (!ed) {
        Rectangle add = { x + 22, top + 4 + h + 4, ui_text_w(F_MED, 12.5f, "Add entry") + 30, 26 };
        if (ui_button(ui_id("f-add"), add, ICON_PLUS, "Add entry", BTN_GHOST)) {
            A.modal = MD_ADD_ENTRY; A.mFlag = false; A.mPath[0] = 0;
            snprintf(A.mFile, sizeof A.mFile, "%s", f->path);
            A.mText[0] = A.mText2[0] = A.mErr[0] = 0;
        }
    }
    ui_scroll_end(sc, h + 60);
}
