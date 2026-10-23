/* runner.c -- background jobs (bash with the OpenFOAM environment) and log parsing */
#define _GNU_SOURCE
#include "runner.h"
#include "program.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEXT_CAP_MAX (3 << 20)          /* keep at most ~3 MB of each log in memory */

/* ============================================================================ series */
static void series_push(Series *s, float t, float v)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->t = realloc(s->t, sizeof(float) * (size_t)s->cap);
        s->v = realloc(s->v, sizeof(float) * (size_t)s->cap);
    }
    s->t[s->n] = t; s->v[s->n] = v; s->n++;
}
static void series_free(Series *s) { free(s->t); free(s->v); memset(s, 0, sizeof *s); }

static int series_index(LogMon *m, const char *name)
{
    for (int i = 0; i < m->nSeries; i++) if (!strcmp(m->series[i].name, name)) return i;
    if (m->nSeries >= MAX_SERIES) return -1;
    Series *s = &m->series[m->nSeries];
    memset(s, 0, sizeof *s);
    pl_strlcpy(s->name, name, sizeof s->name);
    return m->nSeries++;
}

/* ============================================================================ log text */
static void text_append_line(LogMon *m, const char *s, int n)
{
    if (m->textLen + n + 1 > m->textCap) {
        if (m->textCap >= TEXT_CAP_MAX) {
            /* drop the oldest half of the lines */
            int drop = m->nLines / 2;
            if (drop < 1) drop = 1;
            int cut = drop < m->nLines ? m->lineStart[drop] : m->textLen;
            memmove(m->text, m->text + cut, (size_t)(m->textLen - cut));
            m->textLen -= cut;
            memmove(m->lineStart, m->lineStart + drop, sizeof(int) * (size_t)(m->nLines - drop));
            m->nLines -= drop;
            for (int i = 0; i < m->nLines; i++) m->lineStart[i] -= cut;
        } else {
            int nc = m->textCap ? m->textCap * 2 : 65536;
            while (nc < m->textLen + n + 1) nc *= 2;
            m->text = realloc(m->text, (size_t)nc);
            m->textCap = nc;
        }
    }
    if (m->nLines == m->capLines) {
        m->capLines = m->capLines ? m->capLines * 2 : 1024;
        m->lineStart = realloc(m->lineStart, sizeof(int) * (size_t)m->capLines);
    }
    m->lineStart[m->nLines++] = m->textLen;
    memcpy(m->text + m->textLen, s, (size_t)n);
    m->textLen += n;
    m->text[m->textLen++] = '\n';
}

const char *logmon_line(const LogMon *m, int i, int *len)
{
    if (i < 0 || i >= m->nLines) { *len = 0; return ""; }
    int a = m->lineStart[i];
    int b = (i + 1 < m->nLines) ? m->lineStart[i + 1] - 1 : m->textLen - 1;
    *len = b - a;
    return m->text + a;
}

/* ============================================================================ parser */
static double num_after(const char *line, const char *key, bool *ok)
{
    const char *p = strstr(line, key);
    if (!p) { *ok = false; return 0; }
    char *e;
    double v = strtod(p + strlen(key), &e);
    *ok = e != p + strlen(key);
    return v;
}

static bool killed_line(LogMon *m, const char *line)
{
    /* a rank killed from outside, mostly by Linux's out-of-memory killer: OpenFOAM prints nothing, only mpirun
       ("process rank 1 ... exited on signal 9 (Killed)") or bash ("line 3: 1234 Killed  icoFoam") report it */
    const char *sg = strstr(line, "exited on signal ");
    bool bashKilled = !sg && strstr(line, " Killed ") && strstr(line, "line ");
    int ksig = sg ? atoi(sg + strlen("exited on signal ")) : 9;
    if ((sg || bashKilled) && !m->fatal && ksig != 15 && ksig != 2) {     /* SIGTERM/SIGINT: someone stopped it */
        m->fatal = m->fatalDone = true;
        m->errors++;
        if (ksig == 9)
            snprintf(m->fatalMsg, sizeof m->fatalMsg, "A solver process was killed (SIGKILL) at t = %g after %d time step%s, without an "
                     "OpenFOAM error. When nobody stopped it, this is Linux's out-of-memory killer: the case needs more memory than is "
                     "free. Use fewer processes or a coarser mesh, or close other programs (on WSL, memory= in .wslconfig sets the limit).",
                     m->time, m->steps, m->steps == 1 ? "" : "s");
        else
            snprintf(m->fatalMsg, sizeof m->fatalMsg, "A solver process ended on signal %d at t = %g after %d time step%s.", ksig, m->time,
                     m->steps, m->steps == 1 ? "" : "s");
        return true;
    }
    return false;
}

static void parse_line(LogMon *m, const char *line)
{
    bool ok;
    while (*line == ' ' || *line == '\t') line++;
    /* parallel runs prefix every line with the MPI rank: "[2] Cannot find file ..." */
    if (line[0] == '[' && isdigit((unsigned char)line[1])) {
        const char *q = line + 1;
        while (isdigit((unsigned char)*q)) q++;
        if (*q == ']') { line = q + 1; while (*line == ' ' || *line == '\t') line++; }
    }
    if (m->prog != PROG_OPENFOAM) {                   /* CalculiX, MOOSE, DEME ...: program.c knows their logs */
        if (pl_starts_with(line, "Q-Continuum: exit ")) {
            m->exitCode = atoi(line + 18);
            if (m->exitCode == 0) m->ended = true;
            else if (!m->fatal && m->exitCode != 143 && m->exitCode != 130) {
                char t[96]; snprintf(t, sizeof t, "The program ended with exit code %d.", m->exitCode);
                logmon_fatal(m, t); m->fatalDone = true;
            }
            return;
        }
        if (pl_starts_with(line, "Q-Continuum: could not") || pl_starts_with(line, "Q-Continuum: ")) {
            if (strstr(line, "could not") || strstr(line, "not found")) { logmon_fatal(m, line + 13); m->fatalDone = true; }
            return;
        }
        if (prog_parse_line(m, line)) return;
        killed_line(m, line);
        return;
    }
    if (pl_starts_with(line, "Time = ")) {
        double t = strtod(line + 7, NULL);
        if (m->ended || m->fatal) {                  /* a continued run appended to this log: its outcome is still open */
            m->ended = m->fatal = m->fatalDone = m->converged = false;
            m->fatalMsg[0] = 0;
        }
        if (m->steps > 1 && t <= m->time) {          /* continued from an earlier write: those steps are being redone */
            double dt = m->deltaT > 0 ? m->deltaT : (m->time - m->startTimeSeen) / (m->steps - 1);
            if (dt > 0) {                                /* in double: a corrupt time (1e300, inf) must not overflow an int */
                double redo = floor((m->time - t) / dt + 0.5) + 1;
                m->steps = redo < m->steps ? m->steps - (int)redo : 0;
            }
        }
        if (m->steps == 0) m->startTimeSeen = t;
        m->time = t;
        m->steps++;
        m->boundBits <<= 1;
        memset(m->seenThisStep, 0, sizeof m->seenThisStep);
        m->region[0] = 0;
        return;
    }
    /* chtMultiRegion*: "Solving for fluid region topAir" / "Solving for solid region heater" */
    if (pl_starts_with(line, "Solving for fluid region ") || pl_starts_with(line, "Solving for solid region ")) {
        const char *r = line + strlen("Solving for fluid region ");
        int k = 0;
        while (r[k] && !isspace((unsigned char)r[k]) && k < (int)sizeof m->region - 1) { m->region[k] = r[k]; k++; }
        m->region[k] = 0;
        return;
    }
    /* "Courant Number mean: a max: b", per region "Region: topAir Courant Number mean: ...", or just
       "Courant Number max: b" (chtMultiRegionTwoPhaseEulerFoam); one point per step, the largest.
       interFoam's "Interface Courant Number mean: ..." is kept apart. */
    const char *cn = strstr(line, "Courant Number ");
    if (cn && strstr(cn, "max:")) {
        double co = num_after(cn, "max:", &ok);
        if (!ok) return;
        Series *s = (cn > line && strstr(line, "Interface Courant")) ? &m->interfaceCo : &m->courant;
        if (s->n && s->t[s->n - 1] == (float)m->time && s->stepTag == m->steps) { if (co > s->v[s->n - 1]) s->v[s->n - 1] = (float)co; }
        else { series_push(s, (float)m->time, (float)co); s->stepTag = m->steps; }
        return;
    }
    /* "time step continuity errors : sum local = 8.9e-09, global = ..., cumulative = ..." (the Pip-Boy's Geiger counter) */
    if (pl_starts_with(line, "time step continuity errors")) {
        double v = num_after(line, "sum local = ", &ok);
        if (ok) m->contLocal = v;
        return;
    }
    if (pl_starts_with(line, "deltaT = ")) {
        m->deltaT = strtod(line + 9, NULL);
        if (m->dt.n && m->dt.stepTag == m->steps) m->dt.v[m->dt.n - 1] = (float)m->deltaT;
        else { series_push(&m->dt, (float)m->time, (float)m->deltaT); m->dt.stepTag = m->steps; }
        return;
    }
    if (pl_starts_with(line, "bounding ")) {                     /* "bounding epsilon, min: -1.98 max: ..." */
        m->boundBits |= 1;
        int k = 0; const char *f = line + 9;
        while (f[k] && f[k] != ',' && k < (int)sizeof m->boundField - 1) { m->boundField[k] = f[k]; k++; }
        m->boundField[k] = 0;
        return;
    }
    if (pl_starts_with(line, "ExecutionTime = ")) {
        m->execTime = strtod(line + 16, NULL);
        double c = num_after(line, "ClockTime = ", &ok);
        if (ok) m->clockTime = c;
        return;
    }
    const char *sf = strstr(line, "Solving for ");
    if (sf) {
        if (pl_starts_with(line, "diagonal:")) return;                /* trivial solve (rho), residual always 0 */
        char name[40];
        const char *n0 = sf + 12;
        int k = 0;
        while (n0[k] && n0[k] != ',' && k < 39) { name[k] = n0[k]; k++; }
        name[k] = 0;
        if (pl_starts_with(name, "ILambda")) pl_strlcpy(name, "I (rays)", sizeof name);  /* fvDOM: one series */
        if (m->region[0]) {                                           /* multi-region: "h (topAir)" */
            char full[200]; pl_fmt_ui(full, sizeof full, "%s (%s)", name, m->region);
            pl_strlcpy(name, full, sizeof name);
        }
        double r0 = num_after(sf, "Initial residual = ", &ok);
        if (!ok) return;
        if (!(r0 > 0)) { bool known = false; for (int i = 0; i < m->nSeries; i++) if (!strcmp(m->series[i].name, name)) known = true; if (!known) return; }   /* nothing solved yet (pcorr, a first Uy): no legend entry */
        int si = series_index(m, name);
        if (si < 0) return;
        Series *s = &m->series[si];
        if (!m->seenThisStep[si]) {
            series_push(s, (float)m->time, (float)r0);                 /* 0 = nothing to solve; the chart skips it */
            m->seenThisStep[si] = true;
        } else if (pl_starts_with(name, "I (rays)") && s->n && r0 > s->v[s->n - 1]) {
            s->v[s->n - 1] = (float)r0;                          /* worst ray this step */
        }
        return;
    }
    if (!strcmp(line, "End") || pl_starts_with(line, "Finalising parallel run")) { m->ended = true; return; }
    /* a crash prints no FOAM FATAL, only "[stack trace]" and the signal handler frame */
    const char *sig = strstr(line, "Foam::sigFpe::sigHandler") ? "Floating-point exception (SIGFPE)"
                    : strstr(line, "Foam::sigSegv::sigHandler") ? "Segmentation fault (SIGSEGV)" : NULL;
    if (sig && !m->fatal) {
        m->fatal = m->fatalDone = true;
        m->errors++;
        int k = snprintf(m->fatalMsg, sizeof m->fatalMsg, "%s at t = %g after %d time step%s.", sig, m->time, m->steps, m->steps == 1 ? "" : "s");
        if (m->courant.n && k > 0 && k < (int)sizeof m->fatalMsg) {
            float co = m->courant.v[m->courant.n - 1];
            snprintf(m->fatalMsg + k, sizeof m->fatalMsg - (size_t)k,
                     co > 1 ? " The solution diverged: Courant max had reached %.3g. Reduce deltaT (or use adjustTimeStep with maxCo), "
                              "and check the mesh and boundary conditions."
                            : " Last Courant max %.3g. Check the mesh (checkMesh), the boundary conditions and the linear solver settings.", co);
        }
        return;
    }
    if (killed_line(m, line)) return;
    if (strstr(line, "FOAM FATAL") || strstr(line, "FOAM aborting") || pl_starts_with(line, "Q-Continuum: could not")) {
        if (!m->fatal) { m->fatal = true; m->fatalMsg[0] = 0; m->fatalDone = false; }
        else if (m->fatalMsg[0]) m->fatalDone = true;          /* the same error again from another rank */
        if (pl_starts_with(line, "Q-Continuum: ")) pl_strlcpy(m->fatalMsg, line + strlen("Q-Continuum: "), sizeof m->fatalMsg);
        m->errors++;
        return;
    }
    if (m->fatal && !m->fatalDone && !*line && m->fatalMsg[0]) { m->fatalDone = true; return; }   /* message ends at a blank line */
    if (m->fatal && !m->fatalDone && *line && !pl_starts_with(line, "From ") && !pl_starts_with(line, "in file") &&
        !pl_starts_with(line, "---") && !pl_starts_with(line, "#") && strcmp(line, "FOAM exiting") &&
        !pl_starts_with(line, "FOAM parallel run exiting")) {
        size_t have = strlen(m->fatalMsg), room = sizeof m->fatalMsg - 1 - have;
        if (room > 2) {
            int n = snprintf(m->fatalMsg + have, room + 1, "%s%s", have ? " " : "", line);
            (void)n;
        }
        return;
    }
    if (m->warnOpen) {                                         /* the text of a warning runs to a blank line */
        if (!*line || pl_starts_with(line, "---") || pl_starts_with(line, "//")) m->warnOpen = false;
        else if (!pl_starts_with(line, "From ") && !pl_starts_with(line, "in file") && !pl_starts_with(line, "This can be")) {
            const char *t = line; while (*t == ' ') t++;
            size_t have = strlen(m->warnMsg);
            if (have + 2 < sizeof m->warnMsg) snprintf(m->warnMsg + have, sizeof m->warnMsg - have, "%s%s", have ? " " : "", t);
            return;
        }
    }
    if (strstr(line, "FOAM Warning")) {
        m->warnings++;
        if (strstr(line, "allowSystemOperations")) return;    /* every log has it */
        const char *t = strstr(line, "FOAM Warning") + strlen("FOAM Warning");
        while (*t == ' ' || *t == ':') t++;
        pl_strlcpy(m->warnMsg, t, sizeof m->warnMsg);
        m->warnOpen = true;
        return;
    }
    if (strstr(line, "solution converged in") || strstr(line, "reached convergence criterion")) { m->converged = true; return; }
    if (pl_starts_with(line, "Mesh OK.")) { m->meshOk = true; return; }
    if (pl_starts_with(line, "Failed ") && strstr(line, "mesh checks")) { m->meshFailed = true; return; }
    if (pl_starts_with(line, "nProcs : ")) { m->nProcs = atoi(line + 9); return; }
    if (pl_starts_with(line, "nCells:")) { m->nCells = strtol(line + 7, NULL, 10); return; }
    if (pl_starts_with(line, "cells:")) { long v = strtol(line + 6, NULL, 10); if (v > 0) m->nCells = v; return; }
}

static void consume(LogMon *m, const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n' || m->partialLen >= (int)sizeof m->partial - 1) {
            m->partial[m->partialLen] = 0;
            text_append_line(m, m->partial, m->partialLen);
            parse_line(m, m->partial);
            m->partialLen = 0;
            if (c == '\n') continue;
        }
        m->partial[m->partialLen++] = c;
    }
}

void logmon_feed(LogMon *m, const char *buf, int n) { consume(m, buf, n); }

void logmon_step(LogMon *m, double t)
{
    if (m->ended || m->fatal) { m->ended = m->fatal = m->fatalDone = m->converged = false; m->fatalMsg[0] = 0; m->exitCode = -1; }
    if (m->steps == 0) m->startTimeSeen = t;
    if (m->steps > 0 && t > m->time) m->deltaT = t - m->time;
    m->time = t;
    m->steps++;
    m->boundBits <<= 1;
    memset(m->seenThisStep, 0, sizeof m->seenThisStep);
}

void logmon_value(LogMon *m, const char *series, double v, bool keepMax)
{
    if (!isfinite(v)) return;
    int si = series_index(m, series);
    if (si < 0) return;
    Series *s = &m->series[si];
    if (!m->seenThisStep[si] || !s->n) { series_push(s, (float)m->time, (float)v); m->seenThisStep[si] = true; }
    else if (keepMax && v > s->v[s->n - 1]) s->v[s->n - 1] = (float)v;
}

void logmon_fatal(LogMon *m, const char *text)
{
    if (!m->fatal) { m->fatal = true; m->fatalDone = false; m->fatalMsg[0] = 0; m->errors++; }
    if (m->fatalDone || !text || !*text) return;
    while (*text == ' ' || *text == '\t') text++;
    size_t have = strlen(m->fatalMsg);
    if (have + 2 < sizeof m->fatalMsg) snprintf(m->fatalMsg + have, sizeof m->fatalMsg - have, "%s%s", have ? " " : "", text);
}

void logmon_reset(LogMon *m)
{
    for (int i = 0; i < m->nSeries; i++) series_free(&m->series[i]);
    series_free(&m->courant);
    series_free(&m->interfaceCo);
    series_free(&m->dt);
    free(m->text); free(m->lineStart);
    char name[64], path[PL_PATH];
    pl_strlcpy(name, m->name, sizeof name);
    pl_strlcpy(path, m->path, sizeof path);
    int prog = m->prog;
    memset(m, 0, sizeof *m);
    m->prog = prog;
    m->exitCode = -1;
    pl_strlcpy(m->name, name, sizeof m->name);
    pl_strlcpy(m->path, path, sizeof m->path);
}

extern double g_log_budget_ms;
static void logmon_poll(LogMon *m)
{
    struct stat st;
    if (stat(m->path, &st) != 0) return;
    /* a new run: truncated below what was read, or deleted and recreated (Allrun clears logs). A log
       truncated and rewritten past the old offset between two polls is caught by logset_restart. */
    if ((long long)st.st_size < m->offset || (m->ino && m->ino != (unsigned long long)st.st_ino)) logmon_reset(m);
    m->ino = (unsigned long long)st.st_ino;
    if ((long long)st.st_size == m->offset) {
        /* a last line without a newline ("No times selected" ends reconstructPar's log): complete once the file is quiet */
        if (m->partialLen > 0 && time(NULL) - st.st_mtime >= 1) {
            m->partial[m->partialLen] = 0;
            text_append_line(m, m->partial, m->partialLen);
            parse_line(m, m->partial);
            m->partialLen = 0;
        }
        m->mtime = st.st_mtime;
        return;
    }
    FILE *f = fopen(m->path, "rb");
    if (!f) return;
    if (fseek(f, (long)m->offset, SEEK_SET)) { fclose(f); return; }
    char buf[65536];
    size_t r;
    /* at most ~8 ms per poll, so a 100 MB log is read over a few seconds without the window stalling */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
        consume(m, buf, (int)r);
        m->offset += (long long)r;
        if (r < sizeof buf) break;                               /* caught up with the writer */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6 > g_log_budget_ms) break;
    }
    fclose(f);
    m->mtime = st.st_mtime;
}

double g_log_budget_ms = 8.0;                              /* the app raises it while nobody is interacting */

/* ============================================================================ log set */
void logset_close(LogSet *s)
{
    for (int i = 0; i < s->n; i++) logmon_reset(&s->logs[i]);
    memset(s, 0, sizeof *s);
}

void logset_open(LogSet *s, const char *casePath)
{
    logset_close(s);
    pl_strlcpy(s->casePath, casePath, sizeof s->casePath);
    int p = prog_detect(casePath);
    s->prog = p > 0 ? p : PROG_OPENFOAM;
    logset_poll(s, 0, true);
}

int logset_find(const LogSet *s, const char *name)
{
    for (int i = 0; i < s->n; i++) if (!strcmp(s->logs[i].name, name)) return i;
    return -1;
}

void logset_poll(LogSet *s, double now, bool force)
{
    if (!s->casePath[0]) return;
    if (!force && now - s->lastScan < 0.25) return;
    s->lastScan = now;
    NameList l = { 0 };
    pl_list_dir(s->casePath, &l, false, true);
    /* new logs go at the end, oldest first: a step's log appears when it starts, so tabs follow the run order */
    int first = s->n;
    for (int i = 0; i < l.n; i++) {
        const char *n = l.names[i];
        if (!pl_starts_with(n, "log.") || pl_ends_with(n, "~")) continue;
        if (logset_find(s, n) >= 0 || s->n >= MAX_LOGS) continue;
        LogMon *m = &s->logs[s->n++];
        memset(m, 0, sizeof *m);
        pl_strlcpy(m->name, n, sizeof m->name);
        pl_join(m->path, sizeof m->path, s->casePath, n);
        m->mtime = pl_mtime(m->path);
        m->prog = s->prog;
        m->exitCode = -1;
    }
    names_free(&l);
    for (int i = first + 1; i < s->n; i++)                           /* insertion sort of the new ones by mtime */
        for (int k = i; k > first && s->logs[k].mtime < s->logs[k - 1].mtime; k--) {
            LogMon t = s->logs[k]; s->logs[k] = s->logs[k - 1]; s->logs[k - 1] = t;
        }
    for (int i = 0; i < s->n; i++) {
        if (!pl_is_file(s->logs[i].path)) {                       /* deleted (clean): close the gap, keep the order */
            logmon_reset(&s->logs[i]);
            memmove(&s->logs[i], &s->logs[i + 1], sizeof s->logs[0] * (size_t)(s->n - i - 1));
            s->n--;
            i--;
            continue;
        }
        logmon_poll(&s->logs[i]);
    }
}

void logset_restart(LogSet *s, const char *name)
{
    int k = logset_find(s, name);
    if (k >= 0) logmon_reset(&s->logs[k]);
}

int logset_newest(const LogSet *s)
{
    int best = -1;
    for (int i = 0; i < s->n; i++) if (best < 0 || s->logs[i].mtime > s->logs[best].mtime) best = i;
    return best;
}

int logset_primary(const LogSet *s, const char *app)
{
    if (app && *app) {
        char n[96]; snprintf(n, sizeof n, "log.%s", app);
        int k = logset_find(s, n);
        if (k >= 0 && (s->logs[k].nSeries > 0 || s->logs[k].fatal)) return k;
    }
    int best = -1;
    for (int i = 0; i < s->n; i++)
        if ((s->logs[i].nSeries > 0 || s->logs[i].fatal) && (best < 0 || s->logs[i].mtime > s->logs[best].mtime)) best = i;
    return best;
}

/* ============================================================================ jobs */
const char *step_state_name(StepState s)
{
    static const char *n[] = { "Pending", "Running", "Done", "Failed", "Skipped", "Stopped" };
    return n[s];
}

void job_reset(Job *j, const char *title)
{
    int launches = j->launches;                                   /* monotonic across runs */
    memset(j, 0, sizeof *j);
    j->launches = launches;
    j->pid = -1;
    pl_strlcpy(j->title, title ? title : "Run", sizeof j->title);
}

void job_add(Job *j, const char *id, const char *label, const char *cmd, const char *log, bool solver)
{
    if (j->n >= MAX_STEPS) return;
    JobStep *s = &j->steps[j->n++];
    memset(s, 0, sizeof *s);
    pl_strlcpy(s->id, id, sizeof s->id);
    pl_strlcpy(s->label, label, sizeof s->label);
    if (strlen(cmd) >= sizeof s->cmd)
        snprintf(s->cmd, sizeof s->cmd, "echo 'Q-Continuum: could not run %.60s: its command is longer than %d bytes (a very long case path?)'; exit 1",
                 label, (int)sizeof s->cmd - 1);
    else pl_strlcpy(s->cmd, cmd, sizeof s->cmd);
    pl_strlcpy(s->log, log ? log : "", sizeof s->log);
    s->solver = solver;
}

/* ============================================================================ command record */
static CmdRec CL[CMDLOG_MAX];
static int clSeq, clJobSeq;

int cmdlog_count(void) { return clSeq < CMDLOG_MAX ? clSeq : CMDLOG_MAX; }
const CmdRec *cmdlog_get(int i)
{
    int n = cmdlog_count();
    if (i < 0 || i >= n) return NULL;
    return &CL[(clSeq - n + i) % CMDLOG_MAX];
}
static CmdRec *cmdlog_find(int seq) { return seq > 0 && seq > clSeq - CMDLOG_MAX && seq <= clSeq ? &CL[(seq - 1) % CMDLOG_MAX] : NULL; }

static int cmdlog_add(const Job *j, const JobStep *s, double now)
{
    CmdRec *r = &CL[clSeq % CMDLOG_MAX];
    memset(r, 0, sizeof *r);
    r->seq = ++clSeq;
    r->jobSeq = j->cur == 0 || clJobSeq == 0 ? ++clJobSeq : clJobSeq;
    pl_strlcpy(r->casePath, j->casePath, sizeof r->casePath);
    pl_strlcpy(r->bashrc, j->bashrc, sizeof r->bashrc);
    pl_strlcpy(r->label, s->label, sizeof r->label);
    pl_strlcpy(r->cmd, s->cmd, sizeof r->cmd);
    pl_strlcpy(r->log, s->log, sizeof r->log);
    r->append = s->append;
    r->when = time(NULL);
    r->t0 = now;
    r->st = JS_RUNNING;
    return r->seq;
}

static void cmdlog_end(const JobStep *s)
{
    CmdRec *r = cmdlog_find(s->rec);
    if (!r) return;
    r->st = s->st; r->t1 = s->t1; r->exitCode = s->exitCode;
}

void cmdlog_script(const CmdRec *r, bool withSetup, char *out, int cap)
{
    Str b; str_init(&b);
    if (withSetup) {
        char q[PL_PATH + 8];
        pl_shell_quote(q, sizeof q, r->casePath);
        str_catf(&b, "cd %s\n", q);
        if (r->bashrc[0]) { pl_shell_quote(q, sizeof q, r->bashrc); str_catf(&b, "source %s\n", q); }
    }
    bool multi = strchr(r->cmd, '\n') != NULL;
    if (r->log[0]) {
        char ql[PL_PATH + 8]; pl_shell_quote(ql, sizeof ql, r->log);
        if (multi) str_catf(&b, "(\n%s\n) %s %s 2>&1\n", r->cmd, r->append ? ">>" : ">", ql);
        else str_catf(&b, "( %s ) %s %s 2>&1\n", r->cmd, r->append ? ">>" : ">", ql);
    } else str_catf(&b, "%s\n", r->cmd);
    pl_strlcpy(out, b.s ? b.s : "", (size_t)cap);
    str_free(&b);
}

static bool launch(Job *j, double now)
{
    JobStep *s = &j->steps[j->cur];
    char qc[PL_PATH + 8], qb[PL_PATH + 8];
    pl_shell_quote(qc, sizeof qc, j->casePath);
    pl_shell_quote(qb, sizeof qb, j->bashrc);
    Str sc; str_init(&sc);
    str_catf(&sc, "cd %s || exit 1\n", qc);
    str_cat(&sc, "rm -f .qc-step-rc; trap 'echo $? > .qc-step-rc' EXIT\n");   /* the exit code, for steps run after we quit */
    if (j->bashrc[0]) {
        str_catf(&sc, "source %s > /dev/null 2>&1\n", qb);
        str_catf(&sc, "if [ -z \"$WM_PROJECT_DIR\" ]; then echo \"Q-Continuum: could not load the OpenFOAM environment from %s\"; "
                      "echo \"Set the right etc/bashrc under Settings.\"; exit 127; fi\n", j->bashrc);
    }
    str_cat(&sc, s->cmd);
    str_cat(&sc, "\n");
    char logPath[PL_PATH] = "";
    if (s->log[0]) pl_join(logPath, sizeof logPath, j->casePath, s->log);
    j->pid = pl_spawn_shell_log(sc.s, s->log[0] ? logPath : NULL, s->append);
    str_free(&sc);
    s->rec = cmdlog_add(j, s, now);
    if (j->pid <= 0) { s->st = JS_FAILED; s->exitCode = -1; s->t1 = now; cmdlog_end(s); return false; }
    s->st = JS_RUNNING;
    s->t0 = now;
    j->launches++;
    return true;
}

/* Quitting while a step runs: the step carries on by itself (own session), but the steps after it were ours
   to start. Hand them to a detached script that waits for the current one, checks it succeeded, then runs
   the rest in order, stopping at the first failure. Returns how many steps it took over. */
int job_detach(Job *j)
{
    if (!j->active || j->external || j->cur >= j->n - 1 || j->pid <= 0) return 0;
    char qc[PL_PATH + 8], qb[PL_PATH + 8];
    pl_shell_quote(qc, sizeof qc, j->casePath);
    pl_shell_quote(qb, sizeof qb, j->bashrc);
    Str sc; str_init(&sc);
    str_catf(&sc, "cd %s || exit 1\n", qc);
    str_catf(&sc, "while kill -0 %d 2>/dev/null; do sleep 2; done\n", (int)j->pid);
    str_cat(&sc, "[ \"$(cat .qc-step-rc 2>/dev/null)\" = 0 ] || exit 1\n");
    if (j->bashrc[0]) str_catf(&sc, "source %s > /dev/null 2>&1 || exit 1\n", qb);
    for (int i = j->cur + 1; i < j->n; i++) {
        const JobStep *s = &j->steps[i];
        char ql[PL_PATH + 8];
        pl_shell_quote(ql, sizeof ql, s->log[0] ? s->log : "/dev/null");
        str_catf(&sc, "( %s\n) %s %s 2>&1 || exit 1\n", s->cmd, s->append ? ">>" : ">", ql);
    }
    pl_spawn_detached(sc.s);
    str_free(&sc);
    return j->n - 1 - j->cur;
}

bool job_start(Job *j, const char *casePath, const char *bashrc, double now)
{
    if (j->active || j->n == 0) return false;
    pl_strlcpy(j->casePath, casePath, sizeof j->casePath);
    pl_strlcpy(j->bashrc, bashrc ? bashrc : "", sizeof j->bashrc);
    for (int i = 0; i < j->n; i++) { j->steps[i].st = JS_PENDING; j->steps[i].t0 = j->steps[i].t1 = 0; }
    j->cur = 0;
    j->active = true;
    j->finished = j->failed = j->stopped = j->stopping = false;
    j->startedAt = now;
    if (!launch(j, now)) { j->active = false; j->failed = j->finished = true; j->finishedAt = now; return false; }
    return true;
}

/* a run already going in the case: follow its process groups as one step */
void job_adopt(Job *j, const char *casePath, const char *title, const char *log, const pid_t *pg, int n, int procs, double now)
{
    job_reset(j, title);
    job_add(j, "external", title, "", log, true);
    pl_strlcpy(j->casePath, casePath, sizeof j->casePath);
    j->external = true;
    j->nExtPg = n > 8 ? 8 : n;
    memcpy(j->extPg, pg, sizeof(pid_t) * (size_t)j->nExtPg);
    j->procs = procs > 0 ? procs : 1;
    j->isRun = true;
    j->active = true;
    j->startedAt = now;
    j->steps[0].st = JS_RUNNING;
    j->steps[0].t0 = now;
    j->launches++;
}

static bool group_alive(pid_t pg) { return kill(-pg, 0) == 0 || errno == EPERM; }

void job_poll(Job *j, double now)
{
    if (j->active && j->external) {
        if (now - j->extChecked < 1.0) return;
        j->extChecked = now;
        bool alive = false;
        for (int i = 0; i < j->nExtPg; i++) if (group_alive(j->extPg[i])) alive = true;
        if (alive) {
            if (j->stopping && now - j->stopAt > 4.0) for (int i = 0; i < j->nExtPg; i++) kill(-j->extPg[i], SIGKILL);
            return;
        }
        /* ended: how is for the caller to read from the log (it has no exit code) */
        j->steps[0].st = j->stopping ? JS_STOPPED : JS_DONE;
        j->steps[0].t1 = now;
        j->active = false; j->finished = true; j->finishedAt = now;
        if (j->stopping) j->stopped = true;
        j->cur = 1;
        return;
    }
    if (!j->active || j->pid <= 0) return;
    int st = 0;
    pid_t r = waitpid(j->pid, &st, WNOHANG);
    if (r == 0) {
        if (j->stopping && now - j->stopAt > 4.0) kill(-j->pid, SIGKILL);   /* still alive: force */
        return;
    }
    JobStep *s = &j->steps[j->cur];
    s->t1 = now;
    s->exitCode = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    j->pid = -1;
    if (j->stopping) {
        s->st = JS_STOPPED;
        cmdlog_end(s);
        for (int i = j->cur + 1; i < j->n; i++) j->steps[i].st = JS_SKIPPED;
        j->active = false; j->stopped = j->finished = true; j->finishedAt = now;
        return;
    }
    if (s->exitCode != 0) {
        s->st = JS_FAILED;
        cmdlog_end(s);
        for (int i = j->cur + 1; i < j->n; i++) j->steps[i].st = JS_SKIPPED;
        j->active = false; j->failed = j->finished = true; j->finishedAt = now;
        return;
    }
    s->st = JS_DONE;
    cmdlog_end(s);
    j->cur++;
    if (j->cur >= j->n) { j->active = false; j->finished = true; j->finishedAt = now; return; }
    if (!launch(j, now)) {
        for (int i = j->cur + 1; i < j->n; i++) j->steps[i].st = JS_SKIPPED;
        j->active = false; j->failed = j->finished = true; j->finishedAt = now;
    }
}

void job_stop(Job *j, double now)
{
    if (j->active && j->external && !j->stopping) {
        j->stopping = true;
        j->stopAt = now;
        for (int i = 0; i < j->nExtPg; i++) kill(-j->extPg[i], SIGTERM);
        return;
    }
    if (!j->active || j->pid <= 0 || j->stopping) return;
    j->stopping = true;
    j->stopAt = now;
    kill(-j->pid, SIGTERM);                    /* whole process group: bash, mpirun and ranks */
}
