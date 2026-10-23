/* ui.h -- a small immediate-mode UI toolkit on raylib with a web-style look.
 *
 * All layout is in logical pixels. The whole frame is drawn through a Camera2D zoomed by the UI
 * scale, and fonts are rasterised at physical size, so text stays crisp at 100-200 %.
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"
#include "icons.h"
#include "input.h"

typedef uint32_t UiId;

typedef enum { F_REG = 0, F_MED, F_SEMI, F_MONO, F_MONO_MED, F_ICON, F_COUNT } UiFamily;

typedef struct {
    Color bg, sidebar, surface, surface2, raised, input, inputHover, line, lineSoft, hover, selected;
    Color text, text2, text3, textOnAccent;
    Color accent, accentHover, accentSoft;
    Color info, ok, warn, err, okSoft, warnSoft, errSoft, infoSoft;
    Color cat[5];
    Color chartBg, grid;
    Color series[10];
    Color shadow;
    bool  dark;
    bool  lcars;                       /* the LCARS-style interface: pills, elbows, condensed caps */
    int   game;                        /* GAME_*: a game-menu look on the classic layout: framed boxes, pixel labels */
    Color frame, frameIn;              /* the game themes' box border and its inner highlight */
    Color lc[8];                       /* LCARS block colours, cycled through the frame            */
    bool  glass;                       /* the classic themes: frosted translucent panels over a soft colour backdrop (iOS) */
    bool  hud;                         /* JARVIS: a holographic HUD, thin cyan lines with corner brackets over a dark grid */
    int   retro;                       /* RETRO_*: the screen-and-game themes (Severance ... Windows 95), on the classic layout */
    float crt;                         /* 0 none, else the strength of the CRT pass (curvature, scanlines, glow, flicker) */
    bool  upper;                       /* labels and text in capitals (the identifiers in mono keep their case) */
} Theme;

/* the themes, as Settings stores them */
enum { THEME_LCARS = 0, THEME_DARK, THEME_LIGHT, THEME_STARDEW, THEME_POKEMON, THEME_JARVIS,
       THEME_SEVERANCE, THEME_ALIEN, THEME_PIPBOY, THEME_NERV, THEME_PORTAL, THEME_SOULS, THEME_FACTORIO, THEME_WIN95, THEME_STARCITIZEN, THEME__N };
enum { RETRO_NONE = 0, RETRO_SEVERANCE, RETRO_ALIEN, RETRO_PIPBOY, RETRO_NERV, RETRO_PORTAL, RETRO_SOULS, RETRO_FACTORIO, RETRO_WIN95, RETRO_STARCITIZEN };
int  ui_theme_id(void);                  /* THEME_* now shown */
enum { GAME_NONE = 0, GAME_STARDEW, GAME_POKEMON };

typedef enum { BTN_PRIMARY = 0, BTN_SECONDARY, BTN_GHOST, BTN_DANGER, BTN_ICON } ButtonStyle;

enum { INPUT_MONO = 1, INPUT_LIVE = 2, INPUT_NUMERIC = 4, INPUT_SEARCH = 8, INPUT_READONLY = 16, INPUT_BORDERLESS = 32,
       INPUT_KEEP = 64 /* with INPUT_LIVE: Enter keeps the focus (find next) */ };
enum { DD_FILTER = 1, DD_CUSTOM = 2, DD_MONO = 4 };
enum { TOAST_INFO = 0, TOAST_OK, TOAST_WARN, TOAST_ERR };

typedef struct { float y, target, contentH, viewH; bool dragging; float dragOff; Rectangle view; } UiScroll;

/* ---------------------------------------------------------------- frame */
void  ui_init(float scale, int theme);
void  ui_set_scale(float scale);
void  ui_set_theme(int theme);
void  ui_shutdown(void);
void  ui_begin_frame(void);
void  ui_end_frame(void);              /* draws popups, tooltips, toasts; sets cursor */
/* sound cues the app may turn into sounds: at most one click cue a frame (from ui_end_frame), an error toast */
typedef enum { UI_SND_CLICK, UI_SND_DENIED, UI_SND_ERROR } UiSound;
extern void (*ui_sound_hook)(UiSound s);
float ui_scale(void);
float ui_width(void);                  /* logical window size */
float ui_height(void);
double ui_time(void);
const Theme *ui_theme(void);
bool ui_crt_begin(void);                 /* the CRT themes: draw the frame into a texture (after BeginDrawing) */
void ui_crt_end(void);                   /* ... and onto the screen through the tube (before EndDrawing) */
void ui_backdrop(float w, float h);       /* glass themes: the colour field the translucent panels sit on; JARVIS: its grid and reactor rings */
#define TH (*ui_theme())

/* ---------------------------------------------------------------- ids */
UiId ui_id(const char *s);
UiId ui_idp(const void *p);
UiId ui_idn(UiId base, int n);
void ui_push_id(UiId id);
void ui_pop_id(void);

/* ---------------------------------------------------------------- state */
void ui_set_layer(int layer);          /* 0 page, 1 modal */
bool ui_mouse_ok(void);
bool ui_hovered(Rectangle r);
bool ui_clicked(Rectangle r);          /* pressed and released inside, on this layer */
bool ui_pressed(Rectangle r);
Vector2 ui_mouse(void);
void ui_disable(bool on);              /* greys out and blocks the widgets that follow */
bool ui_any_input_focused(void);
bool ui_input_entered(UiId id);        /* Enter was pressed in it this frame (check after drawing it) */
bool ui_input_focused(UiId id);
void ui_focus_input(UiId id);          /* give the input the keyboard when it is next drawn */
void ui_cursor(int cursor);
void ui_request_frames(int n);        /* keep animating even without input */
bool  ui_awake(void);                     /* something is changing on screen: draw at the full frame rate */

/* ---------------------------------------------------------------- drawing */
Font  ui_font(UiFamily f, float size);
void  ui_text(UiFamily f, float size, const char *s, float x, float y, Color c);
float ui_text_w(UiFamily f, float size, const char *s);
void  ui_text_center(UiFamily f, float size, const char *s, Rectangle r, Color c);
void  ui_text_clip(UiFamily f, float size, const char *s, float x, float y, float maxW, Color c);
float ui_text_wrap(UiFamily f, float size, const char *s, Rectangle r, float lineH, Color c, bool draw);
void  ui_icon(const char *icon, float cx, float cy, float size, Color c);
void  ui_rect(Rectangle r, float radius, Color c);
void  ui_rect_lines(Rectangle r, float radius, float thick, Color c);
void  ui_shadow(Rectangle r, float radius, float strength);
void  ui_line(float x0, float y0, float x1, float y1, float thick, Color c);
void  ui_clip_push(Rectangle r);
void  ui_clip_pop(void);
void  ui_restore_2d(void);             /* after drawing into a render texture */
Rectangle ui_clip_rect(void);
Color ui_mix(Color a, Color b, float t);
Color ui_alpha(Color c, float a);
float ui_anim(UiId id, float target, float speed);
const char *ui_upper(const char *s);   /* uppercase copy (rotating static buffers) for LCARS labels */
const char *ui_cap(const char *s);     /* ui_upper() in the LCARS style, the string itself otherwise */
const char *ui_cap_prose(const char *s);  /* as ui_cap, but leaves identifiers (movingWall, k) alone */
const char *ui_cap_words(const char *s);  /* LCARS capitals per word; identifiers (`marked`, icoFoam, log.x, k) keep their case */
void  ui_pill(Rectangle r, Color c);   /* fully rounded ends */
/* LCARS elbow: a column (x..x+cw, y..y+h) joined to a bar of thickness barH running right to x+w,
   at the bottom (barBottom) or top of the column; R outer radius, r inner (concave) radius */
void  ui_elbow(float x, float y, float cw, float h, float w, float barH, float R, float r, bool barBottom, Color c);
Rectangle ui_inset(Rectangle r, float dx, float dy);

/* ---------------------------------------------------------------- widgets */
bool  ui_button(UiId id, Rectangle r, const char *icon, const char *label, ButtonStyle st);
float ui_button_width(const char *icon, const char *label);
bool  ui_checkbox(UiId id, Rectangle r, bool *v, const char *label);
bool  ui_switch(UiId id, float x, float y, bool *v);
bool  ui_segmented(UiId id, Rectangle r, const char **items, int n, int *sel);
bool  ui_tabs(UiId id, float x, float y, const char **items, int n, int *sel, float *outH);
bool  ui_input(UiId id, Rectangle r, char *buf, int cap, const char *placeholder, int flags);
bool  ui_dropdown(UiId id, Rectangle r, const char *current, const char *const *items, int n, int flags,
                  char *out, int cap, const char *placeholder);
bool  ui_select(UiId id, Rectangle r, const char *const *items, int n, int *sel);
float ui_badge(float x, float y, const char *icon, const char *text, Color fg, Color bg);
float ui_badge_width(const char *icon, const char *text);
void  ui_progress(Rectangle r, float frac, Color c);
void  ui_spinner(float cx, float cy, float rad, Color c);
bool  ui_row(UiId id, Rectangle r, bool selected);          /* hoverable list row */
void  ui_tooltip(Rectangle r, const char *text);
void  ui_divider(float x, float y, float w);

/* scroll areas */
float ui_scroll_begin(UiScroll *s, Rectangle view);         /* returns content top y */
void  ui_scroll_end(UiScroll *s, float contentH);
void  ui_scroll_to(UiScroll *s, float y);

/* modal: returns body rect; *close set when the user dismisses (Esc / outside / x) */
void ui_modal_layer(void);              /* a full-screen moment of the caller's own: the input layer of a modal */
Rectangle ui_modal_begin(const char *title, const char *subtitle, float w, float h, bool *close);
void      ui_modal_end(void);
bool      ui_modal_active(void);

/* toasts */
void ui_toast(int kind, const char *fmt, ...);

extern bool g_ui_no_input;
int  ui_next_char(void);                     /* typed character this frame (0 = none) */
bool ui_mouse_down(void);
bool ui_double_clicked(void);
float ui_take_wheel(void);                   /* this frame's wheel, which then no longer scrolls the page */
bool ui_key(int key);                        /* pressed or auto-repeated this frame */
bool ui_key_down(int key);
void ui_inject_click(float px, float py);    /* tests: screen pixels */
void ui_inject_drag(float x0, float y0, float x1, float y1);   /* tests: screen pixels */
void ui_inject_text(const char *t);         /* tests: typed into the focused input, then Enter */

/* ---------------------------------------------------------------- widget registry (tests) */
typedef struct {
    UiId id;
    Rectangle r;                 /* screen pixels, the whole widget                         */
    Rectangle vis;               /* the part on screen (inside its clip and the window)    */
    Rectangle view;              /* the clip area it was drawn in (a scroll view, a modal) */
    bool disabled;
    int  layer;                  /* 0 page, 1 modal, 2 popup                                */
    char label[64];
} UiWidget;
void ui_register(UiId id, Rectangle r, const char *label);   /* widgets call it; custom clickable areas may too */
const UiWidget *ui_widget_find(UiId id);                     /* drawn last frame, or NULL */
const UiWidget *ui_widget_by_label(const char *label);
bool ui_text_find(const char *s, Rectangle *vis, bool exact); /* text drawn last frame (test mode only) */
int  ui_text_dump(char *out, int cap);
int  ui_toast_count(void);
const char *ui_toast_text(int i);

#endif
