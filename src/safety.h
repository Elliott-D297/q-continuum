/* safety.h -- undo for what the app deletes or overwrites: a trash inside each case for removed results, full-case
 * snapshots, and a git history of the case setup (system, constant, 0.orig, scripts) */
#ifndef SAFETY_H
#define SAFETY_H

#include <stdbool.h>
#include <time.h>
#include "pl_util.h"

#define TRASH_DIR ".qc-trash"         /* <case>/.qc-trash/<id>/ holds the items, <id>.info what they were */
#define SNAP_DIR  ".qc-snapshots"     /* <case>/.qc-snapshots/<id>/ a full copy, <id>.info its label     */
#define HIST_DIR  ".qc-history"       /* git directory; the work tree is the case                          */

/* ------------------------------------------------------------------ trash */
typedef struct {
    char   id[40];                    /* "20261018-120501" (+ "-2" ...)                         */
    char   reason[160];               /* "Clean", "Delete and start over", ...                  */
    char   latest[32];                /* the latest result time it held, "" if none             */
    time_t when;
    int    nItems;
    long long bytes;
} TrashEntry;

/* move case-relative items into a new trash entry; returns how many moved (0: no entry is left behind) */
int  trash_items(const char *casePath, const char *reason, const char *latest, const char *const *rels, int n);
int  trash_list(const char *casePath, TrashEntry *out, int max);      /* newest first; sizes measured */
bool trash_restore(const char *casePath, const char *id, char *msg, int cap);   /* what's in the way goes to the trash */
bool trash_remove(const char *casePath, const char *id);              /* gone for good */
int  trash_prune(const char *casePath, int keep, int maxDays);         /* entries removed; the newest is never pruned */
long long trash_bytes(const char *casePath);

/* ------------------------------------------------------------------ snapshots */
typedef struct {
    char   id[40];
    char   label[160];
    char   latest[32];
    time_t when;
    long long bytes;
    bool   complete;                  /* the copy finished (a .info with done=1) */
} Snapshot;

/* shell commands (run in the case folder) that copy the case into a new snapshot / back from one */
void snapshot_new_id(const char *casePath, char *id, int cap);
void snapshot_copy_cmd(const char *id, const char *label, const char *latest, char *cmd, int cap);
void snapshot_restore_cmd(const char *id, char *cmd, int cap);
int  snapshot_list(const char *casePath, Snapshot *out, int max);       /* newest first */
bool snapshot_remove(const char *casePath, const char *id);
/* restoring: everything in the case now goes to the trash first, so the restore can be undone too */
int  snapshot_clear_for_restore(const char *casePath, const char *id);

/* ------------------------------------------------------------------ setup history */
typedef struct {
    char   rev[48];
    char   subject[200];
    time_t when;
    int    nFiles;
    char   run[16];                   /* "" or the outcome of the run started at this revision */
} HistEntry;

bool hist_available(void);                                          /* git is installed */
bool hist_commit(const char *casePath, const char *message);        /* true when something new was recorded */
void hist_open(const char *casePath);                               /* opening a case: a first record, or edits made elsewhere */
int  hist_list(const char *casePath, HistEntry *out, int max);       /* newest first */
char *hist_show(const char *casePath, const char *rev);             /* malloc'd: the change a revision made */
char *hist_diff_now(const char *casePath, const char *rev);         /* malloc'd: rev -> the setup on disk now */
char *hist_diff_revs(const char *casePath, const char *a, const char *b);   /* malloc'd: the setup change from a to b */
bool hist_restore(const char *casePath, const char *rev, char *msg, int cap);
void hist_run_started(const char *casePath, const char *what);      /* commit + note the revision */
void hist_run_started_bg(const char *casePath, const char *what);   /* the same in a forked child: the UI goes on */
void hist_open_bg(const char *casePath);                             /* hist_open in a forked child */
void hist_run_ended(const char *casePath, const char *outcome);     /* "finished" / "failed" / "stopped" / "paused" */
bool hist_last_good_run(const char *casePath, char *rev, int cap, time_t *when);

#endif
