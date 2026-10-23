/* test_logparse.c -- run the app's log parser over real OpenFOAM logs and compare with reviewed results.
 *
 *   test_logparse <dir>             check every <dir>/NAME.log against NAME.expect (exit 1 on a mismatch)
 *   test_logparse --update <dir>    (re)write the .expect files -- review the diff before committing
 *   test_logparse --dump <log>...   print what the parser makes of any log
 *
 * The fixtures in tests/logs are real v2606 output (tutorials and the user's own cases), trimmed to a few
 * time steps where they were large. Absolute case paths are replaced by <case>.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runner.h"

static void summary(const char *path, Str *o)
{
    LogSet s; memset(&s, 0, sizeof s);
    char dir[PL_PATH]; pl_dirname(dir, sizeof dir, path);
    logset_open(&s, dir);
    int k = logset_find(&s, pl_basename(path));
    if (k < 0) {                                                  /* not named log.*: parse it directly */
        LogMon *m = &s.logs[s.n++];
        memset(m, 0, sizeof *m);
        pl_strlcpy(m->name, pl_basename(path), sizeof m->name);
        pl_strlcpy(m->path, path, sizeof m->path);
        k = s.n - 1;
    }
    for (int i = 0; i < 400; i++) logset_poll(&s, 1000.0 + i, true);   /* big logs take several polls */
    LogMon *l = &s.logs[k];
    str_catf(o, "steps %d\ntime %.6g\ndeltaT %.6g\nended %d\nfatal %d\nconverged %d\n", l->steps, l->time, l->deltaT,
             l->ended, l->fatal, l->converged);
    str_catf(o, "courant n=%d", l->courant.n);
    if (l->courant.n) str_catf(o, " last=%.4g", l->courant.v[l->courant.n - 1]);
    str_catf(o, "\ninterfaceCourant n=%d", l->interfaceCo.n);
    if (l->interfaceCo.n) str_catf(o, " last=%.4g", l->interfaceCo.v[l->interfaceCo.n - 1]);
    str_cat(o, "\n");
    if (l->fatal) str_catf(o, "fatalMsg %s\n", l->fatalMsg);
    for (int i = 0; i < l->nSeries; i++) {
        Series *se = &l->series[i];
        str_catf(o, "series \"%s\" n=%d first=%.4g last=%.4g\n", se->name, se->n, se->n ? se->v[0] : 0.0, se->n ? se->v[se->n - 1] : 0.0);
    }
    logset_close(&s);
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--dump")) {
        for (int i = 2; i < argc; i++) { Str o; str_init(&o); summary(argv[i], &o); printf("== %s\n%s", argv[i], o.s ? o.s : ""); str_free(&o); }
        return 0;
    }
    bool update = argc >= 3 && !strcmp(argv[1], "--update");
    const char *dir = argv[update ? 2 : 1];
    if (argc < 2 || !dir) { fprintf(stderr, "usage: %s [--update] <dir> | --dump <log>...\n", argv[0]); return 2; }
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return 2; }
    char *names[512]; int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 512) if (pl_ends_with(de->d_name, ".log")) names[n++] = pl_strdup(de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], by_name);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        char lp[PL_PATH], ep[PL_PATH], en[300];
        pl_join(lp, sizeof lp, dir, names[i]);
        snprintf(en, sizeof en, "%.*s.expect", (int)strlen(names[i]) - 4, names[i]);
        pl_join(ep, sizeof ep, dir, en);
        Str o; str_init(&o); summary(lp, &o);
        if (update) { pl_write_file(ep, o.s, (size_t)o.len); printf("wrote %s\n", en); }
        else {
            size_t el; char *e = pl_read_file(ep, &el);
            bool ok = e && el == (size_t)o.len && !memcmp(e, o.s, el);
            printf("%s  %s\n", ok ? "ok  " : "FAIL", names[i]);
            if (!ok) { printf("---- expected\n%s---- got\n%s", e ? e : "(no .expect file)\n", o.s); bad++; }
            free(e);
        }
        str_free(&o);
        free(names[i]);
    }
    if (!update) printf("\n%d log%s, %d mismatch%s\n", n, n == 1 ? "" : "s", bad, bad == 1 ? "" : "es");
    return bad ? 1 : 0;
}
