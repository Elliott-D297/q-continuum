/* page_meshquality.c -- Mesh > Quality ("every mesher" plan, step 8): how good the mesh on disk is, from checkMesh (any
 * mesh) and SALOME's quality controls (a mesh SALOME made or imported: salome/mesh.unv, imported.med or .unv), with the worst cells located;
 * and the mesh exported for other programs: OpenFOAM's converters (VTK, Fluent, STAR-CD, EnSight, its surface as STL) and
 * SALOME's (MED, UNV, CGNS, GMF), into <case>/export. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "app.h"
#include "quality.h"
#include "salometool.h"

/* ---- checkMesh's log: the numbers that say whether the solver will cope */
typedef struct {
    char   path[PL_PATH]; long long mt;
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

/* a log's identity: modification time to the nanosecond and size. checkMesh can write its whole log within one second, so
   whole-second times read a half-written log and never again (2026-11-21: Check everything left the tiles at 0 cells) */
static long long file_sig(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return (long long)st.st_mtim.tv_sec * 1000000007LL + st.st_mtim.tv_nsec * 31LL + (long long)st.st_size;
}

static double after(const char *t, const char *key)
{
    const char *p = strstr(t, key);
    return p ? atof(p + strlen(key)) : -1;
}

static void cm_read(const Case *c)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "log.checkMesh");
    long long mt = file_sig(p);
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
    y += section_head(x, y, w, "SALOME's quality report", "SALOME's own measures on the mesh it made or imported: [[aspect-ratio|aspect ratio]], cell "
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

/* ---- the quality explainer (mesh overhaul plan step 4, gap L5): per measure a picture, what it does to the solution, the
   limits and the fix (quality.c); the flagged cells as one cellSet, shown in ParaView */
static QualReport QRep;
static char qrPath[PL_PATH]; static long long qrMt;
static int qOpen = -2;                                    /* the measure shown open; -2: the first problem, once a log is read */

static void qr_read(const Case *c)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, "log.checkMesh");
    long long mt = file_sig(p);
    if (!strcmp(qrPath, p) && mt == qrMt) return;
    bool other = strcmp(qrPath, p) != 0;
    pl_strlcpy(qrPath, p, sizeof qrPath); qrMt = mt;
    char *t = mt ? pl_read_file(p, NULL) : NULL;
    qual_read(t, &QRep);
    free(t);
    if (other || qOpen == -2) qOpen = qual_first_problem(&QRep);
}

static Color qcol(int st) { return st == QV_FAILED ? TH.err : st == QV_FLAGGED ? TH.warn : st == QV_OK ? TH.ok : TH.text3; }

/* the pictures: cells drawn as a hand sketch would, the measure's bad part in the warning colour */
static void dot(float x, float y, Color c) { DrawCircleV((Vector2){ x, y }, 3.5f, c); }
static void arrow(float x0, float y0, float x1, float y1, Color c)
{
    ui_line(x0, y0, x1, y1, 2, c);
    float a = atan2f(y1 - y0, x1 - x0), L = 9;
    ui_line(x1, y1, x1 - L * cosf(a - 0.45f), y1 - L * sinf(a - 0.45f), 2, c);
    ui_line(x1, y1, x1 - L * cosf(a + 0.45f), y1 - L * sinf(a + 0.45f), 2, c);
}
static void poly(const Vector2 *v, int n, float t, Color c) { for (int i = 0; i < n; i++) ui_line(v[i].x, v[i].y, v[(i + 1) % n].x, v[(i + 1) % n].y, t, c); }
static void label(const char *s, float x, float y, Color c) { ui_text(F_REG, 12.5f, s, x, y, c); }

static void picture(int k, Rectangle r)
{
    ui_rect(r, 8, TH.surface2);
    ui_clip_push(r);                                      /* labels stay inside the picture */
    Color ln = TH.text2, hi = TH.lcars ? TH.lc[1] : TH.accent, bad = TH.warn;
    float x = r.x + 20, y = r.y + 18, w = r.width - 40, h = r.height - 36, cx = x + w / 2, cy = y + h / 2;
    switch (k) {
    case QM_NONORTHO: {                                   /* sheared cells: the centre line misses the face normal */
        float s = h * 0.55f, cw = w * 0.36f;
        Vector2 L[4] = { { cx - cw - s / 2, y + h }, { cx - s / 2, y + h }, { cx + s / 2, y }, { cx - cw + s / 2, y } };
        Vector2 R[4] = { { cx - s / 2, y + h }, { cx + cw - s / 2, y + h }, { cx + cw + s / 2, y }, { cx + s / 2, y } };
        poly(L, 4, 1.5f, ln); poly(R, 4, 1.5f, ln);
        ui_line(cx - s / 2, y + h, cx + s / 2, y, 3, hi);                        /* the face */
        float px = cx - cw / 2, nx = cx + cw / 2;
        ui_line(px, cy, nx, cy, 1.5f, ln); dot(px, cy, ln); dot(nx, cy, ln);
        label("P", px - 4, cy + 6, ln); label("N", nx - 4, cy + 6, ln);
        float fx = s, fy = h, l = sqrtf(fx * fx + fy * fy), nxv = fy / l, nyv = fx / l;  /* normal to the face */
        arrow(cx, cy, cx + nxv * 46, cy + nyv * 46, bad);
        label("n", cx + nxv * 46 + 4, cy + nyv * 46 - 6, bad);
        label("θ", cx + 14, cy + 3, bad);
        break;
    }
    case QM_SKEW: {                                       /* unequal cells: the centre line crosses the face off its centre */
        float bw = w * 0.42f, sw = w * 0.30f, fx = x + bw + 10;
        Rectangle B = { x + 10, y, bw, h }, S = { fx, y + h * 0.05f, sw, h * 0.42f };
        ui_rect_lines(B, 0, 1.5f, ln); ui_rect_lines(S, 0, 1.5f, ln);
        ui_line(fx, S.y, fx, S.y + S.height, 3, hi);                            /* the shared face */
        float px = B.x + B.width / 2, py = B.y + B.height / 2, nx = S.x + S.width / 2, ny = S.y + S.height / 2;
        ui_line(px, py, nx, ny, 1.5f, ln); dot(px, py, ln); dot(nx, ny, ln);
        float t = (fx - px) / (nx - px), iy = py + t * (ny - py), fcy = S.y + S.height / 2;
        dot(fx, iy, bad); dot(fx, fcy, hi);
        ui_line(fx + 8, iy, fx + 8, fcy, 2, bad);
        label("line crosses", S.x + S.width + 8, iy - 2, bad); label("face centre", S.x + S.width + 8, fcy - 16, hi);
        break;
    }
    case QM_ASPECT: {
        Rectangle c = { x + 10, cy - h * 0.09f, w - 20, h * 0.18f };
        ui_rect_lines(c, 0, 2, bad);
        label("long", c.x + c.width / 2 - 12, c.y - 18, ln); label("short", c.x + c.width + 4 - 40, c.y + c.height + 4, ln);
        label("aspect ratio = long / short", x + 10, y + h - 14, ln);
        break;
    }
    case QM_VOLUME: {                                     /* an inside-out cell: its corners crossed */
        Vector2 ok[4] = { { x + 10, y + h }, { x + w * 0.4f, y + h }, { x + w * 0.4f, y + h * 0.3f }, { x + 10, y + h * 0.3f } };
        poly(ok, 4, 1.5f, ln); label("a cell", x + 14, y + h * 0.3f - 18, ln);
        float ox = x + w * 0.55f;
        Vector2 a = { ox, y + h }, b = { ox + w * 0.4f, y + h }, c2 = { ox, y + h * 0.3f }, d = { ox + w * 0.4f, y + h * 0.3f };
        Vector2 m = { ox + w * 0.2f, y + h * 0.65f };
        DrawTriangle(a, m, b, ui_alpha(bad, 0.25f)); DrawTriangle(c2, d, m, ui_alpha(bad, 0.25f));
        ui_line(a.x, a.y, b.x, b.y, 2, bad); ui_line(b.x, b.y, c2.x, c2.y, 2, bad); ui_line(c2.x, c2.y, d.x, d.y, 2, bad); ui_line(d.x, d.y, a.x, a.y, 2, bad);
        label("turned inside out", ox, y + h * 0.3f - 18, bad);
        break;
    }
    case QM_PYRAMID: {
        float s = h * 0.8f;
        Rectangle c = { x + 10, y + (h - s) / 2, s, s };
        ui_rect_lines(c, 0, 1.5f, ln);
        float ccx = c.x + s / 2, ccy = c.y + s / 2;
        dot(ccx, ccy, ln);
        DrawTriangle((Vector2){ c.x + s, c.y }, (Vector2){ ccx, ccy }, (Vector2){ c.x + s, c.y + s }, ui_alpha(hi, 0.25f));
        label("pyramid: face to centre", c.x, c.y + s + 2, hi);
        float ox = x + w * 0.58f, s2 = s;
        Vector2 q[4] = { { ox, c.y }, { ox + s2, c.y }, { ox + s2 * 0.25f, c.y + s2 * 0.5f }, { ox, c.y + s2 } };   /* a face folded past the centre */
        poly(q, 4, 1.5f, ln);
        float qcx = ox + s2 * 0.45f, qcy = c.y + s2 * 0.5f;
        dot(qcx, qcy, ln);
        DrawTriangle((Vector2){ ox + s2 * 0.25f, c.y + s2 * 0.5f }, (Vector2){ ox + s2, c.y }, (Vector2){ qcx, qcy }, ui_alpha(bad, 0.35f));
        label("points the wrong way", ox, c.y + s + 2, bad);
        break;
    }
    case QM_CONCAVE: {
        Vector2 v[5] = { { x + 30, y + 10 }, { x + w - 30, y + 10 }, { x + w - 30, y + h - 10 }, { cx, y + h * 0.45f }, { x + 30, y + h - 10 } };
        poly(v, 5, 2, ln);
        dot(v[3].x, v[3].y, bad);
        label("a corner bent inward", v[3].x - 60, v[3].y - 22, bad);
        break;
    }
    case QM_WARPED: {
        Vector2 a = { x + 20, y + h * 0.75f }, b = { cx, y + h }, c2 = { x + w - 20, y + h * 0.55f }, d = { cx + 20, y + 4 };
        DrawTriangle(a, b, d, ui_alpha(hi, 0.18f)); DrawTriangle(b, c2, d, ui_alpha(bad, 0.22f));
        Vector2 q[4] = { a, b, c2, d }; poly(q, 4, 2, ln);
        ui_line(b.x, b.y, d.x, d.y, 1, ui_alpha(ln, 0.6f));
        label("one corner lifted: two planes", x + 10, y + 2, ln);
        break;
    }
    case QM_DETERMINANT: {
        float s = h * 0.7f;
        ui_rect_lines((Rectangle){ x + 20, y + (h - s) / 2, s, s }, 0, 1.5f, ln);
        label("1", x + 20 + s / 2 - 3, y + (h - s) / 2 + s / 2 - 8, ln);
        float ox = x + w * 0.5f;
        Vector2 q[4] = { { ox, cy + 8 }, { ox + w * 0.42f, cy + 2 }, { ox + w * 0.44f, cy - 6 }, { ox + 2, cy } };
        poly(q, 4, 2, bad);
        label("crushed flat: near 0", ox, cy + 16, bad);
        break;
    }
    case QM_WEIGHT: case QM_VOLRATIO: {
        float bs = h * 0.9f, ss = h * 0.18f;
        Rectangle B = { x + 20, y + (h - bs) / 2, bs, bs }, S = { B.x + bs, cy - ss / 2, ss, ss };
        ui_rect_lines(B, 0, 1.5f, ln); ui_rect_lines(S, 0, 2, bad);
        dot(B.x + bs / 2, cy, ln); dot(S.x + ss / 2, cy, bad);
        ui_line(B.x + bs / 2, cy, S.x + ss / 2, cy, 1, ln);
        ui_line(S.x, S.y - 6, S.x, S.y + ss + 6, 3, hi);
        label(k == QM_WEIGHT ? "face near its centre" : "a 100 : 1 size jump", B.x + bs + ss + 10, cy - 8, bad);
        break;
    }
    default: {                                            /* an open cell */
        float s = h * 0.8f;
        Rectangle c = { cx - s / 2, y + (h - s) / 2, s, s };
        ui_line(c.x, c.y, c.x + s, c.y, 1.5f, ln); ui_line(c.x, c.y, c.x, c.y + s, 1.5f, ln); ui_line(c.x, c.y + s, c.x + s, c.y + s, 1.5f, ln);
        ui_line(c.x + s, c.y, c.x + s, c.y + s * 0.35f, 1.5f, ln); ui_line(c.x + s, c.y + s * 0.65f, c.x + s, c.y + s, 1.5f, ln);
        arrow(c.x + s * 0.6f, cy, c.x + s + 40, cy, bad);
        label("a face missing: it leaks", c.x + s + 6, cy + 8, bad);
        break;
    }
    }
    ui_clip_pop();
}

static float q_section_text(float x, float y, float w, const char *head, const char *text)
{
    ui_text(F_MED, 11.5f, head, x, y, TH.text3);
    return 18 + ui_text_wrap(F_REG, 13, text, (Rectangle){ x, y + 18, w, 400 }, 19, TH.text, true) + 10;
}

static float q_detail(int k, float x, float y, float w)
{
    const QualMetric *m = &QUAL[k];
    bool side = w >= 720;
    float pw = side ? 280 : w, ph = 170, tx = side ? x + pw + 20 : x, tw = side ? w - pw - 20 : w;
    float ty = side ? y : y + ph + 12;
    float h = q_section_text(tx, ty, tw, "WHAT IT IS", m->what);
    h += q_section_text(tx, ty + h, tw, "WHAT IT DOES TO THE SOLUTION", m->effect);
    h += q_section_text(tx, ty + h, tw, "THE LIMITS", m->threshold);
    h += q_section_text(tx, ty + h, tw, "THE FIX", m->fix);
    picture(k, (Rectangle){ x, y, pw, ph });
    return side ? fmaxf(ph, h) : ph + 12 + h;
}

/* checkMesh's sets: cleared before it runs (a set left from a mesh since fixed would show as bad); gathered by topoSet into
   one cellSet, qualityCells, which subsetMesh, refineMesh -cellSet or setFields can use; that and the face sets to VTK */
#define QSETS "nonOrthoFaces skewFaces wrongOrientedFaces zeroAreaFaces zeroVolumeCells highAspectRatioCells nonClosedCells " \
              "concaveFaces warpedFaces cellDeterminant concaveCells lowWeightFaces faceVolumeRatio lowQualityTetFaces"
static bool qPending;
static char qPendingCase[PL_PATH];

static void quality_paraview(const Case *c)
{
    app_write_foam_file();
    char dir[PL_PATH], py[PL_PATH], foam[PL_PATH + 64], n[300], err[256] = "";
    pl_join(dir, sizeof dir, c->s.path, "paraview"); pl_mkdirs(dir);
    pl_join(py, sizeof py, dir, "quality.py");
    snprintf(n, sizeof n, "%s.foam", c->s.name); pl_join(foam, sizeof foam, c->s.path, n);
    const char *script =
        "# Q-Continuum (Mesh > Quality): the cells checkMesh flagged, red, inside the mesh drawn faintly; the flagged faces orange.\n"
        "# Written by Q-Continuum each time you press the button; run from the case folder.\n"
        "from paraview.simple import *\n"
        "import glob, os\n"
        "view = GetActiveViewOrCreate('RenderView')\n"
        "foam = glob.glob('*.foam')\n"
        "if foam:\n"
        "    mesh = OpenFOAMReader(registrationName='mesh', FileName=os.path.abspath(foam[0]))\n"
        "    d = Show(mesh, view)\n"
        "    d.Representation = 'Surface'\n"
        "    d.Opacity = 0.12\n"
        "    ColorBy(d, None)\n"
        "for f in sorted(glob.glob(os.path.join('postProcessing', 'quality', 'qualityCells_*.vtm'))):\n"
        "    cells = XMLMultiBlockDataReader(registrationName='flagged cells', FileName=[os.path.abspath(f)])\n"
        "    d = Show(cells, view)\n"
        "    d.Representation = 'Surface With Edges'\n"
        "    ColorBy(d, None)\n"
        "    d.DiffuseColor = [0.9, 0.15, 0.15]\n"
        "for f in sorted(glob.glob(os.path.join('postProcessing', 'constant', '*', '*.vtp'))):\n"
        "    faces = XMLPolyDataReader(registrationName=os.path.basename(f)[:-4], FileName=[os.path.abspath(f)])\n"
        "    d = Show(faces, view)\n"
        "    ColorBy(d, None)\n"
        "    d.DiffuseColor = [1.0, 0.6, 0.1]\n"
        "view.ResetCamera()\n"
        "Render()\n";
    if (!pl_write_file(py, script, strlen(script))) { ui_toast(TOAST_ERR, "Could not write paraview/quality.py"); return; }
    if (launch_paraview(&A.env, &A.set, foam, py, err, sizeof err)) ui_toast(TOAST_OK, "Opening the flagged cells in ParaView (paraview/quality.py)");
    else ui_toast(TOAST_ERR, "%s", err);
}

static void quality_cells(Case *c)
{
    if (!A.cs || !app_can_start() || !app_require_foam()) return;
    app_save_all(true);
    job_reset(A.cj, "Quality cells");
    job_add(A.cj, "checkMesh", "checkMesh (all checks, writing the flagged sets)",
            "for s in " QSETS " qualityCells; do rm -f constant/polyMesh/sets/$s constant/polyMesh/sets/$s.gz; done; "
            "rm -rf postProcessing/quality; for s in " QSETS "; do rm -rf postProcessing/constant/$s; done; "
            "checkMesh -allGeometry -allTopology -writeSets vtk", "log.checkMesh", false);
    job_add(A.cj, "topoSet", "topoSet: the flagged cells as one cellSet (qualityCells)",
            "d=system/topoSetDict.quality; { printf 'FoamFile { version 2.0; format ascii; class dictionary; object topoSetDict; }\\n"
            "// Q-Continuum, Mesh > Quality: every face and cell checkMesh flagged, as the cellSet qualityCells\\nactions\\n(\\n"
            "    { name qualityCells; type cellSet; action new; source labelToCell; value (); }\\n'; "
            "for s in " QSETS "; do f=constant/polyMesh/sets/$s; [ -f $f ] || continue; "
            "if grep -q 'class *faceSet' $f; then printf '    { name qualityCells; type cellSet; action add; source faceToCell; set %s; option any; }\\n' $s; "
            "elif grep -q 'class *cellSet' $f; then printf '    { name qualityCells; type cellSet; action add; source cellToCell; set %s; }\\n' $s; fi; done; "
            "printf ');\\n'; } > $d && topoSet -dict $d", "log.topoSet.quality", false);
    job_add(A.cj, "foamToVTK", "foamToVTK: the flagged cells for ParaView",
            "n=$(grep -o 'qualityCells now size [0-9]*' log.topoSet.quality | tail -1 | grep -o '[0-9]*$'); "
            "if [ \"${n:-0}\" -gt 0 ]; then foamToVTK -cellSet qualityCells -constant -noZero -no-fields -name postProcessing/quality; "
            "else echo 'Q-Continuum: checkMesh flagged no cells'; fi", "log.foamToVTK.quality", false);
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) {
        qPending = true; pl_strlcpy(qPendingCase, c->s.path, sizeof qPendingCase);
        ui_toast(TOAST_INFO, "Finding the flagged cells: checkMesh, then topoSet");
    } else ui_toast(TOAST_ERR, "Could not start checkMesh");
}

static float explainer(Case *c, float x, float y, float w)
{
    float y0 = y;
    qr_read(c);
    /* the job finished: ParaView, or why not */
    if (qPending && !A.cj->active && !strcmp(A.cj->title, "Quality cells") && !strcmp(qPendingCase, c->s.path)) {
        qPending = false;
        if (A.cj->failed) ui_toast(TOAST_ERR, "Finding the flagged cells stopped: the Run page has the log");
        else {
            char lp[PL_PATH]; pl_join(lp, sizeof lp, c->s.path, "log.foamToVTK.quality");
            char *t = pl_read_file(lp, NULL);
            bool none = t && strstr(t, "flagged no cells");
            free(t);
            if (none) ui_toast(TOAST_OK, "checkMesh flagged no cells: nothing to show");
            else quality_paraview(c);
        }
    }
    y += section_head(x, y, w, "What the numbers mean", "Each measure checkMesh makes of the cells: what it is, what a bad value does to "
                                                       "the solution, the limits and the fix. Click one to open it.");
    int prob = qual_first_problem(&QRep);
    float bw = ui_button_width(ICON_EYE, "Show the flagged cells in ParaView");
    ui_disable(A.cj->active || !c->s.meshed);
    if (ui_button(ui_id("mq-cells"), (Rectangle){ x, y, bw, 34 }, ICON_EYE, "Show the flagged cells in ParaView", prob >= 0 ? BTN_PRIMARY : BTN_SECONDARY))
        quality_cells(c);
    ui_disable(false);
    ui_tooltip((Rectangle){ x, y, bw, 34 }, "Runs every check, gathers the faces and cells it flags into one cellSet (qualityCells: refineMesh "
                                             "-cellSet or subsetMesh can use it) and opens them in ParaView, red inside the faint mesh");
    if (!QRep.allGeometry && QRep.have)
        ui_text_wrap(F_REG, 12.5f, "Measures marked \"not measured\" need Check everything.", (Rectangle){ x + bw + 16, y + 9, w - bw - 16, 40 }, 17, TH.text3, true);
    y += 46;
    for (int k = 0; k < QM__N; k++) {
        const QualMetric *m = &QUAL[k];
        int st = QRep.state[k];
        bool open = qOpen == k;
        Rectangle row = { x, y, w, 40 };
        if (ui_culled(row.y, row.y + row.height)) ui_register(ui_idn(ui_id("mq-q"), k), row, NULL);   /* tests scroll to it */
        else {
            if (ui_row(ui_idn(ui_id("mq-q"), k), row, open)) qOpen = open ? -1 : k;
            ui_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + 14, y + 20, 14, TH.text3);
            const char *ic = st == QV_FAILED ? ICON_CIRCLE_X : st == QV_FLAGGED ? ICON_TRIANGLE_ALERT : st == QV_OK ? ICON_CIRCLE_CHECK : ICON_INFO;
            ui_icon(ic, x + 38, y + 20, 15, qcol(st));
            ui_text(F_MED, 14, m->name, x + 56, y + 11, TH.text);
            char v[48], lim[64], what[200];
            qual_fmt(k, QRep.value[k], v, sizeof v);
            if (k == QM_VOLUME) snprintf(lim, sizeof lim, "must be > 0");
            else if (!isnan(m->limit)) { char l2[32]; qual_fmt(k, m->limit, l2, sizeof l2); snprintf(lim, sizeof lim, "%s %s", m->worseHigh ? "max" : "min", l2); }
            else lim[0] = 0;
            if (st == QV_UNMEASURED) snprintf(what, sizeof what, QRep.have ? "not measured" : "no checkMesh log");
            else if (QRep.count[k] > 0) snprintf(what, sizeof what, "%s  ·  %ld past the limit (%s)", v, QRep.count[k], lim);
            else snprintf(what, sizeof what, "%s%s%s", v, lim[0] ? "  ·  " : "", lim);
            ui_text_clip(F_MONO, 12.5f, what, x + 250, y + 12, w - 260, st == QV_OK || st == QV_UNMEASURED ? TH.text2 : qcol(st));
        }
        y += 42;
        if (open) {
            y += 6;
            y += q_detail(k, x + 14, y, w - 28) + 14;
        }
    }
    return y - y0 + 6;
}

float page_meshquality(Case *c, float x, float y, float w)
{
    float y0 = y;
    cm_read(c);
    if (!c->s.meshed) { y += note(x, y, w, "No mesh yet: generate or import one, then its quality shows here."); return y - y0; }
    y += section_head(x, y, w, "checkMesh", "[[checkmesh|OpenFOAM's own check]] of the mesh on disk. [[non-orthogonality|Non-orthogonality]] over 70 degrees and [[skewness]] over 4 "
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
            char soft[420]; app_mesh_verdict_text(CM.ok ? NULL : app_mesh_verdict(c), soft, sizeof soft);
            if (soft[0]) str_catf(&d, "\n%s", soft);
            float ch = callout_height(w, "What checkMesh flags", d.s);
            callout((Rectangle){ x, y, w, ch }, CM.ok || soft[0] ? TOAST_INFO : TOAST_WARN, soft[0] ? ICON_INFO : ICON_TRIANGLE_ALERT,
                    "What checkMesh flags", d.s);
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
    if (CM.have || c->s.meshed) y += explainer(c, x, y, w) + 10;
    y += salome_report(c, x, y, w) + 10;
    y += export_section(c, x, y, w);
    return y - y0;
}
