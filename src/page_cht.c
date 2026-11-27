/* page_cht.c -- Physics > Setup for conjugate heat transfer (improvement plan A2, 2026-11-26): the mesh's cellZones as
 * fluid and solid regions with their materials, the outer boundaries, the run, and Write case files. The model and the
 * files are chtsetup.c's; this page edits cht.ini and writes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "chtsetup.h"
#include "safety.h"

static struct { char path[PL_PATH]; ChtSetup s; bool ok, stale; char kept[600]; double armQuit; } C;

static void cht_sync(Case *c)
{
    if (strcmp(C.path, c->s.path)) {
        pl_strlcpy(C.path, c->s.path, sizeof C.path);
        C.ok = cht_load(c->s.path, &C.s);
        C.stale = false; C.kept[0] = 0; C.armQuit = 0;
    }
}
static void changed(void) { cht_save(C.path, &C.s); C.stale = true; }

bool cht_start(Case *c, char *err, int cap)
{
    ChtSetup s;
    if (!cht_from_mesh(c->s.path, &s, err, cap)) return false;
    if (!cht_save(c->s.path, &s)) { snprintf(err, (size_t)cap, "Could not write cht.ini"); return false; }
    C.path[0] = 0;                                           /* read again */
    return true;
}

static bool num(UiId id, Rectangle r, double *v, double lo, double hi, const char *unit)
{
    char b[64]; snprintf(b, sizeof b, "%.6g", *v);
    float uw = unit ? ui_text_w(F_MONO, 12, unit) + 12 : 0;
    if (unit) ui_text(F_MONO, 12, unit, r.x + r.width - uw + 6, r.y + (r.height - 12) / 2 - 1, TH.text3);
    if (!ui_input(id, (Rectangle){ r.x, r.y, r.width - uw, r.height }, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) return false;
    char *e; double d = strtod(b, &e);
    while (e && *e == ' ') e++;
    if (e == b || (e && *e) || !(d >= lo && d <= hi)) { ui_toast(TOAST_ERR, "Give a number from %g to %g", lo, hi); return false; }
    *v = d;
    return true;
}

static void write_now(Case *c, bool go)
{
    char err[1500];
    if (A.set.history) hist_commit(c->s.path, "Before Physics > Setup wrote the conjugate heat transfer case");
    if (!cht_write(c->s.path, &C.s, C.kept, sizeof C.kept, err, sizeof err)) { ui_toast(TOAST_ERR, "Not written: %s", err); return; }
    C.stale = false;
    case_rescan(c);
    if (C.kept[0]) ui_toast(TOAST_WARN, "Written; kept as you edited them: %s", C.kept);
    else ui_toast(TOAST_OK, "Written for %s: %d regions. The Run page splits the mesh into them first.", C.s.transient ? "chtMultiRegionFoam" : "chtMultiRegionSimpleFoam", C.s.nR);
    if (go) app_go(PG_RUN);
}

static float regions(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Regions", "One per cellZone of the mesh. A fluid moves and carries heat; a solid conducts it and can make it "
                                          "(a chip, a heater). Where two regions meet, their temperatures and heat flows are matched.");
    static const char *KIND[CR__N] = { "Fluid", "Solid" };
    const char *fl[8], *so[16];
    for (int i = 0; i < CHT_NFLUIDS && i < 8; i++) fl[i] = CHT_FLUIDS[i].name;
    for (int i = 0; i < CHT_NSOLIDS && i < 16; i++) so[i] = CHT_SOLIDS[i].name;
    for (int i = 0; i < C.s.nR; i++) {
        ChtRegion *r = &C.s.r[i];
        Rectangle box = { x, y, w, r->kind == CR_SOLID ? 128 : 104 };
        ui_rect(box, 10, TH.surface2); ui_rect_lines(box, 10, 1, TH.lineSoft);
        float cx = x + 16, cy = y + 14;
        ui_icon(r->kind == CR_SOLID ? ICON_BOX : ICON_DROPLETS, cx + 8, cy + 9, 16, TH.accent);
        ui_text_clip(F_SEMI, 15, r->name, cx + 26, cy, 220, TH.text);
        if (r->rest) ui_text_clip(F_REG, 12, "the cells in no zone", cx + 26, cy + 20, 220, TH.text3);
        char id[96];
        float sx = x + 270, sw = fminf(200, w - 290);
        int k = r->kind;
        snprintf(id, sizeof id, "cht-kind:%s", r->name);
        if (ui_segmented(ui_id(id), (Rectangle){ sx, cy - 4, sw, 32 }, KIND, CR__N, &k) && k != r->kind) { r->kind = k; r->mat = 0; changed(); }
        float mx = sx + sw + 12, mw = fmaxf(160, w - (mx - x) - 16);
        int m = r->mat;
        snprintf(id, sizeof id, "cht-mat:%s", r->name);
        if (ui_select(ui_id(id), (Rectangle){ mx, cy - 4, mw, 32 }, r->kind == CR_SOLID ? so : fl, r->kind == CR_SOLID ? CHT_NSOLIDS : CHT_NFLUIDS, &m) && m != r->mat) {
            r->mat = m; changed();
        }
        const ChtMat *mt = r->kind == CR_SOLID ? &CHT_SOLIDS[r->mat] : &CHT_FLUIDS[r->mat];
        char props[200];
        if (r->kind == CR_SOLID) pl_fmt_ui(props, sizeof props, "k %.3g W/(m K) · ρ %.4g kg/m³ · cp %.4g J/(kg K) · diffusivity %.2e m²/s", mt->k, mt->rho, mt->cp, cht_diffusivity(mt));
        else pl_fmt_ui(props, sizeof props, "k %.3g W/(m K) · ρ %.4g kg/m³ · cp %.4g J/(kg K) · μ %.3g Pa s · Pr %.3g%s", mt->k, mt->rho, mt->cp, mt->mu,
                       cht_prandtl(mt), mt->gas ? " · ideal gas" : "");
        ui_text_clip(F_REG, 12.5f, props, cx, cy + 44, w - 32, TH.text3);
        float ry = cy + 66;
        ui_text(F_REG, 13, "Starts at", cx, ry + 8, TH.text2);
        snprintf(id, sizeof id, "cht-T0:%s", r->name);
        if (num(ui_id(id), (Rectangle){ cx + 80, ry, 130, 32 }, &r->T0, 1, 5000, "K")) changed();
        if (r->kind == CR_SOLID) {
            ui_text(F_REG, 13, "Heat made", cx + 240, ry + 8, TH.text2);
            snprintf(id, sizeof id, "cht-Q:%s", r->name);
            if (num(ui_id(id), (Rectangle){ cx + 320, ry, 140, 32 }, &r->Q, -1e9, 1e9, "W")) changed();
            ui_text_clip(F_REG, 12.5f, r->Q != 0 ? "spread evenly through the region (a chip's or a heater's power)" : "none: it only conducts",
                         cx + 474, ry + 9, w - 506, TH.text3);
        }
        y += box.height + 10;
    }
    return y - y0;
}

static float boundaries(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Outer boundaries", "The mesh's patches. Inlets and outlets belong to a fluid; a wall can lose or take heat. "
                                                   "The faces between regions aren't listed: they are coupled.");
    static const char *ROLE[CP__N] = { "Wall", "Inlet", "Outlet" };
    for (int i = 0; i < C.s.nP; i++) {
        ChtPatch *p = &C.s.p[i];
        char id[96];
        ui_icon(p->role == CP_WALL ? ICON_PANEL_LEFT : p->role == CP_INLET ? ICON_ARROW_RIGHT : ICON_EXTERNAL_LINK, x + 10, y + 16, 14, TH.text3);
        ui_text_clip(F_MED, 14, p->name, x + 26, y + 8, 180, TH.text);
        int ro = p->role;
        snprintf(id, sizeof id, "cht-role:%s", p->name);
        if (ui_segmented(ui_id(id), (Rectangle){ x + 214, y, 240, 32 }, ROLE, CP__N, &ro) && ro != p->role) { p->role = ro; changed(); }
        float vx = x + 470, vw = w - 470;
        if (p->role == CP_INLET) {
            snprintf(id, sizeof id, "cht-U:%s", p->name);
            if (num(ui_id(id), (Rectangle){ vx, y, 130, 32 }, &p->U, 0, 1e4, "m/s")) changed();
            snprintf(id, sizeof id, "cht-Tin:%s", p->name);
            if (num(ui_id(id), (Rectangle){ vx + 140, y, 120, 32 }, &p->Tin, 1, 5000, "K")) changed();
            ui_text_clip(F_REG, 12.5f, "into the domain", vx + 272, y + 9, vw - 272, TH.text3);
        } else if (p->role == CP_OUTLET) {
            ui_text_clip(F_REG, 12.5f, "held at 1 bar", vx, y + 9, vw, TH.text3);
        } else {
            int h = p->heat;
            snprintf(id, sizeof id, "cht-heat:%s", p->name);
            float sw = fminf(250, vw * 0.5f);
            if (ui_select(ui_id(id), (Rectangle){ vx, y, sw, 32 }, CHT_HEAT_NAME, CT__N, &h) && h != p->heat) { p->heat = h; changed(); }
            float ax = vx + sw + 10;
            snprintf(id, sizeof id, "cht-hv:%s", p->name);
            if (p->heat == CT_FIXED && num(ui_id(id), (Rectangle){ ax, y, 120, 32 }, &p->T, 1, 5000, "K")) changed();
            if (p->heat == CT_FLUX && num(ui_id(id), (Rectangle){ ax, y, 150, 32 }, &p->q, -1e9, 1e9, "W/m²")) changed();
            if (p->heat == CT_CONVECT) {
                if (num(ui_id(id), (Rectangle){ ax, y, 150, 32 }, &p->h, 0, 1e7, "W/m²K")) changed();
                snprintf(id, sizeof id, "cht-Ta:%s", p->name);
                if (num(ui_id(id), (Rectangle){ ax + 160, y, 110, 32 }, &p->Ta, 1, 5000, "K")) changed();
            }
        }
        y += 42;
    }
    if (!C.s.nP) y += ui_text_wrap(F_REG, 13, "The mesh has no outer patches (other than empty or symmetry ones).", (Rectangle){ x, y, w, 60 }, 19, TH.text3, true) + 8;
    return y - y0;
}

static float run_rows(float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "The run", "Steady finds where the temperatures settle; transient follows them in time (a part warming up).");
    static const char *MODE[2] = { "Steady", "Transient" };
    Rectangle cr = prop_row(x, &y, w, "Time", NULL, 34);
    int md = C.s.transient;
    if (ui_segmented(ui_id("cht-mode"), (Rectangle){ cr.x, cr.y, 240, 34 }, MODE, 2, &md) && md != C.s.transient) {
        C.s.transient = md;
        C.s.endTime = md ? 60 : 1000; C.s.writeEvery = md ? 10 : 100;
        changed();
    }
    cr = prop_row(x, &y, w, C.s.transient ? "End time" : "Iterations", NULL, 34);
    if (num(ui_id("cht-end"), (Rectangle){ cr.x, cr.y, 170, 34 }, &C.s.endTime, 1, 1e9, C.s.transient ? "s" : NULL)) changed();
    cr = prop_row(x, &y, w, "Write every", NULL, 34);
    if (num(ui_id("cht-write"), (Rectangle){ cr.x, cr.y, 170, 34 }, &C.s.writeEvery, 1e-6, 1e9, C.s.transient ? "s" : NULL)) changed();
    cr = prop_row(x, &y, w, "Turbulence", "In the fluids: k-omega SST with wall functions, or laminar (Re below about 2000 in a channel or pipe).", 34);
    if (ui_switch(ui_id("cht-turb"), cr.x, cr.y + 7, &C.s.turbulent)) changed();
    ui_text(F_REG, 13, C.s.turbulent ? "k-omega SST" : "Laminar", cr.x + 56, cr.y + 9, TH.text2);
    cr = prop_row(x, &y, w, "Gravity", "On: warm fluid rises (buoyancy). Off: heat is carried only by the flow you drive.", 34);
    bool g = fabs(C.s.g[0]) + fabs(C.s.g[1]) + fabs(C.s.g[2]) > 0;
    if (ui_switch(ui_id("cht-g"), cr.x, cr.y + 7, &g)) { C.s.g[0] = C.s.g[2] = 0; C.s.g[1] = g ? -9.81 : 0; changed(); }
    ui_text(F_REG, 13, g ? TextFormat("(%.3g %.3g %.3g) m/s²", C.s.g[0], C.s.g[1], C.s.g[2]) : "Off", cr.x + 56, cr.y + 9, TH.text2);
    return y - y0;
}

float page_cht(Case *c, float x, float y, float w)
{
    float y0 = y;
    cht_sync(c);
    if (!C.ok) {
        y += ui_text_wrap(F_REG, 14, "cht.ini could not be read.", (Rectangle){ x, y, w, 40 }, 20, TH.warn, true) + 10;
        return y - y0;
    }
    int nf = 0, ns = 0;
    for (int i = 0; i < C.s.nR; i++) { if (C.s.r[i].kind == CR_FLUID) nf++; else ns++; }
    char probs[1500], warn[900], t[200], d[1200];
    cht_check(&C.s, probs, sizeof probs, warn, sizeof warn);
    pl_fmt_ui(t, sizeof t, "Heat through solids · %s · %d fluid%s, %d solid%s", C.s.transient ? "chtMultiRegionFoam" : "chtMultiRegionSimpleFoam",
              nf, nf == 1 ? "" : "s", ns, ns == 1 ? "" : "s");
    if (probs[0]) pl_fmt_ui(d, sizeof d, "Before it can be written: %s", probs);
    else pl_fmt_ui(d, sizeof d, "%s%sWrite case files puts each region's files into constant/, system/ and 0.orig/; the Run page then splits the mesh into the regions.",
                   C.stale ? "Changed since the case files were written. " : "", warn[0] ? warn : "");
    float bw1 = ui_button_width(ICON_FILE_TEXT, "Write case files"), bw2 = ui_button_width(ICON_PLAY, "Write and go to Run");
    bool beside = w >= 980;
    float cw = beside ? w - bw1 - bw2 - 36 : w, ch = callout_height(cw, t, d);
    callout((Rectangle){ x, y, cw, ch }, probs[0] ? TOAST_WARN : C.stale ? TOAST_INFO : TOAST_OK, probs[0] ? ICON_TRIANGLE_ALERT : ICON_THERMOMETER, t, d);
    float by = beside ? y + (ch - 40) / 2 : y + ch + 12, bx = beside ? x + cw + 16 : x;
    ui_disable(A.cj->active || probs[0]);
    if (ui_button(ui_id("cht-write-files"), (Rectangle){ bx, by, bw1, 40 }, ICON_FILE_TEXT, "Write case files", BTN_PRIMARY)) write_now(c, false);
    if (ui_button(ui_id("cht-write-run"), (Rectangle){ bx + bw1 + 10, by, bw2, 40 }, ICON_PLAY, "Write and go to Run", BTN_SECONDARY)) write_now(c, true);
    ui_disable(false);
    y += beside ? ch + 22 : ch + 12 + 40 + 22;
    y += regions(x, y, w) + 12;
    y += boundaries(x, y, w) + 16;
    y += run_rows(x, y, w) + 16;
    /* the way back: a single-region setup again (cht.ini to the trash, the written files stay) */
    double now = GetTime();
    bool armed = C.armQuit > 0 && now - C.armQuit < 4;
    const char *lab = armed ? "Click again: leave heat through solids" : "Back to a single-region setup";
    float lw = ui_button_width(ICON_X, lab);
    if (ui_button(ui_id("cht-quit"), (Rectangle){ x, y, lw, 34 }, ICON_X, lab, BTN_GHOST)) {
        if (!armed) C.armQuit = now;
        else {
            char p[PL_PATH], q[PL_PATH]; pl_join(p, sizeof p, c->s.path, "cht.ini"); pl_join(q, sizeof q, c->s.path, "cht.ini.old");
            rename(p, q); C.path[0] = 0; A.physicsTab = 2;
            ui_toast(TOAST_INFO, "cht.ini kept as cht.ini.old; the case files written from it stay.");
        }
    }
    y += 46;
    return y - y0;
}
