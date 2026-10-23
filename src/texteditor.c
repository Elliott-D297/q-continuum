/* texteditor.c -- the Files page's text editor: OpenFOAM syntax colours, selection, clipboard, undo/redo,
 * find/replace, comment toggling. It edits FDFile text directly (fd_set_text), so the Entries view and every
 * page bound to the file see the change at once, and saving keeps the usual atomic write.
 *
 * Keys: arrows (+Shift select, +Ctrl by word), Home/End, PgUp/PgDn, Ctrl+A/C/X/V, Ctrl+Z, Ctrl+Y or
 * Ctrl+Shift+Z, Ctrl+F find, F3 / Shift+F3 next/previous match, Ctrl+/ comment lines, Ctrl+D duplicate line,
 * Tab / Shift+Tab indent, Enter keeps the indentation.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

#define TABW 4
#define MAX_UNDO 200

typedef struct { char *text; int cur, anc; } Snap;

static struct {
    char      path[PL_PATH];
    int       cur, anc;                 /* byte offsets: caret and selection anchor */
    int       wantCol;                  /* visual column kept by up/down            */
    bool      focused, dragging;
    UiScroll  sc;
    float     hx;                       /* horizontal scroll (px)                   */
    Snap      undo[MAX_UNDO], redo[MAX_UNDO];
    int       nUndo, nRedo;
    double    lastEdit; int lastKind;   /* typing is grouped into one undo step     */
    int      *ls; unsigned char *inCom; int nl, capl;   /* line starts, "inside a block comment" at line start */
    const char *idxText; int idxLen; time_t idxMtime;
    bool      findOpen; char find[128], repl[128];
    bool      scrollToCaret;
} E;

/* ============================================================================ index */
static void build_index(const FDFile *f)
{
    E.nl = 0;
    const char *s = f->text;
    bool com = false;
    for (int i = 0, start = 0;; i++) {
        if (i == 0 || s[i - 1] == '\n') {
            if (E.nl == E.capl) {
                E.capl = E.capl ? E.capl * 2 : 1024;
                E.ls = realloc(E.ls, sizeof(int) * (size_t)E.capl);
                E.inCom = realloc(E.inCom, (size_t)E.capl);
            }
            E.ls[E.nl] = i; E.inCom[E.nl] = com; E.nl++;
            start = i;
        }
        (void)start;
        if (i >= f->len) break;
        if (com) { if (s[i] == '*' && s[i + 1] == '/') { com = false; i++; } continue; }
        if (s[i] == '/' && s[i + 1] == '/') { while (i < f->len && s[i + 1] != '\n') i++; continue; }
        if (s[i] == '/' && s[i + 1] == '*') { com = true; i++; continue; }
        if (s[i] == '"') { i++; while (i < f->len && s[i] != '"' && s[i] != '\n') { if (s[i] == '\\') i++; i++; } }
    }
    E.idxText = f->text; E.idxLen = f->len; E.idxMtime = f->mtime;
}

static int line_of(int pos)
{
    int lo = 0, hi = E.nl - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (E.ls[m] <= pos) lo = m; else hi = m - 1; }
    return lo;
}
static int line_end(const FDFile *f, int ln) { return ln + 1 < E.nl ? E.ls[ln + 1] - 1 : f->len; }

static int vcol(const FDFile *f, int ln, int pos)          /* visual column of pos (tabs expanded) */
{
    int c = 0;
    for (int i = E.ls[ln]; i < pos && i < f->len; i++) c = f->text[i] == '\t' ? (c / TABW + 1) * TABW : c + 1;
    return c;
}
static int pos_at_col(const FDFile *f, int ln, int col)
{
    int c = 0, i = E.ls[ln], e = line_end(f, ln);
    for (; i < e; i++) {
        int nc = f->text[i] == '\t' ? (c / TABW + 1) * TABW : c + 1;
        if (nc > col) { if (col - c > nc - col) i++; break; }   /* nearer edge */
        c = nc;
    }
    return i;
}

/* ============================================================================ editing */
static void snap_free(Snap *s) { free(s->text); s->text = NULL; }
static void push_undo(const FDFile *f)
{
    if (E.nUndo == MAX_UNDO) { snap_free(&E.undo[0]); memmove(E.undo, E.undo + 1, sizeof(Snap) * (MAX_UNDO - 1)); E.nUndo--; }
    E.undo[E.nUndo++] = (Snap){ pl_strdup(f->text), E.cur, E.anc };
    for (int i = 0; i < E.nRedo; i++) snap_free(&E.redo[i]);
    E.nRedo = 0;
}

/* kind: 1 typing, 2 deleting, 0 anything else (always its own undo step) */
static void replace(FDFile *f, int a, int b, const char *ins, int kind)
{
    if (f->binary) return;
    if (a > b) { int t = a; a = b; b = t; }
    double now = ui_time();
    if (!(kind && kind == E.lastKind && now - E.lastEdit < 1.5)) push_undo(f);
    E.lastKind = kind; E.lastEdit = now;
    size_t il = strlen(ins), nl = (size_t)f->len - (size_t)(b - a) + il;
    char *nt = malloc(nl + 1);
    memcpy(nt, f->text, (size_t)a);
    memcpy(nt + a, ins, il);
    memcpy(nt + a + il, f->text + b, (size_t)(f->len - b));
    nt[nl] = 0;
    fd_set_text(f, nt);
    free(nt);
    E.cur = E.anc = a + (int)il;
    build_index(f);
    E.scrollToCaret = true;
}

static void restore(FDFile *f, Snap *from, int *nFrom, Snap *to, int *nTo)
{
    if (!*nFrom) return;
    Snap s = from[--*nFrom];
    if (*nTo == MAX_UNDO) { snap_free(&to[0]); memmove(to, to + 1, sizeof(Snap) * (MAX_UNDO - 1)); (*nTo)--; }
    to[(*nTo)++] = (Snap){ pl_strdup(f->text), E.cur, E.anc };
    fd_set_text(f, s.text);
    E.cur = s.cur < f->len ? s.cur : f->len; E.anc = s.anc < f->len ? s.anc : f->len;
    free(s.text);
    E.lastKind = 0;
    build_index(f);
    E.scrollToCaret = true;
}

static bool has_sel(void) { return E.cur != E.anc; }
static int sel_a(void) { return E.cur < E.anc ? E.cur : E.anc; }
static int sel_b(void) { return E.cur < E.anc ? E.anc : E.cur; }
static bool word_ch(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.'; }

/* prefix every selected line with s (or remove it); used for indent and comments */
static void lines_prefix(FDFile *f, const char *pre, bool remove)
{
    int l0 = line_of(sel_a()), l1 = line_of(sel_b() > sel_a() && sel_b() == E.ls[line_of(sel_b())] ? sel_b() - 1 : sel_b());
    Str b; str_init(&b);
    int a = E.ls[l0], e = line_end(f, l1);
    size_t pl = strlen(pre);
    for (int ln = l0; ln <= l1; ln++) {
        int s0 = E.ls[ln], s1 = line_end(f, ln);
        const char *t = f->text + s0;
        if (remove) {
            int k = 0;
            if (!strncmp(pre, "//", 2)) { while (k < s1 - s0 && (t[k] == ' ' || t[k] == '\t')) k++; }
            if ((size_t)(s1 - s0 - k) >= pl - (pre[pl - 1] == ' ' ? 1 : 0) && !strncmp(t + k, pre, pl - (pre[pl - 1] == ' ' ? 1 : 0))) {
                str_catn(&b, t, (size_t)k);
                int skip = (int)pl - (pre[pl - 1] == ' ' ? 1 : 0);
                if (pre[pl - 1] == ' ' && t[k + skip] == ' ') skip++;
                str_catn(&b, t + k + skip, (size_t)(s1 - s0 - k - skip));
            } else if (!strcmp(pre, "    ")) {                         /* outdent: up to 4 spaces or one tab */
                int k2 = 0; while (k2 < 4 && k2 < s1 - s0 && t[k2] == ' ') k2++;
                if (!k2 && s1 > s0 && t[0] == '\t') k2 = 1;
                str_catn(&b, t + k2, (size_t)(s1 - s0 - k2));
            } else str_catn(&b, t, (size_t)(s1 - s0));
        } else {
            if (s1 > s0 || l0 == l1) str_cat(&b, pre);
            str_catn(&b, t, (size_t)(s1 - s0));
        }
        if (ln < l1) str_cat(&b, "\n");
    }
    char *txt = b.s ? b.s : pl_strdup("");
    replace(f, a, e, txt, 0);
    E.anc = a; E.cur = a + (int)strlen(txt);
    if (b.s) str_free(&b); else free(txt);
}

static bool all_commented(const FDFile *f)
{
    int l0 = line_of(sel_a()), l1 = line_of(sel_b());
    for (int ln = l0; ln <= l1; ln++) {
        const char *t = f->text + E.ls[ln]; int n = line_end(f, ln) - E.ls[ln], k = 0;
        while (k < n && (t[k] == ' ' || t[k] == '\t')) k++;
        if (k < n && strncmp(t + k, "//", 2)) return false;
    }
    return true;
}

static int find_next(const FDFile *f, int from, bool back)
{
    size_t n = strlen(E.find);
    if (!n) return -1;
    if (!back) {
        const char *p = strstr(f->text + (from < f->len ? from : f->len), E.find);
        if (!p) p = strstr(f->text, E.find);                 /* wrap */
        return p ? (int)(p - f->text) : -1;
    }
    int best = -1;
    for (const char *p = strstr(f->text, E.find); p; p = strstr(p + 1, E.find)) {
        if (p - f->text < from) best = (int)(p - f->text);
        else if (best < 0) best = (int)(p - f->text);     /* wrap to the last one */
    }
    if (best >= 0 && best >= from) for (const char *p = strstr(f->text, E.find); p; p = strstr(p + 1, E.find)) best = (int)(p - f->text);
    return best;
}

static void select_match(int at) { if (at >= 0) { E.anc = at; E.cur = at + (int)strlen(E.find); E.scrollToCaret = true; } }

/* ============================================================================ drawing */
typedef enum { K_TEXT, K_COMMENT, K_DIRECTIVE, K_MACRO, K_STRING, K_NUMBER, K_KEY, K_PUNCT } Kind;

static Color kind_color(Kind k)
{
    switch (k) {
    case K_COMMENT: return TH.text3;
    case K_DIRECTIVE: return TH.cat[3];
    case K_MACRO: return TH.cat[1];
    case K_STRING: return TH.cat[2];
    case K_NUMBER: return TH.cat[4];
    case K_KEY: return TH.lcars ? TH.accent : TH.cat[0];
    case K_PUNCT: return TH.text2;
    default: return TH.text;
    }
}

/* colour one line: kinds[] per byte */
static bool shellMode;                                   /* scripts: '#' comments, no dictionary keys */

static void classify(const char *s, int n, bool inCom, unsigned char *kinds)
{
    int i = 0;
    if (shellMode) inCom = false;
    bool keySeen = false;
    while (i < n) {
        if (inCom) {
            int st = i;
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) i++;
            if (i < n) { i += 2; inCom = false; }
            memset(kinds + st, K_COMMENT, (size_t)(i - st));
            continue;
        }
        char c = s[i];
        if (shellMode && c == '#' && (i == 0 || isspace((unsigned char)s[i - 1]))) { memset(kinds + i, K_COMMENT, (size_t)(n - i)); return; }
        if (!shellMode && c == '/' && i + 1 < n && s[i + 1] == '/') { memset(kinds + i, K_COMMENT, (size_t)(n - i)); return; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') { inCom = true; kinds[i] = kinds[i + 1] = K_COMMENT; i += 2; continue; }
        if (c == '"') {
            int st = i++;
            while (i < n && s[i] != '"') { if (s[i] == '\\') i++; i++; }
            if (i < n) i++;
            memset(kinds + st, K_STRING, (size_t)(i - st));
            keySeen = true;
            continue;
        }
        if ((c == '#' && !shellMode) || c == '$') {
            int st = i++;
            if (c == '$' && i < n && s[i] == '{') { int d = 0; for (; i < n; i++) { if (s[i] == '{') d++; else if (s[i] == '}' && --d == 0) { i++; break; } } }
            else while (i < n && (word_ch(s[i]) || s[i] == '/' || s[i] == ':' || s[i] == '{' || s[i] == '}' || (c == '#' && s[i] == '#'))) i++;
            memset(kinds + st, c == '#' ? K_DIRECTIVE : K_MACRO, (size_t)(i - st));
            keySeen = true;
            continue;
        }
        if (strchr("{}();[]", c)) { kinds[i++] = K_PUNCT; if (c == ';' || c == '{' || c == '}') keySeen = false; continue; }
        if (isspace((unsigned char)c)) { kinds[i++] = K_TEXT; continue; }
        int st = i;
        bool num = isdigit((unsigned char)c) || ((c == '-' || c == '+' || c == '.') && i + 1 < n && (isdigit((unsigned char)s[i + 1]) || s[i + 1] == '.'));
        while (i < n && !isspace((unsigned char)s[i]) && !strchr(";{}\"[]", s[i]) && !(s[i] == ')' && !num) && !(s[i] == '(' && num)) {
            if (s[i] == '(') { int d = 0; for (; i < n; i++) { if (s[i] == '(') d++; else if (s[i] == ')' && --d == 0) { i++; break; } } break; }
            i++;
        }
        if (i == st) i++;
        Kind k = num ? K_NUMBER : !keySeen && !shellMode ? K_KEY : K_TEXT;
        memset(kinds + st, k, (size_t)(i - st));
        keySeen = true;
    }
}

/* returns true when the text changed this frame */
static double drawnAt;
static int gotoLine;
void text_editor_goto(int line) { gotoLine = line; }
bool text_editor(FDFile *f, Rectangle r)
{
    drawnAt = GetTime();
    if (strcmp(E.path, f->path)) {                                  /* another file: fresh state */
        for (int i = 0; i < E.nUndo; i++) snap_free(&E.undo[i]);
        for (int i = 0; i < E.nRedo; i++) snap_free(&E.redo[i]);
        E.nUndo = E.nRedo = 0;
        pl_strlcpy(E.path, f->path, sizeof E.path);
        E.cur = E.anc = 0; E.sc = (UiScroll){ 0 }; E.hx = 0; E.focused = false; E.lastKind = 0;
        E.idxText = NULL;
    }
    shellMode = !strstr(f->text, "FoamFile");
    if (E.idxText != f->text || E.idxLen != f->len || E.idxMtime != f->mtime) build_index(f);   /* reloaded or edited elsewhere */
    if (E.cur > f->len) E.cur = f->len;
    if (E.anc > f->len) E.anc = f->len;
    bool ro = f->binary;
    int before = E.nUndo + E.nRedo * 1000;
    const char *before_text = f->text; int before_len = f->len;

    /* ---- find bar */
    float barH = E.findOpen ? 44 : 0;
    if (E.findOpen) {
        Rectangle fb = { r.x, r.y, r.width, 38 };
        float fx = fb.x + 6;
        if (ui_input(ui_id("te-find"), (Rectangle){ fx, fb.y + 2, 220, 34 }, E.find, sizeof E.find, "Find", INPUT_MONO | INPUT_SEARCH)) select_match(find_next(f, sel_a(), false));
        fx += 228;
        if (ui_button(ui_id("te-prev"), (Rectangle){ fx, fb.y + 2, 34, 34 }, ICON_CHEVRON_LEFT, NULL, BTN_SECONDARY)) select_match(find_next(f, sel_a(), true));
        if (ui_button(ui_id("te-next"), (Rectangle){ fx + 40, fb.y + 2, 34, 34 }, ICON_CHEVRON_RIGHT, NULL, BTN_SECONDARY)) select_match(find_next(f, sel_b(), false));
        fx += 84;
        if (!ro) {
            ui_input(ui_id("te-repl"), (Rectangle){ fx, fb.y + 2, 200, 34 }, E.repl, sizeof E.repl, "Replace with", INPUT_MONO);
            fx += 208;
            bool selIsHit = E.find[0] && sel_b() - sel_a() == (int)strlen(E.find) && !strncmp(f->text + sel_a(), E.find, strlen(E.find));
            float bw = ui_button_width(NULL, "Replace");
            if (ui_button(ui_id("te-rep1"), (Rectangle){ fx, fb.y + 2, bw, 34 }, NULL, "Replace", BTN_SECONDARY) && E.find[0]) {
                if (selIsHit) { int a = sel_a(); replace(f, a, sel_b(), E.repl, 0); select_match(find_next(f, a + (int)strlen(E.repl), false)); }
                else select_match(find_next(f, sel_a(), false));
            }
            fx += bw + 6;
            float bw2 = ui_button_width(NULL, "All");
            if (ui_button(ui_id("te-repall"), (Rectangle){ fx, fb.y + 2, bw2, 34 }, NULL, "All", BTN_SECONDARY) && E.find[0]) {
                Str b; str_init(&b); int n = 0; size_t fl = strlen(E.find);
                const char *p = f->text, *q;
                while ((q = strstr(p, E.find))) { str_catn(&b, p, (size_t)(q - p)); str_cat(&b, E.repl); p = q + fl; n++; }
                str_cat(&b, p);
                if (n) { replace(f, 0, f->len, b.s, 0); E.cur = E.anc = 0; }
                str_free(&b);
                ui_toast(TOAST_INFO, "Replaced %d occurrence%s", n, n == 1 ? "" : "s");
            }
            fx += bw2 + 6;
        }
        int hits = 0;
        if (E.find[0]) for (const char *p = strstr(f->text, E.find); p && hits < 100000; p = strstr(p + 1, E.find)) hits++;
        char ht[48]; snprintf(ht, sizeof ht, E.find[0] ? "%d match%s" : "", hits, hits == 1 ? "" : "es");
        ui_text(F_REG, 12.5f, ht, fx + 6, fb.y + 11, hits ? TH.text2 : TH.text3);
        if (ui_button(ui_id("te-findx"), (Rectangle){ fb.x + fb.width - 36, fb.y + 2, 34, 34 }, ICON_X, NULL, BTN_ICON)) E.findOpen = false;
    }
    Rectangle body = { r.x, r.y + barH, r.width, r.height - barH };
    ui_rect(body, 10, TH.input);
    ui_rect_lines(body, 10, 1, E.focused ? ui_alpha(TH.accent, 0.6f) : TH.lineSoft);
    Rectangle view = ui_inset(body, 2, 2);

    const float fs = 13, lh = 19;
    const float cw = ui_text_w(F_MONO, fs, "M");
    char nmax[16]; snprintf(nmax, sizeof nmax, "%d", E.nl);
    const float gutter = ui_text_w(F_MONO, 12, nmax) + 26;
    const float tx0 = view.x + gutter + 8;
    const float textW = view.width - gutter - 22;

    /* ---- mouse */
    Vector2 m = ui_mouse();
    bool inView = ui_hovered(view);
    if (inView) ui_cursor(MOUSE_CURSOR_IBEAM);
    float top0 = view.y - roundf(E.sc.y) + 8;                    /* y of line 0 (before this frame's scroll) */
    if (ui_pressed(view)) {
        E.focused = true;
        int ln = (int)floorf((m.y - top0) / lh); if (ln < 0) ln = 0; if (ln >= E.nl) ln = E.nl - 1;
        int p = pos_at_col(f, ln, (int)floorf((m.x - tx0 + E.hx) / cw + 0.5f));
        bool shift = ui_key_down(KEY_LEFT_SHIFT) || ui_key_down(KEY_RIGHT_SHIFT);
        if (ui_double_clicked()) {                                  /* select the word */
            int a = p, b = p;
            while (a > 0 && word_ch(f->text[a - 1])) a--;
            while (b < f->len && word_ch(f->text[b])) b++;
            E.anc = a; E.cur = b;
        } else { E.cur = p; if (!shift) E.anc = p; E.dragging = true; }
        E.wantCol = -1; E.lastKind = 0;
    } else if (ui_mouse_down() && E.dragging) {
        int ln = (int)floorf((m.y - top0) / lh); if (ln < 0) ln = 0; if (ln >= E.nl) ln = E.nl - 1;
        E.cur = pos_at_col(f, ln, (int)floorf((m.x - tx0 + E.hx) / cw + 0.5f));
        if (m.y < view.y + 4) E.sc.target -= lh; else if (m.y > view.y + view.height - 4) E.sc.target += lh;
    } else if (!ui_mouse_down()) E.dragging = false;
    if (!inView && ui_pressed((Rectangle){ 0, 0, ui_width(), ui_height() })) E.focused = false;
    if (ui_modal_active()) E.focused = false;

    /* ---- keyboard */
    if (E.focused && !ui_any_input_focused()) {
        bool ctrl = ui_key_down(KEY_LEFT_CONTROL) || ui_key_down(KEY_RIGHT_CONTROL);
        bool shift = ui_key_down(KEY_LEFT_SHIFT) || ui_key_down(KEY_RIGHT_SHIFT);
        int ln = line_of(E.cur);
        int move = -1;
        if (ui_key(KEY_LEFT)) {
            if (has_sel() && !shift) move = sel_a();
            else { move = E.cur > 0 ? E.cur - 1 : 0; if (ctrl) { while (move > 0 && !word_ch(f->text[move])) move--; while (move > 0 && word_ch(f->text[move - 1])) move--; } }
            E.wantCol = -1;
        }
        if (ui_key(KEY_RIGHT)) {
            if (has_sel() && !shift) move = sel_b();
            else { move = E.cur < f->len ? E.cur + 1 : f->len; if (ctrl) { while (move < f->len && !word_ch(f->text[move])) move++; while (move < f->len && word_ch(f->text[move])) move++; } }
            E.wantCol = -1;
        }
        int dl = ui_key(KEY_UP) ? -1 : ui_key(KEY_DOWN) ? 1 : ui_key(KEY_PAGE_UP) ? -(int)(view.height / lh - 2) : ui_key(KEY_PAGE_DOWN) ? (int)(view.height / lh - 2) : 0;
        if (dl) {
            if (E.wantCol < 0) E.wantCol = vcol(f, ln, E.cur);
            int nl2 = ln + dl; if (nl2 < 0) nl2 = 0; if (nl2 >= E.nl) nl2 = E.nl - 1;
            move = pos_at_col(f, nl2, E.wantCol);
        }
        if (ui_key(KEY_HOME)) {
            int a = E.ls[ln], k = a; while (k < f->len && (f->text[k] == ' ' || f->text[k] == '\t')) k++;
            move = ctrl ? 0 : (E.cur == k ? a : k);                 /* smart home: indentation first */
            E.wantCol = -1;
        }
        if (ui_key(KEY_END)) { move = ctrl ? f->len : line_end(f, ln); E.wantCol = -1; }
        if (move >= 0) { E.cur = move; if (!shift) E.anc = move; E.scrollToCaret = true; E.lastKind = 0; }

        if (ctrl && ui_key(KEY_A)) { E.anc = 0; E.cur = f->len; }
        if (ctrl && (ui_key(KEY_C) || ui_key(KEY_X)) && has_sel()) {
            char *t = pl_strndup(f->text + sel_a(), (size_t)(sel_b() - sel_a()));
            SetClipboardText(t); free(t);
            if (ui_key(KEY_X) && !ro) replace(f, sel_a(), sel_b(), "", 0);
        }
        if (ctrl && ui_key(KEY_F)) {
            E.findOpen = true;
            if (has_sel() && sel_b() - sel_a() < (int)sizeof E.find && !memchr(f->text + sel_a(), '\n', (size_t)(sel_b() - sel_a())))
                snprintf(E.find, sizeof E.find, "%.*s", sel_b() - sel_a(), f->text + sel_a());
        }
        if (ui_key(KEY_F3)) select_match(find_next(f, shift ? sel_a() : sel_b(), shift));
        if (!ro) {
            if (ctrl && ui_key(KEY_V)) { const char *cb = GetClipboardText(); if (cb && *cb) replace(f, sel_a(), sel_b(), cb, 0); }
            if (ctrl && (ui_key(KEY_Y) || (shift && ui_key(KEY_Z)))) restore(f, E.redo, &E.nRedo, E.undo, &E.nUndo);
            else if (ctrl && ui_key(KEY_Z)) restore(f, E.undo, &E.nUndo, E.redo, &E.nRedo);
            if (ctrl && ui_key(KEY_SLASH)) { bool c = all_commented(f); lines_prefix(f, "// ", c); }
            if (ctrl && ui_key(KEY_D)) {                            /* duplicate the line (or the selection) */
                if (has_sel()) { char *t = pl_strndup(f->text + sel_a(), (size_t)(sel_b() - sel_a())); int b = sel_b(); replace(f, b, b, t, 0); E.anc = b; free(t); }
                else { int a = E.ls[ln], e = line_end(f, ln); Str b; str_init(&b); str_cat(&b, "\n"); str_catn(&b, f->text + a, (size_t)(e - a)); int col = E.cur - a; replace(f, e, e, b.s, 0); E.cur = E.anc = e + 1 + col; str_free(&b); }
            }
            if (ui_key(KEY_TAB)) {
                if (has_sel() && line_of(sel_a()) != line_of(sel_b())) lines_prefix(f, "    ", shift);
                else if (shift) { E.anc = E.cur; lines_prefix(f, "    ", true); E.anc = E.cur; }
                else { int sp = TABW - vcol(f, ln, E.cur) % TABW; char t[8]; snprintf(t, sizeof t, "%.*s", sp, "        "); replace(f, sel_a(), sel_b(), t, 1); }
            }
            if (ui_key(KEY_ENTER) || ui_key(KEY_KP_ENTER)) {        /* keep the indentation; one level more after '{' */
                int a = E.ls[ln], k = a; while (k < E.cur && (f->text[k] == ' ' || f->text[k] == '\t')) k++;
                Str b; str_init(&b); str_cat(&b, "\n"); str_catn(&b, f->text + a, (size_t)(k - a));
                int p = sel_a() - 1; while (p >= a && f->text[p] == ' ') p--;
                if (p >= a && f->text[p] == '{') str_cat(&b, "    ");
                replace(f, sel_a(), sel_b(), b.s, 0);
                str_free(&b);
            }
            if (ui_key(KEY_BACKSPACE)) {
                if (has_sel()) replace(f, sel_a(), sel_b(), "", 2);
                else if (E.cur > 0) {
                    int a = E.cur - 1;
                    if (ctrl) { while (a > 0 && !word_ch(f->text[a])) a--; while (a > 0 && word_ch(f->text[a - 1])) a--; }
                    replace(f, a, E.cur, "", 2);
                }
            }
            if (ui_key(KEY_DELETE)) {
                if (has_sel()) replace(f, sel_a(), sel_b(), "", 2);
                else if (E.cur < f->len) replace(f, E.cur, E.cur + 1, "", 2);
            }
            int ch;
            char typed[64]; int tn = 0;
            while ((ch = ui_next_char()) > 0 && tn < 58) {
                if (ch < 32 || ctrl) continue;
                if (ch < 0x80) typed[tn++] = (char)ch;
                else if (ch < 0x800) { typed[tn++] = (char)(0xC0 | (ch >> 6)); typed[tn++] = (char)(0x80 | (ch & 0x3F)); }
                else { typed[tn++] = (char)(0xE0 | (ch >> 12)); typed[tn++] = (char)(0x80 | ((ch >> 6) & 0x3F)); typed[tn++] = (char)(0x80 | (ch & 0x3F)); }
            }
            typed[tn] = 0;
            if (tn) replace(f, sel_a(), sel_b(), typed, 1);
        }
    }

    /* ---- keep the caret visible */
    float contentH = E.nl * lh + 16;
    if (E.scrollToCaret) {
        E.scrollToCaret = false;
        int cl = line_of(E.cur);
        float cy = cl * lh + 8;
        if (cy < E.sc.target) E.sc.target = cy - lh;
        if (cy + lh * 2 > E.sc.target + view.height) E.sc.target = cy + lh * 2 - view.height;
        float maxY = fmaxf(0, contentH - view.height);
        if (E.sc.target < 0) E.sc.target = 0;
        if (E.sc.target > maxY) E.sc.target = maxY;
        float cx = vcol(f, cl, E.cur) * cw;
        if (cx < E.hx) E.hx = fmaxf(0, cx - cw * 8);
        if (cx > E.hx + textW - cw * 2) E.hx = cx - textW + cw * 8;
    }

    if (gotoLine > 0 && E.nl > 0) {                     /* an error's file:line: the line a third of the way down, cursor on it */
        int ln = gotoLine - 1 < E.nl ? gotoLine - 1 : E.nl - 1;
        E.cur = E.anc = E.ls[ln];
        float t = ln * lh - view.height / 3; if (t < 0) t = 0;
        E.sc.target = E.sc.y = t; E.hx = 0;
        gotoLine = 0;
    }
    /* ---- draw */
    float top = ui_scroll_begin(&E.sc, view) + 8;
    int first = (int)((view.y - top) / lh) - 1, last = first + (int)(view.height / lh) + 3;
    if (first < 0) first = 0;
    if (last > E.nl) last = E.nl;
    int sa = sel_a(), sb = sel_b(), cl = line_of(E.cur);
    unsigned char kinds[2048];
    char seg[2048];
    size_t fl = strlen(E.find);
    for (int ln = first; ln < last; ln++) {
        float y = top + ln * lh;
        int a = E.ls[ln], e = line_end(f, ln), n = e - a;
        if (ln == cl && E.focused) DrawRectangleRec((Rectangle){ view.x + gutter, y - 1, view.width - gutter, lh }, ui_alpha(TH.hover, 0.7f));
        char num[16]; snprintf(num, sizeof num, "%d", ln + 1);
        ui_text(F_MONO, 12, num, view.x + gutter - 10 - ui_text_w(F_MONO, 12, num), y + 1, ln == cl ? TH.text2 : TH.text3);
        ui_clip_push((Rectangle){ view.x + gutter, view.y, view.width - gutter, view.height });
        /* selection and find hits */
        if (sb > sa && sa <= e && sb >= a) {
            float x0 = tx0 - E.hx + vcol(f, ln, sa > a ? sa : a) * cw;
            float x1 = tx0 - E.hx + vcol(f, ln, sb < e ? sb : e) * cw + (sb > e ? cw * 0.5f : 0);
            DrawRectangleRec((Rectangle){ x0, y - 1, x1 - x0, lh }, ui_alpha(TH.accent, 0.28f));
        }
        if (fl && E.findOpen) {
            for (int k = a; k + (int)fl <= e; k++)
                if (!strncmp(f->text + k, E.find, fl)) {
                    float x0 = tx0 - E.hx + vcol(f, ln, k) * cw;
                    ui_rect_lines((Rectangle){ x0 - 1, y - 1, fl * cw + 2, lh }, 3, 1, TH.warn);
                }
        }
        /* text, by colour runs (tabs expanded) */
        int nn = n < (int)sizeof kinds ? n : (int)sizeof kinds - 1;
        classify(f->text + a, nn, E.inCom[ln], kinds);
        int col = 0, i = 0;
        while (i < nn) {
            Kind k = (Kind)kinds[i];
            int sl = 0, c0 = col;
            while (i < nn && kinds[i] == k && sl < (int)sizeof seg - 8) {
                char ch = f->text[a + i];
                if (ch == '\t') { int to = (col / TABW + 1) * TABW; while (col < to) { seg[sl++] = ' '; col++; } }
                else { seg[sl++] = ch; if (((unsigned char)ch & 0xC0) != 0x80) col++; }
                i++;
            }
            seg[sl] = 0;
            float x = tx0 - E.hx + c0 * cw;
            if (x < view.x + view.width && x + sl * cw > view.x) ui_text(F_MONO, fs, seg, x, y, kind_color(k));
        }
        /* caret */
        if (ln == cl && E.focused && fmod(ui_time() - E.lastEdit, 1.0) < 0.6) {
            float cx = tx0 - E.hx + vcol(f, ln, E.cur) * cw;
            DrawRectangleRec((Rectangle){ cx, y, 2, lh - 2 }, TH.accent);
        }
        ui_clip_pop();
    }
    ui_scroll_end(&E.sc, contentH);
    if (E.focused) ui_request_frames(2);                              /* blink */

    /* status line */
    char st[160];
    snprintf(st, sizeof st, "Ln %d, Col %d%s%s", cl + 1, vcol(f, cl, E.cur) + 1, ro ? "  ·  read-only (binary)" : "",
             has_sel() ? "  ·  selection" : "");
    ui_text(F_MONO, 11.5f, st, body.x + body.width - 14 - ui_text_w(F_MONO, 11.5f, st), body.y + body.height + 4, TH.text3);
    (void)before;
    return f->text != before_text || f->len != before_len;
}

void text_editor_open_find(void) { E.findOpen = true; }
/* the editor has the keyboard (it was drawn just now and clicked into): Ctrl+Z is its own undo, not the app's */
bool text_editor_focused(void) { return E.focused && GetTime() - drawnAt < 0.25; }
