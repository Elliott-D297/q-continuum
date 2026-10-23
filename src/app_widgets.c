/* app_widgets.c -- page furniture and controls bound to dictionary entries */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "fieldinfo.h"
#include "program.h"

/* ============================================================================ furniture */
static unsigned str_hash(const char *s) { unsigned h = 5381; while (s && *s) h = h * 33 + (unsigned char)*s++; return h; }
Color lc_for(const char *key) { return TH.lc[str_hash(key) % 8]; }

float page_header(float x, float y, float w, const char *title, const char *subtitle)
{
    if (TH.lcars) {
        ui_text_clip(F_SEMI, 34, ui_upper(title), x, y - 4, w, TH.accent);      /* a long case name ends in "..." (H-21) */
        float h = 40;
        if (subtitle && *subtitle) h += ui_text_wrap(F_REG, 15, subtitle, (Rectangle){ x, y + 40, fminf(w, 760), 200 }, 22, TH.text2, true);
        return h + 22;
    }
    ui_text_clip(F_SEMI, 26, title, x, y, w, TH.text);
    float h = 40;
    if (subtitle && *subtitle) h += ui_text_wrap(F_REG, 14, subtitle, (Rectangle){ x, y + 40, fminf(w, 760), 200 }, 21, TH.text2, true);
    return h + 22;
}

float section_head_r(float x, float y, float w, const char *title, const char *desc, const char *right)
{
    float rw = right && *right ? ui_text_w(F_MONO, 12, right) + 14 : 0;
    if (rw) ui_text(F_MONO, 12, right, x + w - rw + 14, y + 4, TH.text3);
    return section_head(x, y, w - rw, title, desc);
}

float section_head(float x, float y, float w, const char *title, const char *desc)
{
    if (TH.lcars) {
        /* (▌ TITLE ═══════════════) : a cap, the title, then a bar running to the edge */
        Color c = lc_for(title);
        const char *T = ui_upper(title);
        ui_rect((Rectangle){ x, y + 1, 26, 20 }, 10, c);
        DrawRectangleRec((Rectangle){ x + 16, y + 1, 10, 20 }, c);
        float tw = ui_text_w(F_MED, 18, T);
        ui_text(F_MED, 18, T, x + 34, y + 1, c);
        float bx = x + 34 + tw + 10;
        if (bx < x + w - 20) {
            DrawRectangleRec((Rectangle){ bx, y + 8, x + w - bx - 6, 6 }, ui_alpha(c, 0.8f));
            ui_rect((Rectangle){ x + w - 12, y + 8, 12, 6 }, 3, ui_alpha(c, 0.8f));
        }
        float h = 30;
        if (desc && *desc) h += ui_text_wrap(F_REG, 14, desc, (Rectangle){ x, y + 30, fminf(w, 720), 200 }, 20, TH.text3, true) + 2;
        return h + 8;
    }
    ui_text(F_SEMI, 16, title, x, y, TH.text);
    float h = 26;
    if (desc && *desc) h += ui_text_wrap(F_REG, 13, desc, (Rectangle){ x, y + 25, fminf(w, 720), 200 }, 19, TH.text3, true) + 2;
    return h + 8;
}

Rectangle prop_row(float x, float *y, float w, const char *label, const char *hint, float ctrlH)
{
    float lw = fminf(300, w * 0.38f);
    float lh = 20;
    if (hint && *hint) lh += ui_text_wrap(F_REG, 12.5f, hint, (Rectangle){ 0, 0, lw - 24, 400 }, 17, TH.text3, false);
    float rowH = fmaxf(ctrlH, lh) + 22;
    if (TH.lcars) ui_text(F_MED, 15, ui_cap_prose(label), x, *y + 11, TH.text);
    else ui_text(F_MED, 13.5f, label, x, *y + 12, TH.text);
    if (hint && *hint) ui_text_wrap(F_REG, 12.5f, hint, (Rectangle){ x, *y + 33, lw - 24, 400 }, 17, TH.text3, true);
    Rectangle c = { x + lw, *y + 11, w - lw, ctrlH };
    ui_divider(x, *y + rowH - 1, w);
    *y += rowH;
    return c;
}

float callout_height(float w, const char *title, const char *text)
{
    float h = 18;
    if (title && *title) h += 20;
    if (text && *text) h += ui_text_wrap(F_REG, 13, text, (Rectangle){ 0, 0, w - 64, 999 }, 19, TH.text2, false);
    return h + 4;
}

void callout(Rectangle r, int kind, const char *icon, const char *title, const char *text)
{
    Color c = kind == TOAST_OK ? TH.ok : kind == TOAST_WARN ? TH.warn : kind == TOAST_ERR ? TH.err : TH.info;
    Color bg = kind == TOAST_OK ? TH.okSoft : kind == TOAST_WARN ? TH.warnSoft : kind == TOAST_ERR ? TH.errSoft : TH.infoSoft;
    if (TH.lcars) {
        ui_rect(r, 16, bg);
        ui_rect((Rectangle){ r.x, r.y, 26, r.height }, 13, c);
        DrawRectangleRec((Rectangle){ r.x + 13, r.y, 13, r.height }, c);
        DrawRectangleRec((Rectangle){ r.x + 26, r.y, 4, r.height }, BLACK);
        float ty = r.y + 11;
        if (title && *title) { ui_text(F_MED, 15, ui_cap_words(title), r.x + 42, ty, c); ty += 21; }
        if (text && *text) ui_text_wrap(F_REG, 13, text, (Rectangle){ r.x + 42, ty, r.width - 64, 999 }, 19, TH.text, true);
        return;
    }
    ui_rect(r, 9, bg);
    ui_rect_lines(r, 9, 1, ui_alpha(c, 0.35f));
    ui_icon(icon ? icon : ICON_INFO, r.x + 22, r.y + 21, 16, c);
    float ty = r.y + 11;
    if (title && *title) { ui_text(F_SEMI, 13.5f, ui_cap_words(title), r.x + 42, ty, TH.text); ty += 21; }
    if (text && *text) ui_text_wrap(F_REG, 13, text, (Rectangle){ r.x + 42, ty, r.width - 64, 999 }, 19, TH.text2, true);
}

Color cat_color(Category c) { return TH.cat[c >= 0 && c < 5 ? c : 4]; }
const char *cat_icon(Category c)
{
    switch (c) {
    case CAT_INCOMP: return ICON_WIND;
    case CAT_HEAT: return ICON_FLAME;
    case CAT_MULTI: return ICON_DROPLETS;
    case CAT_COMP: return ICON_ZAP;
    default: return ICON_BOX;
    }
}
Color status_color(CaseStatus s)
{
    switch (s) {
    case CS_RUNNING: return TH.accent;
    case CS_DONE: return TH.ok;
    case CS_FAILED: return TH.err;
    case CS_STOPPED: case CS_PAUSED: return TH.warn;
    case CS_QUEUED: return TH.info;
    case CS_MESHED: return TH.info;
    default: return TH.text3;
    }
}

float stat_tile(Rectangle r, const char *icon, const char *label, const char *value, const char *sub)
{
    if (TH.lcars) {
        /* a coloured block on the left like an LCARS readout, figures to its right */
        Color c = lc_for(label);
        ui_rect((Rectangle){ r.x, r.y, 30, r.height }, 15, c);
        DrawRectangleRec((Rectangle){ r.x + 15, r.y, 15, r.height }, c);
        DrawRectangleRec((Rectangle){ r.x + 34, r.y, r.width - 34, 6 }, c);
        ui_rect((Rectangle){ r.x + r.width - 12, r.y, 12, 6 }, 3, c);
        ui_text(F_MED, 14, ui_upper(label), r.x + 44, r.y + 16, c);
        ui_text_clip(F_SEMI, 28, value, r.x + 44, r.y + 38, r.width - 52, TH.text);
        if (sub && *sub) ui_text_clip(F_REG, 13, sub, r.x + 44, r.y + r.height - 22, r.width - 52, TH.text3);
        return r.height;
    }
    ui_rect(r, 10, TH.surface);
    ui_rect_lines(r, 10, 1, TH.lineSoft);
    ui_icon(icon, r.x + 26, r.y + 26, 16, TH.text3);
    ui_text(F_MED, 12.5f, label, r.x + 42, r.y + 18, TH.text3);
    ui_text_clip(F_SEMI, 21, value, r.x + 18, r.y + 44, r.width - 36, TH.text);
    if (sub && *sub) ui_text_clip(F_REG, 12.5f, sub, r.x + 18, r.y + 74, r.width - 36, TH.text3);
    return r.height;
}

/* LCARS panel: dark field with a coloured cap down its left side and a rail along the top */
void lc_panel(Rectangle r, Color c, float hov, bool sel)
{
    ui_rect(r, 16, ui_mix(TH.surface, TH.hover, hov));
    ui_rect((Rectangle){ r.x, r.y, 24, r.height }, 12, c);
    DrawRectangleRec((Rectangle){ r.x + 12, r.y, 12, r.height }, c);
    DrawRectangleRec((Rectangle){ r.x + 24, r.y, 4, r.height }, TH.bg);
    DrawRectangleRec((Rectangle){ r.x + 28, r.y, r.width - 28 - 16, 4 }, ui_alpha(c, 0.75f));
    ui_rect((Rectangle){ r.x + r.width - 24, r.y, 24, 4 }, 2, ui_alpha(c, 0.75f));
    if (sel) ui_rect_lines(r, 16, 2, TH.accent);
}

Color head_color(void) { return TH.lcars ? TH.accent : TH.text; }

/* ============================================================================ bound controls */
/* a dictionary-bound control's id: "dict:<file relative to the case>:<key path>", e.g. dict:system/controlDict:deltaT.
   Stable across frames and runs, so test scripts (and the command bar) can find the control for an entry. */
static UiId path_id(FDFile *f, const char *path)
{
    const char *rel = f->path;
    if (A.cs) { size_t n = strlen(A.cs->s.path); if (!strncmp(rel, A.cs->s.path, n) && rel[n] == '/') rel += n + 1; }
    char b[PL_PATH + 1100]; snprintf(b, sizeof b, "dict:%s:%.1000s", rel, path);
    return ui_id(b);
}

/* every typed value goes through here: refused with the reason when it would break the file or the case (H-01, H-07) */
bool app_put_value(FDFile *f, const char *path, const char *nv)
{
    char old[1024] = "", cls[64] = "";
    fd_get(f, path, old, sizeof old);
    fd_get(f, "FoamFile/class", cls, sizeof cls);
    const char *why = value_problem(path, old, nv, cls);
    if (why) {
        const char *k = strrchr(path, '/');
        ui_toast(TOAST_ERR, "%s not changed: %s", k ? k + 1 : path, why);
        return false;
    }
    return fd_put(f, path, nv);
}

bool dv_text(FDFile *f, const char *path, Rectangle r, int flags, const char *ph)
{
    char v[1024] = "";
    bool has = fd_get(f, path, v, sizeof v);
    if (ui_input(path_id(f, path), r, v, sizeof v, has ? NULL : ph, flags | INPUT_MONO)) {
        pl_trim_inplace(v);
        if (!v[0] && !has) return false;
        return v[0] ? app_put_value(f, path, v) : fd_put(f, path, "\"\"");
    }
    return false;
}

bool dv_number(FDFile *f, const char *path, Rectangle r, const char *def)
{
    char v[1024] = "";
    bool has = fd_get(f, path, v, sizeof v);
    int a = 0, b = 0;
    bool num = has && fd_number_span(v, &a, &b);
    char shown[1024];
    if (num) snprintf(shown, sizeof shown, "%.*s", b - a, v + a);
    else snprintf(shown, sizeof shown, "%s", v);
    UiId id = path_id(f, path);
    float sw = 0;
    if (num && (a > 0 || v[b])) {                               /* show units / surrounding text */
        char rest[256];
        snprintf(rest, sizeof rest, "%.*s%s", a, v, v + b);
        pl_trim_inplace(rest);
        if (rest[0]) {
            sw = fminf(ui_text_w(F_MONO, 12, rest) + 14, r.width * 0.5f);
            ui_text_clip(F_MONO, 12, rest, r.x + r.width - sw + 6, r.y + (r.height - 12) / 2 - 1, sw - 8, TH.text3);
        }
    }
    Rectangle ir = { r.x, r.y, r.width - sw, r.height };
    if (ui_input(id, ir, shown, sizeof shown, has ? NULL : def, INPUT_MONO | INPUT_NUMERIC)) {
        pl_trim_inplace(shown);
        if (!shown[0]) return false;
        if (num) {
            char nv[1200];
            snprintf(nv, sizeof nv, "%.*s%s%s", a, v, shown, v + b);
            return app_put_value(f, path, nv);
        }
        return app_put_value(f, path, shown);
    }
    return false;
}

bool dv_choice(FDFile *f, const char *path, Rectangle r, const char *const *items, int n, bool custom, const char *ph)
{
    char v[512] = "", out[256];
    fd_get(f, path, v, sizeof v);
    if (ui_dropdown(path_id(f, path), r, v, items, n, DD_FILTER | (custom ? DD_CUSTOM : 0) | DD_MONO, out, sizeof out, ph)) {
        if (strcmp(out, v) != 0) return custom ? app_put_value(f, path, out) : fd_put(f, path, out);
    }
    return false;
}

static int bool_word(const char *v)
{
    char w[32]; fd_word(v, w, sizeof w);
    if (!strcmp(w, "yes") || !strcmp(w, "on") || !strcmp(w, "true") || !strcmp(w, "1")) return 1;
    if (!strcmp(w, "no") || !strcmp(w, "off") || !strcmp(w, "false") || !strcmp(w, "0") || !strcmp(w, "none")) return 0;
    return -1;
}

static const char *flip_word(const char *v, bool on)
{
    char w[32]; fd_word(v, w, sizeof w);
    if (!strcmp(w, "on") || !strcmp(w, "off")) return on ? "on" : "off";
    if (!strcmp(w, "true") || !strcmp(w, "false")) return on ? "true" : "false";
    if (!strcmp(w, "1") || !strcmp(w, "0")) return on ? "1" : "0";
    return on ? "yes" : "no";
}

bool dv_switch(FDFile *f, const char *path, float x, float y, const char *defaultOff)
{
    char v[64] = "";
    bool has = fd_get(f, path, v, sizeof v);
    int b = has ? bool_word(v) : -1;
    bool on = b == 1;
    if (!has && defaultOff) snprintf(v, sizeof v, "%s", defaultOff);
    if (ui_switch(path_id(f, path), x, y, &on)) return fd_put(f, path, flip_word(v, on));
    ui_text(F_MONO, 12.5f, has ? v : "not set", x + 44, y + 2, TH.text3);
    return false;
}

/* one-line summary of a long / list value */
static void summarize(const char *v, char *out, int cap)
{
    int k = 0;
    bool sp = false;
    for (const char *p = v; *p && k < cap - 1; p++) {
        if (isspace((unsigned char)*p)) { if (!sp && k) { out[k++] = ' '; sp = true; } continue; }
        out[k++] = *p; sp = false;
    }
    out[k] = 0;
}

bool dv_value_editor(FDFile *f, const char *path, const char *key, const char *ctx, Rectangle r)
{
    char v[1024] = "";
    FDEntry *e = fd_find(f, path);
    if (!e) return false;
    int len = e->valEnd - e->valStart;
    if (len >= (int)sizeof v || (len > 0 && memchr(f->text + e->valStart, '\n', (size_t)len))) {
        char big[400];
        int n = len < 399 ? len : 399;
        memcpy(big, f->text + e->valStart, (size_t)n); big[n] = 0;
        char sum[400]; summarize(big, sum, sizeof sum);
        int lines = 1;
        for (int i = e->valStart; i < e->valEnd; i++) if (f->text[i] == '\n') lines++;
        char lab[64]; snprintf(lab, sizeof lab, "%d lines", lines);
        float bw = ui_badge_width(NULL, lab);
        Rectangle ir = { r.x, r.y, r.width - bw - 44, r.height };
        ui_input(path_id(f, path), ir, sum, sizeof sum, NULL, INPUT_MONO | INPUT_READONLY);
        ui_badge(ir.x + ir.width + 8, r.y + (r.height - 22) / 2, NULL, lab, TH.text3, TH.lineSoft);
        Rectangle eb = { r.x + r.width - 30, r.y + (r.height - 28) / 2, 28, 28 };
        if (ui_button(ui_idn(path_id(f, path), 3), eb, ICON_EXTERNAL_LINK, NULL, BTN_ICON)) launch_editor(&A.env, &A.set, f->path);
        ui_tooltip(eb, "Lists and multi-line values are edited in your text editor");
        return false;
    }
    fd_entry_value(f, e, v, sizeof v);
    int bw = bool_word(v);
    char w[64]; fd_word(v, w, sizeof w);
    const char *const *items = NULL;
    int n = suggest_for_key(key, ctx, &items);
    bool isBoolKey = n > 0 && items && (!strcmp(items[0], "yes") || !strcmp(items[0], "on"));
    /* 0 and 1 are numbers (startTime 0, nCorrectors 1) unless the key is known to be a switch */
    bool numeric = !strcmp(w, "0") || !strcmp(w, "1");
    if (bw >= 0 && (isBoolKey || (n == 0 && !numeric)) && strlen(v) <= 5) return dv_switch(f, path, r.x, r.y + (r.height - 20) / 2, NULL);
    if (n > 0) return dv_choice(f, path, r, items, n, true, NULL);
    /* a number, or a dimensioned scalar ([0 2 -1 0 0 0 0] 1e-05): a number field, which replaces the value when you
       type (a text field appended: kappa 80 + "160" became 80160). Vectors and lists stay text: only their last
       number would be edited. */
    char t[1024]; pl_strlcpy(t, v, sizeof t); pl_trim_inplace(t);
    const char *close = strrchr(t, ']');
    bool dimScalar = close && !strchr(t, '(') && pl_is_number(close + 1 + strspn(close + 1, " \t"));
    if (pl_is_number(t) || dimScalar) return dv_number(f, path, r, NULL);
    return dv_text(f, path, r, 0, NULL);
}

/* ============================================================================ tree editor */
bool tree_expanded(UiId id, bool def)
{
    for (int i = 0; i < A.nExpanded; i++) if (A.expanded[i] == id) return A.expandedState[i];
    return def;
}
void tree_toggle(UiId id, bool def)
{
    for (int i = 0; i < A.nExpanded; i++) if (A.expanded[i] == id) { A.expandedState[i] = !A.expandedState[i]; return; }
    if (A.nExpanded < 1024) { A.expanded[A.nExpanded] = id; A.expandedState[A.nExpanded] = !def; A.nExpanded++; }
}

float dict_tree(FDFile *f, FDEntry *dict, const char *prefix, float x, float y, float w, int depth, bool *edited)
{
    const float rowH = 38, ind = 22;
    float y0 = y;
    float keyW = fminf(300, w * 0.36f);
    for (int i = 0; i < dict->nChild && !*edited; i++) {
        FDEntry *e = &dict->child[i];
        if (depth == 0 && !strcmp(e->key, "FoamFile")) continue;
        char path[600];
        if (prefix[0]) snprintf(path, sizeof path, "%s/%s", prefix, e->key); else snprintf(path, sizeof path, "%s", e->key);
        float ix = x + depth * ind;
        Rectangle row = { x - 8, y, w + 16, rowH };
        bool hov = ui_hovered(row);
        if (e->kind == FD_DIRECTIVE) {
            char v[300]; snprintf(v, sizeof v, "%s %.*s", e->key, e->valEnd - e->valStart < 250 ? e->valEnd - e->valStart : 250, f->text + e->valStart);
            char s[300]; summarize(v, s, sizeof s);
            ui_icon(ICON_FILE_CODE, ix + 8, y + rowH / 2, 14, TH.text3);
            ui_text_clip(F_MONO, 12.5f, s, ix + 24, y + 11, w - (ix - x) - 30, TH.text3);
            y += rowH;
            continue;
        }
        if (e->kind == FD_DICT) {
            UiId id = path_id(f, path);
            bool open = tree_expanded(id, depth < 1);
            if (ui_row(id, (Rectangle){ ix - 6, y + 3, w - (ix - x) + 12, rowH - 6 }, false)) tree_toggle(id, depth < 1);
            ui_icon(open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, ix + 7, y + rowH / 2, 14, TH.text3);
            ui_text(F_MONO_MED, 13, e->key, ix + 22, y + 11, TH.text);
            char cnt[32]; snprintf(cnt, sizeof cnt, "%d", e->nChild);
            ui_text(F_REG, 12, cnt, ix + 30 + ui_text_w(F_MONO_MED, 13, e->key), y + 12, TH.text3);
            y += rowH;
            if (open) {
                y += dict_tree(f, e, path, x, y, w, depth + 1, edited);
                if (*edited) return y - y0;
                Rectangle add = { ix + 22, y + 2, ui_text_w(F_MED, 12.5f, "Add entry") + 30, 26 };
                bool ah = ui_hovered(add);
                if (ah) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
                ui_icon(ICON_PLUS, add.x + 8, add.y + 13, 13, ah ? TH.accent : TH.text3);
                ui_text(F_MED, 12.5f, "Add entry", add.x + 20, add.y + 5, ah ? TH.accent : TH.text3);
                if (ui_clicked(add)) {
                    A.modal = MD_ADD_ENTRY; A.mFlag = false;
                    snprintf(A.mPath, sizeof A.mPath, "%s", path);
                    snprintf(A.mFile, sizeof A.mFile, "%s", f->path);
                    A.mText[0] = A.mText2[0] = A.mErr[0] = 0;
                }
                y += 32;
            }
            continue;
        }
        /* value entry */
        const char *hint = hint_for_key(e->key);
        ui_text_clip(F_MONO, 13, e->key, ix + 22, y + 11, keyW - (ix - x) - 22, TH.text2);
        if (hint) ui_tooltip((Rectangle){ ix + 22, y, keyW - (ix - x) - 22, rowH }, hint);
        Rectangle vr = { x + keyW, y + 4, w - keyW - 34, rowH - 8 };
        if (dv_value_editor(f, path, e->key, prefix, vr)) { *edited = true; return y - y0; }
        if (hov) {
            Rectangle del = { x + w - 28, y + 6, 26, 26 };
            if (ui_button(path_id(f, path) ^ 0x5a5a, del, ICON_TRASH_2, NULL, BTN_ICON)) {
                char key[128];
                snprintf(key, sizeof key, "%s", e->key);              /* e is invalid after the edit */
                fd_remove(f, path);
                *edited = true;
                ui_toast(TOAST_INFO, "Removed %s (not saved yet)", key);
                return y - y0;
            }
            ui_tooltip(del, "Remove this entry");
        }
        y += rowH;
    }
    return y - y0;
}

/* Multi-region cases: pick the region the page edits. Returns the height used (0 for single-region cases). */
float region_bar(float x, float y, float w)
{
    Case *c = A.cs;
    if (!c || c->s.nRegions == 0) return 0;
    const char *lab0 = ui_cap("Region");
    ui_text(F_MED, 13, lab0, x, y + 6, TH.text2);
    float lx = x + ui_text_w(F_MED, 13, lab0) + 14, ly = y, h = 30;
    for (int k = -1; k < c->s.nRegions; k++) {
        const char *lab = k < 0 ? ui_cap("Base mesh") : c->s.regions[k];          /* region names keep their case */
        bool on = k < 0 ? !c->region[0] : !strcmp(c->region, c->s.regions[k]);
        float bw = ui_text_w(F_MED, 13, lab) + 28;
        if (lx + bw > x + w) { lx = x + ui_text_w(F_MED, 13, lab0) + 14; ly += h + 6; }
        Rectangle pill = { lx, ly, bw, h };
        char rid[96]; snprintf(rid, sizeof rid, "region:%s", k < 0 ? "" : c->s.regions[k]);
        ui_register(ui_id(rid), pill, lab);
        bool hov = ui_hovered(pill);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        Color oc = TH.lcars ? lc_for(k < 0 ? "base" : c->s.regions[k]) : TH.accent;
        ui_rect(pill, h / 2, on ? oc : hov ? TH.hover : BLANK);
        if (!on) ui_rect_lines(pill, h / 2, 1, TH.line);
        ui_text(F_MED, 13, lab, lx + 14, ly + 6, on ? TH.textOnAccent : TH.text2);
        if (k < 0) ui_tooltip(pill, "constant/polyMesh and the fields before splitMeshRegions");
        if (ui_clicked(pill) && !on) {
            case_set_region(c, k < 0 ? "" : c->s.regions[k]);
            A.bcField = A.bcPatch = -1;
        }
        lx += bw + 8;
    }
    return ly - y + h + 18;
}

/* Region fields in 0/<region> are written by splitMeshRegions (and changeDictionary): say so where they are edited */
float region_generated_note(float x, float y, float w)
{
    Case *c = A.cs;
    if (!c || !c->region[0] || pl_starts_with(c->fieldDir, "0.orig")) return 0;
    char body[600], cd[200], p[PL_PATH];
    snprintf(cd, sizeof cd, "system/%s/changeDictionaryDict", c->region);
    pl_join(p, sizeof p, c->s.path, cd);
    bool hasCD = pl_is_file(p);
    snprintf(body, sizeof body, "%s/ is written by splitMeshRegions from the base fields%s. A run from the start writes it again, "
             "so edits here last until then. For lasting changes edit %s%s.", c->fieldDir,
             hasCD ? ", then changed by changeDictionary" : "", hasCD ? cd : "the base-mesh fields",
             hasCD ? " (Files page) or the base-mesh fields" : ", or keep per-region fields in 0.orig/<region>");
    float ch = callout_height(w, "Generated fields", body);
    callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "Generated fields", body);
    return ch + 18;
}

/* ============================================================================ the other programs */
const char *prog_icon(int prog)
{
    switch (prog) {
    case PROG_DEME: return ICON_ORBIT;   case PROG_CHRONO: return ICON_FAN;     case PROG_CALCULIX: return ICON_BOX;
    case PROG_ASTER: return ICON_HASH;   case PROG_SATURNE: return ICON_WIND;   case PROG_MOOSE: return ICON_ATOM;
    case PROG_ELMER: return ICON_ZAP;    case PROG_SPARTA: return ICON_ROCKET;  default: return ICON_DROPLETS;
    }
}

Color prog_color(int prog)
{
    if (TH.lcars) { static const int K[PROG_COUNT] = { 1, 4, 6, 2, 7, 5, 3, 0, 5 }; return TH.lc[K[prog >= 0 && prog < PROG_COUNT ? prog : 0]]; }
    return TH.series[(prog + 3) % 10];
}
