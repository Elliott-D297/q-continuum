/* flavour.c -- what the screen and game themes add on top of the colours and type (user request, 2026-10-20):
 *
 *   Severance   a case is a "file" with a codename and a completion percentage; converged equations go into the four bins
 *               (WO FC DR MA); a finished parameter sweep (the queue emptied after several runs) earns a waffle party
 *   Alien       MU/TH/UR: the status reads like the terminal, a diverged or failed run is SPECIAL ORDER 937, CASE EXPENDABLE
 *   Pip-Boy     STAT / INV / DATA tabs over the case pages, logs are holotapes, a Geiger counter on the continuity error
 *   NERV        runs are Unit-01, -02...; convergence as a sync ratio; PATTERN BLUE when the Courant number passes 1
 *   Portal      a run is a test chamber with a subject number, the facility AI remarks on the mesh, a finished run earns cake
 *   Dark Souls  every write is a bonfire, HP from the residuals and stamina from the time-step budget, YOU DIED on a failure
 *               with a respawn from the last bonfire (latestTime)
 *   Factorio    the run is an assembler, written times ride a belt to post-processing, throughput in simulated seconds per
 *               wall-clock hour; errors are biters
 *   Windows 95  a chunky "copying" progress bar, a paperclip offering help when Co > 1, a blue screen when a tool crashes
 *   Star Citizen a run is a contract somewhere in Stanton, the Run page a quantum drive spooling towards endTime, a failed run
 *               is a 30k disconnect, a finished one pays out in aUEC
 *
 * Nothing here changes what the app does: the themes' moments say the same things the plain themes say, in their words.
 * Full-screen moments are dismissed with a click or Esc; nothing flashes faster than 1 Hz.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

static unsigned hash_str(const char *s) { unsigned h = 2166136261u; for (; s && *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }

/* ============================================================================ names for a case */
const char *flavour_case_tag(const char *path)
{
    static char b[96];
    unsigned h = hash_str(path);
    switch (TH.retro) {
    case RETRO_SEVERANCE: {
        static const char *const F[] = { "Cold Harbor", "Siena", "Tumwater", "Dranesville", "Allentown", "Labrador", "Jesup",
                                         "Wellington", "Loveland", "Narva", "Culpepper", "Ocula" };
        snprintf(b, sizeof b, "File: %s", F[h % (sizeof F / sizeof F[0])]);
        return b;
    }
    case RETRO_PORTAL: snprintf(b, sizeof b, "Test chamber %02u · subject #%04u", h % 19 + 1, h % 9000 + 1000); return b;
    case RETRO_NERV: snprintf(b, sizeof b, "Unit-%02u", h % 13 + 1); return b;
    case RETRO_PIPBOY: snprintf(b, sizeof b, "Vault %u terminal", h % 120 + 1); return b;
    case RETRO_ALIEN: snprintf(b, sizeof b, "Nostromo · file %03u", h % 1000); return b;
    case RETRO_STARCITIZEN: {
        static const char *const L[] = { "Hurston", "Crusader", "ArcCorp", "microTech", "Lorville", "Orison", "Area18",
                                         "New Babbage", "Daymar", "Yela", "Aberdeen", "Grim HEX" };
        snprintf(b, sizeof b, "Contract #%05u · %s", h % 90000 + 10000, L[h % (sizeof L / sizeof L[0])]);
        return b;
    }
    default: return NULL;
    }
}

/* ============================================================================ the moments (full screen, a modal) */
static struct { int ev; double at; char path[PL_PATH]; char text[300]; char latest[32]; } O;
static int sweepRuns;

void flavour_event(int ev, const char *casePath, const char *detail)
{
    if (!TH.retro) return;
    /* which moments each theme has */
    int r = TH.retro;
    bool show = false;
    switch (ev) {
    case FEV_FAILED:  show = r == RETRO_ALIEN || r == RETRO_SOULS || r == RETRO_WIN95 || r == RETRO_FACTORIO || r == RETRO_NERV || r == RETRO_STARCITIZEN; break;
    case FEV_DIVERGE: show = r == RETRO_ALIEN || r == RETRO_SOULS || r == RETRO_FACTORIO; break;
    case FEV_COURANT: show = r == RETRO_NERV || r == RETRO_WIN95; break;
    case FEV_DONE:    show = r == RETRO_PORTAL || r == RETRO_SOULS || r == RETRO_FACTORIO || r == RETRO_STARCITIZEN; break;
    case FEV_SWEEP:   show = r == RETRO_SEVERANCE; break;
    default: break;
    }
    if (!show || A.modal != MD_NONE || (g_ui_no_input && !getenv("QC_TEST_FLAVOUR"))) return;   /* never over a question, nor in a scripted run unless asked */
    O.ev = ev; O.at = GetTime();
    pl_strlcpy(O.path, casePath ? casePath : "", sizeof O.path);
    pl_strlcpy(O.text, detail ? detail : "", sizeof O.text);
    O.latest[0] = 0;
    if (casePath && *casePath) { CaseSummary cs; case_summarize(casePath, &cs); pl_strlcpy(O.latest, case_latest_result(&cs), sizeof O.latest); }
    A.modal = MD_FLAVOUR;
}

/* the queue ran several cases and is empty now: a parameter sweep is done (Severance's waffle party) */
void flavour_queue_progress(int started, bool emptyNow)
{
    if (started > 0) sweepRuns += started;
    if (emptyNow && sweepRuns >= 3) { flavour_event(FEV_SWEEP, "", NULL); sweepRuns = 0; }
    else if (emptyNow) sweepRuns = 0;
}

static void center(UiFamily f, float size, const char *s, float cx, float y, Color c)
{
    ui_text(f, size, s, cx - ui_text_w(f, size, s) / 2, y, c);
}

static void draw_cake(float cx, float cy, float s)
{
    Color sponge = { 120, 70, 40, 255 }, cream = { 250, 246, 238, 255 }, cherry = { 220, 20, 40, 255 };
    DrawRectangleRec((Rectangle){ cx - s, cy - s * 0.3f, 2 * s, s * 0.9f }, sponge);
    DrawRectangleRec((Rectangle){ cx - s, cy - s * 0.42f, 2 * s, s * 0.18f }, cream);
    for (int i = 0; i < 6; i++) DrawCircleV((Vector2){ cx - s + s * 0.2f + i * s * 0.32f, cy - s * 0.42f }, s * 0.1f, cream);
    DrawRectangleRec((Rectangle){ cx - s, cy + s * 0.15f, 2 * s, s * 0.08f }, cream);
    DrawRectangleRec((Rectangle){ cx - s * 0.04f, cy - s * 0.95f, s * 0.08f, s * 0.5f }, (Color){ 240, 230, 200, 255 });
    DrawCircleV((Vector2){ cx, cy - s * 1.0f }, s * 0.08f, (Color){ 255, 200, 60, 255 });
    DrawCircleV((Vector2){ cx + s * 0.4f, cy - s * 0.5f }, s * 0.11f, cherry);
}

static void draw_bonfire(float cx, float cy, float s, float t)
{
    Color ember = { 255, 140, 40, 255 }, core = { 255, 220, 120, 255 };
    for (int i = 0; i < 3; i++) {
        float a = sinf(t * 2.0f + i) * 0.08f;
        DrawTriangle((Vector2){ cx + (i - 1) * s * 0.35f, cy - s * (1.1f + a) }, (Vector2){ cx + (i - 1) * s * 0.35f - s * 0.3f, cy },
                     (Vector2){ cx + (i - 1) * s * 0.35f + s * 0.3f, cy }, ui_alpha(ember, 0.8f));
    }
    DrawTriangle((Vector2){ cx, cy - s * 0.7f }, (Vector2){ cx - s * 0.2f, cy }, (Vector2){ cx + s * 0.2f, cy }, core);
    DrawRectangleRec((Rectangle){ cx - s * 0.05f, cy - s * 1.5f, s * 0.1f, s * 1.3f }, (Color){ 110, 100, 90, 255 });   /* the sword */
    DrawRectangleRec((Rectangle){ cx - s * 0.6f, cy, s * 1.2f, s * 0.12f }, (Color){ 60, 50, 40, 255 });
}

static void draw_biter(float x, float y, float s, float t)
{
    Color b = { 150, 80, 60, 255 };
    DrawEllipse((int)x, (int)y, s, s * 0.6f, b);
    for (int i = 0; i < 3; i++) {
        float k = sinf(t * 8 + i) * s * 0.15f;
        DrawLineEx((Vector2){ x - s * 0.5f + i * s * 0.5f, y + s * 0.4f }, (Vector2){ x - s * 0.7f + i * s * 0.5f, y + s + k }, 3, b);
    }
    DrawCircleV((Vector2){ x + s * 0.6f, y - s * 0.1f }, s * 0.12f, (Color){ 255, 230, 120, 255 });
}

static void draw_clippy(float x, float y, float s)
{
    /* a paperclip standing up: an outer loop, an inner loop, two eyes and brows above */
    Color wire = { 110, 112, 120, 255 };
    float w1 = s * 0.42f, w2 = s * 0.26f, top = y, bot = y + s * 1.9f, th = 4;
    DrawRing((Vector2){ x, top + w1 }, w1 - th / 2, w1 + th / 2, 180, 360, 24, wire);            /* outer top */
    DrawRectangleRec((Rectangle){ x - w1 - th / 2, top + w1, th, bot - top - w1 - w2 }, wire);
    DrawRectangleRec((Rectangle){ x + w1 - th / 2, top + w1, th, s * 1.2f }, wire);
    DrawRing((Vector2){ x - w1 + w2, bot - w2 }, w2 - th / 2, w2 + th / 2, 0, 180, 20, wire);      /* bottom turn */
    DrawRectangleRec((Rectangle){ x - w1 + 2 * w2 - th / 2, top + s * 0.55f, th, bot - w2 - top - s * 0.55f }, wire);
    DrawRing((Vector2){ x - w1 + 2 * w2 + (w1 - w2) / 2 - th / 2, top + s * 0.55f }, (w1 - 2 * w2 + w1) / 2 - th / 2 - 4, (w1 - 2 * w2 + w1) / 2 + th / 2 - 4, 180, 360, 16, wire);
    float ey = top + s * 0.35f;
    DrawCircleV((Vector2){ x - s * 0.16f, ey }, s * 0.13f, WHITE); DrawCircleLinesV((Vector2){ x - s * 0.16f, ey }, s * 0.13f, wire);
    DrawCircleV((Vector2){ x + s * 0.16f, ey }, s * 0.13f, WHITE); DrawCircleLinesV((Vector2){ x + s * 0.16f, ey }, s * 0.13f, wire);
    DrawCircleV((Vector2){ x - s * 0.13f, ey + s * 0.03f }, s * 0.055f, BLACK); DrawCircleV((Vector2){ x + s * 0.19f, ey + s * 0.03f }, s * 0.055f, BLACK);
    DrawLineEx((Vector2){ x - s * 0.3f, ey - s * 0.2f }, (Vector2){ x - s * 0.05f, ey - s * 0.24f }, 3, BLACK);
    DrawLineEx((Vector2){ x + s * 0.05f, ey - s * 0.24f }, (Vector2){ x + s * 0.3f, ey - s * 0.18f }, 3, BLACK);
}

/* the moment itself: draw_modals calls it while A.modal is MD_FLAVOUR */
void flavour_modal(bool *close)
{
    float W = ui_width(), H = ui_height(), t = (float)(GetTime() - O.at);
    ui_request_frames(2);
    Rectangle all = { 0, 0, W, H };
    bool click = ui_clicked(all) && t > 0.4f;
    const char *who = O.path[0] ? pl_basename(O.path) : "";
    float cx = W / 2, cy = H / 2;
    switch (TH.retro) {
    case RETRO_SOULS:
        if (O.ev == FEV_DONE) {
            DrawRectangleRec(all, ui_alpha(BLACK, 0.75f));
            DrawRectangleGradientV(0, (int)(cy - 70), (int)W, 140, ui_alpha(BLACK, 0), ui_alpha((Color){ 40, 30, 10, 255 }, 0.9f));
            center(F_SEMI, 48, "BONFIRE LIT", cx, cy - 30, (Color){ 230, 190, 110, 255 });
            draw_bonfire(cx, cy + 120, 40, t);
        } else {                                                /* YOU DIED, red on black, slowly in */
            float a = fminf(1, t / 1.5f);
            DrawRectangleRec(all, ui_alpha(BLACK, 0.85f * a));
            DrawRectangleRec((Rectangle){ 0, cy - 60, W, 120 }, ui_alpha(BLACK, 0.6f * a));
            center(F_SEMI, 72, "YOU DIED", cx, cy - 40, ui_alpha((Color){ 160, 20, 20, 255 }, a));
            char sub[200]; pl_fmt_ui(sub, sizeof sub, "%s%s%s", who, O.text[0] ? ": " : "", O.text);
            center(F_REG, 15, sub, cx, cy + 60, ui_alpha((Color){ 190, 170, 140, 255 }, a));
            if (O.latest[0] && t > 1.0f) {
                char lab[96]; pl_fmt_ui(lab, sizeof lab, "Respawn at the last bonfire (t = %s)", O.latest);
                float bw = ui_button_width(ICON_FLAME, lab);
                if (ui_button(ui_id("fl-respawn"), (Rectangle){ cx - bw / 2, cy + 100, bw, 40 }, ICON_FLAME, lab, BTN_SECONDARY)) {
                    /* the bonfire is the latest written time: continue from it */
                    if (A.cs && !strcmp(A.cs->s.path, O.path)) { A.continueRun = true; app_go(PG_RUN); A.runPending = true; }
                    *close = true; return;
                }
            }
        }
        break;
    case RETRO_ALIEN:
        DrawRectangleRec(all, BLACK);
        if (O.ev == FEV_DONE) center(F_SEMI, 34, "PROCESS COMPLETE", cx, cy - 20, TH.accent);
        else {
            float y = cy - 110;
            ui_text(F_SEMI, 26, "INQUIRY: STATUS OF CASE", cx - 300, y, TH.text2); y += 50;
            char l[200]; pl_fmt_ui(l, sizeof l, "> %s", who); ui_text(F_SEMI, 26, l, cx - 300, y, TH.text2); y += 60;
            ui_text(F_SEMI, 34, "SPECIAL ORDER 937", cx - 300, y, TH.accent); y += 50;
            if (fmodf(t, 1.0f) < 0.6f || t > 4) ui_text(F_SEMI, 34, "CASE EXPENDABLE", cx - 300, y, TH.err);   /* 1 Hz */
            y += 60;
            if (O.text[0]) ui_text_wrap(F_SEMI, 18, O.text, (Rectangle){ cx - 300, y, 640, 120 }, 26, TH.text3, true);
        }
        break;
    case RETRO_WIN95:
        if (O.ev == FEV_COURANT) {                             /* the paperclip */
            Rectangle bub = { W - 430, H - 300, 360, 150 };
            DrawRectangleRec(bub, (Color){ 255, 255, 225, 255 });
            DrawRectangleLinesEx(bub, 1, BLACK);
            ui_text_wrap(F_REG, 14, "It looks like your Courant number is above 1. Would you like help?", (Rectangle){ bub.x + 14, bub.y + 14, bub.width - 28, 60 }, 20, BLACK, true);
            if (ui_button(ui_id("fl-clippy-yes"), (Rectangle){ bub.x + 14, bub.y + 96, 160, 32 }, NULL, "Halve the time step", BTN_SECONDARY)) { app_alert_halve(); *close = true; return; }
            if (ui_button(ui_id("fl-clippy-no"), (Rectangle){ bub.x + 186, bub.y + 96, 160, 32 }, NULL, "Don't show this again", BTN_SECONDARY)) { *close = true; return; }
            draw_clippy(W - 120, H - 140, 60);
            click = false;
        } else {                                               /* the blue screen */
            DrawRectangleRec(all, (Color){ 0, 0, 170, 255 });
            Rectangle tag = { cx - 50, cy - 160, 100, 26 };
            DrawRectangleRec(tag, (Color){ 170, 170, 170, 255 });
            ui_text_center(F_MONO_MED, 16, "Windows", tag, (Color){ 0, 0, 170, 255 });
            char l1[300]; pl_fmt_ui(l1, sizeof l1, "A fatal exception 0E has occurred in %s.", O.text[0] ? O.text : "checkMesh");
            float y = cy - 100;
            ui_text(F_MONO, 16, l1, cx - 330, y, WHITE); y += 26;
            ui_text(F_MONO, 16, "The current case will be terminated.", cx - 330, y, WHITE); y += 44;
            ui_text(F_MONO, 16, "*  The log on the Run page says what went wrong.", cx - 330, y, WHITE); y += 26;
            ui_text(F_MONO, 16, "*  Press any key or click to continue.", cx - 330, y, WHITE); y += 50;
            ui_text(F_MONO, 16, "Press any key to continue _", cx - 140, y, WHITE);
            if (GetKeyPressed() != 0 && t > 0.4f) click = true;
        }
        break;
    case RETRO_NERV: {
        bool blue = O.ev == FEV_COURANT;
        Color c = blue ? (Color){ 60, 140, 255, 255 } : (Color){ 255, 40, 30, 255 };
        float a = 0.6f + 0.4f * sinf(t * 6.2832f);              /* 1 Hz */
        DrawRectangleRec(all, ui_alpha(BLACK, 0.8f));
        for (int i = -3; i <= 3; i++) DrawPolyLines((Vector2){ cx + i * 150, cy }, 6, 80, 30, ui_alpha(c, 0.4f * a));
        DrawRectangleRec((Rectangle){ 0, cy - 70, W, 140 }, ui_alpha(c, 0.18f));
        DrawRectangleRec((Rectangle){ 0, cy - 72, W, 4 }, ui_alpha(c, a)); DrawRectangleRec((Rectangle){ 0, cy + 68, W, 4 }, ui_alpha(c, a));
        center(F_SEMI, 64, blue ? "PATTERN BLUE" : "EMERGENCY", cx, cy - 34, ui_alpha(c, 0.7f + 0.3f * a));
        char sub[300]; pl_fmt_ui(sub, sizeof sub, "%s%s%s", flavour_case_tag(O.path) ? flavour_case_tag(O.path) : who,
                                 O.text[0] ? "  ·  " : "", O.text[0] ? O.text : blue ? "Courant number above 1" : "");
        center(F_REG, 16, sub, cx, cy + 90, ui_alpha(c, 0.9f));
        break;
    }
    case RETRO_PORTAL:
        DrawRectangleRec(all, ui_alpha(WHITE, 0.92f));
        draw_cake(cx, cy - 20, 70);
        center(F_SEMI, 30, "Test complete. Cake dispensed.", cx, cy + 90, (Color){ 30, 36, 40, 255 });
        center(F_REG, 15, "As promised. This one is not a lie.", cx, cy + 132, (Color){ 90, 100, 110, 255 });
        break;
    case RETRO_FACTORIO:
        DrawRectangleRec(all, ui_alpha(BLACK, 0.7f));
        if (O.ev == FEV_DONE) {
            center(F_SEMI, 34, "Research completed", cx, cy - 30, TH.accent);
            center(F_REG, 16, who, cx, cy + 20, TH.text2);
        } else {
            for (int i = 0; i < 5; i++) draw_biter(cx - 260 + i * 130 + fmodf(t * 40, 130), cy + 90, 26, t + i);
            center(F_SEMI, 34, "Your base is under attack", cx, cy - 60, TH.err);
            char sub[300]; pl_fmt_ui(sub, sizeof sub, "%s%s%s", who, O.text[0] ? ": " : "", O.text);
            center(F_REG, 16, sub, cx, cy - 10, TH.text2);
        }
        break;
    case RETRO_SEVERANCE:
        DrawRectangleRec(all, ui_alpha(TH.bg, 0.92f));
        for (int i = 0; i < 9; i++) {                         /* waffles */
            float x = cx - 320 + i * 80, y = cy + 80 + sinf(t * 2 + i) * 6;
            DrawRectangleRec((Rectangle){ x, y, 56, 56 }, (Color){ 214, 160, 80, 255 });
            for (int g = 1; g < 4; g++) { DrawRectangleRec((Rectangle){ x + g * 14, y, 2, 56 }, (Color){ 170, 115, 50, 255 }); DrawRectangleRec((Rectangle){ x, y + g * 14, 56, 2 }, (Color){ 170, 115, 50, 255 }); }
        }
        center(F_SEMI, 40, "WAFFLE PARTY", cx, cy - 70, TH.accent);
        center(F_REG, 16, "Every file of the sweep is refined. Management is delighted.", cx, cy - 16, TH.text2);
        break;
    case RETRO_STARCITIZEN:
        DrawRectangleRec(all, ui_alpha(TH.bg, 0.9f));
        if (O.ev == FEV_DONE) {                                /* the payout */
            unsigned pay = (hash_str(O.path) % 90 + 10) * 1250u;
            DrawRectangleRec((Rectangle){ 0, cy - 64, W, 2 }, ui_alpha(TH.accent, 0.8f));
            DrawRectangleRec((Rectangle){ 0, cy + 62, W, 2 }, ui_alpha(TH.accent, 0.8f));
            center(F_SEMI, 40, "Contract complete", cx, cy - 46, TH.accent);
            char sub[200]; pl_fmt_ui(sub, sizeof sub, "+%u aUEC  ·  %s", pay, flavour_case_tag(O.path) ? flavour_case_tag(O.path) : who);
            center(F_MED, 18, sub, cx, cy + 14, TH.warn);
        } else {                                               /* 30k: the server is gone, and so is the run */
            float a = fminf(1, t / 0.8f);
            center(F_SEMI, 34, "Disconnected", cx, cy - 80, ui_alpha(TH.text, a));
            center(F_MED, 18, "Error code 30000", cx, cy - 30, ui_alpha(TH.err, a));
            char sub[300]; pl_fmt_ui(sub, sizeof sub, "%s%s%s", who, O.text[0] ? ": " : "", O.text);
            center(F_REG, 15, sub, cx, cy + 10, ui_alpha(TH.text2, a));
            center(F_REG, 14, "The log on the Run page says what went wrong. Click to return to the main menu.", cx, cy + 40, ui_alpha(TH.text3, a));
            for (int i = 0; i < 3; i++)                        /* the spinner of a reconnect that never comes */
                DrawRing((Vector2){ cx, cy + 110 }, 18 - i * 5, 20 - i * 5, t * (120 + 60 * i), t * (120 + 60 * i) + 240, 24, ui_alpha(TH.accent, 0.5f + 0.2f * i));
        }
        break;
    default: *close = true; return;
    }
    if (click || inp_key_pressed(KEY_ESCAPE) || t > 12) *close = true;
}

/* ============================================================================ the Run page's instrument */
static double last_residual(const LogMon *m, double *worst)
{
    double w = 0, best = 1;
    for (int i = 0; m && i < m->nSeries; i++) {
        const Series *s = &m->series[i];
        if (!s->n) continue;
        double v = s->v[s->n - 1];
        if (v > w) w = v;
        if (v < best) best = v;
    }
    if (worst) *worst = w;
    return best;
}

/* height used; nothing for the themes without one */
float flavour_run(Case *c, LogMon *m, bool running, float x, float y, float w)
{
    if (!TH.retro || !c) return 0;
    double t0 = c->s.startTime, t1 = c->s.endTime, tn = m ? m->time : t0;
    double frac = t1 > t0 ? (tn - t0) / (t1 - t0) : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    double worst = 0; last_residual(m, &worst);
    double co = m && m->courant.n ? m->courant.v[m->courant.n - 1] : 0;
    float h = 0, now = (float)GetTime();
    if (running) ui_request_frames(2);
    switch (TH.retro) {
    case RETRO_WIN95: {                                         /* a little window: title bar and the chunky progress bar */
        Rectangle win = { x, y, w, 92 };
        ui_rect(win, 0, TH.surface); ui_rect_lines(win, 0, 1, TH.lineSoft);
        DrawRectangleGradientH((int)x + 3, (int)y + 3, (int)w - 6, 20, (Color){ 0, 0, 128, 255 }, (Color){ 16, 132, 208, 255 });
        char tt[160]; pl_fmt_ui(tt, sizeof tt, "Copying %s...", running ? (c->s.app[0] ? c->s.app : "files") : "files");
        ui_text(F_MED, 12.5f, tt, x + 8, y + 6, WHITE);
        Rectangle bar = { x + 12, y + 34, w - 24, 22 };
        DrawRectangleRec(bar, WHITE);
        DrawRectangleLinesEx(bar, 1, (Color){ 128, 128, 128, 255 });
        int blocks = (int)((bar.width - 4) / 14), on = (int)(frac * blocks + 0.5);
        for (int i = 0; i < on; i++) DrawRectangleRec((Rectangle){ bar.x + 2 + i * 14, bar.y + 3, 12, bar.height - 6 }, (Color){ 0, 0, 128, 255 });
        char st[160]; pl_fmt_ui(st, sizeof st, "t = %g of %g  (%.0f%%)", tn, t1, frac * 100);
        ui_text(F_REG, 12.5f, st, x + 12, y + 64, BLACK);
        h = 104;
        break;
    }
    case RETRO_SOULS: {                                        /* HP from the residuals, stamina from the Courant headroom, bonfires */
        double hp = worst > 0 ? fmin(1, fmax(0, (-log10(worst)) / 6.0)) : 1, st = co > 0 ? fmax(0, 1 - co / 1.0) : 1;
        float bw = fminf(420, w * 0.5f);
        DrawRectangleRec((Rectangle){ x, y, bw, 10 }, (Color){ 40, 10, 10, 255 }); DrawRectangleRec((Rectangle){ x, y, bw * (float)hp, 10 }, (Color){ 170, 20, 20, 255 });
        DrawRectangleRec((Rectangle){ x, y + 16, bw * 0.8f, 8 }, (Color){ 20, 40, 20, 255 }); DrawRectangleRec((Rectangle){ x, y + 16, bw * 0.8f * (float)st, 8 }, (Color){ 60, 150, 50, 255 });
        ui_text(F_REG, 12, "HP: the residuals", x + bw + 12, y - 3, TH.text3);
        ui_text(F_REG, 12, "Stamina: Courant headroom", x + bw * 0.8f + 12, y + 13, TH.text3);
        /* bonfires: the written times along the run */
        float by = y + 40;
        DrawRectangleRec((Rectangle){ x, by + 22, w, 2 }, TH.lineSoft);
        int lit = 0;
        for (int i = 0; i < c->times.n && t1 > t0; i++) {
            double tv = atof(c->times.names[i]);
            if (tv <= t0) continue;
            float px = x + (float)((tv - t0) / (t1 - t0)) * w;
            draw_bonfire(px, by + 22, 7, now + i);
            lit++;
        }
        char st2[96]; pl_fmt_ui(st2, sizeof st2, "%d bonfire%s lit", lit, lit == 1 ? "" : "s");
        ui_text(F_REG, 12, st2, x + w - ui_text_w(F_REG, 12, st2), by - 4, TH.text3);
        h = 76;
        break;
    }
    case RETRO_FACTORIO: {                                     /* assembler -> belt of written times -> post-processing */
        float bh = 54;
        Rectangle asm_ = { x, y, 70, bh }, pp = { x + w - 90, y, 90, bh };
        DrawRectangleRec(asm_, (Color){ 90, 86, 80, 255 }); DrawRectangleLinesEx(asm_, 2, BLACK);
        DrawCircleSector((Vector2){ asm_.x + 35, asm_.y + 27 }, 16, now * (running ? 90 : 0), now * (running ? 90 : 0) + 300, 12, TH.accent);
        DrawRectangleRec(pp, (Color){ 70, 80, 96, 255 }); DrawRectangleLinesEx(pp, 2, BLACK);
        ui_text(F_REG, 11, "post-", pp.x + 8, pp.y + 10, TH.text); ui_text(F_REG, 11, "processing", pp.x + 8, pp.y + 26, TH.text);
        Rectangle belt = { x + 76, y + 18, w - 76 - 96, 18 };
        DrawRectangleRec(belt, (Color){ 60, 58, 54, 255 });
        float off = running ? fmodf(now * 60, 20) : 0;
        for (float k = belt.x - 20 + off; k < belt.x + belt.width; k += 20)
            if (k > belt.x) DrawTriangle((Vector2){ k, belt.y + 4 }, (Vector2){ k, belt.y + belt.height - 4 }, (Vector2){ k + 8, belt.y + belt.height / 2 }, (Color){ 255, 210, 58, 255 });
        int n = 0;
        for (int i = 0; i < c->times.n; i++) if (atof(c->times.names[i]) > t0) n++;
        for (int i = 0; i < n && i < 40; i++) {                /* the crates: written times */
            float px = belt.x + 6 + fmodf(i * 26.0f + (running ? now * 20 : 0), belt.width - 20);
            DrawRectangleRec((Rectangle){ px, belt.y + 3, 12, 12 }, (Color){ 190, 140, 70, 255 });
        }
        double wall = m ? (m->clockTime >= 1 ? m->clockTime : m->execTime) : 0;   /* ClockTime counts whole seconds */
        double rate = m && wall > 0 && m->time > m->startTimeSeen ? (m->time - m->startTimeSeen) / wall : 0;
        char st[160]; pl_fmt_ui(st, sizeof st, "Production: %.4g simulated s per wall-clock hour  ·  %d time folder%s produced", rate * 3600, n, n == 1 ? "" : "s");
        ui_text(F_REG, 12.5f, st, x, y + bh + 6, TH.text2);
        h = bh + 32;
        break;
    }
    case RETRO_NERV: {                                         /* sync ratio from the residuals */
        double sync = worst > 0 ? fmin(400, fmax(0, (-log10(worst) - 1) / 5.0 * 100)) : 0;
        const char *tag = flavour_case_tag(c->s.path);
        char st[160]; pl_fmt_ui(st, sizeof st, "%s  ·  SYNC RATIO %.1f%%", tag ? tag : "", sync);
        ui_text(F_SEMI, 20, st, x, y, sync >= 100 ? TH.ok : TH.accent);
        float bw = w * 0.5f;
        DrawRectangleRec((Rectangle){ x + w - bw, y + 6, bw, 12 }, ui_alpha(TH.accent, 0.15f));
        DrawRectangleRec((Rectangle){ x + w - bw, y + 6, bw * (float)fmin(1, sync / 100), 12 }, sync >= 100 ? TH.ok : TH.accent);
        h = 38;
        break;
    }
    case RETRO_SEVERANCE: {                                    /* the file, its %, the four bins */
        const char *tag = flavour_case_tag(c->s.path);
        char st[160]; pl_fmt_ui(st, sizeof st, "%s   %.0f%% complete", tag ? tag : "", frac * 100);
        ui_text(F_SEMI, 18, st, x, y, TH.text);
        static const char *const BIN[4] = { "WO", "FC", "DR", "MA" };
        float bw = (w - 3 * 12) / 4;
        for (int b = 0; b < 4; b++) {
            Rectangle br = { x + b * (bw + 12), y + 30, bw, 40 };
            ui_rect(br, 4, TH.surface); ui_rect_lines(br, 4, 1, TH.lineSoft);
            int tot = 0, conv = 0; char names[120] = ""; int k = 0;
            for (int i = 0; m && i < m->nSeries; i++) {
                if ((int)(hash_str(m->series[i].name) % 4) != b || !m->series[i].n) continue;
                tot++;
                if (m->series[i].v[m->series[i].n - 1] < 1e-5) { conv++; if (k < 100) k += snprintf(names + k, sizeof names - (size_t)k, "%s%s", k ? " " : "", m->series[i].name); }
            }
            float f = tot ? (float)conv / tot : 0;
            DrawRectangleRec((Rectangle){ br.x + 2, br.y + br.height - 4, (br.width - 4) * f, 3 }, TH.accent);
            ui_text(F_SEMI, 14, BIN[b], br.x + 10, br.y + 6, TH.text);
            ui_text_clip(F_MONO, 11.5f, names[0] ? names : "-", br.x + 40, br.y + 8, br.width - 50, TH.text2);
        }
        h = 82;
        break;
    }
    case RETRO_PIPBOY: {                                       /* STAT / INV / DATA, and the Geiger counter */
        static const char *const TABS[3] = { "STAT", "INV", "DATA" };
        static const Page TO[3] = { PG_RUN, PG_FILES, PG_OVERVIEW };
        float tx = x;
        for (int i = 0; i < 3; i++) {
            float tw = ui_text_w(F_SEMI, 16, TABS[i]) + 26;
            Rectangle tr = { tx, y, tw, 28 };
            bool on = A.page == TO[i];
            if (on) DrawRectangleLinesEx(tr, 1, TH.accent);
            ui_text_center(F_SEMI, 16, TABS[i], tr, on ? TH.text : TH.text3);
            ui_register(ui_idn(ui_id("pip-tab"), i), tr, TABS[i]);
            if (ui_clicked(tr) && !on) app_go(TO[i]);
            tx += tw + 10;
        }
        double ce = m ? fabs(m->contLocal) : 0;
        double lvl = ce > 0 ? fmin(1, fmax(0, (log10(ce) + 12) / 10)) : 0;   /* 1e-12 .. 1e-2 */
        float gx = x + w - 260;
        ui_text(F_SEMI, 13, "RADS", gx, y + 6, TH.text2);
        DrawRectangleLinesEx((Rectangle){ gx + 50, y + 6, 200, 14 }, 1, TH.line);
        DrawRectangleRec((Rectangle){ gx + 52, y + 8, 196 * (float)lvl, 10 }, lvl > 0.7 ? TH.err : TH.accent);
        char st[96]; pl_fmt_ui(st, sizeof st, "continuity error %.2g", ce);
        ui_text(F_REG, 11.5f, st, gx + 50, y + 24, TH.text3);
        /* the counter crackles with it, a click now and then (with the theme's sounds on) */
        static double nextClick;
        if (running && ce > 0 && GetTime() > nextClick) { app_sound(SND_CLICK); nextClick = GetTime() + 1.6 - 1.5 * lvl; }
        h = 50;
        break;
    }
    case RETRO_PORTAL: {                                        /* the chamber, the subject, and the facility AI's remark */
        const char *tag = flavour_case_tag(c->s.path);
        ui_text(F_SEMI, 15, tag ? tag : "", x, y, TH.text);
        char line[300] = "";
        char lp[PL_PATH]; pl_join(lp, sizeof lp, c->s.path, "log.checkMesh");
        char *cm = pl_read_file(lp, NULL);
        long nonOrth = 0;
        const char *q = cm ? strstr(cm, "Number of severely non-orthogonal") : NULL;
        if (q) { const char *e = strstr(q, ": "); if (e) nonOrth = atol(e + 2); }
        double maxNO = 0; q = cm ? strstr(cm, "Max non-orthogonality = ") : NULL; if (q) maxNO = atof(q + 24);
        free(cm);
        if (q && maxNO > 0) pl_fmt_ui(line, sizeof line, "\"Your mesh is %.0f degrees non-orthogonal%s. We are all very proud of you.\"", maxNO,
                                       nonOrth ? TextFormat(", with %ld severely skewed faces", nonOrth) : "");
        else if (co > 1) pl_fmt_ui(line, sizeof line, "\"A Courant number of %.2g. Interesting. Most subjects keep it below one.\"", co);
        else if (running) pl_fmt_ui(line, sizeof line, "\"The test is going well. We will stop enjoying it if the residuals rise.\"");
        else pl_fmt_ui(line, sizeof line, "\"When you're ready, the chamber is. There will be cake.\"");
        ui_text_wrap(F_REG, 13.5f, line, (Rectangle){ x, y + 24, w, 40 }, 19, TH.text2, true);
        h = 62;
        break;
    }
    case RETRO_STARCITIZEN: {                                  /* the quantum drive: spool, then travel to endTime */
        const char *tag = flavour_case_tag(c->s.path);
        char st[200]; pl_fmt_ui(st, sizeof st, "%s  ·  %s", tag ? tag : "", running ? (frac > 0 ? "Quantum travel" : "Spooling") : frac >= 1 ? "Arrived" : "Quantum drive ready");
        ui_text(F_SEMI, 16, st, x, y, TH.accent);
        Rectangle bar = { x, y + 26, w, 10 };
        DrawRectangleRec(bar, ui_alpha(TH.accent, 0.12f));
        for (float k = bar.x; k < bar.x + bar.width; k += 24) DrawRectangleRec((Rectangle){ k, bar.y, 1, bar.height }, ui_alpha(TH.bg, 0.8f));
        DrawRectangleRec((Rectangle){ bar.x, bar.y, bar.width * (float)frac, bar.height }, TH.accent);
        if (running) {                                         /* the streak at the head */
            float hx = bar.x + bar.width * (float)frac;
            DrawRectangleGradientH((int)(hx - 60), (int)bar.y, 60, (int)bar.height, ui_alpha(WHITE, 0), ui_alpha(WHITE, 0.7f));
        }
        char dl[160]; pl_fmt_ui(dl, sizeof dl, "Distance to destination  %.4g s of simulated time  ·  %.0f%%", t1 - tn > 0 ? t1 - tn : 0, frac * 100);
        ui_text(F_REG, 12.5f, dl, x, y + 42, TH.text2);
        h = 66;
        break;
    }
    case RETRO_ALIEN: {
        char st[160]; pl_fmt_ui(st, sizeof st, "INTERFACE 2037 READY FOR INQUIRY%s", running ? "  ·  PROCESS RUNNING" : "");
        ui_text(F_SEMI, 18, st, x, y, TH.text2);
        h = 30;
        break;
    }
    default: break;
    }
    return h;
}
