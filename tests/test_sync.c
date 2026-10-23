/* test_sync.c -- the case directory is the source of truth: files changed on disk are noticed (even within the same
 * second), a file edited in the app takes in disk changes without losing its own edits, real conflicts are reported
 * rather than overwritten, and the pause sequence (B-01) leaves both the restored stopAt and the user's edit on disk.
 *
 *   test_sync <scratch dir>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "foamcase.h"
#include "foamdict.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *CD =
    "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       dictionary;\n    object      controlDict;\n}\n"
    "// a comment that must survive\n"
    "application     icoFoam;\n\nstartFrom       startTime;\n\nstartTime       0;\n\nstopAt          endTime;\n\n"
    "endTime         0.5;\n\ndeltaT          0.005;\n\nwriteControl    timeStep;\n\nwriteInterval   20;\n";

static char *with(const char *text, const char *from, const char *to)   /* text with the first `from` replaced */
{
    const char *p = strstr(text, from);
    if (!p) { printf("test setup: '%s' not found\n", from); exit(2); }
    size_t a = (size_t)(p - text), n = strlen(text) - strlen(from) + strlen(to);
    char *r = malloc(n + 1);
    memcpy(r, text, a); strcpy(r + a, to); strcat(r, p + strlen(from));
    return r;
}

static void put_file(const char *p, const char *t) { if (!pl_write_file(p, t, strlen(t))) { printf("cannot write %s\n", p); exit(2); } }

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: test_sync <scratch dir>\n"); return 2; }
    char dir[PL_PATH]; pl_join(dir, sizeof dir, argv[1], "syncCase");
    char sys[PL_PATH]; pl_join(sys, sizeof sys, dir, "system");
    pl_mkdirs(sys);
    char cdp[PL_PATH]; pl_join(cdp, sizeof cdp, dir, "system/controlDict");

    /* 1. a change on disk within the same second is noticed */
    put_file(cdp, CD);
    FDFile *f = fd_load(cdp);
    char *d1 = with(CD, "endTime         0.5;", "endTime         0.7;");
    put_file(cdp, d1);
    CHECK(fd_changed_on_disk(f), "a change written within the same second is not seen");
    CHECK(fd_sync(f) == FD_SYNC_RELOADED, "clean file not reloaded");
    char v[64] = ""; fd_get(f, "endTime", v, sizeof v);
    CHECK(!strcmp(v, "0.7"), "after reload endTime is '%s'", v);
    fd_free(f); free(d1);

    /* 2. dirty in the app + changed on disk: both edits kept, nothing else touched */
    put_file(cdp, CD);
    f = fd_load(cdp);
    fd_put(f, "deltaT", "0.0025");
    char *d2 = with(CD, "stopAt          endTime;", "stopAt          writeNow;");
    put_file(cdp, d2);
    CHECK(fd_sync(f) == FD_SYNC_REBASED, "dirty file not rebased onto the disk version");
    char *want = with(d2, "deltaT          0.005;", "deltaT          0.0025;");
    CHECK(!strcmp(f->text, want), "rebased text differs:\n%s", f->text);
    CHECK(f->dirty, "a rebased file with the user's edit must stay unsaved");
    CHECK(fd_save(f), "save after rebase failed");
    size_t n; char *disk = pl_read_file(cdp, &n);
    CHECK(disk && !strcmp(disk, want), "disk after save differs");
    free(disk); free(want); free(d2); fd_free(f);

    /* 3. the same entry changed on both sides: a conflict, nothing written */
    put_file(cdp, CD);
    f = fd_load(cdp);
    fd_put(f, "deltaT", "0.0025");
    char *d3 = with(CD, "deltaT          0.005;", "deltaT          0.001;");
    put_file(cdp, d3);
    CHECK(fd_sync(f) == FD_SYNC_CONFLICT, "a real conflict was not reported");
    CHECK(f->conflict && f->diskText && !strcmp(f->diskText, d3), "the disk version is not kept for the conflict view");
    fd_get(f, "deltaT", v, sizeof v);
    CHECK(!strcmp(v, "0.0025"), "the user's edit was lost in the conflict (deltaT '%s')", v);
    CHECK(!fd_save(f), "save went ahead over a conflict");
    disk = pl_read_file(cdp, &n);
    CHECK(disk && !strcmp(disk, d3), "a conflicted save overwrote the disk");
    free(disk);
    fd_resolve(f, true);                                  /* keep mine */
    CHECK(!f->conflict && fd_save(f), "keep-mine resolution did not save");
    disk = pl_read_file(cdp, &n);
    CHECK(disk && strstr(disk, "deltaT          0.0025;"), "keep-mine did not write the user's value");
    free(disk); free(d3); fd_free(f);

    /* 4. a raw text edit (a comment) can't be replayed entry by entry: conflict, not a silent merge */
    put_file(cdp, CD);
    f = fd_load(cdp);
    char *t4 = with(CD, "// a comment that must survive", "// my note");
    fd_set_text(f, t4);
    char *d4 = with(CD, "endTime         0.5;", "endTime         2;");
    put_file(cdp, d4);
    CHECK(fd_sync(f) == FD_SYNC_CONFLICT, "a text edit was merged blindly");
    fd_resolve(f, false);                                 /* take the disk version */
    CHECK(!f->dirty && !strcmp(f->text, d4), "take-disk resolution did not reload");
    free(t4); free(d4); fd_free(f);

    /* 5. B-01: Pause sets stopAt writeNow, the user edits deltaT in the app, the run ends and the app restores stopAt.
          Saving must keep both: stopAt endTime and the new deltaT. */
    put_file(cdp, CD);
    char q[PL_PATH]; pl_join(q, sizeof q, dir, PAUSE_REQ);
    put_file(q, "stopAt=endTime\n");
    Case *c = case_open(dir);
    CHECK(c != NULL, "case_open failed");
    if (c) {
        FDFile *cd = case_dict(c, "system/controlDict");
        char *paused = with(CD, "stopAt          endTime;", "stopAt          writeNow;");
        usleep(1100000);                                  /* the pause comes a while after the case was opened */
        put_file(cdp, paused);                            /* what case_pause_request writes */
        case_reload_changed(c);
        fd_put(cd, "deltaT", "0.0025");                   /* the user edits while it is paused */
        case_pause_settle(dir, true, "10.095");           /* the run ended: stopAt goes back */
        case_reload_changed(c);
        CHECK(case_save_all(c) >= 0, "save failed");
        disk = pl_read_file(cdp, &n);
        CHECK(disk && strstr(disk, "stopAt          endTime;"), "stopAt writeNow came back (B-01):\n%s", disk ? disk : "");
        CHECK(disk && strstr(disk, "deltaT          0.0025;"), "the user's deltaT was lost");
        free(disk); free(paused);
        case_close(c);
    }
    printf("%s: %d failure%s\n", fails ? "FAILED" : "passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
