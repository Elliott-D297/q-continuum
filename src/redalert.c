/* redalert.c -- RED ALERT (ROADMAP 3.3): is a running solver diverging? Read from its log monitor, in numbers.
 *
 * Alert (stop it now):
 *   - a NaN residual or Courant number;
 *   - an initial residual that rose above 0.1 and to 1000 times its lowest (median of the last 3 steps);
 *   - Courant max over 50; over 5 times maxCo (adjustable Δt); or over 5 and 4 times what it was 10 steps before (fixed Δt);
 *   - an adjustable Δt fallen below 1/1000 of what it started at.
 * Watch (look at it):
 *   - a residual at 100 times its lowest and above 0.01;
 *   - Courant max doubled in 10 steps and above 1 (fixed Δt);
 *   - Δt below 1/100 of its start;
 *   - a field bounded in 18 of the last 20 steps, after the first 100 (k and epsilon bound at the start of most runs).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "redalert.h"

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static float median3(const Series *s)
{
    float v[3] = { 0, 0, 0 }; int n = 0;
    for (int i = s->n - 1; i >= 0 && n < 3; i--) v[n++] = s->v[i];
    if (n < 3) return n ? v[n - 1] : 0;
    qsort(v, 3, sizeof v[0], cmpf);
    return v[1];
}

static void add(Alert *a, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void add(Alert *a, int level, const char *fmt, ...)
{
    char line[300];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    size_t L = strlen(a->text);
    snprintf(a->text + L, sizeof a->text - L, "%s%s", L ? "\n" : "", line);
    if (level > a->level) a->level = level;
    a->reasons++;
}

static int popcount64(unsigned long long x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }

int red_alert_check(const LogMon *m, bool adjustDt, double maxCo, Alert *out)
{
    memset(out, 0, sizeof *out);
    if (!m || m->ended || m->fatal || m->steps < 5) return 0;
    /* residuals */
    int worst = -1; double worstRatio = 0;
    for (int i = 0; i < m->nSeries; i++) {
        const Series *s = &m->series[i];
        if (s->n < 5) continue;
        float last = s->v[s->n - 1];
        if (isnan(last) || isinf(last)) { add(out, RA_ALERT, "The %s residual is not a number: the solution has blown up.", s->name); out->kind |= RA_RESIDUAL; continue; }
        float mn = INFINITY;
        for (int k = 0; k < s->n; k++) if (s->v[k] > 0 && s->v[k] < mn) mn = s->v[k];
        float med = median3(s);
        if (!(mn < INFINITY) || med <= 0) continue;
        double ratio = med / mn;
        if (ratio > worstRatio && med > 1e-2 && ratio > 100) { worstRatio = ratio; worst = i; }
    }
    if (worst >= 0) {
        const Series *s = &m->series[worst];
        float mn = INFINITY; for (int k = 0; k < s->n; k++) if (s->v[k] > 0 && s->v[k] < mn) mn = s->v[k];
        double med = median3(s);
        bool alert = med > 0.1 && worstRatio >= 1000;
        add(out, alert ? RA_ALERT : RA_WATCH, "The %s initial residual rose from %.2g to %.2g (%.0f times its lowest).", s->name, mn, med, worstRatio);
        out->kind |= RA_RESIDUAL;
    }
    /* Courant */
    const Series *c = &m->courant;
    if (c->n >= 2) {
        float co = c->v[c->n - 1];
        if (isnan(co)) { add(out, RA_ALERT, "The Courant number is not a number."); out->kind |= RA_COURANT; }
        else if (co > 50) { add(out, RA_ALERT, "Courant max is %.3g: far beyond what any of these solvers stays stable at.", co); out->kind |= RA_COURANT; }
        else if (adjustDt && maxCo > 0) {
            if (co > 5 * maxCo && co > 2) { add(out, RA_ALERT, "Courant max is %.3g, %.0f times maxCo %.3g: the time step can't keep up.", co, co / maxCo, maxCo); out->kind |= RA_COURANT; }
        } else if (c->n > 10) {
            float before = c->v[c->n - 11];
            if (co > 5 && before > 0 && co > 4 * before) { add(out, RA_ALERT, "Courant max rose from %.3g to %.3g in 10 steps.", before, co); out->kind |= RA_COURANT; }
            else if (co > 1 && before > 0 && co > 2 * before) { add(out, RA_WATCH, "Courant max doubled in 10 steps, %.3g to %.3g.", before, co); out->kind |= RA_COURANT; }
        }
    }
    /* time step */
    const Series *d = &m->dt;
    if (adjustDt && d->n >= 5) {
        float d0 = 0; for (int k = 0; k < 5 && k < d->n; k++) if (d->v[k] > d0) d0 = d->v[k];
        float dl = d->v[d->n - 1];
        if (d0 > 0 && dl > 0 && dl < 1e-3f * d0) { add(out, RA_ALERT, "The time step fell from %.3g s to %.3g s (%.0f times smaller).", d0, dl, d0 / dl); out->kind |= RA_DT; }
        else if (d0 > 0 && dl > 0 && dl < 1e-2f * d0) { add(out, RA_WATCH, "The time step fell from %.3g s to %.3g s.", d0, dl); out->kind |= RA_DT; }
    }
    /* bounding */
    if (m->steps > 100) {
        int n = popcount64(m->boundBits & ((1ULL << 20) - 1));
        if (n >= 18) { add(out, RA_WATCH, "%s was bounded (clipped to stay positive) in %d of the last 20 steps.", m->boundField[0] ? m->boundField : "A field", n); out->kind |= RA_BOUNDING; }
    }
    if (out->level == RA_ALERT) snprintf(out->title, sizeof out->title, "Red alert: the run is diverging");
    else if (out->level == RA_WATCH) snprintf(out->title, sizeof out->title, "Yellow alert: the run may be heading for trouble");
    return out->level;
}
