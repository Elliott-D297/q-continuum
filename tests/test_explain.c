/* test_explain.c <case> <log name>: what the error translator says about the case's failed log, then its fix applied
 * (FIX_PUT / FIX_RENAME) and saved. Used by tests/explain_fixes.sh, which reruns the real solver afterwards.
 * Prints "title: ...", "text: ...", "where: file:line", "fix: <kind> <label>", "applied: yes|no". */
#include <stdio.h>
#include <string.h>
#include "explain.h"
#include "foamdict.h"
#include "runner.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_explain <case> <log>\n"); return 2; }
    char lp[PL_PATH]; pl_join(lp, sizeof lp, argv[1], argv[2]);
    size_t n; char *t = pl_read_file(lp, &n);
    if (!t) { printf("no log %s\n", lp); return 2; }
    LogMon m; memset(&m, 0, sizeof m);
    logmon_feed(&m, t, (int)n);
    if (!m.fatal && !m.warnMsg[0]) { printf("no error in the log\n"); return 1; }
    Explanation x;
    if (!foam_explain(argv[1], argv[2], m.fatal ? m.fatalMsg : m.warnMsg, &x)) { printf("not in the catalogue: %s\n", m.fatalMsg); return 1; }
    printf("title: %s\ntext: %s\nwhere: %s:%d (%s)\n", x.title, x.text, x.file, x.line, x.key);
    static const char *K[] = { "none", "put", "rename", "add-bc", "mesh" };
    printf("fix: %s %s\n", K[x.fix], x.fixLabel);
    if (x.fix == FIX_PUT || x.fix == FIX_RENAME) {
        char fp[PL_PATH]; pl_join(fp, sizeof fp, argv[1], x.fixFile);
        FDFile *f = fd_load(fp);
        bool ok = f && explain_apply(f, &x) && fd_save(f);
        printf("applied: %s\n", ok ? "yes" : "no");
        fd_free(f);
        return ok ? 0 : 1;
    }
    printf("applied: no (the app does this one)\n");
    return 0;
}
