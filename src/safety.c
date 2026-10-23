/* safety.c -- the per-case trash, snapshots and setup history (see safety.h) */
#define _GNU_SOURCE
#include "safety.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ============================================================================ small helpers */
static void stamp_id(const char *base, char *id, int cap)
{
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    char s[32]; strftime(s, sizeof s, "%Y%m%d-%H%M%S", &tm);
    snprintf(id, (size_t)cap, "%s", s);
    for (int k = 2; k < 100; k++) {                              /* two in the same second */
        char p[PL_PATH], info[PL_PATH + 8];
        pl_join(p, sizeof p, base, id);
        snprintf(info, sizeof info, "%s.info", p);
        if (!pl_exists(p) && !pl_exists(info)) return;
        snprintf(id, (size_t)cap, "%s-%d", s, k);
    }
}

/* "key=value" lines */
static void info_get(const char *text, const char *key, char *out, int cap)
{
    out[0] = 0;
    size_t kl = strlen(key);
    for (const char *l = text; l && *l; ) {
        const char *e = strchr(l, '\n');
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            int n = (int)((e ? e : l + strlen(l)) - (l + kl + 1));
            snprintf(out, (size_t)cap, "%.*s", n, l + kl + 1);
            return;
        }
        l = e ? e + 1 : NULL;
    }
}

static void info_write(const char *path, const char *k1, const char *v1, const char *latest, int n, bool done)
{
    char b[1024];
    int len = snprintf(b, sizeof b, "%s=%s\nlatest=%s\nwhen=%lld\nitems=%d\ndone=%d\n", k1, v1, latest ? latest : "",
                       (long long)time(NULL), n, done ? 1 : 0);
    pl_write_file(path, b, (size_t)len);
}

static bool safe_id(const char *id)                               /* a name of ours, never a path */
{
    if (!id || !id[0] || strlen(id) > 39) return false;
    for (const char *p = id; *p; p++) if (!((*p >= '0' && *p <= '9') || *p == '-')) return false;
    return true;
}

/* ============================================================================ trash */
int trash_items(const char *casePath, const char *reason, const char *latest, const char *const *rels, int n)
{
    char base[PL_PATH], id[40], dir[PL_PATH], info[PL_PATH + 8];
    pl_join(base, sizeof base, casePath, TRASH_DIR);
    pl_mkdirs(base);
    stamp_id(base, id, sizeof id);
    pl_join(dir, sizeof dir, base, id);
    snprintf(info, sizeof info, "%s.info", dir);
    int moved = 0;
    for (int i = 0; i < n; i++) {
        if (!rels[i] || !rels[i][0] || strstr(rels[i], "..") || rels[i][0] == '/') continue;
        char from[PL_PATH], to[PL_PATH], toDir[PL_PATH];
        pl_join(from, sizeof from, casePath, rels[i]);
        struct stat st;
        if (lstat(from, &st) != 0) continue;
        pl_join(to, sizeof to, dir, rels[i]);
        pl_dirname(toDir, sizeof toDir, to);
        pl_mkdirs(toDir);
        if (rename(from, to) == 0) moved++;                     /* same file system: instant, whatever the size */
    }
    if (moved) info_write(info, "reason", reason, latest, moved, true);
    else rmdir(dir);
    return moved;
}

static int cmp_desc(const void *a, const void *b) { return strcmp(*(char *const *)b, *(char *const *)a); }

int trash_list(const char *casePath, TrashEntry *out, int max)
{
    char base[PL_PATH]; pl_join(base, sizeof base, casePath, TRASH_DIR);
    NameList l = { 0 };
    pl_list_dir(base, &l, true, false);
    if (l.n > 1) qsort(l.names, (size_t)l.n, sizeof(char *), cmp_desc);
    int n = 0;
    for (int i = 0; i < l.n && n < max; i++) {
        if (!safe_id(l.names[i])) continue;
        TrashEntry *e = &out[n];
        memset(e, 0, sizeof *e);
        pl_strlcpy(e->id, l.names[i], sizeof e->id);
        char d[PL_PATH], ip[PL_PATH + 8], v[64];
        pl_join(d, sizeof d, base, e->id);
        snprintf(ip, sizeof ip, "%s.info", d);
        char *t = pl_read_file(ip, NULL);
        if (t) {
            info_get(t, "reason", e->reason, sizeof e->reason);
            info_get(t, "latest", e->latest, sizeof e->latest);
            info_get(t, "when", v, sizeof v); e->when = (time_t)atoll(v);
            info_get(t, "items", v, sizeof v); e->nItems = atoi(v);
            free(t);
        }
        if (!e->when) e->when = pl_mtime(d);
        if (!e->reason[0]) pl_strlcpy(e->reason, "Deleted results", sizeof e->reason);
        e->bytes = pl_dir_size(d, 16);
        n++;
    }
    names_free(&l);
    return n;
}

long long trash_bytes(const char *casePath)
{
    char base[PL_PATH]; pl_join(base, sizeof base, casePath, TRASH_DIR);
    return pl_is_dir(base) ? pl_dir_size(base, 16) : 0;
}

/* every file and folder in the entry, relative to it: top level, plus one level down for constant/ and the
   like, which hold mesh items next to the user's own setup */
static void entry_items(const char *dir, const char *rel, NameList *out, int depth)
{
    char d[PL_PATH];
    if (rel[0]) pl_join(d, sizeof d, dir, rel); else pl_strlcpy(d, dir, sizeof d);
    NameList l = { 0 };
    pl_list_dir(d, &l, false, false);
    for (int i = 0; i < l.n; i++) {
        char r[PL_PATH], full[PL_PATH];
        if (rel[0]) pl_join(r, sizeof r, rel, l.names[i]); else pl_strlcpy(r, l.names[i], sizeof r);
        pl_join(full, sizeof full, dir, r);
        /* a folder that only exists to hold a deeper item (constant/ for constant/polyMesh) is walked into */
        bool container = depth < 2 && pl_is_dir(full) && (!strcmp(l.names[i], "constant") || !strcmp(l.names[i], "system"));
        if (container) entry_items(dir, r, out, depth + 1);
        else names_add(out, r);
    }
    names_free(&l);
}

bool trash_restore(const char *casePath, const char *id, char *msg, int cap)
{
    if (!safe_id(id)) { snprintf(msg, (size_t)cap, "Not a trash entry"); return false; }
    char base[PL_PATH], dir[PL_PATH];
    pl_join(base, sizeof base, casePath, TRASH_DIR);
    pl_join(dir, sizeof dir, base, id);
    if (!pl_is_dir(dir)) { snprintf(msg, (size_t)cap, "That trash entry is gone"); return false; }
    NameList items = { 0 };
    entry_items(dir, "", &items, 0);
    /* what's in the way now (a newer run's results) goes to the trash first, so this can be undone as well */
    NameList clash = { 0 };
    for (int i = 0; i < items.n; i++) {
        char p[PL_PATH]; pl_join(p, sizeof p, casePath, items.names[i]);
        struct stat st;
        if (lstat(p, &st) == 0) names_add(&clash, items.names[i]);
    }
    int displaced = 0;
    if (clash.n) {
        char why[200]; snprintf(why, sizeof why, "Replaced when restoring %s", id);
        displaced = trash_items(casePath, why, "", (const char *const *)clash.names, clash.n);
    }
    int back = 0, failed = 0;
    for (int i = 0; i < items.n; i++) {
        char from[PL_PATH], to[PL_PATH], toDir[PL_PATH];
        pl_join(from, sizeof from, dir, items.names[i]);
        pl_join(to, sizeof to, casePath, items.names[i]);
        pl_dirname(toDir, sizeof toDir, to);
        pl_mkdirs(toDir);
        if (rename(from, to) == 0) back++; else failed++;
    }
    names_free(&items); names_free(&clash);
    if (!failed) {
        pl_remove_tree(dir);                                    /* only empty folders are left */
        char info[PL_PATH + 8]; snprintf(info, sizeof info, "%s.info", dir); unlink(info);
        rmdir(base);                                            /* if that was the last entry */
    }
    if (displaced) snprintf(msg, (size_t)cap, "Restored %d item%s; %d that %s in the way went to the trash", back, back == 1 ? "" : "s",
                            displaced, displaced == 1 ? "was" : "were");
    else snprintf(msg, (size_t)cap, "Restored %d item%s", back, back == 1 ? "" : "s");
    if (failed) { size_t L = strlen(msg); snprintf(msg + L, (size_t)cap - L, " (%d could not be moved back)", failed); }
    return failed == 0;
}

bool trash_remove(const char *casePath, const char *id)
{
    if (!safe_id(id)) return false;
    char base[PL_PATH], dir[PL_PATH], info[PL_PATH + 8];
    pl_join(base, sizeof base, casePath, TRASH_DIR);
    pl_join(dir, sizeof dir, base, id);
    snprintf(info, sizeof info, "%s.info", dir);
    bool ok = !pl_exists(dir) || pl_remove_tree(dir) == 0;
    unlink(info);
    rmdir(base);
    return ok;
}

int trash_prune(const char *casePath, int keep, int maxDays)
{
    char base[PL_PATH]; pl_join(base, sizeof base, casePath, TRASH_DIR);
    if (!pl_is_dir(base)) return 0;
    NameList l = { 0 };
    pl_list_dir(base, &l, true, false);
    if (l.n > 1) qsort(l.names, (size_t)l.n, sizeof(char *), cmp_desc);
    int removed = 0, kept = 0;
    time_t now = time(NULL);
    for (int i = 0; i < l.n; i++) {
        if (!safe_id(l.names[i])) continue;
        char d[PL_PATH]; pl_join(d, sizeof d, base, l.names[i]);
        time_t m = pl_mtime(d);
        bool old = maxDays > 0 && m && difftime(now, m) > maxDays * 86400.0;
        if (kept >= 1 && (kept >= keep || old)) { if (trash_remove(casePath, l.names[i])) removed++; }
        else kept++;
    }
    names_free(&l);
    if (kept == 0) rmdir(base);
    return removed;
}

/* ============================================================================ snapshots */
void snapshot_new_id(const char *casePath, char *id, int cap)
{
    char base[PL_PATH]; pl_join(base, sizeof base, casePath, SNAP_DIR);
    stamp_id(base, id, cap);
}

void snapshot_copy_cmd(const char *id, const char *label, const char *latest, char *cmd, int cap)
{
    char qi[100], ql[400], qt[64];
    pl_shell_quote(qi, sizeof qi, id);
    pl_shell_quote(ql, sizeof ql, label);
    pl_shell_quote(qt, sizeof qt, latest ? latest : "");
    /* reflink: a copy-on-write clone where the file system can (btrfs, xfs), a plain copy elsewhere */
    snprintf(cmd, (size_t)cap,
             "set -e; shopt -s dotglob nullglob\n"
             "id=%s\n"
             "mkdir -p " SNAP_DIR "/\"$id\"\n"
             "printf 'label=%%s\\nlatest=%%s\\nwhen=%%s\\ndone=0\\n' %s %s \"$(date +%%s)\" > " SNAP_DIR "/\"$id\".info\n"
             "echo \"Copying the case into " SNAP_DIR "/$id ...\"\n"
             "for f in *; do case \"$f\" in .qc-trash|.qc-snapshots|.qc-history) continue;; esac\n"
             "  cp -a --reflink=auto -- \"$f\" " SNAP_DIR "/\"$id\"/\ndone\n"
             "sed -i 's/^done=0$/done=1/' " SNAP_DIR "/\"$id\".info\n"
             "echo \"Snapshot $id: $(du -sh " SNAP_DIR "/\"$id\" | cut -f1)\"", qi, ql, qt);
}

void snapshot_restore_cmd(const char *id, char *cmd, int cap)
{
    char qi[100];
    pl_shell_quote(qi, sizeof qi, id);
    snprintf(cmd, (size_t)cap,
             "set -e; shopt -s dotglob nullglob\n"
             "id=%s\n"
             "[ -d " SNAP_DIR "/\"$id\" ] || { echo \"Q-Continuum: could not find snapshot $id\"; exit 1; }\n"
             "echo \"Copying snapshot $id back into the case ...\"\n"
             "for f in " SNAP_DIR "/\"$id\"/*; do cp -a --reflink=auto -- \"$f\" .; done\n"
             "echo \"Restored snapshot $id. What the case held before is in the trash (History > Deleted results).\"", qi);
}

int snapshot_list(const char *casePath, Snapshot *out, int max)
{
    char base[PL_PATH]; pl_join(base, sizeof base, casePath, SNAP_DIR);
    NameList l = { 0 };
    pl_list_dir(base, &l, true, false);
    if (l.n > 1) qsort(l.names, (size_t)l.n, sizeof(char *), cmp_desc);
    int n = 0;
    for (int i = 0; i < l.n && n < max; i++) {
        if (!safe_id(l.names[i])) continue;
        Snapshot *s = &out[n];
        memset(s, 0, sizeof *s);
        pl_strlcpy(s->id, l.names[i], sizeof s->id);
        char d[PL_PATH], ip[PL_PATH + 8], v[64];
        pl_join(d, sizeof d, base, s->id);
        snprintf(ip, sizeof ip, "%s.info", d);
        char *t = pl_read_file(ip, NULL);
        if (t) {
            info_get(t, "label", s->label, sizeof s->label);
            info_get(t, "latest", s->latest, sizeof s->latest);
            info_get(t, "when", v, sizeof v); s->when = (time_t)atoll(v);
            info_get(t, "done", v, sizeof v); s->complete = atoi(v) == 1;
            free(t);
        }
        if (!s->when) s->when = pl_mtime(d);
        s->bytes = pl_dir_size(d, 16);
        n++;
    }
    names_free(&l);
    return n;
}

bool snapshot_remove(const char *casePath, const char *id)
{
    if (!safe_id(id)) return false;
    char base[PL_PATH], dir[PL_PATH], info[PL_PATH + 8];
    pl_join(base, sizeof base, casePath, SNAP_DIR);
    pl_join(dir, sizeof dir, base, id);
    snprintf(info, sizeof info, "%s.info", dir);
    bool ok = !pl_exists(dir) || pl_remove_tree(dir) == 0;
    unlink(info);
    return ok;
}

int snapshot_clear_for_restore(const char *casePath, const char *id)
{
    NameList l = { 0 };
    pl_list_dir(casePath, &l, false, false);
    NameList take = { 0 };
    for (int i = 0; i < l.n; i++) if (!pl_starts_with(l.names[i], ".qc-")) names_add(&take, l.names[i]);
    char why[200]; snprintf(why, sizeof why, "The case before restoring snapshot %s", id);
    int n = take.n ? trash_items(casePath, why, "", (const char *const *)take.names, take.n) : 0;
    names_free(&l); names_free(&take);
    return n;
}

/* ============================================================================ setup history (git) */
bool hist_available(void) { return pl_which("git", NULL, 0); }

/* every git call: our own git dir, the case as work tree, no hooks or signing from the user's global config */
#define GIT "git --git-dir=" HIST_DIR " --work-tree=. -c user.name=Q-Continuum -c user.email=q-continuum@localhost " \
            "-c core.autocrlf=false -c commit.gpgsign=false -c core.hooksPath=/dev/null "

/* the setup: dictionaries, initial fields, scripts. Not meshes, results, logs or anything over 2 MB. */
#define SETUP_FILES \
    "{ find system constant 0.orig -type f -size -2M ! -path '*/polyMesh/*' ! -path 'constant/extendedFeatureEdgeMesh/*' " \
    "! -name '*.eMesh' ! -name '*~' 2>/dev/null; [ -d 0.orig ] || find 0 -type f -size -2M ! -path '*/polyMesh/*' 2>/dev/null; " \
    "find . -maxdepth 1 -type f -size -2M ! -name 'log.*' ! -name '*.foam' ! -name '.qc-*' ! -name '*~' 2>/dev/null | sed 's|^\\./||'; " \
    "find salome paraview -maxdepth 1 -type f -size -2M -name '*.py' 2>/dev/null; }"

/* the run-start record runs in a forked child (a git commit takes 0.5 s on /mnt/c: a frozen frame); every other git
   call waits for it first, so two never touch the index at once */
static pid_t histChild;
static void hist_wait(void)
{
    if (histChild > 0) { waitpid(histChild, NULL, 0); histChild = 0; }
}

static char *git_run(const char *casePath, const char *body, size_t max, int *st)
{
    hist_wait();
    char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, casePath);
    Str s; str_init(&s);
    str_catf(&s, "cd %s || exit 1\n", q);
    str_cat(&s, body);
    char *out = pl_run_capture(s.s, max, st);
    str_free(&s);
    return out;
}

bool hist_commit(const char *casePath, const char *message)
{
    if (!hist_available()) return false;
    char qm[600]; pl_shell_quote(qm, sizeof qm, message);
    char body[2400];
    snprintf(body, sizeof body,
             "[ -d " HIST_DIR " ] || git --git-dir=" HIST_DIR " init -q || exit 1\n"
             GIT "add -u 2>/dev/null\n"
             SETUP_FILES " | " GIT "add --pathspec-from-file=- 2>/dev/null\n"
             GIT "diff --cached --quiet && exit 3\n"
             GIT "commit -q -m %s || exit 1\n", qm);
    int st;
    free(git_run(casePath, body, 256, &st));
    return st == 0;
}

void hist_open(const char *casePath)
{
    if (!hist_available() || access(casePath, W_OK) != 0) return;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, HIST_DIR);
    hist_commit(casePath, pl_is_dir(p) ? "Changed outside Q-Continuum" : "The setup when the history began");
}

static void run_outcome(const char *casePath, const char *rev, char *out, int cap)
{
    out[0] = 0;
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, HIST_DIR "/qc-runs");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char r[48], o[16];
        if (sscanf(l, "%47s %15s", r, o) == 2 && !strcmp(r, rev)) pl_strlcpy(out, o, (size_t)cap);   /* the last word wins */
    }
    free(t);
}

int hist_list(const char *casePath, HistEntry *out, int max)
{
    char body[600];
    snprintf(body, sizeof body, "[ -d " HIST_DIR " ] || exit 0\n" GIT "log -n %d --format='@%%H%%x09%%ct%%x09%%s' --name-only 2>/dev/null", max);
    int st;
    char *t = git_run(casePath, body, 4 << 20, &st);
    if (!t) return 0;
    int n = -1;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        if (l[0] == '@') {
            if (n + 1 >= max) break;
            HistEntry *e = &out[++n];
            memset(e, 0, sizeof *e);
            char *a = strchr(l, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
            if (!a || !b) { n--; continue; }
            snprintf(e->rev, sizeof e->rev, "%.*s", (int)(a - l - 1), l + 1);
            e->when = (time_t)atoll(a + 1);
            pl_strlcpy(e->subject, b + 1, sizeof e->subject);
        } else if (n >= 0 && l[0]) out[n].nFiles++;
    }
    free(t);
    for (int i = 0; i <= n; i++) run_outcome(casePath, out[i].rev, out[i].run, sizeof out[i].run);
    return n + 1;
}

static bool safe_rev(const char *rev)
{
    if (!rev || !rev[0] || strlen(rev) > 47) return false;
    for (const char *p = rev; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

char *hist_show(const char *casePath, const char *rev)
{
    if (!safe_rev(rev)) return NULL;
    char body[600];
    snprintf(body, sizeof body, GIT "show --stat --patch --format='%%s%%n%%cd%%n' --date=format:'%%d %%b %%Y %%H:%%M' %s 2>&1", rev);
    int st;
    return git_run(casePath, body, 2 << 20, &st);
}

char *hist_diff_revs(const char *casePath, const char *a, const char *b)
{
    if (!safe_rev(a) || !safe_rev(b)) return NULL;
    char body[600];
    snprintf(body, sizeof body, GIT "diff --stat --patch %s %s 2>&1", a, b);
    int st;
    char *t = git_run(casePath, body, 2 << 20, &st);
    if (st != 0) { free(t); return NULL; }
    return t;
}

char *hist_diff_now(const char *casePath, const char *rev)
{
    if (!safe_rev(rev)) return NULL;
    char body[1600];
    /* new setup files are marked "to be added" so they show in the comparison too */
    snprintf(body, sizeof body, SETUP_FILES " | " GIT "add --intent-to-add --pathspec-from-file=- 2>/dev/null\n"
             GIT "diff --stat --patch %s 2>&1", rev);
    int st;
    char *t = git_run(casePath, body, 2 << 20, &st);
    if (t && !t[0]) { free(t); t = pl_strdup("No changes: the setup on disk is the same as in this version.\n"); }
    return t;
}

bool hist_restore(const char *casePath, const char *rev, char *msg, int cap)
{
    if (!safe_rev(rev)) { snprintf(msg, (size_t)cap, "Not a revision"); return false; }
    char why[120]; snprintf(why, sizeof why, "Before restoring the version of %.10s", rev);
    hist_commit(casePath, why);                                 /* the current setup stays reachable */
    char body[600];
    snprintf(body, sizeof body, GIT "restore --source=%s --staged --worktree -- :/ 2>&1", rev);
    int st;
    char *out = git_run(casePath, body, 4096, &st);
    if (st == 0) snprintf(msg, (size_t)cap, "The setup is back to the version of %.10s", rev);
    else snprintf(msg, (size_t)cap, "git restore failed: %s", out ? out : "");
    free(out);
    if (st == 0) { char m2[120]; snprintf(m2, sizeof m2, "Restored the version of %.10s", rev); hist_commit(casePath, m2); }
    return st == 0;
}

static void runs_append(const char *casePath, const char *rev, const char *what)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, HIST_DIR "/qc-runs");
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "%s %s %lld\n", rev, what, (long long)time(NULL));
    fclose(f);
}

static bool head_rev(const char *casePath, char *rev, int cap)
{
    int st;
    char *t = git_run(casePath, "[ -d " HIST_DIR " ] || exit 1\n" GIT "rev-parse HEAD 2>/dev/null", 128, &st);
    bool ok = t && st == 0 && t[0];
    if (ok) { pl_trim_inplace(t); pl_strlcpy(rev, t, (size_t)cap); }
    free(t);
    return ok && safe_rev(rev);
}

void hist_run_started(const char *casePath, const char *what)
{
    if (!hist_available()) return;
    char m[200]; snprintf(m, sizeof m, "Before run: %s", what);
    hist_commit(casePath, m);
    char rev[48];
    if (head_rev(casePath, rev, sizeof rev)) runs_append(casePath, rev, "started");
}

void hist_run_started_bg(const char *casePath, const char *what)
{
    hist_wait();
    pid_t pid = fork();
    if (pid == 0) { hist_run_started(casePath, what); _exit(0); }   /* no GL, no UI in here: git and a file append */
    if (pid > 0) histChild = pid;
    else hist_run_started(casePath, what);                          /* no fork: do it here */
}

void hist_open_bg(const char *casePath)
{
    if (!hist_available() || access(casePath, W_OK) != 0) return;
    hist_wait();
    pid_t pid = fork();
    if (pid == 0) { hist_open(casePath); _exit(0); }
    if (pid > 0) histChild = pid;
    else hist_open(casePath);
}

void hist_run_ended(const char *casePath, const char *outcome)
{
    hist_wait();                                                    /* its "started" line first */
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, HIST_DIR "/qc-runs");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    char last[48] = "";                                          /* the revision of the run that just ended */
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char r[48], o[16];
        if (sscanf(l, "%47s %15s", r, o) == 2 && !strcmp(o, "started")) pl_strlcpy(last, r, sizeof last);
    }
    free(t);
    if (last[0]) runs_append(casePath, last, outcome);
}

bool hist_last_good_run(const char *casePath, char *rev, int cap, time_t *when)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, HIST_DIR "/qc-runs");
    char *t = pl_read_file(p, NULL);
    if (!t) return false;
    bool found = false;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char r[48], o[16]; long long w = 0;
        if (sscanf(l, "%47s %15s %lld", r, o, &w) >= 2 && !strcmp(o, "finished")) {
            pl_strlcpy(rev, r, (size_t)cap);
            if (when) *when = (time_t)w;
            found = true;
        }
    }
    free(t);
    return found;
}
