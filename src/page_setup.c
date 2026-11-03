/* page_setup.c -- Physics > Setup (user request, 2026-10-24): the whole simulation set up here instead of in the files.
 * The flow, the fluid, the turbulence model, each boundary's conditions, the starting values and the run controls are
 * kept in <case>/simulation.ini (simsetup.c) as they are edited; Write case files turns them into 0/, constant/ and
 * system/. The boundaries are the STL mesher's (renamed and typed there, by clicking the part), the wind tunnel's, and
 * any other patch the mesh has. setup_boundary_editor is also drawn beside the STL on the mesher tab.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "app.h"
#include "safety.h"
#include "simsetup.h"
#include "foamdict.h"
#include "precheck.h"

/* Setup's steps (the page, below) */
enum { SS_MODELS = 0, SS_MATERIALS, SS_BOUNDARIES, SS_START, SS_REPORTS, SS_SOLVER, SS_CHECK, SS__N };

static struct {
    char     casePath[PL_PATH];
    SimSetup s;
    double   size;               /* the part's length, m                       */
    double   height;             /* buoyancy's length, m: the mesh's extent along gravity (else size) */
    bool     external;           /* the STL sits in a wind tunnel              */
    double   cdMt;               /* system/controlDict when its controls were last taken in */
    int      step;               /* the open step (SS_*)                       */
    bool     picking;            /* the template cards, over a setup that has one */
    bool     more[SS__N];            /* a step's More rows shown                   */
} P;

/* a file's modification time to the nanosecond (0: none): a setup edited in the same second as the write still counts */
static double mtime_ns(const char *p)
{
    struct stat st;
    return stat(p, &st) ? 0 : (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 1e-9;
}

/* ============================================================================ the setup of the open case */
static void save(void)
{
    if (P.casePath[0]) sim_save(P.casePath, &P.s);
}

/* a mesh patch's role from its name (a SALOME or Gmsh group: "inlet", "outflow", "symmetry_plane" ...), else its type: only
   for a patch new to the setup, which then keeps what the user chose */
static int role_guess(const char *name, const char *type)
{
    char l[128]; size_t i = 0;
    for (; name[i] && i < sizeof l - 1; i++) l[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    l[i] = 0;
    if (strstr(l, "inlet") || strstr(l, "inflow")) return ROLE_INLET;
    if (strstr(l, "outlet") || strstr(l, "outflow") || strstr(l, "exit")) return ROLE_OUTLET;
    if (strstr(l, "sym") || !strcmp(type, "symmetry") || !strcmp(type, "symmetryPlane")) return ROLE_SYMMETRY;
    if (strstr(l, "farfield") || strstr(l, "far_field") || strstr(l, "open") || strstr(l, "atmos") || strstr(l, "ambient")) return ROLE_OPENING;
    return ROLE_WALL;
}

/* the boundaries: the STL's and the tunnel's (stlmesh_patches), then the mesh's own patches not among them */
static void refresh(Case *c)
{
    SimPatch p[SIM_MAX_B];
    int n = stlmesh_patches(c, p, SIM_MAX_B, &P.size, &P.external);
    /* a mesh being generated is half made (blockMesh's background patch, no STL patches yet): its patches aren't the
       case's; the ones from the mesh before stay as they were until it is done */
    if (A.cj->active && !strcmp(A.cj->casePath, c->s.path) && !strcmp(A.cj->title, "Mesh")) {
        for (int i = 0; i < P.s.nB && n < SIM_MAX_B; i++) {
            const SimBoundary *b = &P.s.b[i];
            bool have = false;
            for (int k = 0; k < n; k++) if (!strcmp(p[k].name, b->name)) { have = true; p[k].meshType = b->meshType; }
            if (!have) p[n++] = (SimPatch){ b->name, b->role, b->meshType, false, b->area };
        }
        sim_set_boundaries(&P.s, p, n);
        return;
    }
    /* only a real mesh's patches (constant/polyMesh/boundary): without one the case's patch list comes from blockMeshDict
       or from the fields, whose "types" are conditions (noSlip ...), not what the mesh will be */
    for (int i = 0; i < c->nPatches && n < SIM_MAX_B && c->patchSource == 1; i++) {
        bool have = false;
        for (int k = 0; k < n; k++) if (!strcmp(p[k].name, c->patches[i].name)) { have = true; p[k].meshType = c->patches[i].type; }
        if (have) continue;
        const char *t = c->patches[i].type;
        p[n++] = (SimPatch){ c->patches[i].name, role_guess(c->patches[i].name, t), t, false, 0 };
    }
    sim_set_boundaries(&P.s, p, n);
}

/* the wind tunnel's free stream at a speed: along the mesher's flow direction */
static void free_stream(Case *c, double speed, double out[3])
{
    double v[3];
    stlmesh_flow(c, v);
    for (int k = 0; k < 3; k++) out[k] = speed * v[k];
}

/* our controlDict edited elsewhere (the Run page's end time, Files, an editor): its run controls into the setup, so
   Write case files keeps them. The files have them already, so that alone doesn't make them out of date. */
static void take_controls(Case *c)
{
    char cd[PL_PATH]; pl_join(cd, sizeof cd, c->s.path, "system/controlDict");
    double mt = mtime_ns(cd);
    if (mt == P.cdMt) return;
    P.cdMt = mt;
    if (!sim_take_controls(c->s.path, &P.s)) return;
    char ini[PL_PATH], u[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(u, sizeof u, c->s.path, "0/U");
    struct stat st;
    bool had = !stat(ini, &st);
    double uMt = mtime_ns(u);
    save();
    if (had && uMt && mtime_ns(ini) > uMt && st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 1e-9 <= uMt) {
        struct timespec ts[2] = { st.st_mtim, st.st_mtim };     /* as old as it was: still not newer than the files */
        utimensat(AT_FDCWD, ini, ts, 0);
    }
}

/* buoyancy's gravity, while it isn't typed in: down the STL mesher's up direction (kept as it is without an STL) */
static void gravity_auto(Case *c)
{
    double g[3], lo[3], hi[3], gm;
    P.height = P.size;                                      /* the Rayleigh number's length: the height of the mesh */
    if (P.s.energy == EN_BUOYANT && (gm = sim_g_mag(&P.s)) > 0 && mesh_bounds(c->s.path, lo, hi)) {
        double h = 0;
        for (int k = 0; k < 3; k++) h += (hi[k] - lo[k]) * fabs(P.s.g[k]) / gm;
        if (h > 0) P.height = h;
    }
    if (!(P.height > 0) && P.s.energy != EN_OFF && mesh_bounds(c->s.path, lo, hi))   /* no STL: the mesh's largest side */
        for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > P.height) P.height = hi[k] - lo[k];
    /* the Nusselt number's length, while it isn't typed in: the same height (heat on only, so other cases' files stay current) */
    if (P.s.energy != EN_OFF && P.s.nuAuto && P.height > 0 && fabs(P.s.nuLength - P.height) > 1e-9 * P.height) { P.s.nuLength = P.height; save(); }
    if (P.s.energy != EN_BUOYANT || !P.s.gAuto || !stlmesh_gravity(c, g) || !memcmp(g, P.s.g, sizeof g)) return;
    memcpy(P.s.g, g, sizeof g);
    save();
}

static void sync(Case *c)
{
    static double at = -1;
    if (!strcmp(P.casePath, c->s.path) && at == ui_time()) return;   /* once a frame */
    at = ui_time();
    if (strcmp(P.casePath, c->s.path)) {
        pl_strlcpy(P.casePath, c->s.path, sizeof P.casePath);
        P.size = 0; P.external = false; P.cdMt = 0;
        P.step = 0; P.picking = false; memset(P.more, 0, sizeof P.more);
        SimPatch dummy[1];
        stlmesh_patches(c, dummy, 0, &P.size, &P.external);
        bool had = sim_load(c->s.path, &P.s, P.size);
        refresh(c);
        if (!had && P.external)                             /* a wind tunnel: the air starts at the inlet's speed */
            for (int i = 0; i < P.s.nB; i++) if (!strcmp(P.s.b[i].name, "inlet") && P.s.b[i].role == ROLE_INLET) free_stream(c, P.s.b[i].speed, P.s.U0);
        if (had) take_controls(c);
        return;
    }
    refresh(c);
    take_controls(c);
    gravity_auto(c);
}

void setup_flow_changed(Case *c)
{
    sync(c);
    if (!P.external) return;
    SimBoundary *in = sim_find(&P.s, "inlet");             /* the tunnel's inlet sits on the other side now: the start turns too */
    if (in && in->role == ROLE_INLET) { free_stream(c, in->inlet == IN_SPEED ? in->speed : sim_ref_speed(&P.s), P.s.U0); save(); }
}

void setup_rename(Case *c, const char *from, const char *to)
{
    sync(c);
    sim_rename(&P.s, from, to);
    save();
}

/* ============================================================================ fields */
static bool num_in(UiId id, Rectangle r, double *v, const char *fmt, double lo, double hi, const char *unit)
{
    char b[64]; snprintf(b, sizeof b, fmt, *v);
    float uw = unit ? ui_text_w(F_MONO, 12, unit) + 12 : 0;
    if (unit) ui_text(F_MONO, 12, unit, r.x + r.width - uw + 6, r.y + (r.height - 12) / 2 - 1, TH.text3);
    Rectangle ir = { r.x, r.y, r.width - uw, r.height };
    if (!ui_input(id, ir, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) return false;
    char *e; double d = strtod(b, &e);
    while (e && *e == ' ') e++;
    if (e == b || (e && *e) || !(d >= lo && d <= hi)) { ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi); return false; }
    *v = d;
    return true;
}

static bool row_num(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, double *v, double lo, double hi,
                    const char *unit, const char *after)
{
    Rectangle cr = prop_row(x, y, w, label, hints ? hint : NULL, 34);
    float fw = fminf(180, cr.width);
    bool ch = num_in(ui_id(id), (Rectangle){ cr.x, cr.y, fw, 34 }, v, "%.6g", lo, hi, unit);
    if (after && *after) ui_text_clip(F_REG, 13, after, cr.x + fw + 12, cr.y + 9, cr.width - fw - 12, TH.text3);
    if (ch) save();
    return ch;
}

static bool row_vec(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, double v[3], const char *unit)
{
    Rectangle cr = prop_row(x, y, w, label, hints ? hint : NULL, 34);
    static const char *const AX[3] = { "x", "y", "z" };
    float fw = fminf(120, (cr.width - 16) / 3);
    bool ch = false;
    for (int k = 0; k < 3; k++) {
        char u[16]; snprintf(u, sizeof u, "%s %s", AX[k], k == 2 && unit ? unit : "");
        if (num_in(ui_idn(ui_id(id), k), (Rectangle){ cr.x + k * (fw + 8), cr.y, fw, 34 }, &v[k], "%.6g", -1e9, 1e9, u)) ch = true;
    }
    if (ch) save();
    return ch;
}

/* a length in the unit that suits it ("35 µm", "1.2 mm", "0.4 m"); four buffers in turn, for several in one line */
static const char *fmt_len(double m)
{
    static char b[4][32]; static int k;
    char *o = b[k++ & 3];
    double a = fabs(m);
    if (a > 0 && a < 1e-3) snprintf(o, 32, "%.3g µm", m * 1e6);
    else if (a > 0 && a < 1) snprintf(o, 32, "%.3g mm", m * 1e3);
    else snprintf(o, 32, "%.3g m", m);
    return o;
}

static double vlen3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void fmt_flow(char *o, size_t cap, double u) { if (u > 0) pl_fmt_ui(o, cap, "mean %.4g m/s", u); else o[0] = 0; }
static const char *celsius(char *o, size_t cap, double K) { pl_fmt_ui(o, cap, "%.4g °C", K - 273.15); return o; }

/* ============================================================================ one boundary */
static float note(float x, float y, float w, const char *t);

float setup_boundary_editor(Case *c, const char *name, float x, float y, float w)
{
    sync(c);
    float y0 = y;
    SimBoundary *b = sim_find(&P.s, name);
    if (!b) return 0;
    bool hints = w > 620, turb = P.s.turb != TURB_LAMINAR, heat = P.s.energy != EN_OFF, gas = sim_is_gas(&P.s);
    ui_push_id(ui_id(b->name));
    if (sim_constraint(b->meshType)) {
        char t[200]; pl_fmt_ui(t, sizeof t, "A %s patch in the mesh: every field takes that type, so there is nothing to set.", b->meshType);
        y += ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y + 4, w, 200 }, 19, TH.text3, true) + 12;
        ui_pop_id();
        return y - y0;
    }
    char a[96] = "";
    switch (b->role) {
    case ROLE_INLET: {
        Rectangle cr = prop_row(x, &y, w, "Inlet by", hints ? "What you know about the flow coming in." : NULL, 34);
        int k = b->inlet;
        if (!gas && k == IN_MACH) k = IN_SPEED;             /* Mach inlets are a gas's; an incompressible setup writes it as speed */
        if (ui_select(ui_id("in-kind"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, INLET_NAME, gas ? IN__N : IN_MACH, &k) && k != b->inlet) { b->inlet = k; save(); }
        char ma[96] = "";
        if (gas) { double a0 = sim_sound_speed(&P.s, b->T); if (a0 > 0 && b->inlet == IN_SPEED) pl_fmt_ui(ma, sizeof ma, "Ma %.3g", b->speed / a0); }
        if (b->inlet == IN_MACH && gas) {
            double a0 = sim_sound_speed(&P.s, b->T);
            pl_fmt_ui(a, sizeof a, "%.4g m/s at %.4g K", b->mach * a0, b->T);
            row_num(x, &y, w, hints, "in-mach", "Mach number", "The speed over the speed of sound at the inlet's temperature, straight in.", &b->mach, 0, 50, NULL, a);
        } else if (b->inlet == IN_SPEED) {
            double was = b->speed;
            if (row_num(x, &y, w, hints, "in-speed", "Speed", "Into the domain, straight through the boundary.", &b->speed, 0, 1e5, "m/s", ma)
                && P.external && !strcmp(b->name, "inlet")) {
                double w0[3]; free_stream(c, was, w0);          /* the start was the free stream: it follows the inlet */
                if (P.s.U0[0] == w0[0] && P.s.U0[1] == w0[1] && P.s.U0[2] == w0[2]) { free_stream(c, b->speed, P.s.U0); save(); }
            }
        } else if (b->inlet == IN_VECTOR)
            row_vec(x, &y, w, hints, "in-vec", "Velocity", "In the mesh's axes.", b->vec, "m/s");
        else if (b->inlet == IN_FLOW) {
            fmt_flow(a, sizeof a, b->area > 0 ? b->flow / b->area : 0);
            row_num(x, &y, w, hints, "in-flow", "Volume flow rate", "Spread evenly over the boundary.", &b->flow, 0, 1e9, "m³/s", a);
        } else if (b->inlet == IN_MASS) {
            fmt_flow(a, sizeof a, b->area > 0 ? b->mdot / (P.s.rho * b->area) : 0);
            row_num(x, &y, w, hints, "in-mdot", "Mass flow rate", "At the fluid's density.", &b->mdot, 0, 1e9, "kg/s", a);
        } else if (gas) {
            pl_fmt_ui(a, sizeof a, "%.6g Pa absolute", sim_pabs(&P.s, b->p));
            row_num(x, &y, w, hints, "in-p", "Total pressure", "Gauge, above the operating pressure: the gas starts from rest at it and speeds up to the outlet's "
                                                             "pressure (isentropically).", &b->p, -1e9, 1e9, "Pa", a);
        } else
            row_num(x, &y, w, hints, "in-p", "Total pressure", "Gauge (above the outlet's): the flow comes in as fast as this pushes it.", &b->p, -1e9, 1e9, "Pa", NULL);
        if (turb) {
            /* plan step 5: by an intensity and a mixing length, a viscosity ratio, or a pipe's diameter */
            Rectangle tr = prop_row(x, &y, w, "Turbulence by", hints ? "What you know about the turbulence coming in. Not sure: a pipe or duct by its diameter, "
                                                                       "anything else by intensity and length." : NULL, 34);
            int ts = b->turbSpec;
            if (ui_select(ui_id("in-tspec"), (Rectangle){ tr.x, tr.y, fminf(240, tr.width), 34 }, TI_NAME, TI__N, &ts) && ts != b->turbSpec) { b->turbSpec = ts; save(); }
            double Ub = b->inlet == IN_SPEED ? fabs(b->speed) : sim_ref_speed(&P.s), I, Lm;
            sim_inlet_turbulence(&P.s, b, Ub, &I, &Lm);
            if (b->turbSpec != TI_DIAMETER)
                row_num(x, &y, w, hints, "in-ti", "Turbulence intensity", "1 % is calm (a wind tunnel), 5 % a duct, 10 % or more behind a fan.", &b->intensity, 0.01, 100, "%", NULL);
            if (b->turbSpec == TI_LENGTH)
                row_num(x, &y, w, hints, "in-tl", "Mixing length", "About 7 % of the duct's width, or of the body for a wind tunnel.", &b->length, 1e-9, 1e6, "m", NULL);
            else if (b->turbSpec == TI_RATIO) {
                pl_fmt_ui(a, sizeof a, "mixing length %s", fmt_len(Lm));
                row_num(x, &y, w, hints, "in-ratio", "Viscosity ratio", "nut / nu: 1 to 10 in calm or external flow, 10 to 100 in ducts, more behind a fan.", &b->ratio, 1e-6, 1e8, NULL, a);
            } else {
                pl_fmt_ui(a, sizeof a, "%s%.3g %%, mixing length %s", b->dh > 0 ? "" : TextFormat("0: from its area. "), I, fmt_len(Lm));
                row_num(x, &y, w, hints, "in-dh", "Hydraulic diameter", "4 x area / perimeter: a round pipe's diameter. The intensity follows from its Reynolds number "
                                                                        "(0.16 Re^-1/8), the length is 7 % of it.", &b->dh, 0, 1e6, "m", a);
            }
        }
        if (heat && gas && b->inlet == IN_PRESSURE)
            row_num(x, &y, w, hints, "in-T", "Total temperature", "Of the gas at rest before it speeds up, in kelvin; it cools as it does.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        else if (heat) row_num(x, &y, w, hints, "in-T", "Temperature", "Of the fluid coming in, in kelvin.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        break;
    }
    case ROLE_OUTLET:
        if (gas) pl_fmt_ui(a, sizeof a, "%.6g Pa absolute", sim_pabs(&P.s, b->p));
        row_num(x, &y, w, hints, "out-p", "Pressure", gas ? "Static, gauge: 0 means the operating pressure. What flows back in comes in still."
                                                       : "Static, gauge: 0 means the surroundings. What flows back in comes in still.", &b->p, -1e9, 1e9, "Pa", gas ? a : NULL);
        if (heat) row_num(x, &y, w, hints, "out-T", "Backflow temperature", "Of anything that flows back in through it.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        if (gas) {
            /* a transient run's pressure waves leave instead of bouncing back off a fixed pressure (waveTransmissive) */
            Rectangle nr = prop_row(x, &y, w, "Non-reflecting", hints ? "Pressure waves leave through it instead of bouncing back in. For transient runs; the pressure "
                                                                        "tends to the one above." : NULL, 24);
            bool steady = P.s.flow == FLOW_STEADY, on = b->nonRefl && !steady;
            ui_disable(steady);
            if (ui_switch(ui_id("out-nonrefl"), nr.x, nr.y + 2, &on)) { b->nonRefl = on; save(); }
            ui_disable(false);
            if (steady) {
                ui_tooltip((Rectangle){ nr.x, nr.y, 60, 24 }, "A steady run has no waves: this is for transient runs");
                ui_text_clip(F_REG, 12.5f, "Transient runs only", nr.x + 60, nr.y + 4, nr.width - 60, TH.text3);
            }
        }
        break;
    case ROLE_OPENING: {
        /* surroundings at rest (a window), or the free stream far from a body (freestream conditions). Not with heat on an
           incompressible setup, whose openings are the buoyant solvers' prghTotalPressure */
        bool can = gas || !heat;
        if (can) {
            Rectangle kr = prop_row(x, &y, w, "Opening to", hints ? "Surroundings: still air beyond it (a window, a room's open side). Free stream: the flow far from a "
                                                                    "body, coming in where it blows in and leaving elsewhere (a far field round a wing)." : NULL, 34);
            static const char *OK[] = { "Surroundings", "Free stream" };
            int fk = b->farField ? 1 : 0;
            if (ui_segmented(ui_id("op-kind"), (Rectangle){ kr.x, kr.y, fminf(300, kr.width), 34 }, OK, 2, &fk) && (fk == 1) != b->farField) {
                b->farField = fk == 1;
                if (b->farField && !vlen3(b->vec)) memcpy(b->vec, P.s.U0, sizeof b->vec);   /* the start is usually the free stream */
                save();
            }
        }
        if (can && b->farField) {
            a[0] = 0;
            if (gas) { double a0 = sim_sound_speed(&P.s, b->T); if (a0 > 0) pl_fmt_ui(a, sizeof a, "Free-stream Mach number %.3g", vlen3(b->vec) / a0); }
            row_vec(x, &y, w, hints, "op-u", "Free-stream velocity", "In the mesh's axes; usually the starting velocity too.", b->vec, "m/s");
            if (gas && a[0] && vlen3(b->vec) > 0) y += note(x, y + 2, w, a) - 4;
            row_num(x, &y, w, hints, "op-p", "Free-stream pressure", "Static, gauge.", &b->p, -1e9, 1e9, "Pa", NULL);
            if (heat) row_num(x, &y, w, hints, "op-T", "Free-stream temperature", "Static, in kelvin.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
            if (turb) {
                row_num(x, &y, w, hints, "op-ti", "Turbulence intensity", "0.1 % or less in free air.", &b->intensity, 0.001, 100, "%", NULL);
                row_num(x, &y, w, hints, "op-tl", "Mixing length", NULL, &b->length, 1e-9, 1e6, "m", NULL);
            }
            break;
        }
        row_num(x, &y, w, hints, "op-p", "Surrounding pressure", "Total, gauge. The flow goes in or out through it as it likes (a window, the open sides of a room).", &b->p, -1e9, 1e9, "Pa", NULL);
        if (heat) row_num(x, &y, w, hints, "op-T", "Surrounding temperature", gas ? "Total: of the gas at rest beyond it." : "Of the fluid that comes in through it.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        break;
    }
    case ROLE_SYMMETRY:
        y += ui_text_wrap(F_REG, 13, "Nothing to set: the flow slides along it and nothing crosses it (a mirror plane, or the far sides of a wind tunnel).",
                          (Rectangle){ x, y + 4, w, 200 }, 19, TH.text3, true) + 14;
        break;
    default: {
        Rectangle cr = prop_row(x, &y, w, "Wall", hints ? "No slip: the fluid sticks to it. Slip: it slides along without friction. Moving: a belt, a lid, a road under a car." : NULL, 34);
        int k = b->wall;
        if (ui_segmented(ui_id("wall-kind"), (Rectangle){ cr.x, cr.y, fminf(300, cr.width), 34 }, (const char **)WALL_NAME, WL__N, &k) && k != b->wall) { b->wall = k; save(); }
        if (b->wall == WL_MOVING) row_vec(x, &y, w, hints, "wall-u", "Wall velocity", "Along the wall.", b->wallU, "m/s");
        if (heat) {
            static const char *const heatHint[HT__N] = {
                "No heat crosses it: an insulated wall.",
                "The wall is held at a temperature (a heater with a thermostat, ice).",
                "Heat goes in at this rate per square metre, evenly: negative cools.",
                "Heat goes in at this total rate, spread over the wall: negative cools.",
                "Heat passes to or from surroundings at a temperature through a coefficient h (outside air: 5 to 25 W/(m²·K))." };
            Rectangle hr = prop_row(x, &y, w, "Heat", hints ? heatHint[b->heat] : NULL, 34);
            int hk = b->heat;
            if (ui_select(ui_id("wall-heat"), (Rectangle){ hr.x, hr.y, fminf(240, hr.width), 34 }, HEAT_NAME, HT__N, &hk) && hk != b->heat) { b->heat = hk; save(); }
            if (b->heat == HT_FIXED) row_num(x, &y, w, hints, "wall-T", "Temperature", NULL, &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
            else if (b->heat == HT_FLUX) {
                if (b->area > 0) pl_fmt_ui(a, sizeof a, "%.4g W over its %.3g m²", b->q * b->area, b->area); else a[0] = 0;
                row_num(x, &y, w, hints, "wall-q", "Heat flux", "Into the fluid.", &b->q, -1e9, 1e9, "W/m²", a);
            } else if (b->heat == HT_POWER) {
                if (b->area > 0) pl_fmt_ui(a, sizeof a, "%.4g W/m² over its %.3g m²", b->Q / b->area, b->area); else a[0] = 0;
                row_num(x, &y, w, hints, "wall-Q", "Heat rate", "Into the fluid, the whole wall.", &b->Q, -1e12, 1e12, "W", a);
            } else if (b->heat == HT_CONVECT) {
                row_num(x, &y, w, hints, "wall-h", "Heat transfer coefficient", NULL, &b->h, 1e-6, 1e9, "W/(m²·K)", NULL);
                row_num(x, &y, w, hints, "wall-Ta", "Surrounding temperature", NULL, &b->Ta, 1, 1e4, "K", celsius(a, sizeof a, b->Ta));
            }
        }
        if (b->wall != WL_SLIP && turb && strcmp(b->meshType, "wall") && b->fromStl) {
            char t[200]; pl_fmt_ui(t, sizeof t, "In the mesh this is not a wall (its type is %s), so it gets no wall functions. Make it a Wall in the STL mesher and mesh again.", b->meshType);
            y += ui_text_wrap(F_REG, 12.5f, t, (Rectangle){ x, y + 4, w, 200 }, 18, TH.warn, true) + 12;
        }
        break;
    }
    }
    ui_pop_id();
    return y - y0;
}

/* the open case's setup, for the Run page's reports (plan step 6); NULL without a simulation.ini */
const SimSetup *setup_current(Case *c)
{
    char ini[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini);
    if (!pl_is_file(ini)) return NULL;
    sync(c);
    return &P.s;
}

/* ============================================================================ the walls' y+ after a run (plan step 5) */
int setup_yplus_advice(Case *c, double avg, double max, char *out, size_t cap)
{
    sync(c);
    out[0] = 0;
    if (P.s.turb == TURB_LAMINAR || !(avg > 0)) return -1;
    int wt = sim_wall_treatment(&P.s);
    bool canFn = !sim_wall_why_not(&P.s, WT_FUNCTIONS), canRes = !sim_wall_why_not(&P.s, WT_RESOLVED);
    if (wt == WT_FUNCTIONS) {
        if (avg >= 30 && avg <= 300) { pl_fmt_ui(out, cap, "Walls' y+ averages %.3g: in the log layer (30 to 300), as wall functions want.", avg); return 0; }
        if (avg < 30) {
            pl_fmt_ui(out, cap, "Walls' y+ averages %.3g: too fine for wall functions (30 to 300), whose first cell must sit in the log layer. %s"
                      "make the first cell about %.2g times thicker (Physics > Setup, Models: the y+ helper sends the layers to the STL mesher).", avg,
                      canRes ? "Pick Resolved walls (Physics > Setup, Models: Walls), or " : "Pick k-omega SST to resolve the wall, or ", 30 / avg * 1.5);
            return 1;
        }
        pl_fmt_ui(out, cap, "Walls' y+ averages %.3g: too coarse even for wall functions (30 to 300). Make the first cell about %.2g times thinner "
                  "(Physics > Setup, Models: the y+ helper sends the layers to the STL mesher).", avg, avg / 100);
        return 1;
    }
    if (avg <= 2 && max <= 5) { pl_fmt_ui(out, cap, "Walls' y+ averages %.3g (at most %.3g): the viscous sublayer is resolved.", avg, max); return 0; }
    pl_fmt_ui(out, cap, "Walls' y+ averages %.3g (at most %.3g): too coarse to resolve the wall, which wants about 1. Make the first cell about %.2g times thinner "
              "(Physics > Setup, Models: the y+ helper sends the layers to the STL mesher)%s.", avg, max, avg,
              canFn && avg >= 30 ? ", or pick Wall functions: this y+ suits them" : canFn ? ", or pick Wall functions and coarsen it to y+ 30 or more" : "");
    return 1;
}

/* ============================================================================ writing */
/* the case files are out of date: not written, or older than the setup or the mesh (the step bar's Physics step) */
static bool files_stale(Case *c)
{
    char ini[PL_PATH], u[PL_PATH], faces[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(u, sizeof u, c->s.path, "0/U");
    pl_join(faces, sizeof faces, c->s.path, "constant/polyMesh/faces");
    double uMt = mtime_ns(u), simMt = mtime_ns(ini);
    return !uMt || !simMt || simMt > uMt || mtime_ns(faces) > uMt;
}

/* the mesh's patch types follow the roles chosen here, for the patches not from the STL (an imported mesh's groups, which
   the converters make plain patches): a wall is a wall patch (wall functions, wall distance), the rest plain patches.
   Constraint types (empty, symmetryPlane, cyclic ...) stay. The STL's own follow its types when it is meshed. */
static int match_patch_types(Case *c)
{
    if (c->patchSource != 1) return 0;
    const char *rel = "constant/polyMesh/boundary";
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, rel);
    FDFile *b = fd_load(p);
    if (!b) return 0;
    int n = 0;
    for (int i = 0; i < P.s.nB; i++) {
        SimBoundary *sb = &P.s.b[i];
        if (sb->fromStl || sim_constraint(sb->meshType)) continue;
        char key[128], cur[48] = "";
        snprintf(key, sizeof key, "%s/type", sb->name);
        if (!fd_get(b, key, cur, sizeof cur)) continue;
        const char *want = sb->role == ROLE_WALL ? "wall" : "patch";
        if (!strcmp(cur, want) || (strcmp(cur, "wall") && strcmp(cur, "patch"))) continue;   /* mappedWall ... stay */
        if (fd_put(b, key, want)) { pl_strlcpy(sb->meshType, want, sizeof sb->meshType); n++; }
    }
    if (n && !fd_save(b)) n = 0;
    fd_free(b);
    if (n) case_drop_file(c, rel);
    return n;
}

static bool write_files(Case *c, bool thenRun)
{
    if (case_dirty_count(c) > 0) { ui_toast(TOAST_ERR, "Save or undo your unsaved edits first: Write case files rewrites the files they are in"); return false; }
    if (A.cj->active || app_job_running()) { ui_toast(TOAST_ERR, "Wait for the case's job to finish first"); return false; }
    if (A.set.history) hist_commit(c->s.path, "Before the simulation setup was written");
    /* simulation.ini first: an untouched setup (all defaults) has none yet, and the case files are up to date only when
       they are newer than it */
    if (!sim_save(c->s.path, &P.s)) { ui_toast(TOAST_ERR, "Could not write simulation.ini"); return false; }
    int nt = match_patch_types(c);
    char err[300];
    bool ok = sim_write(c->s.path, &P.s, err, sizeof err);
    for (int i = 0; SIM_FILES[i]; i++) case_drop_file(c, SIM_FILES[i]);
    case_rescan(c);
    if (!ok) { ui_toast(TOAST_ERR, "%s", err); return false; }
    if (A.set.history) { char m[200]; snprintf(m, sizeof m, "Simulation setup: %s, %s", sim_app(&P.s), TURB_NAME[P.s.turb]); hist_commit(c->s.path, m); history_invalidate(); }
    const char *f[SIM_MAX_F]; int nf = sim_fields(&P.s, f);
    char list[96] = "";
    for (int i = 0; i < nf; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? " " : "", f[i]);
    ui_toast(TOAST_OK, "Wrote 0/ (%s), constant/ and system/ for %s%s%s", list, sim_app(&P.s), c->s.meshed ? "" : ". Generate the mesh before running",
             nt ? TextFormat("; %d patch type%s in the mesh set to match", nt, nt == 1 ? "" : "s") : "");
    /* hand edits to the files: kept, or replaced because the setup changed the same setting */
    int nk = 0, nr = 0;
    for (const char *q = sim_write_kept(); *q; q++) nk += *q == '\n';
    for (const char *q = sim_write_replaced(); *q; q++) nr += *q == '\n';
    if (nr) ui_toast(TOAST_WARN, "Replaced %d hand edit%s where the setup changed the same setting: %.*s%s", nr, nr == 1 ? "" : "s",
                     (int)strcspn(sim_write_replaced(), "\n"), sim_write_replaced(), nr > 1 ? " ..." : "");
    else if (nk) ui_toast(TOAST_INFO, "Kept %d hand edit%s in the case files (%.*s%s)", nk, nk == 1 ? "" : "s",
                          (int)strcspn(sim_write_kept(), "\n"), sim_write_kept(), nk > 1 ? " ..." : "");
    if (thenRun) app_go(PG_RUN);
    return true;
}

static bool wf_imported(Case *c);

/* the case files brought up to date with the setup before anything runs them (user request, 2026-10-26: a boundary
   changed on the Mesh tab ran with the old 0/U). Only for a case the setup owns: made from an STL, or with case files
   it wrote before; once there is a mesh (its patches are the boundaries); never with unsaved edits in the way or a job going. */
bool setup_refresh_files(Case *c)
{
    if (!c || !c->s.meshed || c->s.nRegions > 0 || A.cj->active || app_job_running() || case_dirty_count(c) > 0) return false;
    char ini[PL_PATH], msh[PL_PATH], u[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(msh, sizeof msh, c->s.path, "mesher/mesher.ini");
    pl_join(u, sizeof u, c->s.path, "0/U");
    if (!pl_is_file(msh) && !(wf_imported(c) && pl_is_file(ini))) {
        if (!pl_is_file(ini)) return false;
        char *t = pl_read_head(u, 1200);
        bool ours = t && strstr(t, "Q-Continuum's simulation setup");
        free(t);
        if (!ours) return false;
    }
    sync(c);
    refresh(c);                                            /* sync runs once a frame: a mesh just made may be newer than it */
    if (!P.s.nB || !files_stale(c)) return false;
    /* a write that failed (and said why) isn't tried again until the setup or the mesh changes */
    static char failedCase[PL_PATH]; static double failedAt;
    char faces[PL_PATH]; pl_join(faces, sizeof faces, c->s.path, "constant/polyMesh/faces");
    double at = mtime_ns(ini) + mtime_ns(faces);
    if (!strcmp(failedCase, c->s.path) && at == failedAt) return false;
    if (write_files(c, false)) { failedCase[0] = 0; return true; }
    pl_strlcpy(failedCase, c->s.path, sizeof failedCase); failedAt = at;
    return false;
}

/* a mesh just imported (SALOME, Gmsh: user request, 2026-11-02): 0/ written at once, one boundary condition per patch with
   the role its name suggests, so Boundary conditions has fields to show without a trip to Physics > Setup. A case the setup
   already owns is brought up to date as before; one with field files of its own keeps them. */
bool setup_after_import(Case *c)
{
    if (!c || !wf_imported(c) || !c->s.meshed || c->s.nRegions > 0) return false;
    char ini[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini);
    if (pl_is_file(ini)) return setup_refresh_files(c);
    if (c->fields.n > 0 || case_dirty_count(c) > 0 || A.cj->active) return false;
    sync(c);
    refresh(c);                                            /* sync runs once a frame: this one's may be from before the mesh */
    return P.s.nB > 0 && write_files(c, false);
}

/* Boundary conditions with no field files: can the setup write them (a case it owns, or one it can start owning: any mesh
   with patches and no fields), and do it */
bool setup_can_write_fields(Case *c)
{
    return c && c->s.meshed && c->s.nRegions == 0 && !c->s.prog && c->fields.n == 0 && c->nPatches > 0 && c->patchSource == 1;
}

bool setup_write_fields(Case *c)
{
    if (!setup_can_write_fields(c)) return false;
    sync(c);
    refresh(c);
    if (!P.s.nB) { ui_toast(TOAST_ERR, "The mesh has no patches to write boundary conditions for"); return false; }
    return write_files(c, false);
}

/* ============================================================================ the steps, STL to run (user request, 2026-10-26)
 * One bar over the STL mesher, Physics > Setup and Run: each step says whether it is done, out of date (the mesher's
 * settings changed after the mesh, the mesh or the setup after the case files) or next, opens its page when clicked,
 * and the button on the right does the next thing. The times of the files say it all: mesher/mesher.ini and
 * simulation.ini are only rewritten when they change. */
enum { WF_TODO = 0, WF_DONE, WF_STALE, WF_BUSY };
enum { WF_STL = 0, WF_BOUNDARIES, WF_MESH, WF_PHYSICS, WF_RUN, WF__N };

typedef struct { int state; char sub[120]; } WfStep;

static bool has_patch(const Case *c, const char *name)
{
    for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].name, name) && c->patches[i].nFaces > 0) return true;
    return false;
}

static bool wf_imported(Case *c)
{
    char a[PL_PATH], b[PL_PATH];
    pl_join(a, sizeof a, c->s.path, "mesher/mesher.ini");
    pl_join(b, sizeof b, c->s.path, "salome/imported.txt");
    return !pl_is_file(a) && pl_is_file(b);
}

static void wf_status(Case *c, WfStep st[WF__N])
{
    memset(st, 0, sizeof(WfStep) * WF__N);
    char ini[PL_PATH], sim[PL_PATH], faces[PL_PATH], u[PL_PATH];
    pl_join(ini, sizeof ini, c->s.path, "mesher/mesher.ini");
    sim_ini_path(c->s.path, sim, sizeof sim);
    pl_join(faces, sizeof faces, c->s.path, "constant/polyMesh/faces");
    pl_join(u, sizeof u, c->s.path, "0/U");
    double iniMt = mtime_ns(ini), simMt = mtime_ns(sim), meshMt = mtime_ns(faces), uMt = mtime_ns(u);
    bool here = A.cj->active && !strcmp(A.cj->casePath, c->s.path), meshing = here && !strcmp(A.cj->title, "Mesh");
    bool imported = wf_imported(c);                        /* a mesh imported from SALOME or Gmsh, not one made from an STL */
    /* 1 the STL (or the imported mesh file) */
    int nStl = 0, nIn = 0, nOut = 0, nWall = 0;
    for (int i = 0; i < P.s.nB; i++) {
        const SimBoundary *b = &P.s.b[i];
        if (b->fromStl) nStl++;
        if (sim_constraint(b->meshType)) continue;
        if (b->role == ROLE_INLET) nIn++;
        else if (b->role == ROLE_OUTLET || b->role == ROLE_OPENING) nOut++;
        else if (b->role == ROLE_WALL) nWall++;
    }
    if (imported) { st[WF_STL].state = WF_DONE; pl_fmt_ui(st[WF_STL].sub, sizeof st[WF_STL].sub, "%d patch%s", P.s.nB, P.s.nB == 1 ? "" : "es"); nStl = P.s.nB; }
    else if (nStl) { st[WF_STL].state = WF_DONE; pl_fmt_ui(st[WF_STL].sub, sizeof st[WF_STL].sub, "%d boundar%s%s", nStl, nStl == 1 ? "y" : "ies", P.external ? ", wind tunnel" : ""); }
    else pl_strlcpy(st[WF_STL].sub, "Choose the part", sizeof st[WF_STL].sub);
    /* 2 the boundaries: something brings the flow in and lets it out, and nothing the checks complain about */
    char warn[2000];
    int nw = sim_check(&P.s, warn, sizeof warn);
    char *nl = strchr(warn, '\n'); if (nl) *nl = 0;
    if (!nStl) pl_strlcpy(st[WF_BOUNDARIES].sub, "After the STL", sizeof st[WF_BOUNDARIES].sub);
    else if (nw) pl_strlcpy(st[WF_BOUNDARIES].sub, warn, sizeof st[WF_BOUNDARIES].sub);
    else if (!nIn && !nOut) pl_strlcpy(st[WF_BOUNDARIES].sub, "Name the inlets and outlets", sizeof st[WF_BOUNDARIES].sub);
    else {
        st[WF_BOUNDARIES].state = WF_DONE;
        pl_fmt_ui(st[WF_BOUNDARIES].sub, sizeof st[WF_BOUNDARIES].sub, "%d in, %d out, %d wall%s", nIn, nOut, nWall, nWall == 1 ? "" : "s");
    }
    /* 3 the mesh: made from the mesher's settings as they are (a type change from inlet to outlet is the same mesh); a
       mesh from before there was a signature: newer than the settings */
    int cur = meshMt ? stlmesh_mesh_current(c) : -1;
    char sig[PL_PATH], cml[PL_PATH];
    pl_join(sig, sizeof sig, c->s.path, imported ? "salome/imported.txt" : "mesher/mesh.sig");
    pl_join(cml, sizeof cml, c->s.path, "log.checkMesh");
    double sigMt = mtime_ns(sig);
    bool unfinished = sigMt > 0 && mtime_ns(cml) < sigMt;     /* Generate ends with checkMesh: a failed one never got there */
    if (unfinished && !imported && !mtime_ns(cml) && meshMt > sigMt) {
        /* no log.checkMesh at all: a start over before the fix put the mesh's logs in the trash. A mesh made after the
           settings that has every boundary of the STL got there (a failed snappy leaves only blockMesh's patches) */
        bool all = nStl > 0;
        for (int i = 0; i < P.s.nB && all; i++) if (P.s.b[i].fromStl && !has_patch(c, P.s.b[i].name)) all = false;
        if (all) unfinished = false;
    }
    if (meshing) { st[WF_MESH].state = WF_BUSY; pl_fmt_ui(st[WF_MESH].sub, sizeof st[WF_MESH].sub, "%s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label); }
    else if (!meshMt) pl_strlcpy(st[WF_MESH].sub, imported ? "Not imported yet" : "Not generated yet", sizeof st[WF_MESH].sub);
    else if (!imported && !P.external && has_patch(c, "background")) {
        /* snappy keeps the cells on the wrong side of a gap: blockMesh's box is still there, around the part */
        st[WF_MESH].state = WF_STALE; pl_strlcpy(st[WF_MESH].sub, "Leaked: the STL has a gap", sizeof st[WF_MESH].sub);
    }
    else if (unfinished) { st[WF_MESH].state = WF_STALE; pl_strlcpy(st[WF_MESH].sub, imported ? "Not finished: import again" : "Not finished: generate again", sizeof st[WF_MESH].sub); }
    else if (cur == 0 || (cur < 0 && iniMt > meshMt)) { st[WF_MESH].state = WF_STALE; pl_strlcpy(st[WF_MESH].sub, "Settings changed: generate again", sizeof st[WF_MESH].sub); }
    else {
        st[WF_MESH].state = WF_DONE;
        int lc = logset_find(&A.logs, "log.checkMesh");
        bool failed = lc >= 0 && A.logs.logs[lc].meshFailed;
        char n[32];
        if (c->s.nCells >= 1000000) snprintf(n, sizeof n, "%.2f million", c->s.nCells / 1e6);
        else snprintf(n, sizeof n, "%ld", c->s.nCells);
        if (c->s.nCells > 0) pl_fmt_ui(st[WF_MESH].sub, sizeof st[WF_MESH].sub, "%s cells%s", n, failed ? ", checks failed" : "");
        else pl_strlcpy(st[WF_MESH].sub, failed ? "checkMesh: checks failed" : "Generated", sizeof st[WF_MESH].sub);
    }
    /* 4 the case files: newer than the setup and the mesh */
    if (!uMt || !simMt) pl_strlcpy(st[WF_PHYSICS].sub, "Case files not written", sizeof st[WF_PHYSICS].sub);
    else if (simMt > uMt) { st[WF_PHYSICS].state = WF_STALE; pl_strlcpy(st[WF_PHYSICS].sub, "Setup changed: write again", sizeof st[WF_PHYSICS].sub); }
    else if (meshMt > uMt) { st[WF_PHYSICS].state = WF_STALE; pl_strlcpy(st[WF_PHYSICS].sub, "New mesh: write again", sizeof st[WF_PHYSICS].sub); }
    else { st[WF_PHYSICS].state = WF_DONE; pl_fmt_ui(st[WF_PHYSICS].sub, sizeof st[WF_PHYSICS].sub, "%s, %s", sim_app(&P.s), TURB_NAME[P.s.turb]); }
    /* 5 the run */
    if (here && !meshing) { st[WF_RUN].state = WF_BUSY; pl_fmt_ui(st[WF_RUN].sub, sizeof st[WF_RUN].sub, "%s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label); }
    else if (c->s.nTimes > 0 || c->s.procTimes > 0) {
        /* results from before the mesh or the case files changed are out of date: the latest time folder says when */
        bool proc = c->s.procTimes > 0 && (!c->s.nTimes || atof(c->s.procLatest) > atof(c->s.latest));
        char lt[PL_PATH], rel[64];
        snprintf(rel, sizeof rel, proc ? "processor0/%s" : "%s", proc ? c->s.procLatest : c->s.latest);
        pl_join(lt, sizeof lt, c->s.path, rel);
        double rMt = mtime_ns(lt);
        if (rMt && (rMt < uMt || rMt < meshMt)) { st[WF_RUN].state = WF_STALE; pl_strlcpy(st[WF_RUN].sub, "Older than the setup: run again", sizeof st[WF_RUN].sub); }
        else { st[WF_RUN].state = WF_DONE; pl_fmt_ui(st[WF_RUN].sub, sizeof st[WF_RUN].sub, "Results to %s%s", proc ? c->s.procLatest : c->s.latest, proc ? " (parallel)" : ""); }
    }
    else pl_strlcpy(st[WF_RUN].sub, "Not run yet", sizeof st[WF_RUN].sub);
}

static void wf_open(Case *c, int k)
{
    if (k <= WF_MESH) {
        A.meshTab = wf_imported(c) ? 1 : 3; app_go(PG_MESH);
        if (!wf_imported(c)) stlmesh_focus(c, k == WF_STL ? 0 : k == WF_BOUNDARIES ? 2 : 4);   /* that step of the mesher's flow */
    }
    else if (k == WF_PHYSICS) { A.physicsTab = 2; app_go(PG_PHYSICS); }
    else app_go(PG_RUN);
}

/* the next thing to do (the first step not done), as the button says it */
static int wf_next(const WfStep st[WF__N], bool imported, const char **label, const char **icon)
{
    static const char *const L[WF__N] = { "Choose an STL", "Set the boundaries", "Generate mesh", "Write case files", "Run simulation" };
    static const char *const LI[WF__N] = { "Import a mesh", "Set the boundaries", "Import the mesh", "Write case files", "Run simulation" };
    static const char *const I[WF__N] = { ICON_FOLDER_OPEN, ICON_LAYERS, ICON_GRID_3X3, ICON_FILE_TEXT, ICON_PLAY };
    for (int k = 0; k < WF__N; k++) {
        if (st[k].state == WF_BUSY) { *label = NULL; return -1; }
        if (st[k].state != WF_DONE) { *label = imported ? LI[k] : L[k]; *icon = I[k]; return k; }
    }
    *label = "Open in ParaView"; *icon = ICON_EYE;
    return WF__N;
}

static void wf_do(Case *c, int k)
{
    bool imported = wf_imported(c);
    if (imported && (k == WF_STL || k == WF_MESH)) wf_open(c, k);   /* the SALOME tab: choose the file, import it */
    else if (k == WF_STL) { A.meshTab = 3; app_go(PG_MESH); stlmesh_choose(); }
    else if (k == WF_BOUNDARIES) { if (imported) { A.physicsTab = 2; P.step = SS_BOUNDARIES; app_go(PG_PHYSICS); } else wf_open(c, k); }
    else if (k == WF_MESH) stlmesh_generate(c);              /* here: the bar shows the steps, the Run page the log */
    else if (k == WF_PHYSICS) write_files(c, false);
    else if (k == WF_RUN) { A.runPending = true; app_go(PG_RUN); }
    else app_open_paraview();
}

float stl_workflow(Case *c, float x, float y, float w)
{
    if (!c) return 0;
    char ini[PL_PATH]; pl_join(ini, sizeof ini, c->s.path, "mesher/mesher.ini");
    bool imported = wf_imported(c);
    if (!pl_is_file(ini) && !imported) return 0;            /* not a case made from an STL or an imported mesh */
    sync(c);
    WfStep st[WF__N];
    wf_status(c, st);
    const char *NAME[WF__N] = { imported ? "Mesh file" : "STL", "Boundaries", "Mesh", "Physics", "Run" };
    const char *nlabel = NULL, *nicon = NULL;
    int next = wf_next(st, imported, &nlabel, &nicon);
    float y0 = y;
    bool narrow = w < 640;                                  /* the Run page's column: one step a line */
    float bw = nlabel ? ui_button_width(nicon, nlabel) : 0;
    /* each step as wide as its text needs, the room left shared out; short of room (a large UI scale), the lines under
       the names wrap rather than lose their ends */
    float sw[WF__N], sx[WF__N], need = 0, avail = w - bw - 16, sh = narrow ? 40 : 54;
    for (int k = 0; k < WF__N; k++) {
        sw[k] = 46 + 14 + fmaxf(ui_text_w(F_SEMI, 14, ui_cap(NAME[k])), ui_text_w(F_REG, 12.5f, st[k].sub));
        need += sw[k];
    }
    for (int k = 0; k < WF__N; k++) {
        sw[k] = need <= avail ? sw[k] + (avail - need) / WF__N : avail * sw[k] / need;
        sx[k] = k ? sx[k - 1] + sw[k - 1] : x;
        if (!narrow && need > avail)
            sh = fmaxf(sh, 34 + ui_text_wrap(F_REG, 12.5f, st[k].sub, (Rectangle){ 0, 0, sw[k] - 6 - 46, 300 }, 16, TH.text3, false));
    }
    for (int k = 0; k < WF__N; k++) {
        Rectangle r = narrow ? (Rectangle){ x, y + k * sh, w, sh - 4 } : (Rectangle){ sx[k], y, sw[k] - 6, sh };
        ui_push_id(ui_idn(ui_id("wf"), k));
        if (ui_row(ui_id("step"), r, k == next)) wf_open(c, k);
        ui_tooltip(r, st[k].sub);
        Color c0 = st[k].state == WF_DONE ? TH.ok : st[k].state == WF_STALE ? TH.warn : k == next || st[k].state == WF_BUSY ? TH.accent : TH.text3;
        float cx = r.x + 20, cy = r.y + r.height / 2;
        if (st[k].state == WF_BUSY) ui_spinner(cx, cy, 8, c0);
        else if (st[k].state == WF_DONE) ui_icon(ICON_CIRCLE_CHECK, cx, cy, 20, c0);
        else if (st[k].state == WF_STALE) ui_icon(ICON_TRIANGLE_ALERT, cx, cy, 18, c0);
        else {
            ui_rect_lines((Rectangle){ cx - 10, cy - 10, 20, 20 }, 10, 1.5f, c0);
            char n[4]; snprintf(n, sizeof n, "%d", k + 1);
            ui_text(F_SEMI, 12, n, cx - ui_text_w(F_SEMI, 12, n) / 2, cy - 8, c0);
        }
        float tx = r.x + 40, tw = r.width - 46;
        if (narrow) {
            ui_text_clip(F_SEMI, 14, ui_cap(NAME[k]), tx, cy - 9, 96, k == next ? TH.text : TH.text2);
            ui_text_clip(F_REG, 12.5f, st[k].sub, tx + 100, cy - 8, tw - 100, st[k].state == WF_STALE ? TH.warn : TH.text3);
        } else {
            float ty = r.y + 9;
            ui_text_clip(F_SEMI, 14, ui_cap(NAME[k]), tx, ty, tw, k == next ? TH.text : TH.text2);
            Color sc = st[k].state == WF_STALE ? TH.warn : TH.text3;
            if (sh > 54) ui_text_wrap(F_REG, 12.5f, st[k].sub, (Rectangle){ tx, ty + 20, tw, r.height - 29 }, 16, sc, true);
            else ui_text_clip(F_REG, 12.5f, st[k].sub, tx, r.y + 29, tw, sc);
        }
        if (!narrow && k < WF__N - 1) ui_icon(ICON_CHEVRON_RIGHT, r.x + r.width + 3, cy, 12, TH.text3);
        ui_pop_id();
    }
    y += narrow ? WF__N * sh + 6 : sh + 4;
    if (nlabel) {
        Rectangle b = narrow ? (Rectangle){ x, y, w, 38 } : (Rectangle){ x + w - bw, y0 + (sh - 40) / 2, bw, 40 };
        ui_disable(A.cj->active && next != WF__N && next != WF_BOUNDARIES);
        if (ui_button(ui_id("wf-next"), b, nicon, nlabel, BTN_PRIMARY)) wf_do(c, next);
        ui_disable(false);
        if (narrow) y += 46;
    }
    ui_divider(x, y + 8, w);
    return y + 24 - y0;
}

/* ============================================================================ the page (user plan step 2, 2026-10-27)
 * A new setup first asks what is being simulated: a template card sets the models and their values. Then a list of steps
 * on the left (Models, Materials, Boundaries, Starting values, Reports, Solver, Check), each saying whether it is done,
 * needs a look or is wrong, with one step open on the right. A step shows only the rows its models need; expert rows
 * sit behind its More. */
enum { SS_OK = 0, SS_ATTN, SS_ERR };

/* a note under a step's head */
static float note(float x, float y, float w, const char *t)
{
    return ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y, w, 200 }, 19, TH.text3, true) + 12;
}

typedef struct { int state; char sub[160]; } SsStatus;

static const char *const SS_TITLE[SS__N] = { "Models", "Materials", "Boundaries", "Starting values", "Reports", "Solver", "Check" };

/* a whole number in a row (correctors), saved as it changes */
static bool row_count(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, int *v, int lo, int hi)
{
    double d = *v;
    if (!row_num(x, y, w, hints, id, label, hint, &d, lo, hi, NULL, NULL)) return false;
    *v = (int)lround(d);
    save();
    return true;
}

/* the step's More: the expert rows below it shown or not */
static float more_toggle(int step, float x, float y)
{
    const char *lab = P.more[step] ? "Fewer settings" : "More settings";
    const char *ic = P.more[step] ? ICON_CHEVRON_UP : ICON_CHEVRON_DOWN;
    if (ui_button(ui_idn(ui_id("su-more"), step), (Rectangle){ x, y + 6, ui_button_width(ic, lab), 32 }, ic, lab, BTN_GHOST)) P.more[step] = !P.more[step];
    return 46;
}

static bool two_d(Case *c) { return sim_two_d(&P.s) || stlmesh_two_d(c); }

/* the Reynolds number from the inflow speed and the part's size; 0: unknown */
static double reynolds(void)
{
    bool driven = P.s.U0[0] || P.s.U0[1] || P.s.U0[2];      /* an inlet, an opening, a moving wall or a start in motion */
    for (int i = 0; i < P.s.nB; i++) {
        const SimBoundary *b = &P.s.b[i];
        if (!sim_constraint(b->meshType) && (b->role == ROLE_INLET || (b->role == ROLE_WALL && b->wall == WL_MOVING))) driven = true;
    }
    return driven && P.size > 0 && P.s.nu > 0 ? sim_ref_speed(&P.s) * P.size / P.s.nu : 0;
}

/* what the checks (sim_check) find in one step, a line each, and for Models a laminar model on a turbulent flow (or the
   other way); returns the count. SA_* are the steps' numbers. */
static int step_issues(int step, char *out, size_t cap)
{
    char all[2000];
    int n = sim_check(&P.s, all, sizeof all), k = 0;
    out[0] = 0;
    char *line = all;
    for (int i = 0; i < n && line; i++) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (sim_check_area(i) == step) snprintf(out + strlen(out), cap - strlen(out), "%s%s", k++ ? "\n" : "", line);
        line = nl ? nl + 1 : NULL;
    }
    double Re = reynolds();
    if (step == SS_MODELS && Re > 1e4 && P.s.turb == TURB_LAMINAR)
        snprintf(out + strlen(out), cap - strlen(out), "%sRe is about %.3g: the flow is turbulent, so a laminar model will not settle. Pick RANS.", k++ ? "\n" : "", Re);
    /* LES and DES: cells across the boundary layer, from the mesh's mean cell (or the STL mesher's surface cell) */
    int ap = sim_approach(P.s.turb);
    if (step == SS_MODELS && (ap == APPR_LES || ap == APPR_DES) && P.size > 0) {
        double d = 0, cell = 0, f; int n;
        sim_wall_spacing(&P.s, sim_ref_speed(&P.s), P.size, 1, NULL, &d, NULL);
        if (!stlmesh_wall_layers(A.cs, &f, &n, &cell) && cell <= 0) {
            double lo[3], hi[3];
            if (A.cs->s.nCells > 0 && mesh_bounds(A.cs->s.path, lo, hi)) {
                double V = 1; int k3 = 0;
                for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > 0) { V *= hi[k] - lo[k]; k3++; }
                if (two_d(A.cs) && k3 == 3) { double mn = 1e30; for (int k = 0; k < 3; k++) mn = fmin(mn, hi[k] - lo[k]); V /= mn; k3 = 2; }
                cell = pow(V / A.cs->s.nCells, 1.0 / k3);
            }
        }
        if (d > 0 && cell > 0 && d / cell < 10)
            snprintf(out + strlen(out), cap - strlen(out), "%sThe boundary layer is about %s thick and the cells near the walls about %s: %.2g across it. %s resolves "
                     "eddies the size of the layer, which wants 10 or more: refine near the walls%s.", k++ ? "\n" : "", fmt_len(d), fmt_len(cell), d / cell,
                     APPR_NAME[ap], ap == APPR_LES ? ", or use DES (RANS in the boundary layer)" : "");
    }
    double Ra = sim_rayleigh(&P.s, P.height);
    if (step == SS_MODELS && Ra > 1e9 && P.s.turb == TURB_LAMINAR)
        snprintf(out + strlen(out), cap - strlen(out), "%sRa is about %.3g: natural convection turns turbulent above about 1e9, so a laminar model will not settle. "
                 "Pick RANS (k-omega SST).", k++ ? "\n" : "", Ra);
    return k;
}

/* the step's problems, over its rows */
static float issues_callout(int step, float x, float y, float w)
{
    char t[1200];
    int n = step_issues(step, t, sizeof t);
    if (!n) return 0;
    bool soft = step == SS_MODELS || step == SS_SOLVER;
    const char *title = n == 1 ? "Check this" : "Check these";
    float h = callout_height(w, title, t);
    callout((Rectangle){ x, y, w, h }, soft ? TOAST_WARN : TOAST_ERR, ICON_TRIANGLE_ALERT, title, t);
    return h + 14;
}

static void ss_status(Case *c, SsStatus st[SS__N])
{
    memset(st, 0, sizeof(SsStatus) * SS__N);
    SimSetup *s = &P.s;
    /* Models */
    pl_fmt_ui(st[SS_MODELS].sub, sizeof st[0].sub, "%s %s%s%s%s", FLOW_NAME[s->flow], sim_approach_label(s, sim_approach(s->turb)),
              s->turb == TURB_LAMINAR ? "" : ", ", s->turb == TURB_LAMINAR ? "" : TURB_NAME[s->turb],
              sim_is_gas(s) ? ", ideal gas" : s->energy == EN_FORCED ? ", heat" : s->energy == EN_BUOYANT ? ", buoyancy" : "");
    /* Materials */
    if (sim_is_gas(s)) pl_fmt_ui(st[SS_MATERIALS].sub, sizeof st[0].sub, "%s at %.4g kPa, %.4g kg/m³", GAS_NAME[s->gas], s->pOp / 1000, s->rho);
    else pl_fmt_ui(st[SS_MATERIALS].sub, sizeof st[0].sub, "%s, %.4g kg/m³", FLUID_NAME[s->fluid], s->rho);
    /* Boundaries */
    int nIn = 0, nOut = 0, nWall = 0;
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (sim_constraint(b->meshType)) continue;
        if (b->role == ROLE_INLET) nIn++;
        else if (b->role == ROLE_OUTLET || b->role == ROLE_OPENING) nOut++;
        else if (b->role == ROLE_WALL) nWall++;
    }
    if (!s->nB) { st[SS_BOUNDARIES].state = SS_ATTN; pl_strlcpy(st[SS_BOUNDARIES].sub, "No boundaries yet", sizeof st[0].sub); }
    else pl_fmt_ui(st[SS_BOUNDARIES].sub, sizeof st[0].sub, "%d in, %d out, %d wall%s", nIn, nOut, nWall, nWall == 1 ? "" : "s");
    /* Starting values */
    double u0 = sqrt(s->U0[0] * s->U0[0] + s->U0[1] * s->U0[1] + s->U0[2] * s->U0[2]);
    if (u0 > 0) pl_fmt_ui(st[SS_START].sub, sizeof st[0].sub, "Moving at %.3g m/s", u0);
    else pl_strlcpy(st[SS_START].sub, "Still fluid", sizeof st[0].sub);
    if (s->energy != EN_OFF) pl_fmt_ui(st[SS_START].sub + strlen(st[SS_START].sub), sizeof st[0].sub - strlen(st[SS_START].sub), ", %.4g °C", s->T0 - 273.15);
    /* Reports */
    bool heatRep = s->energy != EN_OFF && s->heatReport;
    {
        char t[160] = "";
        if (s->nR) pl_fmt_ui(t, sizeof t, "%d report%s", s->nR, s->nR == 1 ? "" : "s");
        if (s->forces) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), "%sLift and drag", t[0] ? ", " : "");
        if (heatRep) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), "%swall heat", t[0] ? ", " : "");
        pl_strlcpy(st[SS_REPORTS].sub, t[0] ? t : "Wall y+ only", sizeof st[0].sub);
    }
    /* Solver */
    if (s->flow == FLOW_STEADY) pl_fmt_ui(st[SS_SOLVER].sub, sizeof st[0].sub, "%.6g iterations%s", s->iterations, s->resStop ? ", stops when converged" : "");
    else pl_fmt_ui(st[SS_SOLVER].sub, sizeof st[0].sub, "%s to %.4g s%s", ALG_NAME[s->algo], s->endTime, s->algo != ALG_PISO && s->adjustDt ? TextFormat(", Courant %.3g", s->maxCo) : "");
    /* a step's problems: its first one instead (Models and Solver: a look; the rest: wrong) */
    int bad = 0, worst = SS_OK;
    for (int k = 0; k < SS_CHECK; k++) {
        char t[1200];
        int n = step_issues(k, t, sizeof t);
        if (!n) continue;
        bad += n;
        char *nl = strchr(t, '\n'); if (nl) *nl = 0;
        st[k].state = k == SS_MODELS || k == SS_SOLVER ? SS_ATTN : SS_ERR;
        pl_strlcpy(st[k].sub, t, sizeof st[0].sub);
        if (st[k].state > worst) worst = st[k].state;
    }
    /* Check */
    if (bad) { st[SS_CHECK].state = worst; pl_fmt_ui(st[SS_CHECK].sub, sizeof st[0].sub, "%d thing%s to look at", bad, bad == 1 ? "" : "s"); }
    else if (files_stale(c)) { st[SS_CHECK].state = SS_ATTN; pl_strlcpy(st[SS_CHECK].sub, "Case files not written", sizeof st[0].sub); }
    else pl_strlcpy(st[SS_CHECK].sub, "Case files match", sizeof st[0].sub);
}

/* ---------------------------------------------------------------------------- What are you simulating? */
static const char *const TMPL_ICON[TMPL__N] = { ICON_WIND, ICON_GIT_FORK, ICON_WAVES, ICON_ORBIT, ICON_THERMOMETER, ICON_FLAME,
                                                ICON_ROCKET, ICON_DROPLETS, ICON_FAN, ICON_SLIDERS_HORIZONTAL };

static void pick_template(Case *c, int t)
{
    sim_template_apply(&P.s, t, P.size);
    if (t == TMPL_GAS) {
        /* high-speed gas: inlets still at a walking pace (the new boundaries' 1 m/s) come in at Mach 0.5; a body in a wind
           tunnel gets its lift and drag measured */
        for (int i = 0; i < P.s.nB; i++) {
            SimBoundary *b = &P.s.b[i];
            if (b->role == ROLE_INLET && b->inlet == IN_SPEED && b->speed < 0.3 * sim_sound_speed(&P.s, b->T)) { b->inlet = IN_MACH; b->mach = 0.5; }
        }
        if (P.external) { P.s.forces = true; P.s.forceAuto = true; }
        sim_settle(&P.s);
    }
    /* a body in a wind tunnel: the air starts at the free stream */
    if (P.external) { SimBoundary *in = sim_find(&P.s, "inlet"); if (in && in->role == ROLE_INLET) free_stream(c, sim_ref_speed(&P.s), P.s.U0); }
    save();
    P.picking = false; P.step = SS_MODELS;
    bool layers = t == TMPL_EXTERNAL || t == TMPL_INTERNAL || t == TMPL_SHEDDING || t == TMPL_HEAT || t == TMPL_NATCONV;
    gravity_auto(c);
    if (stlmesh_template_hints(c, layers)) ui_toast(TOAST_INFO, "%s: prism layers switched on in the STL mesher, for the %s layer", TMPL_NAME[t],
                                                    t == TMPL_HEAT || t == TMPL_NATCONV ? "thermal boundary" : "boundary");
    else ui_toast(TOAST_OK, "%s: %s", TMPL_NAME[t], sim_template_hint(t));
}

static float template_cards(Case *c, float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "What are you simulating?", "Pick the closest. It sets the models and their values, and the steps after it show only "
                                                         "what those models need. Every choice can be changed afterwards.");
    if (P.picking) {
        float bw = ui_button_width(ICON_X, "Keep the current setup");
        if (ui_button(ui_id("su-tmpl-cancel"), (Rectangle){ x, y, bw, 34 }, ICON_X, "Keep the current setup", BTN_GHOST)) P.picking = false;
        y += 46;
    }
    bool twoD = two_d(c);
    int suits = P.size > 0 ? (P.external ? TMPL_EXTERNAL : TMPL_INTERNAL) : -1;   /* what the STL mesher's flow says */
    int cols = (int)fmaxf(1, fminf(5, floorf((w + 14) / 270)));
    float cw = (w - 14 * (cols - 1)) / cols, ch = 0;
    for (int t = 0; t < TMPL__N; t++) {                     /* the tallest card's height, for even rows */
        float h = 58 + ui_text_wrap(F_REG, 13, sim_template_hint(t), (Rectangle){ 0, 0, cw - 32, 300 }, 19, TH.text3, false);
        const char *why = sim_template_why_not(&P.s, t, twoD);
        if (why) h += 8 + ui_text_wrap(F_REG, 12.5f, why, (Rectangle){ 0, 0, cw - 32, 300 }, 17, TH.text3, false);
        if (t == suits) h += 30;
        ch = fmaxf(ch, h + 14);
    }
    for (int t = 0; t < TMPL__N; t++) {
        Rectangle r = { x + (t % cols) * (cw + 14), y + (t / cols) * (ch + 14), cw, ch };
        const char *why = sim_template_why_not(&P.s, t, twoD);
        bool on = P.s.tmpl == t, hov = !why && ui_hovered(r);
        UiId id = ui_idn(ui_id("su-tmpl"), t);
        ui_disable(why != NULL);
        ui_register(id, r, TMPL_NAME[t]);
        ui_disable(false);
        ui_rect(r, 12, on ? ui_alpha(TH.accent, 0.14f) : hov ? TH.hover : TH.surface2);
        ui_rect_lines(r, 12, on ? 2 : 1, on ? TH.accent : TH.lineSoft);
        Color fg = why ? TH.text3 : TH.text;
        ui_icon(TMPL_ICON[t], r.x + 28, r.y + 30, 22, why ? TH.text3 : TH.accent);
        ui_text_clip(F_SEMI, 15, ui_cap(TMPL_NAME[t]), r.x + 50, r.y + 20, r.width - 66, fg);
        float ty = r.y + 50;
        ty += ui_text_wrap(F_REG, 13, sim_template_hint(t), (Rectangle){ r.x + 16, ty, r.width - 32, 300 }, 19, why ? ui_alpha(TH.text3, 0.75f) : TH.text2, true);
        if (why) ui_text_wrap(F_REG, 12.5f, why, (Rectangle){ r.x + 16, ty + 8, r.width - 32, 300 }, 17, TH.warn, true);
        if (t == suits) ui_badge(r.x + 16, r.y + r.height - 34, ICON_CIRCLE_CHECK, P.external ? "Suits your part: flow around it" : "Suits your part: flow through it",
                                 TH.ok, ui_alpha(TH.ok, 0.14f));
        if (on) ui_icon(ICON_CIRCLE_CHECK, r.x + r.width - 22, r.y + 22, 18, TH.accent);
        if (why) ui_tooltip(r, why);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (!why && ui_clicked(r)) pick_template(c, t);
    }
    y += ((TMPL__N + cols - 1) / cols) * (ch + 14);
    return y - y0;
}

/* ---------------------------------------------------------------------------- the walls (plan step 5)
 * The treatment, and from the inflow speed, the part's length and the fluid, the first cell for the y+ it wants, the
 * boundary layer's thickness and the prism layers that cover it; for an STL case what the mesher's layers give now, and a
 * button that sets them. */
static float wall_rows(Case *c, float x, float y, float w, bool hints)
{
    float y0 = y;
    SimSetup *s = &P.s;
    Rectangle cr = prop_row(x, &y, w, "Walls", hints ? "Wall functions: the first cell sits in the log layer (y+ 30 to 300), coarse and cheap. Resolved: "
                                                       "the first cell inside the viscous sublayer (y+ about 1), for separation, heat transfer, LES and DES." : NULL, 34);
    char autoL[48]; pl_fmt_ui(autoL, sizeof autoL, "Automatic (%s)", sim_wall_treatment(&(SimSetup){ .turb = s->turb, .wallTreat = WT_AUTO }) == WT_RESOLVED ? "resolved" : "functions");
    const char *items[WT__N] = { autoL, WALLT_NAME[WT_FUNCTIONS], WALLT_NAME[WT_RESOLVED] }, *why[WT__N];
    for (int i = 0; i < WT__N; i++) why[i] = sim_wall_why_not(s, i);
    int wt = s->wallTreat;
    if (ui_segmented_why(ui_id("su-wall"), (Rectangle){ cr.x, cr.y, fminf(480, cr.width), 34 }, items, WT__N, &wt, why) && wt != s->wallTreat) { s->wallTreat = wt; save(); }
    /* the helper: a flat plate as long as the part at the inflow speed */
    double U = sim_ref_speed(s), L = s->forces && s->fr.lRef > 0 ? s->fr.lRef : P.size, yp = sim_yplus_target(s), h1, d, ut;
    double Re = L > 0 ? sim_wall_spacing(s, U, L, yp, &h1, &d, &ut) : 0;
    char t[600];
    if (!(Re > 0)) {
        y += note(x, y + 4, w, "The y+ helper needs the part's size: choose an STL (Mesh > STL mesher), or type a reference length under Reports.") + 4;
        return y - y0;
    }
    double r = 1.2, tot;
    int nl = sim_layer_count(h1, d, r, &tot);
    if (h1 >= 0.5 * d && sim_wall_treatment(s) == WT_FUNCTIONS) {
        /* a slow or small flow: the log layer wall functions need isn't there; resolving the wall is the way */
        double hr; sim_wall_spacing(s, U, L, 1, &hr, NULL, NULL);
        pl_fmt_ui(t, sizeof t, "For y+ ≈ %.3g the first cell would be %s high (%.3g m/s over %.3g m, Re %.2g), but the boundary layer is only about %s thick: "
                  "there is no log layer for wall functions to sit in. Resolve the wall instead (Walls: Resolved): y+ ≈ 1 wants a first cell of %s.",
                  yp, fmt_len(h1), U, L, Re, fmt_len(d), fmt_len(hr));
    } else
        pl_fmt_ui(t, sizeof t, "For y+ ≈ %.3g the first cell is %s high (%.3g m/s over %.3g m, Re %.2g). The boundary layer grows to about %s; %d layer%s growing by %.2g "
                  "cover it.", yp, fmt_len(h1), U, L, Re, fmt_len(d), nl, nl == 1 ? "" : "s", r);
    double first, cell; int n;
    bool stl = stlmesh_wall_layers(c, &first, &n, &cell);
    if (stl) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), " The STL mesher's %d layer%s start at %s: y+ ≈ %.3g.", n, n == 1 ? "" : "s", fmt_len(first), sim_yplus_of(s, U, L, first));
    else if (cell > 0) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), " The STL mesher has no prism layers yet: the first cell is its surface cell, %s (y+ ≈ %.3g).",
                                 fmt_len(cell), sim_yplus_of(s, U, L, cell));
    float bw = ui_button_width(ICON_LAYERS, "Send to the STL mesher");
    bool side = cell > 0 && w - bw - 16 >= 360;
    float tw = side ? w - bw - 16 : w;
    float th = ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y + 4, tw, 400 }, 19, TH.text3, true);
    if (cell > 0) {
        Rectangle br = side ? (Rectangle){ x + w - bw, y + 4 + fmaxf(0, (th - 34) / 2), bw, 34 } : (Rectangle){ x, y + 4 + th + 8, bw, 34 };
        if (ui_button(ui_id("su-yplus-send"), br, ICON_LAYERS, "Send to the STL mesher", BTN_SECONDARY)) {
            double f; int k;
            if (stlmesh_set_wall_layers(c, h1, &f, &k))
                ui_toast(TOAST_OK, "STL mesher: %d prism layer%s from %s (y+ ≈ %.3g). Generate the mesh again to use %s", k, k == 1 ? "" : "s", fmt_len(f),
                         sim_yplus_of(s, U, L, f), k == 1 ? "it" : "them");
            else ui_toast(TOAST_WARN, "y+ %.3g needs more than 60 layers on the STL mesher's %s surface cells: make its minimum size smaller first", yp, fmt_len(cell));
        }
        if (!side) th += 50;
    }
    y += th + 16;
    return y - y0;
}

/* ---------------------------------------------------------------------------- 1 Models */
static float models_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    bool hints = w > 560;
    /* the template it started from */
    Rectangle cr = prop_row(x, &y, w, "Started from", hints ? sim_template_hint(s->tmpl) : NULL, 34);
    ui_icon(TMPL_ICON[s->tmpl >= 0 ? s->tmpl : TMPL_BLANK], cr.x + 12, cr.y + 17, 18, TH.accent);
    const char *tn = s->tmpl >= 0 && s->tmpl != TMPL_BLANK ? TMPL_NAME[s->tmpl] : "Your own choices";
    ui_text_clip(F_MED, 14, tn, cr.x + 30, cr.y + 8, cr.width - 30, TH.text);
    float tw = ui_text_w(F_MED, 14, tn);
    float bw = ui_button_width(ICON_REFRESH_CW, "Change");
    if (cr.x + 30 + tw + 16 + bw <= cr.x + cr.width && ui_button(ui_id("su-tmpl-change"), (Rectangle){ cr.x + 30 + tw + 16, cr.y, bw, 34 }, ICON_REFRESH_CW, "Change", BTN_SECONDARY))
        P.picking = true;
    /* time */
    cr = prop_row(x, &y, w, "Time", hints ? "Steady: the flow settles to one answer. Transient: vortex shedding, start-up, anything that changes." : NULL, 34);
    int fl = s->flow;
    if (ui_segmented(ui_id("su-flow"), (Rectangle){ cr.x, cr.y, fminf(260, cr.width), 34 }, (const char **)FLOW_NAME, FLOW__N, &fl) && fl != s->flow) {
        s->flow = fl;
        const char *m = sim_settle(s);
        if (m) ui_toast(TOAST_INFO, "%s", m);
        save();
    }
    /* the approach, then the model within it */
    cr = prop_row(x, &y, w, "Turbulence", hints ? "RANS averages the turbulence out: fast, the usual choice. LES resolves the large eddies: a fine 3-D mesh and "
                                                  "small time steps. DES is RANS next to walls and LES away from them." : NULL, 34);
    const char *ap[APPR__N];
    for (int i = 0; i < APPR__N; i++) ap[i] = sim_approach_label(s, i);
    int a = sim_approach(s->turb), a0 = a;
    Rectangle ar = { cr.x, cr.y, fminf(360, cr.width), 34 };
    if (ui_segmented(ui_id("su-appr"), ar, ap, APPR__N, &a) && a != a0) {
        const char *m = sim_set_approach(s, a);
        if (m) ui_toast(TOAST_INFO, "%s", m);
        save();
    }
    if (s->flow == FLOW_STEADY) ui_tooltip(ar, "LES and DES need a transient run: picking one makes the run transient.");
    if (sim_approach(s->turb) != APPR_LAMINAR) {
        int ids[TURB__N], n = sim_approach_models(sim_approach(s->turb), ids), sel = 0;
        const char *names[TURB__N];
        for (int i = 0; i < n; i++) { names[i] = TURB_NAME[ids[i]]; if (ids[i] == s->turb) sel = i; }
        cr = prop_row(x, &y, w, "Model", sim_model_hint(s->turb), 34);
        if (ui_select(ui_id("su-turb"), (Rectangle){ cr.x, cr.y, fminf(260, cr.width), 34 }, names, n, &sel) && ids[sel] != s->turb) { s->turb = ids[sel]; sim_settle(s); save(); }
    } else cr = (Rectangle){ ar.x, ar.y, cr.width, 34 };
    double Re = reynolds();
    if (Re > 0) {
        float rx = sim_approach(s->turb) == APPR_LAMINAR ? ar.width + 16 : fminf(260, cr.width) + 16;
        char re[160]; pl_fmt_ui(re, sizeof re, "Re ≈ %.3g (%.3g m/s over %.3g m)%s", Re, sim_ref_speed(s), P.size,
                                Re < 2000 && s->turb != TURB_LAMINAR ? ": laminar would do" : Re > 1e4 && s->turb == TURB_LAMINAR ? ": that is turbulent" : "");
        ui_text_clip(F_REG, 13, re, cr.x + rx, cr.y + 9, cr.width - rx, (Re < 2000) == (s->turb == TURB_LAMINAR) ? TH.text3 : TH.warn);
    }
    /* the walls (plan step 5): wall functions or resolved, and the y+ helper */
    if (s->turb != TURB_LAMINAR) {
        y += wall_rows(c, x, y, w, hints);
    }
    /* how a transient run steps */
    if (s->flow == FLOW_TRANSIENT) {
        static const char *const algHint[ALG__N] = {
            "pisoFoam: accurate, small fixed time steps (Courant under 1). Suits LES and DES.",
            "pimpleFoam: the usual transient choice; the time step can follow a Courant limit.",
            "pimpleFoam with many outer correctors per step: time steps 5 to 10 times larger, for slow changes." };
        cr = prop_row(x, &y, w, "Algorithm", s->energy != EN_OFF && s->algo != ALG_PISO ? TextFormat("%s%s", sim_app(s), strchr(algHint[s->algo], ':')) : algHint[s->algo], 34);
        int al = s->algo;
        const char *aw[ALG__N];
        for (int i = 0; i < ALG__N; i++) aw[i] = i == ALG_PISO ? sim_algo_why_not(s, i) : NULL;   /* PISO: not with heat */
        if (ui_segmented_why(ui_id("su-algo"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)ALG_NAME, ALG__N, &al, aw) && al != s->algo) { sim_set_algo(s, al); save(); }
    }
    /* heat (plan step 3) */
    static const char *const enHint[EN__N] = {
        "No temperature: the flow is the same everywhere it is.",
        "The flow carries heat from warm walls and inlets (forced convection); the density stays put. buoyantSimpleFoam or buoyantPimpleFoam with gravity off.",
        "Warm fluid rises and cool sinks (natural convection): the density follows the temperature, under gravity." };
    /* compressibility (plan step 4): an ideal gas whose density follows the pressure, with the Mach number as it stands */
    cr = prop_row(x, &y, w, "Compressibility", hints ? "Incompressible: one density (or one that follows only the temperature). Ideal gas: the density follows "
                                                       "the pressure too, for gas above about Mach 0.3 (rhoSimpleFoam, rhoPimpleFoam)." : NULL, 34);
    int co = s->comp;
    float cw = fminf(320, cr.width);
    if (ui_segmented(ui_id("su-comp"), (Rectangle){ cr.x, cr.y, cw, 34 }, (const char **)COMP_NAME, COMP__N, &co) && co != s->comp) {
        int alg = s->algo;
        sim_set_comp(s, co);
        if (co == COMP_GAS && alg != s->algo) ui_toast(TOAST_INFO, "A compressible gas steps through time with rhoPimpleFoam: the algorithm is now PIMPLE");
        else ui_toast(TOAST_INFO, co == COMP_GAS ? "Ideal gas: the temperature is on, and Materials holds the gas" : "Incompressible: back to one density");
        save();
    }
    double Ma = sim_mach(s);
    if (Ma > 0 && cr.width - cw - 16 > 80) {
        char mt[160]; pl_fmt_ui(mt, sizeof mt, "Ma ≈ %.2g%s", Ma, Ma > 1 ? ": supersonic" : Ma > 0.8 ? ": transonic" : Ma > 0.3 && !sim_is_gas(s) ? ": compressible" : "");
        ui_text_clip(F_REG, 13, mt, cr.x + cw + 16, cr.y + 9, cr.width - cw - 16, Ma > 0.3 && !sim_is_gas(s) ? TH.warn : TH.text3);
    }
    cr = prop_row(x, &y, w, "Energy", hints ? (sim_is_gas(s) ? "A compressible gas always carries its temperature: its density follows it." : enHint[s->energy]) : NULL, 34);
    int en = s->energy;
    const char *ew[EN__N];
    for (int i = 0; i < EN__N; i++) ew[i] = sim_energy_why_not(s, i);
    if (ui_segmented_why(ui_id("su-energy"), (Rectangle){ cr.x, cr.y, fminf(420, cr.width), 34 }, (const char **)ENERGY_NAME, EN__N, &en, ew) && en != s->energy) {
        int alg = s->algo;
        sim_set_energy(s, en);
        gravity_auto(c);
        if (alg != s->algo) ui_toast(TOAST_INFO, "With heat on, buoyantPimpleFoam steps through time: the algorithm is now PIMPLE");
        save();
    }
    if (s->energy == EN_BUOYANT) {
        double g[3];
        bool have = stlmesh_gravity(c, g);
        if (have) {                                         /* from the STL mesher's Up, or typed in */
            cr = prop_row(x, &y, w, "Gravity", hints ? "9.81 m/s² down the STL mesher's Up direction (Mesh > STL mesher, Flow), or typed in." : NULL, 34);
            static const char *GM[] = { "From the mesher's Up", "Typed in" };
            int mode = s->gAuto ? 0 : 1;
            float mw = fminf(320, cr.width);
            if (ui_segmented(ui_id("su-gmode"), (Rectangle){ cr.x, cr.y, mw, 34 }, GM, 2, &mode) && (mode == 0) != s->gAuto) { s->gAuto = mode == 0; gravity_auto(c); save(); }
            if (s->gAuto) {
                char t[96]; pl_fmt_ui(t, sizeof t, "(%.3g %.3g %.3g) m/s²", s->g[0], s->g[1], s->g[2]);
                if (cr.width - mw - 16 > 80) ui_text_clip(F_MONO, 13, t, cr.x + mw + 16, cr.y + 9, cr.width - mw - 16, TH.text3);
            } else row_vec(x, &y, w, hints, "su-g", "Gravity vector", NULL, s->g, "m/s²");
        } else row_vec(x, &y, w, hints, "su-g", "Gravity", "m/s², in the mesh's axes: (0 -9.81 0) when y is up.", s->g, "m/s²");
        /* how strong the buoyancy is: Ra (natural convection's own number), Ri (against the inflow) */
        double Ra = sim_rayleigh(s, P.height), Ri = sim_richardson(s, P.height);
        if (Ra > 0) {
            char t[240];
            pl_fmt_ui(t, sizeof t, "Ra ≈ %.3g over a height of %.3g m with %.3g K across: %s%s", Ra, P.height, sim_delta_T(s),
                      Ra < 1e8 ? "laminar" : Ra < 1e9 ? "near the turn to turbulence" : "turbulent",
                      Ri > 0 ? TextFormat(". Ri ≈ %.3g: %s", Ri, Ri < 0.1 ? "the inflow carries the heat; buoyancy hardly matters" : Ri > 10 ? "buoyancy drives the flow"
                                                                                                                    : "buoyancy and the inflow both matter") : "");
            y += note(x, y + 4, w, t);
        }
    }
    y += 8;
    y += issues_callout(SS_MODELS, x, y, w);
    /* the physics still to come: there, greyed, saying so */
    y += 12;
    y += section_head(x, y, w, "Other physics", "Free surfaces and rotating zones come in later updates; hover an option for what it will do.");
    static const char *const MU[] = { "None", "Free surface" }, *const MO[] = { "None", "Rotating zone" }, *const EX[] = { "None", "Passive scalar" };
    static const char *const MUw[] = { NULL, "Water and air with the surface between them (VOF, needs a transient run): a later update" },
                      *const MOw[] = { NULL, "A zone turning with a fan or a stirrer (MRF, frozen rotor): a later update" },
                      *const EXw[] = { NULL, "A dye or a concentration carried by the flow: a later update" };
    struct { const char *id, *label, *const *items, *const *why; int n; } G[] = {
        { "su-multi", "Multiphase", MU, MUw, 2 }, { "su-motion", "Motion", MO, MOw, 2 }, { "su-extras", "Extras", EX, EXw, 2 } };
    for (int g = 0; g < 3; g++) {
        cr = prop_row(x, &y, w, G[g].label, NULL, 34);
        int sel = 0;
        ui_segmented_why(ui_id(G[g].id), (Rectangle){ cr.x, cr.y, fminf(G[g].n * 140.f, cr.width), 34 }, (const char **)G[g].items, G[g].n, &sel, G[g].why);
    }
    return y - y0;
}

/* ---------------------------------------------------------------------------- 2 Materials */
/* a compressible gas: its preset (molar mass, gamma, Sutherland's viscosity) and the operating pressure; what follows at
   the start (density, viscosity, cp, conductivity, the speed of sound) shown as it is worked out */
static float gas_materials(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    y += issues_callout(SS_MATERIALS, x, y, w);
    Rectangle cr = prop_row(x, &y, w, "Gas", "An ideal gas: p = rho R T. Custom for your own molar mass, gamma and viscosity.", 34);
    int g = s->gas;
    if (ui_segmented(ui_id("su-gas"), (Rectangle){ cr.x, cr.y, fminf(400, cr.width), 34 }, (const char **)GAS_NAME, GAS__N, &g) && g != s->gas) {
        sim_gas_preset(s, g); sim_settle(s); save();
    }
    char a[96]; pl_fmt_ui(a, sizeof a, "%.5g atm", s->pOp / 101325);
    if (row_num(x, &y, w, true, "su-pop", "Operating pressure", "Absolute. Every pressure typed elsewhere is gauge, above this one: 0 at an outlet means this.",
                &s->pOp, 1, 1e9, "Pa", a)) { sim_settle(s); save(); }
    bool rows = s->gas == GAS_CUSTOM || P.more[SS_MATERIALS];
    if (rows) {
        bool ch = false;
        ch |= row_num(x, &y, w, true, "su-molw", "Molar mass", "Air 28.96, nitrogen 28.01, CO2 44.01.", &s->molW, 1e-3, 1e4, "kg/kmol", NULL);
        ch |= row_num(x, &y, w, true, "su-gamma", "Ratio of specific heats", "gamma = cp / cv: 1.4 for air, 1.67 for a monatomic gas.", &s->gamma, 1.0001, 3, NULL, NULL);
        ch |= row_num(x, &y, w, true, "su-as", "Sutherland As", "mu = As √T / (1 + Ts / T).", &s->muAs, 1e-12, 1, "Pa·s/√K", NULL);
        ch |= row_num(x, &y, w, true, "su-ts", "Sutherland Ts", NULL, &s->muTs, 0, 1e5, "K", NULL);
        if (ch) { s->gas = GAS_CUSTOM; sim_settle(s); save(); }
    }
    char t[400];
    pl_fmt_ui(t, sizeof t, "%sAt %.6g Pa and %.4g °C: density %.4g kg/m³, viscosity %.4g Pa·s, specific heat %.4g J/(kg·K), conductivity %.3g W/(m·K), "
              "speed of sound %.4g m/s.", rows ? "" : TextFormat("Molar mass %.4g kg/kmol, gamma %.4g, Sutherland As %.4g, Ts %.4g K. ", s->molW, s->gamma, s->muAs, s->muTs),
              sim_pabs(s, s->p0), s->T0 - 273.15, s->rho, sim_gas_mu(s, s->T0), s->cp, s->kappa, sim_sound_speed(s, s->T0));
    y += note(x, y + 8, w, t) + 8;
    if (s->gas != GAS_CUSTOM) y += more_toggle(SS_MATERIALS, x, y);
    return y - y0;
}

static float materials_step(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    if (sim_is_gas(s)) return gas_materials(x, y, w);
    bool heat = s->energy != EN_OFF, buoy = s->energy == EN_BUOYANT;
    y += issues_callout(SS_MATERIALS, x, y, w);
    Rectangle cr = prop_row(x, &y, w, "Fluid", heat ? "Air and water at 20 °C, their thermal properties too; Custom for your own."
                                                    : "Air and water at 20 °C; Custom for your own density and viscosity.", 34);
    int f = s->fluid;
    if (ui_segmented(ui_id("su-fluid"), (Rectangle){ cr.x, cr.y, fminf(360, cr.width), 34 }, (const char **)FLUID_NAME, FLUID__N, &f) && f != s->fluid) { sim_fluid(s, f); save(); }
    char mu[64]; pl_fmt_ui(mu, sizeof mu, "dynamic viscosity %.4g Pa·s", s->nu * s->rho);
    double Pr = s->kappa > 0 ? s->nu * s->rho * s->cp / s->kappa : 0;
    bool rows = s->fluid == FLUID_CUSTOM || P.more[SS_MATERIALS];
    if (!rows) {
        char t[400]; pl_fmt_ui(t, sizeof t, "Density %.4g kg/m³, kinematic viscosity %.4g m²/s (%s).", s->rho, s->nu, mu);
        if (heat) pl_fmt_ui(t + strlen(t), sizeof t - strlen(t), " Specific heat %.4g J/(kg·K), conductivity %.3g W/(m·K) (Prandtl number %.3g)%s.",
                            s->cp, s->kappa, Pr, buoy ? TextFormat(", thermal expansion %.3g 1/K%s", sim_beta(s), s->fluid == FLUID_AIR ? " (an ideal gas: 1 / T)" : "") : "");
        y += note(x, y + 8, w, t) + 8;
    } else {
        if (row_num(x, &y, w, true, "su-rho", "Density", heat ? "At the reference temperature." : "The solvers work in pressure over density; this turns pascals into that.",
                    &s->rho, 1e-6, 1e5, "kg/m³", NULL)) { s->fluid = FLUID_CUSTOM; save(); }
        if (row_num(x, &y, w, true, "su-nu", "Kinematic viscosity", "nu = mu / rho.", &s->nu, 1e-12, 1e3, "m²/s", mu)) { s->fluid = FLUID_CUSTOM; save(); }
        if (heat) {
            if (row_num(x, &y, w, true, "su-cp", "Specific heat", "cp: the heat that warms a kilogram by one kelvin.", &s->cp, 1e-3, 1e7, "J/(kg·K)", NULL)) { s->fluid = FLUID_CUSTOM; save(); }
            char pr[64]; pl_fmt_ui(pr, sizeof pr, "Prandtl number %.3g", Pr);
            if (row_num(x, &y, w, true, "su-kappa", "Thermal conductivity", "k: how well it conducts heat.", &s->kappa, 1e-6, 1e4, "W/(m·K)", pr)) { s->fluid = FLUID_CUSTOM; save(); }
            if (buoy && s->fluid != FLUID_AIR && row_num(x, &y, w, true, "su-beta", "Thermal expansion", "beta: how much the density drops per kelvin warmer (an ideal gas: 1 / T).",
                                &s->beta, 0, 1, "1/K", NULL)) { s->fluid = FLUID_CUSTOM; save(); }
        }
    }
    if (buoy) {
        char c[48]; pl_fmt_ui(c, sizeof c, "%.4g °C", s->Tref - 273.15);
        row_num(x, &y, w, true, "su-tref", "Reference temperature", "Where the density is the one above: warmer fluid rises, cooler sinks.", &s->Tref, 1, 1e4, "K", c);
    }
    if (s->fluid != FLUID_CUSTOM) y += more_toggle(SS_MATERIALS, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 3 Boundaries */
static float boundaries_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    y += note(x, y, w, "Rename them and set what each one is on Mesh > STL mesher, by clicking the part. The wind tunnel's own boundaries and any "
                       "other patch of the mesh are here too.");
    y += issues_callout(SS_BOUNDARIES, x, y, w);
    if (!P.s.nB) {
        const char *t = "No boundaries yet", *d = "Choose an STL on Mesh > STL mesher (or generate the mesh): its boundaries appear here.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LAYERS, t, d);
        return y + ch + 16 - y0;
    }
    bool meshed = c->nPatches > 0 && c->patchSource == 1;
    for (int i = 0; i < P.s.nB; i++) {
        SimBoundary *b = &P.s.b[i];
        ui_push_id(ui_idn(ui_id("su-b"), i));
        Rectangle card = { x, y, w, 0 };
        float cy = y + 14;
        float nw = fminf(260, w * 0.34f), sx = x + 16 + nw + 14;
        ui_text_clip(F_MONO_MED, 15, b->name, x + 16, cy + 7, nw, TH.text);
        int role = b->role;
        bool con = sim_constraint(b->meshType);
        ui_disable(con);
        if (ui_select(ui_id("role"), (Rectangle){ sx, cy, 180, 34 }, ROLE_NAME, ROLE__N, &role) && role != b->role) {
            if (b->fromStl) stlmesh_set_role(c, b->name, role);     /* the STL's type: the mesh patch follows on the next Generate */
            b->role = role; save();
        }
        ui_disable(false);
        char sub[200];
        bool inMesh = false;
        for (int k = 0; k < c->nPatches; k++) if (!strcmp(c->patches[k].name, b->name)) inMesh = true;
        pl_fmt_ui(sub, sizeof sub, "%s%s%s", b->fromStl ? "from the STL" : P.external && !inMesh ? "the wind tunnel" : "a mesh patch",
                  b->area > 0 ? TextFormat("  ·  %.3g m²", b->area) : "", meshed && !inMesh ? "  ·  not in the mesh yet: generate it again" : "");
        float subx = sx + 196;
        if (x + w - 14 - subx >= 140) ui_text_clip(F_REG, 12.5f, sub, subx, cy + 9, x + w - 14 - subx, meshed && !inMesh ? TH.warn : TH.text3);
        else { cy += 40; ui_text_clip(F_REG, 12.5f, sub, x + 16, cy + 2, w - 32, meshed && !inMesh ? TH.warn : TH.text3); cy -= 14; }
        cy += 46;
        cy += setup_boundary_editor(c, b->name, x + 16, cy, w - 32);
        card.height = cy - y + 4;
        ui_rect_lines(card, 10, 1, TH.lineSoft);
        ui_pop_id();
        y += card.height + 12;
    }
    return y - y0;
}

/* ---------------------------------------------------------------------------- 5 Reports: what is measured (plan step 6)
 * Each report added from a menu, with only its own rows: a probe's field and points, a surface's boundary and measure,
 * a pressure drop's two boundaries, a volume's measure and field. Each becomes a function object, and a Run-page monitor. */
static const char *field_label(const char *f)
{
    return !strcmp(f, "U") ? "Speed |U|" : !strcmp(f, "p") ? "Pressure" : !strcmp(f, "T") ? "Temperature" : !strcmp(f, "k") ? "Turbulent energy k" :
           !strcmp(f, "omega") ? "omega" : !strcmp(f, "epsilon") ? "epsilon" : !strcmp(f, "nuTilda") ? "nuTilda" : f;
}

/* a select of the setup's fields (r->field), and of its open boundaries (into dst); true when changed */
static bool field_select(UiId id, Rectangle r, SimReport *rp)
{
    const char *f[SIM_MAX_F], *lab[SIM_MAX_F];
    int n = sim_report_fields(&P.s, f), sel = 0;
    for (int i = 0; i < n; i++) { lab[i] = field_label(f[i]); if (!strcmp(f[i], rp->field)) sel = i; }
    if (!ui_select(id, r, lab, n, &sel) || !strcmp(f[sel], rp->field)) return false;
    pl_strlcpy(rp->field, f[sel], sizeof rp->field);
    return true;
}

static bool patch_select(UiId id, Rectangle r, char *dst, size_t cap)
{
    const char *names[SIM_MAX_B]; int n = 0, sel = -1;
    for (int i = 0; i < P.s.nB; i++) {
        if (sim_constraint(P.s.b[i].meshType)) continue;
        if (!strcmp(P.s.b[i].name, dst)) sel = n;
        names[n++] = P.s.b[i].name;
    }
    if (!n) return false;
    int was = sel; if (sel < 0) sel = 0;
    if (!ui_select(id, r, names, n, &sel) || (sel == was && was >= 0)) return false;
    pl_strlcpy(dst, names[sel], cap);
    return true;
}

static float measurements(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    y += section_head(x, y, w, "Measured while it runs", "Each report is worked out every iteration and shown on the Run page as it settles (Monitors), "
                                                         "where it can stop the run once it stops changing. The walls' y+ is always measured.");
    /* add one */
    static const char *ADD[RP__N + 1] = { "Add a report ...", "Probe points: values at points", "Surface: an average, a flow rate, the least or greatest on a boundary",
                                          "Pressure drop between two boundaries", "Volume: the average, least or greatest in the domain" };
    int pick = 0;
    ui_disable(s->nR >= SIM_MAX_R);
    if (ui_select(ui_id("su-rep-add"), (Rectangle){ x, y, fminf(420, w), 36 }, ADD, RP__N + 1, &pick) && pick > 0) {
        double lo[3], hi[3], mid[3] = { 0, 0, 0 };
        if (mesh_bounds(c->s.path, lo, hi)) for (int k = 0; k < 3; k++) mid[k] = 0.5 * (lo[k] + hi[k]);
        if (sim_report_add(s, pick - 1, mid) >= 0) save();
    }
    ui_disable(false);
    y += 50;
    int del = -1;
    for (int i = 0; i < s->nR; i++) {
        SimReport *r = &s->r[i];
        ui_push_id(ui_idn(ui_id("su-rep"), i));
        float cy = y + 12;
        char d[200]; sim_report_describe(s, r, d, sizeof d);
        ui_text_clip(F_SEMI, 14, d, x + 16, cy + 2, w - 80, TH.text);
        char sub[120]; pl_fmt_ui(sub, sizeof sub, "%s  ·  %s  ·  postProcessing/%s", REPORT_NAME[r->kind], sim_report_unit(s, r), r->name);
        ui_text_clip(F_MONO, 12, sub, x + 16, cy + 22, w - 80, TH.text3);
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 46, cy, 34, 34 }, ICON_X, NULL, BTN_ICON)) del = i;
        ui_tooltip((Rectangle){ x + w - 46, cy, 34, 34 }, "Remove this report");
        cy += 50;
        float ix = x + 16, iw = w - 32;
        bool ch = false;
        Rectangle cr;
        switch (r->kind) {
        case RP_PROBE:
            cr = prop_row(ix, &cy, iw, "Field", NULL, 34);
            ch |= field_select(ui_id("field"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r);
            for (int k = 0; k < r->nPts; k++) {
                char id[32], lab[32]; snprintf(id, sizeof id, "pt%d", k); pl_fmt_ui(lab, sizeof lab, "Point %d", k + 1);
                ch |= row_vec(ix, &cy, iw, true, id, lab, k == 0 ? "In the mesh's axes, m." : NULL, r->pts[k], "m");
            }
            {
                float bw = ui_button_width(ICON_PLUS, "Add a point");
                ui_disable(r->nPts >= SIM_MAX_PROBES);
                if (ui_button(ui_id("pt-add"), (Rectangle){ ix, cy + 4, bw, 32 }, ICON_PLUS, "Add a point", BTN_GHOST)) {
                    memcpy(r->pts[r->nPts], r->pts[r->nPts - 1], sizeof r->pts[0]); r->nPts++; ch = true;
                }
                ui_disable(r->nPts <= 1);
                float rw = ui_button_width(ICON_X, "Remove the last");
                if (ui_button(ui_id("pt-del"), (Rectangle){ ix + bw + 8, cy + 4, rw, 32 }, ICON_X, "Remove the last", BTN_GHOST)) { r->nPts--; ch = true; }
                ui_disable(false);
                cy += 44;
            }
            break;
        case RP_SURFACE:
            cr = prop_row(ix, &cy, iw, "Boundary", NULL, 34);
            ch |= patch_select(ui_id("patch"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r->patch, sizeof r->patch);
            cr = prop_row(ix, &cy, iw, "Measure", r->op == SO_MASSAVG ? "Weighted by the flow through each face: the mixed-out value." :
                                                  r->op == SO_FLOW ? "Out of the domain positive, into it negative." : NULL, 34);
            { int op = r->op; if (ui_select(ui_id("op"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, SURF_OP_NAME, SO__N, &op) && op != r->op) { r->op = op; ch = true; } }
            if (r->op != SO_FLOW) {
                cr = prop_row(ix, &cy, iw, "Field", NULL, 34);
                ch |= field_select(ui_id("field"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r);
            }
            break;
        case RP_DROP:
            cr = prop_row(ix, &cy, iw, "From", "Usually the inlet: its area-averaged pressure ...", 34);
            ch |= patch_select(ui_id("from"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r->patch, sizeof r->patch);
            cr = prop_row(ix, &cy, iw, "To", "... minus this one's (the outlet).", 34);
            ch |= patch_select(ui_id("to"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r->patch2, sizeof r->patch2);
            break;
        default:
            cr = prop_row(ix, &cy, iw, "Measure", NULL, 34);
            { int op = r->op; if (ui_select(ui_id("op"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, VOL_OP_NAME, VO__N, &op) && op != r->op) { r->op = op; ch = true; } }
            cr = prop_row(ix, &cy, iw, "Field", NULL, 34);
            ch |= field_select(ui_id("field"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, r);
            break;
        }
        if (ch) save();
        Rectangle card = { x, y, w, cy - y + 6 };
        ui_rect_lines(card, 10, 1, TH.lineSoft);
        ui_pop_id();
        y += card.height + 12;
    }
    if (del >= 0) { sim_report_remove(s, del); save(); }
    if (!s->nR) y += note(x, y, w, "None yet. \"What is my pressure drop?\" Add a Pressure drop. \"What is the speed here?\" Add Probe points there.") + 4;
    y += 12;
    return y - y0;
}

/* ---------------------------------------------------------------------------- 5 Reports: lift and drag (user request, 2026-10-26) */
static float reports_step(Case *c, float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    y += issues_callout(SS_REPORTS, x, y, w);
    y += measurements(c, x, y, w);
    if (s->energy != EN_OFF) {
        y += section_head(x, y, w, "Heat through the walls", "The heat each wall passes into the fluid (W; negative: out of it), and its least and greatest "
                                                             "flux (W/m²), at each write; for each heated or cooled wall, its mean flux, temperature and Nusselt number every "
                                                             "iteration. They are in postProcessing; the Run page shows the latest.");
        Rectangle hr = prop_row(x, &y, w, "Monitor wall heat", "Adds the wallHeatFlux function object to system/controlDict.", 24);
        if (ui_switch(ui_id("su-heat"), hr.x, hr.y + 2, &s->heatReport)) save();
        if (s->heatReport) {
            /* the Nusselt number of each wall that passes heat: Nu = q L / (kappa dT) */
            int nh = 0;
            for (int i = 0; i < s->nB; i++) nh += sim_wall_heated(&s->b[i]);
            Rectangle lr = prop_row(x, &y, w, "Nusselt length", "Nu = q L / (k ΔT), q the wall's mean heat flux. Usually the height of a cavity, the length of a plate "
                                                                 "or a pipe's diameter.", 34);
            int mode = s->nuAuto ? 0 : 1;
            static const char *MODE[] = { "Mesh height", "Typed in" };
            float sw = fminf(240, lr.width * 0.5f);
            if (ui_segmented(ui_id("su-nul"), (Rectangle){ lr.x, lr.y, sw, 34 }, MODE, 2, &mode) && (mode == 0) != s->nuAuto) {
                s->nuAuto = mode == 0;
                if (s->nuAuto && P.height > 0) s->nuLength = P.height;
                save();
            }
            ui_disable(s->nuAuto);
            if (num_in(ui_id("su-nulv"), (Rectangle){ lr.x + sw + 10, lr.y, fminf(160, lr.width - sw - 10), 34 }, &s->nuLength, "%.6g", 1e-12, 1e9, "m")) save();
            ui_disable(false);
            char a[120], d[48] = "";
            double span = sim_delta_T(s);
            if (span > 0) pl_fmt_ui(d, sizeof d, "%.4g K", span);
            pl_fmt_ui(a, sizeof a, s->nuDT > 0 ? "" : span > 0 ? "0: the span the boundaries set, %s" : "0: none known yet", d);
            row_num(x, &y, w, true, "su-nudt", "Nusselt ΔT", "Hot wall to cold wall, or wall to the incoming fluid.", &s->nuDT, 0, 1e5, "K", a);
            if (nh == 0) y += note(x, y + 4, w, "No wall heats or cools the fluid yet (Boundaries): there is no Nusselt number to report.") + 4;
            else if (!(sim_nu_dT(s) > 0)) y += note(x, y + 4, w, "Type a ΔT to get the Nusselt number: with only heat-flux walls the wall temperature isn't known "
                                                                 "before the run. The mean flux and temperature are reported anyway.") + 4;
        }
        y += 18;
    }
    y += section_head(x, y, w, "Lift and drag", "The force of the flow on the part while it runs: drag along the flow, lift across it, and as "
                                                "coefficients, Cd = drag / (½ ρ U² A) and Cl the same with the lift. The Run page plots them as they settle.");
    Rectangle cr = prop_row(x, &y, w, "Monitor forces", "Adds the forces and forceCoeffs function objects to system/controlDict.", 24);
    if (ui_switch(ui_id("su-forces"), cr.x, cr.y + 2, &s->forces)) save();
    if (!s->forces) return y + 8 - y0;
    /* the walls they are summed over */
    float cw[SIM_MAX_B]; int nw = 0; float lineW = 0, lines = 1, room = w - fminf(300, w * 0.38f);
    for (int i = 0; i < s->nB; i++) {
        const SimBoundary *b = &s->b[i];
        if (b->role != ROLE_WALL || sim_constraint(b->meshType)) { cw[i] = 0; continue; }
        cw[i] = ui_text_w(F_REG, 13.5f, b->name) + 44;
        if (lineW + cw[i] > room && lineW > 0) { lines++; lineW = 0; }
        lineW += cw[i]; nw++;
    }
    cr = prop_row(x, &y, w, "On these walls", "The part's walls; leave out a wind tunnel's ground.", nw ? lines * 30 - 6 : 24);
    if (!nw) ui_text(F_REG, 13, "No walls: make the part's boundaries Walls first.", cr.x, cr.y + 4, TH.warn);
    float bx = cr.x, by = cr.y;
    for (int i = 0; i < s->nB; i++) {
        SimBoundary *b = &s->b[i];
        if (!cw[i]) continue;
        if (bx + cw[i] > cr.x + cr.width && bx > cr.x) { bx = cr.x; by += 30; }
        bool on = sim_force_on(s, b), was = on;
        ui_checkbox(ui_idn(ui_id("su-fw"), i), (Rectangle){ bx, by, cw[i] - 14, 24 }, &on, b->name);
        if (on != was) { b->force = on; save(); }
        bx += cw[i];
    }
    /* the reference: from the part (and the tunnel), or typed in */
    static SimForceRef auto_r; static bool twoD, have; static double at = -10; static char atCase[PL_PATH];
    if (ui_time() - at > 0.5 || strcmp(atCase, c->s.path)) {   /* the frontal area walks every triangle: twice a second */
        at = ui_time(); pl_strlcpy(atCase, c->s.path, sizeof atCase);
        have = stlmesh_force_ref(c, &auto_r, &twoD);
    }
    cr = prop_row(x, &y, w, "Reference values", have ? "From the part: its length along the flow, its frontal area (2-D: length × depth), the tunnel's flow and up."
                                                     : "Type them in: there is no STL to measure them from.", 34);
    int mode = have && s->forceAuto ? 0 : 1;
    static const char *MODE[] = { "From the part", "Typed in" };
    ui_disable(!have);
    if (ui_segmented(ui_id("su-fref"), (Rectangle){ cr.x, cr.y, fminf(280, cr.width), 34 }, MODE, 2, &mode) && (mode == 0) != s->forceAuto) { s->forceAuto = mode == 0; save(); }
    ui_disable(false);
    bool locked = have && s->forceAuto;
    if (locked) {
        SimForceRef a = auto_r;
        a.Uinf = s->fr.Uinf;                               /* the speed stays the user's (0: the inflow's) */
        if (memcmp(&a, &s->fr, sizeof a)) { s->fr = a; save(); }
    }
    SimForceRef *r = &s->fr;
    ui_disable(locked);
    row_num(x, &y, w, true, "su-flref", "Reference length", "The chord of a wing, the length of a car.", &r->lRef, 1e-12, 1e9, "m", NULL);
    row_num(x, &y, w, true, "su-faref", "Reference area", twoD ? "2-D: the chord times the mesh's depth, so the coefficients are the usual per-span ones."
                                                               : "The frontal area seen along the flow (a wing's is often its planform area instead).", &r->aRef, 1e-15, 1e12, "m²", NULL);
    bool all = !locked || P.more[SS_REPORTS];              /* worked out from the part: the directions are under More */
    if (all) {
        row_vec(x, &y, w, true, "su-fdrag", "Drag direction", "Along the free stream.", r->drag, NULL);
        row_vec(x, &y, w, true, "su-flift", "Lift direction", "Square to the drag; up for a wing.", r->lift, NULL);
        row_vec(x, &y, w, true, "su-fcofr", "Moment centre", "The pitching moment Cm is about this point: a quarter of the length from the front.", r->CofR, "m");
    }
    ui_disable(false);
    if (all) {
        char a[96]; pl_fmt_ui(a, sizeof a, r->Uinf > 0 ? "" : "0: the inflow speed, %.4g m/s", sim_ref_speed(s));
        row_num(x, &y, w, true, "su-fu", "Reference speed", "The free stream's speed.", &r->Uinf, 0, 1e6, "m/s", a);
    }
    if (twoD) {
        const char *t = "2-D", *d = "Cl and Cd are those of the profile (per unit span); there is no side force. Cm is about the moment centre, nose up positive.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y + 6, w, ch }, TOAST_INFO, ICON_INFO, t, d);
        y += ch + 12;
    }
    if (locked) y += more_toggle(SS_REPORTS, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 4 Starting values */
static float start_step(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    y += note(x, y, w, "What the fluid inside starts as. A steady run forgets them; a good guess only gets there sooner.");
    row_vec(x, &y, w, true, "su-u0", "Velocity", P.external ? "The free stream, for a body in a wind tunnel." : "Usually still (0 0 0) inside a part.", s->U0, "m/s");
    row_num(x, &y, w, true, "su-p0", "Pressure", "Gauge.", &s->p0, -1e9, 1e9, "Pa", NULL);
    if (s->energy != EN_OFF) {
        char c[48]; pl_fmt_ui(c, sizeof c, "%.4g °C", s->T0 - 273.15);
        double was = s->T0;
        if (row_num(x, &y, w, true, "su-T0", "Temperature", "In kelvin. New boundaries start at it too.", &s->T0, 1, 1e4, "K", c)) {
            /* the boundaries still at the old start follow it (they were never set) */
            for (int i = 0; i < s->nB; i++) {
                SimBoundary *b = &s->b[i];
                if (b->T == was) b->T = s->T0;
                if (b->Ta == was) b->Ta = s->T0;
            }
            save();
        }
    }
    if (s->turb == TURB_LAMINAR) return y - y0;
    if (P.more[SS_START]) {
        double k, om, ep, nt;
        sim_turbulence(s, sim_ref_speed(s), s->intensity, s->length, &k, &om, &ep, &nt);
        char a[120];
        if (s->turb == TURB_SPALART) pl_fmt_ui(a, sizeof a, "nuTilda %.3g m²/s", nt);
        else pl_fmt_ui(a, sizeof a, "k %.3g m²/s², %s %.3g", k, s->turb == TURB_KOMEGASST ? "omega" : "epsilon", s->turb == TURB_KOMEGASST ? om : ep);
        row_num(x, &y, w, true, "su-ti", "Turbulence intensity", "Inside at the start, and the default for new inlets.", &s->intensity, 0.01, 100, "%", a);
        row_num(x, &y, w, true, "su-tl", "Mixing length", NULL, &s->length, 1e-9, 1e6, "m", NULL);
    }
    y += more_toggle(SS_START, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 6 Solver */
static float solver_step(float x, float y, float w)
{
    float y0 = y;
    SimSetup *s = &P.s;
    y += note(x, y, w, s->flow == FLOW_STEADY ? TextFormat("%s stops at this many iterations, or sooner once the residuals are under their targets (when that is on).", sim_app(s))
                                              : TextFormat("%s steps through time%s.", sim_app(s), s->algo != ALG_PISO ? "; with an adjustable step it keeps the Courant number under the limit" : ""));
    y += issues_callout(SS_SOLVER, x, y, w);
    if (s->flow == FLOW_STEADY) {
        row_num(x, &y, w, true, "su-it", "Iterations", NULL, &s->iterations, 1, 1e8, NULL, NULL);
        row_num(x, &y, w, true, "su-we", "Write every", "Results are saved this often (iterations).", &s->writeEvery, 1, 1e8, NULL,
                s->writeEvery > s->iterations ? "More than the iterations: saved at the end" : NULL);
        Rectangle rr = prop_row(x, &y, w, "Stop when the residuals are small",
                                "The solver ends the run early, writing that iteration, once every residual is under its target. Off: it runs all the iterations.", 24);
        if (ui_switch(ui_id("su-res"), rr.x, rr.y + 2, &s->resStop)) save();
        if (s->resStop) {
            row_num(x, &y, w, true, "su-resp", "Pressure target", "The p residual to get under; 1e-4 is usual.", &s->resP, 1e-15, 1, NULL, NULL);
            row_num(x, &y, w, true, "su-resu", "Velocity and turbulence target", "U, k, omega ...; 1e-5 is usual.", &s->resRest, 1e-15, 1, NULL, NULL);
        }
        return y - y0;
    }
    row_num(x, &y, w, true, "su-et", "End time", NULL, &s->endTime, 1e-12, 1e12, "s", NULL);
    char a[96] = "";
    if (P.size > 0) pl_fmt_ui(a, sizeof a, "%.3g s for the flow to cross the part once", P.size / sim_ref_speed(s));
    row_num(x, &y, w, true, "su-dt", "Time step", s->algo == ALG_PISO ? "Fixed: choose it for a Courant number under 1 in the smallest cells." : "The first step, when it adjusts.",
            &s->deltaT, 1e-15, 1e9, "s", a);
    if (s->algo == ALG_PISO) {
        prop_row(x, &y, w, "Fixed time step", "pisoFoam keeps the time step above fixed: choose it for a Courant number under 1 in the smallest cells.", 0);
    } else {
        Rectangle cr = prop_row(x, &y, w, "Adjust the time step", "Keeps the Courant number (cells crossed a step) under the limit.", 24);
        if (ui_switch(ui_id("su-adj"), cr.x, cr.y + 2, &s->adjustDt)) save();
        if (s->adjustDt) row_num(x, &y, w, true, "su-co", "Max Courant number", s->algo == ALG_LARGE ? "5 to 10 with large-step PIMPLE." : "1 is safe; up to 2 or 3 with outer correctors.",
                                 &s->maxCo, 0.01, 100, NULL, NULL);
        row_count(x, &y, w, true, "su-nouter", "Outer correctors", s->algo == ALG_LARGE ? "Up to this many per time step; it moves on once the residuals are under the targets below."
                                                                                       : "1 is PISO-like; 2 or 3 steady a larger time step.", &s->nOuter, 1, 100);
    }
    if (s->algo == ALG_LARGE) {
        row_num(x, &y, w, true, "su-oresp", "Pressure target, each step", "The outer correctors stop once p's residual is under this.", &s->resP, 1e-15, 1, NULL, NULL);
        row_num(x, &y, w, true, "su-oresu", "Velocity and turbulence target, each step", NULL, &s->resRest, 1e-15, 1, NULL, NULL);
    }
    row_num(x, &y, w, true, "su-wt", "Write every", "Results are saved this often.", &s->writeTime, 1e-12, 1e12, "s",
            s->writeTime > s->endTime ? "Past the end time: saved at the end" : NULL);
    int ap = sim_approach(s->turb);
    if (ap == APPR_LES || ap == APPR_DES) {
        /* plan step 5: the time averages (fieldAverage) start once the start-up has washed out */
        Rectangle ar = prop_row(x, &y, w, "Averaging starts", "UMean, pMean and UPrime2Mean (fieldAverage) are averaged from then on: after the start-up has "
                                                               "washed out, a few times the flow takes to cross the part.", 34);
        int mode = s->avgStart < 0 ? 0 : 1;
        static const char *AM[] = { "Automatic", "Typed in" };
        float sw = fminf(240, ar.width * 0.5f);
        if (ui_segmented(ui_id("su-avgmode"), (Rectangle){ ar.x, ar.y, sw, 34 }, AM, 2, &mode) && (mode == 1) != (s->avgStart >= 0)) {
            s->avgStart = mode ? 0.4 * s->endTime : -1; save();
        }
        double v = sim_avg_start(s);
        ui_disable(s->avgStart < 0);
        if (num_in(ui_id("su-avg"), (Rectangle){ ar.x + sw + 10, ar.y, fminf(160, ar.width - sw - 10), 34 }, &v, "%.6g", 0, 1e12, "s")) { s->avgStart = v; save(); }
        ui_disable(false);
        double tc = P.size > 0 ? P.size / sim_ref_speed(s) : 0;
        if (tc > 0 && ar.width - sw - 180 > 120) {
            char t[96]; pl_fmt_ui(t, sizeof t, "%.3g crossings in%s", v / tc, s->avgStart < 0 ? ": 40 % of the end time" : "");
            ui_text_clip(F_REG, 12.5f, t, ar.x + sw + 180, ar.y + 9, ar.width - sw - 180, v / tc < 3 ? TH.warn : TH.text3);
        }
    }
    if (P.more[SS_SOLVER]) {
        row_count(x, &y, w, true, "su-ncorr", "Pressure correctors", "2 is usual; 3 for a tighter pressure-velocity coupling each step.", &s->nCorr, 1, 10);
        char autoTs[64]; pl_fmt_ui(autoTs, sizeof autoTs, "Automatic (%s)", TS_NAME[sim_time_scheme(&(SimSetup){ .turb = s->turb, .timeScheme = TS_AUTO })]);
        const char *ts[TS__N] = { autoTs, TS_NAME[TS_EULER], TS_NAME[TS_BACKWARD], TS_NAME[TS_CN] };
        Rectangle cr = prop_row(x, &y, w, "Time scheme", "Euler: first order, robust. Backward: second order, for LES and DES. Crank-Nicolson (0.9): second order, less damping.", 34);
        int tsel = s->timeScheme;
        if (ui_select(ui_id("su-ts"), (Rectangle){ cr.x, cr.y, fminf(260, cr.width), 34 }, ts, TS__N, &tsel) && tsel != s->timeScheme) { s->timeScheme = tsel; save(); }
    }
    y += more_toggle(SS_SOLVER, x, y);
    return y - y0;
}

/* ---------------------------------------------------------------------------- 7 Check */
static float check_step(Case *c, float x, float y, float w, const SsStatus st[SS__N])
{
    float y0 = y;
    int nAll = 0;
    for (int k = 0; k < SS_CHECK; k++) {
        char t[1200];
        int n = step_issues(k, t, sizeof t);
        if (!n) continue;
        nAll += n;
        char go[64]; pl_fmt_ui(go, sizeof go, "Go to %s", SS_TITLE[k]);
        float bw = ui_button_width(ICON_ARROW_RIGHT, go);
        bool side = w - bw - 16 >= 360;
        float cw = side ? w - bw - 16 : w;
        float ch = fmaxf(callout_height(cw, SS_TITLE[k], t), 52);
        callout((Rectangle){ x, y, cw, ch }, st[k].state == SS_ERR ? TOAST_ERR : TOAST_WARN, ICON_TRIANGLE_ALERT, SS_TITLE[k], t);
        Rectangle br = side ? (Rectangle){ x + w - bw, y + (ch - 36) / 2, bw, 36 } : (Rectangle){ x, y + ch + 8, bw, 36 };
        if (ui_button(ui_idn(ui_id("su-check-go"), k), br, ICON_ARROW_RIGHT, go, BTN_SECONDARY)) P.step = k;
        y += ch + (side ? 12 : 56);
    }
    if (!nAll) {
        const char *t = "Nothing to fix", *d = "Every step is set. Write case files, then run.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_OK, ICON_CIRCLE_CHECK, t, d);
        y += ch + 12;
    }
    /* what Q-Continuum chose: the solver, the fields, the files */
    const char *f[SIM_MAX_F]; int nf = sim_fields(&P.s, f);
    char fl[96] = "";
    for (int i = 0; i < nf; i++) snprintf(fl + strlen(fl), sizeof fl - strlen(fl), "%s%s", i ? ", " : "", f[i]);
    y += 6;
    y += section_head(x, y, w, "What will be written", NULL);
    Rectangle cr = prop_row(x, &y, w, "Solver", NULL, 20);
    ui_text_clip(F_MONO, 13.5f, sim_app(&P.s), cr.x, cr.y + 2, cr.width, TH.text);
    cr = prop_row(x, &y, w, "Fields in 0/", NULL, 20);
    ui_text_clip(F_MONO, 13.5f, fl, cr.x, cr.y + 2, cr.width, TH.text);
    const char *files = sim_is_gas(&P.s)
        ? "constant/turbulenceProperties, thermophysicalProperties; system/controlDict, fvSchemes, fvSolution, fvOptions"
        : P.s.energy != EN_OFF
        ? "constant/transportProperties, turbulenceProperties, thermophysicalProperties, g; system/controlDict, fvSchemes, fvSolution"
        : "constant/transportProperties, turbulenceProperties; system/controlDict, fvSchemes, fvSolution";
    float lw = w - fminf(300, w * 0.38f);                    /* prop_row's control column */
    cr = prop_row(x, &y, w, "Files", NULL, ui_text_wrap(F_MONO, 13.5f, files, (Rectangle){ 0, 0, lw, 400 }, 20, TH.text2, false));
    ui_text_wrap(F_MONO, 13.5f, files, (Rectangle){ cr.x, cr.y + 2, cr.width, 400 }, 20, TH.text2, true);
    cr = prop_row(x, &y, w, "Case files", NULL, 20);
    bool stale = files_stale(c);
    ui_text_clip(F_REG, 13.5f, stale ? "Not written since the setup or the mesh changed" : "Written: they match this setup", cr.x, cr.y + 2, cr.width, stale ? TH.text2 : TH.ok);
    y += 10;
    ui_disable(A.cj->active);
    float bw = ui_button_width(ICON_PLAY, "Write and go to Run");
    if (ui_button(ui_id("su-check-run"), (Rectangle){ x, y, bw, 40 }, ICON_PLAY, "Write and go to Run", BTN_PRIMARY)) write_files(c, true);
    ui_disable(false);
    y += 52;
    return y - y0;
}

/* ---------------------------------------------------------------------------- the step list */
/* one step in the list: its mark (number, tick, alert), title and what it holds; a rail item, or a chip when narrow */
static void step_item(int k, Rectangle r, const SsStatus *st, bool rail)
{
    bool open = P.step == k;
    UiId id = ui_idn(ui_id("su-step"), k);
    if (ui_row(id, r, open)) P.step = k;
    ui_register(id, r, SS_TITLE[k]);
    ui_tooltip(r, st->sub);
    float cx = r.x + 20, cy = rail ? r.y + 24 : r.y + r.height / 2, rad = rail ? 13 : 11;
    Color mark = st->state == SS_ERR ? TH.err : st->state == SS_ATTN ? TH.warn : TH.ok;
    DrawCircleV((Vector2){ cx, cy }, rad, open ? TH.accent : ui_alpha(mark, 0.16f));
    DrawCircleLinesV((Vector2){ cx, cy }, rad, open ? TH.accent : mark);
    if (st->state == SS_OK) ui_icon(ICON_CHECK, cx, cy, rail ? 14 : 12, open ? TH.textOnAccent : mark);
    else ui_icon(ICON_TRIANGLE_ALERT, cx, cy - 1, rail ? 13 : 11, open ? TH.textOnAccent : mark);
    float tx = r.x + 42, tw = r.width - 50;
    if (rail) {
        ui_text_clip(F_SEMI, 14, ui_cap(SS_TITLE[k]), tx, r.y + 12, tw, open ? TH.text : TH.text2);
        ui_text_clip(F_REG, 12, st->sub, tx, r.y + 31, tw, st->state == SS_ERR ? TH.err : st->state == SS_ATTN ? TH.warn : open ? TH.text2 : TH.text3);
    } else ui_text_clip(F_SEMI, 13.5f, ui_cap(SS_TITLE[k]), tx, cy - 8, tw, open ? TH.text : TH.text2);
}

float page_setup(float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    sync(c);
    /* what will be written, whether it is, and the buttons */
    bool stale = files_stale(c);
    char t[200], d[400];
    pl_fmt_ui(t, sizeof t, "%s · %s · %s", sim_app(&P.s), TURB_NAME[P.s.turb], sim_is_gas(&P.s) ? TextFormat("%s (ideal gas)", GAS_NAME[P.s.gas]) : FLUID_NAME[P.s.fluid]);
    pl_fmt_ui(d, sizeof d, "%s Write case files puts the setup into 0/, constant/ and system/ (a file that wasn't written here is kept as .orig).",
              stale ? "Changed since the case files were written." : "The case files match this setup.");
    float bw1 = ui_button_width(ICON_FILE_TEXT, "Write case files"), bw2 = ui_button_width(ICON_PLAY, "Write and go to Run");
    bool beside = w >= 980;                                 /* the buttons right of the summary, or under it */
    float cw = beside ? w - bw1 - bw2 - 36 : w;
    float ch = callout_height(cw, t, d);
    callout((Rectangle){ x, y, cw, ch }, stale ? TOAST_INFO : TOAST_OK, stale ? ICON_INFO : ICON_CIRCLE_CHECK, t, d);
    float by = beside ? y + (ch - 40) / 2 : y + ch + 12, bx = beside ? x + cw + 16 : x;
    ui_disable(A.cj->active);
    if (ui_button(ui_id("su-write"), (Rectangle){ bx, by, bw1, 40 }, ICON_FILE_TEXT, "Write case files", BTN_PRIMARY)) write_files(c, false);
    if (ui_button(ui_id("su-write-run"), (Rectangle){ bx + bw1 + 10, by, bw2, 40 }, ICON_PLAY, "Write and go to Run", BTN_SECONDARY)) write_files(c, true);
    ui_disable(false);
    y += beside ? ch + 20 : ch + 12 + 40 + 20;
    /* a new setup: what is being simulated, first */
    /* a case whose files were written before a template was picked (from the step bar) is on its own choices */
    char u[PL_PATH]; pl_join(u, sizeof u, c->s.path, "0/U");
    if ((P.s.tmpl < 0 && !pl_is_file(u)) || P.picking) return y + template_cards(c, x, y, w) + 16 - y0;
    SsStatus st[SS__N];
    ss_status(c, st);
    if (P.step < 0 || P.step >= SS__N) P.step = SS_MODELS;
    bool rail = w >= 900;
    float rw = rail ? fminf(260, fmaxf(210, w * 0.2f)) : 0, cx = rail ? x + rw + 28 : x, cwid = rail ? w - rw - 28 : w;
    float ry = y;
    if (rail) {
        for (int k = 0; k < SS__N; k++) step_item(k, (Rectangle){ x, ry + k * 58, rw, 52 }, &st[k], true);
        for (int k = 0; k + 1 < SS__N; k++) ui_line(x + 20, ry + k * 58 + 38, x + 20, ry + (k + 1) * 58 + 10, 1.5f, TH.lineSoft);
        ry += SS__N * 58;
    } else {
        /* narrow: the steps as chips that wrap */
        float px = x, py = y;
        for (int k = 0; k < SS__N; k++) {
            float iw = ui_text_w(F_SEMI, 13.5f, ui_cap(SS_TITLE[k])) + 56;
            if (px + iw > x + w && px > x) { px = x; py += 44; }
            step_item(k, (Rectangle){ px, py, iw, 38 }, &st[k], false);
            px += iw + 8;
        }
        y = py + 52;
    }
    /* the open step */
    char head[200];
    pl_fmt_ui(head, sizeof head, "%d  %s", P.step + 1, SS_TITLE[P.step]);
    y += section_head(cx, y, cwid, head, NULL);
    switch (P.step) {
    case SS_MODELS:     y += models_step(c, cx, y, cwid); break;
    case SS_MATERIALS:  y += materials_step(cx, y, cwid); break;
    case SS_BOUNDARIES: y += boundaries_step(c, cx, y, cwid); break;
    case SS_START:      y += start_step(cx, y, cwid); break;
    case SS_REPORTS:    y += reports_step(c, cx, y, cwid); break;
    case SS_SOLVER:     y += solver_step(cx, y, cwid); break;
    default:            y += check_step(c, cx, y, cwid, st); break;
    }
    if (P.step + 1 < SS__N) {
        char lab[64]; pl_fmt_ui(lab, sizeof lab, "Next: %s", SS_TITLE[P.step + 1]);
        float nb = ui_button_width(ICON_ARROW_RIGHT, lab);
        if (ui_button(ui_id("su-next"), (Rectangle){ cx, y + 14, nb, 40 }, ICON_ARROW_RIGHT, lab, BTN_PRIMARY)) { P.step++; ui_scroll_to(&A.scr[7], 0); }
        y += 66;
    }
    if (rail && y < ry) y = ry;
    return y + 16 - y0;
}
