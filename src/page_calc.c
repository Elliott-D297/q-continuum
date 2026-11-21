/* page_calc.c -- the Calculators dialog (mesh overhaul plan step 2, gaps L2 L7 L9 L10): wall spacing from y+, cell
 * grading, cells / memory / run time, the dimensionless numbers. The numbers come from calc.c; this file is the inputs,
 * the results and the working. Opened from the palette (help.calc), the STL mesher's prism layers, Physics > Setup's
 * walls and Mesh > Generate; it fills itself in from the open case each time it opens. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "calc.h"
#include "precheck.h"
#include "simsetup.h"
#include "teach.h"

enum { FLU_CASE = CF__N, FLU_CUSTOM, FLU__N };

static struct {
    int tab;
    int fluid;                       /* CF_*, FLU_CASE, FLU_CUSTOM                       */
    bool haveCase;                   /* the open case gave a fluid                     */
    CalcFluid cf;                    /* the open case's fluid (FLU_CASE)               */
    char caseName[96];
    double rho, mu, cp, k, beta, sigma, c;
    int flow;                        /* CW_*                                           */
    double U, L, yplus, rough, r, cell;
    int gmode; double gL, gN, gR, gd1, gdn, gr;
    double cells, steps, nOuter, procs; int solver;
    double dT, g, dt, dx;
    int dimSel;
    char from[300];                  /* where the numbers came from                    */
    bool stl;                        /* the open case is an STL mesher case: the layers can be sent */
    UiScroll sc;
} C = { .fluid = CF_AIR, .rho = 1.204, .mu = 1.825e-5, .cp = 1007, .k = 0.02514, .beta = 3.411e-3, .sigma = 0, .c = 343.2,
        .U = 10, .L = 1, .yplus = 1, .r = 1.2, .gL = 0.1, .gN = 20, .gR = 10, .gd1 = 0.001, .gdn = 0.01, .gr = 1.2,
        .cells = 1e6, .steps = 1000, .nOuter = 1, .procs = 1, .g = 9.81 };

static void scat(char *d, const char *s, size_t cap) { size_t n = strlen(d); if (n + 1 < cap) snprintf(d + n, cap - n, "%s", s); }

static void use_fluid(const CalcFluid *f)
{
    C.rho = f->rho; C.mu = f->mu; C.cp = f->cp; C.k = f->k; C.beta = f->beta; C.sigma = f->sigma; C.c = f->c;
}

static void pick_fluid(int i)
{
    C.fluid = i;
    if (i < CF__N) use_fluid(&CALC_FLUID[i]);
    else if (i == FLU_CASE && C.haveCase) use_fluid(&C.cf);
}

/* the open case's flow: fluid, speed, size, y+, cells, time step */
static void fill_from_case(void)
{
    Case *c = A.cs;
    C.from[0] = 0; C.haveCase = false; C.stl = false;
    if (!c) return;
    /* what only some cases give starts empty, so one case's time step or surface cells don't carry over to the next */
    C.dt = 0; C.dT = 0; C.cell = 0; C.dx = 0;
    pl_strlcpy(C.caseName, c->s.name, sizeof C.caseName);
    double size = 0; bool ext = true;
    SimPatch pa[1];
    stlmesh_patches(c, pa, 0, &size, &ext);
    SimSetup s;
    bool have = sim_load(c->s.path, &s, size);
    char parts[220] = "";
    if (have) {
        CalcFluid f = { "case", "This case", "", s.T0 > 0 ? s.T0 : 293.15, s.rho, s.nu * s.rho, s.cp, s.kappa, s.beta, 0, 0 };
        bool gas = sim_is_gas(&s);
        if (s.phase == PH_VOF) { f.rho = s.rhoL; f.mu = s.nuL * s.rhoL; f.sigma = s.sigma; }
        f.c = gas && s.molW > 0 ? sqrt(s.gamma * 8314.46 / s.molW * f.T) : s.fluid == FLUID_AIR ? 343.2 : 1482;
        if (!(f.cp > 0)) f.cp = s.fluid == FLUID_AIR || gas ? 1007 : 4182;
        if (!(f.k > 0)) f.k = s.fluid == FLUID_AIR || gas ? 0.02514 : 0.598;
        if (!(f.beta > 0)) f.beta = s.fluid == FLUID_AIR || gas ? 1 / f.T : 2.07e-4;
        if (!(f.sigma > 0) && s.fluid == FLUID_WATER) f.sigma = 0.0728;
        C.cf = f; C.haveCase = f.rho > 0 && f.mu > 0;
        if (s.fluid == FLUID_AIR && !gas && s.phase != PH_VOF) pick_fluid(CF_AIR);
        else if (s.fluid == FLUID_WATER && !gas && s.phase != PH_VOF) pick_fluid(CF_WATER);
        else if (C.haveCase) pick_fluid(FLU_CASE);
        double U = sim_ref_speed(&s);
        if (U > 0) C.U = U;
        C.yplus = sim_yplus_target(&s);
        C.solver = s.phase == PH_VOF ? CS_FREESURFACE : gas ? CS_COMPRESSIBLE : s.energy != EN_OFF ? CS_HEAT :
                   s.flow == FLOW_TRANSIENT ? CS_TRANSIENT : CS_STEADY;
        if (s.flow == FLOW_TRANSIENT) {
            if (s.deltaT > 0) C.dt = s.deltaT;
            if (s.deltaT > 0 && s.endTime > 0) C.steps = ceil(s.endTime / s.deltaT);
            if (s.nOuter > 0) C.nOuter = s.nOuter;
        } else if (s.iterations > 0) C.steps = s.iterations;
        if (s.energy != EN_OFF && fabs(s.T0 - s.Tref) > 0.01) C.dT = fabs(s.T0 - s.Tref);
        snprintf(parts, sizeof parts, "the fluid, %.3g m/s inflow, y+ %g", U > 0 ? U : C.U, C.yplus);
    }
    /* the length: an internal flow's inlet diameter, an external part's size */
    double D = 0;
    if (have && !ext)
        for (int i = 0; i < s.nB && !(D > 0); i++)
            if (s.b[i].role == ROLE_INLET) D = s.b[i].dh > 0 ? s.b[i].dh : s.b[i].area > 0 ? sqrt(4 * s.b[i].area / M_PI) : 0;
    if (size > 0) {
        C.flow = !ext && D > 0 ? CW_PIPE : CW_PLATE;
        C.L = C.flow == CW_PIPE ? D : size;
        char t[80]; snprintf(t, sizeof t, "%s%s %.3g m", parts[0] ? ", " : "", C.flow == CW_PIPE ? "inlet diameter" : "part length", C.L);
        scat(parts, t, sizeof parts);
    }
    double first = 0, cell = 0; int n = 0;
    stlmesh_wall_layers(c, &first, &n, &cell);
    if (cell > 0) { C.cell = cell; C.dx = cell; C.stl = true; scat(parts, ", the mesher's surface cells", sizeof parts); }
    double cells = c->s.nCells > 0 ? (double)c->s.nCells : stlmesh_cells_estimate(c);
    if (cells > 0) { C.cells = cells; scat(parts, c->s.nCells > 0 ? ", the mesh's cells" : ", the mesher's cell estimate", sizeof parts); }
    C.procs = run_procs_for_check();
    if (parts[0]) pl_fmt_ui(C.from, sizeof C.from, "Filled in from %s: %s. Change anything; nothing here changes the case until you send it.", c->s.name, parts);
}

void calc_open(int tab)
{
    if (A.modal != MD_CALC) {
        fill_from_case();
        C.sc = (UiScroll){ 0 };
        static bool once;
        if (!once) {                                 /* the working is the point here: open it the first time */
            once = true;
            teach_working_set("calc-wk-wall", true); teach_working_set("calc-wk-grade", true);
            teach_working_set("calc-wk-cost", true); teach_working_set("calc-wk-dim", true);
        }
    }
    if (tab >= 0 && tab < CALC__N) C.tab = tab;
    A.modal = MD_CALC;
}

/* ---------------------------------------------------------------- inputs */
static float LW = 170;    /* label width */

static bool num_in(const char *id, float x, float *y, float w, const char *label, double *v, double lo, double hi, const char *unit, const char *tip)
{
    ui_text(F_REG, 13.5f, label, x, *y + 9, TH.text2);
    if (tip) ui_tooltip((Rectangle){ x, *y, LW - 8, 34 }, tip);
    float fw = fminf(170, w - LW);
    Rectangle r = { x + LW, *y, fw, 34 };
    char b[64];
    if (*v != 0 && (fabs(*v) < 1e-3 || fabs(*v) >= 1e6)) snprintf(b, sizeof b, "%.4g", *v);
    else snprintf(b, sizeof b, "%.6g", *v);
    float uw = unit && *unit ? ui_text_w(F_MONO, 12, unit) + 12 : 0;
    if (uw) ui_text(F_MONO, 12, unit, r.x + r.width - uw + 6, r.y + 10, TH.text3);
    bool ch = false;
    if (ui_input(ui_id(id), (Rectangle){ r.x, r.y, r.width - uw, r.height }, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) {
        char *e; double d = strtod(b, &e);
        while (e && *e == ' ') e++;
        if (e == b || (e && *e) || d < lo || d > hi) ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi);
        else { *v = d; ch = true; }
    }
    *y += 42;
    return ch;
}

static float fluid_inputs(float x, float y, float w, bool all)
{
    float y0 = y;
    const char *items[FLU__N];
    char caseLab[128]; pl_fmt_ui(caseLab, sizeof caseLab, "This case (%s)", C.caseName);
    for (int i = 0; i < CF__N; i++) items[i] = CALC_FLUID[i].name;
    items[FLU_CASE] = caseLab; items[FLU_CUSTOM] = "Custom";
    int n = FLU__N;                                             /* "This case" without a case's fluid: a toast says why */
    ui_text(F_REG, 13.5f, "Fluid", x, y + 9, TH.text2);
    int sel = C.fluid;
    if (!C.haveCase && sel == FLU_CASE) sel = FLU_CUSTOM;
    if (ui_select(ui_id("calc-fluid"), (Rectangle){ x + LW, y, fminf(260, w - LW), 34 }, items, n, &sel) && sel != C.fluid) {
        if (sel == FLU_CASE && !C.haveCase) ui_toast(TOAST_INFO, "The open case has no fluid set yet (Physics > Setup)");
        else pick_fluid(sel);
    }
    y += 40;
    if (C.fluid < CF__N) {
        char st[160]; pl_fmt_ui(st, sizeof st, "%s", CALC_FLUID[C.fluid].state);
        ui_text(F_REG, 12, st, x + LW, y - 2, TH.text3);
        y += 18;
    }
    bool ed = false;
    ed |= num_in("calc-rho", x, &y, w, "Density ρ", &C.rho, 1e-6, 1e5, "kg/m³", NULL);
    ed |= num_in("calc-mu", x, &y, w, "Viscosity μ", &C.mu, 1e-9, 1e3, "Pa s", "Dynamic viscosity; the kinematic one is ν = μ / ρ");
    if (all) {
        ed |= num_in("calc-cp", x, &y, w, "Specific heat cp", &C.cp, 1, 1e6, "J/(kg K)", NULL);
        ed |= num_in("calc-k", x, &y, w, "Conductivity k", &C.k, 1e-6, 1e4, "W/(m K)", NULL);
        ed |= num_in("calc-beta", x, &y, w, "Expansion β", &C.beta, 0, 1, "1/K", "Volumetric thermal expansion: an ideal gas has 1/T");
        ed |= num_in("calc-sigma", x, &y, w, "Surface tension σ", &C.sigma, 0, 10, "N/m", "Against air; 0 for a gas");
        ed |= num_in("calc-c", x, &y, w, "Speed of sound c", &C.c, 0, 1e5, "m/s", NULL);
    }
    if (ed) C.fluid = FLU_CUSTOM;
    return y - y0;
}

static float flow_choice(float x, float y, float w)
{
    static const char *items[2] = { "Along a plate", "In a pipe" };
    ui_text(F_REG, 13.5f, "Flow", x, y + 9, TH.text2);
    ui_tooltip((Rectangle){ x, y, LW - 8, 34 }, "External flow past a body: a flat plate as long as it. Internal flow: a pipe of that diameter.");
    ui_segmented(ui_id("calc-flow"), (Rectangle){ x + LW, y, fminf(260, w - LW), 34 }, items, 2, &C.flow);
    return 42;
}

/* one result line: a big value and what it is */
static float headline(float x, float y, float w, const char *value, const char *what, Color col)
{
    float vw = ui_text_w(F_SEMI, 22, value);
    ui_text(F_SEMI, 22, value, x, y, col);
    float th = ui_text_wrap(F_REG, 13, what, (Rectangle){ x + vw + 14, y + 6, w - vw - 14, 200 }, 19, TH.text2, true);
    return fmaxf(32, th + 10) + 4;
}

static const char *lenstr(double m)
{
    static char b[4][64]; static int k;
    k = (k + 1) & 3;
    char v[40], u[16]; wk_len(m, v, sizeof v, u, sizeof u);
    snprintf(b[k], sizeof b[k], "%s %s", v, u);
    return b[k];
}

/* ---------------------------------------------------------------- the tabs */
static float tab_wall(float x, float y, float w, bool wide)
{
    float lw = wide ? 400 : w, rx = wide ? x + lw + 30 : x, rw = wide ? w - lw - 30 : w;
    float ly = y;
    ly += fluid_inputs(x, ly, lw, false);
    ly += flow_choice(x, ly, lw);
    num_in("calc-U", x, &ly, lw, C.flow == CW_PIPE ? "Bulk velocity U" : "Free stream U", &C.U, 1e-6, 1e5, "m/s", NULL);
    num_in("calc-L", x, &ly, lw, C.flow == CW_PIPE ? "Pipe diameter D" : "Plate length L", &C.L, 1e-6, 1e5, "m",
           C.flow == CW_PIPE ? "The inside diameter (for another shape, the hydraulic diameter 4 A / P)" :
                               "The distance the boundary layer has grown over: the part's length along the flow");
    num_in("calc-yp", x, &ly, lw, "Target y+", &C.yplus, 0.01, 1000, NULL, "About 1 resolves the wall (k-omega SST, LES, heat transfer); 30 to 300 for wall functions");
    if (C.flow == CW_PIPE) num_in("calc-rough", x, &ly, lw, "Roughness ε/D", &C.rough, 0, 0.1, NULL, "0 for a smooth pipe; drawn tubing 1.5e-6 / D, commercial steel 4.5e-5 / D");
    num_in("calc-r", x, &ly, lw, "Layer growth r", &C.r, 1, 3, NULL, "Each prism layer this much thicker than the one below; 1.1 to 1.3 is usual");
    num_in("calc-cell", x, &ly, lw, "Cells beyond", &C.cell, 0, 1e3, "m", "The surface cells the layers blend into (0: leave out)");

    CalcWallIn in = { .flow = C.flow, .rho = C.rho, .mu = C.mu, .U = C.U, .L = C.L, .yplus = C.yplus, .rough = C.rough, .r = C.r, .cell = C.cell };
    CalcWallOut o; Working wk;
    bool ok = calc_wall(&in, &o, &wk);
    float ry = wide ? y : ly + 10;
    if (ok) {
        char v[64], t[400];
        ry += headline(rx, ry, rw, lenstr(o.h1), "first cell height: the first prism layer (its centre at y+ from the wall)", TH.lcars ? TH.lc[1] : TH.accent);
        snprintf(v, sizeof v, "Re %.3g", o.Re);
        pl_fmt_ui(t, sizeof t, "%s; friction velocity %.3g m/s, wall shear %.3g Pa", o.laminar ? "laminar" : "turbulent", o.utau, o.tauw);
        ry += headline(rx, ry, rw, v, t, TH.text);
        if (C.flow == CW_PLATE) {
            pl_fmt_ui(t, sizeof t, "boundary layer thickness at the end of the plate; %d layers growing by %.2g cover it (%s)", o.nCover, C.r, lenstr(o.tCover));
            ry += headline(rx, ry, rw, lenstr(o.delta), t, TH.text);
        }
        if (C.cell > 0 && C.r > 1) {
            snprintf(v, sizeof v, "%d layers", o.nBlend);
            pl_fmt_ui(t, sizeof t, "to blend into the %s cells beyond: %s in all, the outer one %s", lenstr(C.cell), lenstr(o.tBlend), lenstr(o.outerBlend));
            ry += headline(rx, ry, rw, v, t, TH.text);
        }
        if (blocks_wants_first()) {
            float bw = ui_button_width(ICON_BOXES, "Use in the block builder");
            if (ui_button(ui_id("calc-send-blocks"), (Rectangle){ rx, ry + 4, bw, 34 }, ICON_BOXES, "Use in the block builder", BTN_PRIMARY)) {
                blocks_take_first(o.h1); A.modal = MD_NONE;
            }
            ui_tooltip((Rectangle){ rx, ry + 4, bw, 34 }, "The template's first cell at the walls becomes this height; its grading follows");
            ry += 48;
        }
        if (C.stl && A.cs) {
            float bw = ui_button_width(ICON_LAYERS, "Send to the STL mesher");
            if (ui_button(ui_id("calc-send-layers"), (Rectangle){ rx, ry + 4, bw, 34 }, ICON_LAYERS, "Send to the STL mesher", BTN_PRIMARY)) {
                double f; int k;
                if (stlmesh_set_wall_layers(A.cs, o.h1, &f, &k))
                    ui_toast(TOAST_OK, "STL mesher: %d prism layer%s from %s. Generate the mesh again to use %s", k, k == 1 ? "" : "s", lenstr(f), k == 1 ? "it" : "them");
                else ui_toast(TOAST_WARN, "A first layer of %s needs more than 60 layers on the mesher's surface cells: make its minimum size smaller first", lenstr(o.h1));
            }
            ui_tooltip((Rectangle){ rx, ry + 4, bw, 34 }, "Prism layers on the walls starting at this first cell height, as many as reach the mesher's surface cells");
            ry += 48;
        }
    }
    ry += teach_working("calc-wk-wall", rx, ry + 4, rw, &wk) + 4;
    return fmaxf(ly, ry) - y;
}

/* the cells of an edge, drawn to scale */
static float cells_picture(float x, float y, float w, const CalcGradeOut *o)
{
    if (o->N < 1 || !(o->d1 > 0)) return 0;
    float h = 40;
    Rectangle r = { x, y, w, h };
    ui_rect(r, 6, TH.surface2);
    double L = 0, c = o->d1;
    for (int i = 0; i < o->N; i++) { L += c; c *= o->r; }
    float px = x + 8, pw = w - 16;
    c = o->d1;
    double at = 0;
    Color line = TH.lcars ? TH.lc[1] : TH.accent;
    ui_line(px, y + 8, px, y + h - 8, 1.5f, line);
    for (int i = 0; i < o->N && i < 400; i++) {
        at += c; c *= o->r;
        float xx = px + (float)(at / L) * pw;
        ui_line(xx, y + 8, xx, y + h - 8, 1.2f, line);
    }
    ui_line(px, y + h / 2, px + pw, y + h / 2, 1, ui_alpha(line, 0.4f));
    return h + 8;
}

static float tab_grade(float x, float y, float w, bool wide)
{
    float lw = wide ? 400 : w, rx = wide ? x + lw + 30 : x, rw = wide ? w - lw - 30 : w;
    float ly = y;
    static const char *modes[CG__N] = { "Cells and ratio", "First and last cell", "First cell and growth" };
    ui_segmented(ui_id("calc-gmode"), (Rectangle){ x, ly, lw, 34 }, modes, CG__N, &C.gmode);
    ly += 44;
    num_in("calc-gL", x, &ly, lw, "Edge length L", &C.gL, 1e-9, 1e6, "m", NULL);
    if (C.gmode == CG_N_RATIO) {
        num_in("calc-gN", x, &ly, lw, "Cells N", &C.gN, 1, 100000, NULL, NULL);
        num_in("calc-gR", x, &ly, lw, "Expansion ratio R", &C.gR, 1e-6, 1e6, NULL, "Last cell / first cell: the number blockMesh's simpleGrading takes. Below 1 shrinks.");
    } else if (C.gmode == CG_FIRST_LAST) {
        num_in("calc-gd1", x, &ly, lw, "First cell", &C.gd1, 1e-12, 1e6, "m", NULL);
        num_in("calc-gdn", x, &ly, lw, "Last cell", &C.gdn, 1e-12, 1e6, "m", NULL);
    } else {
        num_in("calc-gd1b", x, &ly, lw, "First cell", &C.gd1, 1e-12, 1e6, "m", "At a wall: the first cell height from the Wall spacing tab");
        num_in("calc-gr", x, &ly, lw, "Growth r", &C.gr, 0.01, 100, NULL, "Each cell this much larger than the one before (snappyHexMesh's expansionRatio)");
    }
    CalcGradeIn in = { .mode = C.gmode, .L = C.gL, .N = (int)round(C.gN), .R = C.gR, .d1 = C.gd1, .dn = C.gdn, .r = C.gr };
    CalcGradeOut o; Working wk;
    bool ok = calc_grading(&in, &o, &wk);
    float ry = wide ? y : ly + 10;
    if (ok) {
        char v[64], t[300];
        snprintf(v, sizeof v, "R = %.4g", o.R);
        pl_fmt_ui(t, sizeof t, "%d cells; growth %.4g from one to the next; first %s, last %s", o.N, o.r, lenstr(o.d1), lenstr(o.dn));
        ry += headline(rx, ry, rw, v, t, TH.lcars ? TH.lc[1] : TH.accent);
        ry += cells_picture(rx, ry, rw, &o);
        char snip[96]; snprintf(snip, sizeof snip, "(%d 1 1) simpleGrading (%.4g 1 1)", o.N, o.R);
        ui_text(F_MONO, 12.5f, snip, rx, ry + 9, TH.text2);
        float bw = ui_button_width(ICON_COPY, "Copy");
        if (ui_button(ui_id("calc-gcopy"), (Rectangle){ rx + rw - bw, ry, bw, 32 }, ICON_COPY, "Copy", BTN_GHOST)) {
            SetClipboardText(snip); ui_toast(TOAST_OK, "Copied: paste it into a block of blockMeshDict (the edge along x)");
        }
        ry += 42;
    }
    ry += teach_working("calc-wk-grade", rx, ry + 4, rw, &wk) + 4;
    return fmaxf(ly, ry) - y;
}

static float tab_cost(float x, float y, float w, bool wide)
{
    float lw = wide ? 400 : w, rx = wide ? x + lw + 30 : x, rw = wide ? w - lw - 30 : w;
    float ly = y;
    num_in("calc-cells", x, &ly, lw, "Cells", &C.cells, 1, 1e10, NULL, NULL);
    ui_text(F_REG, 13.5f, "Solver", x, ly + 9, TH.text2);
    ui_select(ui_id("calc-solver"), (Rectangle){ x + LW, ly, fminf(260, lw - LW), 34 }, CALC_SOLVER_NAME, CS__N, &C.solver);
    ly += 42;
    bool tr = C.solver == CS_TRANSIENT || C.solver == CS_FREESURFACE;
    num_in("calc-steps", x, &ly, lw, tr ? "Time steps" : "Iterations", &C.steps, 1, 1e9, NULL, tr ? "End time / time step" : "A steady run usually settles in 500 to 3000");
    if (tr) num_in("calc-nouter", x, &ly, lw, "Outer correctors", &C.nOuter, 1, 100, NULL, "PIMPLE's nOuterCorrectors (1 is PISO)");
    num_in("calc-procs", x, &ly, lw, "Processes", &C.procs, 1, 4096, NULL, "Run in parallel on this many cores");
    char m[160]; pl_fmt_ui(m, sizeof m, "This machine: %d cores, %.1f GB free", A.env.nCpu, mem_available_mb() / 1024);
    ui_text(F_REG, 12.5f, m, x, ly + 2, TH.text3);
    ly += 26;
    CalcCostIn in = { .cells = C.cells, .solver = C.solver, .steps = C.steps, .nOuter = (int)C.nOuter, .procs = (int)C.procs,
                      .memAvailMB = mem_available_mb(), .nCpu = A.env.nCpu };
    CalcCostOut o; Working wk;
    bool ok = calc_cost(&in, &o, &wk);
    float ry = wide ? y : ly + 10;
    if (ok) {
        char v[64], t[300];
        Color mc = o.level == 2 ? TH.err : o.level == 1 ? TH.warn : (TH.lcars ? TH.lc[1] : TH.accent);
        snprintf(v, sizeof v, "%.1f GB", fmax(o.meshMB, o.solveMB) / 1024);
        pl_fmt_ui(t, sizeof t, "memory at the peak: %.1f GB to mesh (snappyHexMesh), %.1f GB to solve%s", o.meshMB / 1024, o.solveMB / 1024,
                  o.level == 2 ? ". More than is free: Linux would kill it" : o.level == 1 ? ". Tight" : "");
        ry += headline(rx, ry, rw, v, t, mc);
        double avail = in.memAvailMB;
        if (avail > 0) {                                 /* the need against what is free */
            Rectangle bar = { rx, ry, rw, 10 };
            ui_rect(bar, 5, TH.surface2);
            float f = (float)fmin(1, fmax(o.meshMB, o.solveMB) / avail);
            ui_rect((Rectangle){ bar.x, bar.y, fmaxf(10, bar.width * f), bar.height }, 5, mc);
            ry += 22;
        }
        if (o.hours < 1) snprintf(v, sizeof v, "%.0f min", fmax(1, o.hours * 60));
        else if (o.hours < 48) snprintf(v, sizeof v, "%.1f h", o.hours);
        else snprintf(v, sizeof v, "%.1f days", o.hours / 24);
        pl_fmt_ui(t, sizeof t, "for the run: %.3g s %s on %d process%s (%.1f times one)", o.secPerStep, tr ? "a time step" : "an iteration",
                  in.procs, in.procs == 1 ? "" : "es", o.speedup);
        ry += headline(rx, ry, rw, v, t, TH.text);
    }
    ry += teach_working("calc-wk-cost", rx, ry + 4, rw, &wk) + 4;
    return fmaxf(ly, ry) - y;
}

static float tab_dim(float x, float y, float w, bool wide)
{
    float lw = wide ? 400 : w, rx = wide ? x + lw + 30 : x, rw = wide ? w - lw - 30 : w;
    float ly = y;
    ly += fluid_inputs(x, ly, lw, true);
    ly += flow_choice(x, ly, lw);
    num_in("calc-dU", x, &ly, lw, "Speed U", &C.U, 0, 1e5, "m/s", NULL);
    num_in("calc-dL", x, &ly, lw, C.flow == CW_PIPE ? "Diameter D" : "Length L", &C.L, 1e-9, 1e5, "m", "The length the number is built on: a pipe's diameter, a body's length along the flow, a heated wall's height");
    num_in("calc-dT", x, &ly, lw, "Temperature difference", &C.dT, 0, 1e4, "K", "Between a heated wall and the fluid, for buoyancy (0: none)");
    num_in("calc-g", x, &ly, lw, "Gravity g", &C.g, 0, 100, "m/s²", NULL);
    num_in("calc-dt", x, &ly, lw, "Time step Δt", &C.dt, 0, 1e6, "s", "For the Courant number (0: steady)");
    num_in("calc-dx", x, &ly, lw, "Cell size Δx", &C.dx, 0, 1e6, "m", NULL);
    CalcDimIn in = { .flow = C.flow, .rho = C.rho, .mu = C.mu, .cp = C.cp, .k = C.k, .beta = C.beta, .sigma = C.sigma, .c = C.c,
                     .U = C.U, .L = C.L, .dT = C.dT, .g = C.g, .dt = C.dt, .dx = C.dx };
    CalcDimOut o;
    calc_dimless(&in, &o);
    float ry = wide ? y : ly + 10;
    for (int k = 0; k < CN__N; k++) {
        bool sel = k == C.dimSel;
        float wh = o.have[k] ? ui_text_wrap(F_REG, 12.5f, o.why[k], (Rectangle){ 0, 0, rw - 24, 200 }, 18, BLANK, false) : 18;
        Rectangle row = { rx, ry, rw, 30 + wh + 8 };
        if (!ui_culled(row.y, row.y + row.height)) {
            if (ui_row(ui_idn(ui_id("calc-dim"), k), row, sel)) C.dimSel = k;
            Color col = !o.have[k] ? TH.text3 : o.level[k] == 2 ? TH.err : o.level[k] == 1 ? TH.warn : TH.text;
            char v[96];
            if (o.have[k]) { char n[40]; wk_num(n, sizeof n, o.v[k]); snprintf(v, sizeof v, "%s = %s", CALC_DIM_SYM[k], n); }
            else snprintf(v, sizeof v, "%s", CALC_DIM_SYM[k]);
            ui_text(F_MONO_MED, 14, v, row.x + 12, row.y + 8, col);
            char nm[80]; snprintf(nm, sizeof nm, "%s%s%s", CALC_DIM_NAME[k], o.have[k] ? ": " : "", o.have[k] ? o.verdict[k] : "");
            ui_text_clip(F_MED, 13.5f, nm, row.x + 190, row.y + 8, row.width - 200, col);
            if (o.have[k]) ui_text_wrap(F_REG, 12.5f, o.why[k], (Rectangle){ row.x + 12, row.y + 30, row.width - 24, 200 }, 18, TH.text2, true);
            else ui_text(F_REG, 12.5f, "Needs more inputs", row.x + 12, row.y + 30, TH.text3);
        }
        ry += row.height + 4;
    }
    Working wk; calc_dimless_working(&in, &o, C.dimSel, &wk);
    ry += teach_working("calc-wk-dim", rx, ry + 8, rw, &wk) + 8;
    return fmaxf(ly, ry) - y;
}

void calc_modal(bool *close)
{
    Rectangle b = ui_modal_begin("Calculators", "The numbers a mesh is sized by, worked out by hand: change any input.", 1080, 760, close);
    static const char *tabs[CALC__N] = { "Wall spacing (y+)", "Grading", "Cells, memory, time", "Dimensionless numbers" };
    float th = 0;
    int was = C.tab;
    ui_tabs(ui_id("calc-tabs"), b.x, b.y, tabs, CALC__N, &C.tab, &th);
    if (C.tab != was) C.sc.target = C.sc.y = 0;
    Rectangle v = { b.x, b.y + th + 10, b.width, b.height - th - 10 };
    float y = ui_scroll_begin(&C.sc, v), y0 = y, x = v.x + 2, w = v.width - 20;
    LW = w < 520 ? 140 : 170;
    if (C.from[0]) y += ui_text_wrap(F_REG, 12.5f, C.from, (Rectangle){ x, y, w, 80 }, 18, TH.text3, true) + 12;
    bool wide = w >= 820;
    switch (C.tab) {
    case CALC_WALL: y += tab_wall(x, y, w, wide); break;
    case CALC_GRADE: y += tab_grade(x, y, w, wide); break;
    case CALC_COST: y += tab_cost(x, y, w, wide); break;
    default: y += tab_dim(x, y, w, wide); break;
    }
    ui_scroll_end(&C.sc, y - y0 + 20);
}
