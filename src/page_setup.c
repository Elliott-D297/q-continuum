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
#include <ctype.h>
#include "safety.h"
#include "simsetup.h"
#include "foamdict.h"
#include "precheck.h"
#include "page_setup_int.h"


SetupState SU;                                           /* the open case's setup (page_setup_int.h) */

/* a file's modification time to the nanosecond (0: none): a setup edited in the same second as the write still counts */
double su_mtime_ns(const char *p)
{
    struct stat st;
    return stat(p, &st) ? 0 : (double)st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 1e-9;
}

/* ============================================================================ the setup of the open case */
void su_save(void)
{
    if (SU.casePath[0]) sim_save(SU.casePath, &SU.s);
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
    int n = stlmesh_patches(c, p, SIM_MAX_B, &SU.size, &SU.external);
    /* a mesh being generated is half made (blockMesh's background patch, no STL patches yet): its patches aren't the
       case's; the ones from the mesh before stay as they were until it is done */
    if (A.cj->active && !strcmp(A.cj->casePath, c->s.path) && !strcmp(A.cj->title, "Mesh")) {
        for (int i = 0; i < SU.s.nB && n < SIM_MAX_B; i++) {
            const SimBoundary *b = &SU.s.b[i];
            bool have = false;
            for (int k = 0; k < n; k++) if (!strcmp(p[k].name, b->name)) { have = true; p[k].meshType = b->meshType; }
            if (!have) p[n++] = (SimPatch){ b->name, b->role, b->meshType, false, b->area };
        }
        sim_set_boundaries(&SU.s, p, n);
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
    sim_set_boundaries(&SU.s, p, n);
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
    double mt = su_mtime_ns(cd);
    if (mt == SU.cdMt) return;
    SU.cdMt = mt;
    if (!sim_take_controls(c->s.path, &SU.s)) return;
    char ini[PL_PATH], u[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(u, sizeof u, c->s.path, "0/U");
    struct stat st;
    bool had = !stat(ini, &st);
    double uMt = su_mtime_ns(u);
    su_save();
    if (had && uMt && su_mtime_ns(ini) > uMt && st.st_mtim.tv_sec + st.st_mtim.tv_nsec * 1e-9 <= uMt) {
        struct timespec ts[2] = { st.st_mtim, st.st_mtim };     /* as old as it was: still not newer than the files */
        utimensat(AT_FDCWD, ini, ts, 0);
    }
}

/* buoyancy's gravity, while it isn't typed in: down the STL mesher's up direction (kept as it is without an STL) */
void su_gravity_auto(Case *c)
{
    double g[3], lo[3], hi[3], gm;
    SU.height = SU.size;                                      /* the Rayleigh number's length: the height of the mesh */
    if (SU.s.energy == EN_BUOYANT && (gm = sim_g_mag(&SU.s)) > 0 && mesh_bounds(c->s.path, lo, hi)) {
        double h = 0;
        for (int k = 0; k < 3; k++) h += (hi[k] - lo[k]) * fabs(SU.s.g[k]) / gm;
        if (h > 0) SU.height = h;
    }
    if (!(SU.height > 0) && SU.s.energy != EN_OFF && mesh_bounds(c->s.path, lo, hi))   /* no STL: the mesh's largest side */
        for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > SU.height) SU.height = hi[k] - lo[k];
    /* the Nusselt number's length, while it isn't typed in: the same height (heat on only, so other cases' files stay current) */
    if (SU.s.energy != EN_OFF && SU.s.nuAuto && SU.height > 0 && fabs(SU.s.nuLength - SU.height) > 1e-9 * SU.height) { SU.s.nuLength = SU.height; su_save(); }
    if ((SU.s.energy != EN_BUOYANT && !sim_is_vof(&SU.s)) || !SU.s.gAuto || !stlmesh_gravity(c, g) || !memcmp(g, SU.s.g, sizeof g)) return;
    memcpy(SU.s.g, g, sizeof g);
    su_save();
}

void su_sync(Case *c)
{
    static double at = -1;
    if (!strcmp(SU.casePath, c->s.path) && at == ui_time()) return;   /* once a frame */
    at = ui_time();
    if (strcmp(SU.casePath, c->s.path)) {
        pl_strlcpy(SU.casePath, c->s.path, sizeof SU.casePath);
        SU.size = 0; SU.external = false; SU.cdMt = 0;
        SU.step = 0; SU.picking = false; memset(SU.more, 0, sizeof SU.more);
        SimPatch dummy[1];
        stlmesh_patches(c, dummy, 0, &SU.size, &SU.external);
        bool had = sim_load(c->s.path, &SU.s, SU.size);
        refresh(c);
        if (!had && SU.external)                             /* a wind tunnel: the air starts at the inlet's speed */
            for (int i = 0; i < SU.s.nB; i++) if (!strcmp(SU.s.b[i].name, "inlet") && SU.s.b[i].role == ROLE_INLET) free_stream(c, SU.s.b[i].speed, SU.s.U0);
        if (had) take_controls(c);
        return;
    }
    refresh(c);
    take_controls(c);
    su_gravity_auto(c);
}

void setup_flow_changed(Case *c)
{
    su_sync(c);
    if (!SU.external) return;
    SimBoundary *in = sim_find(&SU.s, "inlet");             /* the tunnel's inlet sits on the other side now: the start turns too */
    if (in && in->role == ROLE_INLET) { free_stream(c, in->inlet == IN_SPEED ? in->speed : sim_ref_speed(&SU.s), SU.s.U0); su_save(); }
}

void setup_rename(Case *c, const char *from, const char *to)
{
    su_sync(c);
    sim_rename(&SU.s, from, to);
    su_save();
}

/* ============================================================================ fields */
bool su_num_in(UiId id, Rectangle r, double *v, const char *fmt, double lo, double hi, const char *unit)
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

bool su_row_num(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, double *v, double lo, double hi,
                    const char *unit, const char *after)
{
    Rectangle cr = prop_row(x, y, w, label, hints ? hint : NULL, 34);
    float fw = fminf(180, cr.width);
    bool ch = su_num_in(ui_id(id), (Rectangle){ cr.x, cr.y, fw, 34 }, v, "%.6g", lo, hi, unit);
    if (after && *after) ui_text_clip(F_REG, 13, after, cr.x + fw + 12, cr.y + 9, cr.width - fw - 12, TH.text3);
    if (ch) su_save();
    return ch;
}

bool su_row_vec(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, double v[3], const char *unit)
{
    Rectangle cr = prop_row(x, y, w, label, hints ? hint : NULL, 34);
    static const char *const AX[3] = { "x", "y", "z" };
    float fw = fminf(120, (cr.width - 16) / 3);
    bool ch = false;
    for (int k = 0; k < 3; k++) {
        char u[16]; snprintf(u, sizeof u, "%s %s", AX[k], k == 2 && unit ? unit : "");
        if (su_num_in(ui_idn(ui_id(id), k), (Rectangle){ cr.x + k * (fw + 8), cr.y, fw, 34 }, &v[k], "%.6g", -1e9, 1e9, u)) ch = true;
    }
    if (ch) su_save();
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
float su_note(float x, float y, float w, const char *t);

float setup_boundary_editor(Case *c, const char *name, float x, float y, float w)
{
    su_sync(c);
    float y0 = y;
    SimBoundary *b = sim_find(&SU.s, name);
    if (!b) return 0;
    bool hints = w > 620, turb = SU.s.turb != TURB_LAMINAR, heat = SU.s.energy != EN_OFF, gas = sim_is_gas(&SU.s);
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
        if (ui_select(ui_id("in-kind"), (Rectangle){ cr.x, cr.y, fminf(240, cr.width), 34 }, INLET_NAME, gas ? IN__N : IN_MACH, &k) && k != b->inlet) { b->inlet = k; su_save(); }
        if (sim_is_vof(&SU.s)) {
            Rectangle pr = prop_row(x, &y, w, "Brings in", hints ? "Water, air, or either (whatever is next to it: zero gradient). A pressure inlet "
                                                                   "lets air back out if the flow turns round." : NULL, 34);
            int ph = b->phaseIn;
            if (ui_segmented(ui_id("in-phase"), (Rectangle){ pr.x, pr.y, fminf(300, pr.width), 34 }, (const char **)PHASEIN_NAME, PI__N, &ph) && ph != b->phaseIn) { b->phaseIn = ph; su_save(); }
        }
        char ma[96] = "";
        if (gas) { double a0 = sim_sound_speed(&SU.s, b->T); if (a0 > 0 && b->inlet == IN_SPEED) pl_fmt_ui(ma, sizeof ma, "Ma %.3g", b->speed / a0); }
        if (b->inlet == IN_MACH && gas) {
            double a0 = sim_sound_speed(&SU.s, b->T);
            pl_fmt_ui(a, sizeof a, "%.4g m/s at %.4g K", b->mach * a0, b->T);
            su_row_num(x, &y, w, hints, "in-mach", "Mach number", "The speed over the speed of sound at the inlet's temperature, straight in.", &b->mach, 0, 50, NULL, a);
        } else if (b->inlet == IN_SPEED) {
            double was = b->speed;
            if (su_row_num(x, &y, w, hints, "in-speed", "Speed", "Into the domain, straight through the boundary.", &b->speed, 0, 1e5, "m/s", ma)
                && SU.external && !strcmp(b->name, "inlet")) {
                double w0[3]; free_stream(c, was, w0);          /* the start was the free stream: it follows the inlet */
                if (SU.s.U0[0] == w0[0] && SU.s.U0[1] == w0[1] && SU.s.U0[2] == w0[2]) { free_stream(c, b->speed, SU.s.U0); su_save(); }
            }
        } else if (b->inlet == IN_VECTOR)
            su_row_vec(x, &y, w, hints, "in-vec", "Velocity", "In the mesh's axes.", b->vec, "m/s");
        else if (b->inlet == IN_FLOW) {
            fmt_flow(a, sizeof a, b->area > 0 ? b->flow / b->area : 0);
            su_row_num(x, &y, w, hints, "in-flow", "Volume flow rate", "Spread evenly over the boundary.", &b->flow, 0, 1e9, "m³/s", a);
        } else if (b->inlet == IN_MASS) {
            fmt_flow(a, sizeof a, b->area > 0 ? b->mdot / (SU.s.rho * b->area) : 0);
            su_row_num(x, &y, w, hints, "in-mdot", "Mass flow rate", "At the fluid's density.", &b->mdot, 0, 1e9, "kg/s", a);
        } else if (gas) {
            pl_fmt_ui(a, sizeof a, "%.6g Pa absolute", sim_pabs(&SU.s, b->p));
            su_row_num(x, &y, w, hints, "in-p", "Total pressure", "Gauge, above the operating pressure: the gas starts from rest at it and speeds up to the outlet's "
                                                             "pressure (isentropically).", &b->p, -1e9, 1e9, "Pa", a);
        } else
            su_row_num(x, &y, w, hints, "in-p", "Total pressure", "Gauge (above the outlet's): the flow comes in as fast as this pushes it.", &b->p, -1e9, 1e9, "Pa", NULL);
        if (turb) {
            /* plan step 5: by an intensity and a mixing length, a viscosity ratio, or a pipe's diameter */
            Rectangle tr = prop_row(x, &y, w, "Turbulence by", hints ? "What you know about the turbulence coming in. Not sure: a pipe or duct by its diameter, "
                                                                       "anything else by intensity and length." : NULL, 34);
            int ts = b->turbSpec;
            if (ui_select(ui_id("in-tspec"), (Rectangle){ tr.x, tr.y, fminf(240, tr.width), 34 }, TI_NAME, TI__N, &ts) && ts != b->turbSpec) { b->turbSpec = ts; su_save(); }
            double Ub = b->inlet == IN_SPEED ? fabs(b->speed) : sim_ref_speed(&SU.s), I, Lm;
            sim_inlet_turbulence(&SU.s, b, Ub, &I, &Lm);
            if (b->turbSpec != TI_DIAMETER)
                su_row_num(x, &y, w, hints, "in-ti", "Turbulence intensity", "1 % is calm (a wind tunnel), 5 % a duct, 10 % or more behind a fan.", &b->intensity, 0.01, 100, "%", NULL);
            if (b->turbSpec == TI_LENGTH)
                su_row_num(x, &y, w, hints, "in-tl", "Mixing length", "About 7 % of the duct's width, or of the body for a wind tunnel.", &b->length, 1e-9, 1e6, "m", NULL);
            else if (b->turbSpec == TI_RATIO) {
                pl_fmt_ui(a, sizeof a, "mixing length %s", fmt_len(Lm));
                su_row_num(x, &y, w, hints, "in-ratio", "Viscosity ratio", "nut / nu: 1 to 10 in calm or external flow, 10 to 100 in ducts, more behind a fan.", &b->ratio, 1e-6, 1e8, NULL, a);
            } else {
                pl_fmt_ui(a, sizeof a, "%s%.3g %%, mixing length %s", b->dh > 0 ? "" : TextFormat("0: from its area. "), I, fmt_len(Lm));
                su_row_num(x, &y, w, hints, "in-dh", "Hydraulic diameter", "4 x area / perimeter: a round pipe's diameter. The intensity follows from its Reynolds number "
                                                                        "(0.16 Re^-1/8), the length is 7 % of it.", &b->dh, 0, 1e6, "m", a);
            }
        }
        if (heat && gas && b->inlet == IN_PRESSURE)
            su_row_num(x, &y, w, hints, "in-T", "Total temperature", "Of the gas at rest before it speeds up, in kelvin; it cools as it does.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        else if (heat) su_row_num(x, &y, w, hints, "in-T", "Temperature", "Of the fluid coming in, in kelvin.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        break;
    }
    case ROLE_OUTLET:
        if (gas) pl_fmt_ui(a, sizeof a, "%.6g Pa absolute", sim_pabs(&SU.s, b->p));
        su_row_num(x, &y, w, hints, "out-p", "Pressure", gas ? "Static, gauge: 0 means the operating pressure. What flows back in comes in still."
                                                       : "Static, gauge: 0 means the surroundings. What flows back in comes in still.", &b->p, -1e9, 1e9, "Pa", gas ? a : NULL);
        if (heat) su_row_num(x, &y, w, hints, "out-T", "Backflow temperature", "Of anything that flows back in through it.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        if (gas) {
            /* a transient run's pressure waves leave instead of bouncing back off a fixed pressure (waveTransmissive) */
            Rectangle nr = prop_row(x, &y, w, "Non-reflecting", hints ? "Pressure waves leave through it instead of bouncing back in. For transient runs; the pressure "
                                                                        "tends to the one above." : NULL, 24);
            bool steady = SU.s.flow == FLOW_STEADY, on = b->nonRefl && !steady;
            ui_disable(steady);
            if (ui_switch(ui_id("out-nonrefl"), nr.x, nr.y + 2, &on)) { b->nonRefl = on; su_save(); }
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
        bool can = (gas || !heat) && !sim_is_vof(&SU.s);
        if (sim_is_vof(&SU.s)) y += su_note(x, y + 2, w, "The atmosphere: water and air leave freely, air comes back in where the flow turns inward, "
                                                     "at the total pressure below (gauge).") + 6;
        if (can) {
            Rectangle kr = prop_row(x, &y, w, "Opening to", hints ? "Surroundings: still air beyond it (a window, a room's open side). Free stream: the flow far from a "
                                                                    "body, coming in where it blows in and leaving elsewhere (a far field round a wing)." : NULL, 34);
            static const char *OK[] = { "Surroundings", "Free stream" };
            int fk = b->farField ? 1 : 0;
            if (ui_segmented(ui_id("op-kind"), (Rectangle){ kr.x, kr.y, fminf(300, kr.width), 34 }, OK, 2, &fk) && (fk == 1) != b->farField) {
                b->farField = fk == 1;
                if (b->farField && !vlen3(b->vec)) memcpy(b->vec, SU.s.U0, sizeof b->vec);   /* the start is usually the free stream */
                su_save();
            }
        }
        if (can && b->farField) {
            a[0] = 0;
            if (gas) { double a0 = sim_sound_speed(&SU.s, b->T); if (a0 > 0) pl_fmt_ui(a, sizeof a, "Free-stream Mach number %.3g", vlen3(b->vec) / a0); }
            su_row_vec(x, &y, w, hints, "op-u", "Free-stream velocity", "In the mesh's axes; usually the starting velocity too.", b->vec, "m/s");
            if (gas && a[0] && vlen3(b->vec) > 0) y += su_note(x, y + 2, w, a) - 4;
            su_row_num(x, &y, w, hints, "op-p", "Free-stream pressure", "Static, gauge.", &b->p, -1e9, 1e9, "Pa", NULL);
            if (heat) su_row_num(x, &y, w, hints, "op-T", "Free-stream temperature", "Static, in kelvin.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
            if (turb) {
                su_row_num(x, &y, w, hints, "op-ti", "Turbulence intensity", "0.1 % or less in free air.", &b->intensity, 0.001, 100, "%", NULL);
                su_row_num(x, &y, w, hints, "op-tl", "Mixing length", NULL, &b->length, 1e-9, 1e6, "m", NULL);
            }
            break;
        }
        su_row_num(x, &y, w, hints, "op-p", "Surrounding pressure", "Total, gauge. The flow goes in or out through it as it likes (a window, the open sides of a room).", &b->p, -1e9, 1e9, "Pa", NULL);
        if (heat) su_row_num(x, &y, w, hints, "op-T", "Surrounding temperature", gas ? "Total: of the gas at rest beyond it." : "Of the fluid that comes in through it.", &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
        break;
    }
    case ROLE_SYMMETRY:
        y += ui_text_wrap(F_REG, 13, "Nothing to set: the flow slides along it and nothing crosses it (a mirror plane, or the far sides of a wind tunnel).",
                          (Rectangle){ x, y + 4, w, 200 }, 19, TH.text3, true) + 14;
        break;
    default: {
        Rectangle cr = prop_row(x, &y, w, "Wall", hints ? "No slip: the fluid sticks to it. Slip: it slides along without friction. Moving: a belt, a lid, a road under a car." : NULL, 34);
        int k = b->wall;
        if (ui_segmented(ui_id("wall-kind"), (Rectangle){ cr.x, cr.y, fminf(300, cr.width), 34 }, (const char **)WALL_NAME, WL__N, &k) && k != b->wall) { b->wall = k; su_save(); }
        if (b->wall == WL_MOVING) su_row_vec(x, &y, w, hints, "wall-u", "Wall velocity", "Along the wall.", b->wallU, "m/s");
        if (heat) {
            static const char *const heatHint[HT__N] = {
                "No heat crosses it: an insulated wall.",
                "The wall is held at a temperature (a heater with a thermostat, ice).",
                "Heat goes in at this rate per square metre, evenly: negative cools.",
                "Heat goes in at this total rate, spread over the wall: negative cools.",
                "Heat passes to or from surroundings at a temperature through a coefficient h (outside air: 5 to 25 W/(m²·K))." };
            Rectangle hr = prop_row(x, &y, w, "Heat", hints ? heatHint[b->heat] : NULL, 34);
            int hk = b->heat;
            if (ui_select(ui_id("wall-heat"), (Rectangle){ hr.x, hr.y, fminf(240, hr.width), 34 }, HEAT_NAME, HT__N, &hk) && hk != b->heat) { b->heat = hk; su_save(); }
            if (b->heat == HT_FIXED) su_row_num(x, &y, w, hints, "wall-T", "Temperature", NULL, &b->T, 1, 1e4, "K", celsius(a, sizeof a, b->T));
            else if (b->heat == HT_FLUX) {
                if (b->area > 0) pl_fmt_ui(a, sizeof a, "%.4g W over its %.3g m²", b->q * b->area, b->area); else a[0] = 0;
                su_row_num(x, &y, w, hints, "wall-q", "Heat flux", "Into the fluid.", &b->q, -1e9, 1e9, "W/m²", a);
            } else if (b->heat == HT_POWER) {
                if (b->area > 0) pl_fmt_ui(a, sizeof a, "%.4g W/m² over its %.3g m²", b->Q / b->area, b->area); else a[0] = 0;
                su_row_num(x, &y, w, hints, "wall-Q", "Heat rate", "Into the fluid, the whole wall.", &b->Q, -1e12, 1e12, "W", a);
            } else if (b->heat == HT_CONVECT) {
                su_row_num(x, &y, w, hints, "wall-h", "Heat transfer coefficient", NULL, &b->h, 1e-6, 1e9, "W/(m²·K)", NULL);
                su_row_num(x, &y, w, hints, "wall-Ta", "Surrounding temperature", NULL, &b->Ta, 1, 1e4, "K", celsius(a, sizeof a, b->Ta));
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
    su_sync(c);
    return &SU.s;
}

/* ============================================================================ the walls' y+ after a run (plan step 5) */
int setup_yplus_advice(Case *c, double avg, double max, char *out, size_t cap)
{
    su_sync(c);
    out[0] = 0;
    if (SU.s.turb == TURB_LAMINAR || !(avg > 0)) return -1;
    int wt = sim_wall_treatment(&SU.s);
    bool canFn = !sim_wall_why_not(&SU.s, WT_FUNCTIONS), canRes = !sim_wall_why_not(&SU.s, WT_RESOLVED);
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
bool su_files_stale(Case *c)
{
    char ini[PL_PATH], u[PL_PATH], faces[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(u, sizeof u, c->s.path, "0/U");
    pl_join(faces, sizeof faces, c->s.path, "constant/polyMesh/faces");
    double uMt = su_mtime_ns(u), simMt = su_mtime_ns(ini);
    /* an adopted case (W2): its mesh was made after its 0/ files (a tutorial's blockMesh), which is no reason to write; a mesh
       made after it was read into the setup is */
    char ad[PL_PATH]; pl_join(ad, sizeof ad, c->s.path, ".qc-setup/adopted");
    double meshRef = fmax(uMt, su_mtime_ns(ad));
    return !uMt || !simMt || simMt > uMt || su_mtime_ns(faces) > meshRef;
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
    for (int i = 0; i < SU.s.nB; i++) {
        SimBoundary *sb = &SU.s.b[i];
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

bool su_write_files(Case *c, bool thenRun)
{
    if (case_dirty_count(c) > 0) { ui_toast(TOAST_ERR, "Save or undo your unsaved edits first: Write case files rewrites the files they are in"); return false; }
    if (A.cj->active || app_job_running()) { ui_toast(TOAST_ERR, "Wait for the case's job to finish first"); return false; }
    if (A.set.history) hist_commit(c->s.path, "Before the simulation setup was written");
    /* simulation.ini first: an untouched setup (all defaults) has none yet, and the case files are up to date only when
       they are newer than it */
    if (!sim_save(c->s.path, &SU.s)) { ui_toast(TOAST_ERR, "Could not write simulation.ini"); return false; }
    int nt = match_patch_types(c);
    char err[300];
    bool ok = sim_write(c->s.path, &SU.s, err, sizeof err);
    for (int i = 0; SIM_FILES[i]; i++) case_drop_file(c, SIM_FILES[i]);
    case_rescan(c);
    if (!ok) { ui_toast(TOAST_ERR, "%s", err); return false; }
    if (A.set.history) { char m[200]; snprintf(m, sizeof m, "Simulation setup: %s, %s", sim_app(&SU.s), TURB_NAME[SU.s.turb]); hist_commit(c->s.path, m); history_invalidate(); }
    const char *f[SIM_MAX_F]; int nf = sim_fields(&SU.s, f);
    char list[96] = "";
    for (int i = 0; i < nf; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? " " : "", f[i]);
    ui_toast(TOAST_OK, "Wrote 0/ (%s), constant/ and system/ for %s%s%s", list, sim_app(&SU.s), c->s.meshed ? "" : ". Generate the mesh before running",
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

void setup_open_step(int step)
{
    if (A.cs) su_sync(A.cs);
    if (step >= 0 && step < SS__N) SU.step = step;
    A.physicsTab = 2;
    app_go(PG_PHYSICS);
}

/* a run of the setup's that diverged (plan step 9): Robust numerics offered with one click, while the setup's aren't Robust */
bool setup_robust_offer(Case *c)
{
    if (!c || c->s.nRegions > 0) return false;
    char ini[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini);
    if (!pl_is_file(ini)) return false;
    su_sync(c);
    return SU.s.numerics != NUM_ROBUST && SU.s.nB > 0;
}

/* a steady run of the setup's that should stop on its residuals: one that ran all its iterations didn't converge */
bool setup_expects_convergence(Case *c)
{
    if (!c || c->s.nRegions > 0) return false;
    char ini[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini);
    if (!pl_is_file(ini)) return false;
    su_sync(c);
    return SU.s.flow == FLOW_STEADY && SU.s.resStop;
}

bool setup_go_robust(Case *c)
{
    if (!setup_robust_offer(c)) return false;
    int was = SU.s.numerics;
    SU.s.numerics = NUM_ROBUST;
    su_save();
    if (!su_write_files(c, false)) { SU.s.numerics = was; su_save(); return false; }
    ui_toast(TOAST_OK, "Robust numerics: first-order upwind, firmly relaxed%s. Once it runs, Balanced from its results sharpens it",
             SU.s.flow == FLOW_STEADY ? " SIMPLE" : ", Euler in time");
    return true;
}

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
    su_sync(c);
    refresh(c);                                            /* sync runs once a frame: a mesh just made may be newer than it */
    if (!SU.s.nB || !su_files_stale(c)) return false;
    /* a write that failed (and said why) isn't tried again until the setup or the mesh changes */
    static char failedCase[PL_PATH]; static double failedAt;
    char faces[PL_PATH]; pl_join(faces, sizeof faces, c->s.path, "constant/polyMesh/faces");
    double at = su_mtime_ns(ini) + su_mtime_ns(faces);
    if (!strcmp(failedCase, c->s.path) && at == failedAt) return false;
    if (su_write_files(c, false)) { failedCase[0] = 0; return true; }
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
    su_sync(c);
    refresh(c);                                            /* sync runs once a frame: this one's may be from before the mesh */
    return SU.s.nB > 0 && su_write_files(c, false);
}

/* Boundary conditions with no field files: can the setup write them (a case it owns, or one it can start owning: any mesh
   with patches and no fields), and do it */
bool setup_can_write_fields(Case *c)
{
    return c && c->s.meshed && c->s.nRegions == 0 && !c->s.prog && c->fields.n == 0 && c->nPatches > 0 && c->patchSource == 1;
}

/* ============================================================================ adoption (workflow plan W2, 2026-11-14)
 * A case set up by hand (a tutorial, an opened folder) read into the setup: sim_adopt from its files, simulation.ini saved,
 * the merge bases recorded (sim_adopt_bases), so Physics > Setup shows it in plain terms and the next Write case files
 * changes only what was changed here. What Setup couldn't name is listed (.qc-setup/adopt-notes.txt) and stays as written. */
static bool case_adopted(const Case *c)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, ".qc-setup/adopted");
    return pl_is_file(p);
}

bool setup_owns(Case *c)
{
    if (!c || c->s.prog || c->s.nRegions > 0) return false;
    char ini[PL_PATH], msh[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(msh, sizeof msh, c->s.path, "mesher/mesher.ini");
    return pl_is_file(ini) || pl_is_file(msh);
}

bool setup_can_adopt(Case *c)
{
    if (!c || c->s.prog || c->s.nRegions > 0 || !c->s.meshed || c->fields.n == 0 || c->patchSource != 1) return false;
    char ini[PL_PATH], msh[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(msh, sizeof msh, c->s.path, "mesher/mesher.ini");
    if (pl_is_file(ini) || pl_is_file(msh) || wf_imported(c)) return false;
    static const char *const OK[] = { "simpleFoam", "pimpleFoam", "pisoFoam", "icoFoam", NULL };
    for (int i = 0; OK[i]; i++) if (!strcmp(c->s.app, OK[i])) return true;
    return false;
}

const char *setup_adopt_why_not(Case *c)
{
    static char b[200];
    if (!c || c->s.prog || c->s.nRegions > 0) return "Setup writes single-region OpenFOAM cases";
    if (!c->s.meshed || c->patchSource != 1) return "Generate the mesh first: Setup's boundaries are the mesh's patches";
    if (c->fields.n == 0) return "The case has no field files to read the boundaries from";
    snprintf(b, sizeof b, "Setup reads incompressible single-phase cases for now (simpleFoam, pimpleFoam, pisoFoam, icoFoam); this one runs %s",
             c->s.app[0] ? c->s.app : "no solver");
    return setup_can_adopt(c) ? NULL : b;
}

bool setup_adopt(Case *c)
{
    if (!setup_can_adopt(c)) { const char *w = setup_adopt_why_not(c); ui_toast(TOAST_ERR, "%s", w ? w : "Setup can't read this case"); return false; }
    if (case_dirty_count(c) > 0) { ui_toast(TOAST_ERR, "Save or undo your unsaved edits first"); return false; }
    if (A.cj->active || app_job_running()) { ui_toast(TOAST_ERR, "Wait for the case's job to finish first"); return false; }
    su_sync(c);
    refresh(c);
    char notes[4000], err[300] = "";
    if (!sim_adopt(c->s.path, c->fieldDir, &SU.s, notes, sizeof notes)) { ui_toast(TOAST_ERR, "%s", notes); return false; }
    if (A.set.history) hist_commit(c->s.path, "Before Physics > Setup read the case");
    if (!sim_save(c->s.path, &SU.s) || !sim_adopt_bases(c->s.path, &SU.s, err, sizeof err)) {
        ui_toast(TOAST_ERR, "Could not record the setup: %s", err[0] ? err : "simulation.ini");
        char ini[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini); remove(ini);
        return false;
    }
    char np[PL_PATH]; pl_join(np, sizeof np, c->s.path, ".qc-setup/adopt-notes.txt");
    pl_write_file(np, notes, strlen(notes));
    /* the case files are what the setup now says: simulation.ini no newer than them (else the bar asks to write again) */
    char ini[PL_PATH], u[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini);
    pl_join(u, sizeof u, c->s.path, case_rpath(c, "0/U"));
    if (!pl_is_file(u)) pl_join(u, sizeof u, c->s.path, "0/U");
    struct stat st;
    if (!stat(u, &st)) { struct timespec ts[2] = { st.st_mtim, st.st_mtim }; utimensat(AT_FDCWD, ini, ts, 0); }
    SU.cdMt = 0;
    if (A.set.history) { hist_commit(c->s.path, "Physics > Setup read the case"); history_invalidate(); }
    SU.step = SS_MODELS; SU.picking = false;
    A.physicsTab = 2;
    int nk = 0; for (const char *q = notes; *q; q++) nk += *q == '\n';
    ui_toast(TOAST_OK, "Setup read the case: %s, %s, %d boundaries. Writing case files changes only what you change here%s", sim_app(&SU.s),
             TURB_NAME[SU.s.turb], SU.s.nB, nk ? TextFormat("; %d thing%s it couldn't name stay as written", nk, nk == 1 ? "" : "s") : "");
    return true;
}

/* a function object the setup wrote as a report: what it measures and in what (Results, workflow plan W5) */
bool setup_report_info(Case *c, const char *name, char *desc, int dcap, char *unit, int ucap)
{
    if (!setup_owns(c)) return false;
    su_sync(c);
    for (int i = 0; i < SU.s.nR; i++) {
        const SimReport *r = &SU.s.r[i];
        if (strcmp(r->name, name)) continue;
        sim_report_describe(&SU.s, r, desc, dcap);
        pl_strlcpy(unit, sim_report_unit(&SU.s, r), (size_t)ucap);
        return true;
    }
    return false;
}

/* workflow plan W4: the run's length in flow-throughs (the domain's length along the flow over the inflow speed), and the
   numerics preset set from the Numerics page */
bool setup_flow_through(Case *c, double *L, double *U)
{
    if (!setup_owns(c)) return false;
    su_sync(c);
    double u = sim_ref_speed(&SU.s), lo[3], hi[3];
    if (!(u > 0) || !mesh_bounds(c->s.path, lo, hi)) return false;
    double d[3] = { 0, 0, 0 };
    char mi[PL_PATH]; pl_join(mi, sizeof mi, c->s.path, "mesher/mesher.ini");
    if (pl_is_file(mi)) stlmesh_flow(c, d);
    else for (int i = 0; i < SU.s.nB; i++) {                 /* the first inlet's velocity, else the start's */
        const SimBoundary *b = &SU.s.b[i];
        if (b->role == ROLE_INLET && b->inlet == IN_VECTOR) { memcpy(d, b->vec, sizeof d); break; }
    }
    double m = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(m > 0)) { memcpy(d, SU.s.U0, sizeof d); m = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); }
    double len = 0;
    if (m > 0) for (int k = 0; k < 3; k++) len += fabs(hi[k] - lo[k]) * fabs(d[k]) / m;
    else for (int k = 0; k < 3; k++) len = fmax(len, hi[k] - lo[k]);
    if (!(len > 0)) return false;
    *L = len; *U = u;
    return true;
}
/* W6: the Reynolds number Setup works with, and the length it used (the part, else the mesh's narrowest real side: a
   channel's height, not a 2-D case's one-cell thickness); 0 unknown. *laminar: the model is laminar */
double setup_reynolds(Case *c, double *len, bool *laminar)
{
    if (!setup_owns(c)) return 0;
    su_sync(c);
    *laminar = SU.s.turb == TURB_LAMINAR;
    double L = SU.size;
    if (!(L > 0)) {
        double lo[3], hi[3];
        if (!mesh_bounds(c->s.path, lo, hi)) return 0;
        double mx = 0; for (int k = 0; k < 3; k++) mx = fmax(mx, hi[k] - lo[k]);
        L = mx;
        for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > 0.05 * mx && hi[k] - lo[k] < L) L = hi[k] - lo[k];
    }
    double U = sim_ref_speed(&SU.s);
    if (!(L > 0) || !(U > 0) || !(SU.s.nu > 0)) return 0;
    *len = L;
    return U * L / SU.s.nu;
}

int setup_numerics(Case *c) { if (!setup_owns(c)) return -1; su_sync(c); return SU.s.numerics; }
void setup_set_numerics(Case *c, int nm)
{
    if (!setup_owns(c) || nm < 0 || nm >= NUM__N) return;
    su_sync(c);
    if (SU.s.numerics == nm) return;
    SU.s.numerics = nm; su_save();
}

/* the notes of an adopted case, cached by the file's time */
static const char *adopt_notes(const Case *c)
{
    static char path[PL_PATH], text[4000]; static time_t at;
    char np[PL_PATH]; pl_join(np, sizeof np, c->s.path, ".qc-setup/adopt-notes.txt");
    time_t mt = pl_mtime(np);
    if (strcmp(path, np) || mt != at) {
        pl_strlcpy(path, np, sizeof path); at = mt; text[0] = 0;
        char *t = mt ? pl_read_file(np, NULL) : NULL;
        if (t) { pl_strlcpy(text, t, sizeof text); free(t); }
    }
    return text;
}

bool setup_write_fields(Case *c)
{
    if (!setup_can_write_fields(c)) return false;
    su_sync(c);
    refresh(c);
    if (!SU.s.nB) { ui_toast(TOAST_ERR, "The mesh has no patches to write boundary conditions for"); return false; }
    return su_write_files(c, false);
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
    double iniMt = su_mtime_ns(ini), simMt = su_mtime_ns(sim), meshMt = su_mtime_ns(faces), uMt = su_mtime_ns(u);
    bool here = A.cj->active && !strcmp(A.cj->casePath, c->s.path), meshing = here && !strcmp(A.cj->title, "Mesh");
    bool imported = wf_imported(c);                        /* a mesh imported from SALOME or Gmsh, not one made from an STL */
    /* 1 the STL (or the imported mesh file) */
    int nStl = 0, nIn = 0, nOut = 0, nWall = 0;
    for (int i = 0; i < SU.s.nB; i++) {
        const SimBoundary *b = &SU.s.b[i];
        if (b->fromStl) nStl++;
        if (sim_constraint(b->meshType)) continue;
        if (b->role == ROLE_INLET) nIn++;
        else if (b->role == ROLE_OUTLET || b->role == ROLE_OPENING) nOut++;
        else if (b->role == ROLE_WALL) nWall++;
    }
    if (imported) { st[WF_STL].state = WF_DONE; pl_fmt_ui(st[WF_STL].sub, sizeof st[WF_STL].sub, "%d patch%s", SU.s.nB, SU.s.nB == 1 ? "" : "es"); nStl = SU.s.nB; }
    else if (nStl) { st[WF_STL].state = WF_DONE; pl_fmt_ui(st[WF_STL].sub, sizeof st[WF_STL].sub, "%d boundar%s%s", nStl, nStl == 1 ? "y" : "ies", SU.external ? ", wind tunnel" : ""); }
    else pl_strlcpy(st[WF_STL].sub, "Choose the part", sizeof st[WF_STL].sub);
    /* 2 the boundaries: something brings the flow in and lets it out, and nothing the checks complain about */
    char warn[2000];
    int nw = sim_check(&SU.s, warn, sizeof warn);
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
    double sigMt = su_mtime_ns(sig);
    bool unfinished = sigMt > 0 && su_mtime_ns(cml) < sigMt;     /* Generate ends with checkMesh: a failed one never got there */
    if (unfinished && imported && !su_mtime_ns(cml) && meshMt > sigMt) unfinished = false;   /* converted after the import began: its logs were cleaned */
    if (unfinished && !imported && !su_mtime_ns(cml) && meshMt > sigMt) {
        /* no log.checkMesh at all: a start over before the fix put the mesh's logs in the trash. A mesh made after the
           settings that has every boundary of the STL got there (a failed snappy leaves only blockMesh's patches) */
        bool all = nStl > 0;
        for (int i = 0; i < SU.s.nB && all; i++) if (SU.s.b[i].fromStl && !has_patch(c, SU.s.b[i].name)) all = false;
        if (all) unfinished = false;
    }
    if (meshing) { st[WF_MESH].state = WF_BUSY; pl_fmt_ui(st[WF_MESH].sub, sizeof st[WF_MESH].sub, "%s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label); }
    else if (!meshMt) pl_strlcpy(st[WF_MESH].sub, imported ? "Not imported yet" : "Not generated yet", sizeof st[WF_MESH].sub);
    else if (!imported && !SU.external && has_patch(c, "background")) {
        /* snappy keeps the cells on the wrong side of a gap: blockMesh's box is still there, around the part */
        st[WF_MESH].state = WF_STALE; pl_strlcpy(st[WF_MESH].sub, "Leaked: the STL has a gap", sizeof st[WF_MESH].sub);
    }
    else if (unfinished) { st[WF_MESH].state = WF_STALE; pl_strlcpy(st[WF_MESH].sub, imported ? "Not finished: import again" : "Not finished: generate again", sizeof st[WF_MESH].sub); }
    else if (cur == 0 || (cur < 0 && iniMt > meshMt)) { st[WF_MESH].state = WF_STALE; pl_strlcpy(st[WF_MESH].sub, "Settings changed: generate again", sizeof st[WF_MESH].sub); }
    else {
        st[WF_MESH].state = WF_DONE;
        int lc = logset_find(&A.logs, "log.checkMesh");
        bool failed = lc >= 0 && A.logs.logs[lc].meshFailed && !app_mesh_verdict(c)->boundarySkewOnly;
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
    else { st[WF_PHYSICS].state = WF_DONE; pl_fmt_ui(st[WF_PHYSICS].sub, sizeof st[WF_PHYSICS].sub, "%s, %s", sim_app(&SU.s), TURB_NAME[SU.s.turb]); }
    /* 5 the run */
    if (here && !meshing) { st[WF_RUN].state = WF_BUSY; pl_fmt_ui(st[WF_RUN].sub, sizeof st[WF_RUN].sub, "%s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label); }
    else if (c->s.nTimes > 0 || c->s.procTimes > 0) {
        /* results from before the mesh or the case files changed are out of date: the latest time folder says when */
        bool proc = c->s.procTimes > 0 && (!c->s.nTimes || atof(c->s.procLatest) > atof(c->s.latest));
        char lt[PL_PATH], rel[64];
        snprintf(rel, sizeof rel, proc ? "processor0/%s" : "%s", proc ? c->s.procLatest : c->s.latest);
        pl_join(lt, sizeof lt, c->s.path, rel);
        double rMt = su_mtime_ns(lt);
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
    else if (k == WF_BOUNDARIES) { if (imported) { A.physicsTab = 2; SU.step = SS_BOUNDARIES; app_go(PG_PHYSICS); } else wf_open(c, k); }
    else if (k == WF_MESH) stlmesh_generate(c);              /* here: the bar shows the steps, the Run page the log */
    else if (k == WF_PHYSICS) su_write_files(c, false);
    else if (k == WF_RUN) { A.runPending = true; app_go(PG_RUN); }
    else app_open_paraview();
}

/* the bar itself, for either set of steps: n steps with their states, the next one highlighted, its button on the right
   (*clicked: the step clicked, -1 none; *go: the button pressed) */
static float steps_bar(int n, const char *const *name, const WfStep *st, int next, const char *nlabel, const char *nicon,
                       bool nextOff, float x, float y, float w, int *clicked, bool *go)
{
    *clicked = -1; *go = false;
    float y0 = y;
    bool narrow = w < 640;                                  /* the Run page's column: one step a line */
    float bw = nlabel ? ui_button_width(nicon, nlabel) : 0;
    /* each step as wide as its text needs, the room left shared out; short of room (a large UI scale), the lines under
       the names wrap rather than lose their ends */
    float sw[8], sx[8], need = 0, avail = w - bw - 16, sh = narrow ? 40 : 54;
    for (int k = 0; k < n; k++) {
        sw[k] = 46 + 14 + fmaxf(ui_text_w(F_SEMI, 14, ui_cap(name[k])), ui_text_w(F_REG, 12.5f, st[k].sub));
        need += sw[k];
    }
    /* far short of room (six steps on a small window at a large scale): the names only, each step's line in its tooltip, rather
       than lines broken in the middle of words */
    /* short of room: each column shrinks no further than its longest word (a line never breaks inside one); when even that
       doesn't fit, names only */
    float minw[8], sumMin = 0;
    for (int k = 0; k < n; k++) {
        float longest = ui_text_w(F_SEMI, 14, ui_cap(name[k]));
        char wbuf[120]; pl_strlcpy(wbuf, st[k].sub, sizeof wbuf);
        for (char *tok = strtok(wbuf, " "); tok; tok = strtok(NULL, " ")) longest = fmaxf(longest, ui_text_w(F_REG, 12.5f, tok));
        minw[k] = fminf(sw[k], longest + 52 + 6);
        sumMin += minw[k];
    }
    bool subs = narrow || need <= avail || sumMin <= avail;
    if (!subs) {
        need = 0;
        for (int k = 0; k < n; k++) { sw[k] = 46 + 14 + ui_text_w(F_SEMI, 14, ui_cap(name[k])); need += sw[k]; }
        sh = 40;
    }
    for (int k = 0; k < n; k++) {
        if (need <= avail) sw[k] += (avail - need) / n;
        else if (subs && !narrow && need > sumMin) sw[k] = minw[k] + (avail - sumMin) * (sw[k] - minw[k]) / (need - sumMin);
        else sw[k] = avail * sw[k] / need;
        sx[k] = k ? sx[k - 1] + sw[k - 1] : x;
        if (!narrow && subs && need > avail)
            sh = fmaxf(sh, 34 + ui_text_wrap(F_REG, 12.5f, st[k].sub, (Rectangle){ 0, 0, sw[k] - 6 - 46, 300 }, 16, TH.text3, false));
    }
    /* narrow (the Run page's column): the steps done fold into one line, the rest a row each, so the run's own controls stay
       near the top (W1: the whole bar pushed the Courant check off the column) */
    int row = 0, nDone = 0;
    if (narrow) {
        char done[200] = "";
        for (int k = 0; k < n; k++) if (st[k].state == WF_DONE) { snprintf(done + strlen(done), sizeof done - strlen(done), "%s%s", nDone ? ", " : "", name[k]); nDone++; }
        if (nDone) {
            Rectangle r = { x, y, w, sh - 4 };
            ui_icon(ICON_CIRCLE_CHECK, r.x + 20, r.y + r.height / 2, 20, TH.ok);
            char t[240]; pl_fmt_ui(t, sizeof t, nDone == n ? "All done: %s" : "Done: %s", done);
            ui_text_clip(F_REG, 13, t, r.x + 40, r.y + r.height / 2 - 9, w - 46, TH.text2);
            ui_tooltip(r, t);
            row = 1;
        }
    }
    for (int k = 0; k < n; k++) {
        if (narrow && st[k].state == WF_DONE) continue;
        Rectangle r = narrow ? (Rectangle){ x, y + row++ * sh, w, sh - 4 } : (Rectangle){ sx[k], y, sw[k] - 6, sh };
        ui_push_id(ui_idn(ui_id("wf"), k));
        if (ui_row(ui_id("step"), r, k == next)) *clicked = k;
        ui_tooltip(r, st[k].sub);
        Color c0 = st[k].state == WF_DONE ? TH.ok : st[k].state == WF_STALE ? TH.warn : k == next || st[k].state == WF_BUSY ? TH.accent : TH.text3;
        float cx = r.x + 20, cy = r.y + r.height / 2;
        if (st[k].state == WF_BUSY) ui_spinner(cx, cy, 8, c0);
        else if (st[k].state == WF_DONE) ui_icon(ICON_CIRCLE_CHECK, cx, cy, 20, c0);
        else if (st[k].state == WF_STALE) ui_icon(ICON_TRIANGLE_ALERT, cx, cy, 18, c0);
        else {
            ui_rect_lines((Rectangle){ cx - 10, cy - 10, 20, 20 }, 10, 1.5f, c0);
            char num[4]; snprintf(num, sizeof num, "%d", k + 1);
            ui_text(F_SEMI, 12, num, cx - ui_text_w(F_SEMI, 12, num) / 2, cy - 8, c0);
        }
        float tx = r.x + 40, tw = r.width - 46;
        if (narrow) {
            ui_text_clip(F_SEMI, 14, ui_cap(name[k]), tx, cy - 9, 96, k == next ? TH.text : TH.text2);
            ui_text_clip(F_REG, 12.5f, st[k].sub, tx + 100, cy - 8, tw - 100, st[k].state == WF_STALE ? TH.warn : TH.text3);
        } else if (!subs) {
            ui_text_clip(F_SEMI, 14, ui_cap(name[k]), tx, cy - 9, tw, k == next ? TH.text : st[k].state == WF_STALE ? TH.warn : TH.text2);
        } else {
            float ty = r.y + 9;
            ui_text_clip(F_SEMI, 14, ui_cap(name[k]), tx, ty, tw, k == next ? TH.text : TH.text2);
            Color sc = st[k].state == WF_STALE ? TH.warn : TH.text3;
            if (sh > 54) ui_text_wrap(F_REG, 12.5f, st[k].sub, (Rectangle){ tx, ty + 20, tw, r.height - 29 }, 16, sc, true);
            else ui_text_clip(F_REG, 12.5f, st[k].sub, tx, r.y + 29, tw, sc);
        }
        if (!narrow && k < n - 1) ui_icon(ICON_CHEVRON_RIGHT, r.x + r.width + 3, cy, 12, TH.text3);
        ui_pop_id();
    }
    y += narrow ? row * sh + 6 : sh + 4;
    if (nlabel) {
        Rectangle b = narrow ? (Rectangle){ x, y, w, 38 } : (Rectangle){ x + w - bw, y0 + (sh - 40) / 2, bw, 40 };
        ui_disable(nextOff);
        if (ui_button(ui_id("wf-next"), b, nicon, nlabel, BTN_PRIMARY)) *go = true;
        ui_disable(false);
        if (narrow) y += 46;
    }
    ui_divider(x, y + 8, w);
    return y + 24 - y0;
}

float stl_workflow(Case *c, float x, float y, float w)
{
    if (!c) return 0;
    char ini[PL_PATH]; pl_join(ini, sizeof ini, c->s.path, "mesher/mesher.ini");
    bool imported = wf_imported(c);
    if (!pl_is_file(ini) && !imported) return 0;            /* not a case made from an STL or an imported mesh */
    su_sync(c);
    WfStep st[WF__N];
    wf_status(c, st);
    const char *NAME[WF__N] = { imported ? "Mesh file" : "STL", "Boundaries", "Mesh", "Physics", "Run" };
    const char *nlabel = NULL, *nicon = NULL;
    int next = wf_next(st, imported, &nlabel, &nicon), clicked; bool go;
    float h = steps_bar(WF__N, NAME, st, next, nlabel, nicon, A.cj->active && next != WF__N && next != WF_BOUNDARIES, x, y, w, &clicked, &go);
    if (clicked >= 0) wf_open(c, clicked);
    if (go) wf_do(c, next);
    return h;
}

/* ============================================================================ the steps of any other case (workflow plan W1,
 * 2026-11-14): a tutorial, a blockMesh case, a folder opened from elsewhere. Mesh, Boundaries, Physics, Numerics, Run, each
 * from what is on disk, the first one not done the next, its button doing it (or opening the page that does). */
enum { CS_MESH = 0, CS_BOUNDARIES, CS_PHYSICS, CS_NUMERICS, CS_RUN, CS_RESULTS, CS__N };

static const char *case_mesh_tool(const Case *c)
{
    char p[PL_PATH];
    pl_join(p, sizeof p, c->s.path, "system/snappyHexMeshDict");
    if (pl_is_file(p)) return "snappyHexMesh";
    pl_join(p, sizeof p, c->s.path, "system/blockMeshDict");
    if (pl_is_file(p)) return "blockMesh";
    return NULL;
}

static void cs_status(Case *c, WfStep st[CS__N])
{
    memset(st, 0, sizeof(WfStep) * CS__N);
    bool here = A.cj->active && !strcmp(A.cj->casePath, c->s.path);
    bool meshing = here && (!strcmp(A.cj->title, "Mesh") || !strcmp(A.cj->title, "checkMesh"));
    char faces[PL_PATH]; pl_join(faces, sizeof faces, c->s.path, "constant/polyMesh/faces");
    double meshMt = su_mtime_ns(faces);
    const char *mt = case_mesh_tool(c);
    /* 1 the mesh */
    int lc = logset_find(&A.logs, "log.checkMesh");
    bool failed = lc >= 0 && A.logs.logs[lc].meshFailed && !app_mesh_verdict(c)->boundarySkewOnly;
    if (meshing) { st[CS_MESH].state = WF_BUSY; pl_fmt_ui(st[CS_MESH].sub, sizeof st[CS_MESH].sub, "%s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label); }
    else if (!c->s.meshed) pl_strlcpy(st[CS_MESH].sub, mt ? "Not generated yet" : "No mesh: import or build one", sizeof st[CS_MESH].sub);
    else if (c->s.meshStale) { st[CS_MESH].state = WF_STALE; pl_fmt_ui(st[CS_MESH].sub, sizeof st[CS_MESH].sub, "%sDict changed: generate again", mt ? mt : "The mesh"); }
    else if (failed) { st[CS_MESH].state = WF_STALE; pl_strlcpy(st[CS_MESH].sub, "checkMesh: checks failed", sizeof st[CS_MESH].sub); }
    else {
        st[CS_MESH].state = WF_DONE;
        char n[32];
        if (c->s.nCells >= 1000000) snprintf(n, sizeof n, "%.2f million", c->s.nCells / 1e6); else snprintf(n, sizeof n, "%ld", c->s.nCells);
        pl_fmt_ui(st[CS_MESH].sub, sizeof st[CS_MESH].sub, "%s cells, %d patches", n, c->nPatches);
    }
    /* 2 the boundaries: a condition for every patch in every field (the sidebar's count, cached there twice a second) */
    static double bcAt = -10; static char bcPath[PL_PATH]; static int bcMiss;
    if (ui_time() - bcAt > 0.5 || strcmp(bcPath, c->s.path)) {
        bcAt = ui_time(); pl_strlcpy(bcPath, c->s.path, sizeof bcPath);
        char m[8]; bcMiss = c->s.meshed && c->nPatches > 0 && c->fields.n > 0 ? bc_missing_report(c, m, sizeof m) : 0;
    }
    if (!c->s.meshed) pl_strlcpy(st[CS_BOUNDARIES].sub, "After the mesh", sizeof st[CS_BOUNDARIES].sub);
    else if (c->fields.n == 0) pl_fmt_ui(st[CS_BOUNDARIES].sub, sizeof st[CS_BOUNDARIES].sub, "No field files in %s/", c->fieldDir[0] ? c->fieldDir : "0");
    else if (bcMiss > 0) { st[CS_BOUNDARIES].state = WF_STALE; pl_fmt_ui(st[CS_BOUNDARIES].sub, sizeof st[CS_BOUNDARIES].sub, "%d patch condition%s missing", bcMiss, bcMiss == 1 ? "" : "s"); }
    else { st[CS_BOUNDARIES].state = WF_DONE; pl_fmt_ui(st[CS_BOUNDARIES].sub, sizeof st[CS_BOUNDARIES].sub, "%d fields on %d patches", c->fields.n, c->nPatches); }
    /* 3 the physics: a solver and the property files it reads */
    bool props = case_dict(c, "constant/transportProperties") || case_dict(c, "constant/thermophysicalProperties") ||
                 case_dict(c, "constant/physicalProperties") || case_dict(c, "constant/momentumTransport") || case_dict(c, "constant/turbulenceProperties");
    char ini[PL_PATH], u0[PL_PATH];
    sim_ini_path(c->s.path, ini, sizeof ini); pl_join(u0, sizeof u0, c->s.path, "0/U");
    double iniMt = su_mtime_ns(ini), u0Mt = su_mtime_ns(u0);
    if (iniMt && u0Mt && iniMt > u0Mt) { st[CS_PHYSICS].state = WF_STALE; pl_strlcpy(st[CS_PHYSICS].sub, "Setup changed: write again", sizeof st[CS_PHYSICS].sub); }
    else if (!c->s.app[0]) pl_strlcpy(st[CS_PHYSICS].sub, "No solver set", sizeof st[CS_PHYSICS].sub);
    else if (!props) pl_fmt_ui(st[CS_PHYSICS].sub, sizeof st[CS_PHYSICS].sub, "%s: no properties in constant/", c->s.app);
    else { st[CS_PHYSICS].state = WF_DONE; pl_fmt_ui(st[CS_PHYSICS].sub, sizeof st[CS_PHYSICS].sub, "%s", c->s.app); }
    /* 4 the numerics */
    bool sch = case_dict(c, "system/fvSchemes") != NULL, sol = case_dict(c, "system/fvSolution") != NULL;
    if (!sch || !sol) pl_fmt_ui(st[CS_NUMERICS].sub, sizeof st[CS_NUMERICS].sub, "No %s", !sch ? "system/fvSchemes" : "system/fvSolution");
    else { st[CS_NUMERICS].state = WF_DONE; pl_strlcpy(st[CS_NUMERICS].sub, "Schemes and solvers set", sizeof st[CS_NUMERICS].sub); }
    /* 5 the run */
    if (here && !meshing) { st[CS_RUN].state = WF_BUSY; pl_fmt_ui(st[CS_RUN].sub, sizeof st[CS_RUN].sub, "%s ...", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label); }
    else if (c->s.status == CS_FAILED) { st[CS_RUN].state = WF_STALE; pl_strlcpy(st[CS_RUN].sub, "Last run failed", sizeof st[CS_RUN].sub); }
    else if (c->s.nTimes > 0 || c->s.procTimes > 0) {
        bool proc = c->s.procTimes > 0 && (!c->s.nTimes || atof(c->s.procLatest) > atof(c->s.latest));
        char lt[PL_PATH], rel[64];
        snprintf(rel, sizeof rel, proc ? "processor0/%s" : "%s", proc ? c->s.procLatest : c->s.latest);
        pl_join(lt, sizeof lt, c->s.path, rel);
        double rMt = su_mtime_ns(lt);
        if (rMt && rMt < meshMt) { st[CS_RUN].state = WF_STALE; pl_strlcpy(st[CS_RUN].sub, "Older than the mesh: run again", sizeof st[CS_RUN].sub); }
        else { st[CS_RUN].state = WF_DONE; pl_fmt_ui(st[CS_RUN].sub, sizeof st[CS_RUN].sub, "Results to %s%s", proc ? c->s.procLatest : c->s.latest, proc ? " (parallel)" : ""); }
    }
    else pl_strlcpy(st[CS_RUN].sub, "Not run yet", sizeof st[CS_RUN].sub);
    /* 6 the results (W5): what the run's log says of its answer */
    int lk = logset_primary(&A.logs, c->s.app);
    const LogMon *m = lk >= 0 ? &A.logs.logs[lk] : NULL;
    bool steady = strstr(c->s.app, "simple") || strstr(c->s.app, "Simple");
    if (st[CS_RUN].state != WF_DONE) pl_strlcpy(st[CS_RESULTS].sub, "After the run", sizeof st[CS_RESULTS].sub);
    else if (m && m->converged) { st[CS_RESULTS].state = WF_DONE; pl_strlcpy(st[CS_RESULTS].sub, "Converged", sizeof st[CS_RESULTS].sub); }
    else if (m && m->ended && steady) { st[CS_RESULTS].state = WF_STALE; pl_strlcpy(st[CS_RESULTS].sub, "Not converged: check them", sizeof st[CS_RESULTS].sub); }
    else { st[CS_RESULTS].state = WF_DONE; pl_strlcpy(st[CS_RESULTS].sub, m && m->ended ? "Finished" : "To look at", sizeof st[CS_RESULTS].sub); }
}

static void cs_open(Case *c, int k)
{
    if (k == CS_MESH) {                                     /* failed checks: Quality; no mesh or an old one: its dictionaries; else View */
        int lc = logset_find(&A.logs, "log.checkMesh");
        bool failed = lc >= 0 && A.logs.logs[lc].meshFailed && !app_mesh_verdict(c)->boundarySkewOnly;
        A.meshTab = !c->s.meshed || c->s.meshStale ? 0 : failed ? 4 : 2;
        app_go(PG_MESH);
    }
    else if (k == CS_BOUNDARIES) app_go(PG_BC);
    else if (k == CS_PHYSICS) { char ini[PL_PATH]; sim_ini_path(c->s.path, ini, sizeof ini); if (pl_is_file(ini)) A.physicsTab = 2; app_go(PG_PHYSICS); }
    else if (k == CS_NUMERICS) app_go(PG_NUMERICS);
    else if (k == CS_RESULTS) app_go(PG_RESULTS);
    else app_go(PG_RUN);
}

float case_steps(Case *c, float x, float y, float w)
{
    if (!c || c->s.prog || c->s.nRegions > 0) return 0;    /* other programs have their own pages; multi-region keeps its per-region bar */
    float h = stl_workflow(c, x, y, w);                     /* made from an STL or an imported mesh: that bar */
    if (h > 0) return h;
    WfStep st[CS__N];
    cs_status(c, st);
    static const char *const NAME[CS__N] = { "Mesh", "Boundaries", "Physics", "Numerics", "Run", "Results" };
    static const char *const I[CS__N] = { ICON_GRID_3X3, ICON_LAYERS, ICON_SLIDERS_HORIZONTAL, ICON_SLIDERS_HORIZONTAL, ICON_PLAY, ICON_CHART_LINE };
    int next = CS__N;
    bool busy = false;
    for (int k = 0; k < CS__N; k++) {
        if (st[k].state == WF_BUSY) { busy = true; next = -1; break; }
        if (st[k].state != WF_DONE) { next = k; break; }
    }
    const char *mt = case_mesh_tool(c);
    const char *nlabel = NULL, *nicon = NULL;
    if (!busy) {
        if (next == CS_MESH) { nlabel = st[CS_MESH].state == WF_STALE && c->s.meshed && !c->s.meshStale ? "Mesh quality" : mt ? "Generate mesh" : "Make a mesh"; nicon = I[CS_MESH]; }
        else if (next == CS_BOUNDARIES) { nlabel = c->fields.n == 0 && setup_can_write_fields(c) ? "Write boundary files" : "Set the boundaries"; nicon = I[next]; }
        else if (next == CS_PHYSICS) { nlabel = st[CS_PHYSICS].state == WF_STALE ? "Write case files" : "Set the physics"; nicon = st[CS_PHYSICS].state == WF_STALE ? ICON_FILE_TEXT : I[next]; }
        else if (next == CS_NUMERICS) { nlabel = "Set the numerics"; nicon = I[next]; }
        else if (next == CS_RUN) { nlabel = "Run simulation"; nicon = ICON_PLAY; }
        else if (next == CS_RESULTS && A.page == PG_RESULTS) { nlabel = "Run longer"; nicon = ICON_PLAY; }
        else if (next == CS_RESULTS || A.page != PG_RESULTS) { nlabel = "See the results"; nicon = ICON_CHART_LINE; }
        else { nlabel = "Open in ParaView"; nicon = ICON_EYE; }
    }
    int clicked; bool go;
    h = steps_bar(CS__N, NAME, st, next, nlabel, nicon, A.cj->active, x, y, w, &clicked, &go);
    if (clicked >= 0) cs_open(c, clicked);
    if (go) {
        if (next == CS_MESH && mt && !(st[CS_MESH].state == WF_STALE && c->s.meshed && !c->s.meshStale)) app_generate_mesh();
        else if (next == CS_BOUNDARIES && c->fields.n == 0 && setup_can_write_fields(c)) setup_write_fields(c);
        else if (next == CS_PHYSICS && st[CS_PHYSICS].state == WF_STALE) { su_sync(c); su_write_files(c, false); }
        else if (next == CS_RUN) { A.runPending = true; app_go(PG_RUN); }
        else if (next == CS_RESULTS && A.page == PG_RESULTS) { A.continueRun = true; app_go(PG_RUN); }
        else if (next == CS_RESULTS || (next == CS__N && A.page != PG_RESULTS)) app_go(PG_RESULTS);
        else if (next == CS__N) app_open_paraview();
        else cs_open(c, next);
    }
    return h;
}

/* ============================================================================ the page (user plan step 2, 2026-10-27)
 * A new setup first asks what is being simulated: a template card sets the models and their values. Then a list of steps
 * on the left (Models, Materials, Boundaries, Starting values, Reports, Solver, Check), each saying whether it is done,
 * needs a look or is wrong, with one step open on the right. A step shows only the rows its models need; expert rows
 * sit behind its More. */

/* a note under a step's head */
float su_note(float x, float y, float w, const char *t)
{
    return ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y, w, 200 }, 19, TH.text3, true) + 12;
}


const char *const SS_TITLE[SS__N] = { "Models", "Materials", "Boundaries", "Zones", "Starting values", "Reports", "Solver", "Check" };

/* a whole number in a row (correctors), saved as it changes */
bool su_row_count(float x, float *y, float w, bool hints, const char *id, const char *label, const char *hint, int *v, int lo, int hi)
{
    double d = *v;
    if (!su_row_num(x, y, w, hints, id, label, hint, &d, lo, hi, NULL, NULL)) return false;
    *v = (int)lround(d);
    su_save();
    return true;
}

/* the step's More: the expert rows below it shown or not */
float su_more_toggle(int step, float x, float y)
{
    const char *lab = SU.more[step] ? "Fewer settings" : "More settings";
    const char *ic = SU.more[step] ? ICON_CHEVRON_UP : ICON_CHEVRON_DOWN;
    if (ui_button(ui_idn(ui_id("su-more"), step), (Rectangle){ x, y + 6, ui_button_width(ic, lab), 32 }, ic, lab, BTN_GHOST)) SU.more[step] = !SU.more[step];
    return 46;
}

static bool two_d(Case *c) { return sim_two_d(&SU.s) || stlmesh_two_d(c); }

/* the Reynolds number from the inflow speed and the part's size; 0: unknown */
double su_reynolds(void)
{
    bool driven = SU.s.U0[0] || SU.s.U0[1] || SU.s.U0[2];      /* an inlet, an opening, a moving wall or a start in motion */
    for (int i = 0; i < SU.s.nB; i++) {
        const SimBoundary *b = &SU.s.b[i];
        if (!sim_constraint(b->meshType) && (b->role == ROLE_INLET || (b->role == ROLE_WALL && b->wall == WL_MOVING))) driven = true;
    }
    return driven && SU.size > 0 && SU.s.nu > 0 ? sim_ref_speed(&SU.s) * SU.size / SU.s.nu : 0;
}

/* what the checks (sim_check) find in one step, a line each, and for Models a laminar model on a turbulent flow (or the
   other way); returns the count. SA_* are the steps' numbers. */
int su_step_issues(int step, char *out, size_t cap)
{
    char all[2000];
    int n = sim_check(&SU.s, all, sizeof all), k = 0;
    out[0] = 0;
    char *line = all;
    for (int i = 0; i < n && line; i++) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (sim_check_area(i) == step) snprintf(out + strlen(out), cap - strlen(out), "%s%s", k++ ? "\n" : "", line);
        line = nl ? nl + 1 : NULL;
    }
    double Re = su_reynolds();
    if (step == SS_MODELS && Re > 1e4 && SU.s.turb == TURB_LAMINAR)
        snprintf(out + strlen(out), cap - strlen(out), "%sRe is about %.3g: the flow is turbulent, so a laminar model will not settle. Pick RANS.", k++ ? "\n" : "", Re);
    /* LES and DES: cells across the boundary layer, from the mesh's mean cell (or the STL mesher's surface cell) */
    int ap = sim_approach(SU.s.turb);
    if (step == SS_MODELS && (ap == APPR_LES || ap == APPR_DES) && SU.size > 0) {
        double d = 0, cell = 0, f; int n;
        sim_wall_spacing(&SU.s, sim_ref_speed(&SU.s), SU.size, 1, NULL, &d, NULL);
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
    double Ra = sim_rayleigh(&SU.s, SU.height);
    if (step == SS_MODELS && Ra > 1e9 && SU.s.turb == TURB_LAMINAR)
        snprintf(out + strlen(out), cap - strlen(out), "%sRa is about %.3g: natural convection turns turbulent above about 1e9, so a laminar model will not settle. "
                 "Pick RANS (k-omega SST).", k++ ? "\n" : "", Ra);
    return k;
}

/* the step's problems, over its rows */
float su_issues_callout(int step, float x, float y, float w)
{
    char t[1200];
    int n = su_step_issues(step, t, sizeof t);
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
    SimSetup *s = &SU.s;
    /* Models */
    pl_fmt_ui(st[SS_MODELS].sub, sizeof st[0].sub, "%s %s%s%s%s", FLOW_NAME[s->flow], sim_approach_label(s, sim_approach(s->turb)),
              s->turb == TURB_LAMINAR ? "" : ", ", s->turb == TURB_LAMINAR ? "" : TURB_NAME[s->turb],
              sim_is_vof(s) ? ", free surface" : sim_is_gas(s) ? ", ideal gas" : s->energy == EN_FORCED ? ", heat" : s->energy == EN_BUOYANT ? ", buoyancy" : "");
    /* Materials */
    if (sim_is_vof(s)) pl_fmt_ui(st[SS_MATERIALS].sub, sizeof st[0].sub, "%s and air", LIQ_NAME[s->liquid]);
    else if (sim_is_gas(s)) pl_fmt_ui(st[SS_MATERIALS].sub, sizeof st[0].sub, "%s at %.4g kPa, %.4g kg/m³", GAS_NAME[s->gas], s->pOp / 1000, s->rho);
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
    /* Zones */
    {
        int nr = 0, np = 0;
        for (int i = 0; i < s->nZ; i++) { if (s->z[i].kind == ZN_ROTATE) nr++; else np++; }
        if (!s->nZ) pl_strlcpy(st[SS_ZONES].sub, "None", sizeof st[0].sub);
        else pl_fmt_ui(st[SS_ZONES].sub, sizeof st[0].sub, "%s%s%s", nr ? TextFormat("%d rotating", nr) : "", nr && np ? ", " : "",
                       np ? TextFormat("%d porous", np) : "");
    }
    /* Starting values */
    double u0 = sqrt(s->U0[0] * s->U0[0] + s->U0[1] * s->U0[1] + s->U0[2] * s->U0[2]);
    if (s->start == SV_POTENTIAL && !sim_start_why_not(s, s->start)) pl_strlcpy(st[SS_START].sub, "Potential flow", sizeof st[0].sub);
    else if (s->start == SV_MAP && !sim_start_why_not(s, s->start)) pl_fmt_ui(st[SS_START].sub, sizeof st[0].sub, "From %s", s->mapFrom[0] ? pl_basename(s->mapFrom) : "another case");
    else if (u0 > 0) pl_fmt_ui(st[SS_START].sub, sizeof st[0].sub, "Moving at %.3g m/s", u0);
    else pl_strlcpy(st[SS_START].sub, "Still fluid", sizeof st[0].sub);
    if (sim_regions_set(s)) pl_fmt_ui(st[SS_START].sub + strlen(st[SS_START].sub), sizeof st[0].sub - strlen(st[SS_START].sub), ", %d region%s", s->nFill, s->nFill == 1 ? "" : "s");
    if (s->flow == FLOW_STEADY && s->ramp >= 1) pl_fmt_ui(st[SS_START].sub + strlen(st[SS_START].sub), sizeof st[0].sub - strlen(st[SS_START].sub), ", inlets ramped");
    if (s->energy != EN_OFF) pl_fmt_ui(st[SS_START].sub + strlen(st[SS_START].sub), sizeof st[0].sub - strlen(st[SS_START].sub), ", %.4g °C", s->T0 - 273.15);
    if (sim_is_vof(s)) pl_fmt_ui(st[SS_START].sub + strlen(st[SS_START].sub), sizeof st[0].sub - strlen(st[SS_START].sub), s->nFill ? ", water in %d region%s" : ", no water",
                                 s->nFill, s->nFill == 1 ? "" : "s");
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
    {
        const char *nmw = c && sim_numerics_edited(c->s.path) ? "Custom" : NUM_NAME[s->numerics];
        if (s->flow == FLOW_STEADY) pl_fmt_ui(st[SS_SOLVER].sub, sizeof st[0].sub, "%s, %.6g iterations%s", nmw, s->iterations, s->resStop ? ", stops when converged" : "");
        else pl_fmt_ui(st[SS_SOLVER].sub, sizeof st[0].sub, "%s, %s to %.4g s%s", nmw, ALG_NAME[s->algo], s->endTime, s->algo != ALG_PISO && s->adjustDt ? TextFormat(", Courant %.3g", s->maxCo) : "");
    }
    /* a step's problems: its first one instead (Models and Solver: a look; the rest: wrong) */
    int bad = 0, worst = SS_OK;
    for (int k = 0; k < SS_CHECK; k++) {
        char t[1200];
        int n = su_step_issues(k, t, sizeof t);
        if (!n) continue;
        bad += n;
        char *nl = strchr(t, '\n'); if (nl) *nl = 0;
        st[k].state = k == SS_MODELS || k == SS_SOLVER ? SS_ATTN : SS_ERR;
        pl_strlcpy(st[k].sub, t, sizeof st[0].sub);
        if (st[k].state > worst) worst = st[k].state;
    }
    /* Check */
    if (bad) { st[SS_CHECK].state = worst; pl_fmt_ui(st[SS_CHECK].sub, sizeof st[0].sub, "%d thing%s to look at", bad, bad == 1 ? "" : "s"); }
    else if (su_files_stale(c)) { st[SS_CHECK].state = SS_ATTN; pl_strlcpy(st[SS_CHECK].sub, "Case files not written", sizeof st[0].sub); }
    else pl_strlcpy(st[SS_CHECK].sub, "Case files match", sizeof st[0].sub);
}

/* ---------------------------------------------------------------------------- What are you simulating? */
const char *const TMPL_ICON[TMPL__N] = { ICON_WIND, ICON_GIT_FORK, ICON_WAVES, ICON_ORBIT, ICON_THERMOMETER, ICON_FLAME,
                                                ICON_ROCKET, ICON_DROPLETS, ICON_FAN, ICON_SLIDERS_HORIZONTAL };

int su_mesh_zones(const char *casePath, char (*out)[64]);
void su_water_default(Case *c);
void su_list_set(char *list, size_t cap, const char *name, bool on);

static void pick_template(Case *c, int t)
{
    sim_template_apply(&SU.s, t, SU.size);
    if (t == TMPL_GAS) {
        /* high-speed gas: inlets still at a walking pace (the new boundaries' 1 m/s) come in at Mach 0.5; a body in a wind
           tunnel gets its lift and drag measured */
        for (int i = 0; i < SU.s.nB; i++) {
            SimBoundary *b = &SU.s.b[i];
            if (b->role == ROLE_INLET && b->inlet == IN_SPEED && b->speed < 0.3 * sim_sound_speed(&SU.s, b->T)) { b->inlet = IN_MACH; b->mach = 0.5; }
        }
        if (SU.external) { SU.s.forces = true; SU.s.forceAuto = true; }
        sim_settle(&SU.s);
    }
    /* a body in a wind tunnel: the air starts at the free stream */
    if (SU.external) { SimBoundary *in = sim_find(&SU.s, "inlet"); if (in && in->role == ROLE_INLET) free_stream(c, sim_ref_speed(&SU.s), SU.s.U0); }
    if (t == TMPL_VOF) { su_gravity_auto(c); su_water_default(c); }
    if (t == TMPL_ROTATING && !sim_has_rotation(&SU.s)) {
        /* a rotating zone on the mesh's own cell zone (a mixer's rotor), else a cylinder in the middle to place; the torque
           on the walls whose names say they turn */
        char mz[16][64]; int nmz = su_mesh_zones(c->s.path, mz);
        double lo[3], hi[3]; bool hb = mesh_bounds(c->s.path, lo, hi);
        int i = sim_zone_add(&SU.s, ZN_ROTATE, hb ? lo : NULL, hb ? hi : NULL, nmz ? mz[0] : NULL);
        if (i >= 0) {
            SimZone *z = &SU.s.z[i];
            for (int k = 0; k < SU.s.nB; k++) {
                const SimBoundary *b = &SU.s.b[k];
                if (b->role != ROLE_WALL || sim_constraint(b->meshType)) continue;
                char l[64]; size_t n = 0;
                for (; b->name[n] && n < sizeof l - 1; n++) l[n] = (char)tolower((unsigned char)b->name[n]);
                l[n] = 0;
                if (strstr(l, "rotor") || strstr(l, "impeller") || strstr(l, "blade") || strstr(l, "fan") || strstr(l, "prop") || strstr(l, "rotat") || strstr(l, "hub"))
                    su_list_set(z->torque, sizeof z->torque, b->name, true);
            }
        }
        sim_settle(&SU.s);
    }
    su_save();
    SU.picking = false; SU.step = t == TMPL_ROTATING ? SS_ZONES : SS_MODELS;
    bool layers = t == TMPL_EXTERNAL || t == TMPL_INTERNAL || t == TMPL_SHEDDING || t == TMPL_HEAT || t == TMPL_NATCONV;
    su_gravity_auto(c);
    if (stlmesh_template_hints(c, layers)) ui_toast(TOAST_INFO, "%s: prism layers switched on in the STL mesher, for the %s layer", TMPL_NAME[t],
                                                    t == TMPL_HEAT || t == TMPL_NATCONV ? "thermal boundary" : "boundary");
    else ui_toast(TOAST_OK, "%s: %s", TMPL_NAME[t], sim_template_hint(t));
}

static float template_cards(Case *c, float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "What are you simulating?", "Pick the closest. It sets the models and their values, and the steps after it show only "
                                                         "what those models need. Every choice can be changed afterwards.");
    if (SU.picking) {
        float bw = ui_button_width(ICON_X, "Keep the current setup");
        if (ui_button(ui_id("su-tmpl-cancel"), (Rectangle){ x, y, bw, 34 }, ICON_X, "Keep the current setup", BTN_GHOST)) SU.picking = false;
        y += 46;
    }
    bool twoD = two_d(c);
    int suits = SU.size > 0 ? (SU.external ? TMPL_EXTERNAL : TMPL_INTERNAL) : -1;   /* what the STL mesher's flow says */
    int cols = (int)fmaxf(1, fminf(5, floorf((w + 14) / 270)));
    float cw = (w - 14 * (cols - 1)) / cols, ch = 0;
    /* one more card after the templates: heat through solids (conjugate heat transfer, page_cht.c), which needs zones */
    enum { T_CHT = TMPL__N, NCARD = TMPL__N + 1 };
    char mz[16][64]; int nmz = su_mesh_zones(c->s.path, mz);
    static const char *CHT_HINT = "Fluid and solid parts together: a heat sink in air, a heated plate under a flow, a chip on a board. "
                                  "Heat crosses from the solids into the fluid.";
    const char *chtWhy = nmz ? NULL : "Needs a mesh with a cellZone for each part (solid or fluid): give each closed part a cell zone in the "
                                      "STL mesher, or make zones with topoSet (Mesh > Tools).";
#define CARD_HINT(t) ((t) == T_CHT ? CHT_HINT : sim_template_hint(t))
#define CARD_WHY(t) ((t) == T_CHT ? chtWhy : sim_template_why_not(&SU.s, (t), twoD))
    for (int t = 0; t < NCARD; t++) {                       /* the tallest card's height, for even rows */
        float h = 58 + ui_text_wrap(F_REG, 13, CARD_HINT(t), (Rectangle){ 0, 0, cw - 32, 300 }, 19, TH.text3, false);
        const char *why = CARD_WHY(t);
        if (why) h += 8 + ui_text_wrap(F_REG, 12.5f, why, (Rectangle){ 0, 0, cw - 32, 300 }, 17, TH.text3, false);
        if (t == suits) h += 30;
        ch = fmaxf(ch, h + 14);
    }
    for (int t = 0; t < NCARD; t++) {
        Rectangle r = { x + (t % cols) * (cw + 14), y + (t / cols) * (ch + 14), cw, ch };
        const char *why = CARD_WHY(t), *name = t == T_CHT ? "Heat through solids" : TMPL_NAME[t];
        bool on = t < TMPL__N && SU.s.tmpl == t, hov = !why && ui_hovered(r);
        UiId id = ui_idn(ui_id("su-tmpl"), t);
        ui_disable(why != NULL);
        ui_register(id, r, name);
        ui_disable(false);
        ui_rect(r, 12, on ? ui_alpha(TH.accent, 0.14f) : hov ? TH.hover : TH.surface2);
        ui_rect_lines(r, 12, on ? 2 : 1, on ? TH.accent : TH.lineSoft);
        Color fg = why ? TH.text3 : TH.text;
        ui_icon(t == T_CHT ? ICON_BOX : TMPL_ICON[t], r.x + 28, r.y + 30, 22, why ? TH.text3 : TH.accent);
        ui_text_clip(F_SEMI, 15, ui_cap(name), r.x + 50, r.y + 20, r.width - 66, fg);
        float ty = r.y + 50;
        ty += ui_text_wrap(F_REG, 13, CARD_HINT(t), (Rectangle){ r.x + 16, ty, r.width - 32, 300 }, 19, why ? ui_alpha(TH.text3, 0.75f) : TH.text2, true);
        if (why) ui_text_wrap(F_REG, 12.5f, why, (Rectangle){ r.x + 16, ty + 8, r.width - 32, 300 }, 17, TH.warn, true);
        if (t == suits) ui_badge(r.x + 16, r.y + r.height - 34, ICON_CIRCLE_CHECK, SU.external ? "Suits your part: flow around it" : "Suits your part: flow through it",
                                 TH.ok, ui_alpha(TH.ok, 0.14f));
        if (on) ui_icon(ICON_CIRCLE_CHECK, r.x + r.width - 22, r.y + 22, 18, TH.accent);
        if (why) ui_tooltip(r, why);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (!why && ui_clicked(r)) {
            if (t < TMPL__N) pick_template(c, t);
            else {
                char err[400];
                if (cht_start(c, err, sizeof err)) { A.physicsTab = 3; SU.picking = false; ui_toast(TOAST_OK, "Heat through solids: set each region fluid or solid, then the boundaries"); }
                else ui_toast(TOAST_ERR, "%s", err);
            }
        }
    }
#undef CARD_HINT
#undef CARD_WHY
    y += ((NCARD + cols - 1) / cols) * (ch + 14);
    return y - y0;
}

/* ---------------------------------------------------------------------------- the walls (plan step 5)
 * The treatment, and from the inflow speed, the part's length and the fluid, the first cell for the y+ it wants, the
 * boundary layer's thickness and the prism layers that cover it; for an STL case what the mesher's layers give now, and a
 * button that sets them. */
float su_wall_rows(Case *c, float x, float y, float w, bool hints)
{
    float y0 = y;
    SimSetup *s = &SU.s;
    Rectangle cr = prop_row(x, &y, w, "Walls", hints ? "Wall functions: the first cell sits in the log layer (y+ 30 to 300), coarse and cheap. Resolved: "
                                                       "the first cell inside the viscous sublayer (y+ about 1), for separation, heat transfer, LES and DES." : NULL, 34);
    char autoL[48]; pl_fmt_ui(autoL, sizeof autoL, "Automatic (%s)", sim_wall_treatment(&(SimSetup){ .turb = s->turb, .wallTreat = WT_AUTO }) == WT_RESOLVED ? "resolved" : "functions");
    const char *items[WT__N] = { autoL, WALLT_NAME[WT_FUNCTIONS], WALLT_NAME[WT_RESOLVED] }, *why[WT__N];
    for (int i = 0; i < WT__N; i++) why[i] = sim_wall_why_not(s, i);
    int wt = s->wallTreat;
    if (ui_segmented_why(ui_id("su-wall"), (Rectangle){ cr.x, cr.y, fminf(480, cr.width), 34 }, items, WT__N, &wt, why) && wt != s->wallTreat) { s->wallTreat = wt; su_save(); }
    /* the helper: a flat plate as long as the part at the inflow speed */
    double U = sim_ref_speed(s), L = s->forces && s->fr.lRef > 0 ? s->fr.lRef : SU.size, yp = sim_yplus_target(s), h1 = 0, d = 0, ut = 0;
    double Re = L > 0 ? sim_wall_spacing(s, U, L, yp, &h1, &d, &ut) : 0;
    char t[600];
    if (!(Re > 0)) {
        y += su_note(x, y + 4, w, "The y+ helper needs the part's size: choose an STL (Mesh > STL mesher), or type a reference length under Reports.") + 4;
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
    y += th + 8;
    float cw = ui_button_width(ICON_SIGMA, "Work it through");
    if (ui_button(ui_id("su-yplus-calc"), (Rectangle){ x, y, cw, 30 }, ICON_SIGMA, "Work it through", BTN_GHOST)) calc_open(CALC_WALL);
    ui_tooltip((Rectangle){ x, y, cw, 30 }, "The Calculators dialog: this estimate step by step, other fluids (liquid nitrogen, hydrogen, oxygen), a pipe instead of a plate");
    y += 30 + 12;
    return y - y0;
}

/* the steps (Models to Check): page_setup_steps.c */
/* ---------------------------------------------------------------------------- the step list */
/* one step in the list: its mark (number, tick, alert), title and what it holds; a rail item, or a chip when narrow */
static void step_item(int k, Rectangle r, const SsStatus *st, bool rail)
{
    bool open = SU.step == k;
    UiId id = ui_idn(ui_id("su-step"), k);
    if (ui_row(id, r, open)) SU.step = k;
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
    su_sync(c);
    /* what will be written, whether it is, and the buttons */
    bool stale = su_files_stale(c);
    char t[200], d[400];
    pl_fmt_ui(t, sizeof t, "%s · %s · %s", sim_app(&SU.s), TURB_NAME[SU.s.turb], sim_is_vof(&SU.s) ? TextFormat("%s and air", LIQ_NAME[SU.s.liquid])
              : sim_is_gas(&SU.s) ? TextFormat("%s (ideal gas)", GAS_NAME[SU.s.gas]) : FLUID_NAME[SU.s.fluid]);
    bool adopted = case_adopted(c);
    if (adopted)
        pl_fmt_ui(d, sizeof d, "%s Read from the case's own files: Write case files changes only what you change here, and everything else stays as "
                  "the case had it.", stale ? "Changed since the case files were written." : "The case files match this setup.");
    else
        pl_fmt_ui(d, sizeof d, "%s Write case files puts the setup into 0/, constant/ and system/ (a file that wasn't written here is kept as .orig).",
                  stale ? "Changed since the case files were written." : "The case files match this setup.");
    float bw1 = ui_button_width(ICON_FILE_TEXT, "Write case files"), bw2 = ui_button_width(ICON_PLAY, "Write and go to Run");
    bool beside = w >= 980;                                 /* the buttons right of the summary, or under it */
    float cw = beside ? w - bw1 - bw2 - 36 : w;
    float ch = callout_height(cw, t, d);
    callout((Rectangle){ x, y, cw, ch }, stale ? TOAST_INFO : TOAST_OK, stale ? ICON_INFO : ICON_CIRCLE_CHECK, t, d);
    float by = beside ? y + (ch - 40) / 2 : y + ch + 12, bx = beside ? x + cw + 16 : x;
    ui_disable(A.cj->active);
    if (ui_button(ui_id("su-write"), (Rectangle){ bx, by, bw1, 40 }, ICON_FILE_TEXT, "Write case files", BTN_PRIMARY)) su_write_files(c, false);
    if (ui_button(ui_id("su-write-run"), (Rectangle){ bx + bw1 + 10, by, bw2, 40 }, ICON_PLAY, "Write and go to Run", BTN_SECONDARY)) su_write_files(c, true);
    ui_disable(false);
    y += beside ? ch + 20 : ch + 12 + 40 + 20;
    if (adopted) {                                          /* what Setup couldn't name: closed, with a count (layer 1) */
        const char *n = adopt_notes(c);
        int nk = 0; for (const char *q = n; *q; q++) nk += *q == '\n';
        if (nk) {
            static bool open;
            char lab[96]; pl_fmt_ui(lab, sizeof lab, "Kept as the case has it (%d)", nk);
            float lw = ui_button_width(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, lab);
            if (ui_button(ui_id("su-adopt-notes"), (Rectangle){ x, y, lw, 32 }, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, lab, BTN_GHOST)) open = !open;
            y += 38;
            if (open) {
                char line[400]; const char *q = n;
                while (*q) {
                    size_t k = strcspn(q, "\n"); snprintf(line, sizeof line, "%.*s", (int)k, q);
                    ui_icon(ICON_FILE_TEXT, x + 12, y + 9, 13, TH.text3);
                    y += ui_text_wrap(F_REG, 13, line, (Rectangle){ x + 28, y, w - 28, 200 }, 19, TH.text2, true) + 6;
                    q += k; if (*q) q++;
                }
                y += 10;
            }
        }
    }
    /* a new setup: what is being simulated, first */
    /* a case whose files were written before a template was picked (from the step bar) is on its own choices */
    char u[PL_PATH]; pl_join(u, sizeof u, c->s.path, "0/U");
    if ((SU.s.tmpl < 0 && !pl_is_file(u)) || SU.picking) return y + template_cards(c, x, y, w) + 16 - y0;
    SsStatus st[SS__N];
    ss_status(c, st);
    if (SU.step < 0 || SU.step >= SS__N) SU.step = SS_MODELS;
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
    pl_fmt_ui(head, sizeof head, "%d  %s", SU.step + 1, SS_TITLE[SU.step]);
    y += section_head(cx, y, cwid, head, NULL);
    switch (SU.step) {
    case SS_MODELS:     y += models_step(c, cx, y, cwid); break;
    case SS_MATERIALS:  y += materials_step(cx, y, cwid); break;
    case SS_BOUNDARIES: y += boundaries_step(c, cx, y, cwid); break;
    case SS_ZONES:      y += zones_step(c, cx, y, cwid); break;
    case SS_START:      y += start_step(cx, y, cwid); break;
    case SS_REPORTS:    y += reports_step(c, cx, y, cwid); break;
    case SS_SOLVER:     y += solver_step(cx, y, cwid); break;
    default:            y += check_step(c, cx, y, cwid, st); break;
    }
    if (SU.step + 1 < SS__N) {
        char lab[64]; pl_fmt_ui(lab, sizeof lab, "Next: %s", SS_TITLE[SU.step + 1]);
        float nb = ui_button_width(ICON_ARROW_RIGHT, lab);
        if (ui_button(ui_id("su-next"), (Rectangle){ cx, y + 14, nb, 40 }, ICON_ARROW_RIGHT, lab, BTN_PRIMARY)) { SU.step++; ui_scroll_to(&A.scr[7], 0); }
        y += 66;
    }
    if (rail && y < ry) y = ry;
    return y + 16 - y0;
}
