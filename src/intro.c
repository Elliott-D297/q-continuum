/* intro.c -- the start-up animation, one for each theme (user request 2026-11-14: "a cool start up animation ... based on
 * what theme you choose. Real corporate for the default themes, and then very, very creative and in theme for the various
 * themes").
 *
 * The classic Dark and Light themes get a product-launch reveal: Dark draws streamlines past a hidden cylinder that becomes the
 * Q, Light castellates a mesh round the Q and snaps it. Every other theme gets a scene of its own (see the sc_* functions).
 *
 * Each scene is a pure function of the time t: no state, hashed randomness, so a screenshot at a given t (--intro t) shows
 * exactly what plays. The scene covers the window until intro_length(); its last 0.35 s fade to the theme's background, then
 * the app is drawn under that colour as it fades away (FADE). A key or a click skips straight to the fade. Settings >
 * Appearance > Start-up animation turns it off; scripted and screenshot runs never play it unless asked.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "intro.h"
#include "rlgl.h"

#define FADE 0.5

static struct { bool active, cued; double t0, off, t, frozen, acc, last; int frames, theme; } I = { .frozen = -1 };
static struct { float W, H, s; } g;

/* ------------------------------------------------------------------ helpers */
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float seg(float t, float a, float b) { return clamp01((t - a) / (b - a)); }
static float ease(float x) { x = clamp01(x); return x * x * (3 - 2 * x); }
static float eout(float x) { x = clamp01(x); return 1 - (1 - x) * (1 - x) * (1 - x); }
static float eback(float x) { x = clamp01(x); const float c1 = 1.70158f, c3 = c1 + 1; return 1 + c3 * powf(x - 1, 3) + c1 * powf(x - 1, 2); }
static float ebounce(float x)
{
    x = clamp01(x);
    const float n = 7.5625f, d = 2.75f;
    if (x < 1 / d) return n * x * x;
    if (x < 2 / d) { x -= 1.5f / d; return n * x * x + 0.75f; }
    if (x < 2.5f / d) { x -= 2.25f / d; return n * x * x + 0.9375f; }
    x -= 2.625f / d; return n * x * x + 0.984375f;
}
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float rnd(unsigned i)
{
    i ^= i >> 16; i *= 0x7feb352dU; i ^= i >> 15; i *= 0x846ca68bU; i ^= i >> 16;
    return (float)(i & 0xFFFFFF) / 16777216.0f;
}
static Color hx(unsigned v) { return (Color){ (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, 255 }; }
static Color al(Color c, float a) { c.a = (unsigned char)(c.a * clamp01(a)); return c; }
static Color mix(Color a, Color b, float t) { return ui_mix(a, b, clamp01(t)); }
static void fill(Color c) { DrawRectangleRec((Rectangle){ -2, -2, g.W + 4, g.H + 4 }, c); }
static void vgrad(float x, float y, float w, float h, Color top, Color bot)
{
    /* DrawRectangleGradientV takes ints: draw it as a quad so fractional logical sizes stay exact */
    rlBegin(RL_QUADS);
    rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(x, y);
    rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(x, y + h);
    rlColor4ub(bot.r, bot.g, bot.b, bot.a); rlVertex2f(x + w, y + h);
    rlColor4ub(top.r, top.g, top.b, top.a); rlVertex2f(x + w, y);
    rlEnd();
}
static void hgrad(float x, float y, float w, float h, Color l, Color r)
{
    rlBegin(RL_QUADS);
    rlColor4ub(l.r, l.g, l.b, l.a); rlVertex2f(x, y);
    rlColor4ub(l.r, l.g, l.b, l.a); rlVertex2f(x, y + h);
    rlColor4ub(r.r, r.g, r.b, r.a); rlVertex2f(x + w, y + h);
    rlColor4ub(r.r, r.g, r.b, r.a); rlVertex2f(x + w, y);
    rlEnd();
}
static void glow(float x, float y, float r, Color c, float a) { DrawCircleGradient((int)x, (int)y, r, al(c, a), al(c, 0)); }
static void tri(Vector2 a, Vector2 b, Vector2 c, Color col)
{
    float cr = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cr > 0) { Vector2 t = b; b = c; c = t; }        /* raylib wants them counter-clockwise on screen */
    DrawTriangle(a, b, c, col);
}
static float tw(UiFamily f, float size, const char *s) { return ui_text_w(f, size, s); }
static void ctext(UiFamily f, float size, const char *s, float cx, float y, Color c) { ui_text(f, size, s, cx - tw(f, size, s) / 2, y, c); }
/* text stretched about its centre (NERV's title cards, the Souls banner) */
static void stext(UiFamily f, float size, const char *s, float cx, float cy, float sx, float sy, Color c)
{
    rlDrawRenderBatchActive();
    rlPushMatrix();
    rlTranslatef(cx, cy, 0);
    rlScalef(sx, sy, 1);
    ui_text(f, size, s, -tw(f, size, s) / 2, -size * 0.62f, c);
    rlDrawRenderBatchActive();
    rlPopMatrix();
}
/* the first n characters of s, each letter at the place it has in the whole word; fl(i) its alpha */
static void letters(UiFamily f, float size, const char *s, float x, float y, Color c, float (*fl)(int, float), float t)
{
    char pre[128], one[8];
    int n = (int)strlen(s);
    for (int i = 0; i < n && i < 120; i++) {
        float a = fl(i, t);
        if (a <= 0) continue;
        memcpy(pre, s, (size_t)i); pre[i] = 0;
        one[0] = s[i]; one[1] = 0;
        ui_text(f, size, one, x + tw(f, size, pre), y, al(c, a));
    }
}
static const char *spaced(const char *s)              /* "Q-CONTINUUM" -> "Q - C O N T I N U U M" */
{
    static char b[2][160]; static int k;
    char *o = b[k ^= 1]; int n = 0;
    for (const char *p = s; *p && n < 150; p++) { if (p != s) o[n++] = ' '; o[n++] = *p; }
    o[n] = 0;
    return o;
}
static struct tm now_tm(void) { time_t tt = time(NULL); struct tm m; localtime_r(&tt, &m); return m; }

/* the Q: the ring from startDeg round (ringP of it), then the tail (tailP of it); the proportions of the logo (logo.c) */
static Vector2 tail_pt(Vector2 c, float ro, float u)
{
    Vector2 p0 = { c.x + ro * 0.30f, c.y + ro * 0.38f }, c1 = { c.x + ro * 0.75f, c.y + ro * 0.70f };
    Vector2 c2 = { c.x + ro * 0.85f, c.y + ro * 1.70f }, p3 = { c.x + ro * 1.59f, c.y + ro * 1.48f };
    return GetSplinePointBezierCubic(p0, c1, c2, p3, u);
}
static void qmark(Vector2 c, float ro, float th, float ringP, float tailP, Color col, float startDeg)
{
    if (ringP > 0) DrawRing(c, ro - th, ro, startDeg, startDeg + 360 * clamp01(ringP), 72, col);
    if (tailP <= 0) return;
    const int N = 32;
    Vector2 prev = tail_pt(c, ro, 0);
    DrawCircleV(prev, th * 0.5f, col);
    for (int i = 1; i <= N; i++) {
        float u = (float)i / N;
        if (u > tailP) u = tailP;
        Vector2 p = tail_pt(c, ro, u);
        DrawLineEx(prev, p, th, col);
        DrawCircleV(p, th * 0.5f, col);
        prev = p;
        if (u >= tailP) break;
    }
}
/* the mark beside a word, centred as a group: where each goes */
static void group(float ro, float textW, float gap, float *markX, float *textX)
{
    float total = ro + ro * 1.62f + gap + textW, left = g.W / 2 - total / 2;
    *markX = left + ro;
    *textX = left + ro * 2.62f + gap;
}
static void cog(Vector2 c, float ro, float ri, int teeth, float tooth, float rot, Color col)
{
    DrawRing(c, ri, ro, 0, 360, 48, col);
    for (int k = 0; k < teeth; k++) {
        float a = rot + k * 360.0f / teeth;
        DrawRing(c, ro - 1, ro + tooth, a - 90.0f / teeth, a + 90.0f / teeth, 4, col);
    }
}
static void corners(Rectangle r, float len, float th, Color c)   /* HUD brackets */
{
    float x0 = r.x, y0 = r.y, x1 = r.x + r.width, y1 = r.y + r.height;
    DrawRectangleRec((Rectangle){ x0, y0, len, th }, c); DrawRectangleRec((Rectangle){ x0, y0, th, len }, c);
    DrawRectangleRec((Rectangle){ x1 - len, y0, len, th }, c); DrawRectangleRec((Rectangle){ x1 - th, y0, th, len }, c);
    DrawRectangleRec((Rectangle){ x0, y1 - th, len, th }, c); DrawRectangleRec((Rectangle){ x0, y1 - len, th, len }, c);
    DrawRectangleRec((Rectangle){ x1 - len, y1 - th, len, th }, c); DrawRectangleRec((Rectangle){ x1 - th, y1 - len, th, len }, c);
}
static float typed_n(const char *s, float t, float t0, float cps) { float n = (t - t0) * cps; return n < 0 ? 0 : n > (float)strlen(s) ? (float)strlen(s) : n; }
static void typed(UiFamily f, float size, const char *s, float x, float y, Color c, float t, float t0, float cps, bool cursor)
{
    char b[256]; int n = (int)typed_n(s, t, t0, cps);
    if (t < t0) return;
    snprintf(b, sizeof b, "%.*s", n, s);
    ui_text(f, size, b, x, y, c);
    if (cursor && (n < (int)strlen(s) || fmodf(t * 2.5f, 1) < 0.5f))
        DrawRectangleRec((Rectangle){ x + tw(f, size, b) + size * 0.1f, y + size * 0.1f, size * 0.55f, size * 1.0f }, c);
}

/* ================================================================== Dark: streamlines past a hidden cylinder that becomes the Q */
static void streamlines(Vector2 c, float R, float t, float alpha)
{
    Color c0 = hx(0x0A84FF), c1 = hx(0x5E5CE6);
    float s = g.s, sp = 19 * s, step = 6 * s;
    for (int k = 1; k <= 13; k++) {
        float psi = k * sp - sp * 0.4f;
        float head = -c.x - 40 + (g.W + 80) * eout(seg(t, 0.05f + k * 0.035f, 1.45f + k * 0.035f));
        float done = seg(t, 1.45f + k * 0.035f, 1.6f + k * 0.035f);
        float base = 0.62f * (1 - k / 15.0f) + 0.08f;
        Color col = mix(c0, c1, k / 13.0f);
        for (int side = -1; side <= 1; side += 2) {
            Vector2 prev = { 0 }; bool have = false;
            for (float x = -c.x - 20; x < g.W - c.x + 20; x += step) {
                float xx = x > head ? head : x;
                float lo = fabsf(xx) < R ? sqrtf(R * R - xx * xx) : 0, hi = psi + R + 4;
                for (int it = 0; it < 22; it++) {             /* psi = y (1 - R^2 / r^2): flow past a cylinder */
                    float m = 0.5f * (lo + hi), r2 = xx * xx + m * m;
                    if (m * (1 - R * R / r2) < psi) lo = m; else hi = m;
                }
                Vector2 p = { c.x + xx, c.y + side * 0.5f * (lo + hi) };
                if (have) {
                    float sh = 0.55f + 0.45f * sinf(xx * 0.03f / s - t * 7 + k * 0.7f);
                    DrawLineEx(prev, p, 1.6f * s, al(col, alpha * base * sh));
                }
                prev = p; have = true;
                if (x > head) break;
            }
            if (done < 1) DrawCircleV(prev, 2.4f * s, al(WHITE, alpha * (1 - done) * 0.9f));
        }
    }
}
static void footer(float t, Color text, Color track, bool light)
{
    float s = g.s, w = 180 * s, y = g.H - 46 * s;
    float a = ease(seg(t, 0.3f, 1.0f));
    ctext(F_MED, 11 * s, "BUILT ON OPENFOAM", g.W / 2, y - 22 * s, al(text, a));
    DrawRectangleRec((Rectangle){ g.W / 2 - w / 2, y, w, 2 * s }, al(track, a));
    hgrad(g.W / 2 - w / 2, y, w * ease(seg(t, 0.2f, 3.8f)), 2 * s, al(hx(light ? 0x2864F5 : 0x0A84FF), a), al(hx(0x5E5CE6), a));
}
static void sc_dark(float t)
{
    float s = g.s;
    fill(hx(0x07090D));
    vgrad(0, 0, g.W, g.H * 0.5f, al(hx(0x10141D), 1), al(hx(0x07090D), 1));
    const char *word = "Q-Continuum";
    float ro = 62 * s, th = ro * 0.30f, fs = 60 * s, wtx = tw(F_SEMI, fs, word), mx, tx;
    group(ro, wtx, 30 * s, &mx, &tx);
    float mv = ease(seg(t, 2.05f, 2.75f));
    Vector2 c = { lerp(g.W / 2 - ro * 0.3f, mx, mv), g.H * 0.46f };
    glow(c.x, c.y, ro * 5, hx(0x0A84FF), 0.16f * ease(seg(t, 0.6f, 1.8f)));
    float la = 1 - ease(seg(t, 1.9f, 2.6f));
    if (la > 0) streamlines(c, ro, t, la);
    glow(c.x, c.y, ro * 1.6f, hx(0x5E5CE6), 0.35f * ease(seg(t, 1.0f, 1.8f)));
    qmark(c, ro, th, eout(seg(t, 0.85f, 1.65f)), eout(seg(t, 1.55f, 1.95f)), WHITE, 52);
    float e = eout(seg(t, 2.35f, 3.05f)), ty = c.y - fs * 0.48f;
    if (e > 0) {
        ui_clip_push((Rectangle){ tx - 4, ty - fs * 0.3f, (wtx + 30 * s) * e, fs * 1.8f });
        ui_text(F_SEMI, fs, word, tx + 18 * s * (1 - e), ty, WHITE);
        ui_clip_pop();
    }
    float e2 = ease(seg(t, 2.85f, 3.35f));
    ui_text(F_REG, 18 * s, "Computational fluid dynamics, made clear.", tx, ty + fs * 1.15f + 6 * s * (1 - e2), al(hx(0x9AA3B2), e2));
    footer(t, hx(0x5B6475), hx(0x1C2230), false);
}

/* ================================================================== Light: a mesh castellates round the Q, then snaps to it */
static Vector2 L_c; static float L_ro, L_th; static Vector2 L_tail[17];
static float shape_dist(float x, float y)
{
    float d = fabsf(hypotf(x - L_c.x, y - L_c.y) - (L_ro - L_th / 2));
    for (int i = 0; i < 16; i++) {
        Vector2 a = L_tail[i], b = L_tail[i + 1];
        float vx = b.x - a.x, vy = b.y - a.y, u = ((x - a.x) * vx + (y - a.y) * vy) / (vx * vx + vy * vy + 1e-6f);
        u = clamp01(u);
        float dd = hypotf(x - a.x - u * vx, y - a.y - u * vy);
        if (dd < d) d = dd;
    }
    return d;
}
static void cell(float x, float y, float sz, int lvl, float a, float t)
{
    float cx = x + sz / 2, cy = y + sz / 2, d = shape_dist(cx, cy);
    float splitAt = 0.65f + lvl * 0.36f;
    if (lvl < 3 && d < sz * 0.75f + L_th * 0.5f && t > splitAt) {
        float h = sz / 2, f = ease(seg(t, splitAt, splitAt + 0.25f));
        cell(x, y, h, lvl + 1, a * f, t); cell(x + h, y, h, lvl + 1, a * f, t);
        cell(x, y + h, h, lvl + 1, a * f, t); cell(x + h, y + h, h, lvl + 1, a * f, t);
        DrawRectangleLinesEx((Rectangle){ x, y, sz, sz }, 1.0f / ui_scale(), al(mix(hx(0xC5CFE0), hx(0x2864F5), lvl / 4.0f), a / (a > 0 ? a : 1) * 0.9f));
        return;
    }
    if (d < L_th * 0.5f && t > 1.6f) DrawRectangleRec((Rectangle){ x, y, sz, sz }, al(hx(0x5E5CE6), 0.30f * ease(seg(t, 1.6f, 1.9f))));
    DrawRectangleLinesEx((Rectangle){ x, y, sz, sz }, 1.0f / ui_scale(), al(mix(hx(0xC5CFE0), hx(0x2864F5), lvl / 4.0f), a * (0.9f - lvl * 0.12f)));
}
static void sc_light(float t)
{
    float s = g.s;
    vgrad(-2, -2, g.W + 4, g.H + 4, hx(0xFFFFFF), hx(0xEBEFF7));
    const char *word = "Q-Continuum";
    Color ink = hx(0x2864F5);
    float ro = 64 * s, th = ro * 0.32f, fs = 60 * s, wtx = tw(F_SEMI, fs, word), mx, tx;
    group(ro, wtx, 30 * s, &mx, &tx);
    float mv = ease(seg(t, 2.45f, 3.05f));
    Vector2 c0 = { g.W / 2 - ro * 0.3f, g.H * 0.46f };
    Vector2 c = { lerp(c0.x, mx, mv), c0.y };
    L_c = c0; L_ro = ro; L_th = th;
    for (int i = 0; i <= 16; i++) L_tail[i] = tail_pt(c0, ro, i / 16.0f);
    float meshA = 1 - ease(seg(t, 2.1f, 2.6f));
    if (meshA > 0) {
        float cs = 46 * s; int nx = 17, ny = 9;
        float ox = c0.x + ro * 0.3f - nx * cs / 2, oy = c0.y + ro * 0.25f - ny * cs / 2;
        for (int j = 0; j < ny; j++)
            for (int i = 0; i < nx; i++) {
                float a0 = 0.05f + (i + j) * 0.022f, ca = ease(seg(t, a0, a0 + 0.3f));
                /* the edges of the block fade out: a mesh, not a box */
                float ex = fminf(i + 0.5f, nx - i - 0.5f) / 3.0f, ey = fminf(j + 0.5f, ny - j - 0.5f) / 2.5f;
                if (ca > 0) cell(ox + i * cs, oy + j * cs, cs, 0, ca * meshA * clamp01(fminf(ex, ey)), t);
            }
        float sa = meshA * ease(seg(t, 0.25f, 0.6f)) * (1 - ease(seg(t, 1.6f, 2.0f)));   /* the STL: the surface the mesh aims at */
        DrawRing(c0, ro - 1.2f * s, ro, 0, 360, 72, al(hx(0x5E5CE6), sa));
        DrawRing(c0, ro - th, ro - th + 1.2f * s, 0, 360, 72, al(hx(0x5E5CE6), sa));
        const char *cap = t < 0.65f ? "blockMesh" : t < 1.65f ? "castellating" : "snapping";
        ctext(F_MONO, 12 * s, cap, c0.x + ro * 0.3f, oy + ny * cs + 14 * s, al(hx(0x8A94A6), meshA * ease(seg(t, 0.2f, 0.5f))));
    }
    qmark(c, ro, th, 1, 1, al(ink, ease(seg(t, 1.95f, 2.35f))), 52);
    float e = eout(seg(t, 2.75f, 3.35f)), ty = c.y - fs * 0.48f;
    if (e > 0) {
        ui_clip_push((Rectangle){ tx - 4, ty - fs * 0.3f, (wtx + 30 * s) * e, fs * 1.8f });
        ui_text(F_SEMI, fs, word, tx + 18 * s * (1 - e), ty, hx(0x0F172A));
        ui_clip_pop();
    }
    float e2 = ease(seg(t, 3.1f, 3.55f));
    ui_text(F_REG, 18 * s, "Meshing, solving and results, in one place.", tx, ty + fs * 1.15f + 6 * s * (1 - e2), al(hx(0x5B6475), e2));
    footer(t, hx(0x8A94A6), hx(0xDCE2EC), true);
}

/* ================================================================== LCARS: the console powers up */
static float lc_letter(int i, float t) { return t > 1.2f + i * 0.05f ? 1 : 0; }
static void sc_lcars(float t)
{
    float s = g.s, W = g.W, H = g.H, M = 14 * s;
    Color O = hx(0xFF9900), L = hx(0x9999FF), P = hx(0xCC99CC), PE = hx(0xFFCC99), B = hx(0x9999CC), R = hx(0xCC6666), GO = hx(0xFFCC66);
    Color cyc[6] = { P, PE, B, R, GO, L };
    fill(BLACK);
    float cw = 130 * s, h = 96 * s, barH = 24 * s;
    float wTop = cw + 20 * s + (W - 2 * M - cw - 20 * s) * eout(seg(t, 0.05f, 0.55f));
    ui_elbow(M, M, cw, h, wTop, barH, 46 * s, 20 * s, false, O);
    float x = M + cw + 120 * s;                               /* black cuts into the top bar: its blocks */
    for (int i = 0; i < 5 && x < M + wTop - 40 * s; i++) {
        DrawRectangleRec((Rectangle){ x, M, 6 * s, barH }, BLACK);
        DrawRectangleRec((Rectangle){ x + 6 * s, M, fminf(150 * s, M + wTop - x - 6 * s), barH }, cyc[i % 6]);
        x += (90 + 50 * rnd(i + 3)) * s;
    }
    if (wTop >= W - 2 * M - 1) {
        DrawRectangleRec((Rectangle){ W - M - 200 * s, M, 12 * s, barH }, BLACK);
        char hd[64]; snprintf(hd, sizeof hd, "Q-CONTINUUM %s", QC_VERSION);
        float hw = tw(F_SEMI, 22 * s, hd);
        DrawRectangleRec((Rectangle){ W - M - 200 * s - hw - 24 * s, M, hw + 24 * s, barH }, BLACK);
        ui_text(F_SEMI, 22 * s, hd, W - M - 200 * s - hw - 12 * s, M - 1 * s, O);
    }
    float h2 = 110 * s;
    float wBot = cw + 20 * s + (W - 2 * M - cw - 20 * s) * eout(seg(t, 0.15f, 0.7f));
    ui_elbow(M, H - M - h2, cw, h2, wBot, barH * 0.7f, 46 * s, 20 * s, true, L);
    float y = M + h + 6 * s, yEnd = H - M - h2 - 6 * s;      /* the column's blocks drop in */
    for (int i = 0; y < yEnd - 10 * s; i++) {
        float bh = fminf((44 + 50 * rnd(i * 7 + 1)) * s, yEnd - y);
        float a = ease(seg(t, 0.3f + i * 0.08f, 0.45f + i * 0.08f));
        if (a > 0) {
            DrawRectangleRec((Rectangle){ M, y, cw * a, bh }, cyc[(i + 2) % 6]);
            char num[16]; snprintf(num, sizeof num, "%02d-%04d", (int)(rnd(i * 13) * 99), (int)(rnd(i * 17) * 9999));
            if (a >= 1) ui_text(F_MED, 13 * s, num, M + cw - tw(F_MED, 13 * s, num) - 6 * s, y + bh - 17 * s, BLACK);
        }
        y += bh + 6 * s;
    }
    float cx0 = M + cw + 36 * s, cx1 = W - M, ccx = (cx0 + cx1) / 2;
    /* readouts: rows of numbers, some ticking */
    int cols = (int)((cx1 - cx0) / (70 * s));
    for (int r = 0; r < 5; r++) {
        float ra = ease(seg(t, 0.5f + r * 0.07f, 0.6f + r * 0.07f)) * (1 - 0.55f * ease(seg(t, 1.3f, 1.8f)));
        if (ra <= 0) continue;
        for (int k = 0; k < cols; k++) {
            unsigned id = (unsigned)(r * 97 + k);
            int tick = rnd(id + 5) < 0.3f ? (int)(t * 12) : 0;
            int dg = 2 + (int)(rnd(id) * 4);
            char v[12]; snprintf(v, sizeof v, "%0*d", dg, (int)(rnd(id * 31 + (unsigned)tick) * powf(10, (float)dg)));
            ui_text(F_MED, 15 * s, v, cx0 + k * 70 * s, M + barH + 22 * s + r * 22 * s, al(cyc[(k + r) % 6], ra));
        }
    }
    const char *word = "Q-CONTINUUM";
    float fs = 92 * s, ww = tw(F_SEMI, fs, word), ty = H * 0.40f;
    letters(F_SEMI, fs, word, ccx - ww / 2, ty, O, lc_letter, t);
    float pw = ww * eout(seg(t, 1.6f, 2.1f));
    if (pw > 0) ui_pill((Rectangle){ ccx - ww / 2, ty + fs * 1.12f, pw, 10 * s }, L);
    ctext(F_MED, 20 * s, "LIBRARY COMPUTER ACCESS AND RETRIEVAL SYSTEM", ccx, ty + fs * 1.12f + 24 * s, al(P, ease(seg(t, 1.9f, 2.3f))));
    struct tm m = now_tm();
    char sd[96]; snprintf(sd, sizeof sd, "COMPUTATIONAL FLUID DYNAMICS DIVISION   STARDATE %02d%03d.%d", (m.tm_year + 1900) % 100,
                          m.tm_yday * 1000 / 366, m.tm_hour * 10 / 24);
    ctext(F_MED, 18 * s, sd, ccx, ty + fs * 1.12f + 52 * s, al(PE, ease(seg(t, 2.1f, 2.5f))));
    float py = ty + fs * 1.12f + 100 * s, pwid = 22 * s, gap = 6 * s, total = 16 * (pwid + gap);
    if (t > 2.2f) {
        const char *st = t < 3.5f ? "INITIALIZING" : "ONLINE";
        ui_text(F_SEMI, 18 * s, st, ccx - total / 2 - tw(F_SEMI, 18 * s, st) - 14 * s, py - 4 * s, t < 3.5f ? GO : O);
        for (int i = 0; i < 16; i++) {
            bool on = t > 2.3f + i * 0.075f;
            ui_pill((Rectangle){ ccx - total / 2 + i * (pwid + gap), py, pwid, 12 * s }, on ? (i % 5 == 4 ? R : O) : hx(0x332211));
        }
    }
}

/* ================================================================== Stardew: dawn on the farm, the sign drops in */
static float px_(void) { return fmaxf(2, roundf(4 * g.s * ui_scale())) / ui_scale(); }
static float sn(float v) { float p = px_(); return floorf(v / p) * p; }
static void pixel_disc(float cx, float cy, float r, Color c)
{
    float p = px_();
    for (float dy = -r; dy <= r; dy += p) {
        float hw = sn(sqrtf(fmaxf(0, r * r - dy * dy)));
        DrawRectangleRec((Rectangle){ sn(cx - hw), sn(cy + dy), 2 * hw, p }, c);
    }
}
static float hill(float x, float base, float amp, float ph) { return base - amp * (0.6f * sinf(x * 0.004f / g.s + ph) + 0.4f * sinf(x * 0.011f / g.s + ph * 2.3f)); }
static void sc_stardew(float t)
{
    float s = g.s, W = g.W, H = g.H, p = px_();
    float d = ease(seg(t, 0.1f, 2.6f));
    Color nightT = hx(0x1B1E4B), nightB = hx(0x4B3A78), dayT = hx(0x5FA8E8), dayB = hx(0xFFD9A0);
    int nb = 10;
    for (int i = 0; i < nb; i++) {                               /* banded sky, a pixel gradient */
        float u = (float)i / (nb - 1);
        Color c = mix(mix(nightT, nightB, u), mix(dayT, dayB, u), d);
        DrawRectangleRec((Rectangle){ -2, sn(H * 0.7f * i / nb) - 2, W + 4, sn(H * 0.7f / nb) + p + 2 }, c);
    }
    DrawRectangleRec((Rectangle){ -2, sn(H * 0.69f), W + 4, H }, mix(nightB, dayB, d));
    for (int i = 0; i < 46; i++) {
        float tw_ = 0.5f + 0.5f * sinf(t * 3 + i * 1.7f);
        DrawRectangleRec((Rectangle){ sn(rnd(i) * W), sn(rnd(i + 99) * H * 0.45f), p, p }, al(WHITE, (1 - d) * tw_));
    }
    float sunY = lerp(H * 0.66f, H * 0.24f, eout(seg(t, 0.3f, 2.6f)));
    pixel_disc(W * 0.70f, sunY, 64 * s, al(hx(0xFFB85A), 0.35f));
    pixel_disc(W * 0.70f, sunY, 44 * s, hx(0xFFE27A));
    for (int i = 0; i < 4; i++) {                                 /* clouds */
        float cx = fmodf(rnd(i * 5) * W + t * (10 + 6 * i) * s, W + 300 * s) - 150 * s, cy = sn(H * (0.10f + 0.08f * i));
        Color cc = al(mix(hx(0x8A7FB8), WHITE, d), 0.9f);
        for (int k = 0; k < 5; k++) {
            float bw = sn((40 + 30 * rnd(i * 9 + k)) * s), bh = sn((14 + 10 * rnd(i * 7 + k)) * s);
            DrawRectangleRec((Rectangle){ sn(cx + (k - 2) * 26 * s), cy - bh, bw, bh + sn(8 * s) }, cc);
        }
    }
    for (float x = 0; x < W + p * 2; x += p * 2) {                  /* the hills, in pixel columns */
        float y1 = sn(hill(x, H * 0.66f, 30 * s, 1.0f)), y2 = sn(hill(x, H * 0.78f, 18 * s, 4.0f));
        DrawRectangleRec((Rectangle){ x, y1, p * 2, H - y1 }, mix(hx(0x1F3D33), hx(0x3E8E5A), d));
        DrawRectangleRec((Rectangle){ x, y2, p * 2, H - y2 }, mix(hx(0x2A4A2A), hx(0x5DBB4A), d));
        DrawRectangleRec((Rectangle){ x, y2, p * 2, p }, mix(hx(0x3A5A3A), hx(0x8AD86A), d));
    }
    Color wood = mix(hx(0x4A2E1A), hx(0x8B5A2B), d);
    for (float x = sn(20 * s); x < W; x += sn(56 * s)) {             /* the fence */
        float gy = sn(hill(x, H * 0.78f, 18 * s, 4.0f));
        DrawRectangleRec((Rectangle){ x, gy - sn(34 * s), sn(8 * s), sn(36 * s) }, wood);
        DrawRectangleRec((Rectangle){ x, gy - sn(28 * s), sn(56 * s), p * 1.5f }, wood);
        DrawRectangleRec((Rectangle){ x, gy - sn(16 * s), sn(56 * s), p * 1.5f }, wood);
    }
    for (int i = 0; i < 11; i++) {                                   /* crops pop up in a row */
        float x = sn(W * 0.08f + i * W * 0.084f), gy = H * 0.92f;
        float k = eback(seg(t, 0.9f + i * 0.09f, 1.25f + i * 0.09f));
        if (k <= 0) continue;
        DrawRectangleRec((Rectangle){ x - sn(10 * s), gy, sn(20 * s), sn(6 * s) }, hx(0x6B4423));
        float hgt = sn(18 * s * k);
        DrawRectangleRec((Rectangle){ x, gy - hgt, p, hgt }, hx(0x3E8E2E));
        DrawRectangleRec((Rectangle){ x - sn(8 * s * k), gy - hgt, sn(8 * s * k), p * 2 }, hx(0x4FA53A));
        DrawRectangleRec((Rectangle){ x + p, gy - hgt - p, sn(8 * s * k), p * 2 }, hx(0x6BC24A));
        if (i % 3 == 1 && t > 2.4f) pixel_disc(x + p / 2, gy - hgt - p * 2, 5 * s, hx(i % 2 ? 0xF4D03F : 0xFF7FA0));
    }
    for (int i = 0; i < 3; i++) {                                    /* birds */
        float bx = fmodf(-80 * s + (t - 0.8f * i) * 140 * s, W + 200 * s), by = H * (0.20f + 0.05f * i) + sinf(t * 2 + i) * 6 * s;
        float f = sinf(t * 10 + i) > 0 ? p : -p;
        DrawRectangleRec((Rectangle){ sn(bx), sn(by), p, p }, hx(0x2A2A3A));
        DrawRectangleRec((Rectangle){ sn(bx - p), sn(by - f), p, p }, hx(0x2A2A3A));
        DrawRectangleRec((Rectangle){ sn(bx + p), sn(by - f), p, p }, hx(0x2A2A3A));
    }
    for (int i = 0; i < 14; i++) {                                   /* leaves on the wind */
        float lt = fmodf(t * 0.35f + rnd(i), 1.0f);
        float lx = sn(rnd(i + 40) * W + lt * 300 * s), ly = sn(-20 * s + lt * H * 1.1f + sinf(t * 3 + i) * 12 * s);
        DrawRectangleRec((Rectangle){ lx, ly, p * 2, p }, i % 3 ? hx(0x7CC24A) : hx(0xE89A3A));
    }
    /* the sign: drops on its ropes and bounces */
    const char *word = "Q-Continuum";
    float fs = 54 * s, ww = tw(F_SEMI, fs, word), bw = sn(ww + 90 * s), bh = sn(fs * 1.9f);
    float fall = ebounce(seg(t, 1.55f, 2.35f));
    float by = sn(lerp(-bh - 60 * s, H * 0.30f, fall)), bx = sn(W / 2 - bw / 2);
    Color dk = hx(0x5B3314), wd = hx(0xC88A4E), pl = hx(0xB0753E), hi = hx(0xE8B070);
    DrawRectangleRec((Rectangle){ bx + sn(30 * s), -2, p, by + 2 }, hx(0x7A5A3A));
    DrawRectangleRec((Rectangle){ bx + bw - sn(30 * s), -2, p, by + 2 }, hx(0x7A5A3A));
    DrawRectangleRec((Rectangle){ bx + p, by + p * 2, bw, bh }, al(BLACK, 0.25f));
    DrawRectangleRec((Rectangle){ bx + p, by, bw - 2 * p, bh }, dk);
    DrawRectangleRec((Rectangle){ bx, by + p, bw, bh - 2 * p }, dk);
    DrawRectangleRec((Rectangle){ bx + 2 * p, by + 2 * p, bw - 4 * p, bh - 4 * p }, wd);
    for (int k = 1; k < 3; k++) DrawRectangleRec((Rectangle){ bx + 2 * p, sn(by + bh * k / 3), bw - 4 * p, p }, pl);
    DrawRectangleRec((Rectangle){ bx + 2 * p, by + 2 * p, bw - 4 * p, p }, hi);
    ui_text(F_SEMI, fs, word, bx + (bw - ww) / 2 + p, by + bh / 2 - fs * 0.62f + p, al(hi, 0.8f));
    ui_text(F_SEMI, fs, word, bx + (bw - ww) / 2, by + bh / 2 - fs * 0.62f, hx(0x4A250C));
    static const char *const SEASON[4] = { "Winter", "Spring", "Summer", "Fall" };
    struct tm m = now_tm();
    char dt[48]; snprintf(dt, sizeof dt, "%s %d, Year %d", SEASON[(m.tm_mon + 1) % 12 / 3], m.tm_mday > 28 ? 28 : m.tm_mday, m.tm_year + 1900 - 2025);
    float da = seg(t, 2.5f, 2.7f);
    if (da > 0) {
        float dw = sn(tw(F_SEMI, 22 * s, dt) + 36 * s), dh = sn(40 * s), dx = sn(W / 2 - dw / 2), dy = sn(by + bh + 18 * s);
        float k = eback(seg(t, 2.5f, 2.8f));
        Rectangle r = { dx + dw / 2 * (1 - k), dy, dw * k, dh };
        DrawRectangleRec(r, dk);
        DrawRectangleRec(ui_inset(r, p, p), hx(0xFFE9B8));
        if (k > 0.95f) ctext(F_SEMI, 22 * s, dt, W / 2, dy + dh / 2 - 22 * s * 0.62f, hx(0x5B3314));
    }
}

/* ================================================================== Pokémon: the battle wipe, a wild Q appears */
static void ball(Vector2 c, float R, float rot, Color top)
{
    Color ink = hx(0x1A1A1A);
    DrawCircleV(c, R + R * 0.12f, ink);
    DrawCircleSector(c, R, 180 + rot, 360 + rot, 36, top);
    DrawCircleSector(c, R, rot, 180 + rot, 36, WHITE);
    float a = rot * DEG2RAD;
    Vector2 d = { cosf(a) * R, sinf(a) * R };
    DrawLineEx((Vector2){ c.x - d.x, c.y - d.y }, (Vector2){ c.x + d.x, c.y + d.y }, R * 0.18f, ink);
    DrawCircleV(c, R * 0.30f, ink);
    DrawCircleV(c, R * 0.18f, WHITE);
}
static void pk_box(Rectangle r)
{
    float s = g.s;
    DrawRectangleRounded(r, 0.18f, 8, hx(0x283850));
    DrawRectangleRounded(ui_inset(r, 4 * s, 4 * s), 0.16f, 8, hx(0xE8E8F8));
    DrawRectangleRounded(ui_inset(r, 7 * s, 7 * s), 0.14f, 8, WHITE);
}
static void sc_pokemon(float t)
{
    float s = g.s, W = g.W, H = g.H;
    if (t < 0.95f) {
        fill(hx(0x78C850));                                          /* the tall grass, then the wipe */
        for (int i = 0; i < 90; i++) {
            float x = rnd(i) * W, y = rnd(i + 500) * H, sway = sinf(t * 6 + i) * 3 * s;
            for (int k = -1; k <= 1; k++)
                tri((Vector2){ x + k * 7 * s, y }, (Vector2){ x + k * 7 * s + 6 * s, y }, (Vector2){ x + k * 9 * s + sway + 3 * s, y - 18 * s }, hx(0x4A9A30));
        }
        int n = 10;
        for (int i = 0; i < n; i++) {
            float pr = eout(seg(t, 0.12f + i * 0.04f, 0.5f + i * 0.04f)), bh = H / n + 1;
            if (i % 2) DrawRectangleRec((Rectangle){ W - W * pr, i * H / n, W * pr + 2, bh }, BLACK);
            else DrawRectangleRec((Rectangle){ -2, i * H / n, W * pr + 2, bh }, BLACK);
        }
        return;
    }
    vgrad(-2, -2, W + 4, H + 4, hx(0xF8F8E8), hx(0xD8E8C0));
    DrawRectangleRec((Rectangle){ -2, H * 0.52f, W + 4, H }, hx(0xE0ECC8));
    float in = eout(seg(t, 1.0f, 1.55f));
    Vector2 ep = { lerp(-W * 0.3f, W * 0.70f, in), H * 0.44f }, pp = { lerp(W * 1.3f, W * 0.27f, in), H * 0.74f };
    DrawEllipse((int)ep.x, (int)ep.y, 150 * s, 34 * s, hx(0xA8C880));
    DrawEllipse((int)ep.x, (int)ep.y, 138 * s, 28 * s, hx(0xC8E0A0));
    DrawEllipse((int)pp.x, (int)pp.y, 190 * s, 40 * s, hx(0xA8C880));
    DrawEllipse((int)pp.x, (int)pp.y, 176 * s, 33 * s, hx(0xC8E0A0));
    float cry = t > 1.6f && t < 2.0f ? fabsf(sinf((t - 1.6f) * 24)) * 10 * s : 0;    /* the cry: a hop */
    float ls = 130 * s;
    logo_draw(THEME_POKEMON, ep.x - ls / 2, ep.y - ls - 10 * s - cry, ls);
    /* the foe's box */
    float ba = eout(seg(t, 1.55f, 1.85f));
    Rectangle hb = { lerp(-340 * s, 40 * s, ba), H * 0.10f, 300 * s, 78 * s };
    pk_box(hb);
    ui_text(F_SEMI, 20 * s, "Q-CONTINUUM", hb.x + 18 * s, hb.y + 14 * s, hx(0x303030));
    ui_text(F_SEMI, 16 * s, "Lv26", hb.x + hb.width - 64 * s, hb.y + 16 * s, hx(0x303030));
    Rectangle bar = { hb.x + 70 * s, hb.y + 48 * s, hb.width - 90 * s, 10 * s };
    ui_text(F_SEMI, 12 * s, "HP", hb.x + 40 * s, bar.y - 3 * s, hx(0xF8B030));
    DrawRectangleRounded(bar, 1, 6, hx(0x505050));
    DrawRectangleRounded((Rectangle){ bar.x + 2 * s, bar.y + 2 * s, (bar.width - 4 * s) * ease(seg(t, 1.8f, 2.3f)), bar.height - 4 * s }, 1, 6, hx(0x58D068));
    /* the ball for our side */
    if (t > 2.85f && t < 3.45f) {
        float u = seg(t, 2.85f, 3.4f);
        Vector2 bc = { lerp(W * 0.02f, pp.x, u), lerp(H * 0.70f, pp.y - 40 * s, u) - sinf(u * PI) * H * 0.28f };
        ball(bc, 16 * s, u * 900, hx(0xE3350D));
    }
    if (t >= 3.4f) {
        float u = seg(t, 3.4f, 3.9f);
        DrawRing((Vector2){ pp.x, pp.y - 40 * s }, 90 * s * u, 90 * s * u + 10 * s, 0, 360, 48, al(WHITE, 1 - u));
        glow(pp.x, pp.y - 40 * s, 120 * s, WHITE, 0.9f * (1 - u));
        for (int k = 0; k < 8; k++) {
            float a = k * PI / 4, r = 30 * s + 110 * s * u;
            DrawPoly((Vector2){ pp.x + cosf(a) * r, pp.y - 40 * s + sinf(a) * r }, 4, 7 * s * (1 - u), 45, hx(0xF8D030));
        }
    }
    Rectangle tb = { 14 * s, H - 150 * s, W - 28 * s, 136 * s };
    pk_box(tb);
    const char *l1 = t < 2.8f ? "A wild Q-CONTINUUM appeared!" : "Go! NAVIER-STOKES!";
    float t0 = t < 2.8f ? 1.6f : 2.85f;
    typed(F_SEMI, 26 * s, l1, tb.x + 36 * s, tb.y + 40 * s, hx(0x404040), t, t0, 36, false);
    if (typed_n(l1, t, t0, 36) >= (float)strlen(l1) && fmodf(t * 3, 1) < 0.6f) {
        float ax = tb.x + tb.width - 50 * s, ay = tb.y + tb.height - 42 * s;
        tri((Vector2){ ax, ay }, (Vector2){ ax + 18 * s, ay }, (Vector2){ ax + 9 * s, ay + 11 * s }, hx(0xE04040));
    }
    if (t < 1.25f) fill(al(WHITE, 1 - seg(t, 0.95f, 1.25f)));         /* the flash out of the wipe */
}

/* ================================================================== JARVIS: the HUD spins up */
static float jv_ang(float v, float t) { return v * (t + 2.2f * (1 - expf(-2.0f * t))); }
static float jv_letter(int i, float t)
{
    float a0 = 2.15f + i * 0.045f;
    if (t < a0) return 0;
    if (t < a0 + 0.18f) return rnd((unsigned)(i * 53 + (int)(t * 40))) > 0.4f ? 1 : 0.2f;   /* flickers on */
    return 1;
}
static void sc_jarvis(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color cy = hx(0x00D8FF), lt = hx(0x99EFFF), bg = hx(0x02070D);
    fill(bg);
    float ga = ease(seg(t, 0.0f, 0.6f));
    for (float x = fmodf(W / 2, 40 * s); x < W; x += 40 * s) DrawRectangleRec((Rectangle){ x, 0, 1, H }, al(cy, 0.05f * ga));
    for (float y = fmodf(H / 2, 40 * s); y < H; y += 40 * s) DrawRectangleRec((Rectangle){ 0, y, W, 1 }, al(cy, 0.05f * ga));
    Vector2 C = { W / 2, H * 0.43f };
    glow(C.x, C.y, 260 * s, cy, 0.14f + 0.18f * ease(seg(t, 1.2f, 2.0f)));
    float a1 = ease(seg(t, 0.1f, 0.6f));
    DrawRing(C, 150 * s, 152 * s, -90, -90 + 360 * a1, 96, al(cy, 0.8f));
    for (int k = 0; k < 72; k++) {
        if (k / 72.0f > a1) break;
        float a = (k * 5.0f - 90 + jv_ang(6, t)) * DEG2RAD, r0 = k % 6 ? 156 * s : 154 * s, r1 = k % 6 ? 162 * s : 170 * s;
        DrawLineEx((Vector2){ C.x + cosf(a) * r0, C.y + sinf(a) * r0 }, (Vector2){ C.x + cosf(a) * r1, C.y + sinf(a) * r1 }, 1.4f * s, al(cy, 0.7f));
    }
    float a2 = ease(seg(t, 0.25f, 0.8f));
    for (int k = 0; k < 3; k++) {
        float b = jv_ang(40, t) + k * 120;
        DrawRing(C, 124 * s, 132 * s, b, b + 80 * a2, 32, al(cy, 0.85f));
    }
    float a3 = ease(seg(t, 0.4f, 0.95f));
    for (int k = 0; k < 36; k++) {
        float b = -jv_ang(25, t) + k * 10;
        DrawRing(C, 104 * s, 112 * s, b, b + 5, 4, al(cy, 0.6f * a3));
    }
    float a4 = ease(seg(t, 0.55f, 1.1f));
    for (int k = 0; k < 2; k++) {
        float b = jv_ang(90, t) * 0.6f + k * 180;
        DrawRing(C, 80 * s, 92 * s, b, b + 150 * a4, 48, al(lt, 0.55f));
    }
    float core = ease(seg(t, 1.0f, 1.9f));
    for (int k = 0; k < 10; k++) DrawRing(C, 34 * s, 58 * s, k * 36.0f + 5 + jv_ang(-15, t), k * 36.0f + 31 + jv_ang(-15, t), 8, al(cy, 0.25f + 0.6f * core));
    DrawCircleV(C, 30 * s, al(lt, 0.2f + 0.8f * core));
    glow(C.x, C.y, 90 * s, WHITE, 0.6f * core);
    float sw = t * 220;                                                /* the scanner */
    for (int k = 0; k < 8; k++) DrawCircleSector(C, 150 * s, sw - k * 4, sw - k * 4 + 4, 4, al(cy, 0.10f * (1 - k / 8.0f) * a1 * (1 - core * 0.7f)));
    float d = lerp(300 * s, 186 * s, eout(seg(t, 1.3f, 1.9f)));
    corners((Rectangle){ C.x - d, C.y - d, 2 * d, 2 * d }, 26 * s, 2.5f * s, al(cy, ease(seg(t, 1.3f, 1.6f))));
    /* the readouts */
    char cores[64]; snprintf(cores, sizeof cores, "SOLVER CLUSTER ........ %ld CORES", sysconf(_SC_NPROCESSORS_ONLN));
    const char *L[6] = { "BOOT SEQUENCE ......... INIT", "NAVIER-STOKES CORE .... ONLINE", "MESH GENERATION ....... ONLINE",
                         "PRESSURE-VELOCITY ..... COUPLED", "TURBULENCE MODELS ..... LOADED", cores };
    float lx = 36 * s, ly = H * 0.22f;
    if (W > 1000 * s) lx = 60 * s;
    for (int i = 0; i < 6; i++) typed(F_MONO, 12.5f * s, L[i], lx, ly + i * 22 * s, al(cy, 0.85f), t, 0.25f + i * 0.2f, 70, false);
    corners((Rectangle){ lx - 12 * s, ly - 14 * s, 300 * s, 6 * 22 * s + 20 * s }, 12 * s, 1.5f * s, al(cy, 0.5f * ga));
    float rx = W - lx - 220 * s, ry = H * 0.22f;
    for (int i = 0; i < 14; i++) {
        float hgt = (20 + 60 * fabsf(sinf(t * (1.3f + i * 0.37f) + i))) * s * ease(seg(t, 0.4f + i * 0.03f, 0.8f + i * 0.03f));
        DrawRectangleRec((Rectangle){ rx + i * 16 * s, ry + 100 * s - hgt, 10 * s, hgt }, al(cy, 0.55f));
    }
    char hexs[64]; snprintf(hexs, sizeof hexs, "0x%04X  0x%04X  0x%04X", (unsigned)(rnd((unsigned)(t * 15)) * 65535),
                            (unsigned)(rnd((unsigned)(t * 15) + 7) * 65535), (unsigned)(rnd((unsigned)(t * 15) + 9) * 65535));
    ui_text(F_MONO, 12 * s, hexs, rx, ry + 112 * s, al(cy, 0.6f * ga));
    corners((Rectangle){ rx - 12 * s, ry - 14 * s, 244 * s, 150 * s }, 12 * s, 1.5f * s, al(cy, 0.5f * ga));
    const char *word = "Q-CONTINUUM";
    float fs = 56 * s, ww = tw(F_SEMI, fs, word);
    letters(F_SEMI, fs, word, C.x - ww / 2, C.y + 196 * s, lt, jv_letter, t);
    struct tm m = now_tm();
    const char *gr = m.tm_hour < 5 ? "Working late, I see. All systems online." : m.tm_hour < 12 ? "Good morning. All systems online."
                   : m.tm_hour < 18 ? "Good afternoon. All systems online." : "Good evening. All systems online.";
    ctext(F_MED, 18 * s, gr, C.x, C.y + 196 * s + fs * 1.15f, al(cy, ease(seg(t, 2.75f, 3.2f))));
}

/* ================================================================== Severance: macrodata refinement */
static void globe(Vector2 c, float ro, float th, Color col, float a)
{
    DrawRing(c, ro - th, ro, 0, 360, 64, al(col, a));
    float ri = ro - th;
    for (int k = -2; k <= 2; k++) {
        float yy = c.y + k * ri * 0.36f, hw = sqrtf(fmaxf(0, ri * ri - (yy - c.y) * (yy - c.y)));
        DrawLineEx((Vector2){ c.x - hw, yy }, (Vector2){ c.x + hw, yy }, fmaxf(1, th * 0.35f), al(col, a * 0.75f));
    }
    for (int k = 1; k <= 2; k++) DrawEllipseLines((int)c.x, (int)c.y, ri * 0.38f * k, ri, al(col, a * 0.6f));
}
static void sc_severance(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color cy = hx(0x6FE3FF), bg = hx(0x02121A);
    fill(bg);
    float ha = ease(seg(t, 0.05f, 0.4f));
    Rectangle hd = { 40 * s, 26 * s, W - 80 * s, 50 * s };
    DrawRectangleLinesEx(hd, 1.5f * s, al(cy, ha));
    ui_text(F_MONO, 20 * s, "Navier", hd.x + 18 * s, hd.y + 13 * s, al(cy, ha));
    int pct = (int)(seg(t, 0.3f, 2.6f) * 100);
    char pc[24]; snprintf(pc, sizeof pc, "%d%% Complete", pct);
    ui_text(F_MONO, 18 * s, pc, hd.x + hd.width - 110 * s - tw(F_MONO, 18 * s, pc), hd.y + 14 * s, al(cy, ha));
    globe((Vector2){ hd.x + hd.width - 50 * s, hd.y + hd.height / 2 }, 19 * s, 3 * s, cy, ha);
    DrawRectangleRec((Rectangle){ hd.x, hd.y + hd.height + 8 * s, hd.width, 1.5f * s }, al(cy, ha * 0.8f));
    /* the bins */
    float bw = fminf(140 * s, (W - 120 * s) / 5 - 16 * s), bx0 = W / 2 - (5 * bw + 4 * 24 * s) / 2, byy = H - 96 * s;
    float fill3 = ease(seg(t, 1.9f, 2.6f));
    for (int b = 0; b < 5; b++) {
        float x = bx0 + b * (bw + 24 * s);
        Rectangle r = { x, byy, bw, 30 * s };
        DrawRectangleLinesEx(r, 1.5f * s, al(cy, ha));
        char nm[8]; snprintf(nm, sizeof nm, "0%d", b + 1);
        ctext(F_MONO, 16 * s, nm, x + bw / 2, byy + 6 * s, al(cy, ha));
        float f = b == 2 ? 0.37f + 0.63f * fill3 : 0.12f + 0.5f * rnd(b + 3);
        Rectangle pr = { x, byy + 38 * s, bw, 22 * s };
        DrawRectangleLinesEx(pr, 1.5f * s, al(cy, ha));
        DrawRectangleRec((Rectangle){ pr.x, pr.y, pr.width * f, pr.height }, al(cy, ha * (b == 2 && fill3 > 0 && fill3 < 1 ? 0.9f : 0.55f)));
        char fp[8]; snprintf(fp, sizeof fp, "%d%%", (int)(f * 100));
        ui_text(F_MONO, 13 * s, fp, pr.x + 6 * s, pr.y + 4 * s, f > 0.3f ? bg : cy);
    }
    /* the numbers: bobbing, and the scary ones swell round a wandering point, then go to bin 03 */
    float dim = 1 - 0.85f * ease(seg(t, 2.4f, 2.9f));
    float gx = 46 * s, gy = 40 * s, top = hd.y + hd.height + 28 * s, bottom = byy - 24 * s;
    Vector2 hot = { W * (0.35f + 0.25f * sinf(t * 0.9f)), lerp(top, bottom, 0.45f + 0.2f * cosf(t * 1.3f)) };
    if (t > 1.5f) hot = (Vector2){ W * (0.35f + 0.25f * sinf(1.5f * 0.9f)), lerp(top, bottom, 0.45f + 0.2f * cosf(1.5f * 1.3f)) };
    Vector2 bin = { bx0 + 2 * (bw + 24 * s) + bw / 2, byy + 10 * s };
    int nx = (int)((W - 80 * s) / gx), ny = (int)((bottom - top) / gy);
    float ox = W / 2 - (nx - 1) * gx / 2;
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            unsigned id = (unsigned)(j * 131 + i);
            float bx = ox + i * gx + sinf(t * 1.1f + rnd(id) * 6.28f) * 3 * s, by = top + j * gy + cosf(t * 0.9f + rnd(id + 1) * 6.28f) * 3 * s;
            float appear = ease(seg(t, 0.1f + rnd(id + 2) * 0.6f, 0.4f + rnd(id + 2) * 0.6f));
            float dd = hypotf(bx - hot.x, by - hot.y), sc = 1, a = appear * dim;
            bool scary = dd < 105 * s && t > 0.7f;
            if (scary) {
                sc = 1 + 0.8f * (1 - dd / (105 * s)) * ease(seg(t, 0.7f, 1.2f));
                bx += sinf(t * 23 + i) * 1.5f * s; by += cosf(t * 19 + j) * 1.5f * s;
                float go = ease(seg(t, 1.6f + rnd(id + 3) * 0.3f, 2.2f + rnd(id + 3) * 0.3f));
                bx = lerp(bx, bin.x, go); by = lerp(by, bin.y, go); sc = lerp(sc, 0.3f, go);
                a *= 1 - seg(go, 0.85f, 1.0f);
                if (go > 0) a = appear * (1 - seg(go, 0.85f, 1.0f));
            }
            char dg[2] = { (char)('0' + (int)(rnd(id + 77) * 10)), 0 };
            float fs = 20 * s * sc;
            if (a > 0) ui_text(F_MONO, fs, dg, bx - tw(F_MONO, fs, dg) / 2, by - fs * 0.6f, al(cy, a));
        }
    float la = ease(seg(t, 2.5f, 3.1f));
    if (la > 0) {
        Vector2 c = { W / 2, H * 0.40f };
        glow(c.x, c.y, 180 * s, cy, 0.18f * la);
        globe(c, 62 * s, 9 * s, cy, la);
        ctext(F_MONO, 34 * s, spaced("Q-CONTINUUM"), W / 2, c.y + 84 * s, al(cy, la));
        ctext(F_REG, 17 * s, "Please enjoy each simulation equally.", W / 2, c.y + 134 * s, al(cy, ease(seg(t, 3.0f, 3.4f)) * 0.85f));
    }
}

/* ================================================================== Alien: MU/TH/UR answers; the title assembles in fragments */
static void sc_alien(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color gr = hx(0x9DFFB4), dim = hx(0x2F7A45);
    fill(hx(0x000500));
    float hz = H * 0.66f, ta = ease(seg(t, 0.8f, 1.6f));       /* the nav display's terrain */
    if (ta > 0) {
        for (int k = 0; k < 14; k++) {
            float z = fmodf(k / 14.0f + t * 0.12f, 1.0f), yy = hz + (H - hz) * z * z;
            DrawRectangleRec((Rectangle){ 0, yy, W, 1.2f * s }, al(dim, ta * z));
        }
        for (int k = -14; k <= 14; k++) DrawLineEx((Vector2){ W / 2 + k * 18 * s, hz }, (Vector2){ W / 2 + k * 120 * s, H }, 1.2f * s, al(dim, ta * 0.6f));
        for (int k = 0; k < 40; k++) {                                /* a ridge line */
            float x0 = k * W / 40, x1 = (k + 1) * W / 40;
            float y0 = hz - (10 + 30 * rnd(k)) * s * (0.5f + 0.5f * sinf(k * 0.7f)), y1 = hz - (10 + 30 * rnd(k + 1)) * s * (0.5f + 0.5f * sinf((k + 1) * 0.7f));
            DrawLineEx((Vector2){ x0, y0 }, (Vector2){ x1, y1 }, 1.4f * s, al(gr, ta * 0.5f));
        }
    }
    const char *L[5] = { "INTERFACE 2606 READY FOR INQUIRY", "> REQUEST CLARIFICATION ON FLOW FIELD", "> PRIORITY ONE",
                         "> ENSURE CONVERGENCE OF ALL RESIDUALS.", "> ALL OTHER CONSIDERATIONS SECONDARY." };
    float ld = 1 - 0.6f * ease(seg(t, 1.9f, 2.4f));
    int cur = 0;
    for (int i = 0; i < 5; i++) if (t > 0.1f + i * 0.34f) cur = i;
    for (int i = 0; i < 5; i++)
        typed(F_REG, 24 * s, L[i], 56 * s, 50 * s + i * 30 * s, al(gr, ld), t, 0.1f + i * 0.34f, 80, i == cur);
    /* the title: each letter in four slices, lit at random */
    const char *word = "Q-CONTINUUM";
    int n = (int)strlen(word);
    float fs = 66 * s, cellw = fminf(W * 0.8f / n, 82 * s), x0 = W / 2 - cellw * n / 2, ty = H * 0.42f;
    for (int i = 0; i < n; i++) {
        char one[2] = { word[i], 0 };
        float lw = tw(F_MONO_MED, fs, one), lx = x0 + i * cellw + (cellw - lw) / 2;
        for (int k = 0; k < 4; k++) {
            float at = 1.3f + rnd((unsigned)(i * 4 + k + 11)) * 1.5f;
            if (t < at) continue;
            float fl = t < at + 0.12f ? (rnd((unsigned)(t * 60) + (unsigned)k) > 0.5f ? 1 : 0.3f) : 1;
            ui_clip_push((Rectangle){ lx + lw * k / 4 - 0.5f, ty - fs * 0.5f, lw / 4 + 1, fs * 2 });
            ui_text(F_MONO_MED, fs, one, lx, ty, al(hx(0xE8FFF0), fl));
            ui_clip_pop();
        }
    }
    glow(W / 2, ty + fs * 0.4f, W * 0.4f, gr, 0.10f * ease(seg(t, 2.6f, 3.1f)));
    ctext(F_REG, 20 * s, "IN SPACE NO ONE CAN HEAR YOUR SOLVER", W / 2, ty + fs * 1.5f, al(gr, ease(seg(t, 3.0f, 3.5f)) * 0.8f));
}

/* ================================================================== Pip-Boy: the boot scroll, the vault-door cog rolls in */
static void sc_pipboy(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color gr = hx(0x3CFF7A), bg = hx(0x041306);
    fill(bg);
    if (t < 1.35f) {
        static const char *const B[] = { "Q-OS(R) V2.0.6 BOOT LOADER", "COPYRIGHT 2077 Q-CONTINUUM INDUSTRIES", "",
            "> SET TERMINAL/INQUIRE", "RX-2606 TERMINAL FOUND", "> SET FILE/PROTECTION=OWNER:RWED CONSTANT/*", "> RUN DEBUG/MESH.SYS", "",
            "64K RAM SYSTEM", "38911 BYTES FREE", "", "MESH CHECK .................. OK", "BLOCKMESH ................... OK",
            "SNAPPYHEXMESH ............... OK", "SIMPLE ...................... OK", "PISO ........................ OK",
            "K-OMEGA SST ................. OK", "RADIATION SHIELDING ......... OK", "COOLANT PUMPS ............... OK", "",
            "0x000A4F 0x0000 0x6E61 0x7669 0x6572", "0x000A5F 0x7374 0x6F6B 0x6573 0x2121", "", "INITIALIZING Q-OS ..." };
        int nb = (int)(sizeof B / sizeof B[0]);
        float lh = 24 * s;
        int shown = (int)(t * 20) + 1; if (shown > nb) shown = nb;
        int fit = (int)((H - 80 * s) / lh);
        float scroll = shown > fit ? (shown - fit) * lh : 0;
        for (int i = 0; i < shown; i++) ui_text(F_REG, 19 * s, B[i], 50 * s, 40 * s + i * lh - scroll, gr);
        return;
    }
    if (t < 1.55f) {                                                /* the screen collapses to a line and comes back */
        float u = seg(t, 1.35f, 1.55f), hh = H * fabsf(1 - 2 * u) * 0.5f;
        DrawRectangleRec((Rectangle){ 0, H / 2 - fmaxf(hh, 1.5f * s), W, 2 * fmaxf(hh, 1.5f * s) }, al(gr, 0.25f + 0.75f * (hh < 4 * s)));
        return;
    }
    float ro = 72 * s, roll = eout(seg(t, 1.55f, 2.3f));
    Vector2 c = { lerp(-ro * 2, W / 2 - ro * 0.4f, roll), H * 0.38f };
    float dist = c.x + ro * 2, rot = dist / ro * RAD2DEG;
    glow(c.x, c.y, ro * 2.4f, gr, 0.18f);
    cog(c, ro, ro - 20 * s, 12, 13 * s, rot, gr);
    DrawCircleV(c, 17 * s, al(gr, 0.6f));
    float tl = eout(seg(t, 2.3f, 2.55f));
    if (tl > 0) {
        Vector2 a = { c.x + ro * 0.4f, c.y + ro * 0.4f }, b = { c.x + ro * 1.55f, c.y + ro * 1.45f };
        DrawLineEx(a, (Vector2){ lerp(a.x, b.x, tl), lerp(a.y, b.y, tl) }, 22 * s, gr);
    }
    ctext(F_SEMI, 54 * s, "Q-CONTINUUM", W / 2, c.y + ro * 1.85f, al(gr, ease(seg(t, 2.45f, 2.75f))));
    ctext(F_REG, 18 * s, "VAULT 2606  //  FLUID DYNAMICS DIVISION", W / 2, c.y + ro * 1.85f + 66 * s, al(gr, ease(seg(t, 2.65f, 2.95f)) * 0.85f));
    float sa = ease(seg(t, 2.6f, 2.9f));
    float y = H - 76 * s, bw = (W - 100 * s) / 3;
    const char *box[3] = { "HP 115/115", "LEVEL 26", "AP 90/90" };
    for (int i = 0; i < 3; i++) {
        Rectangle r = { 50 * s + i * bw, y, bw - 10 * s, 30 * s };
        DrawRectangleRec(r, al(gr, 0.18f * sa));
        if (i == 1) {
            float xp = ease(seg(t, 2.9f, 3.5f));
            DrawRectangleRec((Rectangle){ r.x + 100 * s, r.y + 10 * s, (r.width - 110 * s) * xp, 10 * s }, al(gr, sa));
            DrawRectangleLinesEx((Rectangle){ r.x + 100 * s, r.y + 10 * s, r.width - 110 * s, 10 * s }, 1.5f * s, al(gr, sa));
        }
        ui_text(F_REG, 18 * s, box[i], r.x + 10 * s, r.y + 5 * s, al(gr, sa));
    }
    if (t > 3.5f && fmodf(t * 4, 1) < 0.7f) ctext(F_SEMI, 22 * s, "LEVEL UP!", W / 2, y - 40 * s, gr);
}

/* ================================================================== NERV: EMERGENCY, the MAGI vote, the title cards */
static void sc_nerv(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color red = hx(0xFF2A2A), org = hx(0xFF6A00), wh = hx(0xF2EDE4), grn = hx(0x37E07A);
    fill(hx(0x050302));
    if (t < 1.75f) {
        float r = 34 * s, hw = r * 1.732f;
        for (int j = -1; j * r * 1.5f < H + r; j++)
            for (int i = -1; i * hw < W + hw; i++) {
                Vector2 c = { i * hw + (j & 1) * hw / 2, j * r * 1.5f };
                float d = hypotf(c.x - W / 2, c.y - H / 2) / W;
                float pulse = 0.5f + 0.5f * sinf(t * 9 - d * 14);
                float on = ease(seg(t, d * 1.2f, d * 1.2f + 0.2f));
                DrawPolyLinesEx(c, 6, r - 3 * s, 30, 2 * s, al((i + j) % 3 ? org : red, on * (0.25f + 0.5f * pulse)));
                if (rnd((unsigned)(i * 31 + j * 17 + (int)(t * 6))) > 0.93f) DrawPoly(c, 6, r - 6 * s, 30, al(red, on * 0.5f));
            }
        Rectangle bnr = { -2, H * 0.30f, W + 4, 150 * s };
        DrawRectangleRec(bnr, al(BLACK, 0.85f));
        for (float x = -60 * s + fmodf(t * 120 * s, 40 * s); x < W + 40 * s; x += 40 * s) {   /* hazard stripes */
            tri((Vector2){ x, bnr.y }, (Vector2){ x + 20 * s, bnr.y }, (Vector2){ x + 32 * s, bnr.y + 16 * s }, red);
            tri((Vector2){ x, bnr.y }, (Vector2){ x + 32 * s, bnr.y + 16 * s }, (Vector2){ x + 12 * s, bnr.y + 16 * s }, red);
            float yb = bnr.y + bnr.height - 16 * s;
            tri((Vector2){ x, yb }, (Vector2){ x + 20 * s, yb }, (Vector2){ x + 32 * s, yb + 16 * s }, red);
            tri((Vector2){ x, yb }, (Vector2){ x + 32 * s, yb + 16 * s }, (Vector2){ x + 12 * s, yb + 16 * s }, red);
        }
        if (fmodf(t * 3, 1) < 0.65f) stext(F_SEMI, 64 * s, "EMERGENCY", W / 2, bnr.y + bnr.height / 2, 1.25f, 1.0f, red);
        const char *pat = t < 0.9f ? "PATTERN ANALYSIS ..." : "PATTERN: TURBULENT  -  CONFIRMED";
        ctext(F_MONO, 16 * s, pat, W / 2, bnr.y - 30 * s, t < 0.9f ? org : red);
        static const char *const MAGI[3] = { "MELCHIOR-1", "BALTHASAR-2", "CASPER-3" };
        for (int k = 0; k < 3; k++) {
            float x = W / 2 + (k - 1) * 250 * s, y = H * 0.74f;
            bool ok = t > 1.05f + k * 0.17f;
            Rectangle r = { x - 115 * s, y, 230 * s, 64 * s };
            DrawRectangleRec(r, al(ok ? grn : red, 0.18f));
            DrawRectangleLinesEx(r, 2 * s, ok ? grn : red);
            ctext(F_MONO, 14 * s, MAGI[k], x, y + 10 * s, wh);
            ctext(F_SEMI, 19 * s, ok ? "APPROVE" : "DELIBERATING", x, y + 32 * s, ok ? grn : org);
        }
        if (t < 0.18f) fill(al(red, 1 - t / 0.18f));
        return;
    }
    if (t < 1.9f) return;                                          /* a beat of black */
    if (t < 2.5f) { stext(F_SEMI, 110 * s, "Q-CONTINUUM", W / 2, H / 2, 0.82f, 1.7f, wh); return; }
    float lx = W * 0.09f;
    if (t > 2.5f) {
        rlDrawRenderBatchActive();
        rlPushMatrix(); rlTranslatef(lx, H * 0.16f, 0); rlScalef(0.86f, 1.35f, 1);
        ui_text(F_SEMI, 40 * s, "NAVIER-STOKES", 0, 0, wh);
        ui_text(F_SEMI, 92 * s, "Q-CONTINUUM", 0, 40 * s, wh);
        rlDrawRenderBatchActive(); rlPopMatrix();
    }
    if (t > 2.85f) {
        rlDrawRenderBatchActive();
        rlPushMatrix(); rlTranslatef(lx, H * 0.56f, 0); rlScalef(0.86f, 1.35f, 1);
        ui_text(F_SEMI, 44 * s, "EPISODE:2.0", 0, 0, wh);
        rlDrawRenderBatchActive(); rlPopMatrix();
    }
    if (t > 3.2f) {
        float fs = 70 * s, ww = tw(F_SEMI, fs, "TURBULENCE ATTACK") * 0.86f;
        rlDrawRenderBatchActive();
        rlPushMatrix(); rlTranslatef(W * 0.92f - ww, H * 0.70f, 0); rlScalef(0.86f, 1.35f, 1);
        ui_text(F_SEMI, fs, "TURBULENCE ATTACK", 0, 0, wh);
        rlDrawRenderBatchActive(); rlPopMatrix();
    }
}

/* ================================================================== Portal: two portals, a flung tile, the chamber sign */
static void portal(Vector2 c, float k, Color col, float t)
{
    if (k <= 0) return;
    float rx = 46 * g.s * k, ry = 92 * g.s * k;
    glow(c.x, c.y, ry * 1.6f, col, 0.35f);
    DrawEllipse((int)c.x, (int)c.y, rx + 6 * g.s, ry + 6 * g.s, al(col, 0.45f));
    DrawEllipse((int)c.x, (int)c.y, rx, ry, col);
    DrawEllipse((int)c.x, (int)c.y, rx * 0.8f, ry * 0.86f, mix(col, hx(0x10202A), 0.55f));
    for (int i = 0; i < 4; i++) {
        float f = fmodf(t * 0.8f + i * 0.25f, 1.0f);
        DrawEllipseLines((int)c.x, (int)c.y, rx * 0.8f * (1 - f), ry * 0.86f * (1 - f), al(col, f * 0.8f));
    }
}
static void sc_portal(float t)
{
    float s = g.s, W = g.W, H = g.H;
    fill(hx(0xC9D0D4));
    float ts = 118 * s;
    for (int j = 0; j * ts < H; j++)
        for (int i = 0; i * ts < W; i++) {
            float k = eout(seg(t, (i + j) * 0.04f, (i + j) * 0.04f + 0.3f));
            Rectangle r = { i * ts + 3 * s, j * ts + 3 * s, ts - 6 * s, ts - 6 * s };
            Rectangle q = { r.x + r.width / 2 * (1 - k), r.y, r.width * k, r.height };
            DrawRectangleRec(q, mix(hx(0xF2F4F5), hx(0xE2E7EA), rnd((unsigned)(i * 7 + j * 13))));
        }
    float floorY = H * 0.80f;
    DrawRectangleRec((Rectangle){ -2, floorY, W + 4, H }, hx(0x9AA4AA));
    DrawRectangleRec((Rectangle){ -2, floorY, W + 4, 4 * s }, hx(0x7D878D));
    float close = 1 - ease(seg(t, 2.7f, 3.0f));
    Vector2 bp = { W * 0.16f, floorY - 100 * s }, op = { W * 0.84f, H * 0.36f };
    portal(bp, eback(seg(t, 0.55f, 0.9f)) * close, hx(0x1A9BFF), t);
    portal(op, eback(seg(t, 0.8f, 1.15f)) * close, hx(0xFF8A00), t);
    float ls = 92 * s;
    if (t > 1.15f && t < 1.55f) {                                   /* into the blue one */
        float u = seg(t, 1.15f, 1.5f), x = lerp(-ls, bp.x, u);
        ui_clip_push((Rectangle){ 0, 0, bp.x, H });
        logo_draw(THEME_PORTAL, x - ls / 2, bp.y - ls / 2, ls);
        ui_clip_pop();
    }
    if (t >= 1.5f) {                                                 /* out of the orange one, flung, and it lands */
        float u = seg(t, 1.5f, 2.15f), x = lerp(op.x, W / 2, u);
        float yl = floorY - ls / 2 - 2 * s, y = lerp(op.y, yl, u) - sinf(u * PI) * H * 0.16f;
        float squash = t > 2.15f ? 1 - 0.15f * sinf(seg(t, 2.15f, 2.4f) * PI) : 1;
        if (u < 0.25f) ui_clip_push((Rectangle){ 0, 0, op.x, H });
        float lw = ls / squash * (2 - squash) * 0.5f + ls * 0.5f, lh = ls * squash;
        logo_draw(THEME_PORTAL, x - lw / 2, y + ls / 2 - lh, ls);
        if (u < 0.25f) ui_clip_pop();
    }
    float sa = t > 2.3f ? (t < 2.45f ? (rnd((unsigned)(t * 50)) > 0.5f ? 1.0f : 0.0f) : 1.0f) : 0;   /* the sign lights flicker on */
    if (sa > 0) {
        Rectangle sg = { W * 0.06f, H * 0.08f, 190 * s, 270 * s };
        DrawRectangleRec(sg, hx(0xF7F8F8));
        DrawRectangleLinesEx(sg, 2 * s, hx(0xB9C3C9));
        ui_text(F_SEMI, 96 * s, "26", sg.x + 14 * s, sg.y + 6 * s, hx(0x2B3135));
        ui_text(F_SEMI, 26 * s, "/06", sg.x + 128 * s, sg.y + 64 * s, hx(0x2B3135));
        for (int k = 0; k < 10; k++) DrawRectangleRec((Rectangle){ sg.x + 14 * s + k * 16.5f * s, sg.y + 140 * s, 12 * s, 8 * s }, k < 7 ? hx(0x2B3135) : hx(0xC9D0D4));
        for (int k = 0; k < 5; k++) {
            Rectangle ic = { sg.x + 14 * s + k * 34 * s, sg.y + 166 * s, 28 * s, 28 * s };
            DrawRectangleRec(ic, k == 1 || k == 3 ? hx(0x2B3135) : hx(0xC9D0D4));
        }
        ui_text(F_MED, 11 * s, "COMPUTATIONAL", sg.x + 14 * s, sg.y + 210 * s, hx(0x2B3135));
        ui_text(F_MED, 11 * s, "ENRICHMENT CENTER", sg.x + 14 * s, sg.y + 226 * s, hx(0x2B3135));
    }
    float wa = ease(seg(t, 2.45f, 2.85f));
    ctext(F_SEMI, 50 * s, "Q-Continuum", W / 2, H * 0.30f, al(hx(0x2B3135), wa));
    ctext(F_REG, 18 * s, "Computational Enrichment Center", W / 2, H * 0.30f + 64 * s, al(hx(0x5B666D), wa));
    ctext(F_REG, 13 * s, "Cake will be served at the end of the simulation.", W / 2, floorY + 26 * s, al(hx(0x3E474D), ease(seg(t, 3.1f, 3.5f))));
}

/* ================================================================== Souls: embers, a bonfire, SIMULATION KINDLED */
static void sc_souls(float t)
{
    float s = g.s, W = g.W, H = g.H;
    fill(hx(0x050403));
    float kin = ease(seg(t, 0.2f, 1.3f));
    Vector2 f = { W / 2, H * 0.84f };
    glow(f.x, f.y, 420 * s, hx(0xFF7A20), 0.28f * kin);
    for (int i = 0; i < 80; i++) {                                    /* embers */
        float sp = (40 + 60 * rnd(i)) * s, life = H * 0.9f;
        float y = f.y - fmodf((t + rnd(i + 3) * 10) * sp, life), x = f.x + (rnd(i + 7) - 0.5f) * W * 0.7f * (1 - (y / f.y)) + sinf(t * 1.6f + i) * 16 * s;
        float a = kin * (y / f.y) * (0.5f + 0.5f * sinf(t * 9 + i * 2.1f));
        DrawCircleV((Vector2){ x, y }, (1.2f + 1.6f * rnd(i + 11)) * s, al(hx(i % 4 ? 0xFF9A40 : 0xFFD080), a));
    }
    DrawEllipse((int)f.x, (int)(f.y + 6 * s), 90 * s, 18 * s, hx(0x24201C));
    DrawEllipse((int)f.x, (int)(f.y + 4 * s), 70 * s, 12 * s, hx(0x3A332C));
    for (int layer = 0; layer < 3; layer++) {                         /* the flames */
        Color fc = layer == 0 ? hx(0xC23A10) : layer == 1 ? hx(0xFF8A20) : hx(0xFFD27A);
        float sc = 1 - layer * 0.28f;
        for (int k = 0; k < 7; k++) {
            float xo = (k - 3) * 13 * s * sc, h = (50 + 40 * (0.5f + 0.5f * sinf(t * (7 + k) + k * 1.3f))) * s * sc * kin * (1 - fabsf((float)k - 3) / 5.0f);
            float wv = sinf(t * 5 + k) * 6 * s;
            tri((Vector2){ f.x + xo - 12 * s * sc, f.y }, (Vector2){ f.x + xo + 12 * s * sc, f.y }, (Vector2){ f.x + xo + wv, f.y - h }, al(fc, 0.9f));
        }
    }
    DrawLineEx((Vector2){ f.x + 8 * s, f.y - 96 * s }, (Vector2){ f.x - 4 * s, f.y + 4 * s }, 6 * s, hx(0x9A948A));   /* the coiled sword */
    DrawLineEx((Vector2){ f.x - 12 * s, f.y - 78 * s }, (Vector2){ f.x + 26 * s, f.y - 74 * s }, 5 * s, hx(0x6A5A40));
    for (int k = 0; k < 4; k++) DrawRing((Vector2){ f.x + 2 * s - k * 1.5f * s, f.y - 20 * s - k * 10 * s }, 6 * s, 8.5f * s, 200, 340, 8, hx(0x7A6A50));
    float ta = ease(seg(t, 0.9f, 2.0f));
    const char *word = "Q-CONTINUUM";
    float fs = 66 * s, ww = tw(F_SEMI, fs, word), ty = H * 0.22f;
    ctext(F_SEMI, fs, word, W / 2, ty, al(hx(0xD8CFC0), ta));
    float ol = 140 * s * ta;
    DrawRectangleRec((Rectangle){ W / 2 - ww / 2 - 24 * s - ol, ty + fs * 0.62f, ol, 1.2f * s }, al(hx(0xA8905A), ta));
    DrawRectangleRec((Rectangle){ W / 2 + ww / 2 + 24 * s, ty + fs * 0.62f, ol, 1.2f * s }, al(hx(0xA8905A), ta));
    DrawPoly((Vector2){ W / 2 - ww / 2 - 24 * s - ol, ty + fs * 0.62f + 0.6f * s }, 4, 4 * s, 0, al(hx(0xA8905A), ta));
    DrawPoly((Vector2){ W / 2 + ww / 2 + 24 * s + ol, ty + fs * 0.62f + 0.6f * s }, 4, 4 * s, 0, al(hx(0xA8905A), ta));
    float ba = ease(seg(t, 2.35f, 2.85f));
    if (ba > 0) {
        float by = H * 0.50f, bh = 110 * s;
        vgrad(-2, by - bh / 2, W + 4, bh / 2, al(BLACK, 0), al(BLACK, 0.8f * ba));
        vgrad(-2, by, W + 4, bh / 2, al(BLACK, 0.8f * ba), al(BLACK, 0));
        float k = 1 + 0.06f * seg(t, 2.35f, 4.2f);
        stext(F_SEMI, 58 * s, "SIMULATION KINDLED", W / 2, by, k * 1.012f, k * 1.03f, al(hx(0xFFB050), 0.16f * ba));
        stext(F_SEMI, 58 * s, "SIMULATION KINDLED", W / 2, by, k, k, al(hx(0xFFD68A), ba));
    }
}

/* ================================================================== Factorio: the belt feeds the assembler; research completed */
static void item(int kind, Vector2 c, float s)
{
    switch (kind % 4) {
    case 0: cog(c, 8 * s, 4 * s, 8, 3 * s, 0, hx(0xA8ADB2)); break;
    case 1: DrawRectangleRec((Rectangle){ c.x - 9 * s, c.y - 6 * s, 18 * s, 12 * s }, hx(0xC87533)); break;
    case 2: DrawRectangleRec((Rectangle){ c.x - 8 * s, c.y - 8 * s, 16 * s, 16 * s }, hx(0x3BAA3B));
            DrawRectangleRec((Rectangle){ c.x - 5 * s, c.y - 1 * s, 10 * s, 2 * s }, hx(0xD8E070)); break;
    default: DrawRectangleRec((Rectangle){ c.x - 9 * s, c.y - 6 * s, 18 * s, 12 * s }, hx(0x9DA3A8)); break;
    }
}
static void belt(float x0, float x1, float y, float t)
{
    float s = g.s;
    DrawRectangleRec((Rectangle){ x0, y - 18 * s, x1 - x0, 36 * s }, hx(0x3C3A36));
    DrawRectangleRec((Rectangle){ x0, y - 18 * s, x1 - x0, 4 * s }, hx(0x262422));
    DrawRectangleRec((Rectangle){ x0, y + 14 * s, x1 - x0, 4 * s }, hx(0x262422));
    for (float x = x0 + fmodf(t * 70 * s, 26 * s); x < x1; x += 26 * s) {
        DrawLineEx((Vector2){ x - 6 * s, y - 8 * s }, (Vector2){ x, y }, 2.5f * s, al(hx(0xE8C23A), 0.7f));
        DrawLineEx((Vector2){ x - 6 * s, y + 8 * s }, (Vector2){ x, y }, 2.5f * s, al(hx(0xE8C23A), 0.7f));
    }
}
static void sc_factorio(float t)
{
    float s = g.s, W = g.W, H = g.H;
    fill(hx(0x2B2A28));
    float ts = 32 * s;
    for (int j = 0; j * ts < H; j++)
        for (int i = 0; i * ts < W; i++)
            if (rnd((unsigned)(i * 37 + j * 91)) > 0.6f) DrawRectangleRec((Rectangle){ i * ts, j * ts, ts, ts }, al(hx(0x353330), 0.6f + 0.4f * rnd((unsigned)(i + j * 7))));
    float by = H * 0.60f, as = 150 * s, ax = W / 2;
    belt(-2, ax - as / 2, by, t);
    belt(ax + as / 2, W + 2, by, t);
    for (int k = 0; k < 20; k++) {                                       /* items in */
        float x = fmodf(t * 70 * s + k * 64 * s, ax - as / 2 + 40 * s) - 40 * s;
        if (x < ax - as / 2 - 8 * s) item(k, (Vector2){ x, by }, s);
    }
    for (int k = 0; k < 12; k++) {                                       /* Q gears out, once it is running */
        float born = 1.0f + k * 0.55f;
        if (t < born) continue;
        float x = ax + as / 2 + (t - born) * 70 * s;
        if (x > W + 20 * s) continue;
        cog((Vector2){ x, by }, 9 * s, 5 * s, 8, 3 * s, (t - born) * 200, hx(0xFF9F1C));
        DrawLineEx((Vector2){ x + 4 * s, by + 4 * s }, (Vector2){ x + 12 * s, by + 12 * s }, 3 * s, hx(0xFF9F1C));
    }
    Rectangle ab = { ax - as / 2, by - as / 2, as, as };                 /* the assembler */
    DrawRectangleRec((Rectangle){ ab.x + 8 * s, ab.y + 10 * s, as, as }, al(BLACK, 0.45f));
    DrawRectangleRec(ab, hx(0x5A5650));
    DrawRectangleLinesEx(ab, 4 * s, hx(0x2E2C29));
    DrawRectangleRec((Rectangle){ ab.x + 6 * s, ab.y + 6 * s, as - 12 * s, 3 * s }, hx(0x7A766E));
    cog((Vector2){ ax, by - 8 * s }, 40 * s, 24 * s, 10, 10 * s, t * 120, hx(0x8A857C));
    DrawCircleV((Vector2){ ax, by - 8 * s }, 12 * s, hx(0x3C3A36));
    float prog = fmodf(t * 1.8f, 1.0f);
    DrawRectangleRec((Rectangle){ ab.x + 16 * s, ab.y + as - 24 * s, as - 32 * s, 8 * s }, hx(0x2E2C29));
    DrawRectangleRec((Rectangle){ ab.x + 16 * s, ab.y + as - 24 * s, (as - 32 * s) * prog, 8 * s }, hx(0x37C837));
    if (fmodf(t * 2, 1) < 0.5f) DrawCircleV((Vector2){ ab.x + as - 16 * s, ab.y + 16 * s }, 5 * s, hx(0x37E037));
    for (int k = 0; k < 4; k++) {                                         /* smoke */
        float u = fmodf(t * 0.6f + k * 0.25f, 1.0f);
        DrawCircleV((Vector2){ ab.x + as - 30 * s + u * 30 * s, ab.y - u * 90 * s }, (8 + 18 * u) * s, al(hx(0x8A8580), 0.35f * (1 - u)));
    }
    /* research */
    Rectangle rp = { W / 2 - 280 * s, 34 * s, 560 * s, 76 * s };
    DrawRectangleRec(rp, hx(0x3A3936)); DrawRectangleLinesEx(rp, 3 * s, hx(0x1E1D1C));
    DrawRectangleRec((Rectangle){ rp.x + 3 * s, rp.y + 3 * s, rp.width - 6 * s, 2 * s }, hx(0x57544E));
    Color flask[3] = { hx(0xE04838), hx(0x48C848), hx(0x3878E0) };
    for (int k = 0; k < 3; k++) DrawRectangleRounded((Rectangle){ rp.x + 16 * s + k * 20 * s, rp.y + 18 * s, 14 * s, 40 * s }, 0.5f, 6, flask[k]);
    float rprog = ease(seg(t, 0.3f, 2.3f));
    bool done = t >= 2.3f;
    ui_text(F_SEMI, 19 * s, done ? "Research completed" : "Researching: Computational fluid dynamics", rp.x + 90 * s, rp.y + 12 * s, done ? hx(0xFFD27A) : hx(0xFFE6C0));
    Rectangle pb = { rp.x + 90 * s, rp.y + 46 * s, rp.width - 110 * s, 14 * s };
    DrawRectangleRec(pb, hx(0x1E1D1C));
    DrawRectangleRec((Rectangle){ pb.x, pb.y, pb.width * rprog, pb.height }, done ? hx(0xFF9F1C) : hx(0x48A0E0));
    float wa = ease(seg(t, 2.45f, 2.85f));
    ctext(F_SEMI, 60 * s, "Q-CONTINUUM", W / 2 + 3 * s, H * 0.25f + 3 * s, al(BLACK, 0.5f * wa));
    ctext(F_SEMI, 60 * s, "Q-CONTINUUM", W / 2, H * 0.25f, al(hx(0xFF9F1C), wa));
    ctext(F_SEMI, 19 * s, "The mesh must grow.", W / 2, H * 0.25f + 76 * s, al(hx(0xE8DCC8), ease(seg(t, 2.9f, 3.3f))));
}

/* ================================================================== Windows 95: starting up, clouds, a welcome dialog */
static void w95_bevel(Rectangle r, bool sunk)
{
    float b = fmaxf(1, 1.5f * g.s);
    DrawRectangleRec(r, hx(0xC0C0C0));
    Color a = sunk ? BLACK : WHITE, c = sunk ? WHITE : BLACK;
    DrawRectangleRec((Rectangle){ r.x, r.y, r.width, b }, a); DrawRectangleRec((Rectangle){ r.x, r.y, b, r.height }, a);
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - b, r.width, b }, c); DrawRectangleRec((Rectangle){ r.x + r.width - b, r.y, b, r.height }, c);
    DrawRectangleRec((Rectangle){ r.x + b, r.y + r.height - 2 * b, r.width - 2 * b, b }, hx(0x808080));
    DrawRectangleRec((Rectangle){ r.x + r.width - 2 * b, r.y + b, b, r.height - 2 * b }, hx(0x808080));
}
static void arrow(float x, float y, float s)
{
    Vector2 p[7] = { { 0, 0 }, { 0, 17 }, { 4, 13 }, { 7, 20 }, { 10, 19 }, { 7, 12 }, { 12, 12 } };
    for (int i = 0; i < 7; i++) { p[i].x = x + p[i].x * s * 1.2f; p[i].y = y + p[i].y * s * 1.2f; }
    tri(p[0], p[1], p[6], WHITE); tri(p[2], p[3], p[4], WHITE); tri(p[2], p[4], p[5], WHITE);
    for (int i = 0; i < 7; i++) DrawLineEx(p[i], p[(i + 1) % 7], 1.2f * s, BLACK);
}
static void sc_win95(float t)
{
    float s = g.s, W = g.W, H = g.H;
    if (t < 0.75f) {
        fill(BLACK);
        if (t > 0.1f) ui_text(F_MONO, 16 * s, "Starting Q-Continuum 95...", 20 * s, 20 * s, hx(0xC0C0C0));
        if (fmodf(t * 3, 1) < 0.5f) DrawRectangleRec((Rectangle){ 20 * s, 48 * s, 10 * s, 3 * s }, hx(0xC0C0C0));
        return;
    }
    if (t < 2.4f) {
        vgrad(-2, -2, W + 4, H + 4, hx(0x3E6FD0), hx(0xA9C9F2));
        for (int i = 0; i < 12; i++) {
            float cx = fmodf(rnd(i) * W + t * 14 * s * (1 + rnd(i + 5)), W + 300 * s) - 150 * s, cy = rnd(i + 20) * H * 0.85f;
            float r = (26 + 30 * rnd(i + 9)) * s;
            for (int k = 0; k < 5; k++)
                DrawCircleV((Vector2){ cx + (k - 2) * r * 0.7f, cy + (k % 2) * r * 0.25f }, r * (k == 2 ? 1.0f : 0.7f), al(WHITE, 0.85f));
        }
        float ls = 110 * s, fs = 60 * s, ww = tw(F_SEMI, fs, "Q-Continuum"), gx = W / 2 - (ls + 24 * s + ww + 80 * s) / 2, cy = H * 0.42f;
        logo_draw(THEME_WIN95, gx, cy - ls / 2, ls);
        ui_text(F_SEMI, fs, "Q-Continuum", gx + ls + 24 * s + 2 * s, cy - fs * 0.62f + 2 * s, al(BLACK, 0.5f));
        ui_text(F_SEMI, fs, "Q-Continuum", gx + ls + 24 * s, cy - fs * 0.62f, WHITE);
        ui_text(F_SEMI, fs * 1.2f, "95", gx + ls + 24 * s + ww + 8 * s, cy - fs * 0.9f, hx(0xFFCC00));
        float bh = 14 * s, y = H - bh;                                 /* the moving bar */
        DrawRectangleRec((Rectangle){ -2, y, W + 4, bh }, hx(0x000080));
        float off = fmodf(t * 420 * s, W);
        for (int k = -1; k < 2; k++) {
            float x = off + k * W;
            hgrad(x, y, W / 2, bh, hx(0x000080), hx(0x5AA0FF));
            hgrad(x + W / 2, y, W / 2, bh, hx(0x5AA0FF), hx(0x000080));
        }
        return;
    }
    fill(hx(0x008080));
    float tbh = 30 * s;                                                  /* the taskbar */
    w95_bevel((Rectangle){ -2, H - tbh, W + 4, tbh + 2 }, false);
    Rectangle st = { 4 * s, H - tbh + 4 * s, 80 * s, tbh - 8 * s };
    w95_bevel(st, false);
    logo_draw(THEME_WIN95, st.x + 4 * s, st.y + 2 * s, st.height - 4 * s);
    ui_text(F_SEMI, 14 * s, "Start", st.x + st.height + 6 * s, st.y + 3 * s, BLACK);
    struct tm m = now_tm();
    char clk[16]; snprintf(clk, sizeof clk, "%d:%02d %s", m.tm_hour % 12 ? m.tm_hour % 12 : 12, m.tm_min, m.tm_hour < 12 ? "AM" : "PM");
    Rectangle ck = { W - 90 * s, st.y, 86 * s, st.height };
    w95_bevel(ck, true);
    ctext(F_REG, 13 * s, clk, ck.x + ck.width / 2, ck.y + 4 * s, BLACK);
    Rectangle wr = { W / 2 - 240 * s, H * 0.42f - 120 * s, 480 * s, 230 * s };
    if (t < 2.62f) {                                                     /* the zoom outline from the Start button */
        float u = seg(t, 2.4f, 2.62f);
        Rectangle z = { lerp(st.x, wr.x, u), lerp(st.y, wr.y, u), lerp(st.width, wr.width, u), lerp(st.height, wr.height, u) };
        DrawRectangleLinesEx(z, 2 * s, hx(0x808080));
        return;
    }
    if (t < 3.35f) {
        DrawRectangleRec((Rectangle){ wr.x + 3 * s, wr.y + 3 * s, wr.width, wr.height }, al(BLACK, 0.5f));
        w95_bevel(wr, false);
        hgrad(wr.x + 4 * s, wr.y + 4 * s, wr.width - 8 * s, 24 * s, hx(0x000080), hx(0x1084D0));
        ui_text(F_SEMI, 14 * s, "Welcome", wr.x + 12 * s, wr.y + 7 * s, WHITE);
        Rectangle xb = { wr.x + wr.width - 26 * s, wr.y + 7 * s, 18 * s, 16 * s };
        w95_bevel(xb, false);
        ctext(F_SEMI, 12 * s, "x", xb.x + xb.width / 2, xb.y + 1 * s, BLACK);
        logo_draw(THEME_WIN95, wr.x + 20 * s, wr.y + 46 * s, 54 * s);
        ui_text(F_SEMI, 22 * s, "Welcome to Q-Continuum 95", wr.x + 90 * s, wr.y + 44 * s, BLACK);
        ui_text(F_SEMI, 14 * s, "Did you know...", wr.x + 90 * s, wr.y + 86 * s, BLACK);
        ui_text_wrap(F_REG, 14 * s, "You can mesh an STL file in five steps: open the Mesh page, then STL mesher.",
                     (Rectangle){ wr.x + 90 * s, wr.y + 108 * s, wr.width - 110 * s, 60 * s }, 19 * s, BLACK, true);
        Rectangle ok = { wr.x + wr.width - 110 * s, wr.y + wr.height - 44 * s, 92 * s, 30 * s };
        bool down = t > 3.15f && t < 3.32f;
        w95_bevel(ok, down);
        DrawRectangleLinesEx(ui_inset(ok, 4 * s, 4 * s), 1, al(BLACK, 0.6f));
        ctext(F_SEMI, 14 * s, "OK", ok.x + ok.width / 2 + (down ? 1 : 0) * s, ok.y + 6 * s + (down ? 1 : 0) * s, BLACK);
        float u = ease(seg(t, 2.7f, 3.1f));
        arrow(lerp(W * 0.85f, ok.x + ok.width * 0.55f, u), lerp(H * 0.86f, ok.y + ok.height * 0.5f, u), s);
        return;
    }
    arrow(W / 2 + 200 * s, H * 0.42f + 80 * s, s);
    ctext(F_SEMI, 15 * s, "It is now safe to compute.", W / 2, H * 0.42f, al(WHITE, ease(seg(t, 3.4f, 3.6f))));
}

/* ================================================================== Star Citizen: quantum travel, arrival over a planet */
static float sc_V(float t) { float u = seg(t, 0.4f, 1.6f); return 0.08f * t + 1.6f * u * u; }
static void sc_starcitizen(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color cy = hx(0x4FD8FF), amber = hx(0xFFB238);
    fill(hx(0x01040A));
    Vector2 C = { W / 2, H * 0.46f };
    float R = hypotf(W, H) * 0.6f;
    if (t < 1.75f) {
        float V = sc_V(t), dv = (sc_V(t + 0.03f) - V);
        glow(C.x, C.y, R * 0.8f, cy, 0.25f * seg(t, 0.6f, 1.6f));
        for (int i = 0; i < 260; i++) {
            float a = rnd(i) * 2 * PI, p = fmodf(rnd(i + 999) + V, 1.0f), p0 = fmaxf(0, p - dv * 6 - 0.004f);
            float r1 = R * 0.04f / (1.04f - p) , r0 = R * 0.04f / (1.04f - p0);
            Vector2 d = { cosf(a), sinf(a) };
            DrawLineEx((Vector2){ C.x + d.x * r0, C.y + d.y * r0 }, (Vector2){ C.x + d.x * r1, C.y + d.y * r1 }, (0.8f + 1.6f * p) * s,
                       al(mix(WHITE, cy, seg(t, 0.6f, 1.4f)), 0.25f + 0.75f * p));
        }
        const char *st = t < 0.5f ? "SPOOLING" : t < 1.0f ? "CALIBRATING" : "ENGAGED";
        float hy = H - 110 * s;
        ctext(F_SEMI, 15 * s, "QUANTUM DRIVE", W / 2, hy, al(cy, 0.8f));
        ctext(F_SEMI, 24 * s, st, W / 2, hy + 22 * s, t < 1.0f ? amber : cy);
        DrawRectangleLinesEx((Rectangle){ W / 2 - 130 * s, hy + 58 * s, 260 * s, 8 * s }, 1.2f * s, al(cy, 0.7f));
        DrawRectangleRec((Rectangle){ W / 2 - 128 * s, hy + 60 * s, 256 * s * seg(t, 0, 1.0f), 4 * s }, cy);
        corners((Rectangle){ W / 2 - 160 * s, hy - 14 * s, 320 * s, 92 * s }, 14 * s, 1.5f * s, al(cy, 0.6f));
        if (t > 1.55f) fill(al(hx(0xDDF6FF), seg(t, 1.55f, 1.75f)));
        return;
    }
    for (int i = 0; i < 160; i++)
        DrawCircleV((Vector2){ rnd(i + 3) * W, rnd(i + 70) * H }, (i % 7 ? 0.8f : 1.5f) * s, al(WHITE, 0.3f + 0.5f * rnd(i + 5)));
    float pr = W * 1.0f, py = lerp(H + pr * 1.0f, H + pr * 0.74f, eout(seg(t, 1.75f, 3.2f)));
    glow(W / 2, py, pr * 1.12f, cy, 0.45f);
    DrawCircleV((Vector2){ W / 2, py }, pr, hx(0x061626));
    DrawRing((Vector2){ W / 2, py }, pr - 3 * s, pr + 1 * s, 180, 360, 180, al(hx(0x9FE8FF), 0.7f));
    glow(W / 2 - pr * 0.3f, py - pr * 0.85f, pr * 0.5f, hx(0x2A6A8A), 0.25f);
    Vector2 m = { W / 2, H * 0.34f };
    float la = ease(seg(t, 1.95f, 2.4f)), ro = 54 * s;
    DrawEllipseLines((int)m.x, (int)m.y, ro * 1.6f, ro * 0.45f, al(cy, 0.45f * la));
    DrawRing(m, ro - 13 * s, ro, -90, -90 + 360 * eout(seg(t, 1.95f, 2.5f)), 72, al(cy, la));
    float st = eout(seg(t, 2.4f, 2.6f));
    Vector2 a = { m.x + ro * 0.35f, m.y + ro * 0.35f }, b = { m.x + ro * 1.6f, m.y + ro * 1.3f }, e = { lerp(a.x, b.x, st), lerp(a.y, b.y, st) };
    if (st > 0) { DrawLineEx(a, e, 14 * s, al(amber, 0.35f)); DrawLineEx(a, e, 7 * s, amber); }
    float d = lerp(140 * s, 92 * s, eout(seg(t, 1.95f, 2.4f)));
    corners((Rectangle){ m.x - d, m.y - d, 2 * d, 2 * d }, 16 * s, 1.5f * s, al(cy, la * 0.8f));
    ctext(F_SEMI, 46 * s, spaced("Q-CONTINUUM"), W / 2, m.y + 112 * s, al(hx(0xE6F7FF), ease(seg(t, 2.35f, 2.75f))));
    ctext(F_SEMI, 15 * s, "ARRIVAL  -  NAVIER SYSTEM  -  OM-2606", W / 2, m.y + 172 * s, al(amber, ease(seg(t, 2.7f, 3.1f))));
}

/* ================================================================== Half-Life: the tram in, the lambda lights */
static void sc_halflife(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color o = hx(0xFF8C1A);
    fill(hx(0x0E0F0E));
    Vector2 vp = { W / 2, H * 0.45f };
    float tun = 1 - ease(seg(t, 2.3f, 2.8f));
    if (tun > 0) {
        int N = 14;
        float V = t * 0.85f - 0.25f * seg(t, 1.8f, 2.6f) * seg(t, 1.8f, 2.6f);
        for (int k = N - 1; k >= 0; k--) {
            float z = fmodf(k / (float)N - V + 10, 1.0f), sc = 1 / (z * 5 + 0.18f);
            float w = W * 0.22f * sc, h = H * 0.20f * sc, a = tun * (1 - z) * 0.9f;
            Rectangle r = { vp.x - w, vp.y - h, 2 * w, 2 * h };
            DrawRectangleLinesEx(r, fmaxf(1, 3 * s * sc * 0.4f), al(hx(0x4A4C48), a));
            DrawRectangleRec((Rectangle){ vp.x - w * 0.3f, vp.y - h + 2 * s, w * 0.6f, fmaxf(1.5f, 5 * s * sc * 0.35f) }, al(hx(0xFFC27A), a));
            if (k % 3 == 0) DrawRectangleRec((Rectangle){ vp.x - w + 2 * s, vp.y + h * 0.1f, fmaxf(1, 8 * s * sc * 0.3f), h * 0.3f }, al(hx(0xB04020), a * 0.8f));
        }
        DrawLineEx((Vector2){ vp.x - 6 * s, vp.y + 20 * s }, (Vector2){ vp.x - W * 0.25f, H }, 3 * s, al(hx(0x3A3C38), tun));   /* the rail */
        DrawLineEx((Vector2){ vp.x + 6 * s, vp.y + 20 * s }, (Vector2){ vp.x + W * 0.25f, H }, 3 * s, al(hx(0x3A3C38), tun));
    }
    struct tm m = now_tm();
    char tm_[40]; snprintf(tm_, sizeof tm_, "TIME: %02d:%02d %s", m.tm_hour % 12 ? m.tm_hour % 12 : 12, m.tm_min, m.tm_hour < 12 ? "AM" : "PM");
    const char *cap[3] = { "Q-CONTINUUM RESEARCH FACILITY", "SECTOR C  -  COMPUTATIONAL FLUID DYNAMICS", tm_ };
    float c0[3] = { 0.1f, 0.9f, 1.7f };
    for (int i = 0; i < 3; i++) {
        float a = ease(seg(t, c0[i], c0[i] + 0.2f)) * (1 - ease(seg(t, c0[i] + 0.6f, c0[i] + 0.8f)));
        if (a > 0) ui_text(F_SEMI, 22 * s, cap[i], 60 * s, H * 0.76f + i * 0 * s, al(hx(0xF0B060), a));
    }
    Vector2 c = { W / 2, H * 0.40f };
    float ro = 74 * s, rp = eout(seg(t, 2.5f, 3.0f));
    glow(c.x, c.y, ro * 2.4f, o, 0.25f * rp);
    if (rp > 0) DrawRing(c, ro - 12 * s, ro, -90, -90 + 360 * rp, 72, o);
    float la = ease(seg(t, 2.8f, 3.1f));
    if (la > 0) ui_lambda(c.x, c.y, ro * 1.05f * (0.85f + 0.15f * la), 12 * s, al(o, la));
    ctext(F_SEMI, 52 * s, "Q-CONTINUUM", W / 2, c.y + ro + 30 * s, al(hx(0xE8E2D8), ease(seg(t, 3.0f, 3.35f))));
    ctext(F_REG, 16 * s, "Your residuals are being monitored.", W / 2, c.y + ro + 96 * s, al(o, ease(seg(t, 3.3f, 3.6f))));
    float ha = ease(seg(t, 2.9f, 3.3f));
    if (ha > 0) {                                                        /* the HUD */
        float y = H - 60 * s;
        DrawRectangleRec((Rectangle){ 40 * s, y + 8 * s, 26 * s, 8 * s }, al(o, ha));
        DrawRectangleRec((Rectangle){ 49 * s, y - 1 * s, 8 * s, 26 * s }, al(o, ha));
        ui_text(F_SEMI, 34 * s, "100", 78 * s, y - 8 * s, al(o, ha));
        ui_text(F_SEMI, 34 * s, "100", W - 140 * s, y - 8 * s, al(o, ha));
        ui_text(F_MED, 12 * s, "HEALTH", 78 * s, y - 24 * s, al(o, ha * 0.8f));
        ui_text(F_MED, 12 * s, "SUIT", W - 140 * s, y - 24 * s, al(o, ha * 0.8f));
    }
}


/* ================================================================== Rebel Alliance: a long time ago, the title recedes, the crawl */
static void outline_text(UiFamily f, float size, const char *str, float cx, float cy, float k, Color line, Color in)
{
    float o = fmaxf(1.0f, 2.2f * g.s * k) / k;                 /* the stroke, in the text's own (scaled) units */
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        stext(f, size, str, cx + cosf(a) * o * k, cy + sinf(a) * o * k, k, k, line);
    }
    stext(f, size, str, cx, cy, k, k, in);
}
static void sc_rebel(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color blue = hx(0x4BD5EE), yel = hx(0xFFE81F);
    fill(BLACK);
    if (t < 1.6f) {
        float a = ease(seg(t, 0.15f, 0.45f)) * (1 - ease(seg(t, 1.25f, 1.55f)));
        ui_text(F_REG, 26 * s, "A long time ago, in a solver far,", W * 0.22f, H * 0.42f, al(blue, a));
        ui_text(F_REG, 26 * s, "far away....", W * 0.22f, H * 0.42f + 36 * s, al(blue, a));
        return;
    }
    for (int i = 0; i < 220; i++)
        DrawCircleV((Vector2){ rnd(i + 5) * W, rnd(i + 900) * H }, (i % 9 ? 0.8f : 1.5f) * s, al(WHITE, 0.35f + 0.55f * rnd(i + 3)));
    float k = 2.6f * expf(-(t - 1.6f) * 1.65f);               /* the title, sliding away into the stars */
    float ta = 1 - ease(seg(t, 3.0f, 3.5f));
    if (ta > 0) outline_text(F_SEMI, 80 * s, "Q-CONTINUUM", W / 2, H * 0.46f, k, al(yel, ta), BLACK);
    static const char *const L[] = { "Episode 2.0", "A NEW MESH", "", "It is a period of civil turbulence.", "Rebel solvers, striking from",
        "a hidden workspace, have won", "their first victory against", "the evil Divergence.", "", "During the battle, Rebel",
        "engineers managed to mesh", "the Empire's ultimate", "geometry, a part with", "enough cells to destroy", "an entire workstation...." };
    int n = (int)(sizeof L / sizeof L[0]);
    float d0 = (t - 2.25f) * 2.3f, kk = 0.38f, hz = H * 0.10f;
    for (int i = n - 1; i >= 0; i--) {
        float d = d0 - i * 1.0f;
        if (d < 0 || !L[i][0]) continue;
        float sc = 1 / (1 + d * kk), y = hz + (H * 1.02f - hz) * sc;
        float a = clamp01((sc - 0.12f) / 0.2f);
        float fs = i == 1 ? 48 * s : 38 * s;
        if (i == 0) fs = 28 * s;
        stext(F_SEMI, fs, L[i], W / 2, y, sc * 1.25f, sc, al(yel, a));
    }
}

/* ================================================================== Galactic Empire: the destroyer passes overhead, the console wakes */
static Vector2 sd_pt(Vector2 nose, float len, float half, float u, float v)   /* u 0 at the nose .. 1 at the stern, v -1 .. 1 across */
{
    return (Vector2){ nose.x + v * u * half, nose.y - u * len };
}
static void sc_empire(float t)
{
    float s = g.s, W = g.W, H = g.H;
    Color red = hx(0xE8212E), steel = hx(0xA4ADB7), cold = hx(0xA9C6E8);
    fill(hx(0x020305));
    for (int i = 0; i < 200; i++)
        DrawCircleV((Vector2){ rnd(i + 11) * W, rnd(i + 400) * H }, (i % 8 ? 0.8f : 1.4f) * s, al(WHITE, 0.3f + 0.5f * rnd(i + 2)));
    if (t < 2.55f) {
        float pr = H * 1.1f;                                     /* a planet below */
        glow(-W * 0.1f, H + pr * 0.6f, pr * 1.15f, hx(0x3A78C8), 0.35f);
        DrawCircleV((Vector2){ -W * 0.1f, H + pr * 0.6f }, pr, hx(0x081626));
        DrawRing((Vector2){ -W * 0.1f, H + pr * 0.6f }, pr - 2 * s, pr, 270, 360, 120, al(hx(0x8FC4FF), 0.6f));
        float u = eout(seg(t, 0.0f, 2.4f));
        Vector2 nose = { W * 0.56f, lerp(-H * 0.05f, H * 0.80f, u) };
        float len = H * 1.25f, half = W * 0.62f;
        /* the lasers fired ahead of it */
        for (int k = 0; k < 6; k++) {
            float lt = fmodf(t * 1.6f + k * 0.37f, 1.0f);
            Vector2 a0 = sd_pt(nose, len, half, 0.12f + 0.05f * k, (k % 2 ? 0.4f : -0.4f));
            Vector2 a = { a0.x + (W * 0.5f - a0.x) * lt * 0.3f, a0.y + lt * H * 0.6f };
            DrawLineEx(a, (Vector2){ a.x, a.y + 26 * s }, 3 * s, al(hx(0xFF4A3A), 0.9f));
            glow(a.x, a.y + 13 * s, 16 * s, hx(0xFF3A2A), 0.4f);
        }
        Vector2 st_l = sd_pt(nose, len, half, 1, -1), st_r = sd_pt(nose, len, half, 1, 1), st_m = sd_pt(nose, len, half, 1, 0);
        tri(nose, st_l, st_m, hx(0x8F969F));                     /* the hull, lit from the left */
        tri(nose, st_m, st_r, hx(0x5E656E));
        DrawLineEx(nose, st_m, 2 * s, hx(0xC8CED6));             /* the keel */
        for (int i = 0; i < 160; i++) {                          /* greebles: panels and trenches */
            float uu = 0.08f + 0.9f * rnd(i * 3 + 1), vv = (rnd(i * 3 + 2) * 2 - 1) * 0.9f;
            Vector2 p = sd_pt(nose, len, half, uu, vv);
            float gw = (6 + 24 * rnd(i + 70)) * s * (0.4f + uu), gh = (2 + 4 * rnd(i + 90)) * s;
            DrawRectangleRec((Rectangle){ p.x - gw / 2, p.y, gw, gh }, al(vv < 0 ? hx(0x60676F) : hx(0x3E444B), 0.9f));
        }
        for (int k = 1; k <= 4; k++) {                            /* the hull's bands */
            float uu = k * 0.2f;
            DrawLineEx(sd_pt(nose, len, half, uu, -1), sd_pt(nose, len, half, uu, 1), 1.5f * s, al(hx(0x2E3339), 0.9f));
        }
        DrawLineEx(nose, st_l, 1.5f * s, al(hx(0xD8DEE6), 0.7f));
        DrawLineEx(nose, st_r, 1.5f * s, al(hx(0x2A2E33), 0.9f));
        fill(al(BLACK, ease(seg(t, 2.25f, 2.55f))));
        return;
    }
    /* the console */
    float cy = H * 0.40f, ro = 70 * s;
    Vector2 c = { W / 2, cy };
    float sy = lerp(-10, H + 10, fmodf((t - 2.55f) * 0.55f, 1.0f));
    DrawRectangleRec((Rectangle){ 0, sy, W, 1.5f * s }, al(red, 0.35f));
    vgrad(0, sy - 60 * s, W, 60 * s, al(red, 0), al(red, 0.06f));
    float rp = eout(seg(t, 2.55f, 2.95f));
    glow(c.x, c.y, ro * 2.2f, cold, 0.10f * rp);
    if (rp > 0) DrawRing(c, ro - 11 * s, ro, -90, -90 + 360 * rp, 72, steel);
    DrawRing(c, 12 * s, 19 * s, 0, 360 * ease(seg(t, 2.7f, 2.9f)), 32, steel);
    for (int k = 0; k < 6; k++) {
        float a = ease(seg(t, 2.85f + k * 0.05f, 2.95f + k * 0.05f));
        if (a <= 0) continue;
        float an = (k * 60.0f + 30.0f) * DEG2RAD;
        DrawLineEx((Vector2){ c.x + cosf(an) * 19 * s, c.y + sinf(an) * 19 * s },
                   (Vector2){ c.x + cosf(an) * lerp(19 * s, ro - 10 * s, a), c.y + sinf(an) * lerp(19 * s, ro - 10 * s, a) }, 7 * s, steel);
    }
    float tl = eout(seg(t, 3.15f, 3.35f));
    if (tl > 0) {
        Vector2 a = { c.x + ro * 0.72f, c.y + ro * 0.72f }, b = { c.x + ro * 1.45f, c.y + ro * 1.40f };
        DrawLineEx(a, (Vector2){ lerp(a.x, b.x, tl), lerp(a.y, b.y, tl) }, 15 * s, red);
    }
    corners((Rectangle){ c.x - ro * 1.7f, c.y - ro * 1.5f, ro * 3.4f, ro * 3.1f }, 18 * s, 2 * s, al(steel, 0.6f * ease(seg(t, 2.6f, 2.9f))));
    const char *word = spaced("Q-CONTINUUM");
    char b[64]; int nch = (int)typed_n(word, t, 3.0f, 60);
    snprintf(b, sizeof b, "%.*s", nch, word);
    float fs = 40 * s, ww = tw(F_SEMI, fs, word);
    ui_text(F_SEMI, fs, b, W / 2 - ww / 2, c.y + ro * 1.75f, hx(0xE6EAEE));
    float la = ease(seg(t, 3.3f, 3.6f));
    DrawRectangleRec((Rectangle){ W / 2 - ww / 2, c.y + ro * 1.75f + fs * 1.35f, ww * la, 2 * s }, red);
    ctext(F_SEMI, 15 * s, "IMPERIAL COMPUTATIONAL DIVISION", W / 2, c.y + ro * 1.75f + fs * 1.6f, al(red, la));
    ctext(F_REG, 15 * s, "Fully armed and operational.", W / 2, c.y + ro * 1.75f + fs * 1.6f + 28 * s, al(hx(0x8A939D), ease(seg(t, 3.6f, 3.9f))));
}

/* ------------------------------------------------------------------ the engine */
static const struct { float len, reveal; } TIMING[THEME__N] = {
    [THEME_LCARS] = { 4.0f, 1.75f }, [THEME_DARK] = { 4.1f, -1 }, [THEME_LIGHT] = { 4.2f, -1 }, [THEME_STARDEW] = { 4.3f, 2.25f },
    [THEME_POKEMON] = { 4.3f, 1.7f }, [THEME_JARVIS] = { 4.0f, 2.15f }, [THEME_SEVERANCE] = { 4.2f, 2.5f }, [THEME_ALIEN] = { 4.4f, 2.9f },
    [THEME_PIPBOY] = { 4.1f, 2.55f }, [THEME_NERV] = { 4.4f, 2.5f }, [THEME_PORTAL] = { 4.2f, 2.15f }, [THEME_SOULS] = { 4.4f, 2.35f },
    [THEME_FACTORIO] = { 4.2f, 2.3f }, [THEME_WIN95] = { 4.2f, 2.62f }, [THEME_STARCITIZEN] = { 4.2f, 1.75f }, [THEME_HALFLIFE] = { 4.2f, 3.0f },
    [THEME_REBEL] = { 5.6f, 1.6f }, [THEME_EMPIRE] = { 4.5f, 2.6f },
};
double intro_length(int theme) { return theme >= 0 && theme < THEME__N ? TIMING[theme].len : 4.0; }

void intro_start(void)
{
    I.active = true; I.cued = false; I.off = 0; I.frames = 0; I.t = 0; I.acc = 0;
    I.t0 = I.last = GetTime();
    I.theme = ui_theme_id();
    ui_request_frames(2);
}
void intro_freeze(double t) { intro_start(); I.frozen = t; I.t = t; }
bool intro_active(void) { return I.active; }
bool intro_hides_app(void) { return I.active && I.t < intro_length(I.theme); }

void intro_update(void)
{
    if (!I.active) return;
    I.theme = ui_theme_id();
    double D = intro_length(I.theme);
    if (I.frozen >= 0) { I.t = I.frozen; ui_request_frames(2); return; }
    /* the clock advances at most 1/20 s a frame: the first frames load the scene's large fonts, and the animation should
       start where it starts rather than jump past its opening */
    double now = GetTime();
    I.acc += fmin(now - I.last, 0.05); I.last = now;
    double raw = g_ui_no_input ? I.frames++ / 60.0 : I.acc;
    I.t = raw + I.off;
    if (I.t < D && (inp_any_key() || inp_button_pressed())) { I.off += D - I.t; I.t = D; I.cued = true; }   /* skipped: straight to the fade */
    float rv = TIMING[I.theme].reveal;
    if (!I.cued && rv >= 0 && I.t >= rv) { I.cued = true; app_sound(SND_DONE); }
    if (I.t >= D + FADE) I.active = false;
    ui_request_frames(2);
}

void intro_draw(float W, float H)
{
    if (!I.active) return;
    g.W = W; g.H = H;
    g.s = fmaxf(0.55f, fminf(1.6f, fminf(W / 1200.0f, H / 750.0f)));
    float t = (float)I.t, D = (float)intro_length(I.theme);
    Color veil = TH.bg;
    if (t < D) {
        switch (I.theme) {
        case THEME_LCARS: sc_lcars(t); break;
        case THEME_LIGHT: sc_light(t); break;
        case THEME_STARDEW: sc_stardew(t); break;
        case THEME_POKEMON: sc_pokemon(t); break;
        case THEME_JARVIS: sc_jarvis(t); break;
        case THEME_SEVERANCE: sc_severance(t); break;
        case THEME_ALIEN: sc_alien(t); break;
        case THEME_PIPBOY: sc_pipboy(t); break;
        case THEME_NERV: sc_nerv(t); break;
        case THEME_PORTAL: sc_portal(t); break;
        case THEME_SOULS: sc_souls(t); break;
        case THEME_FACTORIO: sc_factorio(t); break;
        case THEME_WIN95: sc_win95(t); break;
        case THEME_STARCITIZEN: sc_starcitizen(t); break;
        case THEME_HALFLIFE: sc_halflife(t); break;
        case THEME_REBEL: sc_rebel(t); break;
        case THEME_EMPIRE: sc_empire(t); break;
        default: sc_dark(t); break;
        }
        fill(al(veil, ease(seg(t, D - 0.35f, D))));
        if (I.frozen < 0 && t < 1.2f)
            ui_text(F_REG, 11, "Any key to skip", W - ui_text_w(F_REG, 11, "Any key to skip") - 14, H - 22, al(ui_mix(veil, TH.text3, 0.5f), 0.7f * seg(t, 0.4f, 0.8f)));
    } else fill(al(veil, 1 - ease(seg(t, D, D + (float)FADE))));
    ui_register(ui_id("intro-screen"), (Rectangle){ 0, 0, W, H }, "Start-up animation");
}
