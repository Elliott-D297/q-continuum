/* page_review.c -- ROADMAP 3.5: nothing is written without being seen first. Save, Ctrl+S, Run with unsaved edits and the
 * other programs' Setup page open this review: each file that would change, a line per changed entry ("deltaT:
 * 0.005 -> 0.0025") and the diff itself, then the theme's word for go (LCARS "Make it so", Stardew "Ship it" ...).
 * Settings > Runs turns it off. The diff is textdiff.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "textdiff.h"

#define MAX_ITEMS 32

typedef struct {
    char  name[256];
    char *before, *after;
    Diff  d;
    char  sum[12][160]; int nSum;
    bool  setup;                       /* the other programs' Setup file (page_prog.c), not a dictionary */
} Item;

static struct { Item it[MAX_ITEMS]; int n; int sel; int then; UiScroll sc, ls; } R;

static void clear(void)
{
    for (int i = 0; i < R.n; i++) { free(R.it[i].before); free(R.it[i].after); diff_free(&R.it[i].d); }
    R.n = 0;
}

static void add(const char *name, const char *before, const char *after, bool setup)
{
    if (R.n >= MAX_ITEMS || !before || !after || !strcmp(before, after)) return;
    Item *it = &R.it[R.n++];
    memset(it, 0, sizeof *it);
    pl_strlcpy(it->name, name, sizeof it->name);
    it->before = pl_strdup(before); it->after = pl_strdup(after); it->setup = setup;
    diff_texts(it->before, it->after, 3, &it->d);
    it->nSum = diff_summary(&it->d, it->sum, 12);
}

/* the edits waiting: every dirty dictionary of the open case, the other programs' Setup file */
static int collect(void)
{
    clear();
    if (!A.cs) return 0;
    for (int i = 0; i < A.cs->nFiles; i++) {
        FDFile *f = A.cs->files[i].f;
        if (f->dirty) add(A.cs->files[i].rel, f->base ? f->base : "", f->text, false);
    }
    const char *nm, *b, *a;
    if (prog_setup_pending(&nm, &b, &a)) add(nm, b, a, true);
    R.sel = 0;
    return R.n;
}

bool app_review_needed(void) { return A.cs && A.set.reviewSave && (case_dirty_count(A.cs) > 0 || prog_setup_dirty()); }

void review_open(int then)
{
    if (!collect()) { if (then) A.runPending = true; return; }
    R.then = then;
    R.sc = (UiScroll){ 0 }; R.ls = (UiScroll){ 0 };
    A.modal = MD_REVIEW;
    app_sound(SND_CLICK);
}

/* Save, Ctrl+S: the review first when it is on; true when written now */
bool app_save_request(void)
{
    if (!A.cs) return true;
    if (app_review_needed()) { review_open(0); return false; }
    return app_save_all(false);
}

static void line_row(const DiffLine *l, float x, float y, float w)
{
    Color bg = l->kind == '+' ? TH.okSoft : l->kind == '-' ? TH.errSoft : BLANK;
    if (bg.a) ui_rect((Rectangle){ x, y, w, 20 }, 3, bg);
    if (l->kind == '@') { ui_text(F_MONO, 12, "...", x + 8, y + 3, TH.text3); return; }
    char n[16];
    if (l->aLine) { snprintf(n, sizeof n, "%d", l->aLine); ui_text(F_MONO, 11.5f, n, x + 40 - ui_text_w(F_MONO, 11.5f, n), y + 4, TH.text3); }
    if (l->bLine) { snprintf(n, sizeof n, "%d", l->bLine); ui_text(F_MONO, 11.5f, n, x + 84 - ui_text_w(F_MONO, 11.5f, n), y + 4, TH.text3); }
    char k[2] = { l->kind == ' ' ? ' ' : l->kind, 0 };
    Color c = l->kind == '+' ? TH.ok : l->kind == '-' ? TH.err : TH.text2;
    ui_text(F_MONO_MED, 12.5f, k, x + 94, y + 3, c);
    char t[400]; int len = l->len < 399 ? l->len : 399;
    int j = 0; for (int i = 0; i < len; i++) t[j++] = l->s[i] == '\t' ? ' ' : l->s[i];
    t[j] = 0;
    ui_text_clip(F_MONO, 12.5f, t, x + 110, y + 3, w - 118, l->kind == ' ' ? TH.text2 : TH.text);
}

void review_modal(bool *close)
{
    char sub[200];
    int lines = 0; for (int i = 0; i < R.n; i++) lines += R.it[i].d.added + R.it[i].d.removed;
    pl_fmt_ui(sub, sizeof sub, "%d file%s change%s, %d line%s. Nothing is written until you say so.", R.n, R.n == 1 ? "" : "s",
              R.n == 1 ? "s" : "", lines, lines == 1 ? "" : "s");
    Rectangle b = ui_modal_begin(term(T_REVIEW), sub, 1000, 640, close);
    if (!R.n) { *close = true; return; }
    if (R.sel >= R.n) R.sel = 0;
    /* left: the files and what changes in each */
    float lw = 330;
    Rectangle lr = { b.x, b.y, lw, b.height - 56 };
    float y = ui_scroll_begin(&R.ls, lr);
    float y0 = y;
    for (int i = 0; i < R.n; i++) {
        Item *it = &R.it[i];
        float h = 34 + 20 * (float)(it->nSum ? it->nSum : 1);
        Rectangle row = { lr.x, y, lw - 12, h };
        if (ui_row(ui_idn(ui_id("rv-file"), i), row, i == R.sel)) { R.sel = i; R.sc = (UiScroll){ 0 }; }
        ui_register(ui_idn(ui_id("rv-file"), i), row, it->name);
        ui_icon(ICON_FILE_TEXT, row.x + 16, row.y + 16, 14, TH.text3);
        ui_text_clip(F_MONO_MED, 13, it->name, row.x + 32, row.y + 8, row.width - 90, TH.text);
        char c[32]; snprintf(c, sizeof c, "+%d -%d", it->d.added, it->d.removed);
        ui_text(F_MONO, 12, c, row.x + row.width - 10 - ui_text_w(F_MONO, 12, c), row.y + 9, TH.text3);
        float sy = row.y + 32;
        for (int k = 0; k < it->nSum; k++) { ui_text_clip(F_REG, 12.5f, it->sum[k], row.x + 32, sy, row.width - 40, TH.text2); sy += 20; }
        if (!it->nSum) ui_text(F_REG, 12.5f, "layout or comments", row.x + 32, sy, TH.text3);
        y += h + 6;
    }
    ui_scroll_end(&R.ls, y - y0);
    /* right: the diff of the file picked */
    Rectangle dr = { b.x + lw + 10, b.y, b.width - lw - 10, b.height - 56 };
    ui_rect(dr, 10, TH.chartBg);
    ui_rect_lines(dr, 10, 1, TH.lineSoft);
    Item *it = &R.it[R.sel];
    float dy = ui_scroll_begin(&R.sc, ui_inset(dr, 6, 8));
    float d0 = dy;
    for (int i = 0; i < it->d.n; i++) { line_row(&it->d.l[i], dr.x + 6, dy, dr.width - 12); dy += 21; }
    ui_scroll_end(&R.sc, dy - d0 + 8);
    /* go, keep editing, throw away */
    float by = b.y + b.height - 38;
    const char *go = term(T_REVIEW_GO);
    char gl[64]; pl_fmt_ui(gl, sizeof gl, "%s%s", go, R.then ? " and run" : "");
    float gw = ui_button_width(ICON_CHECK, gl) + 16;
    if (ui_button(ui_id("rv-go"), (Rectangle){ b.x + b.width - gw, by, gw, 38 }, ICON_CHECK, gl, BTN_PRIMARY) || (inp_key_pressed(KEY_ENTER) && inp_key_down(KEY_LEFT_CONTROL))) {
        bool ok = app_save_all(false);
        int then = R.then;
        clear();
        *close = true;
        if (ok && then) { A.runPending = true; app_go(PG_RUN); }
        return;
    }
    float kw = ui_button_width(NULL, "Keep editing");
    if (ui_button(ui_id("rv-keep"), (Rectangle){ b.x + b.width - gw - kw - 10, by, kw, 38 }, NULL, "Keep editing", BTN_SECONDARY)) { clear(); *close = true; return; }
    float xw = ui_button_width(ICON_ROTATE_CCW, "Discard these changes");
    if (ui_button(ui_id("rv-discard"), (Rectangle){ b.x, by, xw, 38 }, ICON_ROTATE_CCW, "Discard these changes", BTN_DANGER)) {
        for (int i = 0; i < A.cs->nFiles; i++) if (A.cs->files[i].f->dirty) fd_reload(A.cs->files[i].f);
        prog_setup_revert();
        ui_toast(TOAST_INFO, "Changes discarded: the files are as they were on disk");
        clear();
        *close = true;
    }
}
