/* page_meshtools.c -- Mesh > Tools (mesh overhaul plan step 10, gaps E6 E7): the patches of the mesh on disk, renamed or
 * given another type, and OpenFOAM's everyday mesh utilities as forms (move / scale / turn, mirror, refine, renumber, a cell
 * zone from a box, merge patches, baffles, check a surface), each run as a job with checkMesh after it. The commands and
 * dictionaries are meshtools.c's. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "meshtools.h"

static const char *const PTYPE[] = { "patch", "wall", "symmetryPlane", "symmetry", "empty", "wedge" };
#define NPTYPE 6

static struct {
    char path[PL_PATH];
    char names[256][64], types[256][32]; int n; double readAt; long long sig;
    char edit[256][64];
    int  tool;
    MtIn in;
    char zones[64][64]; int nZones;
} M = { .tool = MT_TRANSFORM };

static long long bnd_sig(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "constant/polyMesh/boundary");
    return pl_is_file(p) ? (long long)pl_mtime(p) * 1000003LL + pl_file_size(p) : 0;
}

static void reread(Case *c)
{
    M.n = mt_patches(c->s.path, M.names, M.types, 256);
    for (int i = 0; i < M.n; i++) pl_strlcpy(M.edit[i], M.names[i], sizeof M.edit[i]);
    M.nZones = mt_face_zones(c->s.path, M.zones, 64);
    M.sig = bnd_sig(c->s.path);
    M.readAt = GetTime();
}

static void defaults(Case *c)
{
    memset(&M.in, 0, sizeof M.in);
    M.in.scale = 1; M.in.rotAxis[2] = 1; M.in.normal[0] = 1;
    snprintf(M.in.name, sizeof M.in.name, "zone1");
    snprintf(M.in.type, sizeof M.in.type, "wall");
    (void)c;
}

/* ---- running */
static void run_tool(void);
static void run_after_clean(void) { run_tool(); }
static void run_tool(void)
{
    Case *c = A.cs;
    if (!c || !app_require_foam() || !app_can_start()) return;
    M.in.tool = M.tool;
    char cmd[2400], rel[200], dict[4096], err[300];
    if (!mt_script(&M.in, cmd, sizeof cmd, rel, sizeof rel, dict, sizeof dict, err, sizeof err)) { ui_toast(TOAST_ERR, "%s", err); return; }
    case_rescan(c);
    if (M.tool != MT_SURFACE && case_has_results(&c->s)) {   /* results on the mesh about to change would stop the next run */
        app_clean_then("The results were computed on the mesh this tool changes, so they go first.", run_after_clean);
        return;
    }
    if (rel[0]) { char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, rel); if (!pl_write_file(p, dict, strlen(dict))) { ui_toast(TOAST_ERR, "Could not write %s", rel); return; } }
    static const char *const LOG[MT__N] = { "log.transformPoints", "log.mirrorMesh", "log.refineMesh", "log.renumberMesh", "log.topoSet",
                                            "log.createPatch", "log.createBaffles", "log.surfaceCheck" };
    job_reset(A.cj, "Mesh tool");
    job_add(A.cj, "tool", MT_NAME[M.tool], cmd, LOG[M.tool], false);
    if (M.tool != MT_SURFACE) job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "%s: the Run page shows the log", MT_NAME[M.tool]);
    else ui_toast(TOAST_ERR, "Could not start %s", MT_NAME[M.tool]);
}

/* ---- the form's rows */
static bool num(const char *id, float x, float y, float w, double *v)
{
    char b[48];
    if (*v != 0 && (fabs(*v) < 1e-3 || fabs(*v) >= 1e6)) snprintf(b, sizeof b, "%.6g", *v); else snprintf(b, sizeof b, "%.8g", *v);
    if (!ui_input(ui_id(id), (Rectangle){ x, y, w, 32 }, b, sizeof b, NULL, INPUT_MONO | INPUT_NUMERIC)) return false;
    char *e; double d = strtod(b, &e);
    while (e && *e == ' ') e++;
    if (e == b || (e && *e) || !isfinite(d)) { ui_toast(TOAST_ERR, "Give a number"); return false; }
    *v = d;
    return true;
}

static float vec_row(const char *id, const char *label, float x, float y, float w, double *v)
{
    ui_text(F_REG, 13, label, x, y + 8, TH.text2);
    float lw = 150, bw = fminf(130, (w - lw - 16) / 3);
    for (int k = 0; k < 3; k++) { char i2[48]; snprintf(i2, sizeof i2, "%s%d", id, k); num(i2, x + lw + k * (bw + 8), y, bw, &v[k]); }
    return 40;
}

static float tool_form(Case *c, float x, float y, float w)
{
    float y0 = y;
    MtIn *in = &M.in;
    switch (M.tool) {
    case MT_TRANSFORM:
        ui_text(F_REG, 13, "Scale", x, y + 8, TH.text2); num("mt-scale", x + 150, y, 130, &in->scale);
        ui_text(F_REG, 12, "0.001: millimetres to metres", x + 292, y + 9, TH.text3); y += 40;
        y += vec_row("mt-rax", "Turn about the axis", x, y, w, in->rotAxis);
        ui_text(F_REG, 13, "by (degrees)", x, y + 8, TH.text2); num("mt-rdeg", x + 150, y, 130, &in->rotDeg); y += 40;
        y += vec_row("mt-mv", "Then move by (m)", x, y, w, in->move);
        break;
    case MT_MIRROR:
        y += vec_row("mt-mp", "Plane through", x, y, w, in->point);
        y += vec_row("mt-mn", "Its normal", x, y, w, in->normal);
        break;
    case MT_ZONE:
        ui_text(F_REG, 13, "Zone name", x, y + 8, TH.text2);
        ui_input(ui_id("mt-zname"), (Rectangle){ x + 150, y, 200, 32 }, in->name, sizeof in->name, NULL, INPUT_MONO);
        y += 40;
        y += vec_row("mt-zlo", "Box from (m)", x, y, w, in->lo);
        y += vec_row("mt-zhi", "to (m)", x, y, w, in->hi);
        break;
    case MT_MERGE: {
        ui_text(F_REG, 13, "Patches", x, y + 8, TH.text2);
        ui_input(ui_id("mt-mpatches"), (Rectangle){ x + 150, y, w - 150, 32 }, in->patches, sizeof in->patches, "names, space between", INPUT_MONO);
        y += 40;
        ui_text(F_REG, 13, "Into", x, y + 8, TH.text2);
        ui_input(ui_id("mt-mname"), (Rectangle){ x + 150, y, 200, 32 }, in->name, sizeof in->name, NULL, INPUT_MONO);
        int t = 1; for (int i = 0; i < NPTYPE; i++) if (!strcmp(in->type, PTYPE[i])) t = i;
        if (ui_select(ui_id("mt-mtype"), (Rectangle){ x + 360, y, 170, 32 }, PTYPE, NPTYPE, &t)) pl_strlcpy(in->type, PTYPE[t], sizeof in->type);
        y += 40;
        break;
    }
    case MT_BAFFLES: {
        ui_text(F_REG, 13, "Face zone", x, y + 8, TH.text2);
        if (!M.nZones) { ui_text(F_REG, 12.5f, "The mesh has no faceZones (snappyHexMesh's faceZone, or topoSet, makes them)", x + 150, y + 9, TH.text3); y += 40; break; }
        const char *zn[64]; int sel = -1;
        for (int i = 0; i < M.nZones; i++) { zn[i] = M.zones[i]; if (!strcmp(M.zones[i], in->zone)) sel = i; }
        if (ui_select_ph(ui_id("mt-bzone"), (Rectangle){ x + 150, y, 240, 32 }, zn, M.nZones, &sel, "a face zone") && sel >= 0) pl_strlcpy(in->zone, M.zones[sel], sizeof in->zone);
        y += 40;
        break;
    }
    case MT_SURFACE: {
        ui_text(F_REG, 13, "STL", x, y + 8, TH.text2);
        if (!in->file[0]) { char d[PL_PATH]; pl_join(d, sizeof d, c->s.path, "constant/triSurface/geometry.stl"); if (pl_is_file(d)) pl_strlcpy(in->file, d, sizeof in->file); }
        ui_input(ui_id("mt-sfile"), (Rectangle){ x + 150, y, w - 150, 32 }, in->file, sizeof in->file, "path to an .stl", INPUT_MONO);
        y += 40;
        break;
    }
    default: break;                                         /* refine, renumber: nothing to set */
    }
    return y - y0;
}

float meshtools_page(Case *c, float x, float y, float w)
{
    float y0 = y;
    if (strcmp(M.path, c->s.path)) { pl_strlcpy(M.path, c->s.path, sizeof M.path); defaults(c); reread(c); }
    if (GetTime() - M.readAt > 2 && bnd_sig(c->s.path) != M.sig) reread(c);   /* a tool or a new mesh changed it */
    bool busy = A.cj->active;
    /* ---- patches */
    y += section_head(x, y, w, "Patches", "The mesh's boundaries as they are in constant/polyMesh/boundary: rename one (the name changes in every field "
                      "file of 0/ too) or give it another type. Physics > Boundaries says what each one does.") + 4;
    if (!M.n) {
        const char *t = "No mesh yet: make one (or import one) and its patches show here.";
        float ch = callout_height(w, NULL, t);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, t);
        return y + ch + 10 - y0;
    }
    {   char mi[PL_PATH]; pl_join(mi, sizeof mi, c->s.path, "mesher/mesher.ini");
        if (pl_is_file(mi)) {
            const char *t = "This mesh comes from the STL mesher: rename its boundaries on the mesher's Boundaries step instead, or the next Generate brings the old names back.";
            float ch = callout_height(w, NULL, t);
            callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, t);
            y += ch + 10;
        }
    }
    ui_disable(busy);
    for (int i = 0; i < M.n; i++) {
        Rectangle row = { x, y, w, 40 };
        if (ui_culled(row.y, row.y + row.height)) { y += 44; continue; }
        ui_rect(row, 8, i % 2 ? TH.surface : TH.surface2);
        if (ui_input(ui_idn(ui_id("mt-pname"), i), (Rectangle){ x + 8, y + 4, fminf(300, w * 0.4f), 32 }, M.edit[i], sizeof M.edit[i], NULL, INPUT_MONO)
            && strcmp(M.edit[i], M.names[i])) {
            char err[300]; int files = 0;
            if (mt_rename_patch(c->s.path, M.names[i], M.edit[i], &files, err, sizeof err)) {
                ui_toast(TOAST_OK, "Renamed %s to %s (%d file%s)", M.names[i], M.edit[i], files, files == 1 ? "" : "s");
                case_rescan(c); reread(c);
            } else { ui_toast(TOAST_ERR, "%s", err); pl_strlcpy(M.edit[i], M.names[i], sizeof M.edit[i]); }
        }
        int t = -1; for (int k = 0; k < NPTYPE; k++) if (!strcmp(M.types[i], PTYPE[k])) t = k;
        float tx = x + 20 + fminf(300, w * 0.4f);
        if (ui_select_ph(ui_idn(ui_id("mt-ptype"), i), (Rectangle){ tx, y + 4, 180, 32 }, PTYPE, NPTYPE, &t, M.types[i]) && t >= 0 && strcmp(PTYPE[t], M.types[i])) {
            if (!app_require_foam() || !app_can_start()) break;
            /* the name from the mesh's boundary file, quoted: it reaches the shell (sweep 2026-11-22) */
            char cmd[600], ent[128], qe[160], msg[128], qm[160];
            snprintf(ent, sizeof ent, "entry0/%s/type", M.names[i]); pl_shell_quote(qe, sizeof qe, ent);
            snprintf(msg, sizeof msg, "patch %s: %s", M.names[i], PTYPE[t]); pl_shell_quote(qm, sizeof qm, msg);
            snprintf(cmd, sizeof cmd, "foamDictionary constant/polyMesh/boundary -entry %s -set %s && echo %s", qe, PTYPE[t], qm);
            job_reset(A.cj, "Mesh tool");
            job_add(A.cj, "type", "Patch type", cmd, "log.patchType", false);
            if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "%s becomes a %s patch: Physics > Boundaries sets its conditions", M.names[i], PTYPE[t]);
        }
        y += 44;
    }
    ui_disable(false);
    y += 10;
    /* ---- the utilities */
    y += section_head(x, y, w, "Mesh utilities", "OpenFOAM's tools for changing a mesh that is already made, each run with checkMesh after it. "
                      "Generate mesh makes the mesh again from its settings without them: run them again after it.") + 4;
    int t = M.tool;
    ui_text(F_REG, 13, "Tool", x, y + 8, TH.text2);
    if (ui_select(ui_id("mt-tool"), (Rectangle){ x + 150, y, 280, 32 }, MT_NAME, MT__N, &t)) M.tool = t;
    y += 42;
    y += ui_text_wrap(F_REG, 13, MT_WHAT[M.tool], (Rectangle){ x, y, w, 80 }, 19, TH.text2, true) + 10;
    y += tool_form(c, x, y, w);
    ui_disable(busy);
    float bw = ui_button_width(ICON_PLAY, "Run");
    if (ui_button(ui_id("mt-run"), (Rectangle){ x, y + 4, bw, 36 }, ICON_PLAY, "Run", BTN_PRIMARY)) run_tool();
    ui_disable(false);
    y += 52;
    return y - y0;
}
