/* palette.c -- the COMPUTER bar (ROADMAP 3.4): Ctrl+K, type, Enter.
 *
 * One search over the action registry (pages are actions too), the cases, the open case's patches and fields, and every
 * keyword of its dictionaries (system, constant, the initial fields). "deltaT = 0.001" (or "PIMPLE/nCorrectors = 2",
 * "controlDict/endTime = 5") sets that entry: the preview shows old -> new and the file before Enter; the edit is
 * undoable and waits for Save like any other.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "fieldinfo.h"
#include "actions.h"
#include "palette.h"

enum { K_ACTION, K_CASE, K_PATCH, K_FIELD, K_KEY, K_SET };
typedef struct { int kind; int ref; char label[160], sub[200]; char file[256], path[256]; int score; } Item;

#define MAX_KEYS 6000
typedef struct { char file[256], path[256], key[96]; } Key;
static Key *KEYS; static int nKeys;
static char keysFor[PL_PATH];
static char query[256];
static Item *IT; static int nIt, capIt;
static int sel;
static UiScroll sc;

/* ---------------------------------------------------------------- the keyword index of the open case */
static void index_dict(FDFile *f, FDEntry *d, const char *rel, const char *prefix, int depth)
{
    for (int i = 0; i < d->nChild && nKeys < MAX_KEYS; i++) {
        FDEntry *e = &d->child[i];
        if (e->kind == FD_DIRECTIVE || !e->key || e->key[0] == '#') continue;
        char path[256];
        if (prefix[0]) snprintf(path, sizeof path, "%s/%s", prefix, e->key); else snprintf(path, sizeof path, "%s", e->key);
        if (!strcmp(path, "FoamFile") || pl_starts_with(path, "FoamFile/")) continue;
        if (e->kind == FD_DICT) { if (depth < 4) index_dict(f, e, rel, path, depth + 1); continue; }
        Key *k = &KEYS[nKeys++];
        pl_strlcpy(k->file, rel, sizeof k->file);
        pl_strlcpy(k->path, path, sizeof k->path);
        pl_strlcpy(k->key, e->key, sizeof k->key);
    }
}

static void index_dir(Case *c, const char *dir)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->s.path, dir);
    NameList l = { 0 };
    pl_list_dir(p, &l, false, true);
    names_sort(&l);
    for (int i = 0; i < l.n && nKeys < MAX_KEYS; i++) {
        const char *n = l.names[i];
        if (n[0] == '.' || pl_ends_with(n, ".orig") || pl_ends_with(n, "~") || pl_ends_with(n, ".gz")) continue;
        char rel[256]; snprintf(rel, sizeof rel, "%s/%s", dir, n);
        char fp[PL_PATH]; pl_join(fp, sizeof fp, c->s.path, rel);
        if (pl_file_size(fp) > 2 << 20) continue;              /* big fields: lists, not settings */
        FDFile *f = case_dict(c, rel);
        if (f && f->ok && !f->binary) index_dict(f, &f->root, rel, "", 0);
    }
    names_free(&l);
}

static void build_index(void)
{
    if (!A.cs) { nKeys = 0; keysFor[0] = 0; return; }
    if (!KEYS) KEYS = malloc(sizeof *KEYS * MAX_KEYS);
    nKeys = 0;
    pl_strlcpy(keysFor, A.cs->s.path, sizeof keysFor);
    index_dir(A.cs, "system");
    index_dir(A.cs, "constant");
    index_dir(A.cs, A.cs->fieldDir[0] ? A.cs->fieldDir : "0");
}

/* ---------------------------------------------------------------- matching */
/* higher is better, 0 no match: whole word > prefix > substring > letters in order */
static int score(const char *hay, const char *q)
{
    if (!q[0]) return 1;
    size_t n = strlen(q);
    if (!strcasecmp(hay, q)) return 1000;
    if (!strncasecmp(hay, q, n)) return 800 - (int)strlen(hay);
    const char *p = strcasestr(hay, q);
    if (p) return 600 - (int)(p - hay) - (int)strlen(hay) / 4;
    const char *h = hay; int gaps = 0;                    /* "dt" finds deltaT */
    for (const char *c = q; *c; c++) {
        const char *f = h;
        while (*f && tolower((unsigned char)*f) != tolower((unsigned char)*c)) f++;
        if (!*f) return 0;
        gaps += (int)(f - h); h = f + 1;
    }
    return 300 - gaps * 4 > 20 ? 300 - gaps * 4 : 20;
}

static void add(int kind, int ref, int sc_, const char *label, const char *sub, const char *file, const char *path)
{
    if (sc_ <= 0) return;
    if (nIt == capIt) {
        int nc = capIt ? capIt * 2 : 256;
        Item *ni = realloc(IT, sizeof *IT * (size_t)nc);
        if (!ni) return;
        IT = ni; capIt = nc;
    }
    Item *it = &IT[nIt++];
    memset(it, 0, sizeof *it);
    it->kind = kind; it->ref = ref; it->score = sc_;
    pl_strlcpy(it->label, label, sizeof it->label);
    pl_strlcpy(it->sub, sub ? sub : "", sizeof it->sub);
    pl_strlcpy(it->file, file ? file : "", sizeof it->file);
    pl_strlcpy(it->path, path ? path : "", sizeof it->path);
}

static int cmp_item(const void *a, const void *b) { return ((const Item *)b)->score - ((const Item *)a)->score; }

/* "x = v": the key part's last component matches a key, and the whole key part matches the end of file/path */
static bool assign_split(const char *q, char *lhs, int lc, char *rhs, int rc)
{
    const char *eq = strchr(q, '=');
    if (!eq) return false;
    snprintf(lhs, (size_t)lc, "%.*s", (int)(eq - q), q); pl_trim_inplace(lhs);
    snprintf(rhs, (size_t)rc, "%s", eq + 1); pl_trim_inplace(rhs);
    size_t n = strlen(rhs); if (n && rhs[n - 1] == ';') { rhs[n - 1] = 0; pl_trim_inplace(rhs); }
    return lhs[0] && rhs[0] && !strchr(lhs, ' ');
}

static bool key_matches(const Key *k, const char *lhs)
{
    const char *last = strrchr(lhs, '/'); last = last ? last + 1 : lhs;
    if (strcmp(k->key, last)) return false;
    if (last == lhs) return true;
    char full[520]; snprintf(full, sizeof full, "%s/%s", k->file, k->path);
    size_t L = strlen(full), n = strlen(lhs);
    return n <= L && !strcmp(full + L - n, lhs) && (n == L || full[L - n - 1] == '/');
}

static void search(void)
{
    nIt = 0;
    if (A.cs && strcmp(keysFor, A.cs->s.path)) build_index();
    if (!A.cs) nKeys = 0;
    char lhs[256], rhs[256];
    if (A.cs && assign_split(query, lhs, sizeof lhs, rhs, sizeof rhs)) {
        for (int i = 0; i < nKeys; i++) {
            if (!key_matches(&KEYS[i], lhs)) continue;
            FDFile *f = case_dict(A.cs, KEYS[i].file);
            char old[200] = "";
            if (f) fd_get(f, KEYS[i].path, old, sizeof old);
            char lab[300], sub[300];
            pl_fmt_ui(lab, sizeof lab, "Set %s = %s", KEYS[i].path, rhs);
            char cls[64] = ""; if (f) fd_get(f, "FoamFile/class", cls, sizeof cls);
            const char *why = value_problem(KEYS[i].path, old, rhs, cls);
            if (why) pl_fmt_ui(sub, sizeof sub, "%s: can't set it to %s. %s", KEYS[i].file, rhs, why);
            else pl_fmt_ui(sub, sizeof sub, "%s: %s -> %s  (Enter applies; undo with Ctrl+Z, keep with Save)", KEYS[i].file, old, rhs);
            add(K_SET, i, 2000, lab, sub, KEYS[i].file, KEYS[i].path);
        }
        if (nIt == 0) {
            add(K_SET, -1, 2000, "No entry by that name in this case", "Keywords are matched by name, or by the end of file/path: PIMPLE/nCorrectors, controlDict/endTime", NULL, NULL);
        }
        return;
    }
    for (int i = 0; i < actions_count(); i++) {
        const Action *a = actions_get(i);
        if (!a->label) continue;
        int s = score(a->label, query);
        if (!s && a->help && query[0] && strcasestr(a->help, query)) s = 150;      /* words in its help, not scattered letters */
        const char *why = action_blocked(a);
        char sub[200];
        pl_fmt_ui(sub, sizeof sub, "%s%s%s", why ? why : (a->help ? a->help : ""), a->shortcut ? "   " : "", a->shortcut ? a->shortcut : "");
        add(K_ACTION, i, s ? s + (why ? 0 : 50) + 100 : 0, a->label, sub, NULL, NULL);
    }
    for (int i = 0; i < A.nList; i++) {
        char sub[300]; pl_fmt_ui(sub, sizeof sub, "Case · %s", A.list[i].app);
        add(K_CASE, i, query[0] ? score(A.list[i].name, query) : 0, A.list[i].name, sub, NULL, NULL);
    }
    if (A.cs) {
        for (int i = 0; i < A.cs->nPatches; i++) {
            char sub[200]; pl_fmt_ui(sub, sizeof sub, "Patch · %s · its conditions on Boundaries", A.cs->patches[i].type);
            add(K_PATCH, i, query[0] ? score(A.cs->patches[i].name, query) : 0, A.cs->patches[i].name, sub, NULL, NULL);
        }
        for (int i = 0; i < A.cs->fields.n; i++) {
            char sub[200]; pl_fmt_ui(sub, sizeof sub, "Field · %s/%s", A.cs->fieldDir, A.cs->fields.names[i]);
            add(K_FIELD, i, query[0] ? score(A.cs->fields.names[i], query) : 0, A.cs->fields.names[i], sub, NULL, NULL);
        }
        if (query[0]) for (int i = 0; i < nKeys; i++) {
            int s = score(KEYS[i].key, query);
            if (!s) continue;
            FDFile *f = case_dict(A.cs, KEYS[i].file);
            char v[120] = ""; if (f) fd_get(f, KEYS[i].path, v, sizeof v);
            char lab[300], sub[400];
            pl_fmt_ui(lab, sizeof lab, "%s = %s", KEYS[i].path, v);
            pl_fmt_ui(sub, sizeof sub, "%s · type \"%s = ...\" to change it", KEYS[i].file, KEYS[i].key);
            add(K_KEY, i, s - 80, lab, sub, KEYS[i].file, KEYS[i].path);
        }
    }
    qsort(IT, (size_t)nIt, sizeof *IT, cmp_item);
    if (nIt > 200) nIt = 200;
}

/* ---------------------------------------------------------------- doing it */
static void run_item(const Item *it)
{
    switch (it->kind) {
    case K_ACTION: A.modal = MD_NONE; action_run(actions_get(it->ref), false); break;
    case K_CASE: A.modal = MD_NONE; app_open_case(A.list[it->ref].path, PG_OVERVIEW); break;
    case K_PATCH: A.modal = MD_NONE; A.bcPatch = it->ref; A.bcField = BC_INSPECT; app_go(PG_BC); break;
    case K_FIELD: A.modal = MD_NONE; A.bcField = it->ref; A.bcPatch = -1; app_go(PG_BC); break;
    case K_KEY:                                           /* complete it into an assignment to edit */
        pl_fmt_ui(query, sizeof query, "%s/%s = ", strrchr(it->file, '/') ? strrchr(it->file, '/') + 1 : it->file, it->path);
        ui_focus_input(ui_id("pal-in"));
        break;
    case K_SET: {
        if (it->ref < 0) break;
        char lhs[256], rhs[256];
        if (!assign_split(query, lhs, sizeof lhs, rhs, sizeof rhs)) break;
        FDFile *f = case_dict(A.cs, it->file);
        if (!f || f->binary) { ui_toast(TOAST_ERR, "%s can't be edited here", it->file); break; }
        char old[200] = ""; fd_get(f, it->path, old, sizeof old);
        if (app_put_value(f, it->path, rhs)) {
            case_rescan(A.cs);
            ui_toast(TOAST_OK, "%s: %s -> %s in %s (not saved yet)", it->path, old, rhs, it->file);
            A.modal = MD_NONE;
        }                                                 /* else app_put_value said why */
        break;
    }
    }
}

void palette_open(void)
{
    A.modal = MD_PALETTE;
    query[0] = 0; sel = 0; sc = (UiScroll){ 0 };
    keysFor[0] = 0;                                       /* the dictionaries may have changed: index them again */
    ui_focus_input(ui_id("pal-in"));
}

void palette_draw(bool *close)
{
    Rectangle b = ui_modal_begin(term(T_PALETTE), "Commands, pages, cases, patches, fields and keywords. \"deltaT = 0.001\" sets a value.", 820, 560, close);
    UiId in = ui_id("pal-in");
    static char last[256] = "\x01";
    ui_input(in, (Rectangle){ b.x, b.y, b.width, 40 }, query, sizeof query, TH.retro == RETRO_ALIEN ? "INQUIRY:" : "Type a command, a page, a patch, or deltaT = 0.001",
             INPUT_LIVE | INPUT_SEARCH | INPUT_MONO | INPUT_KEEP);
    if (strcmp(last, query) || (A.cs && strcmp(keysFor, A.cs->s.path))) { search(); sel = 0; pl_strlcpy(last, query, sizeof last); }
    if (inp_key_pressed(KEY_ESCAPE)) { *close = true; last[0] = 1; return; }
    if (inp_key_repeat(KEY_DOWN) && sel < nIt - 1) sel++;
    if (inp_key_repeat(KEY_UP) && sel > 0) sel--;
    float rowH = 46;
    Rectangle list = { b.x, b.y + 50, b.width, b.height - 50 };
    if (ui_input_entered(in) && sel < nIt) { Item it = IT[sel]; run_item(&it); last[0] = 1; if (A.modal != MD_PALETTE) return; }
    /* keep the selection in view */
    float top0 = sel * rowH;
    if (top0 < sc.target) sc.target = sc.y = top0;
    if (top0 + rowH > sc.target + list.height) sc.target = sc.y = top0 + rowH - list.height;
    float top = ui_scroll_begin(&sc, list);
    for (int i = 0; i < nIt; i++) {
        float y = top + i * rowH;
        if (y + rowH < list.y || y > list.y + list.height) continue;
        const Item *it = &IT[i];
        Rectangle r = { list.x, y, list.width - 10, rowH - 4 };
        bool blocked = it->kind == K_ACTION && action_blocked(actions_get(it->ref));
        if (i == sel) ui_rect(r, 8, TH.selected); else if (ui_hovered(r)) ui_rect(r, 8, TH.hover);
        const char *ic = it->kind == K_ACTION ? ICON_ZAP : it->kind == K_CASE ? ICON_FOLDER : it->kind == K_PATCH ? ICON_LAYERS :
                         it->kind == K_FIELD ? ICON_WIND : it->kind == K_KEY ? ICON_BRACES : ICON_PENCIL;
        ui_icon(ic, r.x + 16, r.y + r.height / 2, 15, blocked ? TH.text3 : TH.accent);
        ui_text_clip(it->kind == K_KEY || it->kind == K_SET ? F_MONO : F_MED, 13.5f, it->label, r.x + 36, r.y + 5, r.width - 50, blocked ? TH.text3 : TH.text);
        ui_text_clip(F_REG, 12, it->sub, r.x + 36, r.y + 24, r.width - 50, TH.text3);
        ui_register(ui_idn(ui_id("pal-item"), i), r, it->label);
        if (ui_clicked(r)) { sel = i; Item cp = *it; run_item(&cp); last[0] = 1; if (A.modal != MD_PALETTE) break; }
    }
    if (!nIt) ui_text(F_REG, 13, "Nothing matches.", list.x + 12, list.y + 10, TH.text3);
    ui_scroll_end(&sc, nIt * rowH);
}
