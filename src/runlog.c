/* runlog.c -- CAPTAIN'S LOG: one record per run inside the case (see runlog.h) */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "runlog.h"
#include "runner.h"
#include "foamcase.h"
#include "foamdict.h"
#include "explain.h"
#include "safety.h"
#include "program.h"

/* ============================================================================ reading the run from the case */
/* the solver log: the head (nProcs, the first steps) and the last 4 MB (the final residuals, ExecutionTime, the
   error), so a 100 MB log costs no more than a small one */
static void feed_log(LogMon *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    const long head = 256 << 10, tail = 4 << 20;
    if (size <= 0) { fclose(f); return; }
    size_t cap = (size_t)(size <= head + tail ? size : tail) + 1;      /* the whole log, or the larger of the two pieces */
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return; }
    if (size <= head + tail) {
        fseek(f, 0, SEEK_SET);
        size_t n = fread(buf, 1, (size_t)size, f);
        logmon_feed(m, buf, (int)n);
    } else {
        fseek(f, 0, SEEK_SET);
        size_t n = fread(buf, 1, (size_t)head, f);
        while (n > 0 && buf[n - 1] != '\n') n--;                   /* whole lines only */
        logmon_feed(m, buf, (int)n);
        fseek(f, size - tail, SEEK_SET);
        n = fread(buf, 1, cap - 1, f);
        size_t s = 0;
        while (s < n && buf[s] != '\n') s++;
        if (s < n) logmon_feed(m, buf + s + 1, (int)(n - s - 1));
    }
    free(buf);
    fclose(f);
}

/* "Max non-orthogonality = 62.3 average: 7.1" and "Max skewness = 0.42 OK." from log.checkMesh */
static void checkmesh_stats(const char *casePath, double *nonOrtho, double *skew)
{
    *nonOrtho = *skew = -1;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, "log.checkMesh");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    const char *a = strstr(t, "Max non-orthogonality = ");
    if (a) *nonOrtho = atof(a + 24);
    const char *b = strstr(t, "Max skewness = ");
    if (b) *skew = atof(b + 15);
    free(t);
}

static void new_id(const char *dir, time_t when, char *id, int cap)
{
    struct tm tm; localtime_r(&when, &tm);
    char base[32]; strftime(base, sizeof base, "%Y%m%d-%H%M%S", &tm);
    pl_strlcpy(id, base, (size_t)cap);
    for (int k = 2; k < 100; k++) {
        char p[PL_PATH];
        if (!pl_fmt(p, sizeof p, "%s/%s.ini", dir, id) || !pl_exists(p)) return;
        snprintf(id, (size_t)cap, "%s-%d", base, k);
    }
}

/* one line, no newlines (values are written as key=value lines) */
static void one_line(char *s)
{
    for (char *p = s; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
}

/* the setup revision of the run that just ended: the last "started" line of the history's run list */
static void last_started_rev(const char *casePath, char *rev, int cap)
{
    rev[0] = 0;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, HIST_DIR "/qc-runs");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char r[48], o[16];
        if (sscanf(l, "%47s %15s", r, o) == 2 && !strcmp(o, "started")) pl_strlcpy(rev, r, (size_t)cap);
    }
    free(t);
}

bool runlog_write(const RunEnd *in, char *id, int cap)
{
    char dir[PL_PATH];
    pl_join(dir, sizeof dir, in->casePath, RUNLOG_DIR);
    if (!pl_mkdirs(dir)) return false;
    RunRecord r; memset(&r, 0, sizeof r);
    r.ended = in->ended ? in->ended : time(NULL);
    r.wall = in->wall;
    r.started = r.ended - (time_t)llround(in->wall);
    new_id(dir, r.started, r.id, sizeof r.id);
    pl_strlcpy(r.outcome, in->outcome ? in->outcome : "finished", sizeof r.outcome);
    pl_strlcpy(r.version, in->version ? in->version : "", sizeof r.version);
    r.procs = in->procs > 0 ? in->procs : 1;
    r.courant = -1;

    CaseSummary cs; case_summarize(in->casePath, &cs);
    pl_strlcpy(r.app, cs.app, sizeof r.app);
    r.cells = cs.nCells;
    r.endTime = cs.endTime;
    if (cs.prog) { const ProgInfo *pi = prog_info(cs.prog); if (pi && pi->name) pl_strlcpy(r.version, pi->name, sizeof r.version); }
    checkmesh_stats(in->casePath, &r.nonOrtho, &r.skew);

    char lg[96];
    if (in->solverLog && in->solverLog[0]) pl_strlcpy(lg, in->solverLog, sizeof lg);
    else snprintf(lg, sizeof lg, "log.%s", cs.app);
    char lp[PL_PATH]; pl_join(lp, sizeof lp, in->casePath, lg);
    LogMon m; memset(&m, 0, sizeof m); m.exitCode = -1; m.prog = cs.prog;
    if (pl_exists(lp)) {
        feed_log(&m, lp);
        r.reached = m.time;
        r.exec = m.execTime;
        if (m.nProcs > 0) r.procs = m.nProcs;
        if (m.nCells > 0 && r.cells <= 0) r.cells = m.nCells;
        if (m.courant.n > 0) r.courant = m.courant.v[m.courant.n - 1];
        for (int i = 0; i < m.nSeries && r.nRes < RUNLOG_MAX_RES; i++) {
            const Series *s = &m.series[i];
            if (s->n <= 0) continue;
            pl_strlcpy(r.resName[r.nRes], s->name, sizeof r.resName[0]);
            r.resVal[r.nRes++] = s->v[s->n - 1];
        }
        if (!strcmp(r.outcome, "failed")) {                         /* the error in words, as the Run page shows it */
            Explanation x;
            if (foam_explain(in->casePath, lg, m.fatalMsg, &x) && x.title[0]) snprintf(r.reason, sizeof r.reason, "%s", x.title);
            else if (m.fatalMsg[0]) pl_strlcpy(r.reason, m.fatalMsg, sizeof r.reason);
        } else if (!strcmp(r.outcome, "early"))
            snprintf(r.reason, sizeof r.reason, "Stopped at t = %g of %g without an error", m.time, cs.endTime);
    }
    logmon_reset(&m);
    if (!r.reason[0] && !strcmp(r.outcome, "failed") && in->nSteps > 0)
        for (int i = 0; i < in->nSteps; i++)
            if (in->stepState[i] && !strcmp(in->stepState[i], "failed")) { snprintf(r.reason, sizeof r.reason, "%s failed", in->stepLabel[i]); break; }
    one_line(r.reason);

    /* the setup: this run's revision, the one of the run recorded before it, and the change between them */
    last_started_rev(in->casePath, r.rev, sizeof r.rev);
    RunRecord prev[1];
    if (runlog_list(in->casePath, prev, 1) == 1) pl_strlcpy(r.prevRev, prev[0].rev, sizeof r.prevRev);
    if (r.rev[0] && r.prevRev[0]) {
        char *d = hist_diff_revs(in->casePath, r.prevRev, r.rev);
        char p[PL_PATH];
        if (d && d[0] && pl_fmt(p, sizeof p, "%s/%s.diff", dir, r.id)) pl_write_file(p, d, strlen(d));
        free(d);
    }

    Str b; str_init(&b);
    str_catf(&b, "# Q-Continuum run record\nid=%s\nstarted=%lld\nended=%lld\nwall=%.1f\noutcome=%s\nreason=%s\nversion=%s\napp=%s\n"
                 "procs=%d\ncells=%ld\nendTime=%g\nreached=%g\nexec=%g\ncourant=%g\nnonOrtho=%g\nskew=%g\nrev=%s\nprevRev=%s\n",
             r.id, (long long)r.started, (long long)r.ended, r.wall, r.outcome, r.reason, r.version, r.app, r.procs, r.cells,
             r.endTime, r.reached, r.exec, r.courant, r.nonOrtho, r.skew, r.rev, r.prevRev);
    for (int i = 0; i < r.nRes; i++) str_catf(&b, "res %s=%g\n", r.resName[i], r.resVal[i]);
    for (int i = 0; i < in->nSteps && i < RUNLOG_MAX_STEPS; i++) {
        char lab[48]; pl_strlcpy(lab, in->stepLabel[i] ? in->stepLabel[i] : "", sizeof lab);
        one_line(lab);
        for (char *p = lab; *p; p++) if (*p == '=') *p = '-';
        str_catf(&b, "step %s=%s %.1f\n", lab, in->stepState[i] ? in->stepState[i] : "", in->stepSec[i]);
    }
    char p[PL_PATH];
    bool ok = pl_fmt(p, sizeof p, "%s/%s.ini", dir, r.id) && pl_write_file(p, b.s, b.len);
    str_free(&b);
    if (ok && id) pl_strlcpy(id, r.id, (size_t)cap);
    return ok;
}

#define MAX_KIDS 8
static pid_t kids[MAX_KIDS];
void runlog_reap(void)
{
    for (int i = 0; i < MAX_KIDS; i++)
        if (kids[i] > 0 && waitpid(kids[i], NULL, WNOHANG) != 0) kids[i] = 0;
}

void runlog_write_bg(const RunEnd *in)
{
    runlog_reap();
    pid_t pid = fork();
    if (pid == 0) { runlog_write(in, NULL, 0); _exit(0); }            /* no GL in here: files and git only */
    if (pid < 0) { runlog_write(in, NULL, 0); return; }
    for (int i = 0; i < MAX_KIDS; i++) if (kids[i] == 0) { kids[i] = pid; return; }
    waitpid(pid, NULL, 0);                                           /* no free slot: wait for this one */
}

/* ============================================================================ reading records */
bool runlog_read(const char *casePath, const char *id, RunRecord *r)
{
    memset(r, 0, sizeof *r);
    r->courant = r->nonOrtho = r->skew = -1;
    char p[PL_PATH];
    if (!pl_fmt(p, sizeof p, "%s/" RUNLOG_DIR "/%s.ini", casePath, id)) return false;
    char *t = pl_read_file(p, NULL);
    if (!t) return false;
    pl_strlcpy(r->id, id, sizeof r->id);
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        if (l[0] == '#') continue;
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = l, *v = eq + 1;
        if (!strncmp(k, "res ", 4)) {
            if (r->nRes < RUNLOG_MAX_RES) { pl_strlcpy(r->resName[r->nRes], k + 4, sizeof r->resName[0]); r->resVal[r->nRes++] = atof(v); }
        } else if (!strncmp(k, "step ", 5)) {
            if (r->nSteps < RUNLOG_MAX_STEPS) {
                pl_strlcpy(r->stepLabel[r->nSteps], k + 5, sizeof r->stepLabel[0]);
                char st[12] = ""; double sec = 0;
                sscanf(v, "%11s %lf", st, &sec);
                pl_strlcpy(r->stepState[r->nSteps], st, sizeof r->stepState[0]);
                r->stepSec[r->nSteps++] = sec;
            }
        }
        else if (!strcmp(k, "started")) r->started = (time_t)atoll(v);
        else if (!strcmp(k, "ended")) r->ended = (time_t)atoll(v);
        else if (!strcmp(k, "wall")) r->wall = atof(v);
        else if (!strcmp(k, "outcome")) pl_strlcpy(r->outcome, v, sizeof r->outcome);
        else if (!strcmp(k, "reason")) pl_strlcpy(r->reason, v, sizeof r->reason);
        else if (!strcmp(k, "version")) pl_strlcpy(r->version, v, sizeof r->version);
        else if (!strcmp(k, "app")) pl_strlcpy(r->app, v, sizeof r->app);
        else if (!strcmp(k, "procs")) r->procs = atoi(v);
        else if (!strcmp(k, "cells")) r->cells = atol(v);
        else if (!strcmp(k, "endTime")) r->endTime = atof(v);
        else if (!strcmp(k, "reached")) r->reached = atof(v);
        else if (!strcmp(k, "exec")) r->exec = atof(v);
        else if (!strcmp(k, "courant")) r->courant = atof(v);
        else if (!strcmp(k, "nonOrtho")) r->nonOrtho = atof(v);
        else if (!strcmp(k, "skew")) r->skew = atof(v);
        else if (!strcmp(k, "rev")) pl_strlcpy(r->rev, v, sizeof r->rev);
        else if (!strcmp(k, "prevRev")) pl_strlcpy(r->prevRev, v, sizeof r->prevRev);
    }
    free(t);
    if (!pl_fmt(p, sizeof p, "%s/" RUNLOG_DIR "/%s.diff", casePath, id)) return true;
    r->hasDiff = pl_exists(p);
    return true;
}

int runlog_list(const char *casePath, RunRecord *out, int max)
{
    char dir[PL_PATH]; pl_join(dir, sizeof dir, casePath, RUNLOG_DIR);
    NameList l = { 0 };
    pl_list_dir(dir, &l, false, true);
    names_sort(&l);
    int n = 0;
    for (int i = l.n - 1; i >= 0 && n < max; i--) {                 /* ids sort by time: newest last */
        size_t len = strlen(l.names[i]);
        if (len < 5 || strcmp(l.names[i] + len - 4, ".ini")) continue;
        char id[40]; snprintf(id, sizeof id, "%.*s", (int)(len - 4), l.names[i]);
        if (runlog_read(casePath, id, &out[n])) n++;
    }
    names_free(&l);
    return n;
}

char *runlog_diff(const char *casePath, const char *id)
{
    char p[PL_PATH];
    if (!pl_fmt(p, sizeof p, "%s/" RUNLOG_DIR "/%s.diff", casePath, id)) return NULL;
    return pl_read_file(p, NULL);
}

static void thousands(long v, char *out, int cap)
{
    char d[32]; snprintf(d, sizeof d, "%ld", v < 0 ? -v : v);
    int n = (int)strlen(d), k = 0;
    if (v < 0 && k < cap - 1) out[k++] = '-';
    for (int i = 0; i < n && k < cap - 2; i++) {
        out[k++] = d[i];
        if ((n - i - 1) % 3 == 0 && i < n - 1) out[k++] = ' ';
    }
    out[k] = 0;
}

static void duration(double s, char *out, int cap)
{
    if (s < 90) snprintf(out, (size_t)cap, "%.0f s", s);
    else if (s < 5400) snprintf(out, (size_t)cap, "%.1f min", s / 60);
    else snprintf(out, (size_t)cap, "%.1f h", s / 3600);
}

void runlog_summary(const RunRecord *r, char *out, int cap)
{
    char cells[32] = "", wall[32];
    if (r->cells > 0) thousands(r->cells, cells, sizeof cells);
    duration(r->wall, wall, sizeof wall);
    snprintf(out, (size_t)cap, "%s%s%d proc%s%s%s%s · %s", r->app[0] ? r->app : "run", " · ", r->procs, r->procs == 1 ? "" : "s",
             cells[0] ? " · " : "", cells, cells[0] ? " cells" : "", wall);
}

char *runlog_markdown(const char *casePath, const char *caseName)
{
    enum { MAXR = 500 };
    RunRecord *rs = malloc(sizeof *rs * MAXR);
    if (!rs) return NULL;
    int n = runlog_list(casePath, rs, MAXR);
    Str b; str_init(&b);
    str_catf(&b, "# Run log: %s\n\n%d run%s recorded by Q-Continuum in `%s`.\n\n", caseName, n, n == 1 ? "" : "s", casePath);
    if (n) str_cat(&b, "| Run | Started | Outcome | Solver | Processes | Cells | Reached / end time | Wall time |\n|---|---|---|---|---|---|---|---|\n");
    for (int i = n - 1; i >= 0; i--) {
        const RunRecord *r = &rs[i];
        char when[40], cells[32] = "-", wall[32];
        struct tm tm; localtime_r(&r->started, &tm);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
        if (r->cells > 0) thousands(r->cells, cells, sizeof cells);
        duration(r->wall, wall, sizeof wall);
        str_catf(&b, "| %d | %s | %s | %s | %d | %s | %g / %g | %s |\n", n - i, when, r->outcome, r->app, r->procs, cells, r->reached, r->endTime, wall);
    }
    for (int i = n - 1; i >= 0; i--) {
        const RunRecord *r = &rs[i];
        char when[40];
        struct tm tm; localtime_r(&r->started, &tm);
        strftime(when, sizeof when, "%d %b %Y %H:%M", &tm);
        str_catf(&b, "\n## Run %d: %s, %s\n\n", n - i, when, r->outcome);
        if (r->reason[0]) str_catf(&b, "%s\n\n", r->reason);
        str_catf(&b, "- Solver: %s (%s), %d process%s\n", r->app, r->version[0] ? r->version : "OpenFOAM", r->procs, r->procs == 1 ? "" : "es");
        if (r->cells > 0) {
            char cells[32]; thousands(r->cells, cells, sizeof cells);
            str_catf(&b, "- Mesh: %s cells", cells);
            if (r->nonOrtho >= 0) str_catf(&b, ", max non-orthogonality %.1f°, max skewness %.2f", r->nonOrtho, r->skew);
            str_cat(&b, "\n");
        }
        char wall[32]; duration(r->wall, wall, sizeof wall);
        str_catf(&b, "- Reached t = %g of %g; wall time %s", r->reached, r->endTime, wall);
        if (r->exec > 0) str_catf(&b, " (solver ExecutionTime %.1f s)", r->exec);
        str_cat(&b, "\n");
        if (r->courant >= 0) str_catf(&b, "- Max Courant number at the last step: %.3g\n", r->courant);
        if (r->nRes) {
            str_cat(&b, "- Final initial residuals:");
            for (int k = 0; k < r->nRes; k++) str_catf(&b, "%s %s %.3g", k ? "," : "", r->resName[k], r->resVal[k]);
            str_cat(&b, "\n");
        }
        if (r->nSteps) {
            str_cat(&b, "- Steps:");
            for (int k = 0; k < r->nSteps; k++) str_catf(&b, "%s %s (%s, %.0f s)", k ? "," : "", r->stepLabel[k], r->stepState[k], r->stepSec[k]);
            str_cat(&b, "\n");
        }
        if (r->rev[0]) str_catf(&b, "- Setup revision: `%.10s`\n", r->rev);
        char *d = r->hasDiff ? runlog_diff(casePath, r->id) : NULL;
        if (d && d[0]) {
            /* the first 200 lines: the full patch stays in the app and in .qcontinuum/log */
            str_cat(&b, "- Setup changes since the run before:\n\n```diff\n");
            int lines = 0; const char *e = d;
            while (*e && lines < 200) { if (*e == '\n') lines++; e++; }
            str_catf(&b, "%.*s", (int)(e - d), d);
            if (b.len && b.s[b.len - 1] != '\n') str_cat(&b, "\n");
            if (*e) str_cat(&b, "... (cut at 200 lines; the whole change is in .qcontinuum/log)\n");
            str_cat(&b, "```\n");
        } else if (r->prevRev[0] && r->rev[0] && !strcmp(r->prevRev, r->rev)) str_cat(&b, "- Setup unchanged since the run before\n");
        free(d);
    }
    free(rs);
    return b.s ? b.s : pl_strdup("");
}
