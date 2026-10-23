/* script.c -- test scripts that drive the app as a user would (see script.h)
 *
 * Commands
 *   click <target>              dblclick <target>          choose <target> <item text>
 *   type <text>                 enter <text>               key <KEY> [ctrl] [shift]
 *   wheel <dy> [<target>]       drag <target> <dx> <dy>
 *   wait <frames>               sleep <seconds>            wait-until <cond> [timeout <s>]
 *   assert <cond>               screenshot <file.png>      dump <file.txt>
 *   coupling <folder>           (a coupled simulation: the folder with precice-config.xml)
 *   open <case dir>             page <title>               theme <0|1|2>     note <text>     quit
 *   shell <command>             (runs in the open case's folder, as an outside editor or script would; no case: the workspace)
 *   measure                     (count clicks, keystrokes and wall time from here: the setup before it is not the journey)
 *   budget [p95 <ms>] [max <ms>] (frame-time budget, checked when the script ends; frame 0, the frames after a shell
 *                               step and the frames that open a case (open, or a click on one) are left out)
 * Conditions (prefix "not " negates)
 *   page <title>   modal none|any|<name>   text <s>   widget <target>   enabled <target>   toast <s>
 *   job idle|running|finished|failed|stopped   file <case-relative or absolute path>   log <log name> <text>
 *   case-open
 * open, page and theme set the scene; they are not counted as user actions.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "app.h"
#include "actions.h"
#include "script.h"

#define MAX_LINES 2000

static struct {
    char  **line;
    int     n, at;               /* lines, current line */
    int     lineNo[MAX_LINES];
    bool    active, done;
    int     failures;
    /* the command in progress */
    int     phase, waitFrames, scrollTries, settle;
    double  deadline, t0, firstAction;
    float   lastY;
    /* counts for the journey report */
    int     clicks, keys;
    double  frameMs[1 << 16]; int nFrames;        /* each frame's own work, for the frame-time budget */
    int     slow[8]; double slowMs[8]; int nSlow; /* the slowest frames: which, and on which line of the script */
    int     slowLine[8];
    double  budgetP95, budgetMax;                 /* 0 = not checked */
    unsigned char excused[1 << 16];               /* frames that loaded a case or ran a shell step */
    int     excuseUntil;                          /*   frames before this one are excused         */
    char    shot[PL_PATH];
} S;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    printf("[%7.2fs] ", now_s() - S.t0);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...)
{
    char m[600];
    va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    say("FAIL line %d: %s", S.lineNo[S.at], m);
    S.failures++;
    char t[16000]; ui_text_dump(t, sizeof t);              /* what was on screen, for the post-mortem */
    printf("---- visible text ----\n%.4000s\n----------------------\n", t);
}

bool script_load(const char *path)
{
    size_t len; char *t = pl_read_file(path, &len);
    if (!t) { fprintf(stderr, "--script: cannot read %s\n", path); return false; }
    S.line = calloc(MAX_LINES, sizeof(char *));
    int no = 0;
    for (char *l = t, *e; l && *l && S.n < MAX_LINES; l = e ? e + 1 : NULL) {
        e = strchr(l, '\n');
        if (e) *e = 0;
        no++;
        while (isspace((unsigned char)*l)) l++;
        size_t L = strlen(l);
        while (L && isspace((unsigned char)l[L - 1])) l[--L] = 0;
        if (!*l || *l == '#') continue;
        S.lineNo[S.n] = no;
        S.line[S.n++] = strdup(l);
    }
    free(t);
    S.active = true;
    S.t0 = now_s();
    S.firstAction = -1;
    return true;
}

bool script_active(void) { return S.active; }
bool script_done(void) { return S.done; }
int  script_exit_code(void) { return S.failures ? 1 : 0; }

/* ---------------------------------------------------------------- targets */
static UiId parse_id(const char *s)
{
    char base[128]; int k = 0;
    while (s[k] && s[k] != '#' && k < 127) { base[k] = s[k]; k++; }
    base[k] = 0;
    UiId id = ui_id(base);
    for (const char *p = s + k; *p == '#'; ) { id = ui_idn(id, atoi(p + 1)); p++; while (*p && *p != '#') p++; }
    return id;
}

/* where a target is; vis.width 0 when it exists but is off screen */
static bool resolve(const char *t, Rectangle *vis, Rectangle *whole, Rectangle *view, bool *disabled)
{
    *disabled = false;
    if (!strncmp(t, "text:", 5)) {
        Rectangle v;
        if (!ui_text_find(t + 5, &v, false)) return false;
        *vis = *whole = v; *view = v;
        return true;
    }
    const UiWidget *w = !strncmp(t, "label:", 6) ? ui_widget_by_label(t + 6) : ui_widget_find(parse_id(t));
    if (!w) return false;
    *vis = w->vis; *whole = w->r; *view = w->view; *disabled = w->disabled;
    return true;
}

/* ---------------------------------------------------------------- conditions */
static const char *const MODAL_NAMES[] = { "none", "open-folder", "clone", "clean", "delete", "unsaved", "quit-running", "add-entry",
    "run-results", "new-file", "run-script", "cpl-new", "cpl-open", "cpl-results", "pv-claude", "hist-confirm", "precheck", "queue-add",
    "movie", "study-new", "study-out", "study-exp", "deme-new", "conflict", "shortcuts", "whats-new", "palette", "environment" };

const char *app_page_title(int p);

static bool cond(const char *c)
{
    while (isspace((unsigned char)*c)) c++;
    if (!strncmp(c, "not ", 4)) return !cond(c + 4);
    char w[32] = ""; int n = 0;
    sscanf(c, "%31s%n", w, &n);
    const char *arg = c + n;
    while (isspace((unsigned char)*arg)) arg++;
    if (!strcmp(w, "page")) return !strncasecmp(app_page_title(A.page), arg, strlen(arg));
    if (!strcmp(w, "modal")) {
        if (!strcmp(arg, "any")) return A.modal != MD_NONE;
        for (size_t i = 0; i < sizeof MODAL_NAMES / sizeof MODAL_NAMES[0]; i++) if (!strcmp(arg, MODAL_NAMES[i])) return (int)A.modal == (int)i;
        return false;
    }
    if (!strcmp(w, "text")) return ui_text_find(arg, NULL, false);
    if (!strcmp(w, "clipboard")) { const char *cb = GetClipboardText(); return cb && strstr(cb, arg); }
    if (!strcmp(w, "toast")) { for (int i = 0; i < ui_toast_count(); i++) if (strcasestr(ui_toast_text(i), arg)) return true; return false; }
    if (!strcmp(w, "widget") || !strcmp(w, "enabled")) {
        Rectangle v, r, vw; bool dis;
        if (!resolve(arg, &v, &r, &vw, &dis)) return false;
        return !strcmp(w, "widget") || !dis;
    }
    if (!strcmp(w, "case-open")) return A.cs != NULL;
    if (!strcmp(w, "fullscreen")) return A.set.fullscreen;
    if (!strcmp(w, "dirty")) return A.cs && case_dirty_count(A.cs) == atoi(arg);   /* edited files waiting for Save */
    if (!strcmp(w, "job")) {
        Job *j = A.cj;
        if (!strcmp(arg, "running")) return j->active;
        if (!strcmp(arg, "idle")) return !j->active;
        if (!strcmp(arg, "finished")) return !j->active && j->finished && !j->failed && !j->stopped;
        if (!strcmp(arg, "failed")) return !j->active && j->failed;
        if (!strcmp(arg, "stopped")) return !j->active && j->stopped;
        return false;
    }
    if (!strcmp(w, "file")) {
        char p[PL_PATH];
        if (arg[0] == '/' || !A.cs) pl_strlcpy(p, arg, sizeof p); else pl_join(p, sizeof p, A.cs->s.path, arg);
        return pl_exists(p);
    }
    if (!strcmp(w, "log")) {
        char name[PL_PATH] = ""; int k = 0;                  /* an absolute path can be long */
        sscanf(arg, "%4095s%n", name, &k);
        const char *needle = arg + k; while (isspace((unsigned char)*needle)) needle++;
        char p[PL_PATH];
        if (name[0] == '/') pl_strlcpy(p, name, sizeof p);   /* an absolute path: any file (the sound cue log) */
        else if (!A.cs) return false;
        else pl_join(p, sizeof p, A.cs->s.path, name);
        size_t len; char *t = pl_read_file(p, &len);
        bool ok = t && strstr(t, needle);
        free(t);
        return ok;
    }
    say("unknown condition '%s'", c);
    S.failures++;
    return true;
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

/* ---------------------------------------------------------------- running */
static void next(void) { S.at++; S.phase = 0; S.waitFrames = 0; S.scrollTries = 0; S.settle = 0; S.deadline = 0; }
static void act(void) { if (S.firstAction < 0) S.firstAction = now_s(); }

/* bring a target on screen and return its centre; false while still working (or after a failure: S.at moved on) */
static bool target_ready(const char *t, Vector2 *at)
{
    Rectangle vis, whole, view; bool dis;
    if (S.deadline == 0) S.deadline = now_s() + 8;
    if (!resolve(t, &vis, &whole, &view, &dis)) {
        if (now_s() > S.deadline) { fail("no widget or text '%s' on screen", t); next(); }
        return false;
    }
    /* fully visible (or at least its middle) and no longer moving: a smooth scroll has to settle first */
    bool middle = vis.width > 0 && CheckCollisionPointRec((Vector2){ whole.x + whole.width / 2, whole.y + whole.height / 2 }, vis);
    if (!middle) {
        if (++S.scrollTries > 240) { fail("'%s' stays off screen", t); next(); return false; }
        if (S.scrollTries % 6 == 1) {
            float vy = view.height > 0 ? view.y + view.height / 2 : ui_height() * ui_scale() / 2;
            float vx = view.width > 0 ? view.x + view.width / 2 : ui_width() * ui_scale() / 2;
            inp_queue_move(0, vx, vy);
            inp_queue_wheel(0, whole.y > vy ? -3.0f : 3.0f);
        }
        S.settle = 0;
        return false;
    }
    if (fabsf(whole.y - S.lastY) > 0.5f) { S.lastY = whole.y; S.settle = 0; return false; }
    if (++S.settle < 3) return false;
    if (dis) {
        if (now_s() > S.deadline) { fail("'%s' is disabled", t); next(); }
        return false;
    }
    *at = (Vector2){ whole.x + whole.width / 2, whole.y + whole.height / 2 };
    if (!strncmp(t, "text:", 5)) *at = (Vector2){ vis.x + fminf(vis.width / 2, 12), vis.y + vis.height / 2 };
    return true;
}

static void shot(const char *file)
{
    pl_strlcpy(S.shot, file, sizeof S.shot);              /* taken after this frame is drawn */
}

void script_tick(int frame)
{
    (void)frame;
    if (!S.active || S.done) return;
    for (int guard = 0; guard < 50 && S.at < S.n; guard++) {
        if (inp_pending()) return;                        /* earlier input still being played */
        if (S.waitFrames > 0) { S.waitFrames--; return; }
        char *L = S.line[S.at];
        char cmd[32] = ""; int k = 0;
        sscanf(L, "%31s%n", cmd, &k);
        char *arg = L + k; while (isspace((unsigned char)*arg)) arg++;
        if (!strcmp(cmd, "note")) { say("note: %s", arg); next(); continue; }
        if (!strcmp(cmd, "budget")) {
            char k1[8] = "", k2[8] = ""; double v1 = 0, v2 = 0;
            int got = sscanf(arg, "%7s %lf %7s %lf", k1, &v1, k2, &v2);
            for (int i = 0; i < got / 2; i++) {
                const char *k = i ? k2 : k1; double v = i ? v2 : v1;
                if (!strcmp(k, "p95")) S.budgetP95 = v; else if (!strcmp(k, "max")) S.budgetMax = v; else fail("budget: unknown '%s'", k);
            }
            next(); continue;
        }
        if (!strcmp(cmd, "measure")) {                    /* clicks, keys and the frame budget count from here */
            S.clicks = S.keys = 0; S.firstAction = -1;
            memset(S.excused, 1, (size_t)(S.nFrames < (int)sizeof S.excused ? S.nFrames : (int)sizeof S.excused));
            say("measure: counting from here"); next(); continue;
        }
        if (!strcmp(cmd, "wait")) { if (S.phase++ == 0) { S.waitFrames = atoi(arg); continue; } next(); continue; }
        if (!strcmp(cmd, "sleep")) {
            if (S.phase++ == 0) S.deadline = now_s() + atof(arg);
            if (now_s() < S.deadline) return;
            next(); continue;
        }
        if (!strcmp(cmd, "open")) {
            S.excuseUntil = S.nFrames + 3;                /* loading the case, then its first scan */
            char p[PL_PATH]; pl_expand_home(p, sizeof p, arg);
            app_open_case(p, PG_OVERVIEW);
            say("open %s -> %s", p, A.cs ? "ok" : "FAILED");
            if (!A.cs) S.failures++;
            next(); S.waitFrames = 3; continue;
        }
        if (!strcmp(cmd, "page")) {
            int p = -1;
            for (int i = 0; i < PG_COUNT; i++) if (!strncasecmp(app_page_title(i), arg, strlen(arg))) { p = i; break; }
            if (p < 0) fail("no page '%s'", arg); else app_go((Page)p);
            next(); S.waitFrames = 3; continue;
        }
        if (!strcmp(cmd, "action")) {                        /* a registry action, as its shortcut would run it */
            const Action *a = actions_find(arg);
            act();
            if (!a) fail("no action '%s'", arg);
            else { const char *why = action_blocked(a); if (why) fail("action %s can't run: %s", arg, why); else { say("action %s", arg); action_run(a, false); } }
            next(); S.waitFrames = 3; continue;
        }
        if (!strcmp(cmd, "flavour")) {                     /* "flavour <event> [detail]": a theme's moment, as a run would raise it */
            setenv("QC_TEST_FLAVOUR", "1", 1);                 /* asked for by name: shown even in a scripted run */
            int ev = atoi(arg); const char *d = strchr(arg, ' ');
            flavour_event(ev, A.cs ? A.cs->s.path : "", d ? d + 1 : NULL);
            next(); S.waitFrames = 10; continue;
        }
        if (!strcmp(cmd, "theme")) { A.set.theme = atoi(arg); ui_set_theme(A.set.theme); next(); S.waitFrames = 3; continue; }
        if (!strcmp(cmd, "screenshot")) { shot(arg); say("screenshot %s", arg); next(); return; }
        if (!strcmp(cmd, "dump")) {
            char *t = malloc(1 << 20); int n = ui_text_dump(t, 1 << 20);
            pl_write_file(arg, t, (size_t)n); free(t);
            next(); continue;
        }
        if (!strcmp(cmd, "quit")) { S.at = S.n; break; }
        if (!strcmp(cmd, "shell")) {                      /* change things behind the app's back: an editor, a script */
            S.excuseUntil = S.nFrames + 3;
            char c2[PL_PATH * 2 + 1100], q[PL_PATH + 8];
            char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);   /* no case open: the workspace, never the app's cwd */
            pl_shell_quote(q, sizeof q, A.cs ? A.cs->s.path : ws);
            snprintf(c2, sizeof c2, "cd %s && { %s; }", q, arg);
            int rc = pl_run_wait(c2, NULL, 0);
            say("shell (rc %d): %s", rc, arg);
            next(); continue;
        }
        if (!strcmp(cmd, "coupling")) {                    /* open a coupled simulation folder, as Open folder would */
            char p[PL_PATH]; pl_expand_home(p, sizeof p, arg);
            S.excuseUntil = S.nFrames + 3;
            app_coupling_open(p);
            say("coupling %s -> %s", arg, A.cpl ? "ok" : "FAILED");
            next(); continue;
        }
        if (!strcmp(cmd, "assert")) {
            if (!cond(arg)) fail("assert %s", arg); else say("ok: %s", arg);
            next(); continue;
        }
        if (!strcmp(cmd, "wait-until")) {
            char c[400]; pl_strlcpy(c, arg, sizeof c);
            double to = 30;
            char *tp = strstr(c, " timeout ");
            if (tp) { to = atof(tp + 9); *tp = 0; }
            if (S.phase++ == 0) S.deadline = now_s() + to;
            if (cond(c)) { say("until %s (ok)", c); next(); continue; }
            if (now_s() > S.deadline) { fail("timed out after %.0f s waiting for: %s", to, c); next(); continue; }
            return;
        }
        /* "click? <id>": click it when it is there and enabled, else skip it and the type/enter that follows (a
           widget only some cases have, such as the process count an Allrun leaves disabled) */
        if (!strcmp(cmd, "click?")) {
            Rectangle v, r, vw; bool dis;
            if (!resolve(arg, &v, &r, &vw, &dis) || dis) {
                say("click? %s: not there or disabled, skipped", arg);
                next();
                if (S.at < S.n) {
                    char nx[16] = ""; sscanf(S.line[S.at], "%15s", nx);
                    if (!strcmp(nx, "enter") || !strcmp(nx, "type")) next();
                }
                continue;
            }
            pl_strlcpy(cmd, "click", sizeof cmd);
        }
        if (!strcmp(cmd, "click") || !strcmp(cmd, "dblclick")) {
            Vector2 p;
            if (!target_ready(arg, &p)) return;
            act();
            inp_click(p.x, p.y);
            if (cmd[0] == 'd') { inp_queue_button(2, true); inp_queue_button(3, false); }
            S.clicks += cmd[0] == 'd' ? 2 : 1;
            say("%s %s", cmd, arg);
            next(); S.waitFrames = 2; return;
        }
        if (!strcmp(cmd, "choose")) {
            /* "choose <target> <item>": the target is the first word */
            char tg[128] = ""; int q = 0; sscanf(arg, "%127s%n", tg, &q);
            const char *item = arg + q; while (isspace((unsigned char)*item)) item++;
            Vector2 p;
            if (S.phase == 0) { if (!target_ready(tg, &p)) return; act(); inp_click(p.x, p.y); S.clicks++; S.phase = 1; S.waitFrames = 4; S.deadline = 0; return; }
            char tt[200]; snprintf(tt, sizeof tt, "text:%s", item);
            Rectangle v;
            if (!ui_text_find(item, &v, true)) {
                if (S.deadline == 0) S.deadline = now_s() + 5;
                if (now_s() > S.deadline) { fail("'%s' is not among the choices of %s", item, tg); next(); }
                return;
            }
            inp_click(v.x + v.width / 2, v.y + v.height / 2);
            S.clicks++;
            say("choose %s -> %s", tg, item);
            next(); S.waitFrames = 3; return;
        }
        if (!strcmp(cmd, "type") || !strcmp(cmd, "enter")) {
            act();
            inp_queue_text(0, arg);
            S.keys += (int)strlen(arg);
            if (cmd[0] == 'e') { inp_queue_key(1, KEY_ENTER); S.keys++; }
            say("%s \"%s\"", cmd, arg);
            next(); S.waitFrames = 2; return;
        }
        if (!strcmp(cmd, "key")) {
            char kn[32] = ""; sscanf(arg, "%31s", kn);
            int code = inp_key_code(kn);
            if (!code) { fail("unknown key %s", kn); next(); continue; }
            act();
            if (strstr(arg, " ctrl")) inp_queue_hold(0, KEY_LEFT_CONTROL);
            if (strstr(arg, " shift")) inp_queue_hold(0, KEY_LEFT_SHIFT);
            inp_queue_key(0, code);
            S.keys++;
            say("key %s", arg);
            next(); S.waitFrames = 2; return;
        }
        if (!strcmp(cmd, "wheel")) {
            float dy = 0; int q = 0; sscanf(arg, "%f%n", &dy, &q);
            const char *tg = arg + q; while (isspace((unsigned char)*tg)) tg++;
            Vector2 p = { ui_width() * ui_scale() / 2, ui_height() * ui_scale() / 2 };
            if (*tg && !target_ready(tg, &p)) return;
            inp_queue_move(0, p.x, p.y);
            inp_queue_wheel(0, dy);
            next(); S.waitFrames = 20; return;
        }
        if (!strcmp(cmd, "drag")) {
            char tg[128] = ""; float dx = 0, dy = 0;
            sscanf(arg, "%127s %f %f", tg, &dx, &dy);
            Vector2 p;
            if (!target_ready(tg, &p)) return;
            act();
            inp_drag(p.x, p.y, p.x + dx, p.y + dy);
            S.clicks++;
            next(); S.waitFrames = 12; return;
        }
        fail("unknown command '%s'", cmd);
        next();
    }
    if (S.at >= S.n && !S.done) {
        S.done = true;
        double wall = S.firstAction >= 0 ? now_s() - S.firstAction : 0;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
        bool sanitized = true;                            /* ASan/UBSan runs ~5x slower: frame times say nothing */
#else
        bool sanitized = false;
#endif
        if (sanitized && (S.budgetP95 > 0 || S.budgetMax > 0)) say("budget not checked: sanitizer build");
        else if ((S.budgetP95 > 0 || S.budgetMax > 0) && S.nFrames > 1) {
            static double b[1 << 16]; int nb = 0, worst = -1;
            for (int i = 1; i < S.nFrames; i++) if (!S.excused[i]) { if (worst < 0 || S.frameMs[i] > S.frameMs[worst]) worst = i; b[nb++] = S.frameMs[i]; }
            if (nb > 0) {
                qsort(b, (size_t)nb, sizeof b[0], cmpd);
                double bp95 = b[(int)(nb * 0.95)], bmax = b[nb - 1];
                if (S.budgetP95 > 0 && bp95 > S.budgetP95) { say("FAIL budget: p95 %.1f ms > %.1f ms", bp95, S.budgetP95); S.failures++; }
                if (S.budgetMax > 0 && bmax > S.budgetMax) { say("FAIL budget: frame %d took %.0f ms > %.0f ms", worst, bmax, S.budgetMax); S.failures++; }
                if (S.failures == 0) say("ok: budget p95 %.1f ms, max %.0f ms over %d frames", bp95, bmax, nb);
            }
        }
        double sum = 0, p95 = 0, mx = 0;
        if (S.nFrames > 0) {
            for (int i = 0; i < S.nFrames; i++) sum += S.frameMs[i];
            qsort(S.frameMs, (size_t)S.nFrames, sizeof S.frameMs[0], cmpd);
            p95 = S.frameMs[(int)(S.nFrames * 0.95)]; mx = S.frameMs[S.nFrames - 1];
        }
        for (int i = 0; i < S.nSlow; i++) say("slow frame %d: %.0f ms (script line %d)", S.slow[i], S.slowMs[i], S.slowLine[i]);
        say("SUMMARY clicks=%d keystrokes=%d wall=%.1fs failures=%d frames=%d frame_ms_mean=%.2f p95=%.2f max=%.2f", S.clicks, S.keys, wall,
            S.failures, S.nFrames, S.nFrames ? sum / S.nFrames : 0, p95, mx);
    }
}

void script_after_frame(int frame)
{
    (void)frame;
    extern double g_frame_work_ms;
    if (S.active && !S.done && S.nFrames < (int)(sizeof S.frameMs / sizeof S.frameMs[0])) {
        static const void *lastCase;
        if ((const void *)A.cs != lastCase) { lastCase = A.cs; S.excuseUntil = S.nFrames + 3; }   /* a case was opened: its load */
        S.excused[S.nFrames] = S.nFrames < S.excuseUntil;
        S.frameMs[S.nFrames++] = g_frame_work_ms;
        if (g_frame_work_ms > 50 && S.nSlow < 8) { S.slow[S.nSlow] = frame; S.slowMs[S.nSlow] = g_frame_work_ms; S.slowLine[S.nSlow++] = S.at < S.n ? S.lineNo[S.at] : -1; }
    }
    if (!S.shot[0]) return;
    Image im = LoadImageFromScreen();
    ExportImage(im, S.shot);
    UnloadImage(im);
    S.shot[0] = 0;
}
