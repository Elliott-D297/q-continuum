/* page_meshadvice.c -- the "What mesh do I need?" dialog (mesh overhaul plan step 7, gap L6): the questions on the left,
 * the way to mesh it on the right with each step of the reasoning, and a button that opens that path set up (the block
 * builder on its template with the first cell, the STL mesher on the mesher, the import page). The choosing is
 * meshadvice.c; this file asks and shows. Opened from the palette (help.meshq), Mesh's source picker and Calculators. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "calc.h"
#include "meshadvice.h"
#include "simsetup.h"
#include "stlmesh.h"

static struct {
    MaIn in;
    int  fluid;                       /* 0 air, 1 water, 2 the case's, 3 custom ν           */
    double caseNu;
    char from[260];
    UiScroll sc;
} Q = { .in = { .shape = MA_PART, .dims = MA_3D, .flow = MA_EXTERNAL, .walls = MA_WALLS_FUNC, .goal = MA_ANSWER, .U = 10, .L = 1,
                .nu = 1.516e-5 } };

static const double NU_AIR = 1.516e-5, NU_WATER = 1.004e-6;   /* 20 °C (Cengel), as the Calculators' fluids */

/* the open case's answers, as far as it gives them */
static void fill_from_case(void)
{
    Q.from[0] = 0;
    Q.in.salome = A.env.salome[0] != 0;
    Case *c = A.cs;
    if (!c || c->s.prog) return;
    char parts[200] = "";
    char p[PL_PATH];
    pl_join(p, sizeof p, c->s.path, "mesher/mesher.ini");
    bool stl = pl_is_file(p);
    double size = 0; bool ext = true;
    if (stl) {
        SimPatch pa[1]; stlmesh_patches(c, pa, 0, &size, &ext);
        Q.in.shape = MA_PART; Q.in.flow = ext ? MA_EXTERNAL : MA_INTERNAL;
        Q.in.dims = stlmesh_two_d(c) ? MA_2D : MA_3D;
        snprintf(parts, sizeof parts, "its STL part");
    } else {
        pl_join(p, sizeof p, c->s.path, "mesher/blocks.ini");
        char *t = pl_read_file(p, NULL);
        if (t) {
            static const struct { const char *k; int shape, dims, flow; } M[] = {
                { "cavity", MA_BOX, MA_2D, MA_INTERNAL }, { "channel", MA_CHANNEL, MA_2D, MA_INTERNAL }, { "step", MA_STEP, MA_2D, MA_INTERNAL },
                { "plate", MA_PLATE, MA_2D, MA_EXTERNAL }, { "pipe", MA_PIPE, MA_3D, MA_INTERNAL }, { "cylinder", MA_CYLINDER, MA_2D, MA_EXTERNAL },
                { "airfoil", MA_AIRFOIL, MA_2D, MA_EXTERNAL }, { "wedge", MA_PIPE, MA_AXI, MA_INTERNAL } };
            for (size_t i = 0; i < sizeof M / sizeof M[0]; i++) {
                char key[64]; snprintf(key, sizeof key, "template = %s\n", M[i].k);
                if (strstr(t, key)) { Q.in.shape = M[i].shape; Q.in.dims = M[i].dims; Q.in.flow = M[i].flow; snprintf(parts, sizeof parts, "its block builder template"); }
            }
            free(t);
        }
    }
    SimSetup s;
    if (sim_load(c->s.path, &s, size)) {
        double U = sim_ref_speed(&s);
        if (U > 0) Q.in.U = U;
        if (s.nu > 0) { Q.caseNu = s.nu; Q.fluid = 2; Q.in.nu = s.nu; }
        double yp = sim_yplus_target(&s);
        Q.in.walls = yp <= 5 ? MA_WALLS_RESOLVE : MA_WALLS_FUNC;
        char t[120]; snprintf(t, sizeof t, "%sthe fluid and its speed, y+ %g", parts[0] ? ", " : "", yp);
        size_t n = strlen(parts); snprintf(parts + n, sizeof parts - n, "%s", t);
    }
    if (size > 0) Q.in.L = size;
    if (parts[0]) pl_fmt_ui(Q.from, sizeof Q.from, "Filled in from %s: %s. Change any answer; nothing changes the case until you press the button.", c->s.name, parts);
}

void meshq_open(void)
{
    if (A.modal != MD_MESHQ) {                                /* the last case's fluid doesn't carry over */
        Q.caseNu = 0;
        if (Q.fluid == 2) { Q.fluid = 0; Q.in.nu = NU_AIR; }
        fill_from_case();
        Q.sc = (UiScroll){ 0 };
    }
    A.modal = MD_MESHQ;
}

/* ---------------------------------------------------------------- the questions */
static float LW = 150;

static float ask_label(const char *q, float x, float y)
{
    ui_text(F_MED, 13.5f, q, x, y, TH.text);
    return 22;
}

static float ask_seg(const char *id, const char *q, float x, float y, float w, const char **items, int n, int *v)
{
    float h = ask_label(q, x, y);
    ui_segmented(ui_id(id), (Rectangle){ x, y + h, w, 34 }, items, n, v);
    return h + 46;
}

static float ask_sel(const char *id, const char *q, float x, float y, float w, const char *const *items, int n, int *v)
{
    float h = ask_label(q, x, y);
    ui_select(ui_id(id), (Rectangle){ x, y + h, w, 34 }, items, n, v);
    return h + 46;
}

static bool num_in(const char *id, float x, float *y, float w, const char *label, double *v, double lo, double hi, const char *unit)
{
    ui_text(F_REG, 13.5f, label, x, *y + 9, TH.text2);
    float fw = fminf(170, w - LW);
    Rectangle r = { x + LW, *y, fw, 34 };
    char b[64];
    if (*v != 0 && (fabs(*v) < 1e-3 || fabs(*v) >= 1e6)) snprintf(b, sizeof b, "%.4g", *v); else snprintf(b, sizeof b, "%.6g", *v);
    float uw = unit && *unit ? ui_text_w(F_MONO, 12, unit) + 12 : 0;
    if (uw) ui_text(F_MONO, 12, unit, r.x + r.width - uw + 6, r.y + 10, TH.text3);
    bool ch = false;
    if (ui_input(ui_id(id), (Rectangle){ r.x, r.y, r.width - uw, r.height }, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) {
        char *e; double d = strtod(b, &e);
        while (e && *e == ' ') e++;
        if (e == b || (e && *e) || !isfinite(d) || d < lo || d > hi) ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi);
        else { *v = d; ch = true; }
    }
    *y += 42;
    return ch;
}

static float questions(float x, float y, float w)
{
    float y0 = y;
    y += ask_sel("mq-shape", "What are you meshing?", x, y, w, MA_SHAPE_NAME, MA__SHAPES, &Q.in.shape);
    if (Q.in.shape == MA_HAVE) {
        y += ask_sel("mq-from", "Made in", x, y, w, MA_FROM_NAME, MA__FROM, &Q.in.from);
        return y - y0;
    }
    if (Q.in.shape == MA_PART) {
        static const char *F[2] = { "STL", "STEP / IGES" };
        y += ask_seg("mq-file", "The part's file", x, y, w, F, 2, &Q.in.file);
    }
    static const char *D[3] = { "3-D", "2-D", "Axisymmetric" };
    y += ask_seg("mq-dims", "Does the flow vary in all three directions?", x, y, w, D, 3, &Q.in.dims);
    if (Q.in.shape == MA_PART || Q.in.shape == MA_BOX) {
        static const char *FL[2] = { "Through it (internal)", "Round it (external)" };
        y += ask_seg("mq-flow", "Where is the flow?", x, y, w, FL, 2, &Q.in.flow);
    }
    static const char *W[MA__WALLS] = { "Nothing: they just hold the flow in", "Pressure drop, lift, drag (a few %)", "Heat transfer, separation, exact drag" };
    y += ask_sel("mq-walls", "What do you need at the walls?", x, y, w, W, MA__WALLS, &Q.in.walls);
    static const char *G[MA__GOALS] = { "A first look, in minutes", "An engineering answer", "An answer shown not to depend on the mesh" };
    y += ask_sel("mq-goal", "How good an answer?", x, y, w, G, MA__GOALS, &Q.in.goal);
    y += ask_label("The flow (for the Reynolds number and the first cell)", x, y);
    static const char *FN[4] = { "Air, 20 °C", "Water, 20 °C", "This case's fluid", "Other" };
    int f = Q.fluid;
    ui_text(F_REG, 13.5f, "Fluid", x, y + 9, TH.text2);
    if (ui_select(ui_id("mq-fluid"), (Rectangle){ x + LW, y, fminf(220, w - LW), 34 }, FN, 4, &f) && f != Q.fluid) {
        if (f == 2 && !(Q.caseNu > 0)) ui_toast(TOAST_INFO, "The open case has no fluid set yet (Physics > Setup)");
        else { Q.fluid = f; Q.in.nu = f == 0 ? NU_AIR : f == 1 ? NU_WATER : f == 2 ? Q.caseNu : Q.in.nu; }
    }
    y += 42;
    if (Q.fluid == 3) num_in("mq-nu", x, &y, w, "Kinematic viscosity ν", &Q.in.nu, 1e-9, 10, "m²/s");
    num_in("mq-U", x, &y, w, "Speed", &Q.in.U, 0, 1e5, "m/s");
    bool pipe = Q.in.shape == MA_PIPE || Q.in.shape == MA_CHANNEL || (Q.in.shape == MA_BOX && Q.in.flow == MA_INTERNAL);
    num_in("mq-L", x, &y, w, pipe ? "Diameter / height" : "Length along the flow", &Q.in.L, 1e-9, 1e5, "m");
    return y - y0;
}

/* ---------------------------------------------------------------- the answer */
static void go(const MaOut *o)
{
    Case *c = A.cs;
    switch (o->go) {
    case MA_GO_BLOCKS:
        if (!c || c->s.prog) { ui_toast(TOAST_INFO, "Open a case first, or make one with New case: the block builder writes into its system/blockMeshDict"); return; }
        blocks_suggest(o->tmpl, o->layers ? o->h1 : 0, o->cells);
        A.meshTab = 5; app_go(PG_MESH); A.modal = MD_NONE;
        break;
    case MA_GO_STL:
        A.modal = MD_NONE;
        if (!c || c->s.prog) { stlmesh_new_case(); ui_toast(TOAST_INFO, "Pick the part's file: a case is made for it, then set %s on Mesh settings", MESHERS[o->mesher].shortName); return; }
        A.meshTab = 3; app_go(PG_MESH);
        if (stlmesh_suggest(c, o->mesher, o->layers)) ui_toast(TOAST_OK, "STL mesher: %s%s", MESHERS[o->mesher].shortName, o->layers ? ", prism layers on" : "");
        else { stlmesh_choose(); ui_toast(TOAST_INFO, "Pick the part's file first; then %s on Mesh settings", MESHERS[o->mesher].shortName); }
        if (o->layers && o->h1 > 0) {
            double f; int k;
            if (stlmesh_set_wall_layers(c, o->h1, &f, &k)) ui_toast(TOAST_OK, "STL mesher: %d prism layers from a first cell of %.3g mm", k, f * 1e3);
        }
        break;
    case MA_GO_IMPORT:
        A.modal = MD_NONE;
        if (!c || c->s.prog) { salome_import_new_case(); return; }
        A.meshTab = 1; app_go(PG_MESH);
        break;
    default:
        SetClipboardText(o->cmd);
        ui_toast(TOAST_OK, "Copied: %s", o->cmd);
        break;
    }
}

static float answer(float x, float y, float w)
{
    float y0 = y;
    MaOut o; ma_advise(&Q.in, &o);
    Color acc = TH.lcars ? TH.lc[1] : TH.accent;
    ui_text(F_SEMI, 12.5f, ui_cap("The way to mesh it"), x, y, head_color()); y += 22;
    y += ui_text_wrap(F_SEMI, 19, o.title, (Rectangle){ x, y, w, 80 }, 26, acc, true) + 8;
    if (o.go != MA_GO_CONVERT) {
        char t[200], cw[40];
        if (o.cells >= 1e6) snprintf(cw, sizeof cw, "%.2g million", o.cells / 1e6); else snprintf(cw, sizeof cw, "%.0f thousand", round(o.cells / 1e3));
        if (o.nMeshes > 1) pl_fmt_ui(t, sizeof t, "Three meshes, the finest about %s cells", cw);
        else pl_fmt_ui(t, sizeof t, "About %s cells", cw);
        if (o.Re > 0) {                                    /* a learner reads 99,600, not 9.96e+04 */
            char re[40]; double r3 = o.Re >= 1000 ? round(o.Re / pow(10, floor(log10(o.Re)) - 2)) * pow(10, floor(log10(o.Re)) - 2) : o.Re;
            if (r3 >= 1000 && r3 < 1e9) { long v = lround(r3); if (v >= 1000000) snprintf(re, sizeof re, "%ld,%03ld,%03ld", v / 1000000, v / 1000 % 1000, v % 1000); else snprintf(re, sizeof re, "%ld,%03ld", v / 1000, v % 1000); }
            else snprintf(re, sizeof re, "%.3g", r3);
            size_t n = strlen(t); pl_fmt_ui(t + n, sizeof t - n, "  ·  Re %s", re);
        }
        if (o.h1 > 0) { size_t n = strlen(t); pl_fmt_ui(t + n, sizeof t - n, "  ·  first cell %.3g mm (y+ %g)", o.h1 * 1e3, o.yplus); }
        ui_text(F_MED, 13.5f, t, x, y, TH.text2);
        y += 28;
    } else {
        ui_rect((Rectangle){ x, y, w, 34 }, 6, TH.surface2);
        ui_text_clip(F_MONO, 12.5f, o.cmd, x + 10, y + 10, w - 20, TH.text);
        y += 44;
    }
    static const char *GO[MA__GO] = { "Open the block builder with it", "Open the STL mesher with it", "Import the mesh", "Copy the command" };
    static const char *const GI[MA__GO] = { ICON_BOXES, ICON_LAYERS, ICON_FOLDER_OPEN, ICON_COPY };
    float bw = ui_button_width(GI[o.go], GO[o.go]);
    if (ui_button(ui_id("mq-go"), (Rectangle){ x, y, bw, 36 }, GI[o.go], GO[o.go], BTN_PRIMARY)) go(&o);
    y += 52;
    ui_text(F_SEMI, 12.5f, ui_cap("Why"), x, y, head_color()); y += 24;
    for (int i = 0; i < o.nSteps; i++) {
        ui_icon(ICON_CHEVRON_RIGHT, x + 7, y + 9, 13, acc);
        ui_text_clip(F_MED, 13.5f, o.step[i].q, x + 22, y, w - 22, TH.text);
        y += 22;
        y += ui_text_wrap(F_REG, 13, o.step[i].r, (Rectangle){ x + 22, y, w - 22, 300 }, 19, TH.text2, true) + 12;
    }
    if (o.go == MA_GO_BLOCKS || o.go == MA_GO_STL) {
        float cw = ui_button_width(ICON_SIGMA, "Calculators");
        if (ui_button(ui_id("mq-calc"), (Rectangle){ x, y, cw, 32 }, ICON_SIGMA, "Calculators", BTN_GHOST)) calc_open(CALC_WALL);
        ui_tooltip((Rectangle){ x, y, cw, 32 }, "The first cell, the grading and the memory and run time, worked out step by step");
        y += 42;
    }
    return y - y0;
}

void meshq_modal(bool *close)
{
    Rectangle b = ui_modal_begin("What mesh do I need?", "A few questions about the flow; the way to mesh it, and why.", 1080, 760, close);
    float y = ui_scroll_begin(&Q.sc, b), y0 = y, x = b.x + 2, w = b.width - 20;
    LW = w < 520 ? 130 : 150;
    if (Q.from[0]) y += ui_text_wrap(F_REG, 12.5f, Q.from, (Rectangle){ x, y, w, 80 }, 18, TH.text3, true) + 12;
    bool wide = w >= 820;
    float lw = wide ? 400 : w, rx = wide ? x + lw + 34 : x, rw = wide ? w - lw - 34 : w;
    float lh = questions(x, y, lw);
    float rh = answer(rx, wide ? y : y + lh + 16, rw);
    y += wide ? fmaxf(lh, rh) : lh + 16 + rh;
    ui_scroll_end(&Q.sc, y - y0 + 20);
}
