/* logview.c -- the log console shared by the Run, Coupling and DEME pages (ROADMAP 2.2, 2.9).
 *
 * Tabs of log files; a tool row with search (Enter finds the next match), "First error", and "Fold time steps", which
 * folds each finished time step to its "Time = " line (steps with a warning or an error stay open; click a folded
 * line to open it). Routine lines (solver residual lines, ExecutionTime, Courant) are dimmed. The Commands tab lists
 * every command the app ran for this case, with a copy button that gives it as you would type it in a terminal.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"

enum { LK_TEXT, LK_ROUTINE, LK_INFO, LK_TIME, LK_GOOD, LK_WARN, LK_ERR };

static int line_kind(const char *s, int n)
{
    char tmp[200]; int k = n < 199 ? n : 199;
    memcpy(tmp, s, (size_t)k); tmp[k] = 0;
    const char *t = tmp; while (*t == ' ' || *t == '\t') t++;
    /* "time step continuity errors : sum local = ..." is the per-step mass-conservation diagnostic, not a failure */
    if (pl_starts_with(t, "time step continuity errors")) return LK_ROUTINE;
    if (strstr(tmp, "FATAL") || strstr(tmp, "ERROR") || strstr(tmp, "Q-Continuum: could not") || !strcmp(t, "[stack trace]") ||
        strstr(tmp, "::sigFpe::") || strstr(tmp, "::sigSegv::")) return LK_ERR;
    if (strstr(tmp, "Warning") || strstr(tmp, "WARNING")) return LK_WARN;
    if (pl_starts_with(t, "Time = ")) return LK_TIME;
    if (pl_starts_with(t, "End") || strstr(tmp, "Mesh OK")) return LK_GOOD;
    if (strstr(tmp, "Solving for") || pl_starts_with(t, "ExecutionTime") || pl_starts_with(t, "Courant") ||
        pl_starts_with(t, "deltaT = ") || pl_starts_with(t, "PIMPLE: ") || pl_starts_with(t, "Interface Courant") ||
        pl_starts_with(t, "MULES: ") || pl_starts_with(t, "Phase-") || pl_starts_with(t, "diagonal:") ||
        pl_starts_with(t, "Min/max ") || pl_starts_with(t, "Mean and max Courant")) return LK_ROUTINE;
    return LK_TEXT;
}

static Color kind_color(int k)
{
    switch (k) {
    case LK_ERR: return TH.err;
    case LK_WARN: return TH.warn;
    case LK_TIME: return TH.accent;
    case LK_GOOD: return TH.ok;
    case LK_ROUTINE: return TH.text3;
    default: return TH.text;
    }
}

/* ---------------------------------------------------------------- the rows shown: lines, some folded */
static struct {
    const LogMon *m;
    int nLines, textLen;
    char q[128];
    bool fold;
    int expGen;
    int *row, *hid, nRows, cap;      /* hid: lines folded under this row (> 0), -1 an opened step that can fold again */
    int *match, nMatch, capMatch;    /* rows that match the search */
    int cur;                         /* the current match (-1 none) */
    int firstErr;                    /* line, -1 none */
    char exp[64][32]; int nExp;      /* steps opened by hand, by their "Time = ..." text (lines move when old ones are dropped) */
    int scrollRow;                   /* row to bring into view on the next frame (-1 none) */
    int flashLine; double flashT;
    /* a selection of whole lines, to copy: from selA to selB (log lines), dragged with the mouse or Shift+click */
    const LogMon *selFor; int selA, selB; bool selDrag; float selX0, selY0;
} V = { .cur = -1, .firstErr = -1, .scrollRow = -1, .flashLine = -1, .selA = -1, .selB = -1 };

/* lines a..b of the log as text, newline-ended, for the clipboard */
static void copy_lines(const LogMon *m, int a, int b)
{
    if (a > b) { int t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b >= m->nLines) b = m->nLines - 1;
    Str t; str_init(&t);
    for (int i = a; i <= b; i++) { int n; const char *l = logmon_line(m, i, &n); str_catn(&t, l, (size_t)n); str_cat(&t, "\n"); }
    SetClipboardText(t.s ? t.s : "");
    ui_toast(TOAST_OK, "Copied %d line%s", b - a + 1, b == a ? "" : "s");
    str_free(&t);
}

static char find[128];
static const LogSet *cmdsFor;        /* the console showing its Commands tab */

static void time_key(const LogMon *m, int line, char *out, int cap)
{
    int n; const char *s = logmon_line(m, line, &n);
    while (n > 0 && (*s == ' ' || *s == '\t')) { s++; n--; }
    snprintf(out, (size_t)cap, "%.*s", n < cap - 1 ? n : cap - 1, s);
}

static int exp_find(const char *k) { for (int i = 0; i < V.nExp; i++) if (!strcmp(V.exp[i], k)) return i; return -1; }
static void exp_toggle(const char *k)
{
    int i = exp_find(k);
    if (i >= 0) { V.exp[i][0] = 0; memmove(V.exp[i], V.exp[i + 1], sizeof V.exp[0] * (size_t)(V.nExp - i - 1)); V.nExp--; }
    else {
        if (V.nExp == 64) { memmove(V.exp[0], V.exp[1], sizeof V.exp[0] * 63); V.nExp--; }
        pl_strlcpy(V.exp[V.nExp++], k, sizeof V.exp[0]);
    }
    V.expGen++;
}

static void push_row(int line, int hid)
{
    if (V.nRows == V.cap) {
        V.cap = V.cap ? V.cap * 2 : 4096;
        V.row = realloc(V.row, sizeof(int) * (size_t)V.cap);
        V.hid = realloc(V.hid, sizeof(int) * (size_t)V.cap);
    }
    V.row[V.nRows] = line; V.hid[V.nRows] = hid; V.nRows++;
}

static bool line_has(const LogMon *m, int i, const char *q)
{
    int n; const char *s = logmon_line(m, i, &n);
    char tmp[1024]; int k = n < 1023 ? n : 1023;
    memcpy(tmp, s, (size_t)k); tmp[k] = 0;
    return strcasestr(tmp, q) != NULL;
}

static void rebuild(const LogMon *m, bool fold)
{
    static int lastGen = -1;
    if (m == V.m && m->nLines == V.nLines && m->textLen == V.textLen && !strcmp(find, V.q) && fold == V.fold && lastGen == V.expGen) return;
    bool newLog = m != V.m;
    if (newLog) { V.nExp = 0; V.expGen++; V.flashLine = -1; }
    if (newLog || strcmp(find, V.q)) V.cur = -1;
    V.m = m; V.nLines = m->nLines; V.textLen = m->textLen; V.fold = fold; lastGen = V.expGen;
    pl_strlcpy(V.q, find, sizeof V.q);
    V.nRows = 0; V.nMatch = 0; V.firstErr = -1;
    bool folding = fold && !V.q[0];                 /* a search shows every line */
    int blockStart = -1; bool blockBad = false;     /* the "Time = " step being read */
    for (int i = 0; i < m->nLines; i++) {
        int n; const char *s = logmon_line(m, i, &n);
        int k = line_kind(s, n);
        if (k == LK_ERR && V.firstErr < 0) V.firstErr = i;
        if (!folding) {
            push_row(i, 0);
            if (V.q[0] && line_has(m, i, V.q)) {
                if (V.nMatch == V.capMatch) { V.capMatch = V.capMatch ? V.capMatch * 2 : 256; V.match = realloc(V.match, sizeof(int) * (size_t)V.capMatch); }
                V.match[V.nMatch++] = V.nRows - 1;
            }
            continue;
        }
        if (k == LK_TIME) {
            /* the step before is complete: fold it unless it had trouble or was opened */
            if (blockStart >= 0) {
                char key[32]; time_key(m, blockStart, key, sizeof key);
                int hidden = i - blockStart - 1;
                if (hidden > 0 && !blockBad && exp_find(key) < 0) push_row(blockStart, hidden);
                else { push_row(blockStart, hidden > 0 && !blockBad ? -1 : 0); for (int j = blockStart + 1; j < i; j++) push_row(j, 0); }
            }
            blockStart = i; blockBad = false;
            continue;
        }
        if (blockStart >= 0) { if (k == LK_ERR || k == LK_WARN) blockBad = true; continue; }
        push_row(i, 0);                             /* the header before the first step */
    }
    if (folding && blockStart >= 0) for (int j = blockStart; j < m->nLines; j++) push_row(j, 0);   /* the last step stays open */
}

static int row_of_line(int line)
{
    int lo = 0, hi = V.nRows - 1;                   /* rows are in line order */
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (V.row[mid] <= line) lo = mid; else hi = mid - 1; }
    return lo;
}

/* ---------------------------------------------------------------- Commands */
static bool rec_here(const CmdRec *c, const char *path)
{
    size_t n = strlen(path), m = strlen(c->casePath);
    if (!path[0]) return true;
    return !strcmp(c->casePath, path) || (m > n && !strncmp(c->casePath, path, n) && c->casePath[n] == '/') ||
           (n > m && !strncmp(path, c->casePath, m) && path[m] == '/');
}

static void commands_view(Rectangle body, const LogSet *ls)
{
    static UiScroll sc;
    int idx[CMDLOG_MAX], n = 0;
    for (int i = cmdlog_count() - 1; i >= 0; i--) if (rec_here(cmdlog_get(i), ls->casePath)) idx[n++] = i;   /* newest first */
    if (!n) {
        ui_text_wrap(F_REG, 13, "No commands yet. Each step the app runs for this case (meshing, the solver, scripts) is listed here "
                     "with a copy button, so you can run it yourself in a terminal.", (Rectangle){ body.x + 16, body.y + 14, body.width - 32, 80 },
                     19, TH.text3, true);
        return;
    }
    float top = ui_scroll_begin(&sc, body);
    float x = body.x + 14, w = body.width - 28, y = top + 10;
    /* the newest job as one script */
    const CmdRec *last = cmdlog_get(idx[0]);
    float bw = ui_button_width(ICON_COPY, "Copy the last run as a script");
    ui_text(F_REG, 12.5f, "Commands run for this case, newest first. Copy gives cd, the OpenFOAM environment and the command with its log.",
            x, y + 7, TH.text3);
    if (ui_button(ui_id("cmd-copy-job"), (Rectangle){ x + w - bw, y, bw, 30 }, ICON_COPY, "Copy the last run as a script", BTN_GHOST)) {
        Str b; str_init(&b);
        str_cat(&b, "#!/bin/bash\n# Q-Continuum: ");
        str_cat(&b, last->label);
        str_cat(&b, " and the steps after it, as the app ran them\nset -e\n");
        bool first = true;
        for (int i = 0; i < cmdlog_count(); i++) {
            const CmdRec *c = cmdlog_get(i);
            if (c->jobSeq != last->jobSeq) continue;
            char t[9000]; cmdlog_script(c, first, t, sizeof t);
            str_cat(&b, t); first = false;
        }
        SetClipboardText(b.s);
        str_free(&b);
        ui_toast(TOAST_OK, "Copied the run's commands as a script");
    }
    y += 42;
    int prevJob = -1;
    for (int k = 0; k < n; k++) {
        const CmdRec *c = cmdlog_get(idx[k]);
        if (c->jobSeq != prevJob) {
            if (prevJob >= 0) { DrawRectangleRec((Rectangle){ x, y + 2, w, 1 }, TH.lineSoft); y += 10; }
            prevJob = c->jobSeq;
        }
        /* first lines of the command, mono */
        char shown[600]; int lines = 1;
        { int j = 0; for (const char *p = c->cmd; *p && j < (int)sizeof shown - 4; p++) {
              if (*p == '\n') { if (lines == 3) { memcpy(shown + j, " ...", 4); j += 4; break; } lines++; }
              shown[j++] = *p == '\t' ? ' ' : *p; }
          shown[j] = 0; }
        float rowH = 26 + 18 * lines + 10;
        Rectangle row = { x, y, w, rowH };
        if (ui_hovered(row)) ui_rect(row, 8, TH.hover);
        const char *ic = c->st == JS_RUNNING ? NULL : c->st == JS_DONE ? ICON_CIRCLE_CHECK : c->st == JS_FAILED ? ICON_CIRCLE_X :
                         c->st == JS_STOPPED ? ICON_OCTAGON_X : ICON_CIRCLE_DOT;
        Color icc = c->st == JS_DONE ? TH.ok : c->st == JS_FAILED ? TH.err : c->st == JS_STOPPED ? TH.warn : TH.accent;
        if (ic) ui_icon(ic, x + 14, y + 15, 15, icc); else ui_spinner(x + 14, y + 15, 6, icc);
        char head[300], when[16], dur[32] = "";
        struct tm tmv; localtime_r(&c->when, &tmv); strftime(when, sizeof when, "%H:%M:%S", &tmv);
        if (c->t1 > 0) pl_duration(dur, sizeof dur, c->t1 - c->t0);
        snprintf(head, sizeof head, "%s  ·  %s%s%s%s%s", c->label, when, dur[0] ? "  ·  " : "", dur,
                 c->st == JS_FAILED ? "  ·  exit code " : "", "");
        if (c->st == JS_FAILED) { size_t L = strlen(head); snprintf(head + L, sizeof head - L, "%d", c->exitCode); }
        if (c->log[0]) { size_t L = strlen(head); snprintf(head + L, sizeof head - L, "  ·  %s", c->log); }
        ui_text_clip(F_MED, 13, head, x + 30, y + 6, w - 80, TH.text);
        float ty = y + 28;
        char *p = shown;
        for (int l = 0; l < lines; l++) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            ui_text_clip(F_MONO, 12, p, x + 30, ty, w - 80, TH.text2);
            ty += 18;
            if (!e) break;
            p = e + 1;
        }
        if (ui_button(ui_idn(ui_id("cmd-copy"), c->seq), (Rectangle){ x + w - 38, y + 6, 30, 30 }, ICON_COPY, NULL, BTN_ICON)) {
            char t[9000]; cmdlog_script(c, true, t, sizeof t);
            SetClipboardText(t);
            ui_toast(TOAST_OK, "Copied: %s", c->label);
        }
        ui_tooltip((Rectangle){ x + w - 38, y + 6, 30, 30 }, "Copy as terminal commands: cd to the case, load OpenFOAM, run this step into its log");
        y += rowH + 4;
    }
    ui_scroll_end(&sc, y - top + 10);
}

/* ---------------------------------------------------------------- the console */
void run_console(Rectangle r, LogSet *ls, int sel)
{
    ui_rect(r, 12, TH.input);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    bool cmds = cmdsFor == ls;
    /* tabs: Commands, then the log files */
    float tx = r.x + 10, ty = r.y + 10;
    {
        Rectangle t = { tx, ty, ui_text_w(F_MED, 12, "Commands") + 40, 28 };
        bool hov = ui_hovered(t);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (cmds) ui_rect(t, 7, TH.selected); else if (hov) ui_rect(t, 7, TH.hover);
        ui_icon(ICON_TERMINAL, tx + 14, ty + 14, 13, cmds ? TH.text : TH.text2);
        ui_text(F_MED, 12, "Commands", tx + 26, ty + 7, cmds ? TH.text : TH.text2);
        ui_register(ui_id("log-cmds"), t, "Commands");
        ui_tooltip(t, "Every command the app ran for this case, to copy");
        if (ui_clicked(t)) { cmdsFor = cmds ? NULL : ls; cmds = !cmds; }
        tx += t.width + 6;
        DrawRectangleRec((Rectangle){ tx, ty + 6, 1, 16 }, TH.lineSoft);
        tx += 7;
    }
    const char *xl = A.logMax ? "Restore" : "Expand", *xi = A.logMax ? ICON_CHEVRON_DOWN : ICON_CHEVRON_UP;
    float xw = ui_button_width(xi, xl);
    float right = 150 + xw + 8;                              /* Follow and Expand on the right of the tabs */
    float avail = r.x + r.width - right - 10 - tx;
    int shownSel = cmds ? -1 : sel;
    /* when the tabs overflow, drop the oldest ones from the left so the selected log stays visible */
    int start = 0;
    if (sel >= 0 && sel < ls->n) {
        for (;;) {
            float need = start > 0 ? 44 : 0;
            for (int i = start; i <= sel; i++) need += ui_text_w(F_MONO, 12, ls->logs[i].name) + 32;
            if (need <= avail || start >= sel) break;
            start++;
        }
    }
    ui_clip_push((Rectangle){ r.x, r.y, r.width - right, 44 });
    if (start > 0) {
        char more[16]; snprintf(more, sizeof more, "+%d", start);
        Rectangle t = { tx, ty, 38, 28 };
        bool hov = ui_hovered(t);
        if (hov) { ui_cursor(MOUSE_CURSOR_POINTING_HAND); ui_rect(t, 7, TH.hover); }
        ui_text_center(F_MED, 12, more, t, TH.text3);
        ui_tooltip(t, "Earlier logs");
        if (ui_clicked(t)) { A.runLog = start - 1; A.runFollow = true; cmdsFor = NULL; }
        tx += 44;
    }
    float tabEnd = r.x + r.width - right;
    for (int i = start; i < ls->n; i++) {
        const LogMon *m = &ls->logs[i];
        float w = ui_text_w(F_MONO, 12, m->name) + 26;
        /* a tab that doesn't fit whole becomes a "+N" for the later logs instead of a clipped stub */
        if (tx + w > tabEnd - (i < ls->n - 1 ? 44 : 0) && i > sel) {   /* leave room for the "+N" when more follow */
            char more[16]; snprintf(more, sizeof more, "+%d", ls->n - i);
            Rectangle t = { tx, ty, 38, 28 };
            if (t.x + t.width > tabEnd) t.x = tabEnd - t.width;
            bool hov = ui_hovered(t);
            if (hov) { ui_cursor(MOUSE_CURSOR_POINTING_HAND); ui_rect(t, 7, TH.hover); }
            ui_text_center(F_MED, 12, more, t, TH.text3);
            ui_tooltip(t, "Later logs");
            if (ui_clicked(t)) { A.runLog = i; A.runFollow = true; cmdsFor = NULL; }
            break;
        }
        Rectangle t = { tx, ty, w, 28 };
        bool on = i == shownSel;
        bool hov = ui_hovered(t);
        if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
        if (on) ui_rect(t, 7, TH.selected); else if (hov) ui_rect(t, 7, TH.hover);
        Color dc = m->fatal ? TH.err : m->ended ? TH.ok : (A.cj->active && difftime(time(NULL), m->mtime) < 5 ? TH.accent : TH.text3);
        DrawCircleV((Vector2){ tx + 11, ty + 14 }, 3, dc);
        ui_text(F_MONO, 12, m->name, tx + 19, ty + 7, on ? TH.text : TH.text2);
        if (ui_clicked(t)) { A.runLog = i; A.runFollow = true; cmdsFor = NULL; }
        tx += w + 6;
    }
    ui_clip_pop();
    if (ui_button(ui_id("log-max"), (Rectangle){ r.x + r.width - 148 - xw, r.y + 9, xw, 30 }, xi, xl, BTN_GHOST)) { A.logMax = !A.logMax; A.chartMax = false; }
    ui_tooltip((Rectangle){ r.x + r.width - 148 - xw, r.y + 9, xw, 30 }, A.logMax ? "Back to the normal layout (Esc)" : "Give the log the whole page");
    if (A.logMax && inp_key_pressed(KEY_ESCAPE) && !ui_any_input_focused() && A.modal == MD_NONE) A.logMax = false;
    bool fol = A.runFollow;
    ui_checkbox(ui_id("log-follow"), (Rectangle){ r.x + r.width - 140, r.y + 12, 120, 24 }, &fol, "Follow");
    A.runFollow = fol;
    DrawRectangleRec((Rectangle){ r.x + 1, r.y + 46, r.width - 2, 1 }, TH.lineSoft);
    if (cmds) { commands_view((Rectangle){ r.x + 4, r.y + 48, r.width - 8, r.height - 52 }, ls); return; }
    if (sel < 0 || sel >= ls->n) {
        ui_text(F_REG, 13, "No logs yet. Each step writes log.<tool> in the case folder.", r.x + 20, r.y + 62, TH.text3);
        return;
    }
    LogMon *m = &ls->logs[sel];

    /* tool row: search, first error, folding */
    float by = r.y + 53, bx = r.x + 12;
    float fw = fminf(260, r.width * 0.3f);
    UiId fid = ui_id("log-find");
    ui_input(fid, (Rectangle){ bx, by, fw, 30 }, find, sizeof find, "Search the log", INPUT_LIVE | INPUT_SEARCH | INPUT_MONO | INPUT_KEEP);
    rebuild(m, A.set.logFold);
    bx += fw + 8;
    int step = 0;
    if (find[0]) {
        char cnt[48];
        if (!V.nMatch) snprintf(cnt, sizeof cnt, "No matches");
        else if (V.cur >= 0) snprintf(cnt, sizeof cnt, "%d of %d", V.cur + 1, V.nMatch);
        else snprintf(cnt, sizeof cnt, "%d found", V.nMatch);
        ui_text(F_REG, 12.5f, cnt, bx, by + 8, V.nMatch ? TH.text2 : TH.warn);
        bx += ui_text_w(F_REG, 12.5f, cnt) + 8;
        ui_disable(!V.nMatch);
        if (ui_button(ui_id("log-find-prev"), (Rectangle){ bx, by, 30, 30 }, ICON_CHEVRON_UP, NULL, BTN_ICON)) step = -1;
        ui_tooltip((Rectangle){ bx, by, 30, 30 }, "Previous match (Shift+Enter)");
        if (ui_button(ui_id("log-find-next"), (Rectangle){ bx + 32, by, 30, 30 }, ICON_CHEVRON_DOWN, NULL, BTN_ICON)) step = 1;
        ui_tooltip((Rectangle){ bx + 32, by, 30, 30 }, "Next match (Enter)");
        ui_disable(false);
        if (ui_input_entered(fid)) step = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT) ? -1 : 1;
    }
    if (step && V.nMatch) {
        V.cur = V.cur < 0 ? (step > 0 ? 0 : V.nMatch - 1) : (V.cur + step + V.nMatch) % V.nMatch;
        V.scrollRow = V.match[V.cur];
        A.runFollow = false;
    }
    float rx = r.x + r.width - 12;
    float fdw = ui_text_w(F_REG, 13, "Fold time steps") + 34;
    bool fold = A.set.logFold;
    ui_disable(find[0] != 0);
    if (ui_checkbox(ui_id("log-fold"), (Rectangle){ rx - fdw, by + 3, fdw, 24 }, &fold, "Fold time steps")) {
        A.set.logFold = fold; settings_save(&A.set); rebuild(m, fold);
    }
    ui_disable(false);
    ui_tooltip((Rectangle){ rx - fdw, by + 3, fdw, 24 }, find[0] ? "Searching shows every line" :
               "Show each finished time step as its Time = line; steps with a warning or an error stay open");
    rx -= fdw + 12;
    if (V.selFor != m) { V.selFor = m; V.selA = V.selB = -1; V.selDrag = false; }
    {
        bool sel = V.selA >= 0;
        const char *cl = sel ? "Copy lines" : "Copy log";
        float cw = ui_button_width(ICON_COPY, cl);
        if (ui_button(ui_id("log-copy"), (Rectangle){ rx - cw, by, cw, 30 }, ICON_COPY, cl, BTN_GHOST)) {
            if (sel) copy_lines(m, V.selA, V.selB); else copy_lines(m, 0, m->nLines - 1);
        }
        ui_tooltip((Rectangle){ rx - cw, by, cw, 30 }, sel ? "Copy the selected lines (Ctrl+C)" :
                   "Copy the whole log. Drag over lines (or click one, Shift+click another) to copy just those");
        rx -= cw + 8;
    }
    float ew = ui_button_width(ICON_TRIANGLE_ALERT, "First error");
    ui_disable(V.firstErr < 0);
    if (ui_button(ui_id("log-err"), (Rectangle){ rx - ew, by, ew, 30 }, ICON_TRIANGLE_ALERT, "First error", BTN_GHOST)) {
        V.scrollRow = row_of_line(V.firstErr);
        V.flashLine = V.firstErr; V.flashT = GetTime();
        A.runFollow = false;
    }
    ui_disable(false);
    ui_tooltip((Rectangle){ rx - ew, by, ew, 30 }, V.firstErr < 0 ? "No error lines in this log" : "Scroll to the first line that reports an error");

    Rectangle body = { r.x + 4, r.y + 90, r.width - 8, r.height - 94 };
    DrawRectangleRec((Rectangle){ r.x + 1, r.y + 88, r.width - 2, 1 }, TH.lineSoft);
    UiScroll *sc = &A.scr[12];
    float lh = 18;
    float total = V.nRows * lh + 16;
    float maxY = fmaxf(0, total - body.height);
    if (A.runFollow) { sc->target = maxY; sc->y = maxY; }
    if (V.scrollRow >= 0) {                          /* a third of the way down, so the lines before it show too */
        float t = fmaxf(0, fminf(maxY, V.scrollRow * lh - body.height / 3));
        sc->target = t; sc->y = t;
        V.scrollRow = -1;
    }
    float top = ui_scroll_begin(sc, body);
    int first = (int)((body.y - top) / lh) - 1, last = first + (int)(body.height / lh) + 3;
    if (first < 0) first = 0;
    if (last > V.nRows) last = V.nRows;
    int curRow = V.cur >= 0 && V.cur < V.nMatch ? V.match[V.cur] : -1;
    int mi = 0;                                      /* matches are sorted: walk them with the rows */
    if (V.nMatch) { int lo = 0, hi = V.nMatch; while (lo < hi) { int mid = (lo + hi) / 2; if (V.match[mid] < first) lo = mid + 1; else hi = mid; } mi = lo; }
    double flash = V.flashLine >= 0 ? GetTime() - V.flashT : 99;
    if (flash < 2.0) ui_request_frames(2);
    /* selecting: press on a line, drag to another (the view scrolls at the edges); Shift+click extends; Ctrl+C, Ctrl+A */
    {
        Vector2 mp = ui_mouse();
        int row = (int)((mp.y - (top + 8)) / lh);
        if (row < 0) row = 0;
        if (row >= V.nRows) row = V.nRows - 1;
        int line = V.nRows > 0 ? V.row[row] : -1;
        if (ui_pressed(body) && line >= 0) {
            bool shift = inp_key_down(KEY_LEFT_SHIFT) || inp_key_down(KEY_RIGHT_SHIFT);
            if (shift && V.selA >= 0) V.selB = line;
            else if (V.hid[row] == 0) { V.selA = V.selB = line; V.selDrag = true; V.selX0 = mp.x; V.selY0 = mp.y; }
        }
        if (V.selDrag) {
            if (!ui_mouse_down()) V.selDrag = false;
            else if (line >= 0 && (fabsf(mp.y - V.selY0) > 3 || V.selB != V.selA)) {
                V.selB = line;
                A.runFollow = false;
                if (mp.y < body.y + 12) { sc->target = fmaxf(0, sc->target - lh); sc->y = sc->target; }
                if (mp.y > body.y + body.height - 12) { sc->target = fminf(maxY, sc->target + lh); sc->y = sc->target; }
                ui_request_frames(2);
            }
        }
        bool ctrl = inp_key_down(KEY_LEFT_CONTROL) || inp_key_down(KEY_RIGHT_CONTROL);
        bool here = ui_hovered(body) || V.selA >= 0;
        if (here && ctrl && inp_key_pressed(KEY_A) && !ui_any_input_focused() && V.nRows && m->nLines) { V.selA = 0; V.selB = m->nLines - 1; A.runFollow = false; }
        if (ctrl && inp_key_pressed(KEY_C) && V.selA >= 0 && !ui_any_input_focused() && A.modal == MD_NONE) copy_lines(m, V.selA, V.selB);
        if (inp_key_pressed(KEY_ESCAPE) && V.selA >= 0 && !ui_any_input_focused()) V.selA = V.selB = -1;
        if (ui_hovered(body)) ui_cursor(MOUSE_CURSOR_IBEAM);
    }
    int sLo = V.selA < V.selB ? V.selA : V.selB, sHi = V.selA < V.selB ? V.selB : V.selA;
    for (int i = first; i < last; i++) {
        int li = V.row[i], n;
        const char *s = logmon_line(m, li, &n);
        float yy = top + 8 + i * lh;
        bool isMatch = mi < V.nMatch && V.match[mi] == i;
        if (isMatch) mi++;
        if (isMatch) ui_rect((Rectangle){ body.x + 6, yy - 1, body.width - 12, lh }, 4, ui_alpha(TH.warn, i == curRow ? 0.38f : 0.16f));
        if (V.selA >= 0 && li >= sLo && li <= sHi) ui_rect((Rectangle){ body.x + 6, yy - 1, body.width - 12, lh }, 3, ui_alpha(TH.accent, 0.28f));
        if (li == V.flashLine && flash < 2.0) ui_rect((Rectangle){ body.x + 6, yy - 1, body.width - 12, lh }, 4, ui_alpha(TH.err, 0.3f * (float)(1 - flash / 2)));
        char line[400]; int k = n < 399 ? n : 399;
        memcpy(line, s, (size_t)k); line[k] = 0;
        for (char *q = line; *q; q++) if (*q == '\t') *q = ' ';
        float lx = body.x + 14;
        if (V.hid[i] != 0) {                         /* a step's Time = line: click to open or fold it */
            Rectangle hit = { body.x + 6, yy - 1, body.width - 12, lh };
            bool hov = ui_hovered(hit);
            if (hov) { ui_cursor(MOUSE_CURSOR_POINTING_HAND); ui_rect(hit, 4, TH.hover); }
            ui_icon(V.hid[i] > 0 ? ICON_CHEVRON_RIGHT : ICON_CHEVRON_DOWN, lx + 5, yy + 8, 12, TH.text3);
            lx += 16;
            ui_text(F_MONO, 12.5f, line, lx, yy, kind_color(line_kind(s, n)));
            if (V.hid[i] > 0) {
                char more[48]; snprintf(more, sizeof more, "%d lines", V.hid[i]);
                ui_text(F_REG, 12, more, lx + ui_text_w(F_MONO, 12.5f, line) + 14, yy + 1, TH.text3);
            }
            if (ui_clicked(hit)) { char key[32]; time_key(m, li, key, sizeof key); exp_toggle(key); A.runFollow = false; }
            continue;
        }
        ui_text(F_MONO, 12.5f, line, lx, yy, kind_color(line_kind(s, n)));
    }
    if (ui_hovered(body) && inp_wheel() > 0) A.runFollow = false;
    ui_scroll_end(sc, total);
}
