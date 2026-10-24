/* page_case.c -- case overview and mesh pages */
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "xsection.h"

static void fmt_count(char *o, size_t cap, long v)
{
    if (v < 0) { snprintf(o, cap, "-"); return; }
    if (v >= 1000000) snprintf(o, cap, "%.2fM", v / 1e6);
    else if (v >= 10000) snprintf(o, cap, "%.1fk", v / 1e3);
    else snprintf(o, cap, "%ld", v);
}

static const char *mesh_tool(void)
{
    char p[PL_PATH];
    pl_join(p, sizeof p, A.cs->s.path, "system/snappyHexMeshDict");
    if (pl_is_file(p)) return "snappyHexMesh";
    pl_join(p, sizeof p, A.cs->s.path, "system/blockMeshDict");
    if (pl_is_file(p)) return "blockMesh";
    return NULL;
}

void app_generate_mesh(void)
{
    char p[PL_PATH];
    pl_join(p, sizeof p, A.cs->s.path, "system/blockMeshDict");
    bool bm = pl_is_file(p);
    pl_join(p, sizeof p, A.cs->s.path, "system/snappyHexMeshDict");
    bool sn = pl_is_file(p);
    if (!bm && !sn) { ui_toast(TOAST_WARN, "This case has no blockMeshDict or snappyHexMeshDict in system/"); return; }
    if (!app_require_foam() || !app_can_start()) return;
    app_save_all(true);
    job_reset(A.cj, "Mesh");
    if (bm) job_add(A.cj, "blockMesh", "blockMesh", "blockMesh", "log.blockMesh", false);
    if (sn) {
        pl_join(p, sizeof p, A.cs->s.path, "system/surfaceFeatureExtractDict");
        if (pl_is_file(p)) job_add(A.cj, "surfaceFeatureExtract", "surfaceFeatureExtract", "surfaceFeatureExtract", "log.surfaceFeatureExtract", false);
        job_add(A.cj, "snappyHexMesh", "snappyHexMesh", "snappyHexMesh -overwrite", "log.snappyHexMesh", false);
    }
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
    CaseSummary *cs = &A.cs->s;
    if (cs->nRegions > 0) {
        /* multi-region (CHT): zones -> one mesh per region -> per-region fields */
        pl_join(p, sizeof p, cs->path, "system/topoSetDict");
        if (pl_is_file(p)) job_add(A.cj, "topoSet", "topoSet", "topoSet", "log.topoSet", false);
        /* as the Run page and the tutorials' Allrun.pre do it (bug hunt H-06): flat 0.orig fields go to 0 before the split,
           which copies them into every region, and the fluid fields leave the solid regions again; per-region
           0.orig folders are copied after the split instead */
        #define RUNFN ". \"$WM_PROJECT_DIR/bin/tools/RunFunctions\" || exit 1\n"
        char rel[160];
        snprintf(rel, sizeof rel, "0.orig/%s", cs->regions[0]);
        pl_join(p, sizeof p, cs->path, rel);
        bool byRegion = pl_is_dir(p);
        if (cs->hasZeroOrig && !byRegion) job_add(A.cj, "restore0", "Copy 0.orig to 0", RUNFN "restore0Dir 2>&1", "log.restore0", false);
        job_add(A.cj, "split", "splitMeshRegions", byRegion ? "splitMeshRegions -cellZones -overwrite" :
                "splitMeshRegions -cellZones -overwrite || exit 1\n" RUNFN
                "for region in $(foamListRegions solid)\ndo\n"
                "    remove0DirFields -region \"$region\" -- nut alphat epsilon k omega nuTilda U p_rgh\ndone 2>&1",
                "log.splitMeshRegions", false);
        if (byRegion) job_add(A.cj, "restore0", "Copy 0.orig to 0", RUNFN "restore0Dir 2>&1", "log.restore0", false);
        #undef RUNFN
        Str sc; str_init(&sc);
        for (int r = 0; r < cs->nRegions; r++) {
            snprintf(rel, sizeof rel, "system/%s/changeDictionaryDict", cs->regions[r]);
            pl_join(p, sizeof p, cs->path, rel);
            if (pl_is_file(p)) str_catf(&sc, "echo '== region %s'\nchangeDictionary -region %s || exit 1\n", cs->regions[r], cs->regions[r]);
        }
        if (sc.s && sc.s[0]) job_add(A.cj, "changeDict", "changeDictionary", sc.s, "log.changeDictionary", false);
        str_free(&sc);
    }
    if (job_start(A.cj, A.cs->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Meshing started");
}

/* ============================================================================ overview */
static float check_row(float x, float y, float w, int kind, const char *title, const char *text,
                       const char *btn, const char *btnIcon, bool *pressed)
{
    Color c = kind == TOAST_OK ? TH.ok : kind == TOAST_WARN ? TH.warn : kind == TOAST_ERR ? TH.err : TH.info;
    const char *ic = kind == TOAST_OK ? ICON_CIRCLE_CHECK : kind == TOAST_WARN ? ICON_TRIANGLE_ALERT :
                     kind == TOAST_ERR ? ICON_CIRCLE_X : ICON_INFO;
    float bw = btn ? ui_button_width(btnIcon, btn) : 0;
    float tw = w - 44 - (btn ? bw + 16 : 0);
    float th = text && *text ? ui_text_wrap(F_REG, 13, text, (Rectangle){ 0, 0, tw, 999 }, 19, TH.text2, false) : 0;
    float h = fmaxf(26 + th, 36) + 22;
    ui_icon(ic, x + 12, y + 21, 17, c);
    ui_text(F_MED, 14, title, x + 36, y + 12, TH.text);
    if (th > 0) ui_text_wrap(F_REG, 13, text, (Rectangle){ x + 36, y + 33, tw, 999 }, 19, TH.text2, true);
    if (btn && pressed) *pressed = ui_button(ui_id(title), (Rectangle){ x + w - bw, y + 12, bw, 32 }, btnIcon, btn, BTN_SECONDARY);
    ui_divider(x, y + h - 1, w);
    return h;
}

void page_overview(Rectangle r)
{
    Case *c = A.cs;
    UiScroll *sc = &A.scr[3];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, 1240);
    float y = top + 30;

    /* title block */
    float nameMax = w - 260;                                  /* room for the two badges; a long name ends in "..." (H-21) */
    ui_text_clip(F_SEMI, TH.lcars ? 32 : 26, c->s.name, x, y - (TH.lcars ? 3 : 0), nameMax, head_color());
    float bx = x + fminf(ui_text_w(F_SEMI, TH.lcars ? 32 : 26, c->s.name), nameMax) + 16;
    Color cc = cat_color(c->s.cat);
    bx += ui_badge(bx, y + 7, cat_icon(c->s.cat), category_name(c->s.cat), cc, ui_alpha(cc, 0.14f)) + 8;
    if (c->s.app[0]) ui_badge(bx, y + 7, NULL, c->s.app, TH.text2, TH.lineSoft);
    y += 42;
    ui_text_clip(F_MONO, 12.5f, c->s.path, x, y, w - 430, TH.text3);
    float px = x + fminf(ui_text_w(F_MONO, 12.5f, c->s.path), w - 430) + 10;
    if (ui_button(ui_id("ov-copy"), (Rectangle){ px, y - 7, 28, 28 }, ICON_COPY, NULL, BTN_ICON)) { SetClipboardText(c->s.path); ui_toast(TOAST_OK, "Path copied"); }
    ui_tooltip((Rectangle){ px, y - 7, 28, 28 }, "Copy path");
    float rx = x + w;
    float w1 = ui_button_width(ICON_SQUARE_TERMINAL, "Terminal");
    rx -= w1;
    if (ui_button(ui_id("ov-term"), (Rectangle){ rx, y - 10, w1, 32 }, ICON_SQUARE_TERMINAL, "Terminal", BTN_GHOST))
        if (!launch_terminal(&A.env, &A.set, c->s.path)) ui_toast(TOAST_WARN, "No terminal emulator found");
    ui_tooltip((Rectangle){ rx, y - 10, w1, 32 }, "Opens a shell in this case with OpenFOAM loaded");
    float w2 = ui_button_width(ICON_FOLDER_OPEN, "Open folder");
    rx -= w2 + 6;
    if (ui_button(ui_id("ov-folder"), (Rectangle){ rx, y - 10, w2, 32 }, ICON_FOLDER_OPEN, "Open folder", BTN_GHOST)) launch_folder(&A.env, c->s.path);
    float w3 = ui_button_width(ICON_TRASH_2, "Delete");
    rx -= w3 + 6;
    if (ui_button(ui_id("ov-delete"), (Rectangle){ rx, y - 10, w3, 32 }, ICON_TRASH_2, "Delete", BTN_GHOST)) app_ask_delete_case(c->s.path);
    y += 40;

    /* tiles */
    float gap = 16, tw = (w - 3 * gap) / 4, th = 100;
    char v[96], sub[160], a1[32], a2[32];
    fmt_count(a1, sizeof a1, c->s.nCells);
    snprintf(v, sizeof v, c->s.meshed ? "%s cells" : "No mesh", a1);
    fmt_count(a1, sizeof a1, c->s.nFaces); fmt_count(a2, sizeof a2, c->s.nPoints);
    snprintf(sub, sizeof sub, c->s.meshed ? "%s faces, %s points, %d patches" : "Generate it on the Mesh page", a1, a2, c->nPatches);
    stat_tile((Rectangle){ x, y, tw, th }, ICON_GRID_3X3, "Mesh", v, sub);
    snprintf(v, sizeof v, "%s / %g", c->s.latest[0] ? c->s.latest : "0", c->s.endTime);
    snprintf(sub, sizeof sub, "%d time director%s written, dt %g", c->s.nTimes, c->s.nTimes == 1 ? "y" : "ies", c->s.deltaT);
    stat_tile((Rectangle){ x + tw + gap, y, tw, th }, ICON_CLOCK, "Simulated time", v, sub);
    bool running = A.cj->active && !strcmp(A.cj->casePath, c->s.path);
    CaseStatus st = running ? CS_RUNNING : c->s.status;
    char ago[48]; pl_time_ago(ago, sizeof ago, c->s.activity);
    snprintf(sub, sizeof sub, "%s%s%s", c->s.lastLog[0] ? c->s.lastLog : "no logs yet", c->s.lastLog[0] ? ", " : "", c->s.lastLog[0] ? ago : "");
    stat_tile((Rectangle){ x + 2 * (tw + gap), y, tw, th }, ICON_ACTIVITY, "Last run", status_name(st), sub);
    DrawCircleV((Vector2){ x + 2 * (tw + gap) + tw - 22, y + 26 }, 5, status_color(st));
    snprintf(v, sizeof v, "%d field%s", c->fields.n, c->fields.n == 1 ? "" : "s");
    snprintf(sub, sizeof sub, "in %s/", c->fieldDir);
    stat_tile((Rectangle){ x + 3 * (tw + gap), y, tw, th }, ICON_LAYERS, "Initial conditions", v, sub);
    y += th + 34;

    /* checklist + patches */
    float lw = w * 0.6f, rx2 = x + lw + 32, rw = w - lw - 32;
    float ly = y;
    ly += section_head(x, ly, lw, "Case check", "What this case needs before it runs, and what it has produced.");
    bool pr = false;
    const char *mt = mesh_tool();
    const char *badPath = foam_path_problem(c->s.path);
    if (badPath) {
        char t[200]; snprintf(t, sizeof t, "The case path has %s, which OpenFOAM refuses. Copy the case to a folder without it.", badPath);
        ly += check_row(x, ly, lw, TOAST_ERR, "OpenFOAM can't run in this folder", t, "Copy to workspace", ICON_COPY, &pr);
        if (pr) A.copyPending = true;
    }
    if (c->s.readOnly) {
        ly += check_row(x, ly, lw, TOAST_ERR, "Read-only folder", "You can look around, but saving and running need a writable copy.",
                        "Copy to workspace", ICON_COPY, &pr);
        if (pr) A.copyPending = true;
    }
    if (c->s.meshed) {
        char t[160]; snprintf(t, sizeof t, "%s cells across %d patches.", a1[0] ? a1 : "", c->nPatches);
        fmt_count(a1, sizeof a1, c->s.nCells);
        snprintf(t, sizeof t, "%s cells across %d patches.", a1, c->nPatches);
        ly += check_row(x, ly, lw, TOAST_OK, "Mesh generated", t, "Mesh page", ICON_GRID_3X3, &pr);
        if (pr) app_go(PG_MESH);
    } else if (mt && c->s.nRegions > 0) {
        char t[200]; snprintf(t, sizeof t, "This case has %d regions. Generate mesh runs %s, topoSet and splitMeshRegions to build one mesh per region.", c->s.nRegions, mt);
        ly += check_row(x, ly, lw, TOAST_WARN, "Region meshes not built yet", t, "Generate mesh", ICON_PLAY, &pr);
        if (pr) app_generate_mesh();
    } else if (mt) {
        char t[160]; snprintf(t, sizeof t, "Run %s to build it from system/%sDict.", mt, mt);
        ly += check_row(x, ly, lw, TOAST_WARN, "No mesh yet", t, "Generate mesh", ICON_PLAY, &pr);
        if (pr) app_generate_mesh();
    } else {
        ly += check_row(x, ly, lw, TOAST_ERR, "No mesh", "Add a blockMeshDict, or copy a mesh into constant/polyMesh.", NULL, NULL, NULL);
    }
    if (c->s.app[0]) {
        char t[160]; snprintf(t, sizeof t, "Runs to t = %g with time step %g.", c->s.endTime, c->s.deltaT);
        char tt[96]; snprintf(tt, sizeof tt, "Solver: %s", c->s.app);
        ly += check_row(x, ly, lw, TOAST_OK, tt, t, NULL, NULL, NULL);
    } else ly += check_row(x, ly, lw, TOAST_ERR, "No solver set", "Choose an application on the Run page.", "Run page", ICON_PLAY, &pr);
    if (pr) app_go(PG_RUN);
    if (c->region[0] && !pl_starts_with(c->fieldDir, "0.orig")) {
        char t[300]; snprintf(t, sizeof t, "%s/ is written by splitMeshRegions (and changeDictionary), so a run from the start "
                              "replaces it. The Boundary conditions page says where lasting edits go.", c->fieldDir);
        ly += check_row(x, ly, lw, TOAST_INFO, "Region fields are generated", t, NULL, NULL, NULL);
    } else if (c->s.hasZeroOrig)
        ly += check_row(x, ly, lw, TOAST_INFO, "Initial fields live in 0.orig",
                        "They are copied to 0 when you run, so edit them here and the run starts from your values.", NULL, NULL, NULL);
    else if (!c->s.hasZero)
        ly += check_row(x, ly, lw, TOAST_ERR, "No initial conditions", "The case has neither a 0 nor a 0.orig folder.", NULL, NULL, NULL);
    int dirty = case_dirty_count(c);
    if (dirty) {
        char t[96]; snprintf(t, sizeof t, "%d edited file%s not written to disk yet.", dirty, dirty == 1 ? "" : "s");
        ly += check_row(x, ly, lw, TOAST_WARN, "Unsaved changes", t, "Save", ICON_SAVE, &pr);
        if (pr) app_save_all(false);
    }
    if (c->s.nTimes > 0) {
        char t[160]; snprintf(t, sizeof t, "%d time director%s up to t = %s. Open them in ParaView, or clear them to start fresh.",
                              c->s.nTimes, c->s.nTimes == 1 ? "y" : "ies", c->s.latest);
        ly += check_row(x, ly, lw, TOAST_OK, "Results on disk", t, "Clean results", ICON_TRASH_2, &pr);
        if (pr) { A.modal = MD_CLEAN; A.mFlag = A.mFlag2 = false; }
    }
    char foam[300];
    const char *pvd = paraview_description(&A.env, &A.set);
    if (pl_starts_with(pvd, "Not found")) snprintf(foam, sizeof foam, "ParaView wasn't found. Install it, or set its path in Settings.");
    else snprintf(foam, sizeof foam, "Opens %s.foam with %s.", c->s.name, pvd);
    ly += check_row(x, ly, lw, TOAST_INFO, "Post-processing in ParaView", foam, "Open in ParaView", ICON_EXTERNAL_LINK, &pr);
    if (pr) app_open_paraview();
    ly += check_row(x, ly, lw, TOAST_INFO, "ParaView with Claude",
                    A.env.claude[0] ? (app_claude_view_exists() ? "Claude has written paraview/view.py for this case. Open it, or ask Claude for another view."
                                                                : "Claude Code writes a ParaView script with views chosen for this case, then ParaView opens with it.")
                                    : "Needs Claude Code on this machine.", "ParaView with Claude", ICON_SPARKLES, &pr);
    if (pr) { A.modal = MD_PV_CLAUDE; A.mText2[0] = 0; }

    /* patches */
    float ry = y;
    char pd[200];
    if (c->patchSource == 1) snprintf(pd, sizeof pd, "From the mesh (%s).", case_rpath(c, "constant/polyMesh/boundary"));
    else snprintf(pd, sizeof pd, c->patchSource == 2 ? "From blockMeshDict. Generate the mesh to see face counts." : "From the field files.");
    ry += section_head(rx2, ry, rw, "Patches", pd);
    Rectangle pr2 = { rx2, ry, rw, c->nPatches ? fminf(c->nPatches, 12) * 40 + 12 : c->s.meshed ? 46 : 88 };
    ui_rect(pr2, 12, TH.surface);
    ui_rect_lines(pr2, 12, 1, TH.lineSoft);
    for (int i = 0; i < c->nPatches && i < 12; i++) {
        float yy = pr2.y + 6 + i * 40;
        ui_text_clip(F_MONO_MED, 13, c->patches[i].name, pr2.x + 16, yy + 11, rw * 0.5f, TH.text);
        if (c->patches[i].nFaces >= 0) {
            char nf[48]; snprintf(nf, sizeof nf, "%d faces", c->patches[i].nFaces);
            ui_text(F_REG, 12.5f, nf, pr2.x + rw - 16 - ui_text_w(F_REG, 12.5f, nf), yy + 12, TH.text3);
        }
        if (c->patches[i].type[0]) ui_badge(pr2.x + rw * 0.52f, yy + 9, NULL, c->patches[i].type, TH.text2, TH.lineSoft);
        if (i < c->nPatches - 1 && i < 11) ui_divider(pr2.x + 12, yy + 39, rw - 24);
    }
    if (c->nPatches > 12) ui_text(F_REG, 12.5f, "More on the Boundary conditions page", rx2, pr2.y + pr2.height + 8, TH.text3);
    if (c->nPatches == 0) {
        ui_text(F_REG, 13, c->s.meshed ? "No patches found." : "No patches yet: they come from the mesh.", pr2.x + 16, pr2.y + 14, TH.text3);
        if (!c->s.meshed) {
            float gw = ui_button_width(ICON_GRID_3X3, "Go to Mesh");
            if (ui_button(ui_id("ov-patch-mesh"), (Rectangle){ pr2.x + 16, pr2.y + 40, gw, 34 }, ICON_GRID_3X3, "Go to Mesh", BTN_SECONDARY)) app_go(PG_MESH);
        }
    }
    ry += pr2.height + 30;
    if (A.set.meshView && c->s.meshed && c->s.nRegions == 0 && xs_is_2d(c->s.path)) {   /* a 2-D case: what it looks like */
        ry += section_head(rx2, ry, rw, "Cross section", "Click it to open it larger on the Mesh page.");
        xsection_panel((Rectangle){ rx2, ry, rw, 340 }, c, true);
        ry += 360;
    }
    y = fmaxf(ly, ry) + 30;
    ui_scroll_end(sc, y - top);
}

/* ============================================================================ mesh */
typedef struct { int cntA[3], cntB[3]; int nCnt; char grading[160]; char verts[160]; long cells; bool parsed; } BlockInfo;

/* find "hex (...) [zone] (nx ny nz) grading (...)" items inside the blocks list text */
static int parse_blocks(const FDFile *f, const FDEntry *e, BlockInfo *out, int max)
{
    const char *s = f->text;
    int p = e->valStart, end = e->valEnd, n = 0;
    while (p < end && n < max) {
        while (p < end && !(s[p] == 'h' && !strncmp(s + p, "hex", 3) && (p == 0 || !isalnum((unsigned char)s[p - 1])))) p++;
        if (p >= end) break;
        p += 3;
        BlockInfo *b = &out[n];
        memset(b, 0, sizeof *b);
        /* vertex list */
        while (p < end && s[p] != '(') p++;
        int v0 = p, d = 0;
        for (; p < end; p++) { if (s[p] == '(') d++; else if (s[p] == ')' && --d == 0) { p++; break; } }
        snprintf(b->verts, sizeof b->verts, "%.*s", p - v0 < 150 ? p - v0 : 150, s + v0);
        /* optional zone name, then the cell counts */
        while (p < end && s[p] != '(') p++;
        int c0 = p + 1;
        while (p < end && s[p] != ')') p++;
        int c1 = p;
        p++;
        int q = c0;
        while (q < c1 && b->nCnt < 3) {
            while (q < c1 && isspace((unsigned char)s[q])) q++;
            int a = q;
            while (q < c1 && !isspace((unsigned char)s[q])) q++;
            if (q > a) { b->cntA[b->nCnt] = a; b->cntB[b->nCnt] = q; b->nCnt++; }
        }
        /* grading word + list */
        while (p < end && isspace((unsigned char)s[p])) p++;
        int g0 = p;
        while (p < end && s[p] != '(') p++;
        d = 0;
        for (; p < end; p++) { if (s[p] == '(') d++; else if (s[p] == ')' && --d == 0) { p++; break; } }
        char gr[400]; snprintf(gr, sizeof gr, "%.*s", p - g0 < 390 ? p - g0 : 390, s + g0);
        int k = 0; bool sp = false;
        for (char *z = gr; *z && k < 158; z++) { if (isspace((unsigned char)*z)) { if (!sp) b->grading[k++] = ' '; sp = true; } else { b->grading[k++] = *z; sp = false; } }
        b->grading[k] = 0;
        b->cells = 1;
        b->parsed = b->nCnt == 3;
        for (int i = 0; i < b->nCnt; i++) {
            char t[32]; snprintf(t, sizeof t, "%.*s", b->cntB[i] - b->cntA[i], s + b->cntA[i]);
            long v = pl_is_number(t) ? atol(t) : -1;
            if (v < 0) b->cells = -1; else if (b->cells >= 0) b->cells *= v;
        }
        n++;
    }
    return n;
}

/* the vertices list as numbers (scaled); -1 when it uses variables or code */
static int read_vertices(const FDFile *f, double scale, double (*out)[3], int max)
{
    FDEntry *v = fd_find((FDFile *)f, "vertices");
    if (!v) return -1;
    const char *s = f->text;
    int p = v->valStart + 1, n = 0;
    while (p < v->valEnd && n < max) {
        while (p < v->valEnd && s[p] != '(') { if (s[p] == '$' || s[p] == '#') return -1; p++; }
        if (p >= v->valEnd) break;
        p++;
        double c[3]; int k = 0;
        while (k < 3 && p < v->valEnd) {
            char *e; double d = strtod(s + p, &e);
            if (e == s + p) { if (s[p] == ')') break; p++; continue; }
            c[k++] = d; p = (int)(e - s);
        }
        while (p < v->valEnd && s[p] != ')') p++;
        p++;
        if (k != 3) return -1;
        for (int i = 0; i < 3; i++) out[n][i] = c[i] * scale;
        n++;
    }
    return n;
}

/* Base size, as in STAR-CCM+: one reference size that every count follows. The present base size is the
   largest cell (edge length / count over all block directions); a new one scales every count by old/new, so
   the case's own refinement between blocks (and its gradings) is kept and shared edges keep matching counts.
   Directions with 1 cell (2-D) stay 1. Returns false (and says why) when the blocks can't be sized this way.
   *current = the present base size, *cellsOut = the total for base (the present total when base <= 0);
   the file changes only with write. */
#define BS_MAXV 4096
bool blocks_base_size(FDFile *bm, double scale, double base, bool write, double *current, long *cellsOut, char *why, int cap)
{
    static double V[BS_MAXV][3];
    int nv = read_vertices(bm, scale, V, BS_MAXV);
    if (nv <= 0) { snprintf(why, (size_t)cap, "the vertices use variables or code"); return false; }
    FDEntry *be = fd_find(bm, "blocks");
    static BlockInfo blk[64];
    int nb = be ? parse_blocks(bm, be, blk, 64) : 0;
    if (!nb) { snprintf(why, (size_t)cap, "no hex blocks found"); return false; }
    static const int E[3][4][2] = { { {0,1},{3,2},{7,6},{4,5} }, { {0,3},{1,2},{5,6},{4,7} }, { {0,4},{1,5},{2,6},{3,7} } };
    int cnt[64][3];
    double cur = 0;
    for (int i = 0; i < nb; i++) {
        if (!blk[i].parsed) { snprintf(why, (size_t)cap, "block %d's cell counts use variables", i + 1); return false; }
        int vid[8];
        const char *q = blk[i].verts;
        for (int k = 0; k < 8; k++) {
            while (*q && !isdigit((unsigned char)*q)) { if (isalpha((unsigned char)*q) || *q == '$') { snprintf(why, (size_t)cap, "block %d names its vertices", i + 1); return false; } q++; }
            char *e; long t = strtol(q, &e, 10);
            if (e == q || t < 0 || t >= nv) { snprintf(why, (size_t)cap, "block %d has an unknown vertex", i + 1); return false; }
            vid[k] = (int)t; q = e;
        }
        for (int d = 0; d < 3; d++) {
            char t[32]; snprintf(t, sizeof t, "%.*s", blk[i].cntB[d] - blk[i].cntA[d], bm->text + blk[i].cntA[d]);
            cnt[i][d] = atoi(t);
            if (cnt[i][d] < 1) { snprintf(why, (size_t)cap, "block %d has a cell count below 1", i + 1); return false; }
            if (cnt[i][d] == 1) continue;
            double len = 0;
            for (int k = 0; k < 4; k++) {
                const double *a = V[vid[E[d][k][0]]], *b = V[vid[E[d][k][1]]];
                len = fmax(len, sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2])));
            }
            cur = fmax(cur, len / cnt[i][d]);
        }
    }
    if (cur <= 0) { snprintf(why, (size_t)cap, "every block is 1 cell thick"); return false; }
    if (current) *current = cur;
    double f = base > 0 ? cur / base : 1;
    long cells = 0;
    int newc[64][3];
    for (int i = 0; i < nb; i++) {
        for (int d = 0; d < 3; d++) newc[i][d] = cnt[i][d] == 1 ? 1 : (int)fmax(1, lround(cnt[i][d] * f));
        cells += (long)newc[i][0] * newc[i][1] * newc[i][2];
    }
    if (cellsOut) *cellsOut = cells;
    if (base <= 0 || !write) return true;
    /* write the counts from the end of the file backwards, so earlier offsets stay valid */
    for (int i = nb - 1; i >= 0; i--) for (int d = 2; d >= 0; d--) {
        char t[32]; snprintf(t, sizeof t, "%d", newc[i][d]);
        fd_replace_span(bm, blk[i].cntA[d], blk[i].cntB[d], t);
    }
    return true;
}

static bool bbox_from_vertices(FDFile *f, double scale, double *mn, double *mx)
{
    FDEntry *v = fd_find(f, "vertices");
    if (!v) return false;
    for (int k = 0; k < 3; k++) { mn[k] = DBL_MAX; mx[k] = -DBL_MAX; }
    const char *s = f->text;
    int p = v->valStart + 1, n = 0;
    while (p < v->valEnd) {
        while (p < v->valEnd && s[p] != '(') { if (s[p] == '$' || s[p] == '#') return false; p++; }
        if (p >= v->valEnd) break;
        p++;
        double c[3]; int k = 0;
        while (k < 3 && p < v->valEnd) {
            char *e; double d = strtod(s + p, &e);
            if (e == s + p) { if (s[p] == ')') break; p++; continue; }
            c[k++] = d; p = (int)(e - s);
        }
        while (p < v->valEnd && s[p] != ')') p++;
        p++;
        if (k == 3) { for (int i = 0; i < 3; i++) { mn[i] = fmin(mn[i], c[i] * scale); mx[i] = fmax(mx[i], c[i] * scale); } n++; }
    }
    return n > 0;
}

/* one region of a split mesh: its size and its patches, from constant/<region>/polyMesh */
static float region_mesh(float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    float gap = 16, tw = (w - 3 * gap) / 4, th = 92;
    char v[64];
    bool have = c->rCells >= 0;
    if (c->region[0]) {                                   /* the base mesh shows its tiles above already */
        fmt_count(v, sizeof v, c->rCells); stat_tile((Rectangle){ x, y, tw, th }, ICON_BOXES, "Cells", have ? v : "-", NULL);
        fmt_count(v, sizeof v, c->rFaces); stat_tile((Rectangle){ x + tw + gap, y, tw, th }, ICON_SQUARE, "Faces", have ? v : "-", NULL);
        fmt_count(v, sizeof v, c->rPoints); stat_tile((Rectangle){ x + 2 * (tw + gap), y, tw, th }, ICON_CIRCLE_DOT, "Points", have ? v : "-", NULL);
        snprintf(v, sizeof v, "%d", c->nPatches); stat_tile((Rectangle){ x + 3 * (tw + gap), y, tw, th }, ICON_LAYERS, "Patches", v, NULL);
        y += th + 34;
    }
    const char *brel = case_rpath(c, "constant/polyMesh/boundary");
    FDFile *b = case_dict(c, brel);
    if (!b) {
        char msg[300]; snprintf(msg, sizeof msg, "%s does not exist yet. The Run page makes it: topoSet, then splitMeshRegions -cellZones.", brel);
        float ch = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, msg);
        return y + ch + 20 - y0;
    }
    char desc[400], cd[200], p[PL_PATH];
    snprintf(cd, sizeof cd, "system/%s/changeDictionaryDict", c->region);
    pl_join(p, sizeof p, c->s.path, cd);
    if (c->region[0])
        snprintf(desc, sizeof desc, "From %s. splitMeshRegions writes this file, so a fresh split replaces edits made here%s%s.", brel,
                 pl_is_file(p) ? "; lasting patch types belong in " : "", pl_is_file(p) ? cd : "");
    else
        snprintf(desc, sizeof desc, "From %s, as the mesh converter wrote it. Importing the mesh again resets them.", brel);
    y += section_head(x, y, w, "Patch types", desc);
    static const char *types[] = { "patch", "wall", "empty", "symmetryPlane", "symmetry", "cyclic", "wedge", "mappedWall", "mappedPatch" };
    for (int i = 0; i < b->root.nChild; i++) {
        FDEntry *e = &b->root.child[i];
        if (e->kind != FD_DICT || !strcmp(e->key, "FoamFile")) continue;
        char path[300], nf[32] = "", hint[64] = "";
        snprintf(path, sizeof path, "%s/nFaces", e->key);
        fd_get(b, path, nf, sizeof nf);
        if (nf[0]) snprintf(hint, sizeof hint, "%s faces", nf);
        Rectangle pr = prop_row(x, &y, w, e->key, hint[0] ? hint : NULL, 34);
        snprintf(path, sizeof path, "%s/type", e->key);
        if (dv_choice(b, path, (Rectangle){ pr.x, pr.y, 240, 34 }, types, 9, true, "type")) break;
    }
    return y + 24 - y0;
}

float page_mesh_patches(float x, float y, float w) { return region_mesh(x, y, w); }

/* will the next run mesh again, so that continuing on the mesh on disk would ignore the change? The mesh dictionaries
   edited and not saved, saved after the mesh was written, or blocks that make another cell count than the mesh has.
   *why says how, in numbers. */
bool mesh_change_pending(Case *c, char *why, int cap)
{
    static const char *dicts[] = { "system/blockMeshDict", "system/snappyHexMeshDict", "system/surfaceFeatureExtractDict",
                                   "system/topoSetDict", "system/extrudeMeshDict", NULL };
    char n0[32]; fmt_count(n0, sizeof n0, c->s.nCells);
    for (int i = 0; dicts[i]; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, dicts[i]);
        if (!pl_is_file(p)) continue;
        FDFile *f = case_dict(c, dicts[i]);
        if (f && f->dirty) {
            long dc = -1;
            if (!strcmp(dicts[i], "system/blockMeshDict") && !case_dict(c, "system/snappyHexMeshDict")) {
                FDEntry *be = fd_find(f, "blocks");
                static BlockInfo blk[64];
                int nb = be ? parse_blocks(f, be, blk, 64) : 0;
                dc = nb ? 0 : -1;
                for (int k = 0; k < nb && dc >= 0; k++) dc = blk[k].cells >= 0 ? dc + blk[k].cells : -1;
            }
            char n1[32]; fmt_count(n1, sizeof n1, dc);
            if (dc > 0 && c->s.nCells > 0 && dc != c->s.nCells)
                snprintf(why, (size_t)cap, "%s now makes %s cells (not saved yet); the mesh on disk has %s.", dicts[i], n1, n0);
            else snprintf(why, (size_t)cap, "%s has unsaved changes the mesh on disk doesn't have.", dicts[i]);
            return true;
        }
    }
    if (c->s.meshStale) { snprintf(why, (size_t)cap, "%s was changed after the %s-cell mesh was written.", c->s.meshStaleDict, n0); return true; }
    if (c->s.regionStale) { snprintf(why, (size_t)cap, "The region meshes are older than constant/polyMesh or system/topoSetDict."); return true; }
    return false;
}

void page_mesh(Rectangle r)
{
    Case *c = A.cs;
    UiScroll *sc = &A.scr[4];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX);
    float y = top + 30;
    const char *mt = mesh_tool();
    char sub[256];
    snprintf(sub, sizeof sub, mt ? "%s builds this mesh from system/%sDict. Change the settings here, then generate the mesh again."
                                 : c->s.meshed ? "This case brings its own mesh in constant/polyMesh."
                                               : "No mesh yet, and no blockMeshDict to make one: build it on the SALOME tab, or convert one into constant/polyMesh.", mt, mt);
    if (A.meshTab == 1) snprintf(sub, sizeof sub, "SALOME builds this mesh from salome/mesh.py; ideasUnvToFoam converts it.");
    if (A.meshTab == 3) snprintf(sub, sizeof sub, "Mesh any STL: snappyHexMesh here, or SALOME's tetrahedra, polyhedra and body-fitted hexahedra.");
    float bw = ui_button_width(ICON_PLAY, "Generate mesh"), bw2 = ui_button_width(ICON_CIRCLE_CHECK, "Check mesh");
    y += page_header(x, y, w - bw - bw2 - 30, "Mesh", sub);
    ui_disable(A.cj->active || !mt);
    if (A.meshTab == 0 && ui_button(ui_id("mesh-gen"), (Rectangle){ x + w - bw, top + 32, bw, 36 }, ICON_PLAY, "Generate mesh", BTN_PRIMARY)) app_generate_mesh();
    ui_disable(A.cj->active || !c->s.meshed);
    if (ui_button(ui_id("mesh-check"), (Rectangle){ x + w - bw - bw2 - 10, top + 32, bw2, 36 }, ICON_CIRCLE_CHECK, "Check mesh", BTN_SECONDARY))
        app_start_tool("checkMesh", "checkMesh", "checkMesh", "checkMesh", "log.checkMesh");
    ui_disable(false);

    /* status line from the latest logs */
    int lc = logset_find(&A.logs, "log.checkMesh");
    bool meshJob = A.cj->active && !strcmp(A.cj->title, "Mesh");
    if (meshJob || (A.cj->active && !strcmp(A.cj->title, "checkMesh"))) {
        Rectangle b = { x, y, w, 48 };
        ui_rect(b, 10, TH.accentSoft);
        ui_spinner(b.x + 24, b.y + 24, 8, TH.accent);
        char t[160]; snprintf(t, sizeof t, "Running %s", A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label);
        ui_text(F_MED, 14, t, b.x + 44, b.y + 15, TH.text);
        if (ui_button(ui_id("mesh-log"), (Rectangle){ b.x + b.width - 110, b.y + 8, 100, 32 }, NULL, "Show log", BTN_GHOST)) app_go(PG_RUN);
        y += 64;
    } else if (lc >= 0 && (A.logs.logs[lc].meshOk || A.logs.logs[lc].meshFailed)) {
        const LogMon *m = &A.logs.logs[lc];
        const char *t = m->meshOk ? "checkMesh: Mesh OK" : "checkMesh reported failed checks";
        const char *d = m->meshOk ? "All quality checks passed the last time the mesh was checked." :
                                    "Open log.checkMesh on the Run page for details. Skewness and non-orthogonality are the usual culprits.";
        float ch = callout_height(w, t, d);
        callout((Rectangle){ x, y, w, ch }, m->meshOk ? TOAST_OK : TOAST_WARN, m->meshOk ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, t, d);
        y += ch + 20;
    }

    y += region_bar(x, y, w);

    /* the dictionaries as they are, a SALOME script, or the mesh itself */
    if (A.meshTab < 0) {
        char q[PL_PATH], st[PL_PATH]; pl_join(q, sizeof q, c->s.path, "salome/mesh.py"); pl_join(st, sizeof st, c->s.path, "mesher/mesher.ini");
        A.meshTab = pl_is_file(st) ? 3 : pl_is_file(q) ? 1 : 0;
    }
    /* shown in this order; A.meshTab keeps its numbers (0 dictionaries, 1 SALOME, 2 view, 3 STL mesher) */
    static const char *mtabs[] = { "Dictionaries", "STL mesher", "SALOME script", "View" };
    static const int TAB_OF[4] = { 0, 3, 1, 2 };
    int shown = 0;
    for (int k = 0; k < 4; k++) if (TAB_OF[k] == A.meshTab) shown = k;
    float tth;
    if (ui_tabs(ui_id("mesh-tabs"), x, y, mtabs, 4, &shown, &tth)) A.meshTab = TAB_OF[shown];
    ui_divider(x, y + tth, w);
    y += tth + 22;
    if (A.meshTab == 2) {                                  /* the mesh: 3-D surface; a 2-D case also its cross section */
        static int mode;                                   /* 0 cross section (2-D: the plane; 3-D: a cut), 1 surface in 3-D */
        static const char *vm[] = { "Cross section", "Surface (3-D)" };
        UiId sw = ui_id("mesh-view-on");
        float sx = x + w - 34;                             /* off: nothing is read, uploaded or drawn (the GPU copy is freed) */
        if (ui_switch(sw, sx, y + 6, &A.set.meshView)) { if (!A.set.meshView) meshview_release(); settings_save(&A.set); }
        ui_register(sw, (Rectangle){ sx, y + 6, 34, 20 }, "Show mesh");
        ui_text(F_MED, 13, ui_cap("Show mesh"), sx - 12 - ui_text_w(F_MED, 13, ui_cap("Show mesh")), y + 8, TH.text2);
        if (!A.set.meshView) {
            y += 46;
            const char *t = "The mesh view is off", *d = "Nothing is read or drawn here (or on the Overview) until you switch it back on.";
            float ch = callout_height(w, t, d);
            callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_GRID_3X3, t, d);
            y += ch + 20;
            ui_scroll_end(sc, y - top + 20);
            return;
        }
        ui_segmented(ui_id("mesh-view-mode"), (Rectangle){ x, y, 300, 32 }, vm, 2, &mode);
        y += 46;
        float h = fmaxf(460, r.y + r.height - y - 24);
        float vw = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX + 400);       /* the view gets the page's whole width */
        if (mode == 0) {
            char own[PL_PATH]; pl_join(own, sizeof own, c->s.path, case_rpath(c, "constant/polyMesh/owner"));
            if (!pl_is_file(own)) h = meshview_panel((Rectangle){ x, y, vw, h }, c);   /* says there is no mesh, offers to make one */
            else xsection_panel((Rectangle){ x, y, vw, h }, c, false);
        } else h = meshview_panel((Rectangle){ x, y, vw, h }, c);
        y += h + 20;
        ui_scroll_end(sc, y - top + 20);
        return;
    }
    if (c->region[0]) { y += region_mesh(x, y, w); ui_scroll_end(sc, y - top + 20); return; }
    if (A.meshTab == 3) {
        y += page_stlmesh(r, x, y, w);
        ui_scroll_end(sc, y - top + 40);
        return;
    }
    if (A.meshTab == 1) {
        y += page_salome(r, x, y, w);
        ui_scroll_end(sc, y - top + 40);
        return;
    }

    /* the mesh on disk must match the dictionary the solver will be judged against */
    FDFile *bmd = case_dict(c, "system/blockMeshDict");
    long dictCells = -1;
    if (bmd && !case_dict(c, "system/snappyHexMeshDict") && c->s.nRegions == 0) {
        FDEntry *be = fd_find(bmd, "blocks");
        BlockInfo blk[64];
        int nb = be ? parse_blocks(bmd, be, blk, 64) : 0;
        dictCells = nb ? 0 : -1;
        for (int i = 0; i < nb && dictCells >= 0; i++) dictCells = blk[i].cells >= 0 ? dictCells + blk[i].cells : -1;
    }
    bool countOff = c->s.meshed && dictCells > 0 && c->s.nCells > 0 && dictCells != c->s.nCells;
    if (!meshJob && (c->s.meshStale || countOff || (bmd && bmd->dirty && c->s.meshed))) {
        char body[400], a[32], b[32];
        fmt_count(a, sizeof a, c->s.nCells); fmt_count(b, sizeof b, dictCells);
        if (countOff)
            snprintf(body, sizeof body, "constant/polyMesh has %s cells but the blocks below make %s. The solver runs on the mesh on disk: "
                     "generate the mesh again (the Run page does it automatically when the dictionary is newer).", a, b);
        else if (bmd && bmd->dirty)
            snprintf(body, sizeof body, "blockMeshDict has unsaved changes. Generate the mesh again so constant/polyMesh matches them.");
        else
            snprintf(body, sizeof body, "%s was changed after constant/polyMesh was written. Generate the mesh again "
                     "(the Run page does it automatically).", c->s.meshStaleDict);
        float ch = callout_height(w, "Mesh out of date", body);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Mesh out of date", body);
        y += ch + 20;
    }

    /* tiles */
    float gap = 16, tw = (w - 3 * gap) / 4, th = 92;
    char v[64];
    fmt_count(v, sizeof v, c->s.nCells); stat_tile((Rectangle){ x, y, tw, th }, ICON_BOXES, "Cells", c->s.meshed ? v : "-", NULL);
    fmt_count(v, sizeof v, c->s.nFaces); stat_tile((Rectangle){ x + tw + gap, y, tw, th }, ICON_SQUARE, "Faces", c->s.meshed ? v : "-", NULL);
    fmt_count(v, sizeof v, c->s.nPoints); stat_tile((Rectangle){ x + 2 * (tw + gap), y, tw, th }, ICON_CIRCLE_DOT, "Points", c->s.meshed ? v : "-", NULL);
    snprintf(v, sizeof v, "%d", c->nPatches); stat_tile((Rectangle){ x + 3 * (tw + gap), y, tw, th }, ICON_LAYERS, "Patches", v, NULL);
    y += th + 34;

    FDFile *bm = case_dict(c, "system/blockMeshDict");
    if (bm) {
        y += section_head(x, y, w, "Blocks", "Each hex block is split into nx by ny by nz cells. Grading stretches cells towards one end.");
        const char *scaleKey = fd_find(bm, "scale") ? "scale" : "convertToMeters";
        Rectangle cr = prop_row(x, &y, w, "Scale", "Vertex coordinates are multiplied by this.", 34);
        if (dv_number(bm, scaleKey, (Rectangle){ cr.x, cr.y, 200, 34 }, "1")) case_rescan(c);
        char sv[64] = "1"; fd_get(bm, scaleKey, sv, sizeof sv);
        double scale = atof(sv); if (scale == 0) scale = 1;
        double mn[3], mx[3];
        if (bbox_from_vertices(bm, scale, mn, mx)) {
            cr = prop_row(x, &y, w, "Domain size", "Bounding box of the vertices after scaling.", 22);
            char d[200]; snprintf(d, sizeof d, "%g  x  %g  x  %g  m", mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
            ui_text(F_MONO, 13.5f, d, cr.x, cr.y + 2, TH.text);
        }
        /* base size: one cell size for every block */
        {
            static char bsText[32]; static char bsCase[PL_PATH];
            double cur = 0; long cells = 0; char why[160] = "";
            bool ok = blocks_base_size(bm, scale, 0, false, &cur, &cells, why, sizeof why);
            if (strcmp(bsCase, c->s.path)) { pl_strlcpy(bsCase, c->s.path, sizeof bsCase); bsText[0] = 0; }
            char ph[32] = "0.01";                                  /* the present size, as the placeholder */
            if (ok && cur > 0) snprintf(ph, sizeof ph, "%.4g", cur);
            cr = prop_row(x, &y, w, "Base size", ok ? "m: the largest cell. Every block follows it: half the base size, twice the cells along "
                                                      "each edge. Refinement between blocks, gradings and 2-D directions are kept." : why, 34);
            ui_disable(!ok);
            if (ui_input(ui_id("blk-base"), (Rectangle){ cr.x, cr.y, 200, 34 }, bsText, sizeof bsText, ph, INPUT_MONO | INPUT_NUMERIC)) {
                double b = atof(bsText);
                long est = 0;
                if (b <= 0) ui_toast(TOAST_ERR, "The base size must be a length above 0");
                else if (blocks_base_size(bm, scale, b, false, NULL, &est, why, sizeof why) && est > 50000000)
                    ui_toast(TOAST_ERR, "%ld million cells at %g m: not applied. Use a larger base size.", est / 1000000, b);  /* a typo, most likely */
                else if (blocks_base_size(bm, scale, b, true, NULL, &cells, why, sizeof why)) { ui_toast(TOAST_OK, "Base size %g m: %ld cells", b, cells); bsText[0] = 0; }
            }
            ui_disable(false);
            if (ok) {
                char t[96]; fmt_count(v, sizeof v, cells); snprintf(t, sizeof t, "%s cells now", v);
                ui_text(F_MONO, 13, t, cr.x + 214, cr.y + 9, TH.text2);
            }
        }
        FDEntry *be = fd_find(bm, "blocks");
        BlockInfo blk[64];
        int nb = be ? parse_blocks(bm, be, blk, 64) : 0;
        long total = 0;
        y += 12;
        for (int i = 0; i < nb; i++) {
            Rectangle row = { x, y, w, 62 };
            ui_rect(row, 10, TH.surface);
            ui_rect_lines(row, 10, 1, TH.lineSoft);
            char lab[32]; snprintf(lab, sizeof lab, "Block %d", i + 1);
            ui_text(F_MED, 14, lab, row.x + 18, row.y + 12, TH.text);
            ui_text_clip(F_MONO, 12, blk[i].verts, row.x + 18, row.y + 34, 200, TH.text3);
            static const char *ax[3] = { "nx", "ny", "nz" };
            float cx = row.x + 250;
            for (int k = 0; k < blk[i].nCnt && k < 3; k++) {
                char val[32]; snprintf(val, sizeof val, "%.*s", blk[i].cntB[k] - blk[i].cntA[k], bm->text + blk[i].cntA[k]);
                ui_text(F_REG, 12.5f, ax[k], cx, row.y + 22, TH.text3);
                UiId id = ui_idn(ui_id("blk"), i * 3 + k);
                if (ui_input(id, (Rectangle){ cx + 22, row.y + 14, 74, 34 }, val, sizeof val, NULL, INPUT_MONO | INPUT_NUMERIC)) {
                    pl_trim_inplace(val);
                    if (val[0]) {
                        int a = blk[i].cntA[k], b = blk[i].cntB[k];
                        fd_replace_span(bm, a, b, val);
                        nb = parse_blocks(bm, fd_find(bm, "blocks"), blk, 64);   /* offsets moved */
                    }
                }
                cx += 112;
            }
            char cells[48];
            if (blk[i].cells >= 0) { fmt_count(cells, sizeof cells, blk[i].cells); total += blk[i].cells; }
            else snprintf(cells, sizeof cells, "uses variables");
            ui_text(F_MED, 13.5f, cells, row.x + row.width - 18 - ui_text_w(F_MED, 13.5f, cells), row.y + 12, TH.text);
            ui_text_clip(F_MONO, 12, blk[i].grading, row.x + row.width - 260, row.y + 34, 242, TH.text3);
            y += 72;
        }
        if (nb > 1) {
            char t[64]; fmt_count(v, sizeof v, total); pl_fmt_ui(t, sizeof t, "%s cells in total", v);
            ui_text(F_MED, 13.5f, t, x + w - ui_text_w(F_MED, 13.5f, t) - 18, y, TH.text2);
            y += 28;
        }
        if (!nb) ui_text(F_REG, 13, "No hex blocks found (the blocks entry may use variables or code).", x, y, TH.text3), y += 26;
        y += 24;

        FDEntry *bd = fd_find(bm, "boundary");
        if (bd && bd->nChild) {
            y += section_head(x, y, w, "Patch types", "How each boundary face set behaves. Walls get wall functions; empty marks the unused direction of a 2-D case.");
            static const char *types[] = { "patch", "wall", "empty", "symmetryPlane", "symmetry", "cyclic", "wedge" };
            for (int i = 0; i < bd->nChild; i++) {
                char path[300]; snprintf(path, sizeof path, "boundary/%s/type", bd->child[i].key);
                Rectangle pr = prop_row(x, &y, w, bd->child[i].key, NULL, 34);
                if (dv_choice(bm, path, (Rectangle){ pr.x, pr.y, 240, 34 }, types, 7, true, "type")) break;
            }
            y += 24;
        }
    }

    FDFile *sn = case_dict(c, "system/snappyHexMeshDict");
    if (sn) {
        y += section_head(x, y, w, "snappyHexMesh", "Cuts the background mesh to your geometry, snaps it to the surface, then adds boundary layers.");
        Rectangle cr = prop_row(x, &y, w, "Castellate", "Refine and remove cells outside the geometry.", 22);
        dv_switch(sn, "castellatedMesh", cr.x, cr.y + 1, "false");
        cr = prop_row(x, &y, w, "Snap", "Move boundary points onto the surface.", 22);
        dv_switch(sn, "snap", cr.x, cr.y + 1, "false");
        cr = prop_row(x, &y, w, "Add layers", "Extrude prism layers along walls.", 22);
        dv_switch(sn, "addLayers", cr.x, cr.y + 1, "false");
        cr = prop_row(x, &y, w, "Max cells", "Stops refinement before the mesh exceeds this many cells.", 34);
        dv_number(sn, "castellatedMeshControls/maxGlobalCells", (Rectangle){ cr.x, cr.y, 200, 34 }, "2000000");
        cr = prop_row(x, &y, w, "Point inside the fluid", "locationInMesh: decides which side of the surface is kept.", 34);
        dv_text(sn, "castellatedMeshControls/locationInMesh", (Rectangle){ cr.x, cr.y, 300, 34 }, 0, "(0 0 0)");
        y += 24;
    }
    if (!bm && !sn && !c->s.meshed) {
        const char *msg = "SALOME tab: write a mesh script from the template and run it; Q-Continuum converts the result. "
                          "Meshes from Gmsh (gmshToFoam) or Fluent (fluentMeshToFoam) can be converted in a terminal; they land in constant/polyMesh.";
        float ch = callout_height(w, "No mesh yet", msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "No mesh yet", msg);
        y += ch + 20;
    } else if (!bm && !sn) {
        const char *msg = "Meshes imported with ideasUnvToFoam (SALOME), gmshToFoam or fluentMeshToFoam live in constant/polyMesh. Set their patch types here.";
        float ch = callout_height(w, "Imported mesh", msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, "Imported mesh", msg);
        y += ch + 20;
        if (c->s.meshed) y += region_mesh(x, y, w);
    }
    ui_scroll_end(sc, y - top + 20);
}
