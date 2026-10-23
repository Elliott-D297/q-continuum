/* page_bc.c -- boundary-condition matrix: patches x fields */
#include <ctype.h>
#include <math.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "fieldinfo.h"
#include "undo.h"

typedef enum { M_NONE = 0, M_EXACT, M_GROUP, M_PATTERN, M_INCLUDE } MatchKind;

static bool in_words(const char *list, const char *w)
{
    size_t n = strlen(w);
    for (const char *p = list; (p = strstr(p, w)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return true;
    return false;
}

static bool constraint_type(const char *t);

/* in one boundaryField: the exact name, then "regex" keys (last wins), then the patch's groups (inGroups; a constraint
   type is a group of its own), as OpenFOAM looks them up (the same order as SENSOR SWEEP) */
static FDEntry *match_in(FDEntry *bf, const PatchInfo *p, MatchKind *kind, char *via, int cap)
{
    FDEntry *e = fd_child(bf, p->name);
    if (e && e->kind == FD_DICT) { *kind = M_EXACT; return e; }
    for (int i = bf->nChild - 1; i >= 0; i--) {
        FDEntry *c = &bf->child[i];
        if (c->kind == FD_DICT && c->key[0] == '"' && pl_foam_key_matches(c->key, p->name)) {
            *kind = M_PATTERN; snprintf(via, (size_t)cap, "%s", c->key); return c;
        }
    }
    for (int i = 0; i < bf->nChild; i++) {
        FDEntry *c = &bf->child[i];
        if (c->kind != FD_DICT || !c->key) continue;
        if (in_words(p->groups, c->key) || (p->type[0] && !strcmp(c->key, p->type))) {
            *kind = M_GROUP; snprintf(via, (size_t)cap, "%s", c->key); return c;
        }
    }
    return NULL;
}

static FDEntry k_etc;                                     /* #includeEtc "caseDicts/setConstraintTypes" on a constraint patch */

/* the entry that gives a field its condition on a patch, and the file it is in (*src: f, an #include'd file, or NULL
   for k_etc). Bug hunt H-04: groups and #include count, as they do for OpenFOAM. */
static FDEntry *find_bc(FDFile *f, const PatchInfo *p, MatchKind *kind, char *via, int cap, FDFile **src)
{
    FDEntry *bf = fd_find(f, "boundaryField");
    *kind = M_NONE; via[0] = 0;
    if (src) *src = f;
    if (!bf) return NULL;
    FDEntry *e = match_in(bf, p, kind, via, cap);
    if (e) return e;
    for (int i = 0; i < bf->nChild; i++) {
        FDEntry *d = &bf->child[i];
        if (d->kind != FD_DIRECTIVE || !d->key) continue;
        char arg[300]; snprintf(arg, sizeof arg, "%.*s", d->valEnd - d->valStart < 299 ? d->valEnd - d->valStart : 299, f->text + d->valStart);
        char *q1 = strchr(arg, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) continue;
        *q2 = 0;
        const char *name = q1 + 1;
        if (!strcmp(d->key, "#includeEtc")) {
            if (strstr(name, "setConstraintTypes") && constraint_type(p->type)) {
                *kind = M_INCLUDE; snprintf(via, (size_t)cap, "%s", pl_basename(name));
                if (src) *src = NULL;
                return &k_etc;
            }
            continue;
        }
        if (strcmp(d->key, "#include") || !A.cs || name[0] == '/' || strchr(name, '$')) continue;
        /* relative to the field file's folder, as a case-relative path for the dictionary cache */
        size_t n = strlen(A.cs->s.path);
        if (strncmp(f->path, A.cs->s.path, n) || f->path[n] != '/') continue;
        char dir[PL_PATH]; pl_strlcpy(dir, f->path + n + 1, sizeof dir);
        char *sl = strrchr(dir, '/'); if (sl) *sl = 0; else dir[0] = 0;
        char rel[PL_PATH];
        if (!pl_fmt(rel, sizeof rel, "%s%s%s", dir, dir[0] ? "/" : "", name)) continue;
        FDFile *inc = case_dict(A.cs, rel);
        if (!inc || !inc->ok) continue;
        FDEntry root = { 0 };                              /* the included file's top level stands in for boundaryField */
        root.kind = FD_DICT; root.child = inc->root.child; root.nChild = inc->root.nChild;
        MatchKind ik; char iv[160];
        FDEntry *ie = match_in(&root, p, &ik, iv, sizeof iv);
        if (ie) { *kind = M_INCLUDE; snprintf(via, (size_t)cap, "%s", name); if (src) *src = inc; return ie; }
    }
    return NULL;
}

static void entry_text(const FDFile *f, const FDEntry *d, const char *key, char *out, int cap)
{
    out[0] = 0;
    FDEntry *e = fd_child((FDEntry *)d, key);
    if (!e) return;
    char raw[400] = "";
    fd_entry_value(f, e, raw, sizeof raw);
    int k = 0; bool sp = false;
    for (char *p = raw; *p && k < cap - 1; p++) {
        if (isspace((unsigned char)*p)) { if (!sp && k) out[k++] = ' '; sp = true; }
        else { out[k++] = *p; sp = false; }
    }
    out[k] = 0;
}

static bool constraint_type(const char *t)
{
    return !strcmp(t, "empty") || !strcmp(t, "symmetryPlane") || !strcmp(t, "wedge") || !strcmp(t, "cyclic") ||
           !strcmp(t, "symmetry") || !strcmp(t, "cyclicAMI") || !strcmp(t, "processor");
}

/* the type a field uses on a patch ("" when it has none); same matching as the table */
const char *bc_type_on(Case *c, const char *field, int patch, char *out, int cap)
{
    out[0] = 0;
    if (patch < 0 || patch >= c->nPatches) return out;
    char rel[300]; snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, field);
    FDFile *f = case_dict(c, rel);
    MatchKind mk; char via[160];
    FDFile *src = NULL;
    FDEntry *e = f ? find_bc(f, &c->patches[patch], &mk, via, sizeof via, &src) : NULL;
    if (e == &k_etc) pl_strlcpy(out, c->patches[patch].type, (size_t)cap);
    else if (e && src) entry_text(src, e, "type", out, cap);
    return out;
}

/* mesh patches with no boundary condition in a field: the solver (and decomposePar) stop on the first one.
   Fields whose boundaryField pulls entries in with #include are skipped, as are their constraint patches. */
int bc_missing_report(Case *c, char *msg, int cap)
{
    int miss = 0, k = 0;
    msg[0] = 0;
    for (int fi = 0; fi < c->fields.n; fi++) {
        char rel[300];
        snprintf(rel, sizeof rel, "%s/%s", c->fieldDir, c->fields.names[fi]);
        FDFile *f = case_dict(c, rel);
        FDEntry *bf = f ? fd_find(f, "boundaryField") : NULL;
        if (!bf) continue;
        bool incl = false;
        for (int i = 0; i < bf->nChild; i++) if (bf->child[i].kind == FD_DIRECTIVE) incl = true;
        if (incl) continue;
        for (int pi = 0; pi < c->nPatches; pi++) {
            const PatchInfo *pa = &c->patches[pi];
            if (pa->pattern || !strcmp(pa->type, "processor")) continue;
            MatchKind mk; char via[160];
            if (find_bc(f, pa, &mk, via, sizeof via, NULL)) continue;
            if (miss++ < 6 && k < cap - 1)
                k += snprintf(msg + k, (size_t)(cap - k), "%s%s/%s", k ? ", " : "", c->fields.names[fi], pa->name);
        }
    }
    if (miss > 6 && k < cap - 1) snprintf(msg + k, (size_t)(cap - k), " and %d more", miss - 6);
    return miss;
}

#define bc_value_problem field_value_problem      /* fieldinfo.c: shared with the checks on typed values */

static Color bc_color(const char *t)
{
    if (!t[0]) return TH.err;
    if (constraint_type(t)) return TH.text3;
    if (strstr(t, "Gradient") || strstr(t, "zeroGradient") || !strcmp(t, "slip") || !strcmp(t, "fixedFluxPressure")) return TH.cat[2];
    if (strstr(t, "WallFunction") || !strcmp(t, "noSlip")) return TH.cat[4];
    if (strstr(t, "inletOutlet") || strstr(t, "Inlet") || strstr(t, "outlet")) return TH.cat[3];
    if (strstr(t, "Radiation")) return TH.cat[1];
    return TH.cat[0];
}

static FDFile *field_file(int fi, char *cls, int cap);

/* ROADMAP 2.5: the field's unit from its dimensions, whether the turbulence model reads it, and a tooltip saying what it
   is, its typical values and the values the tutorials start it at */
typedef struct { char unit[48], dims[64], tip[900], about[700]; int used; } FieldDesc;   /* about: the tip without the name and units */

static void tut_ranges_begin(void)
{
    static bool started;
    if (started) return;
    started = true;
    char tut[PL_PATH], cache[PL_PATH], dir[PL_PATH];
    env_tutorials_dir(&A.env, &A.set, tut, sizeof tut);
    const char *x = getenv("XDG_CACHE_HOME");
    if (x && *x) snprintf(dir, sizeof dir, "%s/q-continuum", x); else snprintf(dir, sizeof dir, "%s/.cache/q-continuum", pl_home());
    if (pl_fmt(cache, sizeof cache, "%s/tutorial-values.txt", dir)) tut_ranges_start(tut, cache);
}

static void field_desc(int fi, FieldDesc *d)
{
    Case *c = A.cs;
    const char *name = c->fields.names[fi];
    memset(d, 0, sizeof *d);
    d->used = -1;
    FDFile *f = field_file(fi, NULL, 0);
    if (f && fd_get(f, "dimensions", d->dims, sizeof d->dims)) dims_unit(d->dims, d->unit, sizeof d->unit);
    char st[32], model[64];
    case_turbulence(c, st, sizeof st, model, sizeof model);
    d->used = turb_field_used(name, st, model);
    Str b; str_init(&b);
    const FieldInfo *fi_ = field_info(name);
    if (fi_) str_cat(&b, fi_->meaning);
    if (fi_ && fi_->typical) str_catf(&b, "\nTypical: %s", fi_->typical);
    TutRange r;
    if (tut_range(name, &r)) {
        if (r.n == 1 || r.min == r.max) str_catf(&b, "\nIn the OpenFOAM tutorials: %g (%d case%s)", r.min, r.n, r.n == 1 ? "" : "s");
        else str_catf(&b, "\nIn the OpenFOAM tutorials: %g to %g, median %g (%d cases)", r.min, r.max, r.median, r.n);
    }
    if (d->used == 0)
        str_catf(&b, "\nNot used: %s doesn't read %s, so this file has no effect.", !strcmp(st, "laminar") ? "a laminar run" : model, name);
    pl_strlcpy(d->about, b.s ? b.s : "", sizeof d->about);
    str_free(&b);
    char units[160] = "";
    if (d->unit[0]) snprintf(units, sizeof units, "\nUnits: %s %s", d->unit, d->dims);
    pl_fmt_ui(d->tip, sizeof d->tip, "%s%s%s%s", name, units, d->about[0] ? "\n" : "", d->about);
}

/* the error translator's "Add a condition" fix: the default condition for the patch in the field file */
bool app_add_bc(const char *fieldRel, const char *patch)
{
    Case *c = A.cs;
    FDFile *f = c ? case_dict(c, fieldRel) : NULL;
    if (!f) return false;
    const char *ptype = "patch";
    for (int i = 0; i < c->nPatches; i++) if (!strcmp(c->patches[i].name, patch)) ptype = c->patches[i].type;
    char cls[64] = ""; fd_get(f, "FoamFile/class", cls, sizeof cls);
    const char *field = pl_basename(fieldRel);
    char path[300]; snprintf(path, sizeof path, "boundaryField/%s/type", patch);
    const char *t = bc_default_type(field, ptype);
    if (!fd_put(f, path, t)) return false;
    snprintf(path, sizeof path, "boundaryField/%s", patch);
    bc_default_entries(f, path, t, field, cls);
    return true;
}

static FDFile *field_file(int fi, char *cls, int cap)
{
    char rel[300];
    snprintf(rel, sizeof rel, "%s/%s", A.cs->fieldDir, A.cs->fields.names[fi]);
    FDFile *f = case_dict(A.cs, rel);
    if (cls) { cls[0] = 0; if (f) fd_get(f, "FoamFile/class", cls, cap); }
    return f;
}

/* one field's condition on one patch: type, its settings, Add setting. Returns true when it changed the file (entries moved:
   stop drawing this frame). The one-cell drawer and the patch inspector share it; the inspector numbers ids by field. */
#define ID(s) (insp ? ui_idn(ui_id(s), fi) : ui_id(s))
static bool cond_editor(int fi, int patch, float x, float *py, float w, bool insp, MatchKind *pmk)
{
    Case *c = A.cs;
    float y = *py;
    char cls[64];
    FDFile *f = field_file(fi, cls, sizeof cls);
    *pmk = M_NONE;
    if (!f) return false;
    const char *field = c->fields.names[fi];
    const PatchInfo *p = &c->patches[patch];
    MatchKind mk; char via[160];
    FDFile *src = NULL;
    FDEntry *e = find_bc(f, p, &mk, via, sizeof via, &src);
    *pmk = mk;
    if (mk == M_INCLUDE) {                                /* defined in an #include'd file: shown, edited there or overridden */
        char t[128] = "";
        if (e == &k_etc) pl_strlcpy(t, p->type, sizeof t); else if (src) entry_text(src, e, "type", t, sizeof t);
        char msg[400];
        if (e == &k_etc) snprintf(msg, sizeof msg, "%s comes from #includeEtc \"caseDicts/setConstraintTypes\": every %s patch gets type %s.", field, p->type, t);
        else snprintf(msg, sizeof msg, "%s on this patch comes from %s, pulled in with #include: type %s. Edit it in that file, or give "
                      "this patch its own entry here (it then takes precedence).", field, via, t[0] ? t : "not set");
        float h = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, h }, TOAST_INFO, ICON_INFO, NULL, msg);
        y += h + 12;
        if (e != &k_etc && src) {
            float bw = ui_button_width(ICON_GIT_FORK, "Override for this patch");
            if (ui_button(ID("bc-override"), (Rectangle){ x, y, bw, 32 }, ICON_GIT_FORK, "Override for this patch", BTN_SECONDARY)) {
                char body[4096];
                int n = e->valEnd - e->valStart;
                if (n > 4000) n = 4000;
                snprintf(body, sizeof body, "%.*s", n, src->text + e->valStart);
                char path[300]; snprintf(path, sizeof path, "boundaryField/%s", p->name);
                fd_ensure_dict(f, path);
                FDEntry *ne = fd_find(f, path);
                if (ne) fd_replace_span(f, ne->valStart, ne->valEnd, body);
                ui_toast(TOAST_OK, "%s now has its own %s condition in %s", p->name, field, pl_basename(f->path));
                return true;
            }
            float ob = ui_button_width(ICON_EXTERNAL_LINK, "Open the file");
            if (ui_button(ID("bc-open-inc"), (Rectangle){ x + bw + 8, y, ob, 32 }, ICON_EXTERNAL_LINK, "Open the file", BTN_GHOST)) {
                size_t n = strlen(c->s.path);
                if (!strncmp(src->path, c->s.path, n)) pl_strlcpy(A.filesSel, src->path + n + 1, sizeof A.filesSel);
                app_go(PG_FILES);
                return true;
            }
            y += 44;
        }
        *py = y;
        return false;
    }
    if (!e) {
        const char *msg = "OpenFOAM stops with an error when a patch has no condition. Add one to continue.";
        float h = callout_height(w, "No condition for this patch", msg);
        callout((Rectangle){ x, y, w, h }, TOAST_ERR, ICON_CIRCLE_X, "No condition for this patch", msg);
        y += h + 14;
        if (ui_button(ID("bc-add"), (Rectangle){ x, y, w, 36 }, ICON_PLUS, "Add condition", BTN_PRIMARY)) {
            char path[300]; snprintf(path, sizeof path, "boundaryField/%s/type", p->name);
            const char *t = bc_default_type(field, p->type);
            fd_put(f, path, t);
            snprintf(path, sizeof path, "boundaryField/%s", p->name);
            bc_default_entries(f, path, t, field, cls);
            if (!strcmp(t, "fixedValue") && !strcmp(field, "nuTilda")) { snprintf(path, sizeof path, "boundaryField/%s/value", p->name); fd_put(f, path, "uniform 0"); }   /* Spalart-Allmaras: zero at walls */
            return true;
        }
        *py = y + 50;
        return false;
    }
    char base[300];
    snprintf(base, sizeof base, "boundaryField/%s", e->key);
    if (mk != M_EXACT) {
        char msg[300];
        snprintf(msg, sizeof msg, "This patch takes its condition from %s %s, shared with other patches. Edits here change all of them.",
                 mk == M_GROUP ? "the patch group" : "the pattern", via);
        float h = callout_height(w, NULL, msg) + 44;
        Rectangle cr = { x, y, w, h };
        callout(cr, TOAST_INFO, ICON_INFO, NULL, msg);
        if (ui_button(ID("bc-override"), (Rectangle){ x + 42, y + h - 42, 220, 30 }, ICON_GIT_FORK, "Override for this patch", BTN_SECONDARY)) {
            /* copy the shared block into an exact entry for this patch */
            char body[4096];
            int n = e->valEnd - e->valStart;
            if (n > 4000) n = 4000;
            snprintf(body, sizeof body, "%.*s", n, f->text + e->valStart);
            char path[300]; snprintf(path, sizeof path, "boundaryField/%s", p->name);
            fd_ensure_dict(f, path);
            FDEntry *ne = fd_find(f, path);
            if (ne) fd_replace_span(f, ne->valStart, ne->valEnd, body);
            ui_toast(TOAST_OK, "%s now has its own %s condition", p->name, field);
            return true;
        }
        y += h + 14;
    }

    char type[128] = "";
    char tp[340]; snprintf(tp, sizeof tp, "%s/type", base);
    fd_get(f, tp, type, sizeof type);
    ui_text(F_MED, 13.5f, "Type", x, y, TH.text); y += 24;
    const char *const *items; int n = suggest_bc_types(field, cls, &items);
    char out[160];
    if (ui_dropdown(ID("bc-type"), (Rectangle){ x, y, w, 36 }, type, items, n, DD_FILTER | DD_CUSTOM | DD_MONO, out, sizeof out, "type")) {
        if (strcmp(out, type) != 0) {
            fd_put(f, tp, out);
            bc_default_entries(f, base, out, field, cls);
            return true;
        }
    }
    y += 46;
    if (constraint_type(p->type) && strcmp(type, p->type) != 0) {
        char msg[200]; snprintf(msg, sizeof msg, "The mesh patch is %s, so this field must use %s too.", p->type, p->type);
        float h = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, h }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, msg);
        y += h + 12;
    }

    FDEntry *d = fd_find(f, base);
    if (!d) { *py = y; return false; }
    bool any = false;
    for (int i = 0; i < d->nChild; i++) {
        FDEntry *k = &d->child[i];
        if (k->kind != FD_VALUE || !strcmp(k->key, "type")) continue;
        any = true;
        char path[400]; snprintf(path, sizeof path, "%s/%s", base, k->key);
        char key[96]; snprintf(key, sizeof key, "%s", k->key);
        ui_text(F_MONO, 13, key, x, y, TH.text2);
        y += 22;
        if (dv_value_editor(f, path, key, base, (Rectangle){ x, y, w - 36, 34 })) return true;
        UiId del = insp ? ui_idn(ui_idn(ui_id("bc-del"), fi), i) : ui_idn(ui_id(path), 9);
        if (ui_button(del, (Rectangle){ x + w - 30, y + 3, 28, 28 }, ICON_TRASH_2, NULL, BTN_ICON)) { fd_remove(f, path); return true; }
        y += 46;
        if (!strcmp(key, "value") || !strcmp(key, "inletValue")) {      /* refValue's kind depends on the type */
            char raw[400]; fd_entry_value(f, k, raw, sizeof raw);
            const char *why = bc_value_problem(cls, raw);
            if (why) { y -= 6; y += ui_text_wrap(F_REG, 12.5f, why, (Rectangle){ x, y, w, 40 }, 18, TH.err, true) + 10; }
        }
    }
    if (!any) { ui_text(F_REG, 13, "No other settings for this type.", x, y, TH.text3); y += 30; }
    if (constraint_type(type)) { *py = y + 10; return false; }       /* empty, wedge, symmetry: nothing to add (H-22) */
    float bw = ui_button_width(ICON_PLUS, "Add setting");
    if (ui_button(ID("bc-addentry"), (Rectangle){ x, y, bw, 32 }, ICON_PLUS, "Add setting", BTN_GHOST)) {
        A.modal = MD_ADD_ENTRY; A.mFlag = false;
        snprintf(A.mPath, sizeof A.mPath, "%s", base);
        snprintf(A.mFile, sizeof A.mFile, "%s", f->path);
        A.mText[0] = A.mText2[0] = A.mErr[0] = 0;
    }
    y += 48;
    *py = y;
    return false;
}
#undef ID

/* ============================================================================ drawer */
static void drawer(Rectangle r)
{
    Case *c = A.cs;
    if (A.bcField < 0 || A.bcField >= c->fields.n) return;
    char cls[64];
    FDFile *f = field_file(A.bcField, cls, sizeof cls);
    if (!f) return;
    const char *field = c->fields.names[A.bcField];
    ui_shadow(r, 14, 1.2f);
    ui_rect(r, 14, TH.surface);
    ui_rect_lines(r, 14, 1, TH.line);
    float x = r.x + 22, w = r.width - 44, y = r.y + 20;
    if (ui_button(ui_id("bc-close"), (Rectangle){ r.x + r.width - 44, r.y + 14, 30, 30 }, ICON_X, NULL, BTN_ICON)) { A.bcField = -1; return; }

    if (A.bcPatch < 0) {                                  /* internal field */
        ui_text(F_SEMI, 18, "Initial value", x, y, TH.text);
        char sub[160]; snprintf(sub, sizeof sub, "%s in every cell at the start time (%s)", field, cls[0] ? cls : "field");
        ui_text_clip(F_REG, 13, sub, x, y + 28, w - 30, TH.text2);
        y += 66;
        ui_text(F_MED, 13.5f, "internalField", x, y, TH.text); y += 24;
        dv_text(f, "internalField", (Rectangle){ x, y, w, 36 }, 0, strstr(cls, "Vector") ? "uniform (0 0 0)" : "uniform 0");
        y += 50;
        ui_text(F_MED, 13.5f, "Dimensions", x, y, TH.text); y += 22;
        char dim[128] = ""; fd_get(f, "dimensions", dim, sizeof dim);
        ui_text(F_MONO, 13, dim, x, y, TH.text2);
        char un[48];
        if (dims_unit(dim, un, sizeof un)) ui_text(F_REG, 13, un, x + ui_text_w(F_MONO, 13, dim) + 12, y, TH.text);
        y += 30;
        FieldDesc d; field_desc(A.bcField, &d);
        if (d.about[0]) y += ui_text_wrap(F_REG, 13, d.about, (Rectangle){ x, y, w, 300 }, 19, TH.text2, true) + 14;
        const char *hint = "Write uniform 0 for a scalar or uniform (1 0 0) for a vector. Non-uniform fields made by setFields or mapFields show as a list.";
        float h = callout_height(w, NULL, hint);
        callout((Rectangle){ x, y, w, h }, TOAST_INFO, ICON_INFO, NULL, hint);
        return;
    }
    if (A.bcPatch >= c->nPatches) return;
    const PatchInfo *p = &c->patches[A.bcPatch];
    ui_text_clip(F_SEMI, 18, p->name, x, y, w - 40, TH.text);
    char sub[200]; snprintf(sub, sizeof sub, "Field %s · mesh type %s", field, p->type[0] ? p->type : "not set");
    ui_text_clip(F_REG, 13, sub, x, y + 28, w - 30, TH.text2);
    y += 64;

    MatchKind mk;
    if (cond_editor(A.bcField, A.bcPatch, x, &y, w, false, &mk)) return;
    FDEntry *d = NULL;
    {
        char via[160], base[300];
        FDEntry *e = find_bc(f, p, &mk, via, sizeof via, NULL);
        if (!e || mk == M_INCLUDE) return;
        snprintf(base, sizeof base, "boundaryField/%s", e->key);
        d = fd_find(f, base);
    }
    if (!d) return;

    /* copy to similar patches */
    if (mk == M_EXACT && p->type[0] && !constraint_type(p->type)) {
        int same = 0;
        for (int i = 0; i < c->nPatches; i++) if (i != A.bcPatch && !strcmp(c->patches[i].type, p->type)) same++;
        if (same > 0) {
            char lab[160];
            if (same == 1) {
                const char *other = "";
                for (int i = 0; i < c->nPatches; i++) if (i != A.bcPatch && !strcmp(c->patches[i].type, p->type)) other = c->patches[i].name;
                snprintf(lab, sizeof lab, "Copy to %s", other);
            } else snprintf(lab, sizeof lab, "Copy to the other %d patches of type %s", same, p->type);
            ui_divider(x, y, w); y += 16;
            if (ui_button(ui_id("bc-copyall"), (Rectangle){ x, y, w, 34 }, ICON_COPY, lab, BTN_SECONDARY)) {
                char body[4096];
                int nn = d->valEnd - d->valStart;
                if (nn > 4000) nn = 4000;
                snprintf(body, sizeof body, "%.*s", nn, f->text + d->valStart);
                int done = 0;
                for (int i = 0; i < c->nPatches; i++) {
                    if (i == A.bcPatch || strcmp(c->patches[i].type, p->type)) continue;
                    char path[300]; snprintf(path, sizeof path, "boundaryField/%s", c->patches[i].name);
                    fd_ensure_dict(f, path);
                    FDEntry *te = fd_find(f, path);
                    if (te) { fd_replace_span(f, te->valStart, te->valEnd, body); done++; }
                }
                ui_toast(TOAST_OK, "Copied the %s condition to %d patch%s", field, done, done == 1 ? "" : "es");
            }
        }
    }
}

/* ============================================================================ patch inspector (ROADMAP 3.6) */
/* fields in the order they are changed most: U, pressure, T, turbulence, the rest by name */
static int field_rank(const char *n)
{
    static const char *const order[] = { "U", "p", "p_rgh", "T", "k", "epsilon", "omega", "nuTilda", "nut", "alphat", "G", NULL };
    for (int i = 0; order[i]; i++) if (!strcmp(n, order[i])) return i;
    return 100;
}

/* a field's uniform internal value ("uniform 0.375" -> "uniform 0.375"; "" when it is non-uniform) */
static void internal_uniform(FDFile *f, char *out, int cap)
{
    out[0] = 0;
    FDEntry *ie = fd_find(f, "internalField");
    char raw[200] = "";
    if (ie) fd_entry_value(f, ie, raw, sizeof raw);
    const char *t = raw;
    while (isspace((unsigned char)*t)) t++;
    if (!strncmp(t, "uniform", 7) && strlen(t) < (size_t)cap) pl_strlcpy(out, t, (size_t)cap);
}

/* the condition a patch role gives a field. Values start at the field's own uniform internal value, so an inlet's k
   and an outlet's p match what the case starts with; the user edits them below. */
typedef enum { ROLE_WALL, ROLE_INLET, ROLE_OUTLET } PatchRole;
static const char *role_type(PatchRole r, const char *field, const char *cls, bool *takesValue)
{
    bool vec = cls && strstr(cls, "Vector");
    bool pressure = !strcmp(field, "p") || !strcmp(field, "p_rgh");
    *takesValue = false;
    if (r == ROLE_WALL) return bc_default_type(field, "wall");
    if (!strcmp(field, "nut") || !strcmp(field, "alphat") || !strcmp(field, "nuSgs") || !strcmp(field, "mut")) return "calculated";
    if (!strncmp(field, "ILambda", 7) || !strncmp(field, "IDefault", 8) || !strcmp(field, "G") || !strcmp(field, "qr")) return bc_default_type(field, "patch");
    if (r == ROLE_INLET) {
        if (!strcmp(field, "p_rgh")) return "fixedFluxPressure";
        if (pressure) return "zeroGradient";
        *takesValue = true;                               /* U, T, k, epsilon, omega, nuTilda, alpha.*, species: what comes in */
        return "fixedValue";
    }
    if (pressure) { *takesValue = true; return "fixedValue"; }
    (void)vec;
    return "inletOutlet";                                 /* U, T, turbulence, alpha.*, species: backflow takes inletValue */
}

static int apply_role(PatchRole role, int patch, char *note, int ncap)
{
    Case *c = A.cs;
    const PatchInfo *p = &c->patches[patch];
    int changed = 0;
    note[0] = 0;
    for (int fi = 0; fi < c->fields.n; fi++) {
        char cls[64];
        FDFile *f = field_file(fi, cls, sizeof cls);
        if (!f || f->binary) continue;
        const char *field = c->fields.names[fi];
        bool takes;
        const char *t = role_type(role, field, cls, &takes);
        MatchKind mk; char via[160];
        FDFile *src = NULL;
        FDEntry *e = find_bc(f, p, &mk, via, sizeof via, &src);
        char cur[128] = "";
        if (e == &k_etc) pl_strlcpy(cur, p->type, sizeof cur);
        else if (e && src) entry_text(src, e, "type", cur, sizeof cur);
        if (e && !strcmp(cur, t)) continue;                           /* already that (its own entry, a group's, an include's) */
        char path[300];
        snprintf(path, sizeof path, "boundaryField/%s", p->name);
        /* the old values worth keeping: an inlet velocity the patch already had stays */
        char oldVal[200] = "";
        if (e && src && e != &k_etc && !strcmp(cur, "fixedValue")) entry_text(src, e, "value", oldVal, sizeof oldVal);
        FDEntry *own = mk == M_EXACT ? e : NULL;
        if (own) {                                                    /* the role's condition replaces the old one */
            int k = own->valEnd;                                      /* keep the newline + indent before the '}' */
            while (k > own->valStart && (f->text[k - 1] == ' ' || f->text[k - 1] == '\t')) k--;
            if (k > own->valStart && f->text[k - 1] == '\n') k--;
            char keep[64]; snprintf(keep, sizeof keep, "%.*s", own->valEnd - k < 63 ? own->valEnd - k : 63, f->text + k);
            fd_replace_span(f, own->valStart, own->valEnd, keep);
        }
        else fd_ensure_dict(f, path);
        char tp[340]; snprintf(tp, sizeof tp, "%s/type", path);
        fd_put(f, tp, t);
        char iv[200]; internal_uniform(f, iv, sizeof iv);
        bool vec = strstr(cls, "Vector") != NULL;
        if (!strcmp(t, "inletOutlet") && iv[0]) { snprintf(tp, sizeof tp, "%s/inletValue", path); fd_put(f, tp, vec ? "uniform (0 0 0)" : iv); }
        if ((takes || !strcmp(t, "calculated") || strstr(t, "WallFunction") || !strcmp(t, "inletOutlet")) && (iv[0] || oldVal[0])) {
            snprintf(tp, sizeof tp, "%s/value", path);
            fd_put(f, tp, role == ROLE_INLET && oldVal[0] ? oldVal : iv[0] ? iv : oldVal);
        }
        bc_default_entries(f, path, t, field, cls);
        /* an inlet that lets nothing in: say so (H-15) */
        if (role == ROLE_INLET && vec && !oldVal[0]) {
            char v[200] = ""; snprintf(tp, sizeof tp, "%s/value", path); fd_get(f, tp, v, sizeof v);
            double a = 0, b = 0, cc = 0; const char *q = strchr(v, '(');
            if (q && sscanf(q + 1, "%lf %lf %lf", &a, &b, &cc) == 3 && a == 0 && b == 0 && cc == 0)
                pl_fmt_ui(note, (size_t)ncap, " %s is uniform (0 0 0) there: type the inlet velocity below.", field);
        }
        changed++;
    }
    return changed;
}

static void inspector(Rectangle r)
{
    Case *c = A.cs;
    if (A.bcPatch < 0 || A.bcPatch >= c->nPatches) { A.bcField = -1; return; }
    const PatchInfo *p = &c->patches[A.bcPatch];
    ui_shadow(r, 14, 1.2f);
    ui_rect(r, 14, TH.surface);
    ui_rect_lines(r, 14, 1, TH.line);
    if (ui_button(ui_id("bc-close"), (Rectangle){ r.x + r.width - 44, r.y + 14, 30, 30 }, ICON_X, NULL, BTN_ICON)) { A.bcField = -1; return; }
    float x = r.x + 22, w = r.width - 44, y = r.y + 20;
    ui_text_clip(F_SEMI, 18, p->name, x, y, w - 40, TH.text);
    char sub[200]; snprintf(sub, sizeof sub, "Every field on this patch · mesh type %s", p->type[0] ? p->type : "not set");
    ui_text_clip(F_REG, 13, sub, x, y + 28, w - 40, TH.text2);
    y += 60;

    /* the fields, sorted; the ones the turbulence model doesn't read go last, folded */
    int ord[256], n = 0, nUnused = 0;
    static bool showUnused;
    FieldDesc *fd = calloc((size_t)(c->fields.n > 0 ? c->fields.n : 1), sizeof *fd);
    if (!fd) return;
    for (int fi = 0; fi < c->fields.n && n < 256; fi++) { field_desc(fi, &fd[fi]); ord[n++] = fi; }
    for (int i = 1; i < n; i++)                                       /* insertion sort: used first, then rank, then name */
        for (int j = i; j > 0; j--) {
            int a = ord[j - 1], b = ord[j];
            int ka = fd[a].used == 0, kb = fd[b].used == 0;
            int ra = field_rank(c->fields.names[a]), rb = field_rank(c->fields.names[b]);
            bool swap = ka != kb ? ka > kb : ra != rb ? ra > rb : strcmp(c->fields.names[a], c->fields.names[b]) > 0;
            if (!swap) break;
            ord[j - 1] = b; ord[j] = a;
        }
    for (int i = 0; i < n; i++) if (fd[ord[i]].used == 0) nUnused++;

    /* role presets: one click sets every field for a wall, an inlet or an outlet */
    if (!constraint_type(p->type)) {
        ui_text(F_MED, 13, "Make it a", x, y + 8, TH.text2);
        static const char *const lab[] = { "Wall", "Inlet", "Outlet" };
        static const char *const tip[] = {
            "U noSlip, p zeroGradient, p_rgh fixedFluxPressure, k/epsilon/omega/nut wall functions, T zeroGradient",
            "U, T, k, epsilon, omega fixedValue at the field's starting value; p zeroGradient; nut, alphat calculated",
            "p fixedValue at the starting value; U, T and turbulence inletOutlet (backflow takes the inlet value); nut, alphat calculated" };
        float bx = x + ui_text_w(F_MED, 13, "Make it a") + 12;
        for (int k = 0; k < 3; k++) {
            float bw = ui_button_width(NULL, lab[k]);
            Rectangle br = { bx, y, bw, 32 };
            if (ui_button(ui_idn(ui_id("pi-role"), k), br, NULL, lab[k], BTN_SECONDARY)) {
                char what[200], note[200];
                pl_fmt_ui(what, sizeof what, "%s set for %s %s in every field", p->name, k == 0 ? "a" : "an", k == 0 ? "wall" : k == 1 ? "inlet" : "outlet");
                undo_group_begin(what);                       /* one Ctrl+Z takes back every field (H-05) */
                int ch = apply_role((PatchRole)k, A.bcPatch, note, sizeof note);
                undo_group_end();
                if (ch) ui_toast(note[0] ? TOAST_WARN : TOAST_OK, "%s: %d field%s set for %s %s (not saved yet; Ctrl+Z undoes).%s", p->name, ch, ch == 1 ? "" : "s",
                                 k == 0 ? "a" : "an", k == 0 ? "wall" : k == 1 ? "inlet" : "outlet", note);
                else ui_toast(TOAST_INFO, "%s already has those conditions", p->name);
                free(fd);
                return;
            }
            ui_tooltip(br, tip[k]);
            bx += bw + 8;
        }
        y += 44;
    } else {
        char msg[200]; snprintf(msg, sizeof msg, "%s is %s %s patch: every field must use %s, so there is nothing to choose here.",
                                p->name, strchr("aeiou", p->type[0]) ? "an" : "a", p->type, p->type);
        float h = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, h }, TOAST_INFO, ICON_INFO, NULL, msg);
        y += h + 12;
    }
    ui_divider(r.x, y, r.width);
    y += 1;

    Rectangle view = { r.x, y, r.width, r.y + r.height - y - 8 };
    UiScroll *sc = &A.scr[6];
    float top = ui_scroll_begin(sc, view), yy = top + 14;
    for (int i = 0; i < n; i++) {
        int fi = ord[i];
        if (fd[fi].used == 0 && !showUnused) {
            char t[96]; snprintf(t, sizeof t, "Show %d field%s the model doesn't read", nUnused, nUnused == 1 ? "" : "s");
            if (ui_button(ui_id("pi-unused"), (Rectangle){ x, yy, ui_button_width(ICON_CHEVRON_DOWN, t), 30 }, ICON_CHEVRON_DOWN, t, BTN_GHOST)) showUnused = true;
            yy += 40;
            break;
        }
        const char *nm = c->fields.names[fi];
        ui_text(F_MONO_MED, 15, nm, x, yy, fd[fi].used == 0 ? TH.text3 : TH.text);
        float nx = x + ui_text_w(F_MONO_MED, 15, nm) + 10;
        const char *u = fd[fi].used == 0 ? "not used" : fd[fi].unit;
        ui_text(F_REG, 12.5f, u, nx, yy + 2, fd[fi].used == 0 ? TH.warn : TH.text3);
        Rectangle hr = { x, yy - 2, w, 22 };
        ui_tooltip(hr, fd[fi].tip);
        yy += 28;
        MatchKind mk;
        if (cond_editor(fi, A.bcPatch, x, &yy, w, true, &mk)) { ui_scroll_end(sc, yy - top); free(fd); return; }
        if (i < n - 1) { ui_divider(x, yy - 8, w); yy += 10; }
    }
    if (showUnused && nUnused) {
        if (ui_button(ui_id("pi-unused"), (Rectangle){ x, yy, ui_button_width(ICON_CHEVRON_UP, "Hide unused fields"), 30 }, ICON_CHEVRON_UP, "Hide unused fields", BTN_GHOST)) showUnused = false;
        yy += 40;
    }
    ui_scroll_end(sc, yy - top + 10);
    free(fd);
}

/* ============================================================================ page */
void page_bc(Rectangle r)
{
    Case *c = A.cs;
    bool drawerOpen = A.bcField >= 0 || (A.bcField == BC_INSPECT && A.bcPatch >= 0);
    float drawerW = 420;
    Rectangle main = r;
    if (drawerOpen) main.width -= drawerW + 16;
    UiScroll *sc = &A.scr[5];
    float top = ui_scroll_begin(sc, main);
    float x = main.x + PAGE_PAD, w = main.width - 2 * PAGE_PAD;
    float y = top + 30;
    char sub[300];
    snprintf(sub, sizeof sub, "One row per patch, one column per field in %s/. Click a cell to change how that field behaves on that patch, "
             "or a patch name to see and change every field on it.", c->fieldDir);
    y += page_header(x, y, w, "Boundary conditions", sub);
    y += region_bar(x, y, w);
    y += region_generated_note(x, y, w);
    if (pl_starts_with(c->fieldDir, "0.orig")) {
        const char *msg = "This case keeps its initial fields in 0.orig. Runs copy them to 0, so edits here are the ones that count.";
        float ch = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_INFO, NULL, msg);
        y += ch + 18;
    }
    if (c->fields.n == 0 || c->nPatches == 0) {
        const char *msg = c->fields.n == 0 ? "No field files were found in the initial-conditions folder." : "No patches were found. Generate the mesh first.";
        float ch = callout_height(w, NULL, msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, msg);
        y += ch;
        ui_scroll_end(sc, y - top + 40);
        return;
    }

    /* columns (paged when there are many fields) */
    static int colPage = 0;
    float patchW = 210, typeW = 104;
    float avail = w - patchW - typeW;
    int perPage = (int)(avail / 150);
    if (perPage < 1) perPage = 1;
    int nPages = (c->fields.n + perPage - 1) / perPage;
    if (colPage >= nPages) colPage = 0;
    /* keep the field being edited in view: the drawer narrows the table and reflows the columns */
    static int lastSel = -2, lastPer = -1;
    if (A.bcField >= 0 && A.bcField < c->fields.n && (A.bcField != lastSel || perPage != lastPer)) colPage = A.bcField / perPage;
    lastSel = A.bcField; lastPer = perPage;
    int f0 = colPage * perPage, f1 = f0 + perPage < c->fields.n ? f0 + perPage : c->fields.n;
    float colW = avail / (f1 - f0);
    if (nPages > 1) {
        char lab[64]; snprintf(lab, sizeof lab, "Fields %d-%d of %d", f0 + 1, f1, c->fields.n);
        ui_text(F_REG, 13, lab, x + w - 160 - ui_text_w(F_REG, 13, lab), y + 7, TH.text2);
        if (ui_button(ui_id("bc-prev"), (Rectangle){ x + w - 72, y, 32, 32 }, ICON_CHEVRON_LEFT, NULL, BTN_SECONDARY) && colPage > 0) colPage--;
        if (ui_button(ui_id("bc-next"), (Rectangle){ x + w - 32, y, 32, 32 }, ICON_CHEVRON_RIGHT, NULL, BTN_SECONDARY) && colPage < nPages - 1) colPage++;
        y += 44;
    }

    float rowH = 58, headH = 44;
    int nRows = c->nPatches + 1;
    Rectangle card = { x, y, w, headH + nRows * rowH };
    ui_rect(card, 12, TH.surface);
    ui_rect_lines(card, 12, 1, TH.lineSoft);
    ui_text(F_MED, 12.5f, "Patch", x + 18, y + 14, TH.text3);
    ui_text(F_MED, 12.5f, "Mesh type", x + patchW, y + 14, TH.text3);
    tut_ranges_begin();
    FieldDesc fds[64];
    for (int fi = f0; fi < f1 && fi - f0 < 64; fi++) {
        float cx = x + patchW + typeW + (fi - f0) * colW;
        FieldDesc *d = &fds[fi - f0];
        field_desc(fi, d);
        const char *nm = c->fields.names[fi];
        ui_text_clip(F_MONO_MED, 13, nm, cx + 12, y + 7, colW - 20, d->used == 0 ? TH.text3 : TH.text);
        const char *sub = d->used == 0 ? "not used" : d->unit;
        ui_text_clip(F_REG, 11.5f, sub, cx + 12, y + 25, colW - 20, d->used == 0 ? TH.warn : TH.text3);
        Rectangle hc = { cx + 4, y + 2, colW - 8, headH - 4 };
        ui_register(ui_idn(ui_id("bc-head"), fi), hc, nm);
        ui_tooltip(hc, d->tip);
    }
    ui_divider(x, y + headH - 1, w);
    float ry = y + headH;
    int issues = 0;
    for (int row = -1; row < c->nPatches; row++) {
        Rectangle rr = { x, ry, w, rowH };
        if (row >= 0) {                                   /* the patch name opens the patch inspector: every field on it */
            Rectangle pc = { x + 6, ry + 5, patchW - 12, rowH - 10 };
            bool sel = A.bcField == BC_INSPECT && A.bcPatch == row;
            if (ui_row(ui_idn(ui_id("bc-patch"), row), pc, sel)) { A.bcField = BC_INSPECT; A.bcPatch = row; A.scr[6].y = A.scr[6].target = 0; }
            if (sel) ui_rect_lines(pc, 7, 1.5f, TH.accent);
            ui_text_clip(F_MONO_MED, 13.5f, c->patches[row].name, x + 18, ry + 12, patchW - 26, TH.text);
            ui_text(F_REG, 11.5f, "every field", x + 18, ry + 31, sel || ui_hovered(pc) ? TH.accent : TH.text3);
            ui_tooltip(pc, "Patch inspector: this patch's condition in every field, edited in one place");
        }
        else { ui_text(F_MED, 13.5f, "Initial value", x + 18, ry + 12, TH.text); ui_text(F_REG, 12, "inside the domain", x + 18, ry + 31, TH.text3); }
        if (row >= 0 && c->patches[row].type[0]) ui_badge(x + patchW, ry + 18, NULL, c->patches[row].type, TH.text2, TH.lineSoft);
        for (int fi = f0; fi < f1; fi++) {
            char cls[64];
            FDFile *f = field_file(fi, cls, sizeof cls);
            Rectangle cell = { x + patchW + typeW + (fi - f0) * colW + 4, ry + 5, colW - 8, rowH - 10 };
            bool sel = A.bcField == fi && A.bcPatch == row;
            UiId id = ui_idn(ui_idn(ui_id("bc-cell"), fi), row + 1);
            if (ui_row(id, cell, sel)) { A.bcField = fi; A.bcPatch = row; }
            if (sel) ui_rect_lines(cell, 7, 1.5f, TH.accent);
            if (!f) { ui_text(F_REG, 12.5f, "unreadable", cell.x + 8, cell.y + 16, TH.err); continue; }
            if (row < 0) {
                char v[160]; FDEntry *ie = fd_find(f, "internalField");
                char raw[160] = ""; if (ie) fd_entry_value(f, ie, raw, sizeof raw);
                int k = 0; bool sp = false;
                for (char *p = raw; *p && k < 150; p++) { if (isspace((unsigned char)*p)) { if (!sp && k) v[k++] = ' '; sp = true; } else { v[k++] = *p; sp = false; } }
                v[k] = 0;
                const char *un = fi - f0 < 64 && strcmp(fds[fi - f0].unit, "dimensionless") ? fds[fi - f0].unit : "";
                float uw = v[0] && un[0] ? ui_text_w(F_REG, 11.5f, un) + 8 : 0;
                float vw = fminf(ui_text_w(F_MONO, 12.5f, v[0] ? v : "not set"), cell.width - 16 - uw);
                ui_text_clip(F_MONO, 12.5f, v[0] ? v : "not set", cell.x + 8, cell.y + 15, cell.width - 16 - uw, v[0] ? TH.text2 : TH.err);
                if (uw > 0) ui_text(F_REG, 11.5f, un, cell.x + 8 + vw + 6, cell.y + 16, TH.text3);
                continue;
            }
            MatchKind mk; char via[160];
            FDFile *src = NULL;
            FDEntry *e = find_bc(f, &c->patches[row], &mk, via, sizeof via, &src);
            if (!e) {
                ui_icon(ICON_CIRCLE_X, cell.x + 14, cell.y + 24, 14, TH.err);
                ui_text(F_MED, 12.5f, "missing", cell.x + 28, cell.y + 15, TH.err);
                issues++;
                continue;
            }
            char type[96] = "", val[160] = "";
            if (e == &k_etc) pl_strlcpy(type, c->patches[row].type, sizeof type);
            else if (src) {
                entry_text(src, e, "type", type, sizeof type);
                entry_text(src, e, "value", val, sizeof val);
                if (!val[0]) entry_text(src, e, "inletValue", val, sizeof val);
            }
            bool badVal = val[0] && bc_value_problem(cls, val);
            if (badVal) issues++;
            if (!val[0] && src && e != &k_etc) entry_text(src, e, "uniformValue", val, sizeof val);
            bool mismatch = constraint_type(c->patches[row].type) && strcmp(type, c->patches[row].type) != 0 && c->patchSource == 1;
            if (mismatch) issues++;
            DrawCircleV((Vector2){ cell.x + 12, cell.y + 15 }, 3.5f, mismatch ? TH.warn : bc_color(type));
            ui_text_clip(F_MONO, 12.5f, type[0] ? type : "no type", cell.x + 22, cell.y + 7, cell.width - 30, mismatch ? TH.warn : TH.text);
            char second[200];
            if (mk != M_EXACT) snprintf(second, sizeof second, "via %s", via);
            else snprintf(second, sizeof second, "%s", val);
            ui_text_clip(F_MONO, 12, second, cell.x + 22, cell.y + 26, cell.width - 30, badVal ? TH.err : TH.text3);
            if (badVal) ui_tooltip(cell, bc_value_problem(cls, val));
            if (mismatch) ui_tooltip(cell, "The mesh patch is a constraint type; the field must use the same type.");
        }
        if (row < c->nPatches - 1) ui_divider(x + 12, ry + rowH - 1, w - 24);
        (void)rr;
        ry += rowH;
    }
    y = ry + 18;
    /* the columns are paged: missing conditions on the fields out of view count too */
    int hidden = 0;
    for (int fi = 0; fi < c->fields.n; fi++) {
        if (fi >= f0 && fi < f1) continue;
        char cls[64]; FDFile *f = field_file(fi, cls, sizeof cls);
        for (int row = 0; f && row < c->nPatches; row++) {
            MatchKind mk; char via[160];
            if (!find_bc(f, &c->patches[row], &mk, via, sizeof via, NULL)) hidden++;
        }
    }
    if (issues || hidden) {
        char t[160];
        if (!hidden) snprintf(t, sizeof t, "%d condition%s need%s attention", issues, issues == 1 ? "" : "s", issues == 1 ? "s" : "");
        else snprintf(t, sizeof t, "%d condition%s need%s attention here, %d missing on the other fields",
                      issues, issues == 1 ? "" : "s", issues == 1 ? "s" : "", hidden);
        ui_icon(ICON_TRIANGLE_ALERT, x + 8, y + 9, 15, TH.warn);
        ui_text(F_MED, 13.5f, t, x + 24, y, TH.warn);
        y += 30;
    }
    /* legend */
    struct { const char *t; Color c; } lg[] = { { "Fixed value", TH.cat[0] }, { "Gradient / zero gradient", TH.cat[2] },
                                                { "Inlet-outlet", TH.cat[3] }, { "Wall / wall function", TH.cat[4] },
                                                { "Radiation", TH.cat[1] }, { "Constraint", TH.text3 } };
    float lx = x;
    for (size_t i = 0; i < sizeof lg / sizeof lg[0]; i++) {
        DrawCircleV((Vector2){ lx + 5, y + 8 }, 3.5f, lg[i].c);
        ui_text(F_REG, 12.5f, lg[i].t, lx + 14, y, TH.text3);
        lx += ui_text_w(F_REG, 12.5f, lg[i].t) + 34;
    }
    y += 40;
    ui_scroll_end(sc, y - top);

    if (drawerOpen) {
        Rectangle d = { r.x + r.width - drawerW - 16, r.y + 16, drawerW, r.height - 32 };
        UiScroll *ds = &A.scr[6];
        ui_clip_push((Rectangle){ d.x - 24, d.y - 24, d.width + 48, d.height + 48 });   /* room for the shadow */
        if (A.bcField == BC_INSPECT) inspector(d); else drawer(d);
        ui_clip_pop();
        (void)ds;
    }
}
