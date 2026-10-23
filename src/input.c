/* input.c -- the one place that reads raylib's mouse and keyboard; scripted events for tests */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "input.h"

typedef enum { EV_MOVE, EV_BUTTON, EV_WHEEL, EV_KEY, EV_HOLD, EV_CHAR } EvKind;
typedef struct { int at; EvKind k; float x, y; int v; } Ev;

#define MAX_EV 4096
#define MAX_FRAME_KEYS 32
#define MAX_FRAME_CHARS 512

static struct {
    bool    scripted;
    Ev      q[MAX_EV];
    int     nq;
    /* this frame, scripted */
    Vector2 mouse, prevMouse;
    bool    down, prevDown;
    float   wheel;
    int     keys[MAX_FRAME_KEYS], nKeys;       /* pressed this frame */
    int     held[MAX_FRAME_KEYS], nHeld;       /* down this frame */
    int     chars[MAX_FRAME_CHARS], nChars, charPos;
    bool    activity;
} I = { .mouse = { -1e4f, -1e4f }, .prevMouse = { -1e4f, -1e4f } };

void inp_set_scripted(bool on) { I.scripted = on; }
bool inp_scripted(void) { return I.scripted; }
bool inp_pending(void) { return I.nq > 0; }

void inp_begin_frame(void)
{
    I.activity = false;
    if (!I.scripted) {
        I.activity = GetMouseDelta().x != 0 || GetMouseDelta().y != 0 || IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
                     IsMouseButtonReleased(MOUSE_BUTTON_LEFT) || GetMouseWheelMove() != 0 || IsWindowResized();
        return;
    }
    I.prevMouse = I.mouse;
    I.prevDown = I.down;
    I.wheel = 0;
    I.nKeys = I.nHeld = I.nChars = I.charPos = 0;
    int keep = 0;
    for (int i = 0; i < I.nq; i++) {
        Ev *e = &I.q[i];
        if (e->at > 0) { e->at--; I.q[keep++] = *e; continue; }
        I.activity = true;
        switch (e->k) {
        case EV_MOVE: I.mouse = (Vector2){ e->x, e->y }; break;
        case EV_BUTTON: I.down = e->v != 0; break;
        case EV_WHEEL: I.wheel += e->y; break;
        case EV_KEY: if (I.nKeys < MAX_FRAME_KEYS) I.keys[I.nKeys++] = e->v; if (I.nHeld < MAX_FRAME_KEYS) I.held[I.nHeld++] = e->v; break;
        case EV_HOLD: if (I.nHeld < MAX_FRAME_KEYS) I.held[I.nHeld++] = e->v; break;
        case EV_CHAR: if (I.nChars < MAX_FRAME_CHARS) I.chars[I.nChars++] = e->v; break;
        }
    }
    I.nq = keep;
}

bool inp_activity(void) { return I.activity; }

Vector2 inp_mouse(void) { return I.scripted ? I.mouse : GetMousePosition(); }
Vector2 inp_mouse_delta(void)
{
    if (!I.scripted) return GetMouseDelta();
    return (Vector2){ I.mouse.x - I.prevMouse.x, I.mouse.y - I.prevMouse.y };
}
bool inp_button_pressed(void) { return I.scripted ? I.down && !I.prevDown : IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }
bool inp_button_released(void) { return I.scripted ? !I.down && I.prevDown : IsMouseButtonReleased(MOUSE_BUTTON_LEFT); }
bool inp_button_down(void) { return I.scripted ? I.down : IsMouseButtonDown(MOUSE_BUTTON_LEFT); }
float inp_wheel(void) { return I.scripted ? I.wheel : GetMouseWheelMove(); }

static bool in_list(const int *v, int n, int key) { for (int i = 0; i < n; i++) if (v[i] == key) return true; return false; }
bool inp_key_pressed(int key) { return I.scripted ? in_list(I.keys, I.nKeys, key) : IsKeyPressed(key); }
bool inp_key_repeat(int key) { return I.scripted ? in_list(I.keys, I.nKeys, key) : IsKeyPressed(key) || IsKeyPressedRepeat(key); }
bool inp_key_down(int key) { return I.scripted ? in_list(I.held, I.nHeld, key) : IsKeyDown(key); }
int inp_char(void)
{
    if (!I.scripted) return GetCharPressed();
    return I.charPos < I.nChars ? I.chars[I.charPos++] : 0;
}
int inp_any_key(void)
{
    if (!I.scripted) return GetKeyPressed();
    return I.nKeys ? I.keys[0] : 0;
}

/* ---------------------------------------------------------------- queue */
static void push(int at, EvKind k, float x, float y, int v)
{
    if (I.nq >= MAX_EV) return;
    I.q[I.nq++] = (Ev){ at < 0 ? 0 : at, k, x, y, v };
}
void inp_queue_move(int at, float x, float y) { push(at, EV_MOVE, x, y, 0); }
void inp_queue_button(int at, bool down) { push(at, EV_BUTTON, 0, 0, down); }
void inp_queue_wheel(int at, float dy) { push(at, EV_WHEEL, 0, dy, 0); }
void inp_queue_key(int at, int key) { push(at, EV_KEY, 0, 0, key); }
void inp_queue_hold(int at, int key) { push(at, EV_HOLD, 0, 0, key); }
void inp_queue_char(int at, int c) { push(at, EV_CHAR, 0, 0, c); }

void inp_queue_text(int at, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {                                          /* UTF-8 to code points, as GetCharPressed gives them */
        int c = *p++, extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= 0x3F >> extra;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++) c = (c << 6) | (*p++ & 0x3F);
        inp_queue_char(at, c);
    }
}

void inp_click(float x, float y)
{
    inp_queue_move(0, x, y);
    inp_queue_button(0, true);
    inp_queue_button(1, false);
}

void inp_drag(float x0, float y0, float x1, float y1)
{
    inp_queue_move(0, x0, y0);
    inp_queue_button(0, true);
    for (int i = 1; i <= 8; i++) inp_queue_move(i, x0 + (x1 - x0) * i / 8.0f, y0 + (y1 - y0) * i / 8.0f);
    inp_queue_button(9, false);
}

void inp_type_enter(const char *text)
{
    inp_queue_text(0, text);
    inp_queue_key(1, KEY_ENTER);
}

int inp_key_code(const char *n)
{
    static const struct { const char *n; int k; } K[] = {
        { "ENTER", KEY_ENTER }, { "RETURN", KEY_ENTER }, { "ESCAPE", KEY_ESCAPE }, { "ESC", KEY_ESCAPE }, { "TAB", KEY_TAB },
        { "BACKSPACE", KEY_BACKSPACE }, { "DELETE", KEY_DELETE }, { "SPACE", KEY_SPACE }, { "UP", KEY_UP }, { "DOWN", KEY_DOWN },
        { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT }, { "HOME", KEY_HOME }, { "END", KEY_END }, { "PAGEUP", KEY_PAGE_UP },
        { "PAGEDOWN", KEY_PAGE_DOWN }, { "CTRL", KEY_LEFT_CONTROL }, { "SHIFT", KEY_LEFT_SHIFT }, { "ALT", KEY_LEFT_ALT },
        { "SLASH", KEY_SLASH }, { "QUESTION", KEY_SLASH }, { "COMMA", KEY_COMMA }, { "PERIOD", KEY_PERIOD } };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (!strcasecmp(n, K[i].n)) return K[i].k;
    if (n[0] && !n[1] && isalpha((unsigned char)n[0])) return KEY_A + (toupper((unsigned char)n[0]) - 'A');
    if (n[0] && !n[1] && isdigit((unsigned char)n[0])) return KEY_ZERO + (n[0] - '0');
    if ((n[0] == 'F' || n[0] == 'f') && isdigit((unsigned char)n[1])) { int f = 0; sscanf(n + 1, "%d", &f); if (f >= 1 && f <= 12) return KEY_F1 + f - 1; }
    return 0;
}
