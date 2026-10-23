/* page_coupling.c -- preCICE coupled simulations: the set-up at a glance, the configuration as forms and as
 * XML, and all participants run and monitored together. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "app.h"
#include "cplbuild.h"

/* download-meshes.sh (preCICE tutorials keep big meshes out of git): run in the background, log next to it */
static struct { pid_t pid; char path[PL_PATH]; } DL = { .pid = -1 };
static bool dl_running(const Coupling *c)
{
    if (DL.pid <= 0) return false;
    int st;
    if (waitpid(DL.pid, &st, WNOHANG) == DL.pid) {
        DL.pid = -1;
        bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (ok) ui_toast(TOAST_OK, "Meshes downloaded for %s", pl_basename(DL.path));
        else ui_toast(TOAST_ERR, "download-meshes.sh failed: see log.download-meshes in %s", pl_basename(DL.path));
        return false;
    }
    return !strcmp(DL.path, c->path);
}
static void dl_start(const Coupling *c)
{
    if (DL.pid > 0) return;
    char q[PL_PATH + 8], cmd[PL_PATH * 2 + 64], log[PL_PATH];
    pl_shell_quote(q, sizeof q, c->path);
    snprintf(cmd, sizeof cmd, "cd %s && sh ./download-meshes.sh", q);
    pl_join(log, sizeof log, c->path, "log.download-meshes");
    DL.pid = pl_spawn_shell(cmd, log);
    pl_strlcpy(DL.path, c->path, sizeof DL.path);
    if (DL.pid > 0) ui_toast(TOAST_INFO, "Downloading the meshes (log.download-meshes)");
    else ui_toast(TOAST_ERR, "Could not start download-meshes.sh");
}

static char cplResults[600];          /* participants with results, for the overwrite question */
static bool cplOverwriteOk;           /* answered "Run and overwrite": start on the next frame */

static const char *const SCHEMES[] = { "serial-explicit", "parallel-explicit", "serial-implicit", "parallel-implicit" };
static const char *const ACCELS[] = { "none", "constant", "aitken", "IQN-ILS", "IQN-IMVJ" };
static const char *const MAPPINGS[] = { "nearest-neighbor", "nearest-projection", "nearest-neighbor-gradient", "linear-cell-interpolation",
                                        "rbf", "rbf-pum-direct", "axial-geometric-multiscale" };

/* ui_dropdown hands back the chosen text; *sel gets its index */
static bool dd(UiId id, Rectangle r, const char *cur, const char *const *items, int n, int flags, int *sel)
{
    char out[160];
    (void)flags;
    if (!ui_dropdown(id, r, cur, items, n, 0, out, sizeof out, NULL)) return false;
    for (int i = 0; i < n; i++) if (!strcmp(items[i], out)) { *sel = i; return true; }
    return false;
}

/* ============================================================================ state */
static void cpl_open(const char *path);
void app_coupling_open(const char *path) { cpl_open(path); A.page = PG_COUPLING; }
static void cpl_open(const char *path)
{
    if (A.cpl && A.cpl->running) { ui_toast(TOAST_WARN, "Stop the running coupled simulation first"); return; }
    coupling_close(A.cpl);
    A.cpl = coupling_open(path);
    if (!A.cpl) { ui_toast(TOAST_ERR, "%s has no precice-config.xml", path); return; }
    A.cplPart = 0;
    A.cplValidateOk = -1;
    A.cplValidate[0] = 0;
    logset_close(&A.cplLogs);
}

static void cpl_scan(void)
{
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    A.nCpl = coupling_scan(ws, A.cplList, 64);
    A.cplScanned = true;
}

void coupling_frame(double now)
{
    Coupling *c = A.cpl;
    if (!c) return;
    bool was = c->running;
    coupling_poll(c, now);
    if (now - A.cplLastRead > 0.5) {
        A.cplLastRead = now;
        coupling_read_precice_logs(c);
        if (c->cfg && !c->cfg->dirty && fd_changed_on_disk(c->cfg)) { fd_reload(c->cfg); coupling_refresh(c); }
    }
    if (A.cplLogs.casePath[0]) logset_poll(&A.cplLogs, now, false);
    if (was && !c->running) {
        int failed = 0; const char *who = "";
        for (int i = 0; i < c->n; i++) if (c->p[i].failed) { failed++; who = c->p[i].name; }
        if (failed) ui_toast(TOAST_ERR, "Coupled run failed: %s stopped with an error (log.%s in its folder). The others were stopped.", who, who);
        else if (c->p[0].stopped) ui_toast(TOAST_WARN, "Coupled run stopped");
        else ui_toast(TOAST_OK, "Coupled run finished in %.0f s", now - c->startedAt);
    }
}

static void validate(Coupling *c)
{
    if (c->cfg->dirty) fd_save(c->cfg);
    char qe[PL_PATH + 8], qc[PL_PATH + 8], cmd[PL_PATH * 3];
    pl_shell_quote(qe, sizeof qe, A.env.preciceEnv);
    pl_shell_quote(qc, sizeof qc, c->path);
    snprintf(cmd, sizeof cmd, "%s%s%scd %s && precice-config-validate precice-config.xml 2>&1; echo \"exit=$?\"",
             A.env.preciceEnv[0] ? "source " : "", A.env.preciceEnv[0] ? qe : "", A.env.preciceEnv[0] ? " >/dev/null 2>&1; " : "", qc);
    char out[2048] = "";
    pl_run_wait(cmd, out, sizeof out);
    char *ex = strstr(out, "exit=");
    A.cplValidateOk = ex && atoi(ex + 5) == 0;
    if (ex) *ex = 0;
    pl_trim_inplace(out);
    /* drop preCICE's colour codes */
    char clean[2048]; int k = 0;
    for (const char *p = out; *p && k < (int)sizeof clean - 1; p++) {
        if (*p == 27) { while (*p && *p != 'm') p++; continue; }
        clean[k++] = *p;
    }
    clean[k] = 0;
    snprintf(A.cplValidate, sizeof A.cplValidate, "%s", clean[0] ? clean : (A.cplValidateOk ? "The configuration is valid." : "precice-config-validate failed."));
}

/* ============================================================================ widgets */
static float card_title(float x, float y, float w, const char *title, const char *desc)
{
    return section_head(x, y, w, title, desc);
}

/* an attribute bound to a text input; returns true when the file changed (stop drawing this frame) */
static bool attr_input(Coupling *c, XNode *n, const char *attr, Rectangle r, const char *ph)
{
    char v[256]; xml_attr(c->cfg->text, n, attr, v, sizeof v);
    UiId id = ui_idn(ui_id(attr), n->start);
    if (ui_input(id, r, v, sizeof v, ph, INPUT_MONO)) { xml_set_attr(c->cfg, &c->tree, n, attr, v); coupling_refresh(c); return true; }
    return false;
}

/* a positive number (an integer when whole); anything else is refused with a toast and not written */
static bool attr_number(Coupling *c, XNode *n, const char *attr, Rectangle r, const char *ph, bool whole)
{
    char v[256]; xml_attr(c->cfg->text, n, attr, v, sizeof v);
    UiId id = ui_idn(ui_id(attr), n->start);
    if (!ui_input(id, r, v, sizeof v, ph, INPUT_MONO | INPUT_NUMERIC)) return false;
    pl_trim_inplace(v);
    char *e; double d = strtod(v, &e);
    if (e == v || *e || !(d > 0) || (whole && d != floor(d))) {
        ui_toast(TOAST_ERR, "%s must be a %s above 0, not \"%s\"", attr, whole ? "whole number" : "number", v);
        return false;
    }
    xml_set_attr(c->cfg, &c->tree, n, attr, v); coupling_refresh(c);
    return true;
}

static bool attr_choice(Coupling *c, XNode *n, const char *attr, Rectangle r, const char *const *items, int ni)
{
    char v[256]; xml_attr(c->cfg->text, n, attr, v, sizeof v);
    int sel = -1;
    for (int i = 0; i < ni; i++) if (!strcmp(items[i], v)) sel = i;
    UiId id = ui_idn(ui_id(attr), n->start + 7);
    if (dd(id, r, v, items, ni, 0, &sel) && sel >= 0) { xml_set_attr(c->cfg, &c->tree, n, attr, items[sel]); coupling_refresh(c); return true; }
    return false;
}

/* names of the children <tag name="..."> of the configuration root */
static int names_of(Coupling *c, const char *tagPrefix, char out[][64], int max)
{
    XNode *r = coupling_root(c);
    int n = 0;
    for (int i = 0; r && i < r->nChild && n < max; i++)
        if (xml_tag_is(&r->child[i], tagPrefix)) xml_attr(c->cfg->text, &r->child[i], "name", out[n++], 64);
    return n;
}

/* ============================================================================ overview */
static float diagram(Coupling *c, float x, float y, float w)
{
    if (c->n == 0) return 0;
    float bh = 86, gapX = c->n > 1 ? (w - c->n * 200) / (c->n - 1) : 0;
    if (gapX < 40) gapX = 40;
    float bw = c->n > 1 ? (w - (c->n - 1) * gapX) / c->n : 260;
    float cx[CPL_MAX_PART] = { 0 };
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        Rectangle b = { x + i * (bw + gapX), y, bw, bh };
        Color col = TH.lcars ? TH.lc[(i * 3 + 2) % 8] : TH.cat[i % 5];
        if (TH.lcars) {
            ui_rect(b, 22, col);
            ui_text(F_SEMI, 22, ui_upper(p->name), b.x + 18, b.y + 12, BLACK);
            char sub[160]; pl_fmt_ui(sub, sizeof sub, "%s%s%s", p->dirSel >= 0 ? p->dirs[p->dirSel] : "no folder", p->solver[0] ? "  ·  " : "", p->solver);
            ui_text_clip(F_MED, 14, sub, b.x + 18, b.y + 46, bw - 36, BLACK);
        } else {
            ui_rect(b, 12, ui_alpha(col, 0.16f));
            ui_rect_lines(b, 12, 1.5f, col);
            ui_text(F_SEMI, 18, p->name, b.x + 16, b.y + 14, TH.text);
            char sub[160]; pl_fmt_ui(sub, sizeof sub, "%s%s%s", p->dirSel >= 0 ? p->dirs[p->dirSel] : "no folder", p->solver[0] ? "  ·  " : "", p->solver);
            ui_text_clip(F_MONO, 12.5f, sub, b.x + 16, b.y + 46, bw - 32, TH.text2);
        }
        if (p->running) ui_spinner(b.x + bw - 22, b.y + 22, 7, TH.lcars ? BLACK : TH.accent);
        else if (p->failed) ui_icon(ICON_CIRCLE_X, b.x + bw - 22, b.y + 22, 16, TH.err);
        else if (p->done) ui_icon(ICON_CIRCLE_CHECK, b.x + bw - 22, b.y + 22, 16, TH.lcars ? BLACK : TH.ok);
        cx[i] = b.x + bw / 2;
    }
    /* exchanges: one arrow line per exchange below the blocks */
    float ay = y + bh + 18;
    XNode *cs = coupling_scheme(c);
    int k = 0;
    for (int i = 0; cs && i < cs->nChild; i++) {
        if (strcmp(cs->child[i].tag, "exchange")) continue;
        char data[64], mesh[64], fr[64], to[64];
        xml_attr(c->cfg->text, &cs->child[i], "data", data, sizeof data);
        xml_attr(c->cfg->text, &cs->child[i], "mesh", mesh, sizeof mesh);
        xml_attr(c->cfg->text, &cs->child[i], "from", fr, sizeof fr);
        xml_attr(c->cfg->text, &cs->child[i], "to", to, sizeof to);
        int a = -1, b = -1;
        for (int j = 0; j < c->n; j++) { if (!strcmp(c->p[j].name, fr)) a = j; if (!strcmp(c->p[j].name, to)) b = j; }
        if (a < 0 || b < 0) continue;
        float yy = ay + k * 30;
        Color col = TH.lcars ? TH.lc[(k + 1) % 8] : TH.text2;
        float x0 = cx[a], x1 = cx[b];
        ui_line(x0, y + bh, x0, yy, 2, ui_alpha(col, 0.5f));
        ui_line(x0, yy, x1, yy, 2.5f, col);
        float dir = x1 > x0 ? 1 : -1;
        DrawTriangle((Vector2){ x1, yy }, (Vector2){ x1 - 9 * dir, dir > 0 ? yy - 6 : yy + 6 }, (Vector2){ x1 - 9 * dir, dir > 0 ? yy + 6 : yy - 6 }, col);
        char lab[160]; snprintf(lab, sizeof lab, "%s  on %s", data, mesh);
        float lw = ui_text_w(F_MONO, 12.5f, lab);
        float lx = (x0 + x1) / 2 - lw / 2;
        DrawRectangleRec((Rectangle){ lx - 6, yy - 10, lw + 12, 20 }, TH.bg);
        ui_text(F_MONO, 12.5f, lab, lx, yy - 8, col);
        k++;
    }
    return bh + 18 + k * 30 + 16;
}

static void scheme_summary(Coupling *c, char *out, int cap)
{
    XNode *cs = coupling_scheme(c);
    if (!cs) { snprintf(out, (size_t)cap, "No coupling scheme yet."); return; }
    char tw[32] = "?", mt[32] = "?", mi[32] = "", acc[64] = "no acceleration", conv[32] = "";
    XNode *e;
    if ((e = xml_child(cs, "time-window-size", 0))) xml_attr(c->cfg->text, e, "value", tw, sizeof tw);
    if ((e = xml_child(cs, "max-time", 0))) xml_attr(c->cfg->text, e, "value", mt, sizeof mt);
    if ((e = xml_child(cs, "max-iterations", 0))) xml_attr(c->cfg->text, e, "value", mi, sizeof mi);
    int nConv = 0;
    for (int i = 0; i < cs->nChild; i++) {
        if (strstr(cs->child[i].tag, "convergence-measure")) nConv++;
        if (xml_tag_is(&cs->child[i], "acceleration:")) snprintf(acc, sizeof acc, "%s acceleration", cs->child[i].tag + 13);
    }
    if (nConv) snprintf(conv, sizeof conv, ", %d convergence measure%s", nConv, nConv == 1 ? "" : "s");
    bool impl = strstr(cs->tag, "implicit") != NULL;
    snprintf(out, (size_t)cap, "%s: time windows of %s up to t = %s%s%s%s%s.", cs->tag + 16, tw, mt,
             impl && mi[0] ? ", at most " : "", impl && mi[0] ? mi : "", impl && mi[0] ? " iterations per window" : "", impl ? "" : "");
    if (impl) { size_t L = strlen(out); snprintf(out + L - 1, (size_t)cap - L + 1, "%s, %s.", conv, acc); }
}

static float tab_overview(Coupling *c, float x, float y, float w)
{
    float y0 = y;
    y += diagram(c, x, y, w) + 8;
    char sum[400]; scheme_summary(c, sum, sizeof sum);
    y += ui_text_wrap(F_REG, 14, sum, (Rectangle){ x, y, w, 80 }, 20, TH.text2, true) + 18;

    y += card_title(x, y, w, "Set-up checks", "Each participant's folder, preciceDict, controlDict and interface patches against precice-config.xml.");
    CCheck ck[48]; int n = coupling_check(c, ck, 48);
    for (int i = 0; i < n; i++) {
        Color ic = ck[i].kind == 2 ? TH.err : ck[i].kind ? TH.warn : TH.ok;
        ui_icon(ck[i].kind == 2 ? ICON_CIRCLE_X : ck[i].kind ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK, x + 10, y + 11, 16, ic);
        ui_text(F_SEMI, 13.5f, ck[i].who, x + 30, y + 2, TH.text);
        float tw = w - 30 - 150 - (ck[i].fix ? 180 : 0);
        float h = ui_text_wrap(F_REG, 13, ck[i].msg, (Rectangle){ x + 180, y + 2, tw, 200 }, 18, TH.text2, true);
        int part = -1;
        for (int k = 0; k < c->n; k++) if (!strcmp(c->p[k].name, ck[i].who)) part = k;
        if (ck[i].fix == 1) {
            if (ui_button(ui_idn(ui_id("cpl-fix"), i), (Rectangle){ x + w - 170, y - 4, 170, 30 }, ICON_WRENCH, "Add the adapter", BTN_SECONDARY)) {
                if (coupling_fix_adapter(c, part)) ui_toast(TOAST_OK, "Added preCICE_Adapter to %s's controlDict", ck[i].who);
                else ui_toast(TOAST_ERR, "Could not edit %s's controlDict", ck[i].who);
            }
        } else if (ck[i].fix == 2) {                         /* the tutorial's download-meshes.sh, in the background */
            bool busy = dl_running(c);
            ui_disable(busy);
            if (ui_button(ui_idn(ui_id("cpl-fix"), i), (Rectangle){ x + w - 170, y - 4, 170, 30 }, ICON_FOLDER_OPEN, busy ? "Downloading..." : "Download meshes", BTN_SECONDARY))
                dl_start(c);
            ui_disable(false);
        } else if (ck[i].fix >= 100 && part >= 0 && ck[i].fix - 100 < c->p[part].nDirs) {
            char lab[160]; snprintf(lab, sizeof lab, "Use %s", c->p[part].dirs[ck[i].fix - 100]);
            if (ui_button(ui_idn(ui_id("cpl-fix"), i), (Rectangle){ x + w - 170, y - 4, 170, 30 }, ICON_REFRESH_CW, lab, BTN_SECONDARY)) {
                c->p[part].dirSel = ck[i].fix - 100;
                coupling_save_choices(c); coupling_refresh(c);
                if (A.cplPart == part) logset_close(&A.cplLogs);
                ui_toast(TOAST_OK, "%s now runs from %s/", c->p[part].name, c->p[part].dirs[c->p[part].dirSel]);
            }
        }
        y += fmaxf(26, h) + 10;
    }
    y += 10;
    y += card_title(x, y, w, "preCICE's own check", "precice-config-validate reads the configuration the way the participants will.");
    float bw = ui_button_width(ICON_CIRCLE_CHECK, "Validate");
    if (ui_button(ui_id("cpl-val"), (Rectangle){ x, y, bw, 34 }, ICON_CIRCLE_CHECK, "Validate", BTN_SECONDARY)) validate(c);
    y += 46;
    if (A.cplValidateOk >= 0) {
        float ch = callout_height(w, A.cplValidateOk ? "Valid" : "preCICE rejects this configuration", A.cplValidate);
        callout((Rectangle){ x, y, w, ch }, A.cplValidateOk ? TOAST_OK : TOAST_ERR, A.cplValidateOk ? ICON_CIRCLE_CHECK : ICON_CIRCLE_X,
                A.cplValidateOk ? "Valid" : "preCICE rejects this configuration", A.cplValidate);
        y += ch + 10;
    }
    return y - y0;
}

/* ============================================================================ configuration forms */
static float tab_config(Coupling *c, float x, float y, float w)
{
    float y0 = y;
    XNode *r = coupling_root(c);
    if (!r) { ui_text(F_REG, 14, "No <precice-configuration> element: fix the XML first.", x, y, TH.err); return 30; }
    XNode *cs = coupling_scheme(c);
    char parts[CPL_MAX_PART][64]; int np = 0;
    for (int i = 0; i < c->n; i++) pl_strlcpy(parts[np++], c->p[i].name, 64);
    const char *pnames[CPL_MAX_PART]; for (int i = 0; i < np; i++) pnames[i] = parts[i];
    char datas[32][64], meshes[32][64];
    int nd = names_of(c, "data:", datas, 32), nm = names_of(c, "mesh", meshes, 32);
    const char *dn[32], *mn[32];
    for (int i = 0; i < nd; i++) dn[i] = datas[i];
    for (int i = 0; i < nm; i++) mn[i] = meshes[i];

    /* ---- scheme */
    y += card_title(x, y, w, "Coupling scheme", "Serial: participants take turns; parallel: both solve each window at once. Implicit iterates every window to convergence.");
    if (cs) {
        Rectangle pr = prop_row(x, &y, w, "Scheme", "Implicit coupling is what stiff CHT and FSI need.", 34);
        int sel = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(cs->tag + 16, SCHEMES[i])) sel = i;
        if (dd(ui_id("cpl-scheme"), (Rectangle){ pr.x, pr.y, 260, 34 }, cs->tag + 16, SCHEMES, 4, 0, &sel) && sel >= 0) {
            char nt[64]; snprintf(nt, sizeof nt, "coupling-scheme:%s", SCHEMES[sel]);
            xml_rename(c->cfg, &c->tree, cs, nt);
            coupling_refresh(c);
            return y - y0;
        }
        static const struct { const char *tag, *label, *hint, *ph; } vals[] = {
            { "time-window-size", "Time window", "Data are exchanged once per window (s).", "0.01" },
            { "max-time", "End time", "The coupled run stops here (s).", "1" },
            { "max-iterations", "Max iterations", "Per window, implicit schemes only.", "30" } };
        for (int k = 0; k < 3; k++) {
            XNode *e = xml_child(cs, vals[k].tag, 0);
            if (!e && k == 2 && !strstr(cs->tag, "implicit")) continue;
            pr = prop_row(x, &y, w, vals[k].label, vals[k].hint, 34);
            if (e) { if (attr_number(c, e, "value", (Rectangle){ pr.x, pr.y, 200, 34 }, vals[k].ph, k == 2)) return y - y0; }
            else if (ui_button(ui_idn(ui_id("cpl-addval"), k), (Rectangle){ pr.x, pr.y, 120, 34 }, ICON_PLUS, "Add", BTN_GHOST)) {
                char el[160]; snprintf(el, sizeof el, "<%s value=\"%s\" />", vals[k].tag, vals[k].ph);
                xml_append_child(c->cfg, &c->tree, cs, el); coupling_refresh(c); return y - y0;
            }
        }
        XNode *pp = xml_child(cs, "participants", 0);
        if (pp) {
            pr = prop_row(x, &y, w, "Order", "first and second participant (in serial schemes the first solves first).", 34);
            if (attr_choice(c, pp, "first", (Rectangle){ pr.x, pr.y, 180, 34 }, pnames, np)) return y - y0;
            if (attr_choice(c, pp, "second", (Rectangle){ pr.x + 190, pr.y, 180, 34 }, pnames, np)) return y - y0;
        }
        /* acceleration */
        XNode *acc = NULL;
        for (int i = 0; i < cs->nChild; i++) if (xml_tag_is(&cs->child[i], "acceleration:")) acc = &cs->child[i];
        if (strstr(cs->tag, "implicit")) {
            pr = prop_row(x, &y, w, "Acceleration", "Aitken for most CHT; IQN-ILS for strong FSI coupling.", 34);
            int as = 0;
            for (int i = 1; i < 5; i++) if (acc && !strcmp(acc->tag + 13, ACCELS[i])) as = i;
            if (dd(ui_id("cpl-acc"), (Rectangle){ pr.x, pr.y, 200, 34 }, ACCELS[as], ACCELS, 5, 0, &as)) {
                if (as == 0 && acc) xml_remove(c->cfg, &c->tree, acc);
                else if (acc) { char nt[64]; snprintf(nt, sizeof nt, "acceleration:%s", ACCELS[as]); xml_rename(c->cfg, &c->tree, acc, nt); }
                else if (as > 0) {
                    char el[400], ex[64] = "", em[64] = "";
                    XNode *x0 = xml_child(cs, "exchange", 0);
                    if (x0) { xml_attr(c->cfg->text, x0, "data", ex, sizeof ex); xml_attr(c->cfg->text, x0, "mesh", em, sizeof em); }
                    snprintf(el, sizeof el, "<acceleration:%s>\n  <data mesh=\"%s\" name=\"%s\" />\n  <initial-relaxation value=\"0.5\" />\n</acceleration:%s>",
                             ACCELS[as], em, ex, ACCELS[as]);
                    xml_append_child(c->cfg, &c->tree, cs, el);
                }
                coupling_refresh(c);
                return y - y0;
            }
            if (acc) {
                XNode *ir = xml_child(acc, "initial-relaxation", 0);
                if (ir) { pr = prop_row(x, &y, w, "Initial relaxation", "Under-relaxation of the first iteration.", 34);
                          if (attr_input(c, ir, "value", (Rectangle){ pr.x, pr.y, 200, 34 }, "0.5")) return y - y0; }
            }
        }
        y += 18;
        /* ---- exchanges */
        y += card_title(x, y, w, "Exchanges", "What goes where, once per time window. The mesh is the one the data live on when exchanged.");
        float cw = (w - 50) / 4;
        static const char *heads[4] = { "Data", "Mesh", "From", "To" };
        for (int k = 0; k < 4; k++) ui_text(F_MED, 12.5f, heads[k], x + k * (cw + 4), y, TH.text3);
        y += 22;
        for (int i = 0; i < cs->nChild; i++) {
            XNode *e = &cs->child[i];
            if (strcmp(e->tag, "exchange")) continue;
            if (attr_choice(c, e, "data", (Rectangle){ x, y, cw, 34 }, dn, nd)) return y - y0;
            if (attr_choice(c, e, "mesh", (Rectangle){ x + cw + 4, y, cw, 34 }, mn, nm)) return y - y0;
            if (attr_choice(c, e, "from", (Rectangle){ x + 2 * (cw + 4), y, cw, 34 }, pnames, np)) return y - y0;
            if (attr_choice(c, e, "to", (Rectangle){ x + 3 * (cw + 4), y, cw, 34 }, pnames, np)) return y - y0;
            if (ui_button(ui_idn(ui_id("cpl-exdel"), e->start), (Rectangle){ x + w - 36, y, 34, 34 }, ICON_X, NULL, BTN_ICON)) {
                xml_remove(c->cfg, &c->tree, e); coupling_refresh(c); return y - y0;
            }
            y += 40;
        }
        if (ui_button(ui_id("cpl-exadd"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Add exchange"), 30 }, ICON_PLUS, "Add exchange", BTN_GHOST) && np >= 2) {
            char el[512]; pl_fmt_ui(el, sizeof el, "<exchange data=\"%s\" mesh=\"%s\" from=\"%s\" to=\"%s\" />", nd ? datas[0] : "", nm ? meshes[0] : "", parts[0], parts[1]);
            xml_append_child(c->cfg, &c->tree, cs, el); coupling_refresh(c); return y - y0;
        }
        y += 52;
    }

    /* ---- data */
    y += card_title(x, y, w, "Data", "Fields exchanged at the interface. CHT: Temperature and Heat-Flux (or Heat-Transfer-Coefficient + Sink-Temperature).");
    for (int i = 0; i < r->nChild; i++) {
        XNode *e = &r->child[i];
        if (!xml_tag_is(e, "data:")) continue;
        if (attr_input(c, e, "name", (Rectangle){ x, y, 260, 34 }, "Temperature")) return y - y0;
        static const char *const kinds[] = { "scalar", "vector" };
        int ks = !strcmp(e->tag, "data:vector");
        if (dd(ui_idn(ui_id("cpl-dkind"), e->start), (Rectangle){ x + 270, y, 140, 34 }, kinds[ks], kinds, 2, 0, &ks)) {
            char nt[32]; snprintf(nt, sizeof nt, "data:%s", kinds[ks]);
            xml_rename(c->cfg, &c->tree, e, nt); coupling_refresh(c); return y - y0;
        }
        y += 40;
    }
    if (ui_button(ui_id("cpl-dadd"), (Rectangle){ x, y, ui_button_width(ICON_PLUS, "Add data"), 30 }, ICON_PLUS, "Add data", BTN_GHOST)) {
        /* new data go after the last data element */
        int last = -1;
        for (int i = 0; i < r->nChild; i++) if (xml_tag_is(&r->child[i], "data:")) last = i;
        if (last >= 0) {
            char ind[32] = ""; int a = r->child[last].start, ls = a; while (ls > 0 && c->cfg->text[ls - 1] != '\n') ls--;
            snprintf(ind, sizeof ind, "%.*s", a - ls, c->cfg->text + ls);
            char el[128]; snprintf(el, sizeof el, "\n%s<data:scalar name=\"New-Data\" />", ind);
            fd_replace_span(c->cfg, r->child[last].end, r->child[last].end, el);
        } else xml_append_child(c->cfg, &c->tree, r, "<data:scalar name=\"New-Data\" />");
        coupling_refresh(c);
        return y - y0;
    }
    y += 52;

    /* ---- participants' mappings */
    y += card_title(x, y, w, "Mappings", "How read data move from the other participant's mesh to this one (consistent for temperature, conservative for forces).");
    for (int i = 0; i < r->nChild; i++) {
        XNode *pe = &r->child[i];
        if (strcmp(pe->tag, "participant")) continue;
        char pn[64]; xml_attr(c->cfg->text, pe, "name", pn, sizeof pn);
        for (int j = 0; j < pe->nChild; j++) {
            XNode *m = &pe->child[j];
            if (!xml_tag_is(m, "mapping:")) continue;
            ui_text(F_SEMI, 13.5f, pn, x, y + 9, TH.text);
            int ms = -1;
            for (int k = 0; k < 7; k++) if (!strcmp(m->tag + 8, MAPPINGS[k])) ms = k;
            if (dd(ui_idn(ui_id("cpl-map"), m->start), (Rectangle){ x + 110, y, 230, 34 }, m->tag + 8, MAPPINGS, 7, 0, &ms) && ms >= 0) {
                char nt[64]; snprintf(nt, sizeof nt, "mapping:%s", MAPPINGS[ms]);
                xml_rename(c->cfg, &c->tree, m, nt); coupling_refresh(c); return y - y0;
            }
            static const char *const dirs[] = { "read", "write" }, *const cons[] = { "consistent", "conservative", "scaled-consistent-surface", "scaled-consistent-volume" };
            if (attr_choice(c, m, "direction", (Rectangle){ x + 350, y, 100, 34 }, dirs, 2)) return y - y0;
            if (attr_choice(c, m, "constraint", (Rectangle){ x + 460, y, 200, 34 }, cons, 4)) return y - y0;
            char ft[160], a1[64], a2[64];
            xml_attr(c->cfg->text, m, "from", a1, sizeof a1); xml_attr(c->cfg->text, m, "to", a2, sizeof a2);
            snprintf(ft, sizeof ft, "%s -> %s", a1, a2);
            ui_text_clip(F_MONO, 12.5f, ft, x + 670, y + 9, w - 670, TH.text2);
            y += 40;
        }
    }
    y += 16;
    ui_text_wrap(F_REG, 12.5f, "Everything else (meshes, m2n, convergence measures, exports, logging) is on the XML tab.", (Rectangle){ x, y, w, 40 }, 18, TH.text3, true);
    y += 30;
    return y - y0;
}

/* ============================================================================ run */
static void xy_chart(Rectangle r, const char *title, const float *xs, const float *ys, int n, bool logy, Color col, const char *xlab)
{
    ui_rect(r, 12, TH.chartBg);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    ui_text(F_MED, 13, title, r.x + 14, r.y + 10, TH.text2);
    Rectangle p = { r.x + 50, r.y + 36, r.width - 66, r.height - 62 };
    if (n < 1) { ui_text_center(F_REG, 12.5f, "No data yet", p, TH.text3); return; }
    float x0 = xs[0], x1 = xs[0], y0 = 1e30f, y1 = -1e30f;
    for (int i = 0; i < n; i++) {
        x0 = fminf(x0, xs[i]); x1 = fmaxf(x1, xs[i]);
        float v = logy ? (ys[i] > 0 ? log10f(ys[i]) : NAN) : ys[i];
        if (isfinite(v)) { y0 = fminf(y0, v); y1 = fmaxf(y1, v); }
    }
    if (x1 <= x0) x1 = x0 + 1;
    if (!(y1 > y0)) { y0 -= 1; y1 += 1; }
    if (logy) { y0 = floorf(y0); y1 = ceilf(y1); } else { y0 = fminf(0, y0); y1 = ceilf(y1 * 1.1f); }
    int nt = logy ? (int)(y1 - y0) : 4;
    if (nt < 1) nt = 1;
    int every = nt > 6 ? (nt + 5) / 6 : 1;
    for (int t = 0; t <= nt; t += every) {
        float v = y0 + (y1 - y0) * t / nt, py = p.y + p.height - (v - y0) / (y1 - y0) * p.height;
        DrawRectangleRec((Rectangle){ p.x, py, p.width, 1 }, TH.grid);
        char lab[24]; if (logy) snprintf(lab, sizeof lab, "1e%d", (int)v); else snprintf(lab, sizeof lab, "%g", v);
        ui_text(F_MONO, 11, lab, p.x - 8 - ui_text_w(F_MONO, 11, lab), py - 7, TH.text3);
    }
    char xl[64]; snprintf(xl, sizeof xl, "%s %g .. %g", xlab, x0, x1);
    ui_text(F_REG, 11.5f, xl, p.x + p.width - ui_text_w(F_REG, 11.5f, xl), p.y + p.height + 6, TH.text3);
    Vector2 prev = { 0 }; bool have = false;
    int step = n > p.width ? n / (int)p.width : 1;
    for (int i = 0; i < n; i += step) {
        float v = logy ? (ys[i] > 0 ? log10f(ys[i]) : NAN) : ys[i];
        if (!isfinite(v)) { have = false; continue; }
        Vector2 pt = { p.x + (xs[i] - x0) / (x1 - x0) * p.width, p.y + p.height - (v - y0) / (y1 - y0) * p.height };
        if (have) DrawLineEx(prev, pt, 1.6f, col);
        prev = pt; have = true;
    }
}

/* one pill per participant; the chosen one's solver log is shown */
static float participant_pills(Coupling *c, float x, float y)
{
    float lx = x;
    for (int i = 0; i < c->n; i++) {
        const char *lab = c->p[i].name;
        float bw = ui_text_w(F_MED, 13, lab) + 28;
        Rectangle pill = { lx, y, bw, 30 };
        bool on = A.cplPart == i;
        if (ui_hovered(pill)) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        ui_rect(pill, 15, on ? (TH.lcars ? TH.lc[(i * 3 + 2) % 8] : TH.accent) : ui_hovered(pill) ? TH.hover : BLANK);
        if (!on) ui_rect_lines(pill, 15, 1, TH.line);
        ui_text(F_MED, 13, lab, lx + 14, y + 6, on ? TH.textOnAccent : TH.text2);
        if (ui_clicked(pill) && !on) { A.cplPart = i; logset_close(&A.cplLogs); A.runLog = -1; A.runFollow = true; }
        lx += bw + 8;
    }
    return 42;
}

/* the chosen participant's log: its log.<name>, or the newest */
static int participant_log(Coupling *c, int *k)
{
    if (A.cplPart >= c->n) A.cplPart = 0;
    CParticipant *p = &c->p[A.cplPart];
    char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
    if (d[0] && strcmp(A.cplLogs.casePath, d)) logset_open(&A.cplLogs, d);
    char ln[96]; snprintf(ln, sizeof ln, "log.%s", p->name);
    *k = logset_find(&A.cplLogs, ln);
    return A.runLog >= 0 && A.runLog < A.cplLogs.n ? A.runLog : *k >= 0 ? *k : logset_newest(&A.cplLogs);
}

static float tab_run(Coupling *c, float x, float y, float w, float bottom)
{
    float y0 = y;
    /* participants table */
    float c1 = 150, c2 = 240, c3 = 130;
    static const char *heads[] = { "Participant", "Folder", "Processes", "Status" };
    float hx[] = { x, x + c1, x + c1 + c2, x + c1 + c2 + c3 };
    for (int k = 0; k < 4; k++) ui_text(F_MED, 12.5f, heads[k], hx[k], y, TH.text3);
    y += 24;
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        ui_text(F_SEMI, 14, p->name, x, y + 8, TH.text);
        ui_disable(c->running);
        const char *items[CPL_MAX_DIRS]; for (int k = 0; k < p->nDirs; k++) items[k] = p->dirs[k];
        int sel = p->dirSel;
        if (p->nDirs && dd(ui_idn(ui_id("cpl-dir"), i), (Rectangle){ x + c1, y, c2 - 10, 34 }, p->dirSel >= 0 ? p->dirs[p->dirSel] : "",
                                    items, p->nDirs, 0, &sel) && sel >= 0) {
            p->dirSel = sel; coupling_save_choices(c); coupling_refresh(c);
            if (A.cplPart == i) logset_close(&A.cplLogs);
        }
        if (!p->nDirs) ui_text(F_REG, 12.5f, "no folder", x + c1, y + 9, TH.err);
        if (p->foam) {
            if (ui_button(ui_idn(ui_id("cpl-np-"), i), (Rectangle){ x + c1 + c2, y, 34, 34 }, ICON_CHEVRON_LEFT, NULL, BTN_SECONDARY) && p->procs > 1) { p->procs--; coupling_save_choices(c); }
            char np[16]; snprintf(np, sizeof np, "%d", p->procs);
            ui_text_center(F_MONO, 13.5f, np, (Rectangle){ x + c1 + c2 + 36, y, 40, 34 }, TH.text);
            if (ui_button(ui_idn(ui_id("cpl-np+"), i), (Rectangle){ x + c1 + c2 + 78, y, 34, 34 }, ICON_CHEVRON_RIGHT, NULL, BTN_SECONDARY) && p->procs < 256) { p->procs++; coupling_save_choices(c); }
        } else ui_text(F_REG, 12.5f, "-", x + c1 + c2 + 50, y + 9, TH.text3);
        char def[512]; coupling_default_cmd(c, p, def, sizeof def);
        ui_disable(false);
        const char *st = p->running ? "running" : p->failed ? "failed" : p->stopped ? "stopped" : p->done ? "finished" : "ready";
        Color sc = p->running ? TH.accent : p->failed ? TH.err : p->stopped ? TH.warn : p->done ? TH.ok : TH.text3;
        float sx = x + c1 + c2 + c3;
        if (p->running) ui_spinner(sx + 6, y + 17, 6, sc); else DrawCircleV((Vector2){ sx + 6, y + 17 }, 4, sc);
        char stt[64];
        if (p->running || p->done) { char d[32]; pl_duration(d, sizeof d, (p->running ? GetTime() : p->t1) - p->t0); snprintf(stt, sizeof stt, "%s  %s", ui_cap(st), d); }
        else snprintf(stt, sizeof stt, "%s", ui_cap(st));
        ui_text(F_MED, 13, stt, sx + 18, y + 9, sc);
        y += 40;
        ui_disable(c->running);
        ui_text(F_REG, 12, "command", x + 14, y + 10, TH.text3);
        if (ui_input(ui_idn(ui_id("cpl-cmd"), i), (Rectangle){ x + c1, y, w - c1, 32 }, p->cmd, sizeof p->cmd, def[0] ? def : "command that starts this solver", INPUT_MONO))
            coupling_save_choices(c);
        ui_disable(false);
        y += 44;
    }
    y += 6;
    if (c->running) {
        if (ui_button(ui_id("cpl-stop"), (Rectangle){ x, y, 200, 42 }, ICON_SQUARE, "Stop all", BTN_DANGER)) coupling_stop(c, GetTime());
    } else {
        ui_disable(false);                                   /* a coupled run can go alongside case runs */
        bool go = ui_button(ui_id("cpl-go"), (Rectangle){ x, y, 240, 42 }, ICON_PLAY, "Run coupled", BTN_PRIMARY);
        if (go || cplOverwriteOk) {
            CCheck ck[48]; int n = coupling_check(c, ck, 48), errs = 0;
            for (int i = 0; i < n; i++) if (ck[i].kind == 2) errs++;
            char err[256];
            const FoamInstall *fi = env_active(&A.env, &A.set);
            bool confirmed = cplOverwriteOk;
            cplOverwriteOk = false;
            if (errs) { ui_toast(TOAST_ERR, "%d set-up problem%s: see Overview", errs, errs == 1 ? "" : "s"); A.cplTab = 0; }
            else if (!confirmed && coupling_existing_results(c, cplResults, sizeof cplResults)) A.modal = MD_CPL_RESULTS;
            else if (!coupling_start(c, fi ? fi->bashrc : "", A.env.preciceEnv, GetTime(), err, sizeof err)) ui_toast(TOAST_ERR, "%s", err);
            else { ui_toast(TOAST_INFO, "Started %d participants", c->n); logset_close(&A.cplLogs); }
        }
        ui_disable(false);
    }
    y += 58;

    /* preCICE: iterations per window and convergence */
    float hw = (w - 16) / 2, ch = 200;
    CParticipant *itp = NULL, *cvp = NULL;
    for (int i = 0; i < c->n; i++) { if (c->p[i].nIt && !itp) itp = &c->p[i]; if (c->p[i].nCv && !cvp) cvp = &c->p[i]; }
    xy_chart((Rectangle){ x, y, hw, ch }, "Coupling iterations per time window", itp ? itp->itWin : NULL, itp ? itp->itN : NULL, itp ? itp->nIt : 0, false, TH.accent, "window");
    char ct[128]; snprintf(ct, sizeof ct, "Convergence  %s", cvp ? cvp->cvName : "");
    xy_chart((Rectangle){ x + hw + 16, y, hw, ch }, ct, cvp ? cvp->cvX : NULL, cvp ? cvp->cvR : NULL, cvp ? cvp->nCv : 0, true, TH.lcars ? TH.lc[5] : TH.cat[1], "iteration");
    y += ch + 18;

    /* one participant's solver: residuals and log */
    y += participant_pills(c, x, y);
    if (A.cplPart >= c->n) A.cplPart = 0;
    if (c->n) {
        int k, sel = participant_log(c, &k);
        float rest = bottom - y - 20;
        float rh = fmaxf(200, rest * 0.5f);
        run_chart((Rectangle){ x, y, w, rh }, k >= 0 ? &A.cplLogs.logs[k] : NULL);
        y += rh + 14;
        run_console((Rectangle){ x, y, w, fmaxf(220, bottom - y - 20) }, &A.cplLogs, sel);
        y += fmaxf(220, bottom - y - 20) + 10;
    }
    return y - y0;
}

/* ============================================================================ page */
static void coupling_list(Rectangle r)
{
    if (!A.cplScanned) cpl_scan();
    UiScroll *sc = &A.scr[14];
    float y = ui_scroll_begin(sc, r) + 28, x = r.x + 16, w = r.width - 32;   /* 28: below the frame's rounded inner corner */
    ui_text(F_SEMI, TH.lcars ? 20 : 16, ui_cap("Coupled runs"), x, y, head_color());
    y += 34;
    float bw = (w - 8) / 2;
    if (ui_button(ui_id("cpl-new"), (Rectangle){ x, y, bw, 32 }, ICON_PLUS, "From tutorial", BTN_SECONDARY)) { A.modal = MD_CPL_NEW; A.mErr[0] = 0; A.tutSel = -1; }
    if (ui_button(ui_id("cpl-openf"), (Rectangle){ x + bw + 8, y, bw, 32 }, ICON_FOLDER_OPEN, "Open folder", BTN_GHOST)) { A.modal = MD_CPL_OPEN; A.mErr[0] = 0; A.mText[0] = 0; }
    y += 40;
    if (ui_button(ui_id("cpl-build"), (Rectangle){ x, y, w, 32 }, ICON_GIT_FORK, "Couple two cases", BTN_SECONDARY)) A.modal = MD_CPL_BUILD;
    y += 44;
    if (ui_button(ui_id("cpl-rescan"), (Rectangle){ x, y, w, 26 }, ICON_REFRESH_CW, "Rescan workspace", BTN_GHOST)) cpl_scan();
    y += 36;
    /* the open coupled run, when it lives outside the workspace */
    bool listed = false;
    for (int i = 0; i < A.nCpl && A.cpl; i++) if (!strcmp(A.cpl->path, A.cplList[i].path)) listed = true;
    if (A.cpl && !listed) {
        Rectangle rr = { x - 6, y, w + 12, 50 };
        ui_row(ui_id("cpl-ext"), rr, true);
        ui_text_clip(F_SEMI, 14, pl_basename(A.cpl->path), x + 6, y + 6, w - 12, TH.text);
        ui_text_clip(F_REG, 12, "open, outside the workspace", x + 6, y + 27, w - 12, TH.text3);
        y += 54;
    }
    for (int i = 0; i < A.nCpl; i++) {
        CouplingSummary *s = &A.cplList[i];
        Rectangle rr = { x - 6, y, w + 12, 50 };
        bool sel = A.cpl && !strcmp(A.cpl->path, s->path);
        if (ui_row(ui_idp(s), rr, sel)) { app_rescan_cases(true); app_open_case(s->path, PG_OVERVIEW); }   /* a coupled run is a case */
        ui_text_clip(F_SEMI, 14, s->name, x + 6, y + 6, w - 12, TH.text);
        char sub[PL_PATH + 32], parent[PL_PATH]; pl_dirname(parent, sizeof parent, s->path);
        snprintf(sub, sizeof sub, "%d participants  ·  %s", s->nPart, pl_basename(parent));
        ui_text_clip(F_REG, 12, sub, x + 6, y + 27, w - 12, TH.text3);
        y += 54;
    }
    if (!A.nCpl && !A.cpl) y += ui_text_wrap(F_REG, 12.5f, "No folder with a precice-config.xml in the workspace yet.", (Rectangle){ x, y, w, 60 }, 18, TH.text3, true);
    ui_scroll_end(sc, y - (r.y - roundf(sc->y)) + 20);
}

static void empty_state(Rectangle r)
{
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX), y = r.y + 30;
    y += page_header(x, y, w, "Coupling", "Couple OpenFOAM with other solvers (or with another OpenFOAM case) through preCICE: "
                     "conjugate heat transfer between separate fluid and solid solvers, fluid-structure interaction, multi-scale.");
    char t[300];
    snprintf(t, sizeof t, "%s", A.env.preciceVersion[0] ? "preCICE is installed" : "preCICE was not found");
    char body[700];
    snprintf(body, sizeof body, A.env.preciceVersion[0] ?
             "preCICE %s (%s). Pick a coupled run on the left, or start one from a preCICE tutorial: they are laid out as one folder per "
             "participant next to precice-config.xml, and run as they are." :
             "Build preCICE and the OpenFOAM adapter first. Q-Continuum looks for ~/opt/precice-*/env.sh or precice-version on the PATH.%s%s",
             A.env.preciceVersion, A.env.preciceEnv[0] ? A.env.preciceEnv : "on the system paths");
    float ch = callout_height(w, t, body);
    callout((Rectangle){ x, y, w, ch }, A.env.preciceVersion[0] ? TOAST_OK : TOAST_WARN, A.env.preciceVersion[0] ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT, t, body);
    y += ch + 16;
    if (A.env.preciceVersion[0]) {
        float bw = ui_button_width(ICON_PLUS, "Start from a preCICE tutorial");
        if (ui_button(ui_id("cpl-empty-new"), (Rectangle){ x, y, bw, 38 }, ICON_PLUS, "Start from a preCICE tutorial", BTN_PRIMARY)) {
            A.modal = MD_CPL_NEW; A.mErr[0] = 0; A.tutSel = -1;
        }
    }
}

void page_coupling(Rectangle r)
{
    float lw = 300;
    Rectangle list = { r.x, r.y, lw, r.height };
    ui_rect(list, 0, TH.sidebar);
    DrawRectangleRec((Rectangle){ r.x + lw - 1, r.y, 1, r.height }, TH.lineSoft);
    coupling_list(list);
    Rectangle main = { r.x + lw, r.y, r.width - lw, r.height };
    Coupling *c = A.cpl;
    if (!c) { empty_state(main); return; }
    coupling_refresh(c);
    if (A.chartMax && A.cplTab == 3 && c->n) {               /* the participant's residuals take the whole page */
        float x = main.x + PAGE_PAD, w = main.width - 2 * PAGE_PAD, y = main.y + 22;
        y += participant_pills(c, x, y);
        int k; participant_log(c, &k);
        run_chart((Rectangle){ x, y, w, main.y + main.height - y - 24 }, k >= 0 ? &A.cplLogs.logs[k] : NULL);
        if (k < 0 || !A.cplLogs.logs[k].nSeries) chart_max_button(x + w - 10, y + 10);
        return;
    }
    if (A.logMax && A.cplTab == 3 && c->n) {                 /* the participant's log takes the whole page */
        float x = main.x + PAGE_PAD, w = main.width - 2 * PAGE_PAD, y = main.y + 22;
        y += participant_pills(c, x, y);
        int k, sel = participant_log(c, &k);
        run_console((Rectangle){ x, y, w, main.y + main.height - y - 24 }, &A.cplLogs, sel);
        return;
    }
    UiScroll *sc = &A.scr[15];
    float top = ui_scroll_begin(sc, main);
    float x = main.x + PAGE_PAD, w = main.width - 2 * PAGE_PAD, y = top + 30;
    char sub[PL_PATH + 64]; snprintf(sub, sizeof sub, "%s  ·  preCICE %s", c->path, A.env.preciceVersion[0] ? A.env.preciceVersion : "not found");
    float rbw = ui_button_width(ICON_SAVE, "Save");
    y += page_header(x, y, w - rbw - 20, c->name, sub);
    ui_disable(!c->cfg->dirty);
    if (ui_button(ui_id("cpl-save"), (Rectangle){ x + w - rbw, top + 32, rbw, 36 }, ICON_SAVE, "Save", BTN_PRIMARY)) {
        if (fd_save(c->cfg)) ui_toast(TOAST_OK, "Saved precice-config.xml"); else ui_toast(TOAST_ERR, "Could not write precice-config.xml");
    }
    ui_disable(false);
    static const char *tabs[] = { "Overview", "Configuration", "XML", "Run" };
    float th;
    ui_tabs(ui_id("cpl-tabs"), x, y, tabs, 4, &A.cplTab, &th);
    ui_divider(x, y + th, w);
    y += th + 24;
    if (!c->tree.ok && A.cplTab == 1) {
        char m[220]; pl_fmt_ui(m, sizeof m, "precice-config.xml is not valid XML (%s). Fix it on the XML tab.", c->tree.err);
        float ch = callout_height(w, NULL, m);
        callout((Rectangle){ x, y, w, ch }, TOAST_ERR, ICON_CIRCLE_X, NULL, m);
        y += ch + 14;
    }
    float bottom = main.y + main.height;
    switch (A.cplTab) {
    case 0: y += tab_overview(c, x, y, w); break;
    case 1: if (c->tree.ok) y += tab_config(c, x, y, w); break;
    case 2: text_editor(c->cfg, (Rectangle){ x, y, w, bottom - y - 34 }); y = bottom - 20; break;
    case 3: y += tab_run(c, x, y, w, fmaxf(bottom, y + 900)); break;
    default: break;
    }
    ui_scroll_end(sc, y - top + 30);
}

/* ============================================================================ a coupled run as a case (its own pages) */
/* the participant's coupling in its preciceDict: mesh, patches, what it reads and writes */
static void participant_interfaces(Coupling *c, CParticipant *p, char *out, int cap)
{
    out[0] = 0;
    char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
    char pd[PL_PATH];
    if (!d[0] || !pl_fmt(pd, sizeof pd, "%s/system/preciceDict", d)) return;
    FDFile *f = fd_load(pd);
    if (!f) { pl_fmt_ui(out, (size_t)cap, p->foam ? "No system/preciceDict: the adapter wouldn't know what to couple." : "Not an OpenFOAM case: "
                        "its own adapter reads precice-config.xml."); return; }
    FDEntry *ifs = fd_find(f, "interfaces");
    int k = 0;
    char mods[96] = ""; fd_get(f, "modules", mods, sizeof mods);
    k += snprintf(out + k, (size_t)(cap - k), "modules %s", mods[0] ? mods : "(none)");
    for (int i = 0; ifs && i < ifs->nChild && k < cap - 1; i++) {
        FDEntry *e = &ifs->child[i];
        if (e->kind != FD_DICT) continue;
        char mesh[96] = "", pat[160] = "", rd[160] = "", wr[160] = "", path[300];
        snprintf(path, sizeof path, "interfaces/%s/mesh", e->key); fd_get(f, path, mesh, sizeof mesh);
        snprintf(path, sizeof path, "interfaces/%s/patches", e->key); fd_get(f, path, pat, sizeof pat);
        snprintf(path, sizeof path, "interfaces/%s/readData", e->key); fd_get(f, path, rd, sizeof rd);
        snprintf(path, sizeof path, "interfaces/%s/writeData", e->key); fd_get(f, path, wr, sizeof wr);
        for (char *q = rd; *q; q++) if (*q == '\n') *q = ' ';
        for (char *q = wr; *q; q++) if (*q == '\n') *q = ' ';
        k += snprintf(out + k, (size_t)(cap - k), "\n%s: mesh %s on patches %s; reads %s; writes %s", e->key, mesh, pat, rd, wr);
    }
    fd_free(f);
}

static float cpl_header(Coupling *c, float x, float y, float w, const char *title)
{
    char sub[PL_PATH + 96]; snprintf(sub, sizeof sub, "%s  ·  %d participant%s  ·  preCICE %s", c->path, c->n, c->n == 1 ? "" : "s",
                                     A.env.preciceVersion[0] ? A.env.preciceVersion : "not found");
    float rbw = ui_button_width(ICON_SAVE, "Save");
    float h = page_header(x, y, w - rbw - 20, title, sub);
    ui_disable(!c->cfg->dirty);
    if (ui_button(ui_id("cpl-save"), (Rectangle){ x + w - rbw, y + 2, rbw, 36 }, ICON_SAVE, "Save", BTN_PRIMARY)) {
        if (fd_save(c->cfg)) ui_toast(TOAST_OK, "Saved precice-config.xml"); else ui_toast(TOAST_ERR, "Could not write precice-config.xml");
    }
    ui_disable(false);
    return h;
}

/* which: 0 Overview, 1 Coupling setup, 2 Participants, 3 Run */
void page_cpl_case(Rectangle r, int which)
{
    Case *cs = A.cs;
    if (!A.cpl || strcmp(A.cpl->path, cs->s.path)) cpl_open(cs->s.path);
    Coupling *c = A.cpl;
    if (!c) { ui_text(F_REG, 14, "This folder's precice-config.xml couldn't be read.", r.x + PAGE_PAD, r.y + 40, TH.err); return; }
    coupling_refresh(c);
    if (which == 3 && (A.chartMax || A.logMax) && c->n) {    /* the participant's residuals or log take the whole page */
        float x = r.x + PAGE_PAD, w = r.width - 2 * PAGE_PAD, y = r.y + 22;
        y += participant_pills(c, x, y);
        int k, sel = participant_log(c, &k);
        if (A.chartMax) run_chart((Rectangle){ x, y, w, r.y + r.height - y - 24 }, k >= 0 ? &A.cplLogs.logs[k] : NULL);
        else run_console((Rectangle){ x, y, w, r.y + r.height - y - 24 }, &A.cplLogs, sel);
        return;
    }
    UiScroll *sc = &A.scr[15];
    bool xml = which == 1 && A.cplTab == 2;
    float top = xml ? r.y : ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = r.width - 2 * PAGE_PAD, y = top + 30;
    static const char *titles[] = { NULL, "Coupling setup", "Participants", "Run" };
    y += cpl_header(c, x, y, w, which == 0 ? cs->s.name : titles[which]);
    float bottom = r.y + r.height;
    switch (which) {
    case 0: y += tab_overview(c, x, y, w); break;
    case 1: {
        static const char *tabs[] = { "Data, meshes and scheme", "XML" };
        int t = A.cplTab == 2 ? 1 : 0;
        float th;
        ui_tabs(ui_id("cpl-set-tabs"), x, y, tabs, 2, &t, &th);
        A.cplTab = t ? 2 : 1;
        ui_divider(x, y + th, w);
        y += th + 24;
        if (!c->tree.ok && t == 0) {
            char m[220]; pl_fmt_ui(m, sizeof m, "precice-config.xml is not valid XML (%s). Fix it on the XML tab.", c->tree.err);
            float ch = callout_height(w, NULL, m);
            callout((Rectangle){ x, y, w, ch }, TOAST_ERR, ICON_CIRCLE_X, NULL, m);
            y += ch + 14;
        }
        if (t == 1) { text_editor(c->cfg, (Rectangle){ x, y, w, bottom - y - 34 }); return; }
        if (c->tree.ok) y += tab_config(c, x, y, w);
        break;
    }
    case 2: {
        y += diagram(c, x, y, w) + 16;
        for (int i = 0; i < c->n; i++) {
            CParticipant *p = &c->p[i];
            char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
            char info[900]; participant_interfaces(c, p, info, sizeof info);
            float ih = ui_text_wrap(F_MONO, 12.5f, info, (Rectangle){ 0, 0, w - 260, 300 }, 18, BLANK, false);
            Rectangle card = { x, y, w, 70 + ih };
            ui_rect(card, 12, TH.surface); ui_rect_lines(card, 12, 1, TH.lineSoft);
            ui_text(F_SEMI, 17, p->name, x + 18, y + 14, TH.text);
            char sub[300]; pl_fmt_ui(sub, sizeof sub, "%s%s%s  ·  %d process%s", p->dirSel >= 0 ? p->dirs[p->dirSel] : "no folder",
                                     p->solver[0] ? "  ·  " : "", p->solver, p->procs, p->procs == 1 ? "" : "es");
            ui_text_clip(F_REG, 13, sub, x + 18 + ui_text_w(F_SEMI, 17, p->name) + 14, y + 17, w - 300, TH.text2);
            ui_text_wrap(F_MONO, 12.5f, info, (Rectangle){ x + 18, y + 46, w - 260, 300 }, 18, TH.text3, true);
            bool proj = d[0] && case_is_project(d);
            ui_disable(!proj);
            float bw = ui_button_width(ICON_FOLDER_OPEN, "Open as a case");
            if (ui_button(ui_idn(ui_id("cpl-part-open"), i), (Rectangle){ x + w - bw - 16, y + 14, bw, 34 }, ICON_FOLDER_OPEN, "Open as a case", BTN_SECONDARY)) {
                pl_strlcpy(A.cplBack, c->path, sizeof A.cplBack);   /* the participant's pages offer the way back */
                app_open_case(d, PG_OVERVIEW);
                ui_disable(false);
                return;
            }
            ui_disable(false);
            if (!proj) ui_tooltip((Rectangle){ x + w - bw - 16, y + 14, bw, 34 }, "Not a case or project Q-Continuum knows (its folder may be missing)");
            y += card.height + 12;
        }
        break;
    }
    case 3: y += tab_run(c, x, y, w, fmaxf(bottom, y + 900)); break;
    default: break;
    }
    ui_scroll_end(sc, y - top + 30);
}

/* ============================================================================ a coupled case from two cases */
static struct { bool init; CplBuild b; int sel[2]; char name[96]; char kText[2][24]; char err[400]; double inspectAt; } CB;

/* the workspace's OpenFOAM cases, for the pickers */
static int foam_cases(const char *items[], int max)
{
    int n = 0;
    for (int i = 0; i < A.nList && n < max; i++) if (!A.list[i].prog) items[n++] = A.list[i].path;
    return n;
}

static float part_form(int i, float x, float y, float w)
{
    CplPart *p = &CB.b.p[i];
    const char *items[256]; int n = foam_cases(items, 256);
    char labels[256][160]; const char *lab[256];
    for (int k = 0; k < n; k++) {
        char par[PL_PATH]; pl_dirname(par, sizeof par, items[k]);
        pl_fmt_ui(labels[k], sizeof labels[k], "%s   (%s)", pl_basename(items[k]), pl_basename(par)); lab[k] = labels[k];
    }
    ui_text(F_SEMI, 15, i == 0 ? "First: writes Temperature, reads the heat flux" : "Second: writes the heat flux, reads Temperature", x, y, TH.text);
    y += 28;
    ui_text(F_MED, 13, "Case", x, y + 9, TH.text2);
    char cur[200] = "";
    for (int k = 0; k < n; k++) if (!strcmp(items[k], p->src)) pl_strlcpy(cur, labels[k], sizeof cur);
    char pick[200] = "";
    if (ui_dropdown(ui_idn(ui_id("cb-case"), i), (Rectangle){ x + 110, y, w - 110, 34 }, cur, lab, n, DD_FILTER, pick, sizeof pick, "Pick an OpenFOAM case"))
        for (int k = 0; k < n; k++) if (!strcmp(labels[k], pick)) { pl_strlcpy(p->src, items[k], sizeof p->src); p->patch[0] = 0; cplb_inspect(p); }
    y += 42;
    ui_text(F_MED, 13, "Participant", x, y + 9, TH.text2);
    if (ui_input(ui_idn(ui_id("cb-name"), i), (Rectangle){ x + 110, y, 180, 34 }, p->name, sizeof p->name, i ? "Solid" : "Fluid", INPUT_LIVE)) {}
    y += 42;
    ui_text(F_MED, 13, "Interface", x, y + 9, TH.text2);
    const char *pi[128] = { 0 }; char pn[128][128]; int np = 0;
    char tmp[2048]; pl_strlcpy(tmp, p->patches, sizeof tmp);
    for (char *t = strtok(tmp, " "); t && np < 128; t = strtok(NULL, " ")) { pl_strlcpy(pn[np], t, sizeof pn[np]); pi[np] = pn[np]; np++; }
    char ps[128] = "";
    if (ui_dropdown(ui_idn(ui_id("cb-patch"), i), (Rectangle){ x + 110, y, w - 110, 34 }, p->patch, pi, np, DD_FILTER, ps, sizeof ps, "its interface patch"))
        pl_strlcpy(p->patch, ps, sizeof p->patch);
    y += 42;
    if (!strcmp(p->solverType, "basic")) {
        ui_text(F_MED, 13, "Conductivity k", x, y + 9, TH.text2);
        ui_input(ui_idn(ui_id("cb-k"), i), (Rectangle){ x + 110, y, 120, 34 }, CB.kText[i], sizeof CB.kText[i], "W/(m·K)", INPUT_LIVE | INPUT_NUMERIC | INPUT_MONO);
        ui_text(F_REG, 12.5f, "W/(m·K)", x + 240, y + 10, TH.text3);
        p->k = atof(CB.kText[i]);
        y += 42;
    }
    return y;
}

void cpl_build_modal(bool *close)
{
    if (!CB.init) {
        memset(&CB, 0, sizeof CB); CB.init = true;
        snprintf(CB.b.p[0].name, sizeof CB.b.p[0].name, "Fluid"); snprintf(CB.b.p[1].name, sizeof CB.b.p[1].name, "Solid");
        snprintf(CB.name, sizeof CB.name, "cht-coupled");
        app_rescan_cases(true);
    }
    Rectangle b = ui_modal_begin("Couple two cases", "Conjugate heat transfer through preCICE between two OpenFOAM cases: each is copied into the "
                                 "coupled case (the originals stay as they are) with its preciceDict, the adapter and the interface's temperature "
                                 "condition; precice-config.xml couples them across one patch each.", 1000, 720, close);
    float x = b.x, w = 440, y = b.y + 4;
    y = part_form(0, x, y, w) + 12;
    part_form(1, x, y, w);
    CB.b.window = CB.b.maxTime = 0;
    CplCheck ck[24]; int n = cplb_check(&CB.b, ck, 24), bad = 0;
    float rx = x + w + 30, rw = b.width - w - 30, ry = b.y + 4;
    ui_text(F_SEMI, 15, "What it checks", rx, ry, TH.text); ry += 30;
    for (int i = 0; i < n; i++) {
        Color c = ck[i].sev == 2 ? TH.err : ck[i].sev ? TH.warn : TH.ok;
        if (ck[i].sev == 2) bad++;
        ui_icon(ck[i].sev == 2 ? ICON_CIRCLE_X : ck[i].sev ? ICON_TRIANGLE_ALERT : ICON_CIRCLE_CHECK, rx + 8, ry + 9, 14, c);
        ry += ui_text_wrap(F_REG, 13, ck[i].text, (Rectangle){ rx + 24, ry, rw - 24, 200 }, 19, ck[i].sev ? c : TH.text2, true) + 10;
    }
    float by = b.y + b.height - 38;
    ui_text(F_MED, 13, "Coupled case", x, by + 9, TH.text2);
    ui_input(ui_id("cb-out"), (Rectangle){ x + 110, by, 260, 38 }, CB.name, sizeof CB.name, "cht-coupled", INPUT_LIVE);
    if (CB.err[0]) ui_text_clip(F_REG, 12.5f, CB.err, x + 380, by + 11, b.width - 380 - 200, TH.err);
    ui_disable(bad > 0);
    float gw = ui_button_width(ICON_PLUS, "Make the coupled case");
    if (ui_button(ui_id("cb-go"), (Rectangle){ b.x + b.width - gw, by, gw, 38 }, ICON_PLUS, "Make the coupled case", BTN_PRIMARY)) {
        char ws[PL_PATH], dst[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
        pl_trim_inplace(CB.name);
        CB.err[0] = 0;
        if (!CB.name[0] || strchr(CB.name, '/') || foam_path_problem(CB.name)) snprintf(CB.err, sizeof CB.err, "Use a plain folder name.");
        else if (!pl_fmt(dst, sizeof dst, "%s/%s", ws, CB.name)) snprintf(CB.err, sizeof CB.err, "Path too long.");
        else if (cplb_write(&CB.b, dst, CB.err, sizeof CB.err)) {
            ui_toast(TOAST_OK, "Made %s: %s and %s coupled through preCICE", CB.name, CB.b.p[0].name, CB.b.p[1].name);
            CB.init = false;
            *close = true;
            app_rescan_cases(true);
            app_open_case(dst, PG_OVERVIEW);
        }
    }
    ui_disable(false);
}

/* ============================================================================ modals (called from draw_modals) */
/* preCICE tutorials with an OpenFOAM participant: copied with tools/ so their run.sh scripts work unchanged */
static int tut_list(char names[][96], int max)
{
    int n = 0;
    if (!A.env.preciceTutorials[0]) return 0;
    NameList l = { 0 };
    pl_list_dir(A.env.preciceTutorials, &l, true, false);
    names_sort(&l);
    for (int i = 0; i < l.n && n < max; i++) {
        char p[PL_PATH], q[PL_PATH]; pl_join(p, sizeof p, A.env.preciceTutorials, l.names[i]);
        pl_join(q, sizeof q, p, "precice-config.xml");
        if (!pl_is_file(q)) continue;
        NameList s = { 0 }; pl_list_dir(p, &s, true, false);
        bool foam = false;
        for (int k = 0; k < s.n; k++) { char d[PL_PATH], e[PL_PATH]; pl_join(d, sizeof d, p, s.names[k]); pl_join(e, sizeof e, d, "system/preciceDict"); if (pl_is_file(e)) foam = true; }
        names_free(&s);
        if (foam) pl_strlcpy(names[n++], l.names[i], 96);
    }
    names_free(&l);
    return n;
}

void coupling_modal(bool *close)
{
    Rectangle b;
    if (A.modal == MD_CPL_NEW) {
        static char tn[128][96]; static int nt = -1;
        if (nt < 0) nt = tut_list(tn, 128);
        b = ui_modal_begin("New coupled run from a preCICE tutorial",
                           "The tutorial is copied into <workspace>/coupled/ together with its tools/ folder, so each participant's run.sh works as "
                           "published. Only tutorials with an OpenFOAM participant are listed.", 720, 560, close);
        if (!A.env.preciceTutorials[0]) {
            ui_text_wrap(F_REG, 13, "No preCICE tutorials checkout found (looked in ~/src/precice-tutorials). Clone it with:\n"
                         "git clone --depth 1 https://github.com/precice/tutorials.git ~/src/precice-tutorials", (Rectangle){ b.x, b.y, b.width, 80 }, 19, TH.text2, true);
            return;
        }
        UiScroll *sc = &A.scr[16];
        Rectangle lr = { b.x, b.y, b.width, b.height - 54 };
        float y = ui_scroll_begin(sc, lr);
        for (int i = 0; i < nt; i++) {
            if (ui_row(ui_idn(ui_id("cpl-tut"), i), (Rectangle){ lr.x, y, lr.width - 12, 34 }, A.tutSel == i)) A.tutSel = i;
            ui_text(F_MONO, 13, tn[i], lr.x + 12, y + 8, TH.text);
            y += 36;
        }
        ui_scroll_end(sc, nt * 36 + 8);
        if (A.mErr[0]) ui_text(F_REG, 12.5f, A.mErr, b.x, b.y + b.height - 30, TH.err);
        ui_disable(A.tutSel < 0 || A.tutSel >= nt);
        if (ui_button(ui_id("cpl-tut-go"), (Rectangle){ b.x + b.width - 140, b.y + b.height - 38, 140, 38 }, ICON_COPY, "Copy", BTN_PRIMARY)) {
            char ws[PL_PATH], dst[PL_PATH], src[PL_PATH], tools[PL_PATH], q1[PL_PATH + 8], q2[PL_PATH + 8], q3[PL_PATH + 8], q4[PL_PATH + 8], cmd[PL_PATH * 5];
            pl_expand_home(ws, sizeof ws, A.set.workspace);
            char cdir[PL_PATH]; pl_join(cdir, sizeof cdir, ws, "coupled");
            pl_join(dst, sizeof dst, cdir, tn[A.tutSel]);
            pl_join(src, sizeof src, A.env.preciceTutorials, tn[A.tutSel]);
            pl_join(tools, sizeof tools, A.env.preciceTutorials, "tools");
            if (pl_exists(dst)) snprintf(A.mErr, sizeof A.mErr, "%s already exists in the workspace.", tn[A.tutSel]);
            else {
                pl_mkdirs(cdir);
                pl_shell_quote(q1, sizeof q1, src); pl_shell_quote(q2, sizeof q2, cdir);
                pl_shell_quote(q3, sizeof q3, tools); pl_shell_quote(q4, sizeof q4, cdir);
                char out[256] = "";
                if (pl_fmt(cmd, sizeof cmd, "cp -r %s %s/ && { [ -d %s/tools ] || cp -r %s %s/; } && echo ok", q1, q2, q2, q3, q4)) pl_run_wait(cmd, out, sizeof out);
                else pl_strlcpy(out, "the paths are too long", sizeof out);
                if (!strstr(out, "ok")) pl_fmt_ui(A.mErr, sizeof A.mErr, "Copy failed: %s", out);
                else { A.modal = MD_NONE; cpl_scan(); cpl_open(dst); A.page = PG_COUPLING; ui_toast(TOAST_OK, "Copied %s", tn[A.tutSel]); }
            }
        }
        ui_disable(false);
    } else if (A.modal == MD_CPL_RESULTS) {
        char sub[900];
        snprintf(sub, sizeof sub, "%s. A new coupled run starts every participant from its start time and overwrites those time "
                 "directories; a participant run in parallel is decomposed again, which deletes its processor* folders.", cplResults);
        b = ui_modal_begin("This coupled run already has results", sub, 620, 230, close);
        float by = b.y + b.height - 38;
        if (ui_button(ui_id("m-cr-cancel"), (Rectangle){ b.x, by, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
        float w2 = ui_button_width(ICON_TRASH_2, "Run and overwrite");
        if (ui_button(ui_id("m-cr-over"), (Rectangle){ b.x + b.width - w2, by, w2, 38 }, ICON_TRASH_2, "Run and overwrite", BTN_DANGER)) {
            cplOverwriteOk = true; A.modal = MD_NONE;
        }
    } else if (A.modal == MD_CPL_OPEN) {
        b = ui_modal_begin("Open a coupled run", "A folder that holds precice-config.xml, with one folder per participant next to it.", 600, 230, close);
        ui_input(ui_id("cpl-open-path"), (Rectangle){ b.x, b.y + 4, b.width, 38 }, A.mText, sizeof A.mText, "~/OpenFOAM/.../myCoupledRun", INPUT_MONO);
        if (A.mErr[0]) ui_text(F_REG, 12.5f, A.mErr, b.x, b.y + 50, TH.err);
        if (ui_button(ui_id("cpl-open-go"), (Rectangle){ b.x + b.width - 120, b.y + b.height - 38, 120, 38 }, ICON_FOLDER_OPEN, "Open", BTN_PRIMARY)) {
            char p[PL_PATH], q[PL_PATH]; pl_expand_home(p, sizeof p, A.mText); pl_trim_inplace(p);
            pl_join(q, sizeof q, p, "precice-config.xml");
            if (!pl_is_file(q)) pl_fmt_ui(A.mErr, sizeof A.mErr, "%s has no precice-config.xml.", p);
            else { A.modal = MD_NONE; cpl_open(p); A.page = PG_COUPLING; }
        }
    }
}
