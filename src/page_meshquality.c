/* page_meshquality.c -- Mesh > Quality ("every mesher" plan, step 8): how good the mesh on disk is, from checkMesh (any
 * mesh) and SALOME's quality controls (a mesh SALOME made or imported: salome/mesh.unv, imported.med or .unv), with the worst cells located;
 * and the mesh exported for other programs: OpenFOAM's converters (VTK, Fluent, STAR-CD, EnSight, its surface as STL) and
 * SALOME's (MED, UNV, CGNS, GMF), into <case>/export. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "salometool.h"

/* ---- checkMesh's log: the numbers that say whether the solver will cope */
typedef struct {
    char   path[PL_PATH]; time_t mt;
    long   cells, hexa, prism, tet, poly, pyr;
    double nonOrtho, skew, aspect, minVol;
    bool   have, ok;
    int    nFail;
    char   fail[8][200];
} CmLog;
static CmLog CM;

/* a small note under the controls (as the STL mesher's) */
static float note(float x, float y, float w, const char *t)
{
    return ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y, w, 200 }, 19, TH.text3, true) + 12;
}

static double after(const char *t, const char *key)
{
    const char *p = strstr(t, key);
    return p ? atof(p + strlen(key)) : -1;
}

static void cm_read(const Case *c)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "log.checkMesh");
    time_t mt = pl_mtime(p);
    if (!strcmp(CM.path, p) && mt == CM.mt) return;
    memset(&CM, 0, sizeof CM);
    pl_strlcpy(CM.path, p, sizeof CM.path); CM.mt = mt;
    char *t = mt ? pl_read_file(p, NULL) : NULL;
    if (!t) return;
    CM.have = true;
    CM.cells = (long)after(t, "    cells:");
    CM.hexa = (long)after(t, "hexahedra:"); CM.prism = (long)after(t, "prisms:"); CM.tet = (long)after(t, "tetrahedra:");
    CM.poly = (long)after(t, "polyhedra:"); CM.pyr = (long)after(t, "pyramids:");
    CM.nonOrtho = after(t, "Mesh non-orthogonality Max: ");
    CM.skew = after(t, "Max skewness = ");
    CM.aspect = after(t, "Max aspect ratio = ");
    CM.minVol = after(t, "Min volume = ");
    CM.ok = strstr(t, "Mesh OK.") != NULL;
    for (char *l = strtok(t, "\n"); l && CM.nFail < 8; l = strtok(NULL, "\n")) {
        char *q = strstr(l, "***");
        if (q) pl_strlcpy(CM.fail[CM.nFail++], q + 3, sizeof CM.fail[0]);
    }
    free(t);
}

/* ---- SALOME's report: its answer kept for the case it was made for */
static struct { char casePath[PL_PATH]; QJ *ans; char exportMsg[400]; } QR;

static bool salome_mesh_file(const Case *c, char *out, size_t cap)
{
    static const char *const F[] = { "salome/mesh.unv", "salome/imported.med", "salome/imported.unv", NULL };
    for (int i = 0; F[i]; i++) { pl_join(out, cap, c->s.path, F[i]); if (pl_is_file(out)) return true; }
    out[0] = 0;
    return false;
}

static void bars(float x, float y, float w, float h, const QJ *bins)
{
    int n = qj_len(bins);
    if (n <= 0) return;
    double mx = 1;
    for (int i = 0; i < n; i++) mx = fmax(mx, qj_num(qj_get(qj_at(bins, i), "count"), 0));
    float bw = w / n;
    for (int i = 0; i < n; i++) {
        double v = qj_num(qj_get(qj_at(bins, i), "count"), 0);
        float bh = v > 0 ? fmaxf(2, (float)(log1p(v) / log1p(mx)) * h) : 0;   /* log: the few bad cells still show */
        ui_rect((Rectangle){ x + i * bw + 1, y + h - bh, bw - 2, bh }, 2, i >= n - 2 && v > 0 ? TH.warn : TH.accent);
    }
    ui_line(x, y + h, x + w, y + h, 1, TH.lineSoft);
}

static float salome_report(const Case *c, float x, float y, float w)
{
    float y0 = y;
    char mf[PL_PATH];
    bool have = salome_mesh_file(c, mf, sizeof mf);
    y += section_head(x, y, w, "SALOME's quality report", "SALOME's own measures on the mesh it made or imported: aspect ratio, cell "
                                                          "volume, edge length, the boundary faces' skewness and warping, as "
                                                          "histograms (log scale: a handful of bad cells still shows), and where the "
                                                          "worst cells are.");
    if (strcmp(QR.casePath, c->s.path)) { qj_free(QR.ans); QR.ans = NULL; QR.exportMsg[0] = 0; pl_strlcpy(QR.casePath, c->s.path, sizeof QR.casePath); }
    QJ *a = stool_take("quality");
    if (a) { qj_free(QR.ans); QR.ans = a; }
    a = stool_take("export");
    if (a) {
        if (qj_true(qj_get(a, "ok"))) pl_fmt_ui(QR.exportMsg, sizeof QR.exportMsg, "Wrote %s", qj_str(qj_get(a, "out"), ""));
        else pl_fmt_ui(QR.exportMsg, sizeof QR.exportMsg, "Not exported: %s", qj_str(qj_get(a, "error"), ""));
        ui_toast(qj_true(qj_get(a, "ok")) ? TOAST_OK : TOAST_ERR, "%s", QR.exportMsg);
        qj_free(a);
    }
    if (!have) {
        y += note(x, y, w, "This mesh wasn't made by SALOME (no salome/mesh.unv or imported mesh): checkMesh's numbers above are the report.");
        return y - y0;
    }
    bool busy = stool_busy("quality");
    ui_disable(busy || !A.env.salome[0] || stool_busy(NULL));
    float bw = ui_button_width(ICON_CHART_LINE, "Make the report");
    if (ui_button(ui_id("mq-report"), (Rectangle){ x, y, bw, 36 }, ICON_CHART_LINE, busy ? "Measuring..." : "Make the report", BTN_PRIMARY)) {
        char q[PL_PATH * 2], r[PL_PATH * 2 + 64]; stool_quote(q, sizeof q, mf);
        snprintf(r, sizeof r, "{\"cmd\": \"quality\", \"mesh\": \"%s\"}", q);
        stool_run(c->s.path, "quality", r);
    }
    ui_disable(false);
    ui_text_clip(F_MONO, 12.5f, pl_basename(mf), x + bw + 14, y + 10, w - bw - 14, TH.text3);
    y += 48;
    if (busy) { y += note(x, y, w, TextFormat("SALOME is measuring the cells  ·  %.0f s", stool_seconds())); return y - y0; }
    if (!QR.ans) return y - y0;
    if (!qj_true(qj_get(QR.ans, "ok"))) { y += note(x, y, w, TextFormat("No report: %s", qj_str(qj_get(QR.ans, "error"), ""))); return y - y0; }
    const QJ *ms = qj_get(QR.ans, "measures");
    float cw = (w - 16) / 2, ch = 112;
    for (int i = 0; i < qj_len(ms); i++) {
        const QJ *m = qj_at(ms, i);
        float cx = x + (i % 2) * (cw + 16), cy = y + (i / 2) * (ch + 12);
        ui_rect((Rectangle){ cx, cy, cw, ch }, 10, TH.surface2);
        ui_text_clip(F_MED, 13, qj_str(qj_get(m, "what"), ""), cx + 12, cy + 8, cw - 24, TH.text);
        char r[96]; snprintf(r, sizeof r, "%.4g  to  %.4g", qj_num(qj_get(m, "min"), 0), qj_num(qj_get(m, "max"), 0));
        ui_text(F_MONO, 12.5f, r, cx + 12, cy + 28, TH.text2);
        bars(cx + 12, cy + 50, cw - 24, 50, qj_get(m, "bins"));
    }
    y += ((qj_len(ms) + 1) / 2) * (ch + 12) + 6;
    const QJ *wst = qj_get(QR.ans, "worst");
    if (qj_len(wst)) {
        char h[160]; snprintf(h, sizeof h, "The worst cells by aspect ratio (%d above %.3g):", (int)qj_num(qj_get(QR.ans, "over_threshold"), 0),
                              qj_num(qj_get(QR.ans, "worst_above"), 0));
        ui_text(F_MED, 13, h, x, y, TH.text2); y += 24;
        for (int i = 0; i < qj_len(wst) && i < 8; i++) {
            const QJ *e = qj_at(wst, i), *at = qj_get(e, "at");
            char l[200]; snprintf(l, sizeof l, "cell %-9d aspect ratio %-9.4g at (%.4g, %.4g, %.4g) m", (int)qj_num(qj_get(e, "id"), 0),
                                  qj_num(qj_get(e, "aspect_ratio"), 0), qj_num(qj_at(at, 0), 0), qj_num(qj_at(at, 1), 0), qj_num(qj_at(at, 2), 0));
            ui_text_clip(F_MONO, 12.5f, l, x + 8, y, w - 8, TH.text3);
            y += 20;
        }
        y += 6;
    }
    return y - y0;
}

/* ---- export */
static void export_foam(const Case *c, int k)
{
    static const char *const CMD[] = {
        "foamToVTK -constant -noZero -no-fields && mkdir -p export && rm -rf export/VTK && mv VTK export/ && echo 'Q-Continuum: wrote export/VTK'",
        "foamMeshToFluent && mkdir -p export && mv -f fluentInterface/*.msh export/ && rm -rf fluentInterface && echo 'Q-Continuum: wrote export/*.msh (Fluent)'",
        "mkdir -p export && foamToSurface -constant -noZero export/surface.stl && echo 'Q-Continuum: wrote export/surface.stl'",
        "foamToStarMesh -constant -noZero -scale 1 && mkdir -p export && mv -f *.pcs *.vrt *.cel *.bnd *.inp export/ 2>/dev/null; echo 'Q-Continuum: wrote export/ (STAR-CD, metres)'",
        "foamToEnsight -constant -noZero -no-fields && mkdir -p export && rm -rf export/EnSight && mv EnSight export/ && echo 'Q-Continuum: wrote export/EnSight'" };
    static const char *const LAB[] = { "Export VTK", "Export Fluent", "Export STL surface", "Export STAR-CD", "Export EnSight" };
    (void)c;
    app_start_tool("Export", "export", LAB[k], CMD[k], "log.export");
}

static float export_section(const Case *c, float x, float y, float w)
{
    float y0 = y;
    y += section_head(x, y, w, "Export the mesh", "For other programs, into the case's export/ folder. OpenFOAM's converters work on any mesh; MED, UNV, "
                                                  "CGNS and GMF come from the SALOME mesh, when there is one.");
    static const char *const FO[] = { "VTK", "Fluent .msh", "STL surface", "STAR-CD", "EnSight" };
    float bx = x, rowY = y;
    ui_disable(A.cj->active || !c->s.meshed);
    for (int k = 0; k < 5; k++) {
        float bw = ui_button_width(NULL, FO[k]);
        if (bx + bw > x + w) { bx = x; rowY += 44; }
        if (ui_button(ui_idn(ui_id("mq-exp"), k), (Rectangle){ bx, rowY, bw, 34 }, NULL, FO[k], BTN_SECONDARY)) export_foam(c, k);
        bx += bw + 10;
    }
    ui_disable(false);
    y = rowY + 46;
    char mf[PL_PATH];
    if (salome_mesh_file(c, mf, sizeof mf)) {
        static const char *const SA[] = { "MED", "UNV", "CGNS", "GMF" }, *const EXT[] = { "med", "unv", "cgns", "mesh" };
        bx = x;
        ui_disable(stool_busy(NULL) || !A.env.salome[0]);
        for (int k = 0; k < 4; k++) {
            char lab[32]; snprintf(lab, sizeof lab, "SALOME %s", SA[k]);
            float bw = ui_button_width(NULL, lab);
            if (bx + bw > x + w) { bx = x; y += 44; }
            if (ui_button(ui_idn(ui_id("mq-sexp"), k), (Rectangle){ bx, y, bw, 34 }, NULL, lab, BTN_SECONDARY)) {
                char out[PL_PATH], q1[PL_PATH * 2], q2[PL_PATH * 2], r[PL_PATH * 4 + 96], f[32];
                char d[PL_PATH]; pl_join(d, sizeof d, c->s.path, "export"); pl_mkdirs(d);
                snprintf(f, sizeof f, "mesh.%s", EXT[k]); pl_join(out, sizeof out, d, f);
                stool_quote(q1, sizeof q1, mf); stool_quote(q2, sizeof q2, out);
                snprintf(r, sizeof r, "{\"cmd\": \"export\", \"mesh\": \"%s\", \"format\": \"%s\", \"out\": \"%s\"}", q1, EXT[k], q2);
                stool_run(c->s.path, "export", r);
            }
            bx += bw + 10;
        }
        ui_disable(false);
        y += 46;
    }
    char d[PL_PATH]; pl_join(d, sizeof d, c->s.path, "export");
    if (pl_exists(d)) {
        float ow = ui_button_width(ICON_FOLDER_OPEN, "Open export/");
        if (ui_button(ui_id("mq-open"), (Rectangle){ x, y, ow, 32 }, ICON_FOLDER_OPEN, "Open export/", BTN_GHOST)) launch_folder(&A.env, d);
        if (QR.exportMsg[0]) ui_text_clip(F_REG, 13, QR.exportMsg, x + ow + 14, y + 8, w - ow - 14, TH.text3);
        y += 44;
    }
    return y - y0;
}

float page_meshquality(Case *c, float x, float y, float w)
{
    float y0 = y;
    cm_read(c);
    if (!c->s.meshed) { y += note(x, y, w, "No mesh yet: generate or import one, then its quality shows here."); return y - y0; }
    y += section_head(x, y, w, "checkMesh", "OpenFOAM's own check of the mesh on disk. Non-orthogonality over 70 degrees and skewness over 4 "
                                           "are where solvers start to struggle; a failed check names what to fix.");
    ui_disable(A.cj->active);
    float fw = ui_button_width(ICON_CIRCLE_CHECK, "Check everything");
    if (ui_button(ui_id("mq-full"), (Rectangle){ x + w - fw, y - 44, fw, 34 }, ICON_CIRCLE_CHECK, "Check everything", BTN_SECONDARY))
        app_start_tool("checkMesh", "checkMesh", "checkMesh (all checks)", "checkMesh -allGeometry -allTopology", "log.checkMesh");
    ui_disable(false);
    if (!CM.have) { y += note(x, y, w, "No log.checkMesh yet: Check everything runs it."); }
    else {
        float gap = 12, tw = (w - 4 * gap) / 5, th = 86;
        char v[5][48];
        snprintf(v[0], 48, "%ld", CM.cells);
        snprintf(v[1], 48, CM.nonOrtho >= 0 ? "%.1f°" : "-", CM.nonOrtho);
        snprintf(v[2], 48, CM.skew >= 0 ? "%.2f" : "-", CM.skew);
        snprintf(v[3], 48, CM.aspect >= 0 ? "%.3g" : "-", CM.aspect);
        snprintf(v[4], 48, "%d", CM.nFail);
        static const char *const L[5] = { "Cells", "Max non-orthogonality", "Max skewness", "Max aspect ratio", "Failed checks" };
        static const char *const I[5] = { ICON_BOXES, ICON_GAUGE, ICON_ACTIVITY, ICON_SIGMA, ICON_TRIANGLE_ALERT };
        char types[160]; snprintf(types, sizeof types, "%ld hex · %ld prism · %ld tet · %ld poly", CM.hexa > 0 ? CM.hexa : 0, CM.prism > 0 ? CM.prism : 0,
                                  CM.tet > 0 ? CM.tet : 0, CM.poly > 0 ? CM.poly : 0);
        for (int k = 0; k < 5; k++) stat_tile((Rectangle){ x + k * (tw + gap), y, tw, th }, I[k], L[k], v[k], k == 0 ? types : NULL);
        y += th + 16;
        if (CM.nFail) {
            Str d; str_init(&d);
            for (int i = 0; i < CM.nFail; i++) str_catf(&d, "%s%s", i ? "\n" : "", CM.fail[i]);
            float ch = callout_height(w, "What checkMesh flags", d.s);
            callout((Rectangle){ x, y, w, ch }, CM.ok ? TOAST_INFO : TOAST_WARN, ICON_TRIANGLE_ALERT, "What checkMesh flags", d.s);
            y += ch + 14;
            str_free(&d);
        } else if (CM.ok) {
            const char *d = "Every check passed.";
            float ch = callout_height(w, NULL, d);
            callout((Rectangle){ x, y, w, ch }, TOAST_OK, ICON_CIRCLE_CHECK, NULL, d);
            y += ch + 14;
        }
    }
    y += 10;
    y += salome_report(c, x, y, w) + 10;
    y += export_section(c, x, y, w);
    return y - y0;
}
