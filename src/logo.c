/* logo.c -- the Q mark, one for each theme (user request, 2026-10-21), and the window and desktop icon made from it.
 *
 * Every mark is a Q on a tile, drawn in the theme's own way: LCARS elbows, a glass tile, a wooden sign, a Poké Ball, the arc
 * reactor, Lumon's globe, a phosphor Q, a vault-door cog, NERV's hexagon, a portal, a coiled sword, a gear on a belt, a
 * Windows 95 button, a quantum jump and the lambda. The colours are fixed per theme (not TH), so Settings can show them side by side.
 *
 * The icon: when the theme changes, the mark is rendered at 512 px, scaled to 256 and set as the window icon (the taskbar
 * under WSLg). Outside test runs it also replaces the desktop icon: ~/.local/share/icons (Linux desktops), and on WSL the
 * .ico WSLg made for the Start menu shortcut, written in a child process so the window never waits on /mnt/c.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "rlgl.h"

static Color hexc(unsigned v) { return (Color){ (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, 255 }; }
static Color alpha(Color c, float a) { c.a = (unsigned char)(c.a * (a < 0 ? 0 : a > 1 ? 1 : a)); return c; }
static void rrect(Rectangle r, float radius, Color c)
{
    float m = fminf(r.width, r.height);
    if (radius <= 0 || m <= 0) { DrawRectangleRec(r, c); return; }
    DrawRectangleRounded(r, fminf(1, 2 * radius / m), 24, c);
}
static void rrect_lines(Rectangle r, float radius, float th, Color c)
{
    float m = fminf(r.width, r.height);
    DrawRectangleRoundedLinesEx(r, fminf(1, 2 * radius / m), 24, th, c);
}
/* the Q's tail: a cubic from inside the bowl out to the lower right */
static void tail(Vector2 c, float ro, float x, float y, float s, float th, Color col)
{
    Vector2 p0 = { c.x + ro * 0.30f, c.y + ro * 0.38f }, p3 = { x + s * 0.90f, y + s * 0.86f };
    DrawSplineSegmentBezierCubic(p0, (Vector2){ c.x + ro * 0.75f, c.y + ro * 0.70f }, (Vector2){ x + s * 0.70f, y + s * 0.92f }, p3, th, col);
}
/* a bold pixel Q, 10 x 10 cells */
static const char *const PIXQ[10] = {
    "..######..", ".########.", "##......##", "##......##", "##......##",
    "##...##.##", "##....####", ".########.", "..#######.", ".......###" };
static void pixel_q(float x, float y, float cell, Color c)
{
    for (int r = 0; r < 10; r++)
        for (int k = 0; k < 10; k++)
            if (PIXQ[r][k] == '#') DrawRectangleRec((Rectangle){ x + k * cell, y + r * cell, ceilf(cell), ceilf(cell) }, c);
}
static void cog(Vector2 c, float ro, float ri, int teeth, float tooth, Color col)
{
    DrawRing(c, ri, ro, 0, 360, 48, col);
    for (int k = 0; k < teeth; k++) {
        float a = k * 360.0f / teeth;
        DrawRing(c, ro - 1, ro + tooth, a - 90.0f / teeth, a + 90.0f / teeth, 4, col);
    }
}

void logo_draw(int theme, float x, float y, float s)
{
    Rectangle r = { x, y, s, s };
    Vector2 c = { x + s * 0.47f, y + s * 0.46f };
    switch (theme) {
    case THEME_LCARS: {                                  /* black tile, the orange elbow and lilac block, a pink Q */
        float u = s / 256.0f;
        rrect(r, 48 * u, BLACK);
        Color o = hexc(0xFF9900), l = hexc(0x9999FF), p = hexc(0xCC99CC);
        rrect((Rectangle){ x + 30 * u, y + 30 * u, 196 * u, 32 * u }, 16 * u, o);
        rrect((Rectangle){ x + 30 * u, y + 30 * u, 66 * u, 120 * u }, 30 * u, o);
        DrawRectangleRec((Rectangle){ x + 30 * u, y + 46 * u, 66 * u, 104 * u }, o);
        DrawRectangleRec((Rectangle){ x + 30 * u, y + 156 * u, 66 * u, 70 * u }, l);
        Vector2 q = { x + 166 * u, y + 148 * u };
        DrawRing(q, 36 * u, 54 * u, 0, 360, 48, p);
        DrawLineEx((Vector2){ x + 177 * u, y + 163 * u }, (Vector2){ x + 211 * u, y + 197 * u }, 20 * u, p);
        break;
    }
    case THEME_DARK: case THEME_LIGHT: {                 /* glass: a blue-violet tile, a sheen, two flow lines, a white Q */
        /* dark: a blue-violet tile and a white Q; light: frosted white glass and a blue-violet Q */
        bool dark = theme == THEME_DARK;
        Color c0 = { 10, 132, 255, 255 }, c1 = { 94, 92, 230, 255 };
        Color ink = dark ? WHITE : (Color){ 40, 100, 245, 255 }, flow = dark ? alpha(WHITE, 0.28f) : alpha(c1, 0.30f);
        if (dark) {
            rrect(r, s * 0.22f, c1);
            rrect((Rectangle){ x, y, s, s * 0.72f }, s * 0.22f, alpha(c0, 0.85f));
            rrect((Rectangle){ x + s * 0.03f, y + s * 0.03f, s * 0.94f, s * 0.46f }, s * 0.2f, alpha(WHITE, 0.16f));
        } else {
            rrect(r, s * 0.22f, (Color){ 222, 230, 248, 255 });
            rrect((Rectangle){ x, y, s, s * 0.72f }, s * 0.22f, (Color){ 246, 249, 255, 255 });
            rrect_lines(r, s * 0.22f, fmaxf(1, s * 0.02f), alpha(c1, 0.35f));
        }
        for (int k = 0; k < 2; k++) {
            float yy = y + s * (0.30f + 0.42f * k);
            Vector2 prev = { x + s * 0.06f, yy };
            for (int i = 1; i <= 16; i++) {
                float t = i / 16.0f;
                Vector2 p = { x + s * (0.06f + 0.88f * t), yy + sinf(t * 6.2832f + k * 1.6f) * s * 0.035f };
                DrawLineEx(prev, p, s * 0.035f, flow);
                prev = p;
            }
        }
        float ro = s * 0.27f, th = s * 0.085f;
        DrawRing(c, ro - th, ro, 0, 360, 48, ink);
        tail(c, ro, x, y, s, th, ink);
        break;
    }
    case THEME_STARDEW: {                                /* a wooden sign: planks, a dark frame, a cream pixel Q, a sprout */
        Color wood = hexc(0xC88A4E), dark = hexc(0x6B3A16), plank = hexc(0xB0753E);
        rrect(r, s * 0.10f, dark);
        Rectangle in = { x + s * 0.07f, y + s * 0.07f, s * 0.86f, s * 0.86f };
        rrect(in, s * 0.05f, wood);
        for (int k = 1; k < 4; k++) DrawRectangleRec((Rectangle){ in.x, in.y + in.height * k / 4.0f, in.width, fmaxf(1, s * 0.012f) }, plank);
        DrawRectangleRec((Rectangle){ in.x, in.y, in.width, fmaxf(1, s * 0.02f) }, hexc(0xE8B070));
        float cell = s * 0.064f;
        pixel_q(x + s * 0.18f + cell * 0.4f, y + s * 0.20f + cell * 0.4f, cell, alpha(dark, 0.5f));
        pixel_q(x + s * 0.18f, y + s * 0.20f, cell, hexc(0xFFF3D6));
        Color g = hexc(0x3E8E2E);                        /* a sprout from the tail */
        DrawLineEx((Vector2){ x + s * 0.80f, y + s * 0.82f }, (Vector2){ x + s * 0.84f, y + s * 0.70f }, s * 0.03f, g);
        DrawCircleV((Vector2){ x + s * 0.79f, y + s * 0.69f }, s * 0.05f, g);
        DrawCircleV((Vector2){ x + s * 0.89f, y + s * 0.67f }, s * 0.05f, hexc(0x4FA53A));
        break;
    }
    case THEME_POKEMON: {                                /* a Poké Ball Q on the cursor's yellow */
        rrect(r, s * 0.2f, hexc(0xFFCB05));
        rrect_lines((Rectangle){ x + s * 0.04f, y + s * 0.04f, s * 0.92f, s * 0.92f }, s * 0.17f, fmaxf(1, s * 0.03f), hexc(0x2A5DB0));
        float R = s * 0.30f;
        Color red = hexc(0xE3350D), ink = hexc(0x1A1A1A);
        DrawCircleSector(c, R, 180, 360, 36, red);
        DrawCircleSector(c, R, 0, 180, 36, WHITE);
        DrawRectangleRec((Rectangle){ c.x - R, c.y - s * 0.03f, 2 * R, s * 0.06f }, ink);
        DrawRing(c, R, R + s * 0.04f, 0, 360, 48, ink);
        DrawCircleV(c, s * 0.09f, ink);
        DrawCircleV(c, s * 0.055f, WHITE);
        tail(c, R * 1.1f, x, y, s, s * 0.09f, ink);
        break;
    }
    case THEME_JARVIS: {                                 /* the arc reactor, its tail a streamline */
        Color cy = hexc(0x00D8FF), clear = cy; clear.a = 0;
        rrect(r, s * 0.12f, hexc(0x02070D));
        rrect_lines(r, s * 0.12f, fmaxf(1, s * 0.02f), alpha(cy, 0.5f));
        float ro = s * 0.33f;
        DrawCircleGradient((int)c.x, (int)c.y, s * 0.5f, alpha(cy, 0.30f), clear);
        DrawRing(c, ro - s * 0.06f, ro, 0, 360, 48, cy);
        for (int k = 0; k < 10; k++) DrawRing(c, ro * 0.45f, ro * 0.74f, k * 36.0f + 5, k * 36.0f + 31, 8, alpha(cy, 0.75f));
        DrawCircleV(c, ro * 0.30f, (Color){ 153, 239, 255, 255 });
        Vector2 p0 = { c.x + ro * 0.62f, c.y + ro * 0.62f }, p3 = { x + s * 0.92f, y + s * 0.90f };
        DrawSplineSegmentBezierCubic(p0, (Vector2){ c.x + ro * 0.95f, c.y + ro * 0.95f }, (Vector2){ x + s * 0.80f, y + s * 0.94f }, p3, s * 0.07f, cy);
        break;
    }
    case THEME_SEVERANCE: {                              /* Lumon's globe as the bowl: a ring banded with latitudes */
        Color cy = hexc(0x6FE3FF);
        rrect(r, s * 0.14f, hexc(0x062230));
        DrawCircleGradient((int)c.x, (int)c.y, s * 0.48f, alpha(cy, 0.18f), alpha(cy, 0));
        float ro = s * 0.29f, th = s * 0.06f;
        DrawRing(c, ro - th, ro, 0, 360, 48, cy);
        for (int k = -2; k <= 2; k++) {                  /* latitudes, chords of the inner circle */
            float yy = c.y + k * (ro - th) * 0.36f, hw = sqrtf(fmaxf(0, (ro - th) * (ro - th) - (yy - c.y) * (yy - c.y)));
            DrawLineEx((Vector2){ c.x - hw, yy }, (Vector2){ c.x + hw, yy }, fmaxf(1, s * 0.022f), alpha(cy, 0.75f));
        }
        tail(c, ro, x, y, s, th, cy);
        break;
    }
    case THEME_ALIEN: {                                  /* MU/TH/UR: a phosphor pixel Q on black, glowing, with scanlines */
        Color g = hexc(0x9DFFB4);
        rrect(r, s * 0.06f, hexc(0x000500));
        float cell = s * 0.064f, qx = x + s * 0.18f, qy = y + s * 0.18f;
        DrawCircleGradient((int)c.x, (int)c.y, s * 0.5f, alpha(g, 0.20f), alpha(g, 0));
        pixel_q(qx, qy, cell, g);
        for (float yy = y + s * 0.02f; yy < y + s * 0.98f; yy += fmaxf(2, s * 0.03f))
            DrawRectangleRec((Rectangle){ x + s * 0.02f, yy, s * 0.96f, fmaxf(1, s * 0.01f) }, alpha(BLACK, 0.45f));
        break;
    }
    case THEME_PIPBOY: {                                 /* a vault-door cog for the bowl, in Vault-Tec green */
        Color g = hexc(0x3CFF7A);
        rrect(r, s * 0.08f, hexc(0x041306));
        DrawCircleGradient((int)c.x, (int)c.y, s * 0.5f, alpha(g, 0.14f), alpha(g, 0));
        float ro = s * 0.27f;
        cog(c, ro, ro - s * 0.08f, 12, s * 0.05f, g);
        DrawCircleV(c, s * 0.07f, alpha(g, 0.6f));
        DrawLineEx((Vector2){ c.x + ro * 0.4f, c.y + ro * 0.4f }, (Vector2){ x + s * 0.90f, y + s * 0.88f }, s * 0.09f, g);
        break;
    }
    case THEME_NERV: {                                   /* black, a red hexagon for the bowl, a half leaf for the tail */
        Color red = hexc(0xFF2A2A), orange = hexc(0xFF6A00);
        DrawRectangleRec(r, hexc(0x050302));
        float k = s * 0.12f;
        DrawTriangle((Vector2){ x, y }, (Vector2){ x, y + k }, (Vector2){ x + k, y }, orange);   /* a chamfered corner, as the panels have */
        DrawPolyLinesEx(c, 6, s * 0.30f, 30, s * 0.075f, red);
        Vector2 a = { c.x + s * 0.10f, c.y + s * 0.12f }, b = { x + s * 0.92f, y + s * 0.90f };
        DrawTriangle(a, (Vector2){ x + s * 0.70f, y + s * 0.92f }, b, red);
        DrawTriangle(a, b, (Vector2){ x + s * 0.88f, y + s * 0.66f }, alpha(red, 0.7f));
        break;
    }
    case THEME_PORTAL: {                                 /* a test chamber's white tile: a blue portal for the bowl, orange tail */
        Color blue = hexc(0x1A9BFF), orange = hexc(0xFF8A00);
        rrect(r, s * 0.16f, hexc(0xF2F4F5));
        rrect_lines(r, s * 0.16f, fmaxf(1, s * 0.02f), hexc(0xB9C3C9));
        DrawEllipse((int)c.x, (int)c.y, s * 0.24f, s * 0.32f, alpha(blue, 0.25f));
        DrawEllipse((int)c.x, (int)c.y, s * 0.22f, s * 0.30f, blue);
        DrawEllipse((int)c.x, (int)c.y, s * 0.14f, s * 0.22f, hexc(0xF2F4F5));
        tail((Vector2){ c.x, c.y + s * 0.04f }, s * 0.26f, x, y, s, s * 0.08f, orange);
        break;
    }
    case THEME_SOULS: {                                  /* an iron ring over embers, a coiled sword through it as the tail */
        Color ember = hexc(0xE0A040), iron = hexc(0x8A8478);
        rrect(r, s * 0.06f, hexc(0x0B0907));
        DrawCircleGradient((int)c.x, (int)(y + s * 0.95f), s * 0.6f, alpha(hexc(0xFF7A20), 0.35f), alpha(ember, 0));
        float ro = s * 0.28f;
        DrawRing(c, ro - s * 0.07f, ro, 0, 360, 48, iron);
        DrawRing(c, ro - s * 0.07f, ro - s * 0.05f, 0, 360, 48, alpha(ember, 0.6f));
        Vector2 hilt = { c.x - s * 0.02f, c.y - s * 0.02f }, tip = { x + s * 0.90f, y + s * 0.90f };
        DrawLineEx(hilt, tip, s * 0.055f, hexc(0xD8D2C4));       /* the blade */
        DrawLineEx((Vector2){ hilt.x - s * 0.07f, hilt.y + s * 0.07f }, (Vector2){ hilt.x + s * 0.07f, hilt.y - s * 0.07f }, s * 0.04f, ember);
        DrawCircleV(tip, s * 0.03f, ember);
        break;
    }
    case THEME_FACTORIO: {                               /* an orange gear on a dark plate, the tail a yellow belt */
        Color o = hexc(0xFF9F1C), belt = hexc(0xFFD23A);
        rrect(r, s * 0.05f, hexc(0x2E2D2B));
        DrawRectangleLinesEx(r, fmaxf(1, s * 0.03f), hexc(0x141312));
        DrawRectangleRec((Rectangle){ x + s * 0.03f, y + s * 0.03f, s * 0.94f, fmaxf(1, s * 0.012f) }, alpha(WHITE, 0.12f));
        float ro = s * 0.25f;
        cog(c, ro, ro - s * 0.08f, 9, s * 0.06f, o);
        Vector2 a = { c.x + ro * 0.5f, c.y + ro * 0.5f }, b = { x + s * 0.92f, y + s * 0.92f };
        DrawLineEx(a, b, s * 0.12f, hexc(0x3C3A36));
        for (int k = 0; k < 3; k++) {                    /* chevrons along the belt */
            float t = 0.25f + k * 0.27f;
            Vector2 p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
            float d = s * 0.035f;
            DrawLineEx((Vector2){ p.x - d, p.y + d * 0.2f }, p, s * 0.025f, belt);
            DrawLineEx((Vector2){ p.x + d * 0.2f, p.y - d }, p, s * 0.025f, belt);
        }
        break;
    }
    case THEME_WIN95: {                                  /* a raised grey button with a title bar, a navy pixel Q */
        DrawRectangleRec(r, hexc(0xC0C0C0));
        float b = fmaxf(1, s * 0.025f);
        DrawRectangleRec((Rectangle){ x, y, s, b }, WHITE); DrawRectangleRec((Rectangle){ x, y, b, s }, WHITE);
        DrawRectangleRec((Rectangle){ x, y + s - b, s, b }, BLACK); DrawRectangleRec((Rectangle){ x + s - b, y, b, s }, BLACK);
        DrawRectangleRec((Rectangle){ x + b, y + s - 2 * b, s - 2 * b, b }, hexc(0x808080));
        DrawRectangleRec((Rectangle){ x + s - 2 * b, y + b, b, s - 2 * b }, hexc(0x808080));
        DrawRectangleGradientH((int)(x + 2 * b), (int)(y + 2 * b), (int)(s - 4 * b), (int)(s * 0.12f), hexc(0x000080), hexc(0x1084D0));
        pixel_q(x + s * 0.22f, y + s * 0.25f, s * 0.058f, hexc(0x000080));
        break;
    }
    case THEME_STARCITIZEN: {                            /* deep space: a cyan ring on its orbit, the tail a quantum streak */
        Color cy = hexc(0x4FD8FF), amber = hexc(0xFFB238);
        rrect(r, s * 0.10f, hexc(0x03070D));
        DrawCircleGradient((int)(x + s * 0.05f), (int)(y + s * 1.05f), s * 0.7f, alpha(cy, 0.22f), alpha(cy, 0));
        unsigned h = 977u;
        for (int i = 0; i < 18; i++) {
            h = h * 1103515245u + 12345u; float sx = x + s * (0.06f + ((h >> 8) % 1000) / 1000.0f * 0.88f);
            h = h * 1103515245u + 12345u; float sy = y + s * (0.06f + ((h >> 8) % 1000) / 1000.0f * 0.88f);
            DrawCircleV((Vector2){ sx, sy }, fmaxf(0.5f, s * (i % 5 ? 0.006f : 0.012f)), alpha(WHITE, 0.7f));
        }
        float ro = s * 0.27f;
        DrawEllipseLines((int)c.x, (int)c.y, s * 0.42f, s * 0.12f, alpha(cy, 0.45f));   /* the orbit */
        DrawRing(c, ro - s * 0.07f, ro, 0, 360, 48, cy);
        Vector2 a = { c.x + ro * 0.35f, c.y + ro * 0.35f }, b = { x + s * 0.92f, y + s * 0.88f };
        DrawLineEx(a, b, s * 0.08f, alpha(amber, 0.35f));
        DrawLineEx(a, b, s * 0.04f, amber);
        DrawCircleV(b, s * 0.035f, (Color){ 255, 240, 210, 255 });
        break;
    }
    case THEME_HALFLIFE: {                               /* gunmetal, the lambda inside an orange ring for the bowl, a steel tail */
        Color o = hexc(0xFF8C1A);
        rrect(r, s * 0.08f, hexc(0x232423));
        DrawRectangleGradientV((int)x, (int)y, (int)s, (int)(s * 0.5f), alpha(WHITE, 0.10f), alpha(WHITE, 0));
        rrect_lines(r, s * 0.08f, fmaxf(1, s * 0.02f), hexc(0x5E5C57));
        float ro = s * 0.30f;
        DrawRing(c, ro - s * 0.06f, ro, 0, 360, 48, o);
        ui_lambda(c.x, c.y, s * 0.34f, fmaxf(1, s * 0.055f), o);
        Vector2 a = { c.x + ro * 0.62f, c.y + ro * 0.62f }, b = { x + s * 0.92f, y + s * 0.92f };
        DrawLineEx(a, b, s * 0.07f, o);
        break;
    }
    default: logo_draw(THEME_DARK, x, y, s); break;
    }
}

/* ============================================================================ the icon */
/* the mark as a 256-px image: rendered at twice that and scaled down (render textures have no MSAA) */
static Image logo_image(int theme)
{
    const int N = 512;
    RenderTexture2D rt = LoadRenderTexture(N, N);
    rlDrawRenderBatchActive();
    BeginTextureMode(rt);
    ClearBackground(BLANK);
    rlDisableBackfaceCulling();
    logo_draw(theme, 0, 0, (float)N);
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    EndTextureMode();
    Image im = LoadImageFromTexture(rt.texture);
    UnloadRenderTexture(rt);
    ImageFlipVertical(&im);
    ImageResize(&im, 256, 256);
    return im;
}

static void put_le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_le32(unsigned char *p, unsigned v) { put_le16(p, v & 0xFFFF); put_le16(p + 2, v >> 16); }

/* an .ico of PNG entries (Vista and later read them): 256, 48, 32 and 16 px */
static unsigned char *make_ico(Image big, int *len)
{
    static const int SZ[4] = { 256, 48, 32, 16 };
    unsigned char *png[4]; int n[4], total = 6 + 16 * 4;
    for (int k = 0; k < 4; k++) {
        Image im = ImageCopy(big);
        if (SZ[k] != 256) ImageResize(&im, SZ[k], SZ[k]);
        png[k] = ExportImageToMemory(im, ".png", &n[k]);
        UnloadImage(im);
        total += n[k];
    }
    unsigned char *o = malloc((size_t)total);
    put_le16(o, 0); put_le16(o + 2, 1); put_le16(o + 4, 4);
    int off = 6 + 16 * 4;
    for (int k = 0; k < 4; k++) {
        unsigned char *e = o + 6 + 16 * k;
        e[0] = (unsigned char)(SZ[k] == 256 ? 0 : SZ[k]); e[1] = e[0]; e[2] = 0; e[3] = 0;
        put_le16(e + 4, 1); put_le16(e + 6, 32); put_le32(e + 8, (unsigned)n[k]); put_le32(e + 12, (unsigned)off);
        memcpy(o + off, png[k], (size_t)n[k]); off += n[k];
        MemFree(png[k]);
    }
    *len = total;
    return o;
}

static char *b64(const unsigned char *d, int n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc((size_t)(n + 2) / 3 * 4 + 1), *p = o;
    for (int i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < n ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        *p++ = T[v >> 18 & 63]; *p++ = T[v >> 12 & 63];
        *p++ = i + 1 < n ? T[v >> 6 & 63] : '='; *p++ = i + 2 < n ? T[v & 63] : '=';
    }
    *p = 0;
    return o;
}

/* the desktop's copies: a user icon overriding the installed one, and on WSL the .ico behind the Start menu shortcut */
static void write_desktop_icons(const unsigned char *png, int npng, const unsigned char *ico, int nico)
{
    const char *xd = getenv("XDG_DATA_HOME");
    char base[PL_PATH], p[PL_PATH + 64];
    if (xd && *xd) pl_strlcpy(base, xd, sizeof base); else snprintf(base, sizeof base, "%s/.local/share", pl_home());
    snprintf(p, sizeof p, "%s/icons/hicolor/256x256/apps", base); pl_mkdirs(p);
    snprintf(p, sizeof p, "%s/icons/hicolor/256x256/apps/q-continuum.png", base); pl_write_file(p, (const char *)png, (size_t)npng);
    /* the installed icon is an SVG, which outranks any PNG size: the user one is an SVG too, the PNG inside it */
    char *b = b64(png, npng);
    size_t ns = strlen(b) + 256;
    char *svg = malloc(ns);
    int k = snprintf(svg, ns, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 256 256\"><image width=\"256\" height=\"256\" "
                              "href=\"data:image/png;base64,%s\"/></svg>\n", b);
    snprintf(p, sizeof p, "%s/icons/hicolor/scalable/apps", base); pl_mkdirs(p);
    snprintf(p, sizeof p, "%s/icons/hicolor/scalable/apps/q-continuum.svg", base); pl_write_file(p, svg, (size_t)k);
    free(svg); free(b);
    const char *distro = getenv("WSL_DISTRO_NAME");
    if (!distro || !*distro || getenv("QC_TEST_NO_WINDOWS")) return;
    char pat[PL_PATH]; snprintf(pat, sizeof pat, "/mnt/c/Users/*/AppData/Local/Temp/WSLDVCPlugin/%s/q-continuum.ico", distro);
    glob_t g; int changed = 0;
    if (glob(pat, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) if (access(g.gl_pathv[i], W_OK) == 0 && pl_write_file(g.gl_pathv[i], (const char *)ico, (size_t)nico)) changed++;
        globfree(&g);
    }
    if (changed && access("/mnt/c/Windows/System32/ie4uinit.exe", X_OK) == 0)   /* Explorer caches icons: refresh it */
        execl("/mnt/c/Windows/System32/ie4uinit.exe", "ie4uinit.exe", "-show", (char *)NULL);   /* the last thing this child does */
}

void logo_set_icon(int theme, bool desktop)
{
    Image im = logo_image(theme);
    Image w = ImageCopy(im); ImageResize(&w, 64, 64);
    SetWindowIcon(w); UnloadImage(w);
    /* only when it changed since the last time it was written (a marker in the cache), and never in test runs */
    char dir[PL_PATH], mark[PL_PATH + 16], prev[16] = "", now[16];
    const char *xc = getenv("XDG_CACHE_HOME");
    if (xc && *xc) snprintf(dir, sizeof dir, "%s/q-continuum", xc); else snprintf(dir, sizeof dir, "%s/.cache/q-continuum", pl_home());
    if (!g_settings_readonly) pl_mkdirs(dir);
    snprintf(mark, sizeof mark, "%s/icon-theme", dir);
    FILE *f = fopen(mark, "r"); if (f) { if (!fgets(prev, sizeof prev, f)) prev[0] = 0; fclose(f); }
    snprintf(now, sizeof now, "%d\n", theme);
    if (!desktop || g_settings_readonly || !strcmp(prev, now)) { UnloadImage(im); return; }
    int npng = 0, nico = 0;
    unsigned char *png = ExportImageToMemory(im, ".png", &npng);
    unsigned char *ico = make_ico(im, &nico);
    UnloadImage(im);
    pl_write_file(mark, now, strlen(now));
    pid_t pid = fork();                                  /* /mnt/c can take a while: a grandchild does it, nobody waits */
    if (pid == 0) {
        if (fork() == 0) {
            int nul = open("/dev/null", O_WRONLY);
            if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
            write_desktop_icons(png, npng, ico, nico);
            _exit(0);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
    MemFree(png); free(ico);
}

int logo_export(const char *dir)
{
    pl_mkdirs(dir);
    int n = 0;
    Image sheet = GenImageColor(5 * 272 + 16, 3 * 272 + 16, (Color){ 128, 128, 128, 255 });   /* all of them, to compare */
    for (int t = 0; t < THEME__N; t++) {
        char p[PL_PATH + 32]; snprintf(p, sizeof p, "%s/logo-%02d.png", dir, t);
        Image im = logo_image(t);
        if (ExportImage(im, p)) n++;
        ImageDraw(&sheet, im, (Rectangle){ 0, 0, 256, 256 }, (Rectangle){ 16 + (t % 5) * 272.0f, 16 + floorf(t / 5.0f) * 272.0f, 256, 256 }, WHITE);
        UnloadImage(im);
    }
    char p[PL_PATH + 32]; snprintf(p, sizeof p, "%s/sheet.png", dir);
    ExportImage(sheet, p);
    UnloadImage(sheet);
    return n;
}
