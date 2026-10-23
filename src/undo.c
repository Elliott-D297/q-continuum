/* undo.c -- undo and redo for every dictionary edit in the open case (ROADMAP 2.8).
 *
 * foamdict calls fd_edit_hook after each edit with the text before it; the writes of one click (several entries in
 * one frame) make one step, and so does typing in the Files editor until a second's pause. Steps are kept by path, not by FDFile, so
 * a file reloaded or dropped from the cache in between still undoes; one changed since (outside the app) does not.
 * The journal is per case and cleared when another case opens.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "undo.h"

#define UNDO_MAX 100
typedef struct { char rel[256]; char *before, *after; double t; char what[200]; int group; } Step;
static Step US[UNDO_MAX], RS[UNDO_MAX];
static int nU, nR;
static int muted;
static int curGroup, nextGroup = 1;                      /* steps made between undo_group_begin/end undo as one */
static char groupWhat[200];
static char forCase[PL_PATH];

static void step_free(Step *s) { free(s->before); free(s->after); s->before = s->after = NULL; }

void undo_clear(void)
{
    for (int i = 0; i < nU; i++) step_free(&US[i]);
    for (int i = 0; i < nR; i++) step_free(&RS[i]);
    nU = nR = 0;
}

/* "deltaT 0.002 in system/controlDict": the first line that differs, as it reads after the edit */
static void describe(Step *s)
{
    const char *a = s->before, *b = s->after;
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    const char *t = b[i] || !a[i] ? b : a;                      /* a deletion at the end: name what went */
    int ls = i; while (ls > 0 && t[ls - 1] != '\n') ls--;
    int le = i; while (t[le] && t[le] != '\n') le++;
    char line[120]; int n = le - ls < 119 ? le - ls : 119;
    memcpy(line, t + ls, (size_t)n); line[n] = 0;
    for (char *p = line; *p; p++) if (*p == '\t') *p = ' ';
    char *q = line; while (*q == ' ') q++;
    /* runs of spaces (column alignment) read as one */
    char out[120]; int k = 0; bool sp = false;
    for (char *p = q; *p && k < 118; p++) { if (*p == ' ') { if (!sp) out[k++] = ' '; sp = true; } else { out[k++] = *p; sp = false; } }
    while (k > 0 && (out[k - 1] == ';' || out[k - 1] == ' ')) k--;
    out[k] = 0;
    if (out[0]) pl_fmt_ui(s->what, sizeof s->what, "%s in %s", out, s->rel);
    else pl_fmt_ui(s->what, sizeof s->what, "an edit to %s", s->rel);
}

static const char *rel_of(const FDFile *f)
{
    if (!A.cs) return NULL;
    for (int i = 0; i < A.cs->nFiles; i++) if (A.cs->files[i].f == f) return A.cs->files[i].rel;
    return NULL;
}

static void hook(FDFile *f, const char *before)
{
    if (muted || !A.cs) return;
    const char *rel = rel_of(f);
    if (!rel) return;                                         /* a scratch copy (rebase), not a file of the case */
    if (strcmp(forCase, A.cs->s.path)) { undo_clear(); pl_strlcpy(forCase, A.cs->s.path, sizeof forCase); }
    double now = GetTime();
    Step *last = nU ? &US[nU - 1] : NULL;
    /* one step: the writes of one click (the same frame), or typing in the Files editor (a pause of a second ends it) */
    double span = text_editor_focused() ? 1.0 : 0.05;
    if (last && !strcmp(last->rel, rel) && now - last->t < span && !strcmp(last->after, before) && last->group == curGroup) {
        free(last->after); last->after = pl_strdup(f->text); last->t = now;      /* the same edit, continued */
        if (!strcmp(last->before, last->after)) { step_free(last); nU--; }      /* ... back where it started */
        else { describe(last); if (last->group && groupWhat[0]) pl_strlcpy(last->what, groupWhat, sizeof last->what); }
    } else {
        if (nU == UNDO_MAX) { step_free(&US[0]); memmove(US, US + 1, sizeof US[0] * (UNDO_MAX - 1)); nU--; }
        Step *s = &US[nU++];
        memset(s, 0, sizeof *s);
        pl_strlcpy(s->rel, rel, sizeof s->rel);
        s->before = pl_strdup(before);
        s->after = pl_strdup(f->text);
        s->t = now;
        describe(s);
        s->group = curGroup;
        if (curGroup && groupWhat[0]) pl_strlcpy(s->what, groupWhat, sizeof s->what);
    }
    for (int i = 0; i < nR; i++) step_free(&RS[i]);           /* a new edit ends the redo trail */
    nR = 0;
}

void undo_init(void) { fd_edit_hook = hook; }

static bool here(void) { return A.cs && !strcmp(forCase, A.cs->s.path); }

const char *undo_label(bool redo)
{
    if (!here()) return NULL;
    if (redo) return nR ? RS[nR - 1].what : NULL;
    return nU ? US[nU - 1].what : NULL;
}

void undo_group_begin(const char *what)
{
    curGroup = nextGroup++;
    pl_strlcpy(groupWhat, what ? what : "", sizeof groupWhat);
}
void undo_group_end(void) { curGroup = 0; groupWhat[0] = 0; }

/* a group: every step of it, each file checked first, then all applied (or none) */
static bool apply_group(bool redo)
{
    Step *from = redo ? RS : US, *to = redo ? US : RS;
    int *nf = redo ? &nR : &nU, *nt = redo ? &nU : &nR;
    int g = from[*nf - 1].group, k = *nf;
    while (k > 0 && from[k - 1].group == g) k--;
    for (int i = *nf - 1; i >= k; i--) {
        FDFile *f = case_dict(A.cs, from[i].rel);
        if (!f || strcmp(f->text, redo ? from[i].before : from[i].after)) {
            ui_toast(TOAST_WARN, "Can't %s %s: %s changed since (outside Q-Continuum?)", redo ? "redo" : "undo", from[i].what, from[i].rel);
            return false;
        }
    }
    char what[200]; pl_strlcpy(what, from[*nf - 1].what, sizeof what);
    bool dirty = false;
    int n = *nf - k;
    for (int j = 0; j < n; j++) {
        Step s = from[--*nf];
        FDFile *f = case_dict(A.cs, s.rel);
        muted++;
        fd_set_text(f, redo ? s.after : s.before);
        muted--;
        f->dirty = !f->base || strcmp(f->text, f->base) != 0;
        dirty |= f->dirty;
        if (*nt == UNDO_MAX) { step_free(&to[0]); memmove(to, to + 1, sizeof to[0] * (UNDO_MAX - 1)); (*nt)--; }
        s.t = 0;
        to[(*nt)++] = s;
    }
    case_rescan(A.cs);
    ui_toast(TOAST_INFO, "%s: %s (%d file%s%s)", redo ? "Redone" : "Undone", what, n, n == 1 ? "" : "s", dirty ? ", not saved yet" : ", as saved on disk");
    return true;
}

bool undo_apply(bool redo)
{
    if (!here()) return false;
    Step *from = redo ? RS : US, *to = redo ? US : RS;
    int *nf = redo ? &nR : &nU, *nt = redo ? &nU : &nR;
    if (!*nf) return false;
    if (from[*nf - 1].group) return apply_group(redo);
    Step s = from[--*nf];
    FDFile *f = case_dict(A.cs, s.rel);
    const char *want = redo ? s.before : s.after;              /* what the file must hold for the step to apply */
    if (!f || strcmp(f->text, want)) {
        ui_toast(TOAST_WARN, "Can't %s %s: the file changed since (outside Q-Continuum?)", redo ? "redo" : "undo", s.what);
        step_free(&s);
        return false;
    }
    muted++;
    fd_set_text(f, redo ? s.after : s.before);
    muted--;
    f->dirty = !f->base || strcmp(f->text, f->base) != 0;      /* back to what is on disk: nothing to save */
    if (*nt == UNDO_MAX) { step_free(&to[0]); memmove(to, to + 1, sizeof to[0] * (UNDO_MAX - 1)); (*nt)--; }
    s.t = 0;                                                   /* never continued by a later edit */
    to[(*nt)++] = s;
    case_rescan(A.cs);
    ui_toast(TOAST_INFO, "%s: %s%s", redo ? "Redone" : "Undone", s.what, f->dirty ? " (not saved yet)" : " (as saved on disk)");
    return true;
}
