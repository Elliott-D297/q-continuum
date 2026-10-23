/* page_history.c -- History: the setup's change history (git), snapshots of the whole case, and the trash that
 * Clean and "Delete and start over" move results into */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "safety.h"
#include "runlog.h"

#define MAX_HIST 300
#define MAX_TRASH 64
#define MAX_SNAPS 64

/* what the page shows, read again when the case or the data changes (lists, sizes, git output) */
static struct {
    char      path[PL_PATH];
    double    readAt;
    HistEntry hist[MAX_HIST]; int nHist;
    TrashEntry trash[MAX_TRASH]; int nTrash;
    Snapshot  snaps[MAX_SNAPS]; int nSnaps;
    int       sel;                       /* selected revision, -1 = none */
    char      shownRev[48];
    bool      shownVsNow;                /* the diff shows rev -> now instead of the change rev made */
    char     *text;                      /* git output in the right pane */
    int      *lineStart; int nLines, capLines;
    char      lastGood[48]; time_t lastGoodAt;
    long long freeBytes, caseBytes;
    int       snapPending;               /* a snapshot job was started: re-read the list when it ends */
    bool      loading;                   /* the first read for this case is still on its way */
} H = { .sel = -1 };

/* What a read produces. It runs in a forked child (git log per revision, the case's size: 100 ms with 10 000 time
   folders, seconds on a multi-GB case), which writes this plain struct down a pipe; frames go on meanwhile. */
typedef struct {
    HistEntry  hist[MAX_HIST]; int nHist;
    TrashEntry trash[MAX_TRASH]; int nTrash;
    Snapshot   snaps[MAX_SNAPS]; int nSnaps;
    char       lastGood[48]; time_t lastGoodAt;
    long long  freeBytes, caseBytes;
} HistData;

static struct { pid_t pid; int fd; size_t got; bool again; char path[PL_PATH]; HistData *d; } R = { .fd = -1 };

static void read_into(const char *path, HistData *d)
{
    memset(d, 0, sizeof *d);
    d->nHist = hist_list(path, d->hist, MAX_HIST);
    d->nTrash = trash_list(path, d->trash, MAX_TRASH);
    d->nSnaps = snapshot_list(path, d->snaps, MAX_SNAPS);
    hist_last_good_run(path, d->lastGood, sizeof d->lastGood, &d->lastGoodAt);
    struct statvfs vs;
    d->freeBytes = statvfs(path, &vs) == 0 ? (long long)vs.f_bavail * (long long)vs.f_frsize : -1;
    d->caseBytes = pl_dir_size(path, 16) - trash_bytes(path);
    for (int i = 0; i < d->nSnaps; i++) d->caseBytes -= d->snaps[i].bytes;
    if (d->caseBytes < 0) d->caseBytes = 0;
}

static void read_end(void)
{
    if (R.fd >= 0) close(R.fd);
    if (R.pid > 0) { kill(R.pid, SIGKILL); waitpid(R.pid, NULL, 0); }
    R.fd = -1; R.pid = 0; R.got = 0;
}

static void read_start(const char *path)
{
    if (!R.d) R.d = malloc(sizeof *R.d);
    int p[2];
    if (!R.d || pipe(p) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return; }
    if (pid == 0) {                                      /* child: no GL, no UI; just the reads, then _exit */
        close(p[0]);
        read_into(path, R.d);
        const char *b = (const char *)R.d; size_t left = sizeof *R.d;
        while (left > 0) { ssize_t n = write(p[1], b, left); if (n <= 0) _exit(1); b += n; left -= (size_t)n; }
        _exit(0);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    R.pid = pid; R.fd = p[0]; R.got = 0;
    pl_strlcpy(R.path, path, sizeof R.path);
}

/* take in what the child has written so far; the whole struct swaps in at once */
static void read_poll(void)
{
    if (R.fd < 0) return;
    for (;;) {
        ssize_t n = read(R.fd, (char *)R.d + R.got, sizeof *R.d - R.got);
        if (n > 0) { R.got += (size_t)n; if (R.got < sizeof *R.d) continue; }
        if (n == 0 || R.got == sizeof *R.d || (n < 0 && errno != EAGAIN && errno != EINTR)) break;
        return;                                          /* more to come */
    }
    bool ok = R.got == sizeof *R.d && !strcmp(R.path, H.path);
    if (ok) {
        HistData *d = R.d;
        memcpy(H.hist, d->hist, sizeof H.hist); H.nHist = d->nHist;
        memcpy(H.trash, d->trash, sizeof H.trash); H.nTrash = d->nTrash;
        memcpy(H.snaps, d->snaps, sizeof H.snaps); H.nSnaps = d->nSnaps;
        pl_strlcpy(H.lastGood, d->lastGood, sizeof H.lastGood); H.lastGoodAt = d->lastGoodAt;
        H.freeBytes = d->freeBytes; H.caseBytes = d->caseBytes;
        H.loading = false;
        if (H.sel >= H.nHist) H.sel = -1;
    }
    read_end();
    if (R.again) { R.again = false; H.readAt = GetTime(); read_start(H.path); }   /* asked again while this one ran */
}

void history_invalidate(void) { H.readAt = 0; }

static void set_text(char *t)
{
    free(H.text); H.text = t; H.nLines = 0;
    if (!t) return;
    for (char *p = t; ; ) {
        if (H.nLines == H.capLines) { H.capLines = H.capLines ? H.capLines * 2 : 1024; H.lineStart = realloc(H.lineStart, sizeof(int) * (size_t)H.capLines); }
        H.lineStart[H.nLines++] = (int)(p - t);
        char *e = strchr(p, '\n');
        if (!e) break;
        p = e + 1;
        if (!*p) break;
    }
}

static void refresh(bool force)
{
    Case *c = A.cs;
    read_poll();
    if (!force && !strcmp(H.path, c->s.path) && GetTime() - H.readAt < 20) return;
    if (strcmp(H.path, c->s.path)) {                     /* another case: forget the last one's lists at once */
        H.sel = -1; H.shownRev[0] = 0; set_text(NULL);
        H.nHist = H.nTrash = H.nSnaps = 0; H.lastGood[0] = 0; H.freeBytes = H.caseBytes = -1;
        H.loading = true;
        read_end();
    }
    pl_strlcpy(H.path, c->s.path, sizeof H.path);
    H.readAt = GetTime();
    if (R.fd >= 0) { R.again = true; return; }           /* one read at a time; this one follows it */
    read_start(c->s.path);
}

static void show_rev(int i, bool vsNow)
{
    if (i < 0 || i >= H.nHist) return;
    H.sel = i;
    pl_strlcpy(H.shownRev, H.hist[i].rev, sizeof H.shownRev);
    H.shownVsNow = vsNow;
    set_text(vsNow ? hist_diff_now(A.cs->s.path, H.hist[i].rev) : hist_show(A.cs->s.path, H.hist[i].rev));
    A.scr[20].target = A.scr[20].y = 0;
}

static Color diff_color(const char *s, int n)
{
    if ((n >= 4 && (!strncmp(s, "+++ ", 4) || !strncmp(s, "--- ", 4))) || (n == 3 && !strncmp(s, "---", 3))) return TH.text2;
    if (n >= 10 && !strncmp(s, "diff --git", 10)) return TH.text;
    if (n >= 2 && !strncmp(s, "@@", 2)) return TH.accent;
    if (n >= 1 && s[0] == '+') return TH.ok;
    if (n >= 1 && s[0] == '-') return TH.err;
    return TH.text3;
}

static void text_pane(Rectangle r)
{
    ui_rect(r, 12, TH.input);
    ui_rect_lines(r, 12, 1, TH.lineSoft);
    Rectangle body = ui_inset(r, 4, 6);
    if (!H.text) {
        ui_text_center(F_REG, 13, "Pick a version on the left to see what it changed.", body, TH.text3);
        return;
    }
    UiScroll *sc = &A.scr[20];
    float lh = 18, total = H.nLines * lh + 16;
    float top = ui_scroll_begin(sc, body);
    int first = (int)((body.y - top) / lh) - 1, last = first + (int)(body.height / lh) + 3;
    if (first < 0) first = 0;
    if (last > H.nLines) last = H.nLines;
    for (int i = first; i < last; i++) {
        const char *s = H.text + H.lineStart[i];
        const char *e = strchr(s, '\n');
        int n = e ? (int)(e - s) : (int)strlen(s);
        char line[400]; int k = n < 399 ? n : 399;
        memcpy(line, s, (size_t)k); line[k] = 0;
        for (char *q = line; *q; q++) if (*q == '\t') *q = ' ';
        ui_text(F_MONO, 12.5f, line, body.x + 12, top + 8 + i * lh, i < 2 && !H.shownVsNow ? TH.text : diff_color(s, n));
    }
    ui_scroll_end(sc, total);
}

/* ============================================================================ tabs */
static const char *run_badge(const char *run, Color *fg, Color *bg)
{
    if (!strcmp(run, "finished")) { *fg = TH.ok; *bg = TH.okSoft; return "run finished"; }
    if (!strcmp(run, "failed")) { *fg = TH.err; *bg = TH.errSoft; return "run failed"; }
    if (!strcmp(run, "stopped") || !strcmp(run, "paused")) { *fg = TH.warn; *bg = TH.warnSoft; return !strcmp(run, "paused") ? "run paused" : "run stopped"; }
    if (!strcmp(run, "early")) { *fg = TH.warn; *bg = TH.warnSoft; return "run ended early"; }
    if (!strcmp(run, "started")) { *fg = TH.accent; *bg = TH.accentSoft; return "run started"; }
    return NULL;
}

static float tab_changes(Rectangle area, float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    if (!hist_available()) {
        const char *m = "Change history needs git. Install it (sudo apt install git) and restart Q-Continuum.";
        float ch = callout_height(w, NULL, m);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, m);
        return ch + 10;
    }
    y += ui_text_wrap(F_REG, 13, "Every save and every run records the case setup (system, constant, 0.orig and the scripts, not meshes or "
                      "results) in .qc-history. Pick a version to see what changed, compare it with the files now, or go back to it.",
                      (Rectangle){ x, y, fminf(w, 860), 80 }, 19, TH.text3, true) + 14;
    float bx = x;
    float bw = ui_button_width(ICON_SAVE, "Record the setup now");
    if (ui_button(ui_id("h-rec"), (Rectangle){ bx, y, bw, 34 }, ICON_SAVE, "Record the setup now", BTN_SECONDARY)) {
        app_save_all(true);
        if (hist_commit(c->s.path, "Recorded by hand")) ui_toast(TOAST_OK, "Recorded the setup");
        else ui_toast(TOAST_INFO, "Nothing changed since the last record");
        refresh(true);
    }
    bx += bw + 10;
    if (H.lastGood[0]) {
        int gi = -1;
        for (int i = 0; i < H.nHist; i++) if (!strcmp(H.hist[i].rev, H.lastGood)) gi = i;
        char lab[96], ago[32]; pl_time_ago(ago, sizeof ago, H.lastGoodAt);
        snprintf(lab, sizeof lab, "Changes since the last good run (%s)", ago);
        bw = ui_button_width(ICON_GIT_FORK, lab);
        ui_disable(gi < 0);
        if (ui_button(ui_id("h-good"), (Rectangle){ bx, y, bw, 34 }, ICON_GIT_FORK, lab, BTN_SECONDARY)) show_rev(gi, true);
        ui_disable(false);
    }
    y += 50;
    float listW = fminf(430, w * 0.42f), paneX = x + listW + 20, paneW = w - listW - 20;
    float bottom = area.y + area.height - 24;
    float h = fmaxf(320, bottom - y);
    /* list */
    Rectangle lr = { x, y, listW, h };
    ui_rect(lr, 12, TH.surface);
    ui_rect_lines(lr, 12, 1, TH.lineSoft);
    if (H.nHist == 0) {
        ui_text_wrap(F_REG, 13, H.loading ? "Reading the history..." : "No history yet. It starts with the next save or run.", (Rectangle){ x + 16, y + 16, listW - 32, 80 }, 19, TH.text3, true);
    } else {
        UiScroll *sc = &A.scr[21];
        float top = ui_scroll_begin(sc, ui_inset(lr, 4, 6));
        float ry = top + 2;
        for (int i = 0; i < H.nHist; i++) {
            HistEntry *e = &H.hist[i];
            Rectangle row = { x + 6, ry, listW - 12, 52 };
            if (ry + 52 > lr.y && ry < lr.y + lr.height) {
                if (ui_row(ui_idn(ui_id("h-row"), i), row, i == H.sel)) show_rev(i, false);
                char ago[32], meta[96]; pl_time_ago(ago, sizeof ago, e->when);
                snprintf(meta, sizeof meta, "%s  ·  %d file%s", ago, e->nFiles, e->nFiles == 1 ? "" : "s");
                ui_text_clip(F_MED, 13.5f, e->subject, row.x + 12, row.y + 8, row.width - 24, TH.text);
                ui_text(F_REG, 12, meta, row.x + 12, row.y + 29, TH.text3);
                Color fg, bg; const char *b = run_badge(e->run, &fg, &bg);
                if (b) ui_badge(row.x + row.width - ui_badge_width(NULL, b) - 8, row.y + 25, NULL, b, fg, bg);
            }
            ry += 54;
        }
        ui_scroll_end(sc, ry - top + 6);
    }
    /* the selected version */
    if (H.sel >= 0 && H.sel < H.nHist) {
        HistEntry *e = &H.hist[H.sel];
        float px = paneX;
        char lab[64]; snprintf(lab, sizeof lab, H.shownVsNow ? "What it changed" : "Compare with now");
        bw = ui_button_width(ICON_GIT_FORK, lab);
        if (ui_button(ui_id("h-cmp"), (Rectangle){ px, y, bw, 34 }, ICON_GIT_FORK, lab, BTN_SECONDARY)) show_rev(H.sel, !H.shownVsNow);
        px += bw + 10;
        bw = ui_button_width(ICON_UNDO_2, "Go back to this version");
        ui_disable(H.sel == 0 && !H.shownVsNow);
        if (ui_button(ui_id("h-restore"), (Rectangle){ px, y, bw, 34 }, ICON_UNDO_2, "Go back to this version", BTN_DANGER)) {
            A.modal = MD_HIST_CONFIRM; A.histAction = 1; pl_strlcpy(A.mText2, e->rev, sizeof A.mText2);
        }
        ui_disable(false);
        char t[200]; snprintf(t, sizeof t, H.shownVsNow ? "%.10s -> the files on disk now" : "%.10s", e->rev);
        ui_text_clip(F_MONO, 12, t, px + bw + 14, y + 10, paneX + paneW - (px + bw + 14), TH.text3);
        text_pane((Rectangle){ paneX, y + 44, paneW, h - 44 });
    } else text_pane((Rectangle){ paneX, y, paneW, h });
    return y + h - y0;
}

static float tab_snapshots(float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    y += ui_text_wrap(F_REG, 13, "A snapshot is a full copy of the case (setup, mesh, results and logs) in .qc-snapshots. Take one before a "
                      "risky change; going back to it puts the case exactly as it was, and what the case held then goes to the trash.",
                      (Rectangle){ x, y, fminf(w, 860), 80 }, 19, TH.text3, true) + 14;
    char sz[32], fr[32], t[200];
    pl_human_size(sz, sizeof sz, H.caseBytes);
    pl_human_size(fr, sizeof fr, H.freeBytes);
    bool room = H.freeBytes < 0 || H.freeBytes > H.caseBytes * 11 / 10;
    if (H.loading || H.caseBytes < 0) snprintf(t, sizeof t, "Measuring the case...");
    else snprintf(t, sizeof t, "A snapshot takes about %s here; %s free on this disk.", sz, fr);
    ui_text(F_REG, 13, t, x, y, room ? TH.text2 : TH.warn);
    y += 28;
    ui_text(F_MED, 13, "Label", x, y, TH.text2);
    y += 22;
    static char label[128];
    ui_input(ui_id("h-snap-label"), (Rectangle){ x, y, fminf(w - 220, 520), 38 }, label, sizeof label, "e.g. before switching to k-omega SST", 0);
    bool busy = A.cj->active;
    ui_disable(busy || !room);
    float bw = ui_button_width(ICON_HARD_DRIVE, "Take snapshot");
    if (ui_button(ui_id("h-snap"), (Rectangle){ x + fminf(w - 220, 520) + 12, y, bw, 38 }, ICON_HARD_DRIVE, "Take snapshot", BTN_PRIMARY)) {
        char id[40], cmd[2048];
        snapshot_new_id(c->s.path, id, sizeof id);
        snapshot_copy_cmd(id, label[0] ? label : "Snapshot", case_latest_result(&c->s), cmd, sizeof cmd);
        app_start_tool("Snapshot", "snapshot", "Snapshot", cmd, "log.qc-snapshot");
        if (A.cj->active) { H.snapPending = 1; label[0] = 0; }
    }
    ui_disable(false);
    if (busy) ui_tooltip((Rectangle){ x + fminf(w - 220, 520) + 12, y, bw, 38 }, "Wait for the run or tool in this case to finish");
    y += 56;
    if (H.nSnaps == 0) { ui_text(F_REG, 13, "No snapshots of this case yet.", x, y, TH.text3); return y + 30 - y0; }
    for (int i = 0; i < H.nSnaps; i++) {
        Snapshot *s = &H.snaps[i];
        Rectangle row = { x, y, w, 62 };
        ui_rect(row, 10, TH.surface);
        ui_rect_lines(row, 10, 1, TH.lineSoft);
        char when[64], ago[32]; struct tm tm; localtime_r(&s->when, &tm); strftime(when, sizeof when, "%d %b %Y %H:%M", &tm);
        pl_time_ago(ago, sizeof ago, s->when);
        ui_icon(ICON_HARD_DRIVE, row.x + 24, row.y + 31, 18, s->complete ? TH.accent : TH.warn);
        ui_text_clip(F_MED, 14, s->label[0] ? s->label : s->id, row.x + 48, row.y + 11, w - 420, TH.text);
        char sb[32]; pl_human_size(sb, sizeof sb, s->bytes);
        snprintf(t, sizeof t, "%s (%s)  ·  %s  ·  %s%s%s", when, ago, sb, s->latest[0] ? "results to t = " : "no results",
                 s->latest, s->complete ? "" : "  ·  INCOMPLETE: the copy did not finish");
        ui_text_clip(F_REG, 12, t, row.x + 48, row.y + 34, w - 330, s->complete ? TH.text3 : TH.warn);
        float bx = row.x + w - 14;
        bw = ui_button_width(ICON_TRASH_2, "Delete");
        bx -= bw;
        if (ui_button(ui_idn(ui_id("h-snap-del"), i), (Rectangle){ bx, row.y + 14, bw, 34 }, ICON_TRASH_2, "Delete", BTN_GHOST)) {
            A.modal = MD_HIST_CONFIRM; A.histAction = 3; pl_strlcpy(A.mText2, s->id, sizeof A.mText2);
        }
        bw = ui_button_width(ICON_UNDO_2, "Go back to this");
        bx -= bw + 8;
        ui_disable(busy || !s->complete);
        if (ui_button(ui_idn(ui_id("h-snap-go"), i), (Rectangle){ bx, row.y + 14, bw, 34 }, ICON_UNDO_2, "Go back to this", BTN_SECONDARY)) {
            A.modal = MD_HIST_CONFIRM; A.histAction = 2; pl_strlcpy(A.mText2, s->id, sizeof A.mText2);
        }
        ui_disable(false);
        y += 70;
    }
    return y - y0;
}

static float tab_trash(float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    char tot[32]; long long all = 0;
    for (int i = 0; i < H.nTrash; i++) all += H.trash[i].bytes;
    pl_human_size(tot, sizeof tot, all);
    char t[400];
    snprintf(t, sizeof t, "Clean, \"Delete and start over\" and clean-up scripts move results here (.qc-trash) instead of deleting them. "
             "The newest %d are kept for up to %d days; older ones are deleted for good. Now: %d entr%s, %s.", TRASH_KEEP, TRASH_DAYS,
             H.nTrash, H.nTrash == 1 ? "y" : "ies", tot);
    y += ui_text_wrap(F_REG, 13, t, (Rectangle){ x, y, fminf(w, 860), 80 }, 19, TH.text3, true) + 14;
    if (H.nTrash) {
        float bw = ui_button_width(ICON_TRASH_2, "Empty the trash");
        if (ui_button(ui_id("h-empty"), (Rectangle){ x, y, bw, 34 }, ICON_TRASH_2, "Empty the trash", BTN_GHOST)) {
            A.modal = MD_HIST_CONFIRM; A.histAction = 5; A.mText2[0] = 0;
        }
        y += 50;
    } else { ui_text(F_REG, 13, "The trash is empty.", x, y, TH.text3); return y + 30 - y0; }
    bool busy = A.cj->active;
    for (int i = 0; i < H.nTrash; i++) {
        TrashEntry *e = &H.trash[i];
        Rectangle row = { x, y, w, 62 };
        ui_rect(row, 10, TH.surface);
        ui_rect_lines(row, 10, 1, TH.lineSoft);
        char when[64], ago[32]; struct tm tm; localtime_r(&e->when, &tm); strftime(when, sizeof when, "%d %b %Y %H:%M", &tm);
        pl_time_ago(ago, sizeof ago, e->when);
        ui_icon(ICON_TRASH_2, row.x + 24, row.y + 31, 18, TH.text2);
        ui_text_clip(F_MED, 14, e->reason, row.x + 48, row.y + 11, w - 420, TH.text);
        char sb[32]; pl_human_size(sb, sizeof sb, e->bytes);
        snprintf(t, sizeof t, "%s (%s)  ·  %d item%s, %s%s%s", when, ago, e->nItems, e->nItems == 1 ? "" : "s", sb,
                 e->latest[0] ? "  ·  results to t = " : "", e->latest);
        ui_text_clip(F_REG, 12, t, row.x + 48, row.y + 34, w - 330, TH.text3);
        float bx = row.x + w - 14;
        float bw = ui_button_width(ICON_X, "Delete for good");
        bx -= bw;
        if (ui_button(ui_idn(ui_id("h-tr-del"), i), (Rectangle){ bx, row.y + 14, bw, 34 }, ICON_X, "Delete for good", BTN_GHOST)) {
            A.modal = MD_HIST_CONFIRM; A.histAction = 4; pl_strlcpy(A.mText2, e->id, sizeof A.mText2);
        }
        bw = ui_button_width(ICON_UNDO_2, "Restore");
        bx -= bw + 8;
        ui_disable(busy || c->s.runningLog[0]);
        if (ui_button(ui_idn(ui_id("h-tr-go"), i), (Rectangle){ bx, row.y + 14, bw, 34 }, ICON_UNDO_2, "Restore", BTN_SECONDARY)) {
            char msg[300];
            bool ok = trash_restore(c->s.path, e->id, msg, sizeof msg);
            case_rescan(c); case_reload_changed(c);
            logset_open(&A.logs, c->s.path);
            A.stepInit = false;
            ui_toast(ok ? TOAST_OK : TOAST_WARN, "%s", msg);
            app_rescan_cases(true);
            refresh(true);
        }
        ui_disable(false);
        if (busy) ui_tooltip((Rectangle){ bx, row.y + 14, bw, 34 }, "Stop the run in this case first");
        y += 70;
    }
    return y - y0;
}

/* ============================================================================ 3.7 CAPTAIN'S LOG: the run records */
#define MAX_RUNS 300
static void show_run(int i);
static struct { char path[PL_PATH]; double readAt; RunRecord *r; int n, sel; } L = { .sel = -1 };

static void runs_refresh(bool force)
{
    Case *c = A.cs;
    runlog_reap();
    bool other = strcmp(L.path, c->s.path) != 0;
    if (!force && !other && GetTime() - L.readAt < 3) return;     /* a few small files: cheap to read again */
    if (!L.r) L.r = malloc(sizeof *L.r * MAX_RUNS);
    if (!L.r) return;
    char keep[40] = "";
    bool newest = L.sel == 0;                                    /* the newest was shown: show the new newest */
    if (!other && L.sel > 0 && L.sel < L.n) pl_strlcpy(keep, L.r[L.sel].id, sizeof keep);
    pl_strlcpy(L.path, c->s.path, sizeof L.path);
    L.readAt = GetTime();
    L.n = runlog_list(c->s.path, L.r, MAX_RUNS);
    L.sel = -1;
    for (int i = 0; i < L.n; i++) if (keep[0] && !strcmp(L.r[i].id, keep)) L.sel = i;
    if (newest && L.n > 0) show_run(0);
}

static const char *outcome_badge(const char *o, Color *fg, Color *bg)
{
    *fg = TH.text2; *bg = TH.lineSoft;                           /* an outcome run_badge doesn't know: plain */
    if (!strcmp(o, "converged")) { *fg = TH.ok; *bg = TH.okSoft; return "converged"; }
    if (!strcmp(o, "paused")) { *fg = TH.warn; *bg = TH.warnSoft; return "paused"; }
    const char *b = run_badge(o, fg, bg);
    return b ? b + 4 : o;                                        /* "run finished" -> "finished" */
}

/* the selected record as text: what ran, how it ended, the numbers, then the setup change since the run before */
static void show_run(int i)
{
    if (i < 0 || i >= L.n) return;
    L.sel = i;
    const RunRecord *r = &L.r[i];
    Str b; str_init(&b);
    char when[64], sum[200]; struct tm tm; localtime_r(&r->started, &tm);
    strftime(when, sizeof when, "%d %b %Y %H:%M", &tm);
    runlog_summary(r, sum, sizeof sum);
    str_catf(&b, "Run of %s: %s\n%s, %s\n\n", when, r->outcome, sum, r->version[0] ? r->version : "OpenFOAM");
    if (r->reason[0]) str_catf(&b, "Why: %s\n", r->reason);
    str_catf(&b, "Reached t = %g of endTime %g", r->reached, r->endTime);
    if (r->exec > 0) str_catf(&b, "  (ExecutionTime %.1f s", r->exec);
    if (r->exec > 0 && r->wall > 0 && r->reached > 0) str_catf(&b, ", %.3g simulated s per wall s", r->reached / r->wall);
    if (r->exec > 0) str_cat(&b, ")");
    str_cat(&b, "\n");
    if (r->nonOrtho >= 0) str_catf(&b, "Mesh: max non-orthogonality %.1f deg, max skewness %.2f (log.checkMesh)\n", r->nonOrtho, r->skew);
    if (r->courant >= 0) str_catf(&b, "Max Courant number, last step: %.3g\n", r->courant);
    str_cat(&b, "\n");
    char *d = r->hasDiff ? runlog_diff(A.cs->s.path, r->id) : NULL;
    if (d && d[0]) { str_cat(&b, "Setup changes since the run before\n\n"); str_cat(&b, d); }
    else if (r->prevRev[0] && r->rev[0]) str_cat(&b, "The setup was the same as for the run before.\n");
    else if (!r->prevRev[0]) str_cat(&b, "The first recorded run of this case.\n");
    else str_cat(&b, "No setup history for this run (Settings > History was off).\n");
    if (r->nRes) {
        str_cat(&b, "\nFinal initial residuals\n");
        for (int k = 0; k < r->nRes; k++) str_catf(&b, "  %-24s %.3e\n", r->resName[k], r->resVal[k]);
    }
    if (r->nSteps) {
        str_cat(&b, "\nSteps\n");
        for (int k = 0; k < r->nSteps; k++) str_catf(&b, "  %-24s %-8s %6.1f s\n", r->stepLabel[k], r->stepState[k], r->stepSec[k]);
    }
    free(d);
    set_text(b.s);                                               /* set_text takes the buffer */
    H.shownVsNow = false;
    A.scr[20].target = A.scr[20].y = 0;
}

static float tab_runlog(Rectangle area, float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    runs_refresh(false);
    if (L.sel < 0 && L.n > 0) show_run(0);
    if (L.n == 0 && H.text && !H.shownRev[0]) set_text(NULL);
    y += ui_text_wrap(F_REG, 13, "Every run of this case is recorded in .qcontinuum/log inside the case, so the record travels with it: the "
                      "solver, processes, mesh, how far it got, the final residuals, how it ended and the setup change since the run before.",
                      (Rectangle){ x, y, fminf(w, 860), 80 }, 19, TH.text3, true) + 14;
    float bx = x;
    float bw = ui_button_width(ICON_COPY, "Export as Markdown");
    ui_disable(L.n == 0);
    if (ui_button(ui_id("rl-export"), (Rectangle){ bx, y, bw, 34 }, ICON_COPY, "Export as Markdown", BTN_SECONDARY)) {
        char *md = runlog_markdown(c->s.path, c->s.name);
        char p[PL_PATH];
        if (md && pl_fmt(p, sizeof p, "%s/.qcontinuum/run-log.md", c->s.path) && pl_write_file(p, md, strlen(md))) {
            SetClipboardText(md);
            ui_toast(TOAST_OK, "Copied %d run%s as Markdown; also saved to .qcontinuum/run-log.md", L.n, L.n == 1 ? "" : "s");
        } else ui_toast(TOAST_ERR, "Couldn't write .qcontinuum/run-log.md");
        free(md);
    }
    ui_disable(false);
    ui_tooltip((Rectangle){ bx, y, bw, 34 }, "A table of the runs and a section per run, for a lab notebook or a thesis methods section");
    y += 50;
    float listW = fminf(430, w * 0.42f), paneX = x + listW + 20, paneW = w - listW - 20;
    float bottom = area.y + area.height - 24;
    float h = fmaxf(320, bottom - y);
    Rectangle lr = { x, y, listW, h };
    ui_rect(lr, 12, TH.surface);
    ui_rect_lines(lr, 12, 1, TH.lineSoft);
    if (L.n == 0) {
        ui_text_wrap(F_REG, 13, "No runs recorded yet. The next run of this case starts the log.", (Rectangle){ x + 16, y + 16, listW - 32, 80 }, 19, TH.text3, true);
        float ew = ui_button_width(ICON_PLAY, "Go to Run");
        if (ui_button(ui_id("rl-go-run"), (Rectangle){ x + 16, y + 70, ew, 34 }, ICON_PLAY, "Go to Run", BTN_PRIMARY)) app_go(PG_RUN);
        text_pane((Rectangle){ paneX, y, paneW, h });
        return y + h - y0;
    }
    UiScroll *sc = &A.scr[17];
    float top = ui_scroll_begin(sc, ui_inset(lr, 4, 6));
    float ry = top + 2;
    for (int i = 0; i < L.n; i++) {
        const RunRecord *r = &L.r[i];
        Rectangle row = { x + 6, ry, listW - 12, 52 };
        if (ry + 52 > lr.y && ry < lr.y + lr.height) {
            if (ui_row(ui_idn(ui_id("rl-row"), i), row, i == L.sel)) show_run(i);
            char when[64], ago[32], sum[200]; struct tm tm; localtime_r(&r->started, &tm);
            strftime(when, sizeof when, "%d %b %H:%M", &tm);
            pl_time_ago(ago, sizeof ago, r->started);
            char head[120]; snprintf(head, sizeof head, "Run %d  ·  %s  ·  %s", L.n - i, when, ago);
            runlog_summary(r, sum, sizeof sum);
            Color fg, bg; const char *b = outcome_badge(r->outcome, &fg, &bg);
            float bwid = ui_badge_width(NULL, b);
            ui_text_clip(F_MED, 13.5f, head, row.x + 12, row.y + 8, row.width - 30 - bwid, TH.text);
            ui_text_clip(F_REG, 12, sum, row.x + 12, row.y + 29, row.width - 24, TH.text3);
            ui_badge(row.x + row.width - bwid - 8, row.y + 6, NULL, b, fg, bg);
        }
        ry += 54;
    }
    ui_scroll_end(sc, ry - top + 6);
    if (L.sel >= 0 && L.sel < L.n) {
        const RunRecord *r = &L.r[L.sel];
        float px = paneX;
        bw = ui_button_width(ICON_UNDO_2, "Go back to this run's setup");
        ui_disable(!r->rev[0]);
        if (ui_button(ui_id("rl-restore"), (Rectangle){ px, y, bw, 34 }, ICON_UNDO_2, "Go back to this run's setup", BTN_DANGER)) {
            A.modal = MD_HIST_CONFIRM; A.histAction = 1; pl_strlcpy(A.mText2, r->rev, sizeof A.mText2);
        }
        ui_disable(false);
        if (!r->rev[0]) ui_tooltip((Rectangle){ px, y, bw, 34 }, "No setup history was kept for this run (Settings > History)");
        else ui_tooltip((Rectangle){ px, y, bw, 34 }, "system, constant and the initial fields as they were when this run started; the setup now stays in the history");
        text_pane((Rectangle){ paneX, y + 44, paneW, h - 44 });
    } else text_pane((Rectangle){ paneX, y, paneW, h });
    return y + h - y0;
}

/* ============================================================================ page */
void page_history(Rectangle r)
{
    Case *c = A.cs;
    refresh(false);
    /* a snapshot job just ended: show it */
    if (H.snapPending && !A.cj->active) { H.snapPending = 0; refresh(true); }
    UiScroll *sc = &A.scr[22];
    bool fixed = A.histTab == 0 || A.histTab == 3;   /* the changes and run-log tabs fill the page and scroll inside */
    float top = fixed ? r.y : ui_scroll_begin(sc, r);
    float x = r.x + PAGE_PAD, w = fminf(r.width - 2 * PAGE_PAD, CONTENT_MAX + 300), y = top + 30;
    char sub[200]; pl_fmt_ui(sub, sizeof sub, "What changed in %s, and how to get back to an earlier state.", c->s.name);
    y += page_header(x, y, w, "History", sub);
    char t0[48], t1[48], t2[48];
    snprintf(t0, sizeof t0, "Changes (%d)", H.nHist);
    snprintf(t1, sizeof t1, "Snapshots (%d)", H.nSnaps);
    snprintf(t2, sizeof t2, "Deleted results (%d)", H.nTrash);
    runs_refresh(false);
    char t3[64]; snprintf(t3, sizeof t3, "%s (%d)", term(T_LOG), L.n);
    const char *tabs[] = { t0, t3, t1, t2 };
    static const int tabOf[] = { 0, 3, 1, 2 };                   /* shown order -> histTab */
    int shown = 0;
    for (int i = 0; i < 4; i++) if (tabOf[i] == A.histTab) shown = i;
    float th;
    int was = A.histTab;
    ui_tabs(ui_id("h-tabs"), x, y, tabs, 4, &shown, &th);
    A.histTab = tabOf[shown];
    if (was != A.histTab) {
        refresh(true);
        if (A.histTab == 3) { runs_refresh(true); L.sel = -1; set_text(NULL); }
        else if (was == 3) { set_text(NULL); H.sel = -1; H.shownRev[0] = 0; }
    }
    ui_divider(x, y + th, w);
    y += th + 22;
    if (A.histTab == 0) { tab_changes(r, x, y, w); return; }
    if (A.histTab == 3) { tab_runlog(r, x, y, w); return; }
    y += A.histTab == 1 ? tab_snapshots(x, y, w) : tab_trash(x, y, w);
    ui_scroll_end(sc, y - top + 30);
}

/* the confirmations (draw_modals) */
void history_modal(bool *close)
{
    Case *c = A.cs;
    if (!c) { *close = true; return; }
    const char *title = "", *go = "OK";
    char sub[600] = "";
    switch (A.histAction) {
    case 1:
        title = "Go back to this version?";
        snprintf(sub, sizeof sub, "The setup files (system, constant, 0.orig, scripts) become those of %.10s. Files made since then are "
                 "removed, and edits not saved yet are saved first. The current setup is recorded before, so this can be undone. Results and "
                 "meshes stay as they are.", A.mText2);
        go = "Go back";
        break;
    case 2:
        title = "Go back to this snapshot?";
        snprintf(sub, sizeof sub, "Everything in the case now (setup, mesh, results, logs) moves to the trash, then snapshot %s is copied back. "
                 "Restoring that trash entry undoes this.", A.mText2);
        go = "Go back";
        break;
    case 3: title = "Delete this snapshot?"; snprintf(sub, sizeof sub, "Snapshot %s is deleted for good.", A.mText2); go = "Delete"; break;
    case 4: title = "Delete for good?"; snprintf(sub, sizeof sub, "Trash entry %s is deleted and can't be restored.", A.mText2); go = "Delete"; break;
    case 5: title = "Empty the trash?"; snprintf(sub, sizeof sub, "Every entry in this case's trash is deleted and can't be restored."); go = "Empty it"; break;
    }
    Rectangle b = ui_modal_begin(title, sub, 600, 240, close);
    if (ui_button(ui_id("m-h-cancel"), (Rectangle){ b.x, b.y + b.height - 38, 100, 38 }, NULL, "Cancel", BTN_GHOST)) *close = true;
    float bw = ui_button_width(NULL, go) + 20;
    if (!ui_button(ui_id("m-h-go"), (Rectangle){ b.x + b.width - bw, b.y + b.height - 38, bw, 38 }, NULL, go, BTN_DANGER)) return;
    A.modal = MD_NONE;
    char msg[400] = "";
    switch (A.histAction) {
    case 1:
        if (!app_save_all(true)) return;
        if (hist_restore(c->s.path, A.mText2, msg, sizeof msg)) {
            case_reload_changed(c); case_rescan(c); A.stepInit = false;
            ui_toast(TOAST_OK, "%s", msg);
        } else ui_toast(TOAST_ERR, "%s", msg);
        H.sel = -1; set_text(NULL);
        break;
    case 2: {
        if (!app_can_start() || !app_save_all(true)) return;           /* unsaved edits go to the trash with the rest */
        int n = snapshot_clear_for_restore(c->s.path, A.mText2);
        char cmd[1024]; snapshot_restore_cmd(A.mText2, cmd, sizeof cmd);
        logset_open(&A.logs, c->s.path);
        app_start_tool("Restore snapshot", "restore", "Restore snapshot", cmd, "log.qc-restore");
        ui_toast(TOAST_INFO, "Moved %d item%s to the trash; copying the snapshot back", n, n == 1 ? "" : "s");
        H.snapPending = 1;
        break;
    }
    case 3: if (snapshot_remove(c->s.path, A.mText2)) ui_toast(TOAST_OK, "Deleted snapshot %s", A.mText2); break;
    case 4: if (trash_remove(c->s.path, A.mText2)) ui_toast(TOAST_OK, "Deleted %s for good", A.mText2); break;
    case 5: {
        int n = 0;
        for (int i = 0; i < H.nTrash; i++) if (trash_remove(c->s.path, H.trash[i].id)) n++;
        ui_toast(TOAST_OK, "Emptied the trash (%d entr%s)", n, n == 1 ? "y" : "ies");
        break;
    }
    }
    refresh(true);
}
