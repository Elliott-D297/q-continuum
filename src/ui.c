/* ui.c -- immediate-mode UI toolkit (see ui.h) */
#define _GNU_SOURCE
#include "ui.h"
#include "input.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "rlgl.h"

#define MAX_FONTS 64
#define MAX_ANIM 512
#define MAX_POPUP_ITEMS 256
#define MAX_CLIP 16
#define MAX_TOASTS 5

typedef struct { UiFamily f; int px; bool plex; Font font; } FontSlot;
typedef struct { UiId id; float v; unsigned frame; } AnimSlot;
typedef struct { int kind; char msg[240]; double t0; } Toast;

typedef struct {
    bool  open;
    UiId  owner;
    int   layer;
    Rectangle anchor, rect;
    char  items[MAX_POPUP_ITEMS][96];
    int   n, flags, hover;
    char  filter[96];
    float scroll;
    bool  justOpened;
    /* result for the owner, picked up next frame */
    bool  resultReady;
    UiId  resultOwner;
    char  result[256];
} Popup;

static struct {
    float scale;
    int   themeId;
    Theme th;
    FontSlot fonts[MAX_FONTS];
    int   nFonts;
    int   codepoints[400];
    int   nCodepoints;

    /* input */
    Vector2 mouse, pressPos;
    bool  mPressed, mDown, mReleased, mDouble;
    double lastPressT;
    double awakeUntil;          /* draw at full rate until then: input, animation, toasts, ui_request_frames */
    Vector2 lastRawMouse;
    float wheel;
    int   layer, inputLayer, pressLayer;
    bool  modalThis, modalPrev;
    bool  disabled;
    UiId  active;
    int   cursor;
    unsigned frame;
    double time, dt;

    /* text editing */
    UiId  editId;
    bool  editSeen;
    char  editBuf[2048], editOrig[2048];
    int   editCur, editAnchor, editFlags;
    float editScroll;
    double blinkT;
    bool  editCommitPending;

    /* ids */
    UiId  idStack[16];
    int   nId, idOver;         /* pushes past the stack size, so pops stay matched */

    AnimSlot anim[MAX_ANIM];
    Rectangle clip[MAX_CLIP];
    bool  clickHit, clickDenied;      /* a click landed on a control (or a disabled one) this frame */
    int   nClip, clipOver;     /* pushes past MAX_CLIP: counted so each pop matches its push */

    Popup pop;
    UiScroll *wheelTarget;

    /* tooltip */
    UiId  tipKey;
    double tipStart;
    char  tip[256];
    bool  tipShow;

    Toast toasts[MAX_TOASTS];
    int   nToasts;
    Camera2D cam;
} U;
static UiId focusReq;                                     /* ui_focus_input: focus this input when next drawn */
static UiId enteredId;                                    /* the input Enter was pressed in this frame */

bool g_ui_no_input = false;

/* ============================================================================ widget registry
 * Every interactive widget records where it was drawn, so a test script can click it by id or label rather than by
 * pixel, and (in test mode) every string drawn, so a script can click or check visible text. Lookups read the
 * previous frame's complete list. */
#define REG_MAX 4096
#define TXT_MAX 6000
typedef struct { char s[96]; Rectangle r, vis; int layer; } RegText;
static UiWidget regW[2][REG_MAX]; static int nRegW[2];
static RegText regT[2][TXT_MAX]; static int nRegT[2];
static int regCur;

static Rectangle to_screen(Rectangle r) { return (Rectangle){ r.x * U.scale, r.y * U.scale, r.width * U.scale, r.height * U.scale }; }
static Rectangle intersect(Rectangle a, Rectangle b)
{
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y), x1 = fminf(a.x + a.width, b.x + b.width), y1 = fminf(a.y + a.height, b.y + b.height);
    return x1 > x0 && y1 > y0 ? (Rectangle){ x0, y0, x1 - x0, y1 - y0 } : (Rectangle){ 0, 0, 0, 0 };
}
static void reg_swap(void) { regCur ^= 1; nRegW[regCur] = nRegT[regCur] = 0; }
void ui_register(UiId id, Rectangle r, const char *label)
{
    if (nRegW[regCur] >= REG_MAX) return;
    UiWidget *w = &regW[regCur][nRegW[regCur]++];
    Rectangle c = ui_clip_rect();
    Rectangle win = { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() };
    w->id = id; w->r = to_screen(r); w->view = intersect(to_screen(c), win); w->vis = intersect(w->r, w->view);
    w->disabled = U.disabled; w->layer = U.layer;
    snprintf(w->label, sizeof w->label, "%s", label ? label : "");
}
static void reg_text(const char *s, Rectangle r)
{
    if (!g_ui_no_input || nRegT[regCur] >= TXT_MAX) return;
    RegText *t = &regT[regCur][nRegT[regCur]++];
    snprintf(t->s, sizeof t->s, "%s", s);
    Rectangle win = { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() };
    t->r = to_screen(r); t->vis = intersect(intersect(t->r, to_screen(ui_clip_rect())), win); t->layer = U.layer;
}
const UiWidget *ui_widget_find(UiId id)
{
    int p = regCur ^ 1;
    const UiWidget *best = NULL;
    for (int i = 0; i < nRegW[p]; i++)                    /* the last drawn wins: a modal's widgets over the page's */
        if (regW[p][i].id == id) best = &regW[p][i];
    return best;
}
const UiWidget *ui_widget_by_label(const char *label)
{
    int p = regCur ^ 1;
    const UiWidget *best = NULL;
    for (int i = 0; i < nRegW[p]; i++)
        if (regW[p][i].label[0] && !strcasecmp(regW[p][i].label, label) && (!best || regW[p][i].layer >= best->layer)) best = &regW[p][i];
    return best;
}
/* drawn text containing s (case-insensitive); exact matches first, then the topmost layer */
bool ui_text_find(const char *s, Rectangle *vis, bool exact)
{
    int p = regCur ^ 1, bestK = -1, bestScore = -1;
    for (int i = 0; i < nRegT[p]; i++) {
        const RegText *t = &regT[p][i];
        bool ex = !strcasecmp(t->s, s);
        if (exact ? !ex : !strcasestr(t->s, s)) continue;
        int score = (ex ? 100 : 0) + t->layer * 10 + (t->vis.width > 0 ? 1 : 0);
        if (score > bestScore) { bestScore = score; bestK = i; }
    }
    if (bestK < 0) return false;
    if (vis) *vis = regT[p][bestK].vis;
    return true;
}
int ui_text_dump(char *out, int cap)                     /* every visible string of the last frame, one per line */
{
    int p = regCur ^ 1, k = 0;
    for (int i = 0; i < nRegT[p] && k < cap - 100; i++) if (regT[p][i].vis.width > 0) k += snprintf(out + k, (size_t)(cap - k), "%s\n", regT[p][i].s);
    return k;
}
int ui_toast_count(void) { return U.nToasts; }
const char *ui_toast_text(int i) { return i >= 0 && i < U.nToasts ? U.toasts[i].msg : ""; }

/* scripted input for tests goes through input.c: a click at screen pixels, a drag, typed text + Enter */
void ui_inject_click(float px, float py) { inp_click(px, py); }
void ui_inject_drag(float x0, float y0, float x1, float y1) { inp_drag(x0, y0, x1, y1); }
void ui_inject_text(const char *t) { inp_type_enter(t); }
static int in_char(void) { return inp_char(); }
int  ui_next_char(void) { return in_char(); }
bool ui_mouse_down(void) { return U.mDown; }
bool ui_double_clicked(void) { return U.mDouble; }
bool ui_key(int key) { return inp_key_repeat(key); }   /* pressed or auto-repeated */
bool ui_key_down(int key) { return inp_key_down(key); }
static bool in_enter(void) { return inp_key_pressed(KEY_ENTER) || inp_key_pressed(KEY_KP_ENTER); }

/* ============================================================================ theme */
static Color hex(unsigned v) { return (Color){ (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, 255 }; }

static void make_theme(int t)
{
    Theme *h = &U.th;
    memset(h, 0, sizeof *h);
    if (t == 0) {                                   /* LCARS: black field, orange / lavender / periwinkle blocks */
        h->dark = true; h->lcars = true;
        h->bg = hex(0x000000); h->sidebar = hex(0x000000); h->surface = hex(0x0B0910); h->surface2 = hex(0x100D17);
        h->raised = hex(0x151120); h->input = hex(0x000000); h->inputHover = hex(0x07060B);
        h->line = hex(0x3D3152); h->lineSoft = hex(0x251E33); h->hover = hex(0x1C1628); h->selected = hex(0x3A2808);
        h->text = hex(0xFFE2BF); h->text2 = hex(0xCFA7D6); h->text3 = hex(0x8E7FB0); h->textOnAccent = hex(0x000000);
        h->accent = hex(0xFF9900); h->accentHover = hex(0xFFB347); h->accentSoft = (Color){ 255, 153, 0, 44 };
        h->info = hex(0x99CCFF); h->ok = hex(0x66CC99); h->warn = hex(0xFFCC33); h->err = hex(0xFF5A4E);
        h->okSoft = (Color){ 102, 204, 153, 30 }; h->warnSoft = (Color){ 255, 204, 51, 30 };
        h->errSoft = (Color){ 255, 90, 78, 34 }; h->infoSoft = (Color){ 153, 204, 255, 28 };
        h->chartBg = hex(0x000000); h->grid = hex(0x221B30);
        h->shadow = (Color){ 0, 0, 0, 0 };
        unsigned lc[8] = { 0xFF9900, 0xCC99CC, 0x9999FF, 0xFFCC99, 0xCC6666, 0xFFCC66, 0x99CCFF, 0xFF9966 };
        for (int i = 0; i < 8; i++) h->lc[i] = hex(lc[i]);
        h->cat[0] = hex(0x99CCFF); h->cat[1] = hex(0xFF9966); h->cat[2] = hex(0x66CCCC); h->cat[3] = hex(0xCC99FF); h->cat[4] = hex(0xA89CC0);
        unsigned se[10] = { 0xFF9900, 0x99CCFF, 0xCC99CC, 0xFFCC66, 0xFF5A4E, 0x9999FF, 0x66CC99, 0xFF9966, 0x66CCCC, 0xE6D3F0 };
        for (int i = 0; i < 10; i++) h->series[i] = hex(se[i]);
        return;
    }
    if (t == THEME_STARDEW || t == THEME_POKEMON) {
        /* the game menus on the classic layout. Stardew Valley: parchment boxes in dark wood frames with an orange bevel,
           a green OK button; Pokemon: white boxes with a slate double border, the cursor's yellow, type colours in charts */
        bool sv = t == THEME_STARDEW;
        unsigned c[] = { /* bg      sidebar   surface   surface2  raised    input     inputHov  line      lineSoft  hover     selected */
            sv ? 0xD9A066 : 0xE4ECF4, sv ? 0xC88A4E : 0xC9D8E8, sv ? 0xFFE4B0 : 0xFFFFFF, sv ? 0xFBD89C : 0xF6F9FC,
            sv ? 0xFFEBC2 : 0xFFFFFF, sv ? 0xFFF3D6 : 0xFFFFFF, sv ? 0xFFF7E3 : 0xF7FAFD, sv ? 0x8A4B1E : 0x4A5868,
            sv ? 0xE0B477 : 0xD5DEE8, sv ? 0xF6CF8C : 0xE9F1FA, sv ? 0xF2B866 : 0xFFF0A8 };
        Color *dst[] = { &h->bg, &h->sidebar, &h->surface, &h->surface2, &h->raised, &h->input, &h->inputHover, &h->line,
                         &h->lineSoft, &h->hover, &h->selected };
        for (int i = 0; i < 11; i++) *dst[i] = hex(c[i]);
        h->dark = false; h->game = sv ? GAME_STARDEW : GAME_POKEMON;
        h->text = hex(sv ? 0x3B1E08 : 0x283038); h->text2 = hex(sv ? 0x6B3A16 : 0x50606E); h->text3 = hex(sv ? 0x8E5A30 : 0x7A8A9A);
        h->textOnAccent = hex(sv ? 0xFFF6DD : 0xFFFFFF);
        h->accent = hex(sv ? 0x3E8E2E : 0x2A5DB0); h->accentHover = hex(sv ? 0x4FA53A : 0x3A70C8);
        h->accentSoft = ui_alpha(h->accent, 0.2f);
        h->info = hex(sv ? 0x2F6FB0 : 0x2A5DB0); h->ok = hex(sv ? 0x3E8E2E : 0x2FA344); h->warn = hex(sv ? 0xB86E00 : 0xC79100);
        h->err = hex(sv ? 0xC0392B : 0xE03C31);
        h->okSoft = ui_alpha(h->ok, 0.16f); h->warnSoft = ui_alpha(h->warn, 0.16f); h->errSoft = ui_alpha(h->err, 0.16f);
        h->infoSoft = ui_alpha(h->info, 0.14f);
        h->chartBg = hex(sv ? 0xFFF3D6 : 0xFFFFFF); h->grid = hex(sv ? 0xEBCB94 : 0xE3EAF2);
        h->shadow = hex(sv ? 0x3B1E08 : 0x283038);
        h->frame = hex(sv ? 0x5B2B0F : 0x404850); h->frameIn = hex(sv ? 0xF2A350 : 0x78B8E8);
        unsigned ca[5] = { sv ? 0x2F6FB0 : 0x4A78E0, sv ? 0xD9692A : 0xE86E1E, sv ? 0x168A85 : 0x2A9C98, sv ? 0x7D4FB0 : 0xA040A0, sv ? 0x8A6E52 : 0x8890A0 };
        for (int i = 0; i < 5; i++) h->cat[i] = hex(ca[i]);
        unsigned se[2][10] = { { 0x2F6FB0, 0xD9692A, 0x3E8E2E, 0xC0392B, 0x7D4FB0, 0x168A85, 0xB8860B, 0xC6418A, 0x5E9A22, 0x6B4A2E },
                               { 0x4A78E0, 0xE86E1E, 0x4FA83A, 0xC9A000, 0xA040A0, 0xD8332B, 0x2A9C98, 0x705898, 0xB06A9A, 0x6A5A48 } };
        for (int i = 0; i < 10; i++) h->series[i] = hex(se[sv ? 0 : 1][i]);
        return;
    }
    if (t >= THEME_SEVERANCE && t <= THEME_STARCITIZEN) {
        /* the screen and game themes, on the classic layout. One table: bg, sidebar, surface, surface2, raised, input, line,
           lineSoft, hover, selected, text, text2, text3, onAccent, accent, ok, warn, err, info; then the series */
        static const struct { int retro; bool dark; float crt; bool upper; unsigned c[19]; unsigned se[6]; } P[] = {
            /* Severance: Lumon's MDR terminal, cyan numbers on deep teal-navy, a lit-from-within glass screen */
            { RETRO_SEVERANCE, true, 0.35f, false, { 0x041A26, 0x062230, 0x082B3B, 0x0A3346, 0x0E3C52, 0x03161F, 0x2E7E96, 0x16485A,
              0x0D3A4C, 0x125A70, 0xC7F4FF, 0x86C9DA, 0x4F8FA2, 0x041A26, 0x6FE3FF, 0x7FF0C6, 0xF2D27A, 0xFF8A7A, 0x6FE3FF },
              { 0x6FE3FF, 0xF2D27A, 0x7FF0C6, 0xFF8A7A, 0xB9A6FF, 0xE2F6FA } },
            /* Alien, MU/TH/UR 6000: green-white phosphor on black, one colour, the tube does the rest */
            { RETRO_ALIEN, true, 1.0f, true, { 0x000500, 0x000500, 0x011004, 0x021406, 0x031A08, 0x000300, 0x2FAF52, 0x145C26,
              0x0B3A16, 0x16602A, 0x9DFFB4, 0x5FD07E, 0x3A8F52, 0x000500, 0x9DFFB4, 0x9DFFB4, 0xD8FF7A, 0xFFFFFF, 0x9DFFB4 },
              { 0x9DFFB4, 0x5FD07E, 0xD8FFE2, 0x3A8F52, 0xB8FFC8, 0x7AE89A } },
            /* Pip-Boy 3000: amber-green phosphor, Vault-Tec's terminal green */
            { RETRO_PIPBOY, true, 0.7f, true, { 0x020C04, 0x041306, 0x061A09, 0x08200C, 0x0A2810, 0x020A03, 0x1FD15C, 0x0E6A2C,
              0x0C3D1A, 0x10662C, 0x3CFF7A, 0x22C25A, 0x168A40, 0x020C04, 0x3CFF7A, 0x3CFF7A, 0xE8FF5C, 0xFF6A4A, 0x3CFF7A },
              { 0x3CFF7A, 0xE8FF5C, 0x22C25A, 0xA6FFC0, 0x7BE395, 0x168A40 } },
            /* NERV: black and the warning orange of the command centre, MAGI red, hazard yellow */
            { RETRO_NERV, true, 0.0f, true, { 0x050302, 0x0A0504, 0x120806, 0x170A07, 0x1E0D09, 0x060303, 0xFF6A00, 0x6A2A08,
              0x2A1006, 0x4A1A06, 0xFFB070, 0xE07A3A, 0x9A5A30, 0x000000, 0xFF6A00, 0x58E08A, 0xFFD000, 0xFF2A2A, 0x9FD3FF },
              { 0xFF6A00, 0xFFD000, 0xFF2A2A, 0x9FD3FF, 0x58E08A, 0xE07A3A } },
            /* Portal: Aperture's clean white test chambers, the blue and orange portals */
            { RETRO_PORTAL, false, 0.0f, false, { 0xF2F4F5, 0xE5E9EB, 0xFFFFFF, 0xF7F9FA, 0xFFFFFF, 0xFFFFFF, 0xB9C3C9, 0xDDE3E6,
              0xEDF1F3, 0xD8EEFF, 0x1E2428, 0x56616A, 0x8C979F, 0xFFFFFF, 0x1A9BFF, 0x2FA36B, 0xFF8A00, 0xE0452F, 0x1A9BFF },
              { 0x1A9BFF, 0xFF8A00, 0x2FA36B, 0xE0452F, 0x8A5CD8, 0x56616A } },
            /* Dark Souls: ash and ember, a bonfire's glow, parchment text, blood red */
            { RETRO_SOULS, true, 0.0f, false, { 0x0B0907, 0x0F0C09, 0x15110D, 0x1A1510, 0x211A13, 0x0A0806, 0x8A6A3A, 0x3A2E1E,
              0x2A2016, 0x3A2A16, 0xE8DCC0, 0xB8A580, 0x7A6A50, 0x0B0907, 0xE0A040, 0x9AB86A, 0xE0A040, 0xC02020, 0xC8B890 },
              { 0xE0A040, 0xC8B890, 0xC02020, 0x9AB86A, 0xFF7A20, 0x7A6A50 } },
            /* Factorio: dark machined plates, a warm grey UI, the orange of a selected entity, belt yellow */
            { RETRO_FACTORIO, true, 0.0f, false, { 0x242322, 0x2E2D2B, 0x3A3836, 0x41403D, 0x4A4846, 0x1D1C1B, 0x6E6A62, 0x2A2927,
              0x4F4C47, 0x5E4A28, 0xF2E9D8, 0xC9C0AE, 0x8F877A, 0x1D1C1B, 0xFF9F1C, 0x7CC24A, 0xFFD23A, 0xE2433A, 0x6FB6E8 },
              { 0xFF9F1C, 0x7CC24A, 0x6FB6E8, 0xE2433A, 0xFFD23A, 0xC9C0AE } },
            /* Windows 95: teal desktop, grey dialogs with bevels, navy title bars */
            { RETRO_WIN95, false, 0.0f, false, { 0xC0C0C0, 0xC0C0C0, 0xC0C0C0, 0xC0C0C0, 0xC0C0C0, 0xFFFFFF, 0x808080, 0xA0A0A0,
              0xD4D0C8, 0x000080, 0x000000, 0x202020, 0x505050, 0xFFFFFF, 0x000080, 0x008000, 0x808000, 0xC00000, 0x000080 },
              { 0x000080, 0xC00000, 0x008000, 0x808000, 0x800080, 0x008080 } },
            /* Star Citizen: a mobiGlas / cockpit MFD, cyan holographic lines on deep space blue, amber for the selected target */
            { RETRO_STARCITIZEN, true, 0.0f, true, { 0x03070D, 0x050C15, 0x08131F, 0x0B1826, 0x0F1F31, 0x040A12, 0x3FB8D8, 0x15394A,
              0x0E2638, 0x0F3A52, 0xD8F4FF, 0x8EC6D8, 0x5A8597, 0x03070D, 0x4FD8FF, 0x5FE3A1, 0xFFB238, 0xFF4F4F, 0x4FD8FF },
              { 0x4FD8FF, 0xFFB238, 0x5FE3A1, 0xFF4F4F, 0xA98BFF, 0xD8F4FF } },
        };
        const int i = t - THEME_SEVERANCE;
        const unsigned *c = P[i].c;
        h->retro = P[i].retro; h->dark = P[i].dark; h->crt = P[i].crt; h->upper = P[i].upper;
        h->bg = hex(c[0]); h->sidebar = hex(c[1]); h->surface = hex(c[2]); h->surface2 = hex(c[3]); h->raised = hex(c[4]);
        h->input = hex(c[5]); h->inputHover = ui_mix(hex(c[5]), hex(c[8]), 0.5f); h->line = hex(c[6]); h->lineSoft = hex(c[7]);
        h->hover = hex(c[8]); h->selected = hex(c[9]); h->text = hex(c[10]); h->text2 = hex(c[11]); h->text3 = hex(c[12]);
        h->textOnAccent = hex(c[13]); h->accent = hex(c[14]); h->accentHover = ui_mix(hex(c[14]), WHITE, 0.25f);
        h->accentSoft = ui_alpha(hex(c[14]), 0.18f); h->ok = hex(c[15]); h->warn = hex(c[16]); h->err = hex(c[17]); h->info = hex(c[18]);
        h->okSoft = ui_alpha(h->ok, 0.16f); h->warnSoft = ui_alpha(h->warn, 0.16f); h->errSoft = ui_alpha(h->err, 0.18f);
        h->infoSoft = ui_alpha(h->info, 0.15f);
        h->chartBg = h->input; h->grid = ui_alpha(h->line, 0.25f);
        h->shadow = h->dark ? (Color){ 0, 0, 0, 255 } : (Color){ 40, 50, 60, 255 };
        h->frame = h->line; h->frameIn = h->lineSoft;
        if (h->retro == RETRO_WIN95) { h->frame = hex(0x000000); h->frameIn = hex(0xFFFFFF); h->text3 = hex(0x606060); }
        for (int k = 0; k < 5; k++) h->cat[k] = hex(P[i].se[k]);
        for (int k = 0; k < 10; k++) h->series[k] = hex(P[i].se[k % 6]);
        if (h->retro == RETRO_ALIEN || h->retro == RETRO_PIPBOY)   /* one phosphor: the series differ by brightness, the charts by dash */
            for (int k = 0; k < 10; k++) h->series[k] = ui_mix(h->accent, h->text3, (float)k / 9.0f);
        return;
    }
    if (t == THEME_JARVIS) {
        /* JARVIS: Stark's heads-up display. Near-black navy, arc-reactor cyan for lines, accents and the main series, a gold
           secondary (the suit's), translucent panels over a faint grid; the HUD's corner brackets come from ui_rect_lines */
        #define RGBA(r, g, b, a) ((Color){ r, g, b, (unsigned char)((a) * 255) })
        h->dark = true; h->hud = true;
        h->bg = hex(0x02070D); h->sidebar = RGBA(0, 170, 255, 0.035); h->surface = RGBA(0, 190, 255, 0.055);
        h->surface2 = RGBA(0, 190, 255, 0.035); h->raised = RGBA(6, 22, 34, 0.96);
        h->input = RGBA(0, 12, 22, 0.70); h->inputHover = RGBA(0, 40, 60, 0.70);
        h->line = RGBA(0, 216, 255, 0.42); h->lineSoft = RGBA(0, 216, 255, 0.20); h->hover = RGBA(0, 216, 255, 0.09);
        h->selected = RGBA(0, 216, 255, 0.20);
        h->text = hex(0xDDF6FF); h->text2 = hex(0x8FC9DD); h->text3 = hex(0x5A8597); h->textOnAccent = hex(0x01131D);
        h->accent = hex(0x00D8FF); h->accentHover = hex(0x6BE8FF); h->accentSoft = RGBA(0, 216, 255, 0.18);
        h->info = hex(0x00D8FF); h->ok = hex(0x3DF5A7); h->warn = hex(0xFFC247); h->err = hex(0xFF4D5E);
        h->okSoft = RGBA(61, 245, 167, 0.14); h->warnSoft = RGBA(255, 194, 71, 0.15);
        h->errSoft = RGBA(255, 77, 94, 0.17); h->infoSoft = RGBA(0, 216, 255, 0.13);
        h->chartBg = RGBA(0, 10, 18, 0.55); h->grid = RGBA(0, 216, 255, 0.08);
        h->shadow = hex(0x00D8FF);                  /* panels glow rather than cast a shadow */
        h->cat[0] = hex(0x00D8FF); h->cat[1] = hex(0xFFB547); h->cat[2] = hex(0x3DF5A7); h->cat[3] = hex(0xB18CFF); h->cat[4] = hex(0x7FA6B8);
        unsigned se[10] = { 0x00D8FF, 0xFFB547, 0x3DF5A7, 0xFF4D5E, 0xB18CFF, 0x7CF3FF, 0xFFE66B, 0xFF7AC6, 0x2E9BFF, 0xC9D6DF };
        for (int i = 0; i < 10; i++) h->series[i] = hex(se[i]);
        #undef RGBA
        return;
    }
    /* the classic themes are glass (iOS): translucent panels over a soft colour backdrop (ui_backdrop), Apple's system
       colours, larger corners. Panels are white at a low alpha on dark, at a high one on light. */
    #define RGBA(r, g, b, a) ((Color){ r, g, b, (unsigned char)((a) * 255) })
    h->glass = true;
    if (t == 1) {                                   /* glass, dark */
        h->dark = true;
        h->bg = hex(0x0A0E1A); h->sidebar = RGBA(255, 255, 255, 0.045); h->surface = RGBA(255, 255, 255, 0.075);
        h->surface2 = RGBA(255, 255, 255, 0.05); h->raised = RGBA(38, 40, 52, 0.94);
        h->input = RGBA(0, 0, 0, 0.26); h->inputHover = RGBA(0, 0, 0, 0.34);
        h->line = RGBA(255, 255, 255, 0.20); h->lineSoft = RGBA(255, 255, 255, 0.11); h->hover = RGBA(255, 255, 255, 0.08);
        h->selected = RGBA(10, 132, 255, 0.30);
        h->text = hex(0xF5F5F7); h->text2 = hex(0xB4B4BC); h->text3 = hex(0x7E7E88); h->textOnAccent = hex(0xFFFFFF);
        h->accent = hex(0x0A84FF); h->accentHover = hex(0x409CFF); h->accentSoft = RGBA(10, 132, 255, 0.22);
        h->info = hex(0x64D2FF); h->ok = hex(0x30D158); h->warn = hex(0xFF9F0A); h->err = hex(0xFF453A);
        h->okSoft = RGBA(48, 209, 88, 0.16); h->warnSoft = RGBA(255, 159, 10, 0.16);
        h->errSoft = RGBA(255, 69, 58, 0.18); h->infoSoft = RGBA(100, 210, 255, 0.15);
        h->chartBg = RGBA(0, 0, 0, 0.24); h->grid = RGBA(255, 255, 255, 0.07);
        h->shadow = (Color){ 0, 0, 0, 255 };
    } else {                                        /* glass, light */
        h->dark = false;
        h->bg = hex(0xEDF0F7); h->sidebar = RGBA(255, 255, 255, 0.46); h->surface = RGBA(255, 255, 255, 0.64);
        h->surface2 = RGBA(255, 255, 255, 0.48); h->raised = RGBA(255, 255, 255, 0.95);
        h->input = RGBA(255, 255, 255, 0.78); h->inputHover = RGBA(255, 255, 255, 0.92);
        h->line = RGBA(60, 60, 67, 0.20); h->lineSoft = RGBA(60, 60, 67, 0.10); h->hover = RGBA(0, 0, 0, 0.045);
        h->selected = RGBA(0, 122, 255, 0.15);
        h->text = hex(0x1C1C1E); h->text2 = hex(0x4A4A50); h->text3 = hex(0x8A8A90); h->textOnAccent = hex(0xFFFFFF);
        h->accent = hex(0x007AFF); h->accentHover = hex(0x3395FF); h->accentSoft = RGBA(0, 122, 255, 0.14);
        h->info = hex(0x32ADE6); h->ok = hex(0x28A745); h->warn = hex(0xE08600); h->err = hex(0xFF3B30);
        h->okSoft = RGBA(52, 199, 89, 0.15); h->warnSoft = RGBA(255, 149, 0, 0.15);
        h->errSoft = RGBA(255, 59, 48, 0.13); h->infoSoft = RGBA(50, 173, 230, 0.13);
        h->chartBg = RGBA(255, 255, 255, 0.70); h->grid = RGBA(60, 60, 67, 0.08);
        h->shadow = (Color){ 30, 40, 80, 255 };
    }
    #undef RGBA
    /* category colours: blue = incompressible, orange = heat, teal = multiphase, violet = compressible */
    h->cat[0] = h->dark ? hex(0x5AA9E6) : hex(0x2F7FC1);
    h->cat[1] = h->dark ? hex(0xF08A4B) : hex(0xD9692A);
    h->cat[2] = h->dark ? hex(0x3FC1C9) : hex(0x14949C);
    h->cat[3] = h->dark ? hex(0xA78BFA) : hex(0x7A5CE0);
    h->cat[4] = h->dark ? hex(0x8FA0BA) : hex(0x6A7A94);
    /* residual series: Apple's system colours, distinguishable on both grounds */
    unsigned sd[10] = { 0x0A84FF, 0xFF9F0A, 0x30D158, 0xFF453A, 0xBF5AF2, 0x64D2FF, 0xFFD60A, 0xFF375F, 0x63E6E2, 0xAC8E68 };
    unsigned sl[10] = { 0x007AFF, 0xE08600, 0x28A745, 0xFF3B30, 0xAF52DE, 0x0FA0D6, 0xB89200, 0xFF2D55, 0x00A6A1, 0x8E6E48 };
    for (int i = 0; i < 10; i++) h->series[i] = hex(h->dark ? sd[i] : sl[i]);
}

const Theme *ui_theme(void) { return &U.th; }
int ui_theme_id(void) { return U.themeId; }

/* the field behind the glass: a few large, soft colour pools (blurry already, so the panels over them read as frosted) */
/* JARVIS: a blue glow from above, a faint grid, and the arc reactor's rings low on the right (static: an idle window
   stays at its low frame rate) */
static void hud_backdrop(float w, float h)
{
    Color cy = U.th.accent, clear = cy; clear.a = 0;
    float s = fmaxf(w, h);
    DrawCircleGradient((int)(w * 0.45f), (int)(-h * 0.15f), s * 0.75f, ui_alpha(cy, 0.10f), clear);
    DrawCircleGradient((int)(w * 0.88f), (int)(h * 0.86f), s * 0.30f, ui_alpha(cy, 0.08f), clear);
    Color gl = ui_alpha(cy, 0.035f);
    for (float x = 0; x < w; x += 48) DrawRectangleRec((Rectangle){ x, 0, 1, h }, gl);
    for (float y = 0; y < h; y += 48) DrawRectangleRec((Rectangle){ 0, y, w, 1 }, gl);
    Vector2 c = { w * 0.88f, h * 0.86f };
    float R = fminf(w, h) * 0.22f;
    DrawRing(c, R * 0.97f, R, 0, 360, 96, ui_alpha(cy, 0.10f));
    DrawRing(c, R * 0.80f, R * 0.83f, 0, 360, 96, ui_alpha(cy, 0.08f));
    for (int k = 0; k < 10; k++)                              /* the reactor's ten segments */
        DrawRing(c, R * 0.55f, R * 0.70f, k * 36.0f + 4, k * 36.0f + 32, 12, ui_alpha(cy, 0.06f));
    for (int k = 0; k < 72; k++) {                            /* a ticked scale outside */
        float a = k * 5.0f * DEG2RAD, l = k % 6 == 0 ? 0.07f : 0.035f;
        DrawLineEx((Vector2){ c.x + cosf(a) * R * 1.04f, c.y + sinf(a) * R * 1.04f },
                   (Vector2){ c.x + cosf(a) * R * (1.04f + l), c.y + sinf(a) * R * (1.04f + l) }, 1, ui_alpha(cy, 0.09f));
    }
    DrawCircleGradient((int)c.x, (int)c.y, R * 0.42f, ui_alpha(cy, 0.10f), clear);
}

/* the screen and game themes' backgrounds: static, so an idle window stays at its low frame rate */
static void retro_backdrop(float w, float h)
{
    Color a = U.th.accent, clear = a; clear.a = 0;
    float s = fmaxf(w, h);
    switch (U.th.retro) {
    case RETRO_SEVERANCE:                          /* the glow of a lit terminal in a dim room */
        DrawCircleGradient((int)(w * 0.5f), (int)(h * 0.45f), s * 0.7f, ui_alpha(a, 0.07f), clear);
        break;
    case RETRO_NERV: {                             /* a faint hexagonal grid */
        Color g = ui_alpha(a, 0.05f);
        float R = 26, dx = R * 1.732f;
        for (int row = 0; row * R * 1.5f < h + R; row++)
            for (int col = 0; col * dx < w + dx; col++) {
                float cx = col * dx + (row % 2 ? dx / 2 : 0), cy = row * R * 1.5f;
                DrawPolyLines((Vector2){ cx, cy }, 6, R, 30, g);
            }
        break;
    }
    case RETRO_SOULS:                              /* a bonfire's glow from below, darkness at the edges */
        DrawCircleGradient((int)(w * 0.5f), (int)(h * 1.1f), s * 0.6f, ui_alpha(a, 0.10f), clear);
        break;
    case RETRO_FACTORIO: {                         /* tread-plate dots */
        Color d = ui_alpha(BLACK, 0.18f);
        for (float y = 8; y < h; y += 22) for (float x = (fmodf(y, 44) < 22 ? 8 : 19); x < w; x += 22) DrawRectangleRec((Rectangle){ x, y, 6, 2 }, d);
        break;
    }
    case RETRO_STARCITIZEN: {                      /* deep space: a planet's limb glowing low on the left, a scatter of stars */
        DrawCircleGradient((int)(w * 0.08f), (int)(h * 1.25f), s * 0.75f, ui_alpha(a, 0.09f), clear);
        unsigned r = 2166136261u;
        for (int i = 0; i < 140; i++) {
            r = r * 1103515245u + 12345u; float sx = (r >> 8) % 10000 / 10000.0f * w;
            r = r * 1103515245u + 12345u; float sy = (r >> 8) % 10000 / 10000.0f * h;
            r = r * 1103515245u + 12345u; float b = 0.10f + ((r >> 8) % 100) / 100.0f * 0.30f;
            DrawRectangleRec((Rectangle){ sx, sy, i % 9 ? 1 : 2, i % 9 ? 1 : 2 }, ui_alpha(WHITE, b));
        }
        break;
    }
    case RETRO_PORTAL: {                           /* white panels of a test chamber */
        Color g = ui_alpha(BLACK, 0.035f);
        for (float x = 0; x < w; x += 96) DrawRectangleRec((Rectangle){ x, 0, 1, h }, g);
        for (float y = 0; y < h; y += 96) DrawRectangleRec((Rectangle){ 0, y, w, 1 }, g);
        break;
    }
    default: break;
    }
}

void ui_backdrop(float w, float h)
{
    if (U.th.retro) { retro_backdrop(w, h); return; }
    if (U.th.hud) { hud_backdrop(w, h); return; }
    if (!U.th.glass) return;
    bool d = U.th.dark;
    struct { float x, y, r; Color c; float a; } P[] = {
        { 0.12f, 0.08f, 0.62f, { 10, 132, 255, 255 }, d ? 0.34f : 0.26f },
        { 0.92f, 0.18f, 0.55f, { 94, 92, 230, 255 },   d ? 0.30f : 0.20f },
        { 0.62f, 1.05f, 0.60f, { 48, 209, 190, 255 },  d ? 0.20f : 0.20f },
        { 0.05f, 0.95f, 0.42f, { 255, 55, 95, 255 },   d ? 0.13f : 0.12f },
    };
    float s = fmaxf(w, h);
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        Color in = ui_alpha(P[i].c, P[i].a), out = P[i].c; out.a = 0;
        DrawCircleGradient((int)(P[i].x * w), (int)(P[i].y * h), P[i].r * s, in, out);
    }
}
static void fonts_unload(void);
void ui_set_theme(int t)
{
    if (t < 0 || t >= THEME__N) t = 0;
    bool was = U.th.lcars, wasHud = U.th.hud;
    int wasGame = U.th.game, wasRetro = U.th.retro;
    U.themeId = t;
    make_theme(t);
    if (was != U.th.lcars || wasGame != U.th.game || wasHud != U.th.hud || wasRetro != U.th.retro) fonts_unload();   /* the sans families switch typeface */
}

/* ============================================================================ fonts */
static void fonts_unload(void)
{
    for (int i = 0; i < U.nFonts; i++) UnloadFont(U.fonts[i].font);
    U.nFonts = 0;
}

static void build_codepoints(void)
{
    int n = 0;
    for (int c = 32; c < 127; c++) U.codepoints[n++] = c;
    for (int c = 160; c < 256; c++) U.codepoints[n++] = c;
    const int extra[] = { 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193,
                          0x0394, 0x03B1, 0x03B2, 0x03B5, 0x03BA, 0x03BC, 0x03BD, 0x03C1, 0x03C3, 0x03C9, 0x03C6,
                          0x03B7, 0x03B8, 0x03BB, 0x03C4, 0x2202, 0x221A, 0x221E,
                          0x2207, 0x2248, 0x2264, 0x2265, 0x2212, 0x2713, 0x2074 };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) U.codepoints[n++] = extra[i];
    U.nCodepoints = n;
}

/* stb_truetype sizes a font by (ascender - descender); IBM Plex has 1.3 em of that, so sizes in this
   file are CSS-style em sizes multiplied by EM before rasterising. Icons (Lucide) are 1 em tall. */
#define EM 1.3f
/* the themed display faces. Antonio (LCARS, all sans text) has the same 1.29 em ascender-descender span but much taller
   capitals; it is drawn slightly smaller and shifted so its baseline lands where IBM Plex's would. The game themes'
   pixel faces take labels and headings only (body text stays IBM Plex): k scales them to Plex's cap height (both caps
   are 0.70 em; Silkscreen is wide, so 0.85 of that), ys = 0.865 - ascender x their em, which puts the baseline on Plex's. */
typedef struct { const unsigned char *reg, *med, *semi; const unsigned int *nreg, *nmed, *nsemi;
                 float k, ys, sp; bool body; } DisplayFace;
#define ANT_K 0.95f
static const DisplayFace FACE_LCARS = { asset_Antonio_Regular_ttf, asset_Antonio_Medium_ttf,
                                        asset_Antonio_Bold_ttf, &asset_Antonio_Regular_ttf_len,
                                        &asset_Antonio_Medium_ttf_len, &asset_Antonio_Bold_ttf_len, ANT_K, 0.865f - 1.160f * ANT_K, 0.4f, true };
static const DisplayFace FACE_STARDEW = { asset_PixelifySans_ttf, asset_PixelifySans_ttf,
                                          asset_PixelifySans_ttf, &asset_PixelifySans_ttf_len,
                                          &asset_PixelifySans_ttf_len, &asset_PixelifySans_ttf_len, 1.20f / 1.3f, 0.865f - 0.92f, 0, false };
static const DisplayFace FACE_POKEMON = { asset_Silkscreen_Regular_ttf, asset_Silkscreen_Regular_ttf,
                                          asset_Silkscreen_Bold_ttf, &asset_Silkscreen_Regular_ttf_len,
                                          &asset_Silkscreen_Regular_ttf_len, &asset_Silkscreen_Bold_ttf_len, 0.85f * 1.28f / 1.3f,
                                          0.865f - 1.03f * 0.85f, 0.2f, false };
/* JARVIS: Rajdhani, condensed and technical, labels and headings only. Its capitals are 0.643 em over a 1.276 em span:
   k = 1.10 puts them a little above Plex's (it is narrow), ys = 0.865 - 0.93 x 1.10 x 1.3 / 1.276; tracked out a little */
static const DisplayFace FACE_JARVIS = { asset_Rajdhani_Medium_ttf, asset_Rajdhani_SemiBold_ttf,
                                         asset_Rajdhani_Bold_ttf, &asset_Rajdhani_Medium_ttf_len,
                                         &asset_Rajdhani_SemiBold_ttf_len, &asset_Rajdhani_Bold_ttf_len, 1.10f,
                                         0.865f - 0.93f * 1.10f * 1.3f / 1.276f, 0.5f, false };
/* the screen and game themes' faces (SIL OFL, google/fonts): k and ys from each font's own metrics (cap height to Plex's 0.70 em,
   baseline onto Plex's), see the comment above; VT323 a size up, it is a terminal's bitmap face */
static const DisplayFace FACE_VT323 = { asset_VT323_Regular_ttf, asset_VT323_Regular_ttf, asset_VT323_Regular_ttf,
                                        &asset_VT323_Regular_ttf_len, &asset_VT323_Regular_ttf_len, &asset_VT323_Regular_ttf_len,
                                        1.10f, 0.865f - 0.80f * 1.10f * 1.3f / 1.0f, 0.3f, true };
static const DisplayFace FACE_TECHMONO = { asset_ShareTechMono_Regular_ttf, asset_ShareTechMono_Regular_ttf, asset_ShareTechMono_Regular_ttf,
                                           &asset_ShareTechMono_Regular_ttf_len, &asset_ShareTechMono_Regular_ttf_len,
                                           &asset_ShareTechMono_Regular_ttf_len, 0.867f, -0.020f, 0, true };
static const DisplayFace FACE_TECHMONO_LABELS = { asset_ShareTechMono_Regular_ttf, asset_ShareTechMono_Regular_ttf, asset_ShareTechMono_Regular_ttf,
                                                  &asset_ShareTechMono_Regular_ttf_len, &asset_ShareTechMono_Regular_ttf_len,
                                                  &asset_ShareTechMono_Regular_ttf_len, 0.867f, -0.020f, 0, false };
static const DisplayFace FACE_CINZEL = { asset_Cinzel_ttf, asset_Cinzel_ttf, asset_Cinzel_ttf, &asset_Cinzel_ttf_len, &asset_Cinzel_ttf_len,
                                         &asset_Cinzel_ttf_len, 1.037f, -0.111f, 0.3f, false };
static const DisplayFace FACE_TITILLIUM = { asset_TitilliumWeb_SemiBold_ttf, asset_TitilliumWeb_SemiBold_ttf, asset_TitilliumWeb_SemiBold_ttf,
                                            &asset_TitilliumWeb_SemiBold_ttf_len, &asset_TitilliumWeb_SemiBold_ttf_len,
                                            &asset_TitilliumWeb_SemiBold_ttf_len, 1.196f, -0.293f, 0, false };
static const DisplayFace FACE_ORANIENBAUM = { asset_Oranienbaum_Regular_ttf, asset_Oranienbaum_Regular_ttf, asset_Oranienbaum_Regular_ttf,
                                              &asset_Oranienbaum_Regular_ttf_len, &asset_Oranienbaum_Regular_ttf_len,
                                              &asset_Oranienbaum_Regular_ttf_len, 0.95f, 0.865f - 0.895f * 0.95f * 1.3f / 1.155f, 0.2f, false };
static const DisplayFace FACE_ELECTROLIZE = { asset_Electrolize_Regular_ttf, asset_Electrolize_Regular_ttf, asset_Electrolize_Regular_ttf,
                                              &asset_Electrolize_Regular_ttf_len, &asset_Electrolize_Regular_ttf_len,
                                              &asset_Electrolize_Regular_ttf_len, 0.912f, -0.046f, 0.4f, false };
static const DisplayFace *retro_face(void)
{
    switch (U.th.retro) {
    case RETRO_ALIEN: return &FACE_VT323;
    case RETRO_PIPBOY: return &FACE_TECHMONO;
    case RETRO_SEVERANCE: return &FACE_TECHMONO_LABELS;
    case RETRO_NERV: return &FACE_ORANIENBAUM;
    case RETRO_SOULS: return &FACE_CINZEL;
    case RETRO_FACTORIO: return &FACE_TITILLIUM;
    case RETRO_STARCITIZEN: return &FACE_ELECTROLIZE;
    default: return NULL;
    }
}
static const DisplayFace *display(UiFamily f)
{
    const DisplayFace *d = U.th.lcars ? &FACE_LCARS : U.th.game == GAME_STARDEW ? &FACE_STARDEW : U.th.game == GAME_POKEMON ? &FACE_POKEMON
                         : U.th.hud ? &FACE_JARVIS : retro_face();
    if (!d || !(f == F_MED || f == F_SEMI || (f == F_REG && d->body))) return NULL;
    return d;
}
static float em(UiFamily f) { const DisplayFace *d = display(f); return f == F_ICON ? 1.0f : EM * (d ? d->k : 1.0f); }
static float yshift(UiFamily f) { const DisplayFace *d = display(f); return d ? d->ys : -0.16f; }
static float spacing(UiFamily f) { const DisplayFace *d = display(f); return d ? d->sp : 0; }

static float snap(float v) { return roundf(v * U.scale) / U.scale; }

/* the font for a family; plex: IBM Plex even in the LCARS style (Antonio has no Greek, math or arrows) */
static Font font_for(UiFamily f, float size, bool plex)
{
    const DisplayFace *d = plex ? NULL : display(f);
    float e = f == F_ICON ? 1.0f : EM * (d ? d->k : 1.0f);
    int px = (int)lroundf(size * e * U.scale);
    if (px < 6) px = 6;
    for (int i = 0; i < U.nFonts; i++) if (U.fonts[i].f == f && U.fonts[i].px == px && U.fonts[i].plex == plex) return U.fonts[i].font;
    const unsigned char *data; unsigned int len;
    if (d) {
        if (f == F_REG) { data = d->reg; len = *d->nreg; }
        else if (f == F_MED) { data = d->med; len = *d->nmed; }
        else { data = d->semi; len = *d->nsemi; }
    } else switch (f) {
    case F_MED:      data = asset_IBMPlexSans_Medium_ttf;   len = asset_IBMPlexSans_Medium_ttf_len; break;
    case F_SEMI:     data = asset_IBMPlexSans_SemiBold_ttf; len = asset_IBMPlexSans_SemiBold_ttf_len; break;
    case F_MONO:     data = asset_IBMPlexMono_Regular_ttf;  len = asset_IBMPlexMono_Regular_ttf_len; break;
    case F_MONO_MED: data = asset_IBMPlexMono_Medium_ttf;   len = asset_IBMPlexMono_Medium_ttf_len; break;
    case F_ICON:     data = asset_lucide_ttf;               len = asset_lucide_ttf_len; break;
    default:         data = asset_IBMPlexSans_Regular_ttf;  len = asset_IBMPlexSans_Regular_ttf_len; break;
    }
    Font font;
    if (f == F_ICON) font = LoadFontFromMemory(".ttf", data, (int)len, px, (int *)PL_ICON_CODEPOINTS, PL_ICON_COUNT);
    else font = LoadFontFromMemory(".ttf", data, (int)len, px, U.codepoints, U.nCodepoints);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    if (U.nFonts < MAX_FONTS) { U.fonts[U.nFonts] = (FontSlot){ f, px, plex, font }; U.nFonts++; }
    return font;
}
Font ui_font(UiFamily f, float size) { return font_for(f, size, false); }

/* characters the display faces lack: Greek, arrows, math operators, check marks. LCARS text with any of these is drawn in runs, the
   missing ones in IBM Plex at the same size (Co ≈ |U|·Δt/Δx). */
static bool plex_only(int cp) { return (cp >= 0x370 && cp <= 0x3FF) || (cp >= 0x2190 && cp <= 0x22FF) || cp == 0x2713; }

/* draw (or just measure) s in runs; returns the width */
static float text_runs(UiFamily f, float size, const char *s, float x, float y, Color c, bool draw)
{
    float w = 0;
    const char *p = s;
    char run[1024];
    while (*p) {
        int n, cp = GetCodepointNext(p, &n);
        bool plex = plex_only(cp);
        int k = 0;
        while (*p && k < (int)sizeof run - 5) {             /* the longest run of the same kind */
            int m, q = GetCodepointNext(p, &m);
            if (plex_only(q) != plex) break;
            memcpy(run + k, p, (size_t)m); k += m; p += m;
        }
        run[k] = 0;
        float e = plex ? EM : em(f), sp = plex ? 0 : spacing(f), ys = plex ? -0.16f : yshift(f);
        Font font = font_for(f, size, plex);
        if (draw) DrawTextEx(font, run, (Vector2){ snap(x + w), snap(y + ys * size) }, size * e, sp, c);
        w += MeasureTextEx(font, run, size * e, sp).x + (*p ? spacing(f) : 0);   /* the face's letter spacing between runs too */
    }
    return w;
}
static bool needs_runs(UiFamily f, const char *s)
{
    if (!display(f)) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p >= 0xCD) {   /* lead bytes of U+0370 and above */
        int n, cp = GetCodepointNext((const char *)p, &n);
        if (plex_only(cp)) return true;
        p += n - 1;
    }
    return false;
}


/* the capital-letter screens (Alien, Pip-Boy): sans text in capitals, ASCII only; mono text (identifiers) keeps its case */
static const char *caps(UiFamily f, const char *s, char *buf, size_t cap)
{
    if (!U.th.upper || f == F_MONO || f == F_MONO_MED || f == F_ICON) return s;
    size_t i = 0;
    for (; s[i] && i < cap - 1; i++) buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    buf[i] = 0;
    return buf;
}

void ui_text(UiFamily f, float size, const char *s, float x, float y, Color c)
{
    if (!s || !*s) return;
    char ub[2048]; s = caps(f, s, ub, sizeof ub);
    if (U.disabled) c = ui_alpha(c, 0.45f);
    if (g_ui_no_input) reg_text(s, (Rectangle){ x, y, ui_text_w(f, size, s), size });
    if (needs_runs(f, s)) { text_runs(f, size, s, x, y, c, true); return; }
    Font font = ui_font(f, size);
    /* callers treat y as the top of a size-high box; shift so cap height sits centred in it */
    DrawTextEx(font, s, (Vector2){ snap(x), snap(y + yshift(f) * size) }, size * em(f), spacing(f), c);
}

float ui_text_w(UiFamily f, float size, const char *s)
{
    if (!s || !*s) return 0;
    char ub[2048]; s = caps(f, s, ub, sizeof ub);
    if (needs_runs(f, s)) return text_runs(f, size, s, 0, 0, BLANK, false);
    return MeasureTextEx(ui_font(f, size), s, size * em(f), spacing(f)).x;
}

const char *ui_upper(const char *s)
{
    static char buf[8][256];
    static int k;
    char *b = buf[k = (k + 1) % 8];
    int i = 0;
    for (; s && s[i] && i < 255; i++) b[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
    b[i] = 0;
    return b;
}
const char *ui_cap(const char *s) { return U.th.lcars && s ? ui_upper(s) : s; }
/* LCARS capitalises labels, but OpenFOAM names are case-sensitive (k vs K, movingWall): only words
   that start with a capital or contain a space are treated as prose */
static bool prose(const char *s) { return s && ((s[0] >= 'A' && s[0] <= 'Z') || strchr(s, ' ')); }
const char *ui_cap_prose(const char *s) { return U.th.lcars && prose(s) ? ui_upper(s) : s; }

/* LCARS capitals word by word: identifiers keep their case. A word is an identifier when it is marked with backticks
   (`epsilon`, which are not drawn) or looks like one: it holds . _ / ( ) [ ] $ # < > = : or a digit, has a capital
   after a lower-case letter (icoFoam, movingWall), or is a single lower-case letter (k, p). Outside LCARS only the
   backticks go. */
static bool ident_word(const char *w, int n)
{
    if (n == 1 && w[0] >= 'a' && w[0] <= 'z') return true;
    for (int i = 0; i < n; i++) {
        char c = w[i];
        if (strchr("._/()[]$#<>=:", c) || (c >= '0' && c <= '9')) return true;
        if (i > 0 && c >= 'A' && c <= 'Z' && w[i - 1] >= 'a' && w[i - 1] <= 'z') return true;
    }
    return false;
}
const char *ui_cap_words(const char *s)
{
    static char buf[8][512];
    static int k;
    char *b = buf[k = (k + 1) % 8];
    int o = 0;
    bool tick = false;
    for (const char *p = s ? s : ""; *p && o < 510; ) {
        if (*p == '`') { tick = !tick; p++; continue; }
        if (*p == ' ') { b[o++] = *p++; continue; }
        int n = 0;
        while (p[n] && p[n] != ' ' && p[n] != '`') n++;
        /* trailing punctuation of a prose word (a colon, a comma) doesn't make it an identifier */
        int core = n;
        while (core > 0 && strchr(",;:!?", p[core - 1])) core--;
        bool keep = tick || !U.th.lcars || ident_word(p, core);
        for (int i = 0; i < n && o < 510; i++) b[o++] = keep || p[i] < 'a' || p[i] > 'z' ? p[i] : (char)(p[i] - 32);
        p += n;
    }
    b[o] = 0;
    return b;
}

void ui_text_center(UiFamily f, float size, const char *s, Rectangle r, Color c)
{
    float w = ui_text_w(f, size, s);
    ui_text(f, size, s, r.x + (r.width - w) / 2, r.y + (r.height - size) / 2, c);
}

void ui_text_clip(UiFamily f, float size, const char *s, float x, float y, float maxW, Color c)
{
    if (!s) return;
    if (ui_text_w(f, size, s) <= maxW) { ui_text(f, size, s, x, y, c); return; }
    char buf[512];
    int n = (int)strlen(s);
    if (n > 500) n = 500;
    float ell = ui_text_w(f, size, "…");
    while (n > 0) {
        memcpy(buf, s, (size_t)n); buf[n] = 0;
        if (ui_text_w(f, size, buf) + ell <= maxW) break;
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;      /* stay on a UTF-8 boundary */
    }
    memcpy(buf, s, (size_t)n);
    memcpy(buf + n, "…", sizeof "…");                         /* n <= 500 of 512 */
    ui_text(f, size, buf, x, y, c);
}

float ui_text_wrap(UiFamily f, float size, const char *s, Rectangle r, float lineH, Color c, bool draw)
{
    char line[1024] = "";
    int ll = 0;
    float y = r.y;
    const char *p = s;
    while (*p) {
        if (*p == '\n') {
            if (draw) ui_text(f, size, line, r.x, y, c);
            y += lineH; ll = 0; line[0] = 0; p++;
            continue;
        }
        const char *w = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        int wl = (int)(p - w);
        /* a word wider than the box (a long path) is broken after a '/' where possible, else anywhere */
        char piece[1024];
        snprintf(piece, sizeof piece, "%.*s", wl, w);
        while (wl > 1 && ui_text_w(f, size, piece) > r.width) {
            if (ll) { if (draw) ui_text(f, size, line, r.x, y, c); y += lineH; ll = 0; line[0] = 0; }
            int fit = wl - 1, slash = -1;
            while (fit > 1) {
                snprintf(piece, sizeof piece, "%.*s", fit, w);
                if (ui_text_w(f, size, piece) <= r.width) break;
                fit--;
            }
            for (int k = fit; k > fit / 2; k--) if (w[k - 1] == '/') { slash = k; break; }
            if (slash > 0) fit = slash;
            if (draw) { snprintf(piece, sizeof piece, "%.*s", fit, w); ui_text(f, size, piece, r.x, y, c); }
            y += lineH;
            w += fit; wl -= fit;
            snprintf(piece, sizeof piece, "%.*s", wl, w);
        }
        char cand[1024];
        snprintf(cand, sizeof cand, "%.*s%s%.*s", ll, line, ll ? " " : "", wl, w);
        if (ll && ui_text_w(f, size, cand) > r.width) {
            if (draw) ui_text(f, size, line, r.x, y, c);
            y += lineH;
            snprintf(line, sizeof line, "%.*s", wl, w);
            ll = (int)strlen(line);
        } else {
            snprintf(line, sizeof line, "%s", cand);
            ll = (int)strlen(line);
        }
        while (*p == ' ') p++;
    }
    if (ll) { if (draw) ui_text(f, size, line, r.x, y, c); y += lineH; }
    return y - r.y;
}

void ui_icon(const char *icon, float cx, float cy, float size, Color c)
{
    if (!icon) return;
    if (U.disabled) c = ui_alpha(c, 0.45f);
    int bytes = 0;
    int cp = GetCodepoint(icon, &bytes);
    Font font = ui_font(F_ICON, size);
    DrawTextCodepoint(font, cp, (Vector2){ snap(cx - size / 2), snap(cy - size / 2) }, size, c);
}

/* ============================================================================ shapes */
static float roundness(Rectangle r, float radius)
{
    float m = fminf(r.width, r.height);
    if (m <= 0) return 0;
    float k = 2 * radius / m;
    return k > 1 ? 1 : k;
}

/* the game themes' corners: Stardew's are pixel-square, Pokemon's softly rounded */
static float game_radius(float radius)
{
    if (U.th.glass) return radius >= 3 ? radius * 1.45f + 1 : radius;          /* iOS: generous, continuous-looking corners */
    if (U.th.hud) return fminf(radius, 3);                                        /* JARVIS: machined, nearly square */
    switch (U.th.retro) {                                                         /* the screens are square, Portal and Severance soft */
    case RETRO_WIN95: case RETRO_ALIEN: case RETRO_NERV: case RETRO_SOULS: return 0;
    case RETRO_PIPBOY: case RETRO_FACTORIO: return fminf(radius, 2);
    case RETRO_SEVERANCE: case RETRO_STARCITIZEN: return fminf(radius, 4);
    case RETRO_PORTAL: return radius >= 3 ? radius * 1.2f : radius;
    default: break;
    }
    return U.th.game == GAME_STARDEW ? fminf(radius, 2) : U.th.game == GAME_POKEMON ? fminf(radius, 6) : radius;
}

void ui_rect(Rectangle r, float radius, Color c)
{
    if (c.a == 0 || r.width <= 0 || r.height <= 0) return;
    radius = game_radius(radius);
    if (radius <= 0.5f) DrawRectangleRec(r, c);
    else DrawRectangleRounded(r, roundness(r, radius), 12, c);
}

/* c is a (hover) blend of a and b */
static bool between(Color c, Color a, Color b)
{
    unsigned char ca[3] = { c.r, c.g, c.b }, aa[3] = { a.r, a.g, a.b }, ba[3] = { b.r, b.g, b.b };
    for (int i = 0; i < 3; i++) {
        int lo = aa[i] < ba[i] ? aa[i] : ba[i], hi = aa[i] < ba[i] ? ba[i] : aa[i];
        if (ca[i] + 2 < lo || ca[i] > hi + 2) return false;
    }
    return true;
}

void ui_rect_lines(Rectangle r, float radius, float thick, Color c)
{
    if (c.a == 0) return;
    if (U.th.game && thick < 2) {
        /* a box border: the frame with its inner highlight (wood and bevel, slate and blue); other borders get heavier */
        bool box = between(c, U.th.lineSoft, U.th.line) && r.width >= 120 && r.height >= 50;   /* cards, panels, modals */
        if (box) {
            radius = game_radius(radius);
            ui_rect_lines(r, radius, 3, U.th.frame);
            Rectangle in = { r.x + 3, r.y + 3, r.width - 6, r.height - 6 };
            Color hi = U.th.frameIn; hi.a = c.a;
            ui_rect_lines(in, fmaxf(radius - 3, 0), 2.5f, hi);
            return;
        }
        thick = 2;
    }
    if (U.th.retro && thick < 2 && r.width >= 120 && r.height >= 44 && between(c, U.th.lineSoft, U.th.line)) {
        /* panels in each screen's own way */
        float x0 = r.x, y0 = r.y, x1 = r.x + r.width, y1 = r.y + r.height;
        switch (U.th.retro) {
        case RETRO_WIN95:                          /* raised: white and light grey top-left, black and dark grey bottom-right */
            DrawRectangleRec((Rectangle){ x0, y0, r.width, 1 }, WHITE); DrawRectangleRec((Rectangle){ x0, y0, 1, r.height }, WHITE);
            DrawRectangleRec((Rectangle){ x0, y1 - 1, r.width, 1 }, BLACK); DrawRectangleRec((Rectangle){ x1 - 1, y0, 1, r.height }, BLACK);
            DrawRectangleRec((Rectangle){ x0 + 1, y1 - 2, r.width - 2, 1 }, (Color){ 128, 128, 128, 255 });
            DrawRectangleRec((Rectangle){ x1 - 2, y0 + 1, 1, r.height - 2 }, (Color){ 128, 128, 128, 255 });
            DrawRectangleRec((Rectangle){ x0 + 1, y0 + 1, r.width - 3, 1 }, (Color){ 223, 223, 223, 255 });
            DrawRectangleRec((Rectangle){ x0 + 1, y0 + 1, 1, r.height - 3 }, (Color){ 223, 223, 223, 255 });
            return;
        case RETRO_FACTORIO:                       /* a dark inset plate with a lit top edge */
            DrawRectangleLinesEx(r, 2, (Color){ 20, 19, 18, c.a });
            DrawRectangleRec((Rectangle){ x0 + 2, y0 + 2, r.width - 4, 1 }, ui_alpha(WHITE, 0.10f));
            DrawRectangleRec((Rectangle){ x0 + 2, y1 - 3, r.width - 4, 1 }, ui_alpha(BLACK, 0.4f));
            return;
        case RETRO_NERV: {                         /* chamfered corners, like the command centre's panels */
            float k = fminf(14, fminf(r.width, r.height) * 0.2f);
            Vector2 pts[9] = { { x0 + k, y0 }, { x1 - k, y0 }, { x1, y0 + k }, { x1, y1 - k }, { x1 - k, y1 }, { x0 + k, y1 }, { x0, y1 - k },
                               { x0, y0 + k }, { x0 + k, y0 } };
            for (int i = 0; i < 8; i++) DrawLineEx(pts[i], pts[i + 1], 1.5f, ui_alpha(U.th.accent, 0.75f * c.a / 255.0f));
            return;
        }
        case RETRO_SOULS:                          /* a thin gilt double rule */
            DrawRectangleLinesEx(r, 1, ui_alpha(U.th.line, 0.9f * c.a / 255.0f));
            DrawRectangleLinesEx((Rectangle){ x0 + 3, y0 + 3, r.width - 6, r.height - 6 }, 1, ui_alpha(U.th.line, 0.45f * c.a / 255.0f));
            return;
        case RETRO_STARCITIZEN: {                  /* an MFD panel: a faint hairline, the top-right corner cut, a bright tick there */
            float k = fminf(16, fminf(r.width, r.height) * 0.22f);
            Color hl = ui_alpha(U.th.line, 0.45f * c.a / 255.0f), br = ui_alpha(U.th.accent, 0.9f * c.a / 255.0f);
            Vector2 pts[6] = { { x0, y0 }, { x1 - k, y0 }, { x1, y0 + k }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
            for (int i = 0; i < 5; i++) DrawLineEx(pts[i], pts[i + 1], 1, hl);
            DrawLineEx(pts[1], pts[2], 2, br);
            DrawRectangleRec((Rectangle){ x0, y0, fminf(40, r.width * 0.2f), 2 }, br);
            return;
        }
        case RETRO_ALIEN: case RETRO_PIPBOY:       /* one phosphor line */
            DrawRectangleLinesEx(r, 1, ui_alpha(U.th.line, c.a / 255.0f));
            return;
        default: break;                            /* Severance, Portal: the plain border below */
        }
    }
    if (U.th.hud && thick < 2 && r.width >= 120 && r.height >= 44 && between(c, U.th.lineSoft, U.th.line)) {
        /* a HUD panel: a faint hairline, bright corner brackets, a short tab along the top edge */
        DrawRectangleLinesEx(r, 1, ui_alpha(c, 0.55f));
        Color b = ui_alpha(U.th.accent, 0.85f * c.a / 255.0f), g = ui_alpha(U.th.accent, 0.18f * c.a / 255.0f);
        float L = fminf(16, fminf(r.width, r.height) * 0.25f);
        float x0 = r.x, y0 = r.y, x1 = r.x + r.width, y1 = r.y + r.height;
        const float C[4][4] = { { x0, y0, 1, 1 }, { x1, y0, -1, 1 }, { x0, y1, 1, -1 }, { x1, y1, -1, -1 } };
        for (int k = 0; k < 4; k++) {
            float cx = C[k][0], cy = C[k][1], sx = C[k][2], sy = C[k][3];
            DrawLineEx((Vector2){ cx, cy + sy * 0.5f }, (Vector2){ cx + sx * L, cy + sy * 0.5f }, 3, g);   /* the glow */
            DrawLineEx((Vector2){ cx + sx * 0.5f, cy }, (Vector2){ cx + sx * 0.5f, cy + sy * L }, 3, g);
            DrawLineEx((Vector2){ cx, cy + sy * 0.75f }, (Vector2){ cx + sx * L, cy + sy * 0.75f }, 1.5f, b);
            DrawLineEx((Vector2){ cx + sx * 0.75f, cy }, (Vector2){ cx + sx * 0.75f, cy + sy * L }, 1.5f, b);
        }
        if (r.width >= 240) DrawRectangleRec((Rectangle){ x0 + L + 10, y0, fminf(60, r.width * 0.15f), 2 }, ui_alpha(b, 0.6f));
        return;
    }
    if (U.th.glass && thick < 2 && r.width >= 120 && r.height >= 44 && between(c, U.th.lineSoft, U.th.line)) {
        /* a glass panel's rim: a light edge all round and a brighter catch of light along the top */
        float rad = game_radius(radius);
        Color rim = U.th.dark ? ui_alpha(WHITE, 0.13f) : ui_alpha(WHITE, 0.85f);
        Rectangle i = { r.x + 1, r.y + 1, r.width - 2, r.height - 2 };
        DrawRectangleRoundedLinesEx(i, roundness(i, fmaxf(rad - 1, 0.5f)), 16, 1, rim);
        if (!U.th.dark) DrawRectangleRoundedLinesEx((Rectangle){ r.x, r.y, r.width, r.height }, roundness(r, rad), 16, 1, ui_alpha(c, 0.55f));
        DrawRectangleGradientH((int)(r.x + rad), (int)r.y + 1, (int)(r.width - 2 * rad), 1, ui_alpha(WHITE, U.th.dark ? 0.05f : 0.6f),
                               ui_alpha(WHITE, U.th.dark ? 0.32f : 1.0f));
        return;
    }
    radius = game_radius(radius);
    /* raylib strokes outside the rectangle: inset so the border sits on the edge */
    Rectangle i = { r.x + thick, r.y + thick, r.width - 2 * thick, r.height - 2 * thick };
    if (radius <= 0.5f) DrawRectangleLinesEx(r, thick, c);
    else DrawRectangleRoundedLinesEx(i, roundness(i, fmaxf(radius - thick, 0.5f)), 12, thick, c);
}

void ui_pill(Rectangle r, Color c) { ui_rect(r, fminf(r.width, r.height) / 2, c); }

void ui_elbow(float x, float y, float cw, float h, float w, float barH, float R, float r, bool barBottom, Color c)
{
    if (R > cw) R = cw;
    if (R > h) R = h;
    if (barBottom) {
        DrawRectangleRec((Rectangle){ x, y, cw, h - R }, c);
        DrawCircleSector((Vector2){ x + R, y + h - R }, R, 90, 180, 32, c);
        DrawRectangleRec((Rectangle){ x + R, y + h - R, cw - R, R }, c);
        DrawRectangleRec((Rectangle){ x + cw, y + h - barH, w - cw, barH }, c);
        DrawRectangleRec((Rectangle){ x + cw, y + h - barH - r, r, r }, c);
        DrawCircleSector((Vector2){ x + cw + r, y + h - barH - r }, r, 0, 360, 32, U.th.bg);
    } else {
        DrawRectangleRec((Rectangle){ x, y + R, cw, h - R }, c);
        DrawCircleSector((Vector2){ x + R, y + R }, R, 180, 270, 32, c);
        DrawRectangleRec((Rectangle){ x + R, y, cw - R, R }, c);
        DrawRectangleRec((Rectangle){ x + cw, y, w - cw, barH }, c);
        DrawRectangleRec((Rectangle){ x + cw, y + barH, r, r }, c);
        DrawCircleSector((Vector2){ x + cw + r, y + barH + r }, r, 0, 360, 32, U.th.bg);
    }
}

void ui_shadow(Rectangle r, float radius, float strength)
{
    if (U.th.lcars) return;
    if (U.th.retro && U.th.retro != RETRO_PORTAL && U.th.retro != RETRO_SEVERANCE) {   /* screens have no shadows; Windows a black 1 px */
        if (U.th.retro == RETRO_WIN95) DrawRectangleRec((Rectangle){ r.x + 2, r.y + 2, r.width, r.height }, ui_alpha(BLACK, 0.5f));
        return;
    }
    if (U.th.game) {                               /* a hard drop shadow, as in the games' menus */
        float o = 3 * fminf(strength, 1.5f) + 1;
        ui_rect((Rectangle){ r.x + o, r.y + o, r.width, r.height }, radius, ui_alpha(U.th.shadow, 0.3f));
        return;
    }
    if (U.th.hud) {                                /* a faint cyan glow round the panel */
        for (int i = 1; i <= 4; i++) {
            float g = (float)i * 2.5f;
            ui_rect((Rectangle){ r.x - g, r.y - g, r.width + 2 * g, r.height + 2 * g }, radius + g,
                    ui_alpha(U.th.shadow, 0.035f * strength * (1.0f - (float)i / 5.0f)));
        }
        return;
    }
    Color s = U.th.shadow;
    float a0 = (U.th.dark ? 0.22f : 0.07f) * strength;
    if (U.th.glass) { a0 = (U.th.dark ? 0.30f : 0.10f) * strength; radius = game_radius(radius); }
    for (int i = 1; i <= 6; i++) {
        float g = (float)i * 2.0f;
        Rectangle e = { r.x - g * 0.6f, r.y - g * 0.3f + g * 0.5f, r.width + g * 1.2f, r.height + g * 1.0f };
        ui_rect(e, radius + g * 0.6f, ui_alpha(s, a0 * (1.0f - (float)i / 7.0f)));
    }
}

void ui_line(float x0, float y0, float x1, float y1, float thick, Color c)
{
    DrawLineEx((Vector2){ x0, y0 }, (Vector2){ x1, y1 }, thick, c);
}

Color ui_mix(Color a, Color b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                    (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t) };
}
Color ui_alpha(Color c, float a) { c.a = (unsigned char)(c.a * (a < 0 ? 0 : a > 1 ? 1 : a)); return c; }
Rectangle ui_inset(Rectangle r, float dx, float dy) { return (Rectangle){ r.x + dx, r.y + dy, r.width - 2 * dx, r.height - 2 * dy }; }

void ui_divider(float x, float y, float w) { DrawRectangleRec((Rectangle){ x, y, w, 1.0f / U.scale }, U.th.lineSoft); }

/* ============================================================================ clipping */
static void apply_clip(void)
{
    if (U.nClip == 0) { EndScissorMode(); return; }
    Rectangle c = U.clip[U.nClip - 1];
    float s = U.scale;
    int x = (int)floorf(c.x * s), y = (int)floorf(c.y * s);
    int w = (int)ceilf((c.x + c.width) * s) - x, h = (int)ceilf((c.y + c.height) * s) - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    BeginScissorMode(x, y, w, h);
}

/* ============================================================================ the CRT pass (Alien, Pip-Boy, Severance) */
/* the frame is drawn into a texture, then onto the screen through a shader: barrel curvature, a phosphor glow (a small blur
   added back), scanlines, vignette and a faint shimmer (1.5 %, far under any flashing threshold) */
static const char *CRT_FS =
    "#version 330\n"
    "in vec2 fragTexCoord; in vec4 fragColor; uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
    "uniform float time; uniform float strength; uniform vec2 res; out vec4 finalColor;\n"
    "vec2 curve(vec2 uv) { uv = uv * 2.0 - 1.0; vec2 o = abs(uv.yx) / vec2(5.0, 4.0); uv = uv + uv * o * o * strength; return uv * 0.5 + 0.5; }\n"
    "void main() {\n"
    "  vec2 uv = curve(fragTexCoord);\n"
    "  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { finalColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "  vec3 c = texture(texture0, uv).rgb; vec2 px = 1.0 / res; vec3 b = vec3(0.0);\n"
    "  for (int i = -2; i <= 2; i++) for (int j = -2; j <= 2; j++) b += texture(texture0, uv + vec2(float(i), float(j)) * px * 1.6).rgb;\n"
    "  c += b / 25.0 * 0.6 * strength;\n"
    "  float sl = 0.78 + 0.22 * sin(uv.y * res.y * 3.14159);\n"
    "  c *= mix(1.0, sl, strength);\n"
    "  vec2 v = uv * (1.0 - uv.yx); c *= pow(clamp(v.x * v.y * 16.0, 0.0, 1.0), 0.28 * strength);\n"
    "  c *= 1.0 - 0.015 * strength * (0.5 + 0.5 * sin(time * 9.0));\n"
    "  finalColor = vec4(c, 1.0);\n"
    "}\n";
static struct { Shader sh; RenderTexture2D rt; int w, h; bool ok, tried, active; int locTime, locStr, locRes; } CRT;

/* the content point under a screen point: what the shader shows there (the same curve) */
static Vector2 crt_unwarp(Vector2 p, float w, float h)
{
    float u = p.x / w * 2 - 1, v = p.y / h * 2 - 1, k = U.th.crt;
    float ox = fabsf(v) / 5.0f, oy = fabsf(u) / 4.0f;
    u = u + u * ox * ox * k; v = v + v * oy * oy * k;
    return (Vector2){ (u * 0.5f + 0.5f) * w, (v * 0.5f + 0.5f) * h };
}

bool ui_crt_begin(void)
{
    CRT.active = false;
    if (!(U.th.crt > 0)) return false;
    if (!CRT.tried) {
        CRT.tried = true;
        CRT.sh = LoadShaderFromMemory(NULL, CRT_FS);
        CRT.ok = IsShaderValid(CRT.sh);
        if (CRT.ok) { CRT.locTime = GetShaderLocation(CRT.sh, "time"); CRT.locStr = GetShaderLocation(CRT.sh, "strength"); CRT.locRes = GetShaderLocation(CRT.sh, "res"); }
    }
    if (!CRT.ok) return false;
    int w = GetScreenWidth(), h = GetScreenHeight();
    if (w != CRT.w || h != CRT.h) {
        if (CRT.w) UnloadRenderTexture(CRT.rt);
        CRT.rt = LoadRenderTexture(w, h); CRT.w = w; CRT.h = h;
        SetTextureFilter(CRT.rt.texture, TEXTURE_FILTER_BILINEAR);
    }
    BeginTextureMode(CRT.rt);
    CRT.active = true;
    return true;
}

void ui_crt_end(void)
{
    if (!CRT.active) return;
    EndTextureMode();
    CRT.active = false;
    float t = (float)GetTime(), k = U.th.crt, res[2] = { (float)CRT.w, (float)CRT.h };
    SetShaderValue(CRT.sh, CRT.locTime, &t, SHADER_UNIFORM_FLOAT);
    SetShaderValue(CRT.sh, CRT.locStr, &k, SHADER_UNIFORM_FLOAT);
    SetShaderValue(CRT.sh, CRT.locRes, res, SHADER_UNIFORM_VEC2);
    ClearBackground(BLACK);
    BeginShaderMode(CRT.sh);
    DrawTextureRec(CRT.rt.texture, (Rectangle){ 0, 0, (float)CRT.w, -(float)CRT.h }, (Vector2){ 0, 0 }, WHITE);   /* render textures are upside down */
    EndShaderMode();
}

/* after rendering into a texture (3-D views): the 2-D camera and the clip rectangle again (and the CRT's target, which
   EndTextureMode left) */
void ui_restore_2d(void) { if (CRT.active) BeginTextureMode(CRT.rt); BeginMode2D(U.cam); apply_clip(); }

void ui_clip_push(Rectangle r)
{
    if (U.nClip > 0) {
        Rectangle p = U.clip[U.nClip - 1];
        float x0 = fmaxf(r.x, p.x), y0 = fmaxf(r.y, p.y);
        float x1 = fminf(r.x + r.width, p.x + p.width), y1 = fminf(r.y + r.height, p.y + p.height);
        r = (Rectangle){ x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0) };
    }
    if (U.nClip < MAX_CLIP) U.clip[U.nClip++] = r;
    else U.clipOver++;                 /* too deep: keep the parent's clip rather than drop a level */
    apply_clip();
}
void ui_clip_pop(void)
{
    if (U.clipOver > 0) U.clipOver--;
    else if (U.nClip > 0) U.nClip--;
    apply_clip();
}
Rectangle ui_clip_rect(void) { return U.nClip ? U.clip[U.nClip - 1] : (Rectangle){ -1e5f, -1e5f, 2e5f, 2e5f }; }

/* ============================================================================ ids, anim */
static UiId fnv(const void *p, size_t n, UiId h)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;
}
static UiId seed(void) { return U.nId ? U.idStack[U.nId - 1] : 2166136261u; }
UiId ui_id(const char *s) { return fnv(s, strlen(s), seed()); }
UiId ui_idp(const void *p) { return fnv(&p, sizeof p, seed()); }
UiId ui_idn(UiId base, int n) { return fnv(&n, sizeof n, base); }
void ui_push_id(UiId id) { if (U.nId < 16) U.idStack[U.nId++] = id; else U.idOver++; }
void ui_pop_id(void) { if (U.idOver) U.idOver--; else if (U.nId) U.nId--; }

float ui_anim(UiId id, float target, float speed)
{
    int k = (int)(id % MAX_ANIM), slot = -1;
    for (int i = 0; i < 8; i++) {
        int j = (k + i) % MAX_ANIM;
        if (U.anim[j].id == id) { slot = j; break; }
        if (slot < 0 && (U.anim[j].id == 0 || U.frame - U.anim[j].frame > 120)) slot = j;
    }
    if (slot < 0) slot = k;
    AnimSlot *a = &U.anim[slot];
    if (a->id != id) { a->id = id; a->v = target; }
    a->frame = U.frame;
    float d = target - a->v;
    float step = speed * (float)U.dt;
    if (fabsf(d) <= step) a->v = target; else { a->v += d > 0 ? step : -step; ui_request_frames(2); }
    return a->v;
}

/* ============================================================================ input helpers */
void ui_set_layer(int layer) { U.layer = layer; }
bool ui_mouse_ok(void) { return !U.disabled && U.layer == U.inputLayer; }
Vector2 ui_mouse(void) { return U.mouse; }
void ui_disable(bool on) { U.disabled = on; }
void ui_cursor(int c) { if (c > U.cursor) U.cursor = c; }
bool ui_any_input_focused(void) { return U.editId != 0 || U.pop.open; }
bool ui_input_entered(UiId id) { return id && enteredId == id; }
bool ui_input_focused(UiId id) { return id && U.editId == id; }
void ui_focus_input(UiId id) { focusReq = id; }
void ui_request_frames(int n) { double t = U.time + n / 60.0 + 0.05; if (t > U.awakeUntil) U.awakeUntil = t; }
bool ui_awake(void) { return U.nToasts > 0 || U.pop.open || U.editId != 0 || U.time < U.awakeUntil; }

bool ui_hovered(Rectangle r)
{
    if (!ui_mouse_ok()) return false;
    return CheckCollisionPointRec(U.mouse, r) && CheckCollisionPointRec(U.mouse, ui_clip_rect());
}
bool ui_pressed(Rectangle r) { return U.mPressed && ui_hovered(r); }
bool ui_clicked(Rectangle r)
{
    bool c = U.mReleased && ui_hovered(r) && U.pressLayer == U.layer && CheckCollisionPointRec(U.pressPos, r);
    if (c) { if (U.disabled) U.clickDenied = true; else U.clickHit = true; }   /* for the sound hook, once a frame */
    return c;
}

void (*ui_sound_hook)(UiSound s);

float ui_scale(void) { return U.scale; }
float ui_width(void) { return GetScreenWidth() / U.scale; }
float ui_height(void) { return GetScreenHeight() / U.scale; }
double ui_time(void) { return U.time; }

/* ============================================================================ frame */
void ui_init(float scale, int theme)
{
    memset(&U, 0, sizeof U);
    U.lastPressT = -10;                               /* the first click is never a double click */
    U.scale = scale > 0 ? scale : 1;
    build_codepoints();
    ui_set_theme(theme);
    U.cam.zoom = U.scale;
}

void ui_set_scale(float scale)
{
    if (fabsf(scale - U.scale) < 1e-3f) return;
    fonts_unload();
    U.scale = scale;
    U.cam.zoom = scale;
}

void ui_shutdown(void) { fonts_unload(); }

void ui_begin_frame(void)
{
    U.frame++;
    double now = GetTime();
    U.dt = U.time > 0 ? now - U.time : 1.0 / 60;
    if (U.dt > 0.1) U.dt = 0.1;
    U.time = now;
    inp_set_scripted(g_ui_no_input);                  /* test screenshots: the real pointer must not scroll or hover */
    inp_begin_frame();
    Vector2 m = inp_mouse();
    if (U.th.crt > 0 && CRT.ok && !g_ui_no_input) m = crt_unwarp(m, (float)GetScreenWidth(), (float)GetScreenHeight());   /* through the tube's curve */
    U.mouse = (Vector2){ m.x / U.scale, m.y / U.scale };
    U.mPressed = inp_button_pressed();
    U.mReleased = inp_button_released();
    U.mDown = inp_button_down();
    U.wheel = inp_wheel();
    /* any input keeps the frame rate up for a while */
    if (inp_activity() || inp_any_key() != 0) U.awakeUntil = now + 1.0;
    U.lastRawMouse = m;
    reg_swap();
    U.mDouble = false;
    if (U.mPressed) {
        U.mDouble = (now - U.lastPressT) < 0.35;
        U.lastPressT = now;
    }
    U.cursor = MOUSE_CURSOR_DEFAULT;
    U.wheelTarget = NULL;
    U.tipShow = false;
    U.editSeen = false;
    enteredId = 0;
    U.disabled = false;
    U.layer = 0;
    U.nId = 0; U.idOver = 0;

    /* who receives the mouse this frame */
    U.modalPrev = U.modalThis;
    U.modalThis = false;
    if (U.pop.open && U.mPressed && !CheckCollisionPointRec(U.mouse, U.pop.rect)) {
        bool onAnchor = CheckCollisionPointRec(U.mouse, U.pop.anchor);
        U.pop.open = false;                          /* click outside closes and is consumed */
        U.mPressed = false;
        U.pressPos = (Vector2){ -1e6f, -1e6f };
        if (onAnchor) U.mReleased = false;
    }
    U.inputLayer = U.pop.open ? 2 : (U.modalPrev ? 1 : 0);
    if (U.mPressed) { U.pressPos = U.mouse; U.pressLayer = U.inputLayer; }

    BeginMode2D(U.cam);
}

/* ---------------------------------------------------------------- popup drawing */
static int popup_visible(int *map)
{
    int n = 0;
    const char *f = U.pop.filter;
    if (!(U.pop.flags & DD_FILTER) || !f[0]) { for (int i = 0; i < U.pop.n; i++) map[n++] = i; return n; }
    /* the item typed exactly first ("T" before "alphat"), then ones that start with it, then the rest that contain it */
    size_t fl = strlen(f);
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < U.pop.n; i++) {
            const char *s = U.pop.items[i];
            int rank = !strcmp(s, f) ? 0 : !strcasecmp(s, f) ? 1 : !strncasecmp(s, f, fl) ? 2 : strcasestr(s, f) ? 3 : -1;
            if (rank == pass) map[n++] = i;
        }
    return n;
}

static void popup_pick(const char *s)
{
    U.pop.resultReady = true;
    U.pop.resultOwner = U.pop.owner;
    snprintf(U.pop.result, sizeof U.pop.result, "%s", s);
    U.pop.open = false;
}

static void draw_popup(void)
{
    Popup *p = &U.pop;
    if (!p->open) return;
    U.layer = 2;
    const Theme *t = &U.th;
    bool filt = p->flags & DD_FILTER;
    UiFamily fam = (p->flags & DD_MONO) ? F_MONO : F_REG;
    float rowH = 30, fs = 13.5f;
    int map[MAX_POPUP_ITEMS];
    int nv = popup_visible(map);
    bool custom = (p->flags & DD_CUSTOM) && p->filter[0];
    int rows = nv + (custom ? 1 : 0);

    float w = p->anchor.width;
    for (int i = 0; i < p->n; i++) w = fmaxf(w, ui_text_w(fam, fs, p->items[i]) + 40);
    w = fminf(w, 460);
    float filterH = filt ? 40 : 0;
    float listH = fminf((float)(rows > 0 ? rows : 1) * rowH, 300);
    float h = filterH + listH + 10;
    float x = p->anchor.x, y = p->anchor.y + p->anchor.height + 4;
    if (y + h > ui_height() - 8) y = fmaxf(8, p->anchor.y - h - 4);
    if (x + w > ui_width() - 8) x = ui_width() - 8 - w;
    p->rect = (Rectangle){ x, y, w, h };
    ui_shadow(p->rect, 10, 1.4f);
    if (t->lcars) {
        ui_rect(p->rect, 14, BLACK);
        ui_rect_lines(p->rect, 14, 2, t->accent);
    } else {
        ui_rect(p->rect, 10, t->raised);
        ui_rect_lines(p->rect, 10, 1, t->line);
    }

    /* keyboard */
    if (inp_key_pressed(KEY_ESCAPE)) { p->open = false; return; }
    if (filt) {
        int c;
        size_t fl = strlen(p->filter);
        while ((c = in_char()) > 0) {
            if (c >= 32 && c < 127 && fl + 1 < sizeof p->filter) { p->filter[fl++] = (char)c; p->filter[fl] = 0; p->hover = 0; p->scroll = 0; }
        }
        if ((inp_key_repeat(KEY_BACKSPACE)) && fl) p->filter[fl - 1] = 0;
        nv = popup_visible(map);
        custom = (p->flags & DD_CUSTOM) && p->filter[0];
        rows = nv + (custom ? 1 : 0);
        Rectangle fr = { x + 8, y + 8, w - 16, 28 };
        ui_rect(fr, 6, t->input);
        ui_rect_lines(fr, 6, 1, t->accent);
        ui_icon(ICON_SEARCH, fr.x + 14, fr.y + 14, 14, t->text3);
        if (p->filter[0]) ui_text(F_REG, 13.5f, p->filter, fr.x + 28, fr.y + 6, t->text);
        else ui_text(F_REG, 13.5f, (p->flags & DD_CUSTOM) ? "Filter or type a value" : "Filter", fr.x + 28, fr.y + 6, t->text3);
        float cx = fr.x + 28 + ui_text_w(F_REG, 13.5f, p->filter) + 1;
        if (fmod(U.time, 1.0) < 0.55) DrawRectangleRec((Rectangle){ cx, fr.y + 6, 1.2f, 16 }, t->text);
    }
    if (inp_key_repeat(KEY_DOWN)) p->hover = p->hover + 1 < rows ? p->hover + 1 : p->hover;
    if (inp_key_repeat(KEY_UP)) p->hover = p->hover > 0 ? p->hover - 1 : 0;
    if (in_enter()) {
        if (custom && p->hover == 0) { popup_pick(p->filter); return; }
        int k = p->hover - (custom ? 1 : 0);
        if (k >= 0 && k < nv) { popup_pick(p->items[map[k]]); return; }
    }

    Rectangle list = { x + 4, y + 5 + filterH, w - 8, listH };
    float maxScroll = fmaxf(0, rows * rowH - listH);
    if (CheckCollisionPointRec(U.mouse, list) && U.wheel != 0) p->scroll -= U.wheel * rowH * 2;
    /* keep the keyboard row visible */
    float hy = p->hover * rowH;
    if (hy < p->scroll) p->scroll = hy;
    if (hy + rowH > p->scroll + listH) p->scroll = hy + rowH - listH;
    if (p->scroll < 0) p->scroll = 0;
    if (p->scroll > maxScroll) p->scroll = maxScroll;

    ui_clip_push(list);
    for (int r = 0; r < rows; r++) {
        Rectangle rr = { list.x, list.y + r * rowH - p->scroll, list.width, rowH };
        if (rr.y + rowH < list.y || rr.y > list.y + list.height) continue;
        bool hov = CheckCollisionPointRec(U.mouse, rr) && CheckCollisionPointRec(U.mouse, list);
        if (hov && (fabsf(inp_mouse_delta().x) > 0 || fabsf(inp_mouse_delta().y) > 0)) p->hover = r;
        if (p->hover == r) ui_rect(rr, t->lcars ? rowH / 2 : 6, t->lcars ? ui_alpha(t->accent, 0.22f) : t->hover);
        if (custom && r == 0) {
            char lab[160]; snprintf(lab, sizeof lab, "Use \"%s\"", p->filter);
            ui_icon(ICON_PENCIL, rr.x + 16, rr.y + rowH / 2, 13, t->accent);
            ui_text_clip(F_MED, fs, lab, rr.x + 30, rr.y + (rowH - fs) / 2 - 1, rr.width - 40, t->text);
            if (hov && U.mReleased && !p->justOpened) { popup_pick(p->filter); ui_clip_pop(); return; }
            continue;
        }
        const char *it = p->items[map[r - (custom ? 1 : 0)]];
        ui_text_clip(fam, fs, it, rr.x + 12, rr.y + (rowH - fs) / 2 - 1, rr.width - 24, t->text);
        if (hov) U.cursor = MOUSE_CURSOR_POINTING_HAND;
        if (hov && U.mReleased && !p->justOpened) { popup_pick(it); ui_clip_pop(); return; }
    }
    if (rows == 0) ui_text(F_REG, fs, "No matches", list.x + 12, list.y + 8, t->text3);
    ui_clip_pop();
    if (maxScroll > 0) {
        float th = listH * listH / (rows * rowH);
        float ty = list.y + (listH - th) * (p->scroll / maxScroll);
        ui_rect((Rectangle){ list.x + list.width - 4, ty, 3, th }, 1.5f, ui_alpha(t->text3, 0.6f));
    }
    if (U.mReleased) p->justOpened = false;
}

static void draw_toasts(void)
{
    const Theme *t = &U.th;
    float y = ui_height() - 20;
    for (int i = U.nToasts - 1; i >= 0; i--) {
        Toast *o = &U.toasts[i];
        double age = U.time - o->t0;
        if (age > 4.5) { memmove(&U.toasts[i], &U.toasts[i + 1], sizeof(Toast) * (size_t)(U.nToasts - i - 1)); U.nToasts--; continue; }
        float a = (float)(age < 0.15 ? age / 0.15 : age > 4.0 ? (4.5 - age) / 0.5 : 1.0);
        float slide = (float)(age < 0.15 ? (1 - age / 0.15) * 12 : 0);
        float w = fminf(fmaxf(ui_text_w(F_REG, 13.5f, o->msg) + 64, 260), 520);
        float h = ui_text_wrap(F_REG, 13.5f, o->msg, (Rectangle){ 0, 0, w - 64, 999 }, 19, t->text, false) + 26;
        Rectangle r = { ui_width() - 20 - w, y - h + slide, w, h };
        /* clicks go through toasts to what is under them; with the mouse there, they fade to show it */
        if (CheckCollisionPointRec(U.mouse, ui_inset(r, -10, -10))) { a *= 0.15f; ui_request_frames(2); }
        Color kc = o->kind == TOAST_OK ? t->ok : o->kind == TOAST_WARN ? t->warn : o->kind == TOAST_ERR ? t->err : t->info;
        const char *ic = o->kind == TOAST_OK ? ICON_CIRCLE_CHECK : o->kind == TOAST_WARN ? ICON_TRIANGLE_ALERT :
                         o->kind == TOAST_ERR ? ICON_CIRCLE_X : ICON_INFO;
        ui_shadow(r, 10, a);
        if (t->lcars) {
            ui_rect(r, 16, ui_alpha(BLACK, a));
            ui_rect_lines(r, 16, 2, ui_alpha(kc, a));
            ui_rect((Rectangle){ r.x + 6, r.y + 6, 12, r.height - 12 }, 6, ui_alpha(kc, a));
        } else {
            ui_rect(r, 10, ui_alpha(t->raised, a));
            ui_rect_lines(r, 10, 1, ui_alpha(t->line, a));
            ui_rect((Rectangle){ r.x, r.y + 10, 3, r.height - 20 }, 1.5f, ui_alpha(kc, a));
        }
        ui_icon(ic, r.x + 26, r.y + 13 + 9, 16, ui_alpha(kc, a));
        ui_text_wrap(F_REG, 13.5f, o->msg, (Rectangle){ r.x + 44, r.y + 13, w - 64, h }, 19, ui_alpha(t->text, a), true);
        y -= h + 10;
    }
}

float ui_take_wheel(void) { float w = U.wheel; U.wheel = 0; return w; }

void ui_end_frame(void)
{
    if (ui_sound_hook && (U.clickHit || U.clickDenied)) ui_sound_hook(U.clickHit ? UI_SND_CLICK : UI_SND_DENIED);
    U.clickHit = U.clickDenied = false;
    const Theme *t = &U.th;
    U.clipOver = 0;
    while (U.nClip) ui_clip_pop();
    /* scroll wheel goes to the innermost scroll area under the mouse */
    if (U.wheelTarget && U.wheel != 0 && !U.pop.open) {
        UiScroll *s = U.wheelTarget;
        float maxY = fmaxf(0, s->contentH - s->viewH);
        s->target -= U.wheel * 80;
        if (s->target < 0) s->target = 0;
        if (s->target > maxY) s->target = maxY;
    }
    if (U.editId && !U.editSeen) U.editId = 0;          /* editor vanished (page changed) */
    if (U.pop.open) {
        U.disabled = false;
        draw_popup();
    }
    U.layer = 0;
    if (U.tipShow && U.tip[0] && !U.pop.open && !U.mDown) {
        float w = fminf(ui_text_w(F_REG, 12.5f, U.tip) + 20, 360);
        float h = ui_text_wrap(F_REG, 12.5f, U.tip, (Rectangle){ 0, 0, w - 20, 999 }, 17, t->text, false) + 14;
        float x = U.mouse.x + 14, y = U.mouse.y + 20;
        if (x + w > ui_width() - 6) x = ui_width() - 6 - w;
        if (y + h > ui_height() - 6) y = U.mouse.y - h - 8;
        Rectangle r = { x, y, w, h };
        ui_shadow(r, 7, 0.8f);
        if (t->lcars) {
            ui_rect(r, 9, t->lc[1]);
            ui_text_wrap(F_REG, 12.5f, U.tip, (Rectangle){ x + 10, y + 7, w - 20, h }, 17, BLACK, true);
        } else {
            ui_rect(r, 7, t->dark ? hex(0x22324D) : hex(0x1E2A40));
            ui_text_wrap(F_REG, 12.5f, U.tip, (Rectangle){ x + 10, y + 7, w - 20, h }, 17, hex(0xEEF2F8), true);
        }
    }
    draw_toasts();
    if (U.mReleased) U.active = 0;
    EndMode2D();
    static int lastCursor = -1;
    if (U.cursor != lastCursor) { SetMouseCursor(U.cursor); lastCursor = U.cursor; }
}

/* ============================================================================ widgets */
float ui_button_width(const char *icon, const char *label)
{
    if (U.th.lcars) {
        if (label && *label) return ui_text_w(F_MED, 14.5f, ui_upper(label)) + 40;
        return 36;
    }
    float w = 24;
    if (icon) w += 16;
    if (label && *label) w += ui_text_w(F_MED, 13.5f, label) + (icon ? 8 : 0);
    else if (icon) w = 34;
    return w;
}

static bool lcars_button(UiId id, Rectangle r, const char *icon, const char *label, ButtonStyle st)
{
    const Theme *t = &U.th;
    bool hov = ui_hovered(r);
    bool down = hov && U.mDown && U.pressLayer == U.layer && CheckCollisionPointRec(U.pressPos, r);
    bool clicked = ui_clicked(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    float h = ui_anim(id, hov ? 1.f : 0.f, 10.f);
    Color base, fg = BLACK;
    bool filled = true;
    switch (st) {
    case BTN_PRIMARY:   base = t->accent; break;
    case BTN_DANGER:    base = t->err; break;
    case BTN_SECONDARY: base = t->lc[1]; break;
    case BTN_ICON:      base = t->lc[2]; filled = h > 0.01f; fg = ui_mix(t->text2, BLACK, h); break;
    default:            base = t->lc[2]; filled = false; fg = ui_mix(t->lc[2], BLACK, h); break;
    }
    Color bg = ui_mix(base, WHITE, 0.22f * h);
    if (down) bg = ui_mix(base, BLACK, 0.15f);
    if (st == BTN_ICON || st == BTN_GHOST) bg = ui_alpha(bg, h);
    if (U.disabled) bg = ui_alpha(bg, 0.35f);
    if (filled || bg.a) ui_pill(r, bg);
    if (st == BTN_GHOST && h < 0.99f) {
        Rectangle i = ui_inset(r, 0.75f, 0.75f);
        DrawRectangleRoundedLinesEx(i, 1.0f, 16, 1.5f, ui_alpha(base, U.disabled ? 0.35f : 0.8f));
    }
    if (U.disabled && st != BTN_GHOST && st != BTN_ICON) fg = ui_alpha(BLACK, 0.8f);
    if (label && *label) {
        const char *L = ui_upper(label);
        float fs = 14.5f, tw = ui_text_w(F_MED, fs, L);
        float tx = r.width > tw + 64 ? r.x + r.width - 18 - tw : r.x + (r.width - tw) / 2;   /* LCARS: labels sit right */
        ui_text(F_MED, fs, L, tx, r.y + (r.height - fs) / 2 - 1, fg);
    } else if (icon) {
        ui_icon(icon, r.x + r.width / 2, r.y + r.height / 2, 16, fg);
    }
    return clicked && !U.disabled;
}

/* Stardew Valley's wooden board, in chunky pixels: a dark outline with notched corners, a lit top and left edge, a shaded
   bottom and right edge, a hard shadow under it; pressed, the light and shade swap and the board sinks into its shadow */
static void sv_board(Rectangle r, Color fill, Color hi, Color lo, Color outline, bool down, bool shadow)
{
    float p = 3;                                             /* one "pixel" of the art */
    r.x = roundf(r.x); r.y = roundf(r.y); r.width = roundf(r.width); r.height = roundf(r.height);
    #define NOTCHED(R, k, C) do { DrawRectangleRec((Rectangle){ (R).x + (k), (R).y, (R).width - 2 * (k), (R).height }, C); \
                                  DrawRectangleRec((Rectangle){ (R).x, (R).y + (k), (R).width, (R).height - 2 * (k) }, C); } while (0)
    if (shadow && !down) NOTCHED(((Rectangle){ r.x + p, r.y + p, r.width, r.height }), p, ui_alpha((Color){ 40, 18, 4, 255 }, 0.45f));
    if (down) { r.x += p * 0.5f; r.y += p; }
    NOTCHED(r, p, outline);
    Rectangle in = { r.x + p, r.y + p, r.width - 2 * p, r.height - 2 * p };
    NOTCHED(in, p, fill);
    Color top = down ? lo : hi, bot = down ? hi : lo;
    DrawRectangleRec((Rectangle){ in.x + p, in.y, in.width - 2 * p, p }, top);                    /* top edge  */
    DrawRectangleRec((Rectangle){ in.x, in.y + p, p, in.height - 2 * p }, top);                   /* left edge */
    DrawRectangleRec((Rectangle){ in.x + p, in.y + in.height - p, in.width - 2 * p, p }, bot);    /* bottom    */
    DrawRectangleRec((Rectangle){ in.x + in.width - p, in.y + p, p, in.height - 2 * p }, bot);    /* right     */
    #undef NOTCHED
}

static bool sv_button(UiId id, Rectangle r, const char *icon, const char *label, ButtonStyle st)
{
    bool hov = ui_hovered(r);
    bool down = hov && U.mDown && U.pressLayer == U.layer && CheckCollisionPointRec(U.pressPos, r);
    bool clicked = ui_clicked(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    float h = ui_anim(id, hov ? 1.f : 0.f, 10.f);
    /* orange planks for the main actions, pale wood for the rest, red for the dangerous ones */
    Color fill, hi, lo, fg, outline = hex(0x5B2B0F);
    bool board = true;
    switch (st) {
    case BTN_PRIMARY:   fill = ui_mix(hex(0xE8973C), hex(0xF5AC52), h); hi = hex(0xFFD08A); lo = hex(0xB0601F); fg = hex(0x4A2108); break;
    case BTN_DANGER:    fill = ui_mix(hex(0xC9452F), hex(0xD9573F), h); hi = hex(0xEE8466); lo = hex(0x8A2A1A); fg = hex(0xFFF1DC); break;
    case BTN_SECONDARY: fill = ui_mix(hex(0xF0CF98), hex(0xF8DCAA), h); hi = hex(0xFFF2D2); lo = hex(0xC9965A); fg = hex(0x4A2108); break;
    default:            /* ghost and icon buttons: bare until hovered, then a pale board */
        fill = hex(0xF8DCAA); hi = hex(0xFFF2D2); lo = hex(0xC9965A); fg = ui_mix(U.th.text2, U.th.text, h);
        board = h > 0.02f || down;
        break;
    }
    if (U.disabled) { fill = ui_mix(fill, hex(0xD9C3A0), 0.6f); hi = ui_mix(hi, fill, 0.6f); lo = ui_mix(lo, fill, 0.6f);
                      outline = ui_mix(outline, hex(0xB89A72), 0.6f); fg = ui_alpha(fg, 0.7f); }
    if (board) sv_board(r, fill, hi, lo, outline, down && !U.disabled, st != BTN_GHOST && st != BTN_ICON);
    float dy = down && !U.disabled ? 3 : 0, dx = down && !U.disabled ? 1.5f : 0;
    UiFamily fam = F_SEMI;
    float fs = 13.5f;
    if (label && *label) {
        float tw = ui_text_w(fam, fs, label);
        float total = tw + (icon ? 16 + 8 : 0);
        float x = r.x + (r.width - total) / 2 + dx;
        if (icon) { ui_icon(icon, x + 8, r.y + r.height / 2 + dy, 15, fg); x += 24; }
        ui_text(fam, fs, label, x, r.y + (r.height - fs) / 2 - 1 + dy, fg);
    } else if (icon) {
        ui_icon(icon, r.x + r.width / 2 + dx, r.y + r.height / 2 + dy, 16, fg);
    }
    return clicked && !U.disabled;
}

bool ui_button(UiId id, Rectangle r, const char *icon, const char *label, ButtonStyle st)
{
    ui_register(id, r, label);
    if (U.th.lcars) return lcars_button(id, r, icon, label, st);
    if (U.th.game == GAME_STARDEW) return sv_button(id, r, icon, label, st);
    const Theme *t = &U.th;
    bool hov = ui_hovered(r);
    bool down = hov && U.mDown && U.pressLayer == U.layer && CheckCollisionPointRec(U.pressPos, r);
    bool clicked = ui_clicked(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    float h = ui_anim(id, hov ? 1.f : 0.f, 8.f);
    Color bg, fg, border = BLANK;
    switch (st) {
    case BTN_PRIMARY:
        bg = ui_mix(t->accent, t->accentHover, h); fg = t->textOnAccent;
        if (down) bg = ui_mix(bg, BLACK, 0.08f);
        break;
    case BTN_DANGER:
        bg = ui_mix(t->errSoft, ui_alpha(t->err, 0.28f), h); fg = t->err; border = ui_alpha(t->err, 0.45f);
        break;
    case BTN_SECONDARY:
        bg = ui_mix(t->surface2, t->raised, h); fg = t->text; border = ui_mix(t->line, t->text3, h * 0.5f);
        if (down) bg = ui_mix(bg, t->hover, 0.6f);
        break;
    default:
        bg = ui_alpha(t->hover, h); fg = ui_mix(t->text2, t->text, h);
        if (down) bg = t->selected;
        break;
    }
    if (U.disabled) { bg = ui_alpha(bg, 0.5f); }
    float rad = st == BTN_ICON ? 7 : 7;
    ui_rect(r, rad, bg);
    if (t->glass && (st == BTN_PRIMARY || st == BTN_SECONDARY) && r.height >= 20) {
        /* a sheen on the top half, as light catches a glass pill */
        Rectangle hi = { r.x + 2, r.y + 1, r.width - 4, r.height * 0.5f };
        ui_rect(hi, rad - 1, ui_alpha(WHITE, st == BTN_PRIMARY ? 0.14f : (t->dark ? 0.05f : 0.35f)));
        if (st == BTN_SECONDARY) border = ui_mix(t->line, t->text3, h * 0.4f);
    }
    if (t->game && (st == BTN_PRIMARY || st == BTN_SECONDARY || st == BTN_DANGER) && r.height >= 20) {
        /* a game key: lit top edge, shaded bottom edge (pressed: swapped), dark outline */
        float e = 3;
        Color lit = ui_alpha(WHITE, 0.45f), shade = ui_alpha(t->frame, 0.35f);
        Rectangle top = { r.x + 2, r.y + 2, r.width - 4, e - 1 }, bot = { r.x + 2, r.y + r.height - e - 1, r.width - 4, e - 1 };
        ui_rect(down ? bot : top, 0, lit);
        ui_rect(down ? top : bot, 0, shade);
        DrawRectangleLinesEx(r, 2, U.disabled ? ui_alpha(t->frame, 0.4f) : st == BTN_DANGER ? t->err : t->frame);
        border = BLANK;
    }
    if (border.a) ui_rect_lines(r, rad, 1, border);
    UiFamily fam = st == BTN_PRIMARY ? F_SEMI : F_MED;
    float fs = 13.5f;
    if (label && *label) {
        float tw = ui_text_w(fam, fs, label);
        float total = tw + (icon ? 16 + 8 : 0);
        float x = r.x + (r.width - total) / 2;
        if (icon) { ui_icon(icon, x + 8, r.y + r.height / 2, 15, fg); x += 24; }
        ui_text(fam, fs, label, x, r.y + (r.height - fs) / 2 - 1, fg);
    } else if (icon) {
        ui_icon(icon, r.x + r.width / 2, r.y + r.height / 2, 16, fg);
    }
    return clicked && !U.disabled;
}

bool ui_checkbox(UiId id, Rectangle r, bool *v, const char *label)
{
    const Theme *t = &U.th;
    Rectangle box = { r.x, r.y + (r.height - 16) / 2, 16, 16 };
    Rectangle hit = { r.x, r.y, 24 + (label ? ui_text_w(F_REG, 13.5f, label) : 0), r.height };
    ui_register(id, hit, label);
    bool hov = ui_hovered(hit);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    bool ch = false;
    if (ui_clicked(hit)) { *v = !*v; ch = true; }
    float k = ui_anim(id, *v ? 1.f : 0.f, 10.f);
    if (t->lcars) {
        Rectangle b = { box.x - 1, box.y - 1, 18, 18 };
        ui_rect(b, 5, ui_mix(t->bg, t->accent, k));
        ui_rect_lines(b, 5, 1.5f, ui_mix(hov ? t->text2 : t->text3, t->accent, k));
        if (k > 0.5f) ui_icon(ICON_CHECK, b.x + 9, b.y + 9, 13, BLACK);
    } else {
        ui_rect(box, 4, ui_mix(t->input, t->accent, k));
        ui_rect_lines(box, 4, 1, ui_mix(hov ? t->text3 : t->line, t->accent, k));
        if (k > 0.5f) ui_icon(ICON_CHECK, box.x + 8, box.y + 8, 12, t->textOnAccent);
    }
    if (label) ui_text(F_REG, 13.5f, label, box.x + 24, r.y + (r.height - 13.5f) / 2 - 1, t->text);
    return ch;
}

bool ui_switch(UiId id, float x, float y, bool *v)
{
    const Theme *t = &U.th;
    Rectangle r = { x, y, 34, 20 };
    ui_register(id, r, NULL);
    bool hov = ui_hovered(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    bool ch = false;
    if (ui_clicked(r)) { *v = !*v; ch = true; }
    float k = ui_anim(id, *v ? 1.f : 0.f, 7.f);
    ui_rect(r, 10, ui_mix(hov ? t->text3 : t->line, t->accent, k));
    float kx = x + 10 + k * 14;
    Color knob = t->lcars ? BLACK : (t->dark ? hex(0xF4F6FA) : WHITE);
    DrawCircleV((Vector2){ kx, y + 10 }, t->lcars ? 6.0f : 7.5f, U.disabled ? ui_alpha(knob, 0.6f) : knob);
    return ch;
}

static bool lcars_pills(UiId id, Rectangle r, const char **items, int n, int *sel, float gap)
{
    const Theme *t = &U.th;
    bool ch = false;
    float w = (r.width - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        Rectangle c = { r.x + i * (w + gap), r.y, w, r.height };
        ui_register(ui_idn(id, i), c, items[i]);
        bool hov = ui_hovered(c), on = i == *sel;
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(c) && !on) { *sel = i; ch = true; }
        Color bg = on ? t->accent : (hov ? hex(0x4A3B63) : hex(0x2E2440));
        if (U.disabled) bg = ui_alpha(bg, 0.4f);
        /* inner segments are square-ended, the outer ends rounded: one LCARS bar */
        float rad = r.height / 2;
        if (n == 1) ui_pill(c, bg);
        else if (i == 0) { ui_pill((Rectangle){ c.x, c.y, c.width, c.height }, bg); DrawRectangleRec((Rectangle){ c.x + c.width - rad, c.y, rad, c.height }, bg); }
        else if (i == n - 1) { ui_pill(c, bg); DrawRectangleRec((Rectangle){ c.x, c.y, rad, c.height }, bg); }
        else DrawRectangleRec(c, bg);
        const char *L = ui_upper(items[i]);
        ui_text_center(F_MED, 14, L, c, on ? BLACK : (hov ? t->text : t->text2));
    }
    return ch;
}

bool ui_segmented(UiId id, Rectangle r, const char **items, int n, int *sel)
{
    const Theme *t = &U.th;
    if (t->lcars) return lcars_pills(id, r, items, n, sel, 4);
    ui_rect(r, 8, t->input);
    ui_rect_lines(r, 8, 1, t->lineSoft);
    float w = (r.width - 6) / n;
    bool ch = false;
    if (*sel >= 0) {                                    /* -1: nothing of this row chosen (the theme picker's two rows) */
        float k = ui_anim(id, (float)*sel, 12.f);
        Rectangle pill = { r.x + 3 + k * w, r.y + 3, w, r.height - 6 };
        ui_rect(pill, 6, t->dark ? t->raised : t->surface);
        ui_rect_lines(pill, 6, 1, t->line);
    }
    for (int i = 0; i < n; i++) {
        Rectangle c = { r.x + 3 + i * w, r.y + 3, w, r.height - 6 };
        ui_register(ui_idn(id, i), c, items[i]);
        bool hov = ui_hovered(c);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(c) && *sel != i) { *sel = i; ch = true; }
        ui_text_center(i == *sel ? F_MED : F_REG, 13, items[i], c, i == *sel ? t->text : (hov ? t->text : t->text2));
    }
    return ch;
}

bool ui_tabs(UiId id, float x, float y, const char **items, int n, int *sel, float *outH)
{
    const Theme *t = &U.th;
    bool ch = false;
    if (t->lcars) {
        float cx = x;
        for (int i = 0; i < n; i++) {
            const char *L = ui_upper(items[i]);
            float w = ui_text_w(F_MED, 14, L) + 40;
            Rectangle r = { cx, y + 1, w, 30 };
            ui_register(ui_idn(id, i), r, items[i]);
            bool hov = ui_hovered(r), on = i == *sel;
            if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
            if (ui_clicked(r) && !on) { *sel = i; ch = true; }
            ui_pill(r, on ? t->accent : (hov ? hex(0x4A3B63) : hex(0x2E2440)));
            ui_text(F_MED, 14, L, r.x + r.width - 16 - ui_text_w(F_MED, 14, L), r.y + 8, on ? BLACK : (hov ? t->text : t->text2));
            cx += w + 6;
        }
        (void)id;
        if (outH) *outH = 34;
        return ch;
    }
    float cx = x;
    for (int i = 0; i < n; i++) {
        float w = ui_text_w(F_MED, 14, items[i]) + 4;
        Rectangle r = { cx - 2, y, w, 34 };
        ui_register(ui_idn(id, i), r, items[i]);
        bool hov = ui_hovered(r);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (ui_clicked(r) && *sel != i) { *sel = i; ch = true; }
        ui_text(F_MED, 14, items[i], cx, y + 7, i == *sel ? t->text : (hov ? t->text : t->text3));
        if (i == *sel) ui_rect((Rectangle){ cx - 2, y + 31, w, 2.5f }, 1.2f, t->accent);
        cx += w + 22;
    }
    (void)id;
    if (outH) *outH = 34;
    return ch;
}

/* ---------------------------------------------------------------- text input */
static int prev_cp(const char *s, int i) { if (i <= 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; return i; }
static int next_cp(const char *s, int i) { int n = (int)strlen(s); if (i >= n) return n; i++; while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++; return i; }

static void edit_delete_sel(void)
{
    int a = U.editAnchor < U.editCur ? U.editAnchor : U.editCur;
    int b = U.editAnchor < U.editCur ? U.editCur : U.editAnchor;
    if (a == b) return;
    memmove(U.editBuf + a, U.editBuf + b, strlen(U.editBuf + b) + 1);
    U.editCur = U.editAnchor = a;
}

static void edit_insert(const char *s)
{
    edit_delete_sel();
    size_t n = strlen(s), L = strlen(U.editBuf);
    if (L + n + 1 > sizeof U.editBuf) n = sizeof U.editBuf - L - 1;
    memmove(U.editBuf + U.editCur + n, U.editBuf + U.editCur, L - (size_t)U.editCur + 1);
    memcpy(U.editBuf + U.editCur, s, n);
    U.editCur += (int)n;
    U.editAnchor = U.editCur;
}

static int pos_from_x(UiFamily f, float fs, const char *s, float x)
{
    int best = 0;
    float bestD = 1e9f;
    char tmp[2048];
    int n = (int)strlen(s);
    for (int i = 0; i <= n; i = next_cp(s, i)) {
        memcpy(tmp, s, (size_t)i); tmp[i] = 0;
        float d = fabsf(ui_text_w(f, fs, tmp) - x);
        if (d < bestD) { bestD = d; best = i; }
        if (i == n) break;
    }
    return best;
}

bool ui_input(UiId id, Rectangle r, char *buf, int cap, const char *ph, int flags)
{
    const Theme *t = &U.th;
    UiFamily fam = (flags & INPUT_MONO) ? F_MONO : F_REG;
    float fs = (flags & INPUT_MONO) ? 13 : 13.5f;
    float padL = (flags & INPUT_SEARCH) ? 32 : (t->lcars ? 16 : 10);
    ui_register(id, r, ph);
    bool hov = ui_hovered(r);
    bool editing = U.editId == id;
    bool changed = false;
    if (hov && !(flags & INPUT_READONLY)) ui_cursor(MOUSE_CURSOR_IBEAM);
    float textX = r.x + padL;

    bool focusedNow = false;
    static bool selectHold;                              /* select-all on focus lasts while that click is held */
    if (!U.mDown) selectHold = false;
    bool asked = focusReq == id;
    if (asked) focusReq = 0;
    if (asked && editing) {                              /* focused already: take the new text the caller put in buf */
        snprintf(U.editBuf, sizeof U.editBuf, "%s", buf);
        U.editCur = U.editAnchor = (int)strlen(U.editBuf);
    }
    if (!editing && ((hov && U.mPressed) || asked) && !(flags & INPUT_READONLY) && !U.disabled) {
        focusedNow = true;                               /* this click focused it: don't also treat it as a cursor click */
        U.editId = id;
        editing = true;
        snprintf(U.editBuf, sizeof U.editBuf, "%s", buf);
        snprintf(U.editOrig, sizeof U.editOrig, "%s", buf);
        U.editFlags = flags;
        U.editScroll = 0;
        U.editCur = U.editAnchor = asked ? (int)strlen(U.editBuf) : pos_from_x(fam, fs, U.editBuf, U.mouse.x - textX);
        if (U.mDouble || (flags & INPUT_NUMERIC)) { U.editAnchor = 0; U.editCur = (int)strlen(U.editBuf); selectHold = true; }
        U.blinkT = U.time;
        U.active = id;
    }

    if (editing) {
        U.editSeen = true;
        bool commit = false, cancel = false;
        if (U.mPressed && !hov && U.layer == U.inputLayer) commit = true;
        else if (U.mPressed && hov && !focusedNow) {
            int p = pos_from_x(fam, fs, U.editBuf, U.mouse.x - textX + U.editScroll);
            if (U.mDouble) { U.editAnchor = 0; U.editCur = (int)strlen(U.editBuf); selectHold = true; }
            else { U.editCur = p; if (!inp_key_down(KEY_LEFT_SHIFT) && !inp_key_down(KEY_RIGHT_SHIFT)) U.editAnchor = p; }
            U.active = id;
            U.blinkT = U.time;
        } else if (U.mDown && U.active == id && !selectHold) {
            U.editCur = pos_from_x(fam, fs, U.editBuf, U.mouse.x - textX + U.editScroll);
        }
        if (!U.pop.open) {
            bool ctrl = inp_key_down(KEY_LEFT_CONTROL) || inp_key_down(KEY_RIGHT_CONTROL);
            bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
            int c;
            while ((c = in_char()) > 0) {
                if (c < 32 || ctrl) continue;
                char u8[8]; int nb = 0;
                const char *enc = CodepointToUTF8(c, &nb);
                memcpy(u8, enc, (size_t)nb); u8[nb] = 0;
                edit_insert(u8);
                U.blinkT = U.time;
            }
            int n = (int)strlen(U.editBuf);
            #define KP(k) (inp_key_repeat(k))
            if (KP(KEY_BACKSPACE)) {
                if (U.editAnchor != U.editCur) edit_delete_sel();
                else if (U.editCur > 0) { int p = prev_cp(U.editBuf, U.editCur); memmove(U.editBuf + p, U.editBuf + U.editCur, (size_t)(n - U.editCur) + 1); U.editCur = U.editAnchor = p; }
                U.blinkT = U.time;
            }
            if (KP(KEY_DELETE)) {
                if (U.editAnchor != U.editCur) edit_delete_sel();
                else if (U.editCur < n) { int q = next_cp(U.editBuf, U.editCur); memmove(U.editBuf + U.editCur, U.editBuf + q, (size_t)(n - q) + 1); }
                U.blinkT = U.time;
            }
            if (KP(KEY_LEFT)) {
                if (!shift && U.editAnchor != U.editCur) U.editCur = U.editAnchor = (U.editCur < U.editAnchor ? U.editCur : U.editAnchor);
                else {
                    U.editCur = prev_cp(U.editBuf, U.editCur);
                    if (ctrl) while (U.editCur > 0 && U.editBuf[U.editCur - 1] != ' ') U.editCur = prev_cp(U.editBuf, U.editCur);
                    if (!shift) U.editAnchor = U.editCur;
                }
                U.blinkT = U.time;
            }
            if (KP(KEY_RIGHT)) {
                if (!shift && U.editAnchor != U.editCur) U.editCur = U.editAnchor = (U.editCur > U.editAnchor ? U.editCur : U.editAnchor);
                else {
                    U.editCur = next_cp(U.editBuf, U.editCur);
                    if (ctrl) while (U.editCur < (int)strlen(U.editBuf) && U.editBuf[U.editCur] != ' ') U.editCur = next_cp(U.editBuf, U.editCur);
                    if (!shift) U.editAnchor = U.editCur;
                }
                U.blinkT = U.time;
            }
            if (inp_key_pressed(KEY_HOME)) { U.editCur = 0; if (!shift) U.editAnchor = 0; }
            if (inp_key_pressed(KEY_END)) { U.editCur = (int)strlen(U.editBuf); if (!shift) U.editAnchor = U.editCur; }
            if (ctrl && inp_key_pressed(KEY_A)) { U.editAnchor = 0; U.editCur = (int)strlen(U.editBuf); }
            if (ctrl && (inp_key_pressed(KEY_C) || inp_key_pressed(KEY_X)) && U.editAnchor != U.editCur) {
                int a = U.editAnchor < U.editCur ? U.editAnchor : U.editCur, b = U.editAnchor < U.editCur ? U.editCur : U.editAnchor;
                char tmp[2048]; memcpy(tmp, U.editBuf + a, (size_t)(b - a)); tmp[b - a] = 0;
                SetClipboardText(tmp);
                if (inp_key_pressed(KEY_X)) edit_delete_sel();
            }
            if (ctrl && inp_key_pressed(KEY_V)) {
                const char *cb = GetClipboardText();
                if (cb) {
                    char tmp[2048]; int k = 0;
                    for (const char *q = cb; *q && k < 2047; q++) if (*q != '\n' && *q != '\r') tmp[k++] = *q == '\t' ? ' ' : *q;
                    tmp[k] = 0;
                    edit_insert(tmp);
                }
            }
            if (in_enter()) enteredId = id;
            if (in_enter() || inp_key_pressed(KEY_TAB)) commit = true;
            if (inp_key_pressed(KEY_ESCAPE)) cancel = true;
            #undef KP
        }
        if (flags & INPUT_LIVE) {
            if (strcmp(buf, U.editBuf) != 0) { snprintf(buf, (size_t)cap, "%s", U.editBuf); changed = true; }
            if (cancel) { if (U.editBuf[0]) { buf[0] = 0; changed = true; } U.editId = 0; editing = false; }
            if (commit && !((flags & INPUT_KEEP) && enteredId == id)) { U.editId = 0; editing = false; }
        } else if (cancel) {
            U.editId = 0; editing = false;
        } else if (commit) {
            if (strcmp(buf, U.editBuf) != 0) { snprintf(buf, (size_t)cap, "%s", U.editBuf); changed = true; }
            U.editId = 0; editing = false;
        }
    }

    /* draw */
    float f = ui_anim(id, editing ? 1.f : 0.f, 10.f);
    if (t->lcars && (!(flags & INPUT_BORDERLESS) || editing || hov)) {
        float rad = r.height / 2;
        ui_rect(r, rad, (flags & INPUT_READONLY) ? t->surface2 : (hov && !editing ? t->inputHover : t->input));
        Color bc = ui_mix(hov ? t->text3 : t->line, t->accent, f);
        Rectangle i = ui_inset(r, 0.75f, 0.75f);
        DrawRectangleRoundedLinesEx(i, 1.0f, 16, f > 0.5f ? 2.0f : 1.5f, bc);
    } else if (!(flags & INPUT_BORDERLESS) || editing || hov) {
        if (f > 0.01f) ui_rect(ui_inset(r, -3, -3), 9, ui_alpha(t->accentSoft, f));
        ui_rect(r, 7, (flags & INPUT_READONLY) ? t->surface2 : (hov && !editing ? t->inputHover : t->input));
        Color bc = ui_mix(hov ? ui_mix(t->line, t->text3, 0.5f) : t->line, t->accent, f);
        ui_rect_lines(r, 7, 1, bc);
    }
    if (flags & INPUT_SEARCH) ui_icon(ICON_SEARCH, r.x + 17, r.y + r.height / 2, 15, t->text3);
    ui_clip_push(t->lcars ? (Rectangle){ r.x + 8, r.y, r.width - 16, r.height } : (Rectangle){ r.x + 2, r.y, r.width - 4, r.height });
    float ty = r.y + (r.height - fs) / 2 - 1;
    const char *shown = editing ? U.editBuf : buf;
    float innerW = r.width - padL - 10;
    if (editing) {
        char tmp[2048];
        memcpy(tmp, U.editBuf, (size_t)U.editCur); tmp[U.editCur] = 0;
        float cx = ui_text_w(fam, fs, tmp);
        if (cx - U.editScroll > innerW) U.editScroll = cx - innerW;
        if (cx - U.editScroll < 0) U.editScroll = cx;
        if (U.editAnchor != U.editCur) {
            int a = U.editAnchor < U.editCur ? U.editAnchor : U.editCur, b = U.editAnchor < U.editCur ? U.editCur : U.editAnchor;
            memcpy(tmp, U.editBuf, (size_t)a); tmp[a] = 0;
            float xa = ui_text_w(fam, fs, tmp);
            memcpy(tmp, U.editBuf, (size_t)b); tmp[b] = 0;
            float xb = ui_text_w(fam, fs, tmp);
            ui_rect((Rectangle){ textX + xa - U.editScroll, r.y + 5, xb - xa, r.height - 10 }, 2, ui_alpha(t->accent, 0.35f));
        }
        ui_text(fam, fs, shown, textX - U.editScroll, ty, t->text);
        if (fmod(U.time - U.blinkT, 1.0) < 0.55)
            DrawRectangleRec((Rectangle){ snap(textX + cx - U.editScroll), r.y + 7, 1.25f, r.height - 14 }, t->accent);
    } else if (shown[0]) {
        ui_text_clip(fam, fs, shown, textX, ty, innerW, (flags & INPUT_READONLY) ? t->text2 : t->text);
    } else if (ph) {
        ui_text_clip(F_REG, 13.5f, ph, textX, r.y + (r.height - 13.5f) / 2 - 1, innerW, t->text3);
    }
    ui_clip_pop();
    return changed;
}

/* ---------------------------------------------------------------- dropdown */
bool ui_dropdown(UiId id, Rectangle r, const char *current, const char *const *items, int n, int flags,
                 char *out, int cap, const char *ph)
{
    const Theme *t = &U.th;
    if (U.pop.resultReady && U.pop.resultOwner == id) {
        U.pop.resultReady = false;
        if (out) snprintf(out, (size_t)cap, "%s", U.pop.result);
        return true;
    }
    bool open = U.pop.open && U.pop.owner == id;
    ui_register(id, r, current && *current ? current : ph);
    bool hov = ui_hovered(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    if (ui_clicked(r) && !U.disabled) {
        if (open) U.pop.open = false;
        else {
            Popup *p = &U.pop;
            memset(p->items, 0, sizeof p->items);
            p->open = true; p->owner = id; p->layer = U.layer; p->anchor = r; p->flags = flags;
            p->n = n < MAX_POPUP_ITEMS ? n : MAX_POPUP_ITEMS;
            p->hover = 0; p->scroll = 0; p->filter[0] = 0; p->justOpened = false;
            for (int i = 0; i < p->n; i++) {
                snprintf(p->items[i], sizeof p->items[i], "%s", items[i]);
                if (current && !strcmp(items[i], current)) p->hover = i + ((flags & DD_CUSTOM) ? 0 : 0);
            }
            p->rect = (Rectangle){ r.x, r.y + r.height, r.width, 10 };
            if (U.editId) U.editId = 0;
            open = true;
        }
    }
    float f = ui_anim(id, open ? 1.f : 0.f, 10.f);
    UiFamily fam = (flags & DD_MONO) ? F_MONO : F_REG;
    float fs = (flags & DD_MONO) ? 13 : 13.5f;
    const char *txt = current && *current ? current : ph;
    if (t->lcars) {
        float rad = r.height / 2, capW = 36;
        ui_rect(r, rad, hov ? t->inputHover : t->input);
        DrawRectangleRoundedLinesEx(ui_inset(r, 0.75f, 0.75f), 1.0f, 16, f > 0.5f ? 2.0f : 1.5f, ui_mix(hov ? t->text3 : t->line, t->accent, f));
        /* a coloured end cap carries the chevron */
        Rectangle cap = { r.x + r.width - capW, r.y, capW, r.height };
        Color cc = ui_mix(open ? t->accent : t->lc[1], WHITE, hov ? 0.2f : 0.0f);
        if (U.disabled) cc = ui_alpha(cc, 0.35f);
        ui_pill(cap, cc);
        DrawRectangleRec((Rectangle){ cap.x, cap.y, rad, cap.height }, cc);
        ui_icon(ICON_CHEVRON_DOWN, cap.x + capW / 2 - 1, r.y + r.height / 2, 14, BLACK);
        ui_text_clip(fam, fs, txt ? txt : "", r.x + 16, r.y + (r.height - fs) / 2 - 1, r.width - capW - 24,
                     current && *current ? t->text : t->text3);
        return false;
    }
    if (f > 0.01f) ui_rect(ui_inset(r, -3, -3), 9, ui_alpha(t->accentSoft, f));
    ui_rect(r, 7, hov ? t->inputHover : t->input);
    ui_rect_lines(r, 7, 1, ui_mix(hov ? ui_mix(t->line, t->text3, 0.5f) : t->line, t->accent, f));
    ui_text_clip(fam, fs, txt ? txt : "", r.x + 10, r.y + (r.height - fs) / 2 - 1, r.width - 38,
                 current && *current ? t->text : t->text3);
    ui_icon(ICON_CHEVRON_DOWN, r.x + r.width - 16, r.y + r.height / 2, 14, t->text3);
    return false;
}

bool ui_select(UiId id, Rectangle r, const char *const *items, int n, int *sel)
{
    char out[256];
    const char *cur = (*sel >= 0 && *sel < n) ? items[*sel] : "";
    if (ui_dropdown(id, r, cur, items, n, 0, out, sizeof out, "Select")) {
        for (int i = 0; i < n; i++) if (!strcmp(items[i], out)) { bool ch = *sel != i; *sel = i; return ch; }
    }
    return false;
}

/* ---------------------------------------------------------------- small pieces */
float ui_badge_width(const char *icon, const char *text)
{
    if (U.th.lcars) return ui_text_w(F_MED, 12.5f, ui_cap_prose(text)) + 22;
    return ui_text_w(F_MED, 12, text) + 18 + (icon ? 17 : 0);
}

float ui_badge(float x, float y, const char *icon, const char *text, Color fg, Color bg)
{
    float w = ui_badge_width(icon, text);
    if (U.th.lcars) {                          /* solid pill, black capitals */
        Rectangle r = { x, y, w, 22 };
        ui_pill(r, U.disabled ? ui_alpha(fg, 0.4f) : fg);
        ui_text(F_MED, 12.5f, ui_cap_prose(text), x + 11, y + 4, BLACK);
        return w;
    }
    Rectangle r = { x, y, w, 22 };
    ui_rect(r, 11, bg);
    float tx = x + 9;
    if (icon) { ui_icon(icon, tx + 6, y + 11, 13, fg); tx += 17; }
    ui_text(F_MED, 12, text, tx, y + 4, fg);
    return w;
}

void ui_progress(Rectangle r, float frac, Color c)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    ui_rect(r, r.height / 2, U.th.lineSoft);
    if (frac > 0) ui_rect((Rectangle){ r.x, r.y, fmaxf(r.height, r.width * frac), r.height }, r.height / 2, c);
    if (U.th.game && r.height >= 4) DrawRectangleLinesEx((Rectangle){ r.x - 1, r.y - 1, r.width + 2, r.height + 2 }, 1, U.th.frame);   /* the HP-bar outline */
}

void ui_spinner(float cx, float cy, float rad, Color c)
{
    float a = (float)fmod(U.time * 360.0, 360.0);
    DrawRing((Vector2){ cx, cy }, rad - 2, rad, a, a + 270, 24, c);
}

bool ui_row(UiId id, Rectangle r, bool selected)
{
    const Theme *t = &U.th;
    ui_register(id, r, NULL);
    bool hov = ui_hovered(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    float h = ui_anim(id, hov ? 1.f : 0.f, 9.f);
    float rad = t->lcars ? fminf(r.height / 2, 14) : 7;
    if (selected) ui_rect(r, rad, t->selected);
    else if (h > 0.01f) ui_rect(r, rad, ui_alpha(t->hover, h));
    return ui_clicked(r);
}

void ui_tooltip(Rectangle r, const char *text)
{
    if (!text || !ui_hovered(r)) return;
    UiId key = fnv(&r, sizeof r, 7);
    if (key != U.tipKey) { U.tipKey = key; U.tipStart = U.time; }
    if (U.time - U.tipStart > 0.45) { U.tipShow = true; snprintf(U.tip, sizeof U.tip, "%s", text); }
}

/* ---------------------------------------------------------------- scroll */
float ui_scroll_begin(UiScroll *s, Rectangle view)
{
    s->view = view;
    s->viewH = view.height;
    float d = s->target - s->y;
    s->y += d * fminf(1.0f, (float)U.dt * 16.0f);
    if (fabsf(d) < 0.3f) s->y = s->target; else ui_request_frames(2);
    if (ui_hovered(view)) U.wheelTarget = s;
    ui_clip_push(view);
    return view.y - roundf(s->y);
}

void ui_scroll_end(UiScroll *s, float contentH)
{
    const Theme *t = &U.th;
    if (s->y > 1 && !t->glass && !t->hud) {           /* scrolled: fade content out under the top edge */
        Color bg = t->bg, clear = t->bg; clear.a = 0;
        DrawRectangleGradientV((int)s->view.x, (int)s->view.y, (int)s->view.width, 16, bg, clear);
    }
    ui_clip_pop();
    s->contentH = contentH;
    float maxY = fmaxf(0, contentH - s->viewH);
    if (s->target > maxY) s->target = maxY;
    if (s->y > maxY + 1) s->y = maxY;
    if (maxY <= 0) { s->y = s->target = 0; return; }
    Rectangle v = s->view;
    float th = fmaxf(30, v.height * v.height / contentH);
    float ty = v.y + (v.height - th) * (s->y / maxY);
    Rectangle track = { v.x + v.width - 9, v.y, 9, v.height };
    Rectangle thumb = { v.x + v.width - 7, ty + 2, 5, th - 4 };
    bool hov = ui_hovered(track);
    if (hov && U.mPressed && CheckCollisionPointRec(U.mouse, thumb)) { s->dragging = true; s->dragOff = U.mouse.y - ty; }
    if (!U.mDown) s->dragging = false;
    if (s->dragging) {
        float nt = (U.mouse.y - s->dragOff - v.y) / (v.height - th) * maxY;
        s->target = s->y = fmaxf(0, fminf(maxY, nt));
    }
    float a = ui_anim(ui_idp(s), (hov || s->dragging) ? 1.f : (ui_hovered(v) ? 0.55f : 0.0f), 4.f);
    if (a > 0.01f) ui_rect(thumb, 2.5f, ui_alpha(t->lcars ? t->lc[1] : t->text3, 0.75f * a));
}

void ui_scroll_to(UiScroll *s, float y) { s->target = y < 0 ? 0 : y; }

/* ---------------------------------------------------------------- modal */
static Rectangle modalCard;
/* a full-screen moment drawn by the caller: takes the input like a modal, draws no card */
void ui_modal_layer(void) { U.modalThis = true; U.layer = 1; }
Rectangle ui_modal_begin(const char *title, const char *subtitle, float w, float h, bool *close)
{
    const Theme *t = &U.th;
    U.modalThis = true;
    U.layer = 1;
    float sw = ui_width(), sh = ui_height();
    if (h > sh - 40) h = sh - 40;
    if (w > sw - 40) w = sw - 40;
    float k = ui_anim(ui_id("##modal"), 1.f, 6.f);
    DrawRectangleRec((Rectangle){ 0, 0, sw, sh }, ui_alpha(t->dark ? hex(0x03070D) : hex(0x1A2436), (t->lcars ? 0.78f : 0.55f) * k));
    Rectangle c = { roundf((sw - w) / 2), roundf((sh - h) / 2 + (1 - k) * 10), w, h };
    modalCard = c;
    ui_shadow(c, 14, 2.0f);
    if (t->lcars) {
        ui_rect(c, 22, BLACK);
        ui_rect_lines(c, 22, 2, t->lc[1]);
        ui_rect((Rectangle){ c.x + 10, c.y + 10, 10, c.height - 20 }, 5, t->accent);    /* side rail */
        ui_text(F_SEMI, 22, ui_cap_words(title), c.x + 34, c.y + 18, t->accent);
    } else {
        if (t->hud) ui_rect(c, 14, t->raised);         /* JARVIS: a near-opaque pane, the brackets come with the border */
        else if (t->glass) {                           /* frosted: the page behind blurs out under a near-opaque pane */
            ui_rect(c, 14, t->dark ? (Color){ 26, 30, 42, 245 } : (Color){ 246, 247, 251, 245 });
            ui_rect(c, 14, t->surface);
        } else ui_rect(c, 14, t->surface);
        ui_rect_lines(c, 14, 1, t->line);
        ui_text(F_SEMI, 18, ui_cap_words(title), c.x + 24, c.y + 20, t->text);
    }
    float top = 52;
    if (subtitle && *subtitle) {
        float sh2 = ui_text_wrap(F_REG, 13.5f, subtitle, (Rectangle){ c.x + (t->lcars ? 34 : 24), c.y + 48, w - 80, 200 }, 19, t->text2, true);
        top = 48 + sh2 + 6;
    }
    if (ui_button(ui_id("##modal-x"), (Rectangle){ c.x + w - 44, c.y + 14, 30, 30 }, ICON_X, NULL, BTN_ICON) && close) *close = true;
    if (close && U.layer == U.inputLayer) {
        if (inp_key_pressed(KEY_ESCAPE) && !U.editId) *close = true;
        if (U.mPressed && !CheckCollisionPointRec(U.mouse, c)) *close = true;
    }
    if (t->lcars) return (Rectangle){ c.x + 34, c.y + top + 8, w - 58, h - top - 8 - 20 };
    return (Rectangle){ c.x + 24, c.y + top + 8, w - 48, h - top - 8 - 20 };
}
void ui_modal_end(void) { U.layer = 0; }
bool ui_modal_active(void) { return U.modalThis || U.modalPrev; }

/* ---------------------------------------------------------------- toasts */
void ui_toast(int kind, const char *fmt, ...)
{
    char msg[sizeof U.toasts[0].msg];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    for (int i = 0; i < U.nToasts; i++)                   /* the same message again: refresh it rather than stack a copy */
        if (U.toasts[i].kind == kind && !strcmp(U.toasts[i].msg, msg)) { U.toasts[i].t0 = U.time > 0 ? U.time : GetTime(); return; }
    if (ui_sound_hook && kind == TOAST_ERR) ui_sound_hook(UI_SND_ERROR);
    if (U.nToasts == MAX_TOASTS) { memmove(&U.toasts[0], &U.toasts[1], sizeof(Toast) * (MAX_TOASTS - 1)); U.nToasts--; }
    Toast *o = &U.toasts[U.nToasts++];
    o->kind = kind;
    o->t0 = U.time > 0 ? U.time : GetTime();
    memcpy(o->msg, msg, sizeof o->msg);
}
