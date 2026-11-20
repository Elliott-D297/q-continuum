/* teach.c -- glossary terms in the app's text, the Glossary dialog, the "show the working" panel (see teach.h) */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "glossary.h"
#include "teach.h"

/* ---------------------------------------------------------------- terms in text */
static struct { int cur; char q[64]; UiScroll list, body; int back[16], nBack; int lastCur; } G = { .cur = -1, .lastCur = -1 };

static void go_term(int i)
{
    if (i < 0 || i == G.cur) return;
    if (G.cur >= 0) {
        if (G.nBack == 16) { memmove(G.back, G.back + 1, sizeof G.back[0] * 15); G.nBack--; }
        G.back[G.nBack++] = G.cur;
    }
    G.cur = i;
}

static void term_hook(const char *key, Rectangle r)
{
    const GlossTerm *t = gloss_find(key);
    if (!t) return;
    char id[64]; snprintf(id, sizeof id, "term:%s", t->key);
    ui_register(ui_id(id), r, id);
    if (!ui_hovered(r)) return;
    ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    ui_rect(r, 3, ui_alpha(TH.lcars ? TH.lc[1] : TH.accent, 0.16f));
    char body[700];
    snprintf(body, sizeof body, "%s\n\n%s\n\nClick for the whole entry.", t->what, t->number);
    ui_tooltip_titled(r, t->name, body, 0.25f);
    if (ui_clicked(r) && (A.modal == MD_NONE || A.modal == MD_GLOSSARY)) teach_glossary_open(t->key);
}

void teach_init(void) { ui_term_hook = term_hook; }

void teach_glossary_open(const char *key)
{
    const GlossTerm *t = key ? gloss_find(key) : NULL;
    if (A.modal != MD_GLOSSARY) { G.nBack = 0; G.q[0] = 0; G.list = (UiScroll){ 0 }; if (t) G.cur = -1; }
    if (t) go_term(gloss_index(t));
    else if (G.cur < 0) G.cur = 0;
    A.modal = MD_GLOSSARY;
}

/* ---------------------------------------------------------------- the Glossary dialog */
static float entry_section(float x, float y, float w, const char *head, const char *text, UiFamily f)
{
    if (!text || !*text) return 0;
    float y0 = y;
    ui_text(F_MED, 12.5f, head, x, y, TH.text3);
    y += 20;
    y += ui_text_wrap(f, f == F_MONO ? 12.5f : 14, text, (Rectangle){ x, y, w, 2000 }, f == F_MONO ? 19 : 21, TH.text, true);
    return y - y0 + 14;
}

void teach_glossary_modal(bool *close)
{
    Rectangle b = ui_modal_begin("Glossary", "The words of CFD and meshing in fluid-mechanics terms. Underlined words "
                                 "anywhere in the app open here.", 960, 660, close);
    float lw = b.width < 700 ? floorf(b.width * 0.34f) : 250;
    UiId in = ui_id("gl-q");
    int idx[128];
    char was[64]; snprintf(was, sizeof was, "%s", G.q);
    ui_input(in, (Rectangle){ b.x, b.y, lw, 34 }, G.q, sizeof G.q, "Search", INPUT_LIVE | INPUT_SEARCH);
    int n = gloss_search(G.q, idx, 128);
    if (strcmp(was, G.q)) { G.list.target = G.list.y = 0; if (n) go_term(idx[0]); }
    if (ui_input_entered(in) && n) go_term(idx[0]);
    if (G.cur < 0) G.cur = n ? idx[0] : 0;

    Rectangle lv = { b.x, b.y + 44, lw, b.height - 44 };
    float top = ui_scroll_begin(&G.list, lv), rh = 30;
    for (int i = 0; i < n; i++) {
        Rectangle row = { lv.x, top + i * rh, lw - 12, rh - 2 };
        if (ui_culled(row.y, row.y + row.height)) continue;
        const GlossTerm *t = gloss_at(idx[i]);
        if (ui_row(ui_idn(ui_id("gl-row"), idx[i]), row, idx[i] == G.cur)) go_term(idx[i]);
        ui_text_clip(F_REG, 13.5f, t->name, row.x + 10, row.y + 7, row.width - 16, idx[i] == G.cur ? TH.text : TH.text2);
    }
    if (!n) ui_text(F_REG, 13, "Nothing matches.", lv.x + 10, lv.y + 8, TH.text3);
    ui_scroll_end(&G.list, n * rh);

    const GlossTerm *t = gloss_at(G.cur);
    if (!t) return;
    if (G.cur != G.lastCur) { G.body.target = G.body.y = 0; G.lastCur = G.cur; }
    Rectangle ev = { b.x + lw + 20, b.y, b.width - lw - 20, b.height };
    ui_line(ev.x - 10, ev.y, ev.x - 10, ev.y + ev.height, 1, TH.lineSoft);
    float y = ui_scroll_begin(&G.body, ev), y0 = y, x = ev.x + 4, w = ev.width - 22;
    if (G.nBack) {
        float bw = ui_button_width(ICON_CHEVRON_LEFT, "Back");
        if (ui_button(ui_id("gl-back"), (Rectangle){ x + w - bw, y, bw, 30 }, ICON_CHEVRON_LEFT, "Back", BTN_GHOST)) {
            G.cur = G.back[--G.nBack];
        }
    }
    ui_text_wrap(F_SEMI, 20, t->name, (Rectangle){ x, y + 2, w - 100, 80 }, 26, TH.text, true);
    y += ui_text_wrap(F_SEMI, 20, t->name, (Rectangle){ x, y + 2, w - 100, 80 }, 26, TH.text, false) + 14;
    y += ui_text_wrap(F_REG, 15, t->what, (Rectangle){ x, y, w, 2000 }, 23, TH.text, true) + 16;
    y += entry_section(x, y, w, "PICTURE IT", t->picture, F_REG);
    y += entry_section(x, y, w, "NUMBERS TO KNOW", t->number, F_REG);
    y += entry_section(x, y, w, "IN OPENFOAM", t->foam, F_MONO);
    char k[48];
    if (gloss_see(t, 0, k, sizeof k)) {
        ui_text(F_MED, 12.5f, "SEE ALSO", x, y, TH.text3);
        y += 22;
        float bx = x;
        for (int i = 0; gloss_see(t, i, k, sizeof k); i++) {
            const GlossTerm *s = gloss_find(k);
            if (!s) continue;
            float bw = ui_button_width(NULL, s->name);
            if (bx + bw > x + w && bx > x) { bx = x; y += 40; }
            if (ui_button(ui_idn(ui_id("gl-see"), gloss_index(s)), (Rectangle){ bx, y, bw, 32 }, NULL, s->name, BTN_SECONDARY))
                go_term(gloss_index(s));
            bx += bw + 8;
        }
        y += 44;
    }
    ui_scroll_end(&G.body, y - y0);
}

/* ---------------------------------------------------------------- show the working */
static struct { UiId id; bool open; } WO[48];
static int nWO;

static bool *open_state(UiId id)
{
    for (int i = 0; i < nWO; i++) if (WO[i].id == id) return &WO[i].open;
    if (nWO == 48) nWO = 0;
    WO[nWO].id = id; WO[nWO].open = false;
    return &WO[nWO++].open;
}

/* the open panel's contents; draw false measures */
static float working_body(const Working *wk, float x, float y, float w, bool draw)
{
    static const char *const HEAD[] = { "GIVEN", "EQUATIONS", "WORKING", "RESULT", "NOTES" };
    float y0 = y;
    if (wk->title[0]) { if (draw) ui_text(F_MED, 14, wk->title, x, y, TH.text); y += 26; }
    /* the symbol column: as wide as the widest "sym = val unit" */
    float col = 0;
    for (int i = 0; i < wk->n; i++) {
        const WkLine *l = &wk->l[i];
        if (l->kind != WK_GIVEN && l->kind != WK_RESULT) continue;
        char s[96]; snprintf(s, sizeof s, "%s = %s%s%s", l->sym, l->val, l->unit[0] ? " " : "", l->unit);
        col = fmaxf(col, ui_text_w(l->kind == WK_RESULT ? F_MONO_MED : F_MONO, 12.5f, s));
    }
    col += 18;
    bool side = col < w * 0.5f;
    for (int kind = WK_GIVEN; kind <= WK_NOTE; kind++) {
        bool any = false;
        for (int i = 0; i < wk->n; i++) {
            const WkLine *l = &wk->l[i];
            if (l->kind != kind) continue;
            if (!any) { y += 4; if (draw) ui_text(F_MED, 11.5f, HEAD[kind], x, y, TH.text3); y += 20; any = true; }
            if (kind == WK_GIVEN || kind == WK_RESULT) {
                char s[96]; snprintf(s, sizeof s, "%s = %s%s%s", l->sym, l->val, l->unit[0] ? " " : "", l->unit);
                UiFamily f = kind == WK_RESULT ? F_MONO_MED : F_MONO;
                if (draw) ui_text(f, 12.5f, s, x, y + 1, kind == WK_RESULT ? (TH.lcars ? TH.lc[1] : TH.accent) : TH.text);
                if (side) y += fmaxf(20, ui_text_wrap(F_REG, 13, l->text, (Rectangle){ x + col, y, w - col, 400 }, 19, TH.text2, draw));
                else { y += 20; if (l->text[0]) y += ui_text_wrap(F_REG, 13, l->text, (Rectangle){ x + 14, y, w - 14, 400 }, 19, TH.text2, draw); }
            } else if (kind == WK_EQ) {
                y += ui_text_wrap(F_MONO, 12.5f, l->text, (Rectangle){ x, y + 1, w, 400 }, 20, TH.text, draw);
                if (l->what[0]) y += ui_text_wrap(F_REG, 12.5f, l->what, (Rectangle){ x + 14, y, w - 14, 400 }, 18, TH.text3, draw) + 4;
            } else if (kind == WK_STEP) {
                y += ui_text_wrap(F_MONO, 12.5f, l->text, (Rectangle){ x, y + 1, w, 400 }, 20, TH.text2, draw);
            } else {
                y += ui_text_wrap(F_REG, 13, l->text, (Rectangle){ x, y, w, 400 }, 19, TH.text3, draw) + 2;
            }
        }
    }
    return y - y0;
}

float teach_working(const char *id, float x, float y, float w, const Working *wk)
{
    UiId uid = ui_id(id);
    bool *open = open_state(uid);
    const char *lab = *open ? "Hide the working" : "Show the working";
    float bw = ui_button_width(*open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, lab);
    if (ui_button(uid, (Rectangle){ x, y, bw, 30 }, *open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, lab, BTN_GHOST)) *open = !*open;
    if (!*open || !wk || !wk->n) return 36;
    float pad = 14, iw = w - 2 * pad;
    float h = working_body(wk, x + pad, 0, iw - 34, false) + 2 * pad;
    Rectangle box = { x, y + 36, w, h };
    if (!ui_culled(box.y, box.y + box.height)) {
        ui_rect(box, TH.lcars ? 14 : 10, TH.surface2);
        if (TH.lcars) ui_rect((Rectangle){ box.x, box.y + 10, 5, box.height - 20 }, 2.5f, TH.lc[1]);
        working_body(wk, x + pad, box.y + pad, iw - 34, true);
        char cid[96]; snprintf(cid, sizeof cid, "%s-copy", id);
        if (ui_button(ui_id(cid), (Rectangle){ box.x + box.width - 40, box.y + 8, 30, 30 }, ICON_COPY, NULL, BTN_ICON)) {
            char t[4096]; wk_text(wk, t, sizeof t);
            SetClipboardText(t);
            ui_toast(TOAST_OK, "Copied the working");
        }
        ui_tooltip((Rectangle){ box.x + box.width - 40, box.y + 8, 30, 30 }, "Copy the working as text");
    }
    return 36 + h + 8;
}
