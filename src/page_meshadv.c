/* page_meshadv.c -- the STL mesher's "Every OpenFOAM setting" panel, under Advanced (user request, 2026-11-13: "when I
 * check that I want advanced meshing, give me access to literally every thing I could change in a mesh that OpenFOAM
 * will allow").
 *
 * Every keyword of the dictionaries the mesher in use writes (meshopts.c MESH_OPTS), grouped as the dictionaries are,
 * each with the value Q-Continuum writes (mesher_preview: the dictionaries made in memory), OpenFOAM's own default when
 * Q-Continuum writes none, and the user's value when they set one. Their values go to <case>/mesher/advanced.ini and
 * Generate applies them (stlmesh.c put_dict). Below: any other keyword by its path, and the programs' options. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "app.h"
#include "foamdict.h"
#include "meshopts.h"
#include "stlmesh.h"
#include "page_meshadv.h"

#define MA_GROUPS 40
static unsigned hash_str(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
static struct {
    char        casePath[PL_PATH];
    MeshOverrides ov;
    FDSig       sig;                 /* advanced.ini as last read: edited on disk, read again */
    bool        sigOk;
    MeshPreview pv;
    FDFile     *pf[MESH_PREVIEW_MAX];
    Str         key;                 /* what the preview was made from */
    char        search[96];
    bool        changedOnly;
    char        open[MA_GROUPS][80]; /* open groups by name */
    int         nOpen;
    int         patchSel, regionSel;
    int         addFile;             /* -1: the mesher's main dictionary */
    char        addPath[256], addVal[512];
    float       clearArm;
} MA;

static bool group_open(const char *g)
{
    for (int i = 0; i < MA.nOpen; i++) if (!strcmp(MA.open[i], g)) return true;
    return false;
}
static void group_toggle(const char *g)
{
    for (int i = 0; i < MA.nOpen; i++)
        if (!strcmp(MA.open[i], g)) { memmove(MA.open[i], MA.open[i + 1], sizeof MA.open[0] * (size_t)(MA.nOpen - i - 1)); MA.nOpen--; return; }
    if (MA.nOpen < MA_GROUPS) pl_strlcpy(MA.open[MA.nOpen++], g, sizeof MA.open[0]);
}

static void drop_preview(void)
{
    for (int i = 0; i < MA.pv.n; i++) { if (MA.pf[i]) fd_free(MA.pf[i]); MA.pf[i] = NULL; }
    mesher_preview_free(&MA.pv);
}

/* the case's values, read again when the case changes or the file does */
static void sync(const char *casePath)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, MO_INI);
    FDSig now; bool ok = fd_sig(p, &now);
    bool changed = strcmp(casePath, MA.casePath) || ok != MA.sigOk || (ok && memcmp(&now, &MA.sig, sizeof now));
    if (!changed) return;
    if (strcmp(casePath, MA.casePath)) {
        pl_strlcpy(MA.casePath, casePath, sizeof MA.casePath);
        MA.patchSel = MA.regionSel = 0; MA.addFile = -1; MA.addPath[0] = MA.addVal[0] = 0;
        drop_preview();
        str_free(&MA.key);
    }
    if (!MA.ov.s) mo_init(&MA.ov);
    mo_load(casePath, &MA.ov);
    MA.sig = now; MA.sigOk = ok;
}

static void save(void)
{
    mo_save(MA.casePath, &MA.ov);
    char p[PL_PATH]; pl_join(p, sizeof p, MA.casePath, MO_INI);
    MA.sigOk = fd_sig(p, &MA.sig);
}

/* the dictionaries as Q-Continuum writes them for these settings; made again when the settings change */
static void sync_preview(const MeshCfg *m, Stl *s)
{
    if (!MA.key.s) str_init(&MA.key);
    Str k; str_init(&k);
    mesher_signature(m, &k);
    str_catf(&k, "%p %d %d\n", (void *)s->tri, s->nTri, s->nRegions);
    for (int r = 0; r < s->nRegions; r++) str_catf(&k, "%s %d\n", s->reg[r].name, s->reg[r].type);
    if (MA.pv.n && MA.key.s && !strcmp(k.s, MA.key.s)) { str_free(&k); return; }
    drop_preview();
    mesher_preview(m, s, &MA.pv);
    for (int i = 0; i < MA.pv.n; i++) {
        char rel[96]; snprintf(rel, sizeof rel, "%s", MA.pv.rel[i]);
        MA.pf[i] = fd_from_text(rel, MA.pv.text[i]);
    }
    str_free(&MA.key);
    MA.key = k;                                           /* (keeps k's buffer) */
}

static FDFile *pfile(const char *rel)
{
    for (int i = 0; i < MA.pv.n; i++) if (!strcmp(MA.pv.rel[i], rel)) return MA.pf[i];
    return NULL;
}

/* a value on one line: a list OpenFOAM writes over several lines ("blocks ( hex (...) ... );") with each run of white space,
   newlines and indents too, made one space, and // comments dropped. OpenFOAM reads it the same; the field is one line. */
static void flatten(const char *in, char *out, int cap)
{
    int n = 0; bool sp = false;
    for (const char *c = in; *c && n < cap - 1; c++) {
        if (c[0] == '/' && c[1] == '/') { while (*c && *c != '\n') c++; if (!*c) break; }
        if (isspace((unsigned char)*c)) { sp = n > 0; continue; }
        if (sp) {
            bool tight = out[n - 1] == '(' || *c == ')';     /* "(0 1 2)", not "( 0 1 2 )" */
            if (!tight && n < cap - 1) out[n++] = ' ';
            sp = false;
        }
        if (n < cap - 1) out[n++] = *c;
    }
    out[n] = 0;
}

/* what Q-Continuum writes there (false: nothing) */
static bool generated(const MeshCfg *m, const char *file, const char *path, char *out, int cap)
{
    if (!strncmp(file, "cmd:", 4)) {
        if (!strcmp(path, "featureAngle") || !strcmp(path, "-angle")) { snprintf(out, (size_t)cap, "%g", m->featureAngle); return true; }
        return false;
    }
    FDFile *f = pfile(file);
    if (!f) return false;
    FDEntry *e = fd_find(f, path);
    if (!e) return false;
    if (e->kind == FD_DICT) { snprintf(out, (size_t)cap, "{ ... }"); return true; }
    char raw[4096];
    if (!fd_get(f, path, raw, sizeof raw)) return false;
    flatten(raw, out, cap);
    return true;
}

/* the names a %s path is made for: the boundaries, or the refinement regions the preview has */
static int names_for(const MeshOpt *op, const MeshCfg *m, const Stl *s, char names[][64], int max)
{
    int n = 0;
    if (op->flags & MO_REGION) {
        char dir[256]; pl_strlcpy(dir, op->path, sizeof dir);
        char *ps = strstr(dir, "/%s"); if (ps) *ps = 0;
        FDFile *f = pfile(op->file);
        FDEntry *d = f ? fd_find(f, dir) : NULL;
        for (int i = 0; d && i < d->nChild && n < max; i++)
            if (d->child[i].kind == FD_DICT) pl_strlcpy(names[n++], d->child[i].key, 64);
        return n;
    }
    for (int r = 0; r < s->nRegions && n < max; r++)
        if (mesher_region_meshed(m, s, r)) pl_strlcpy(names[n++], s->reg[r].name, 64);
    if (m->domain == DOMAIN_EXTERNAL && !strstr(op->path, "refinementSurfaces")) {
        static const char *const TUN[4] = { "inlet", "outlet", "farfield", "ground" };
        for (int k = 0; k < 4 && n < max; k++) if (k < 3 || m->ground) pl_strlcpy(names[n++], TUN[k], 64);
    }
    return n;
}

/* a path with its %s or @ filled in ("" when there is nothing to fill it with) */
static void resolve(const MeshOpt *op, const char *name, char *out, int cap)
{
    const char *at = strchr(op->path, '@');
    if (op->flags & MO_PART && at) {
        char dir[256]; snprintf(dir, sizeof dir, "%.*s", (int)(at - op->path - 1), op->path);
        FDFile *f = pfile(op->file);
        FDEntry *d = f ? fd_find(f, dir) : NULL;
        const char *first = NULL;
        for (int i = 0; d && i < d->nChild && !first; i++) if (d->child[i].kind == FD_DICT) first = d->child[i].key;
        if (!first) { out[0] = 0; return; }
        snprintf(out, (size_t)cap, "%s/%s%s", dir, first, at + 1);
        return;
    }
    if (op->flags & (MO_PATCH | MO_REGION)) {
        const char *ps = strstr(op->path, "%s");
        snprintf(out, (size_t)cap, "%.*s%s%s", (int)(ps - op->path), op->path, name ? name : "", ps + 2);
        return;
    }
    pl_strlcpy(out, op->path, (size_t)cap);
}

static bool matches(const MeshOpt *op)
{
    if (!MA.search[0]) return true;
    return strcasestr(op->label, MA.search) || strcasestr(op->group, MA.search) || strcasestr(op->path, MA.search) || (op->help && strcasestr(op->help, MA.search)) ||
           strcasestr(op->file, MA.search);
}

/* the words of a choice list */
static int split_choices(const char *c, char out[][48], int max)
{
    int n = 0;
    while (c && *c && n < max) {
        const char *bar = strchr(c, '|');
        size_t l = bar ? (size_t)(bar - c) : strlen(c);
        if (l > 47) l = 47;
        memcpy(out[n], c, l); out[n][l] = 0; n++;
        c += l + (bar ? 1 : 0);
        if (!bar) break;
    }
    return n;
}

/* the user's value set (NULL: theirs taken off). A value equal to what Q-Continuum writes is no value of theirs. */
static void set_value(const char *file, const char *path, const char *v, const char *gen)
{
    if (v && gen && !strcmp(v, gen)) v = NULL;
    mo_set(&MA.ov, file, path, v);
    save();
}

/* one keyword: label, help and keyword on the left; its control, and where its value comes from, on the right */
static float option_row(const MeshCfg *m, const MeshOpt *op, const char *path, float x, float y, float w, int idx)
{
    const char *mine = mo_get(&MA.ov, op->file, path);
    char gen[512] = "";
    bool hasGen = generated(m, op->file, path, gen, sizeof gen);
    if (MA.changedOnly && !mine) return 0;
    float y0 = y;
    char hint[700];
    snprintf(hint, sizeof hint, "%s%s%s", op->help ? op->help : "", op->help ? "\n" : "", path);
    /* a row off screen: its height only */
    float lw = fminf(300, w * 0.38f);
    float estH = fmaxf(52, 20 + ui_text_wrap(F_REG, 12.5f, hint, (Rectangle){ 0, 0, lw - 24, 400 }, 17, TH.text3, false)) + 22;
    if (ui_culled(y, y + estH)) return estH;
    ui_push_id(ui_idn(ui_id("ma-opt"), idx));
    Rectangle cr = prop_row(x, &y, w, op->label, hint, 52);
    if (mine) ui_rect((Rectangle){ x - 10, y0 + 10, 3, y - y0 - 20 }, 1.5f, TH.accent);
    const char *cur = mine ? mine : hasGen ? gen : op->dflt && op->dflt[0] != '=' && strcmp(op->dflt, "unlimited") ? op->dflt : "";
    float cw = fminf(320, cr.width - 44);
    Rectangle ctl = { cr.x, cr.y, cw, 34 };
    if (mine && !strcmp(mine, MO_REMOVE)) {
        ui_text(F_MED, 13, "Left out of the dictionary", ctl.x, ctl.y + 9, TH.warn);
    } else if (op->type == MO_BOOL && op->choices) {
        char ch[2][48]; split_choices(op->choices, ch, 2);
        bool on = !strcmp(cur, ch[0]), was = on;
        if (!cur[0]) on = false;
        ui_switch(ui_id("sw"), ctl.x, ctl.y + 5, &on);
        ui_text(F_MONO, 12.5f, cur[0] ? cur : "(not set)", ctl.x + 56, ctl.y + 9, TH.text2);
        if (on != was || (!cur[0] && on)) set_value(op->file, path, on ? ch[0] : ch[1], hasGen ? gen : op->dflt);
    } else if (op->type == MO_WORD && op->choices) {
        char ch[12][48]; int n = split_choices(op->choices, ch, 12);
        const char *items[12]; int sel = -1;
        for (int i = 0; i < n; i++) { items[i] = ch[i]; if (!strcmp(ch[i], cur)) sel = i; }
        int was = sel;
        if (ui_select_ph(ui_id("sel"), ctl, items, n, &sel, cur[0] ? cur : "(not set)") && sel != was && sel >= 0)
            set_value(op->file, path, ch[sel], hasGen ? gen : op->dflt);
    } else {
        char b[512]; pl_strlcpy(b, cur, sizeof b);
        const char *ph = op->type == MO_VEC ? "(x y z)" : op->type == MO_TEXT ? "(not set)" : "(not set)";
        if (ui_input(ui_id("in"), ctl, b, sizeof b, ph, INPUT_MONO | (op->type == MO_INT || op->type == MO_REAL ? INPUT_NUMERIC : 0))) {
            char *v = b; while (isspace((unsigned char)*v)) v++;
            char *e = v + strlen(v); while (e > v && isspace((unsigned char)e[-1])) *--e = 0;
            char err[160];
            if (!*v) set_value(op->file, path, NULL, NULL);
            else if (!mo_check(op, v, err, sizeof err)) ui_toast(TOAST_ERR, "%s: %s", op->label, err);
            else set_value(op->file, path, v, hasGen ? gen : NULL);
        }
    }
    if (cur[0] && ui_text_w(F_MONO, 13, cur) > cw - 30 && !ui_input_focused(ui_id("in"))) ui_tooltip(ctl, cur);   /* cut off: all of it on hover */
    if (mine && ui_button(ui_id("undo"), (Rectangle){ ctl.x + cw + 8, ctl.y + 1, 32, 32 }, ICON_UNDO_2, NULL, BTN_ICON)) set_value(op->file, path, NULL, NULL);
    if (mine) ui_tooltip((Rectangle){ ctl.x + cw + 8, ctl.y + 1, 32, 32 }, "Back to Q-Continuum's value");
    char src[600];
    if (mine && !strcmp(mine, MO_REMOVE)) snprintf(src, sizeof src, "Yours: removed%s%s", hasGen ? "  ·  Q-Continuum writes " : "", hasGen ? gen : "");
    else if (mine) snprintf(src, sizeof src, "Yours  ·  %s%s", hasGen ? "Q-Continuum writes " : op->dflt ? "OpenFOAM's default " : "Q-Continuum writes nothing",
                            hasGen ? gen : op->dflt ? op->dflt : "");
    else if (hasGen) snprintf(src, sizeof src, "Q-Continuum writes this");
    else if (op->dflt) snprintf(src, sizeof src, "Not written: OpenFOAM uses %s", op->dflt);
    else snprintf(src, sizeof src, "Not written");
    ui_text_clip(F_REG, 12, src, ctl.x, ctl.y + 38, cr.width, mine ? TH.accent : TH.text3);
    ui_pop_id();
    return y - y0;
}

static float group_head(const char *g, int nShown, int nMine, float x, float y, float w, bool forceOpen)
{
    Rectangle r = { x, y, w, 36 };
    bool open = forceOpen || group_open(g);
    if (!ui_culled(y, y + 36)) {
        if (ui_row(ui_idn(ui_id("ma-g"), (int)(hash_str(g) & 0x7fffffff)), r, false) && !forceOpen) group_toggle(g);
        ui_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + 14, y + 18, 15, TH.text2);
        ui_text_clip(F_SEMI, 14, g, x + 30, y + 9, w - 200, TH.text);
        char c[64];
        if (nMine) snprintf(c, sizeof c, "%d  ·  %d yours", nShown, nMine); else snprintf(c, sizeof c, "%d", nShown);
        float cw = ui_text_w(F_MONO, 12, c);
        ui_text(F_MONO, 12, c, x + w - cw - 12, y + 11, nMine ? TH.accent : TH.text3);
    }
    return 40;
}

/* the values of a group the user set (any boundary or region) */
static int group_mine(const char *g, unsigned mask)
{
    int n = 0;
    for (int i = 0; i < MA.ov.n; i++)
        for (int k = 0; k < MESH_OPTS_N; k++) {
            const MeshOpt *op = &MESH_OPTS[k];
            if (strcmp(op->group, g) || !(op->when & mask) || strcmp(op->file, MA.ov.s[i].file)) continue;
            /* same keyword? compare with %s / @ as any name */
            const char *a = op->path, *b = MA.ov.s[i].path; bool ok = true;
            while (*a && ok) {
                if ((a[0] == '%' && a[1] == 's') || a[0] == '@') { while (*b && *b != '/') b++; a += a[0] == '%' ? 2 : 1; continue; }
                if (*a++ != *b++) ok = false;
            }
            if (ok && !*b) { n++; break; }
        }
    return n;
}

static bool in_table(const MOSet *st)
{
    for (int k = 0; k < MESH_OPTS_N; k++) {
        const MeshOpt *op = &MESH_OPTS[k];
        if (strcmp(op->file, st->file)) continue;
        const char *a = op->path, *b = st->path; bool ok = true;
        while (*a && ok) {
            if ((a[0] == '%' && a[1] == 's') || a[0] == '@') { while (*b && *b != '/') b++; a += a[0] == '%' ? 2 : 1; continue; }
            if (*a++ != *b++) ok = false;
        }
        if (ok && !*b) return true;
    }
    return false;
}

int meshadv_count(const char *casePath, const MeshCfg *m)
{
    sync(casePath);
    return mo_count(&MA.ov, mo_active_mask(m->mesher, m->twoD, mesher_poly_convert(m)));
}

/* any keyword by its path, for what the table hasn't: limitRegions, faceZoneControls, a motion solver's Coeffs ... */
static float free_section(const MeshCfg *m, float x, float y, float w, unsigned mask)
{
    float y0 = y;
    bool forced = MA.search[0] != 0;
    int nFree = 0;
    for (int i = 0; i < MA.ov.n; i++) if (!in_table(&MA.ov.s[i])) nFree++;
    const char *g = "Any other keyword";
    y += group_head(g, nFree, nFree, x, y, w, forced);
    if (!(forced || group_open(g))) return y - y0;
    y += ui_text_wrap(F_REG, 13, "Any keyword of any dictionary the mesher writes, by its path ('/' between dictionaries): castellatedMeshControls/"
                      "limitRegions/zone0 = { mode inside; levels ((1e15 2)); }. A whole sub-dictionary is given as { ... }; !remove leaves an entry "
                      "out. Generate applies them after its own values, so yours win.", (Rectangle){ x, y, w, 200 }, 19, TH.text3, true) + 10;
    int del = -1;
    for (int i = 0; i < MA.ov.n; i++) {
        MOSet *st = &MA.ov.s[i];
        if (in_table(st)) continue;
        ui_push_id(ui_idn(ui_id("ma-free"), i));
        char lab[400]; snprintf(lab, sizeof lab, "%s  %s", st->file, st->path);
        ui_text_clip(F_MONO, 12.5f, lab, x, y + 9, w * 0.45f, TH.text);
        float vx = x + w * 0.46f, vw = w * 0.54f - 44;
        char b[512]; pl_strlcpy(b, st->value, sizeof b);
        if (ui_input(ui_id("v"), (Rectangle){ vx, y, vw, 34 }, b, sizeof b, NULL, INPUT_MONO)) {
            char err[160];
            if (!b[0]) del = i;
            else if (!strncmp(st->file, "cmd:", 4) ? !mo_safe_args(b) : !mo_check_text(b, err, sizeof err))
                ui_toast(TOAST_ERR, "%s", !strncmp(st->file, "cmd:", 4) ? "No ; & | < > ` $ or quotes in command-line options" : err);
            else { pl_strlcpy(st->value, b, sizeof st->value); save(); }
        }
        if (ui_button(ui_id("del"), (Rectangle){ x + w - 36, y + 1, 32, 32 }, ICON_TRASH_2, NULL, BTN_ICON)) del = i;
        ui_pop_id();
        y += 42;
    }
    if (del >= 0) { MOSet d = MA.ov.s[del]; mo_set(&MA.ov, d.file, d.path, NULL); save(); }
    /* add one: the dictionaries this mesher writes */
    const char *files[MESH_PREVIEW_MAX + 1]; int nf = 0;
    for (int i = 0; i < MA.pv.n; i++) files[nf++] = MA.pv.rel[i];
    (void)mask;
    if (nf == 0) return y - y0 + 6;
    if (MA.addFile < 0 || MA.addFile >= nf) {             /* the mesher's own dictionary to start with */
        MA.addFile = 0;
        for (int i = 0; i < nf; i++)
            if (strstr(files[i], "snappyHexMeshDict") || !strcmp(files[i], "system/meshDict") || strstr(files[i], "foamyHexMeshDict") ||
                strstr(files[i], "foamyQuadMeshDict")) { MA.addFile = i; break; }
    }
    float fw = fminf(260, w * 0.3f), pw = fminf(340, w * 0.34f);
    ui_select(ui_id("ma-add-file"), (Rectangle){ x, y, fw, 34 }, files, nf, &MA.addFile);
    ui_input(ui_id("ma-add-path"), (Rectangle){ x + fw + 8, y, pw, 34 }, MA.addPath, sizeof MA.addPath, "path/to/keyword", INPUT_MONO | INPUT_LIVE);
    float bw = ui_button_width(ICON_PLUS, "Add");
    float vx = x + fw + pw + 16, vw = w - (vx - x) - bw - 8;
    ui_input(ui_id("ma-add-val"), (Rectangle){ vx, y, vw, 34 }, MA.addVal, sizeof MA.addVal, "value", INPUT_MONO | INPUT_LIVE);
    if (ui_button(ui_id("ma-add"), (Rectangle){ x + w - bw, y, bw, 34 }, ICON_PLUS, "Add", BTN_SECONDARY)) {
        char err[160];
        char *p = MA.addPath; while (*p == '/' || *p == ' ') p++;
        char *e = p + strlen(p); while (e > p && (e[-1] == ' ' || e[-1] == '/')) *--e = 0;
        if (!*p || strchr(p, ' ')) ui_toast(TOAST_ERR, "Give the keyword's path: dictionaries and keyword with '/' between, no spaces");
        else if (!MA.addVal[0]) ui_toast(TOAST_ERR, "Give its value (or !remove to leave it out)");
        else if (!mo_check_text(MA.addVal, err, sizeof err)) ui_toast(TOAST_ERR, "%s", err);
        else {
            mo_set(&MA.ov, files[MA.addFile], p, MA.addVal); save();
            ui_toast(TOAST_OK, "%s %s = %s: applied on the next Generate", files[MA.addFile], p, MA.addVal);
            MA.addPath[0] = MA.addVal[0] = 0;
        }
    }
    y += 46;
    (void)m;
    return y - y0;
}

float meshadv_panel(const char *casePath, const MeshCfg *m, Stl *s, float x, float y, float w)
{
    float y0 = y;
    sync(casePath);
    sync_preview(m, s);
    unsigned mask = mo_active_mask(m->mesher, m->twoD, mesher_poly_convert(m));
    int mine = mo_count(&MA.ov, mask);
    char right[64] = ""; if (mine) snprintf(right, sizeof right, "%d of yours in use", mine);
    y += section_head_r(x, y, w, "Every OpenFOAM setting",
                        "Every keyword the mesher's dictionaries take, with the value Q-Continuum writes or OpenFOAM's default. Change one "
                        "and it is yours: Generate writes it over Q-Continuum's value (mesher/advanced.ini), the rest stays as it was. The "
                        "arrow puts Q-Continuum's value back.", right);
    if (MESHERS[m->mesher].engine == ENG_SALOME) {
        const char *d = "SALOME makes this mesh, so there are no OpenFOAM meshing dictionaries: only checkMesh's options apply "
                        "(and polyDualMesh's, when converting). SALOME's own settings are under Cell size and Prism layers.";
        float ch = callout_height(w, NULL, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, d);
        y += ch + 12;
    }
    /* search, what to show, clear */
    float sw = fminf(360, w * 0.45f);
    ui_input(ui_id("ma-search"), (Rectangle){ x, y, sw, 34 }, MA.search, sizeof MA.search, "Find a setting (words or keyword)", INPUT_SEARCH | INPUT_LIVE);
    ui_switch(ui_id("ma-changed"), x + sw + 18, y + 5, &MA.changedOnly);
    ui_text(F_MED, 13, "Only mine", x + sw + 64, y + 9, TH.text2);
    if (MA.ov.n) {
        bool armed = GetTime() - MA.clearArm < 4;
        const char *cl = armed ? "Click again to clear" : "Clear all mine";
        float bw = ui_button_width(ICON_ROTATE_CCW, cl);
        if (ui_button(ui_id("ma-clear"), (Rectangle){ x + w - bw, y, bw, 34 }, ICON_ROTATE_CCW, cl, armed ? BTN_DANGER : BTN_SECONDARY)) {
            if (armed) { MA.ov.n = 0; save(); MA.clearArm = 0; ui_toast(TOAST_OK, "Back to Q-Continuum's values everywhere"); }
            else MA.clearArm = (float)GetTime();
        }
        if (armed) ui_request_frames(2);
    }
    y += 46;
    bool forced = MA.search[0] || MA.changedOnly;
    /* the groups, in the table's order */
    const char *seen[MA_GROUPS]; int nSeen = 0;
    for (int k = 0; k < MESH_OPTS_N; k++) {
        const MeshOpt *op = &MESH_OPTS[k];
        if (!(op->when & mask)) continue;
        bool dup = false; for (int i = 0; i < nSeen; i++) if (!strcmp(seen[i], op->group)) dup = true;
        if (dup || nSeen >= MA_GROUPS) continue;
        seen[nSeen++] = op->group;
        const char *g = op->group;
        int nShown = 0;
        bool perName = false; const MeshOpt *first = NULL;
        for (int j = k; j < MESH_OPTS_N; j++)
            if (!strcmp(MESH_OPTS[j].group, g) && (MESH_OPTS[j].when & mask) && matches(&MESH_OPTS[j])) {
                nShown++; if (!first) first = &MESH_OPTS[j];
                if (MESH_OPTS[j].flags & (MO_PATCH | MO_REGION)) perName = true;
            }
        if (!nShown) continue;
        int nm = group_mine(g, mask);
        if (MA.changedOnly && !nm) continue;
        y += group_head(g, nShown, nm, x, y, w, forced);
        if (!(forced || group_open(g))) continue;
        float ix = x + 18, iw = w - 18;
        char names[STL_MAX_REGIONS + 8][64]; int nn = 0;
        const char *name = NULL;
        if (perName) {
            nn = names_for(first, m, s, names, STL_MAX_REGIONS + 8);
            if (!nn) { ui_text(F_REG, 13, first->flags & MO_REGION ? "No refinement regions: add one under Volumetric controls." : "No boundaries.",
                               ix, y + 4, TH.text3); y += 30; continue; }
            int *sel = first->flags & MO_REGION ? &MA.regionSel : &MA.patchSel;
            if (*sel >= nn) *sel = 0;
            const char *items[STL_MAX_REGIONS + 8]; for (int i = 0; i < nn; i++) items[i] = names[i];
            ui_text(F_MED, 13, first->flags & MO_REGION ? "Region" : "Boundary", ix, y + 9, TH.text2);
            ui_select(ui_idn(ui_id("ma-name"), (int)(hash_str(g) & 0xffff)), (Rectangle){ ix + 80, y, fminf(320, iw - 80), 34 }, items, nn, sel);
            name = names[*sel];
            y += 44;
        }
        for (int j = k; j < MESH_OPTS_N; j++) {
            const MeshOpt *op = &MESH_OPTS[j];
            if (strcmp(op->group, g) || !(op->when & mask) || !matches(op)) continue;
            char path[300]; resolve(op, name, path, sizeof path);
            if (!path[0]) continue;
            y += option_row(m, op, path, ix, y, iw, j);
        }
        y += 6;
    }
    y += free_section(m, x, y, w, mask);
    return y - y0;
}
