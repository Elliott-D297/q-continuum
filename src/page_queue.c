/* page_queue.c -- the run queue: cases waiting to run one after another (or side by side) as cores free up.
 * Kept in <config>/q-continuum/queue.ini, so a queue survives a restart of the app. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"

#define MAX_QUEUE 64

typedef struct {
    char   path[PL_PATH];
    int    mode;                  /* -1 the case's default, 0 built-in steps, 1 Allrun */
    int    procs;
    bool   cont;                  /* continue from the latest time */
    bool   fresh;                 /* start over: old results to the trash */
    time_t added;
    bool   failed;                /* could not start: stays until removed or retried */
} QItem;

static QItem Q[MAX_QUEUE];
static int nQ;
static bool loaded;
static double lastTry;

static void queue_path(char *out, size_t cap)
{
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(out, cap, "%s/q-continuum/queue.ini", x);
    else snprintf(out, cap, "%s/.config/q-continuum/queue.ini", pl_home());
}

static void queue_save(void)
{
    if (g_settings_readonly) return;
    char p[PL_PATH], d[PL_PATH]; queue_path(p, sizeof p);
    pl_dirname(d, sizeof d, p); pl_mkdirs(d);
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum run queue: path|mode|processes|continue|fresh|added|failed\n");
    for (int i = 0; i < nQ; i++)
        str_catf(&b, "item=%s|%d|%d|%d|%d|%lld|%d\n", Q[i].path, Q[i].mode, Q[i].procs, Q[i].cont, Q[i].fresh, (long long)Q[i].added, Q[i].failed);
    pl_write_file(p, b.s ? b.s : "", b.len);
    str_free(&b);
}

static void queue_load(void)
{
    if (loaded) return;
    loaded = true;
    char p[PL_PATH]; queue_path(p, sizeof p);
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    for (char *l = strtok(t, "\n"); l && nQ < MAX_QUEUE; l = strtok(NULL, "\n")) {
        if (!pl_starts_with(l, "item=")) continue;
        char *f[7] = { 0 }; int k = 0;
        for (char *q = l + 5; k < 7; k++) { f[k] = q; char *bar = strchr(q, '|'); if (!bar) { k++; break; } *bar = 0; q = bar + 1; }
        if (k < 6 || !case_is_project(f[0])) continue;
        QItem *it = &Q[nQ++];
        memset(it, 0, sizeof *it);
        pl_strlcpy(it->path, f[0], sizeof it->path);
        it->mode = atoi(f[1]); it->procs = atoi(f[2]); it->cont = atoi(f[3]); it->fresh = atoi(f[4]);
        it->added = (time_t)atoll(f[5]); it->failed = k > 6 && f[6] ? atoi(f[6]) : 0;
    }
    free(t);
}

int app_queue_count(void) { queue_load(); int n = 0; for (int i = 0; i < nQ; i++) if (!Q[i].failed) n++; return n; }

bool app_queue_has(const char *path)
{
    queue_load();
    for (int i = 0; i < nQ; i++) if (!strcmp(Q[i].path, path)) return true;
    return false;
}

void app_queue_add(const char *path, int mode, int procs, bool cont, bool fresh)
{
    queue_load();
    if (app_queue_has(path)) { ui_toast(TOAST_INFO, "%s is already in the queue", pl_basename(path)); return; }
    if (nQ >= MAX_QUEUE) { ui_toast(TOAST_WARN, "The queue is full (%d runs)", MAX_QUEUE); return; }
    QItem *it = &Q[nQ++];
    memset(it, 0, sizeof *it);
    pl_strlcpy(it->path, path, sizeof it->path);
    it->mode = mode; it->procs = procs; it->cont = cont; it->fresh = fresh;
    it->added = time(NULL);
    queue_save();
    int ahead = nQ - 1;
    ui_toast(TOAST_OK, "Queued %s%s", pl_basename(path), ahead ? "" : ": it starts as soon as there are cores for it");
    lastTry = 0;
    app_rescan_cases(true);
}

static void queue_remove(int i)
{
    if (i < 0 || i >= nQ) return;
    memmove(&Q[i], &Q[i + 1], sizeof Q[0] * (size_t)(nQ - i - 1));
    nQ--;
    queue_save();
}

static int queue_budget(void) { return A.set.queueCores > 0 ? A.set.queueCores : A.env.nCpu; }

/* first in, first out: the head starts when its processes fit in the cores left (or nothing runs at all, so a run
   bigger than the budget still gets its turn). A case that is running already waits. */
void queue_tick(double now)
{
    queue_load();
    {   /* the themes: a queue that ran several cases and is now empty is a finished sweep (Severance's waffle party) */
        static int was = -1;
        bool busy = false;
        for (int k = 0; k <= MAX_JOBS; k++) if (A.jobs[k].active) busy = true;
        if (was > 0 && nQ == 0 && !busy) { flavour_queue_progress(0, true); was = 0; }
        else if (nQ > 0) was = nQ;
    }
    if (A.set.queuePaused || nQ == 0 || now - lastTry < 1.0 || A.modal != MD_NONE) return;
    lastTry = now;
    int head = -1;
    for (int i = 0; i < nQ; i++) if (!Q[i].failed) { head = i; break; }
    if (head < 0) return;
    QItem *it = &Q[head];
    for (int k = 0; k <= MAX_JOBS; k++) if (A.jobs[k].active && !strcmp(A.jobs[k].casePath, it->path)) return;
    if (app_jobs_active() >= MAX_JOBS) return;
    int need = it->procs > 0 ? it->procs : 1;
    if (app_jobs_active() > 0 && app_jobs_procs() + need > queue_budget()) return;
    if (!app_bashrc()[0]) return;
    if (!run_queue_ready(it->path)) { lastTry = now - 0.8; return; }   /* asked in the background; try again shortly */
    QItem copy = *it;
    queue_remove(head);                                        /* off the queue before starting: no double start */
    flavour_queue_progress(1, false);
    if (!run_start_queued(copy.path, copy.mode, copy.procs, copy.cont, copy.fresh)) {
        copy.failed = true;
        if (nQ < MAX_QUEUE) { memmove(&Q[1], &Q[0], sizeof Q[0] * (size_t)nQ); Q[0] = copy; nQ++; queue_save(); }
    }
    app_rescan_cases(true);
}

/* ============================================================================ page */
static const char *item_desc(const QItem *it, char *buf, int cap)
{
    const char *how = it->cont ? "continue from the latest time" : it->fresh ? "start over (old results to the trash)" : "fresh run";
    const char *mode = it->mode == 1 ? "Allrun script" : it->mode == 0 ? "built-in steps" : "case default";
    snprintf(buf, (size_t)cap, "%s  ·  %d process%s  ·  %s", it->cont ? "built-in steps" : mode, it->procs, it->procs == 1 ? "" : "es", how);
    return buf;
}

void page_queue(Rectangle r)
{
    queue_load();
    UiScroll *sc = &A.scr[23];
    float top = ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX), y = top + 30;
    y += page_header(x, y, w, "Queue", "Runs wait here and start on their own, oldest first, when there are cores for them. "
                     "Add a case from its Run page (Add to queue). The queue keeps going while Q-Continuum is open, and is kept when it closes.");
    /* controls */
    float bx = x;
    const char *pl = A.set.queuePaused ? "Resume the queue" : "Pause the queue";
    float bw = ui_button_width(A.set.queuePaused ? ICON_PLAY : ICON_PAUSE, pl);
    if (ui_button(ui_id("q-pause"), (Rectangle){ bx, y, bw, 36 }, A.set.queuePaused ? ICON_PLAY : ICON_PAUSE, pl, A.set.queuePaused ? BTN_PRIMARY : BTN_SECONDARY)) {
        A.set.queuePaused = !A.set.queuePaused; settings_save(&A.set); lastTry = 0;
    }
    bx += bw + 24;
    ui_text(F_MED, 13.5f, "Cores for queued runs", bx, y + 9, TH.text2);
    bx += ui_text_w(F_MED, 13.5f, "Cores for queued runs") + 12;
    char cv[16]; snprintf(cv, sizeof cv, "%d", queue_budget());
    if (ui_input(ui_id("q-cores"), (Rectangle){ bx, y, 70, 36 }, cv, sizeof cv, NULL, INPUT_NUMERIC)) {
        int v = atoi(cv); if (v >= 1) { A.set.queueCores = v >= A.env.nCpu ? 0 : v; settings_save(&A.set); }
    }
    char ct[64]; snprintf(ct, sizeof ct, "of %d. Runs in use: %d process%s.", A.env.nCpu, app_jobs_procs(), app_jobs_procs() == 1 ? "" : "es");
    ui_text(F_REG, 13, ct, bx + 80, y + 9, TH.text3);
    y += 56;
    if (A.cs && !app_queue_has(A.cs->s.path) && !(A.cj->active && !strcmp(A.cj->casePath, A.cs->s.path))) {
        extern int run_procs_for_check(void), run_mode_for_queue(void);
        char lab[300]; snprintf(lab, sizeof lab, "Add %s", A.cs->s.name);
        bw = ui_button_width(ICON_PLUS, lab);
        if (ui_button(ui_id("q-add-open"), (Rectangle){ x, y, bw, 34 }, ICON_PLUS, lab, BTN_SECONDARY)) {
            if (case_has_results(&A.cs->s)) A.modal = MD_QUEUE_ADD;
            else app_queue_add(A.cs->s.path, run_mode_for_queue(), run_procs_for_check(), false, false);
        }
        ui_tooltip((Rectangle){ x, y, bw, 34 }, "With the run settings on its Run page");
        y += 50;
    }

    /* running now */
    y += section_head(x, y, w, "Running now", NULL);
    int nrun = 0;
    for (int k = 0; k <= MAX_JOBS; k++) {
        Job *j = &A.jobs[k];
        if (!j->active) continue;
        nrun++;
        Rectangle row = { x, y, w, 52 };
        ui_rect(row, 10, TH.accentSoft);
        ui_spinner(row.x + 22, row.y + 26, 7, TH.accent);
        ui_text_clip(F_MED, 14, pl_basename(j->casePath), row.x + 44, row.y + 8, w - 300, TH.text);
        char d[32], t[200]; pl_duration(d, sizeof d, GetTime() - j->startedAt);
        snprintf(t, sizeof t, "%s  ·  %d process%s  ·  %s", j->cur < j->n ? j->steps[j->cur].label : j->title, j->procs, j->procs == 1 ? "" : "es", d);
        ui_text_clip(F_REG, 12, t, row.x + 44, row.y + 29, w - 300, TH.text2);
        bw = ui_button_width(ICON_ACTIVITY, "Open");
        if (ui_button(ui_idn(ui_id("q-open-run"), k), (Rectangle){ row.x + w - bw - 10, row.y + 9, bw, 34 }, ICON_ACTIVITY, "Open", BTN_GHOST)) {
            if (case_is_project(j->casePath)) app_open_case(j->casePath, PG_RUN);
        }
        y += 58;
    }
    if (!nrun) { ui_text(F_REG, 13, "Nothing is running.", x, y, TH.text3); y += 30; }
    y += 18;

    /* waiting */
    char head[64]; snprintf(head, sizeof head, "Waiting (%d)", nQ);
    y += section_head(x, y, w, head, A.set.queuePaused && nQ ? "The queue is paused: nothing new starts until you resume it." : NULL);
    if (!nQ) { ui_text(F_REG, 13, "The queue is empty.", x, y, TH.text3); y += 30; }
    int move = -1, dir = 0, del = -1, retry = -1;
    for (int i = 0; i < nQ; i++) {
        QItem *it = &Q[i];
        Rectangle row = { x, y, w, 58 };
        ui_rect(row, 10, TH.surface);
        ui_rect_lines(row, 10, 1, it->failed ? TH.err : TH.lineSoft);
        char num[16]; snprintf(num, sizeof num, "%d", i + 1);
        ui_text(F_SEMI, 16, num, row.x + 16, row.y + 18, TH.text3);
        ui_text_clip(F_MED, 14, pl_basename(it->path), row.x + 48, row.y + 9, w - 330, TH.text);
        char t[300];
        if (it->failed) snprintf(t, sizeof t, "Could not start (the notifications said why). Fix the case, then Retry.");
        else item_desc(it, t, sizeof t);
        ui_text_clip(F_REG, 12, t, row.x + 48, row.y + 31, w - 330, it->failed ? TH.err : TH.text3);
        float rx = row.x + w - 10;
        rx -= 34;
        if (ui_button(ui_idn(ui_id("q-del"), i), (Rectangle){ rx, row.y + 12, 34, 34 }, ICON_X, NULL, BTN_ICON)) del = i;
        ui_tooltip((Rectangle){ rx, row.y + 12, 34, 34 }, "Take it off the queue");
        if (it->failed) {
            bw = ui_button_width(ICON_REFRESH_CW, "Retry");
            rx -= bw + 6;
            if (ui_button(ui_idn(ui_id("q-retry"), i), (Rectangle){ rx, row.y + 12, bw, 34 }, ICON_REFRESH_CW, "Retry", BTN_SECONDARY)) retry = i;
        }
        rx -= 40;
        ui_disable(i == nQ - 1);
        if (ui_button(ui_idn(ui_id("q-down"), i), (Rectangle){ rx, row.y + 12, 34, 34 }, ICON_CHEVRON_DOWN, NULL, BTN_ICON)) { move = i; dir = 1; }
        ui_disable(i == 0);
        rx -= 38;
        if (ui_button(ui_idn(ui_id("q-up"), i), (Rectangle){ rx, row.y + 12, 34, 34 }, ICON_CHEVRON_UP, NULL, BTN_ICON)) { move = i; dir = -1; }
        ui_disable(false);
        rx -= 70;
        if (ui_button(ui_idn(ui_id("q-open"), i), (Rectangle){ rx, row.y + 12, 64, 34 }, NULL, "Open", BTN_GHOST)) app_open_case(it->path, PG_RUN);
        y += 64;
    }
    if (move >= 0) { QItem t = Q[move]; Q[move] = Q[move + dir]; Q[move + dir] = t; queue_save(); }
    if (retry >= 0) { Q[retry].failed = false; queue_save(); lastTry = 0; }
    if (del >= 0) { queue_remove(del); app_rescan_cases(true); }
    ui_scroll_end(sc, y - top + 40);
}
