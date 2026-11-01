/* orbit.c -- the 3-D views' camera: free turning keeps the up axis upright (yaw about it, pitch over it, as before), turning
 * about a chosen axis spins the part about that world axis only, whichever way the mouse goes. See orbit.h. */
#include <math.h>
#include <stdio.h>
#include "orbit.h"
#include "ui.h"

#define TURN 0.008f                       /* radians a pixel */
#define PITCH_MAX 1.5607f                 /* just short of straight down: the turntable's up stays defined */

static Vector3 v3(float x, float y, float z) { return (Vector3){ x, y, z }; }
static float   dot(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vector3 cross(Vector3 a, Vector3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vector3 scale(Vector3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static Vector3 add(Vector3 a, Vector3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vector3 unit(Vector3 a) { float l = sqrtf(dot(a, a)); return l > 1e-12f ? scale(a, 1 / l) : a; }
static Vector3 axis(int k) { return v3(k == 0, k == 1, k == 2); }

/* v turned by ang about the unit axis a (right hand) */
static Vector3 rotate(Vector3 v, Vector3 a, float ang)
{
    float c = cosf(ang), s = sinf(ang);
    return add(add(scale(v, c), scale(cross(a, v), s)), scale(a, dot(a, v) * (1 - c)));
}

/* frame (x, y, z) to world: y is the up axis, x and z the world's (up+2)%3 and (up+1)%3 */
static Vector3 frame_w(const Orbit *o, float fx, float fy, float fz)
{
    float w[3]; int u = o->upAxis;
    w[(u + 2) % 3] = fx; w[u] = fy; w[(u + 1) % 3] = fz;
    return v3(w[0], w[1], w[2]);
}

static void frame_of(const Orbit *o, Vector3 v, float f[3])
{
    float w[3] = { v.x, v.y, v.z }; int u = o->upAxis;
    f[0] = w[(u + 2) % 3]; f[1] = w[u]; f[2] = w[(u + 1) % 3];
}

void orbit_set(Orbit *o, float yaw, float pitch)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    o->back = frame_w(o, cp * sy, sp, cp * cy);
    o->up = frame_w(o, -sp * sy, cp, -sp * cy);
}

/* the yaw and pitch the camera is at (looking straight along the up axis: the yaw its up vector says) */
static void yaw_pitch(const Orbit *o, float *yaw, float *pitch)
{
    float b[3], u[3]; frame_of(o, o->back, b); frame_of(o, o->up, u);
    *pitch = asinf(fmaxf(-1, fminf(1, b[1])));
    if (sqrtf(b[0] * b[0] + b[2] * b[2]) > 1e-3f) *yaw = atan2f(b[0], b[2]);
    else { float sg = b[1] > 0 ? 1.f : -1.f; *yaw = atan2f(-u[0] * sg, -u[2] * sg); }
}

void orbit_level(Orbit *o)
{
    float yaw, pitch; yaw_pitch(o, &yaw, &pitch);
    orbit_set(o, yaw, fmaxf(-PITCH_MAX, fminf(PITCH_MAX, pitch)));
}

Vector3 orbit_right(const Orbit *o) { return cross(o->up, o->back); }

/* the camera turned by ang about the unit axis a, kept orthonormal */
static void turn_camera(Orbit *o, Vector3 a, float ang)
{
    o->back = unit(rotate(o->back, a, ang));
    o->up = unit(rotate(o->up, a, ang));
    o->up = unit(add(o->up, scale(o->back, -dot(o->up, o->back))));
}

void orbit_spin(Orbit *o, float ang)
{
    turn_camera(o, axis(o->spin >= 0 ? o->spin : o->upAxis), -ang);   /* the part one way is the camera the other */
}

void orbit_drag(Orbit *o, Vector2 a, Vector2 b, Vector2 c)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    if (o->spin < 0) {
        float yaw, pitch; yaw_pitch(o, &yaw, &pitch);
        yaw -= dx * TURN; pitch += dy * TURN;
        orbit_set(o, yaw, fmaxf(-PITCH_MAX, fminf(PITCH_MAX, pitch)));
        return;
    }
    Vector3 A = axis(o->spin), right = orbit_right(o);
    /* where the near side of the part goes on screen as it turns about A: dragging that way turns it, as if grabbed */
    Vector3 v = cross(A, o->back);
    float sx = dot(v, right), sy = dot(v, o->up), sl = sqrtf(sx * sx + sy * sy), th;
    if (sl > 0.35f) th = TURN * (dx * sx - dy * sy) / sl;
    else {                                /* the axis points at the viewer: turn it like a dial about the view's centre */
        float p0 = atan2f(-(a.y - c.y), a.x - c.x), p1 = atan2f(-(b.y - c.y), b.x - c.x), dp = p1 - p0;
        if (dp > PI) dp -= 2 * PI;
        if (dp < -PI) dp += 2 * PI;
        th = dot(A, o->back) >= 0 ? dp : -dp;
    }
    turn_camera(o, A, -th);
}

void orbit_pan(Orbit *o, float dx, float dy)
{
    float k = o->dist * 0.0016f;
    Vector3 r = orbit_right(o);
    o->target = add(o->target, add(scale(r, -dx * k), scale(o->up, dy * k)));
}

Camera3D orbit_camera(const Orbit *o)
{
    Camera3D c = { 0 };
    c.position = add(o->target, scale(o->back, o->dist));
    c.target = o->target; c.up = o->up; c.fovy = 35; c.projection = CAMERA_PERSPECTIVE;
    return c;
}

void orbit_draw_axes(const Orbit *o, float x, float y, float l, float s)
{
    static const Color col[3] = { { 230, 80, 70, 255 }, { 90, 190, 90, 255 }, { 80, 140, 240, 255 } };
    static const char *nm[3] = { "x", "y", "z" };
    Vector3 r = orbit_right(o);
    for (int i = 0; i < 3; i++) {                /* the turn axis last, on top */
        int k = o->spin >= 0 ? (o->spin + 1 + i) % 3 : i;
        bool hot = k == o->spin;
        Vector3 A = axis(k);
        float sx = dot(A, r), sy = dot(A, o->up);
        Color c = o->spin >= 0 && !hot ? ui_alpha(col[k], 0.5f) : col[k];
        if (hot) {                              /* a ring about the turn axis' tip: what the part turns about */
            DrawRing((Vector2){ x + sx * l, y - sy * l }, 5 * s, 7 * s, 0, 360, 24, c);
        }
        DrawLineEx((Vector2){ x, y }, (Vector2){ x + sx * l, y - sy * l }, (hot ? 4.f : 2.5f) * s, c);
        DrawTextEx(GetFontDefault(), nm[k], (Vector2){ x + sx * (l + 10 * s) - 3 * s, y - sy * (l + 10 * s) - 6 * s }, (hot ? 14 : 12) * s, 1, c);
    }
}

bool orbit_keys(Orbit *o)
{
    if (ui_any_input_focused()) return false;
    const float step = 15 * DEG2RAD;
    int h = ui_key(KEY_RIGHT) - ui_key(KEY_LEFT), v = ui_key(KEY_UP) - ui_key(KEY_DOWN);
    if (!h && !v) return false;
    if (o->spin < 0) {
        float yaw, pitch; yaw_pitch(o, &yaw, &pitch);
        orbit_set(o, yaw - h * step, fmaxf(-PITCH_MAX, fminf(PITCH_MAX, pitch - v * step)));
    } else orbit_spin(o, (h + v) * step);
    return true;
}

const char *orbit_hint(const Orbit *o)
{
    static const char *h[4] = { "drag to turn", "drag across the x axis to spin it", "drag across the y axis to spin it", "drag across the z axis to spin it" };
    return h[o->spin + 1];
}

#define SEG_W 168
#define BTN_W 32
static const char *TURN_LABEL = "Turn about";

#define FIT_W 54

float orbit_tools_w(bool fit)
{
    return ui_text_w(F_MED, 12, ui_cap(TURN_LABEL)) + 8 + SEG_W + 6 + 2 * BTN_W + 4 + (fit ? FIT_W + 4 : 0);
}

int orbit_tools(Orbit *o, const char *id, float x, float y, bool fit)
{
    int changed = 0;
    char nm[64];
    ui_text(F_MED, 12, ui_cap(TURN_LABEL), x, y + 8, TH.text2);
    x += ui_text_w(F_MED, 12, ui_cap(TURN_LABEL)) + 8;
    static const char *items[] = { "Free", "X", "Y", "Z" };
    int sel = o->spin + 1;
    Rectangle seg = { x, y, SEG_W, 30 };
    snprintf(nm, sizeof nm, "%s-turn", id);
    if (ui_segmented(ui_id(nm), seg, items, 4, &sel) && sel != o->spin + 1) {
        o->spin = sel - 1;
        if (o->spin < 0) orbit_level(o);      /* free again: the up axis upright */
        changed = 1;
    }
    ui_tooltip(seg, "Free: drag any way, the up axis stays upright.  X, Y, Z: dragging spins the part about that axis only.  "
                    "The arrow keys turn it 15 degrees while the mouse is over the view.");
    x += SEG_W + 6;
    static const char *tips[2] = { "Turn the part 90 degrees anticlockwise about the turn axis", "Turn the part 90 degrees clockwise about the turn axis" };
    for (int k = 0; k < 2; k++) {
        Rectangle br = { x, y, BTN_W, 30 };
        snprintf(nm, sizeof nm, "%s-rot", id);
        if (ui_button(ui_idn(ui_id(nm), k), br, k == 0 ? ICON_ROTATE_CCW : ICON_REFRESH_CW, NULL, BTN_SECONDARY)) {
            orbit_spin(o, k == 0 ? PI / 2 : -PI / 2);
            changed = 1;
        }
        ui_tooltip(br, tips[k]);
        x += BTN_W + 4;
    }
    if (fit) {
        Rectangle fr = { x, y, FIT_W, 30 };
        snprintf(nm, sizeof nm, "%s-fit", id);
        if (ui_button(ui_id(nm), fr, NULL, "Fit", BTN_SECONDARY)) changed = 2;
        ui_tooltip(fr, "The whole part in view again, from the starting angle (or double-click the view)");
    }
    return changed;
}
