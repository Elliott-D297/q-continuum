/* coupling.c -- preCICE coupled simulations (see coupling.h) */
#define _GNU_SOURCE
#include "coupling.h"

#include <ctype.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================ discovery */
static void scan_dir(const char *dir, int depth, CouplingSummary *out, int max, int *n)
{
    if (*n >= max || depth > 3) return;
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "precice-config.xml");
    if (pl_is_file(p)) {
        CouplingSummary *s = &out[(*n)++];
        memset(s, 0, sizeof *s);
        pl_strlcpy(s->path, dir, sizeof s->path);
        pl_strlcpy(s->name, pl_basename(dir), sizeof s->name);
        s->mtime = pl_mtime(p);
        size_t len; char *t = pl_read_file(p, &len);
        for (const char *q = t; q && (q = strstr(q, "<participant ")); q++) s->nPart++;
        free(t);
        return;                                       /* participants live below: don't descend */
    }
    if (case_is_case(dir)) return;                    /* an OpenFOAM case is not a container of couplings */
    NameList l = { 0 };
    pl_list_dir(dir, &l, true, false);
    for (int i = 0; i < l.n && *n < max; i++) {
        const char *nm = l.names[i];
        if (nm[0] == '.' || pl_starts_with(nm, "processor") || pl_is_number(nm) || !strcmp(nm, "constant") || !strcmp(nm, "system")) continue;
        char sub[PL_PATH]; pl_join(sub, sizeof sub, dir, nm);
        scan_dir(sub, depth + 1, out, max, n);
    }
    names_free(&l);
}

int coupling_scan(const char *root, CouplingSummary *out, int max)
{
    int n = 0;
    scan_dir(root, 0, out, max, &n);
    return n;
}

XNode *coupling_root(Coupling *c) { return xml_child(&c->tree.root, "precice-configuration", 0); }

XNode *coupling_scheme(Coupling *c)
{
    XNode *r = coupling_root(c);
    for (int i = 0; r && i < r->nChild; i++) if (xml_tag_is(&r->child[i], "coupling-scheme:")) return &r->child[i];
    return NULL;
}

static void lower(const char *s, char *o, int cap) { int k = 0; for (; s[k] && k < cap - 1; k++) o[k] = (char)tolower((unsigned char)s[k]); o[k] = 0; }

static bool read_word(const char *dictPath, const char *key, char *out, int cap)
{
    out[0] = 0;
    FDFile *f = fd_load(dictPath);
    if (!f) return false;
    char v[256];
    bool ok = fd_get(f, key, v, sizeof v) && fd_word(v, out, cap);
    fd_free(f);
    for (char *q = out; *q; q++) if (*q == ';' || *q == '"') { memmove(q, q + 1, strlen(q)); q--; }
    return ok;
}

static void load_choices(Coupling *c)
{
    char p[PL_PATH]; pl_join(p, sizeof p, c->path, ".q-continuum-coupling.ini");
    size_t n; char *t = pl_read_file(p, &n);
    if (!t) return;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '='), *dot = strchr(line, '.');
        if (!eq || !dot || dot > eq) continue;
        *eq = 0; *dot = 0;
        const char *k = line, *who = dot + 1, *v = eq + 1;
        for (int i = 0; i < c->n; i++) {
            CParticipant *pp = &c->p[i];
            if (strcmp(pp->name, who)) continue;
            if (!strcmp(k, "dir")) { for (int d = 0; d < pp->nDirs; d++) if (!strcmp(pp->dirs[d], v)) pp->dirSel = d; }
            else if (!strcmp(k, "procs")) pp->procs = atoi(v) > 0 ? atoi(v) : 1;
            else if (!strcmp(k, "cmd")) pl_strlcpy(pp->cmd, v, sizeof pp->cmd);
        }
    }
    free(t);
}

void coupling_save_choices(const Coupling *c)
{
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum: participant folders, processes and commands for this coupled simulation\n");
    for (int i = 0; i < c->n; i++) {
        const CParticipant *p = &c->p[i];
        if (p->dirSel >= 0) str_catf(&b, "dir.%s=%s\n", p->name, p->dirs[p->dirSel]);
        if (p->procs > 1) str_catf(&b, "procs.%s=%d\n", p->name, p->procs);
        if (p->cmd[0]) str_catf(&b, "cmd.%s=%s\n", p->name, p->cmd);
    }
    char path[PL_PATH]; pl_join(path, sizeof path, c->path, ".q-continuum-coupling.ini");
    pl_write_file(path, b.s, b.len);
    str_free(&b);
}

const char *coupling_dir(const Coupling *c, const CParticipant *p, char *buf, int cap)
{
    if (p->dirSel < 0 || p->dirSel >= p->nDirs) { buf[0] = 0; return buf; }
    pl_join(buf, (size_t)cap, c->path, p->dirs[p->dirSel]);
    return buf;
}

static void describe(Coupling *c, CParticipant *p)
{
    char d[PL_PATH], q[PL_PATH];
    coupling_dir(c, p, d, sizeof d);
    p->foam = d[0] && case_is_case(d);
    p->solver[0] = 0;
    if (p->foam) { pl_join(q, sizeof q, d, "system/controlDict"); read_word(q, "application", p->solver, sizeof p->solver); }
    pl_join(q, sizeof q, d, "run.sh");
    p->hasRunSh = d[0] && pl_is_file(q);
}

void coupling_refresh(Coupling *c)
{
    if (c->cfg && (c->cfgText != c->cfg->text || c->cfgLen != c->cfg->len)) {
        xml_parse(c->cfg->text, c->cfg->len, &c->tree);
        c->cfgText = c->cfg->text; c->cfgLen = c->cfg->len;
    }
    /* participants from the configuration, keeping what is known about the ones that stay */
    CParticipant old[CPL_MAX_PART]; int nOld = c->n;
    memcpy(old, c->p, sizeof old);
    c->n = 0;
    XNode *r = coupling_root(c);
    for (int i = 0; r && i < r->nChild && c->n < CPL_MAX_PART; i++) {
        if (strcmp(r->child[i].tag, "participant")) continue;
        CParticipant *p = &c->p[c->n];
        char nm[64]; xml_attr(c->cfg->text, &r->child[i], "name", nm, sizeof nm);
        int k = -1;
        for (int j = 0; j < nOld; j++) if (!strcmp(old[j].name, nm)) k = j;
        if (k >= 0) { *p = old[k]; old[k].itWin = old[k].itN = old[k].cvX = old[k].cvR = NULL; }
        else { memset(p, 0, sizeof *p); pl_strlcpy(p->name, nm, sizeof p->name); p->procs = 1; p->dirSel = -1; p->pid = -1; }
        c->n++;
    }
    for (int j = 0; j < nOld; j++) { free(old[j].itWin); free(old[j].itN); free(old[j].cvX); free(old[j].cvR); }
    /* candidate folders: a case whose preciceDict names the participant, or a folder named <participant>[-*] */
    NameList l = { 0 };
    pl_list_dir(c->path, &l, true, false);
    names_sort(&l);
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        char keep[128] = "";
        if (p->dirSel >= 0) pl_strlcpy(keep, p->dirs[p->dirSel], sizeof keep);
        p->nDirs = 0; p->dirSel = -1;
        char lname[64]; lower(p->name, lname, sizeof lname);
        int preferred = -1;
        for (int k = 0; k < l.n && p->nDirs < CPL_MAX_DIRS; k++) {
            const char *nm = l.names[k];
            if (nm[0] == '.' || !strcmp(nm, "precice-run") || !strcmp(nm, "tools") || !strcmp(nm, "images") || !strcmp(nm, "reference-results")) continue;
            char sub[PL_PATH], pd[PL_PATH], who[64] = "", lnm[128];
            pl_join(sub, sizeof sub, c->path, nm);
            pl_join(pd, sizeof pd, sub, "system/preciceDict");
            bool byDict = pl_is_file(pd) && read_word(pd, "participant", who, sizeof who) && !strcmp(who, p->name);
            lower(nm, lnm, sizeof lnm);
            size_t ln = strlen(lname);
            bool byName = !strncmp(lnm, lname, ln) && (lnm[ln] == 0 || lnm[ln] == '-' || lnm[ln] == '_');
            if (pl_is_file(pd) && !byDict && who[0]) {             /* an OpenFOAM case of another participant? */
                for (int o = 0; o < c->n; o++) if (!strcmp(c->p[o].name, who)) byName = false;
                if (byName) byDict = true;                         /* named like this participant, preciceDict misspelt: the checks say so */
            }
            if (!byDict && !byName) continue;
            pl_strlcpy(p->dirs[p->nDirs], nm, sizeof p->dirs[0]);
            if (byDict && preferred < 0) preferred = p->nDirs;
            if (keep[0] && !strcmp(keep, nm)) p->dirSel = p->nDirs;
            p->nDirs++;
        }
        if (p->dirSel < 0) p->dirSel = preferred >= 0 ? preferred : p->nDirs ? 0 : -1;
    }
    names_free(&l);
    for (int i = 0; i < c->n; i++) describe(c, &c->p[i]);
}

Coupling *coupling_open(const char *path)
{
    char p[PL_PATH]; pl_join(p, sizeof p, path, "precice-config.xml");
    FDFile *f = fd_load(p);
    if (!f) return NULL;
    Coupling *c = calloc(1, sizeof *c);
    pl_strlcpy(c->path, path, sizeof c->path);
    pl_strlcpy(c->name, pl_basename(path), sizeof c->name);
    c->cfg = f;
    coupling_refresh(c);
    load_choices(c);
    for (int i = 0; i < c->n; i++) describe(c, &c->p[i]);
    return c;
}

void coupling_close(Coupling *c)
{
    if (!c) return;
    for (int i = 0; i < c->n; i++) { free(c->p[i].itWin); free(c->p[i].itN); free(c->p[i].cvX); free(c->p[i].cvR); }
    xml_free(&c->tree);
    fd_free(c->cfg);
    free(c);
}

/* ============================================================================ running */
static void set_subdomains(const char *dir, int n)
{
    char p[PL_PATH]; pl_join(p, sizeof p, dir, "system/decomposeParDict");
    FDFile *f = fd_load(p);
    if (!f) {
        char body[200]; snprintf(body, sizeof body, "FoamFile\n{\n    version     2.0;\n    format      ascii;\n    class       dictionary;\n"
                                                    "    object      decomposeParDict;\n}\n\nnumberOfSubdomains %d;\n\nmethod          scotch;\n", n);
        pl_write_file(p, body, strlen(body));
        return;
    }
    char v[32]; snprintf(v, sizeof v, "%d", n);
    fd_put(f, "numberOfSubdomains", v);
    char m[64] = "", w[64] = "";
    if (fd_get(f, "method", m, sizeof m) && fd_word(m, w, sizeof w) && (!strcmp(w, "simple") || !strcmp(w, "hierarchical"))) {
        const char *paths[] = { "simpleCoeffs/n", "hierarchicalCoeffs/n", "coeffs/n", "n" };
        bool okN = false;
        for (int i = 0; i < 4 && !okN; i++) {
            char nv[128]; long a, b, cc;
            const char *q;
            if (fd_get(f, paths[i], nv, sizeof nv) && (q = strchr(nv, '(')) && sscanf(q + 1, "%ld %ld %ld", &a, &b, &cc) == 3) { okN = a * b * cc == n; break; }
        }
        if (!okN) fd_put(f, "method", "scotch");
    }
    if (f->dirty) fd_save(f);
    fd_free(f);
}

void coupling_default_cmd(const Coupling *c, const CParticipant *p, char *buf, int cap)
{
    (void)c;
    buf[0] = 0;
    const char *ovs = "OVS=; mpirun --version 2>&1 | grep -q 'Open MPI' && OVS=--oversubscribe; ";
    if (p->hasRunSh) snprintf(buf, (size_t)cap, p->foam && p->procs > 1 ? "./run.sh -parallel" : "./run.sh");
    else if (p->foam && p->solver[0]) {
        if (p->procs > 1)
            snprintf(buf, (size_t)cap, "decomposePar -force > log.decomposePar 2>&1 && %smpirun $OVS -np %d %s -parallel && "
                     "reconstructPar > log.reconstructPar 2>&1", ovs, p->procs, p->solver);
        else snprintf(buf, (size_t)cap, "%s", p->solver);
    }
}

/* participants whose OpenFOAM case already holds results that a new coupled run overwrites */
int coupling_existing_results(Coupling *c, char *buf, int cap)
{
    int n = 0, k = 0;
    buf[0] = 0;
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
        if (!p->foam || !d[0]) continue;
        CaseSummary *s = calloc(1, sizeof *s);
        if (!s) continue;
        case_summarize(d, s);
        const char *l = case_latest_result(s);
        if (l[0] && k < cap) { k += snprintf(buf + k, (size_t)(cap - k), "%s%s up to t = %s", n ? ", " : "", pl_basename(d), l); n++; }
        free(s);
    }
    return n;
}

/* preCICE leaves precice-run/ (connection addresses) behind after a crash; a new run would wait on it */
static void clear_stale_exchange(Coupling *c)
{
    char p[PL_PATH];
    pl_join(p, sizeof p, c->path, "precice-run");
    if (pl_is_dir(p)) pl_remove_tree(p);
    XNode *r = coupling_root(c);
    for (int i = 0; r && i < r->nChild; i++) {
        if (!xml_tag_is(&r->child[i], "m2n:")) continue;
        char ed[256] = ".";
        xml_attr(c->cfg->text, &r->child[i], "exchange-directory", ed, sizeof ed);
        for (int k = 0; k < c->n; k++) {
            char d[PL_PATH], e[PL_PATH], q[PL_PATH];
            coupling_dir(c, &c->p[k], d, sizeof d);
            if (!d[0]) continue;
            if (ed[0] == '/') pl_strlcpy(e, ed, sizeof e); else pl_join(e, sizeof e, d, ed);
            pl_join(q, sizeof q, e, "precice-run");
            char rq[PL_PATH];
            if (realpath(q, rq) && pl_is_dir(rq) && !strcmp(pl_basename(rq), "precice-run")) pl_remove_tree(rq);
        }
    }
}

bool coupling_start(Coupling *c, const char *bashrc, const char *preciceEnv, double now, char *err, int cap)
{
    err[0] = 0;
    if (c->running) { snprintf(err, (size_t)cap, "The coupled run is already going."); return false; }
    if (c->cfg->dirty && !fd_save(c->cfg)) { snprintf(err, (size_t)cap, "Could not save precice-config.xml."); return false; }
    char cmds[CPL_MAX_PART][700];
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
        if (!d[0]) { snprintf(err, (size_t)cap, "No folder for participant %s.", p->name); return false; }
        if (p->cmd[0]) pl_strlcpy(cmds[i], p->cmd, sizeof cmds[i]);
        else coupling_default_cmd(c, p, cmds[i], sizeof cmds[i]);
        if (!cmds[i][0]) { snprintf(err, (size_t)cap, "%s has no run.sh and is not an OpenFOAM case: give it a command.", p->name); return false; }
    }
    clear_stale_exchange(c);
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
        if (p->foam && p->procs > 1) set_subdomains(d, p->procs);
        char qd[PL_PATH + 8], qb[PL_PATH + 8], qe[PL_PATH + 8], logp[PL_PATH], ln[96];
        pl_shell_quote(qd, sizeof qd, d);
        pl_shell_quote(qb, sizeof qb, bashrc ? bashrc : "");
        pl_shell_quote(qe, sizeof qe, preciceEnv ? preciceEnv : "");
        Str s; str_init(&s);
        str_catf(&s, "cd %s || exit 1\n", qd);
        if (bashrc && *bashrc) str_catf(&s, "source %s > /dev/null 2>&1\n", qb);
        if (preciceEnv && *preciceEnv) str_catf(&s, "source %s > /dev/null 2>&1\n", qe);
        str_catf(&s, "echo \"Q-Continuum: participant %s, $(date)\"\n%s\n", p->name, cmds[i]);
        snprintf(ln, sizeof ln, "log.%s", p->name);
        pl_join(logp, sizeof logp, d, ln);
        p->pid = pl_spawn_shell(s.s, logp);
        str_free(&s);
        p->running = p->pid > 0; p->done = p->failed = p->stopped = false; p->exitCode = 0;
        p->t0 = now; p->t1 = 0;
        p->nIt = p->nCv = 0; p->itMtime = p->cvMtime = 0; p->itSize = p->cvSize = 0;
        if (!p->running) {
            snprintf(err, (size_t)cap, "Could not start %s.", p->name);
            coupling_stop(c, now);
            return false;
        }
    }
    c->running = true; c->stopping = false;
    c->startedAt = now;
    return true;
}

void coupling_poll(Coupling *c, double now)
{
    if (!c->running) return;
    bool any = false, failed = false;
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        if (!p->running) { failed |= p->failed; continue; }
        int st = 0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == 0) { any = true; continue; }
        p->running = false; p->done = true; p->t1 = now;
        p->exitCode = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
        p->failed = p->exitCode != 0 && !c->stopping;
        p->stopped = c->stopping;
        failed |= p->failed;
    }
    /* one participant failed: the others would wait for it forever */
    if (any && failed && !c->stopping) coupling_stop(c, now);
    if (any && c->stopping && now - c->stopAt > 4)
        for (int i = 0; i < c->n; i++) if (c->p[i].running) kill(-c->p[i].pid, SIGKILL);
    if (!any) c->running = c->stopping = false;
}

void coupling_stop(Coupling *c, double now)
{
    c->stopping = true; c->stopAt = now;
    for (int i = 0; i < c->n; i++) if (c->p[i].running && c->p[i].pid > 0) kill(-c->p[i].pid, SIGTERM);
}

/* ============================================================================ preCICE logs */
static void push2(float **a, float **b, int *n, int *cap, float x, float y)
{
    if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *a = realloc(*a, sizeof(float) * (size_t)*cap); *b = realloc(*b, sizeof(float) * (size_t)*cap); }
    (*a)[*n] = x; (*b)[*n] = y; (*n)++;
}

void coupling_read_precice_logs(Coupling *c)
{
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        char d[PL_PATH], f[PL_PATH], nm[128];
        coupling_dir(c, p, d, sizeof d);
        if (!d[0]) continue;
        snprintf(nm, sizeof nm, "precice-%s-iterations.log", p->name);
        pl_join(f, sizeof f, d, nm);
        time_t m = pl_mtime(f);
        long long sz = pl_file_size(f);
        if (m && (m != p->itMtime || sz != p->itSize)) {
            p->itMtime = m; p->itSize = sz; p->nIt = 0;
            size_t len; char *t = pl_read_file(f, &len);
            for (char *line = t ? strtok(t, "\n") : NULL; line; line = strtok(NULL, "\n")) {
                double w, tot, it;
                if (sscanf(line, "%lf %lf %lf", &w, &tot, &it) == 3) push2(&p->itWin, &p->itN, &p->nIt, &p->capIt, (float)w, (float)it);
            }
            free(t);
        }
        snprintf(nm, sizeof nm, "precice-%s-convergence.log", p->name);
        pl_join(f, sizeof f, d, nm);
        m = pl_mtime(f);
        sz = pl_file_size(f);
        if (m && (m != p->cvMtime || sz != p->cvSize)) {
            p->cvMtime = m; p->cvSize = sz; p->nCv = 0;
            size_t len; char *t = pl_read_file(f, &len);
            bool head = true; int k = 0;
            for (char *line = t ? strtok(t, "\n") : NULL; line; line = strtok(NULL, "\n")) {
                if (head) {                                       /* "TimeWindow  Iteration  ResRel(Temperature) ..." */
                    char a[64], b[64], col[64] = "";
                    if (sscanf(line, "%63s %63s %63s", a, b, col) == 3) pl_strlcpy(p->cvName, col, sizeof p->cvName);
                    head = false;
                    continue;
                }
                double w, it, r;
                if (sscanf(line, "%lf %lf %lf", &w, &it, &r) == 3) push2(&p->cvX, &p->cvR, &p->nCv, &p->capCv, (float)++k, (float)r);
            }
            free(t);
        }
    }
}

/* ============================================================================ checks */
static void add(CCheck *o, int max, int *n, int kind, const char *who, int fix, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
#include <stdarg.h>
static void add(CCheck *o, int max, int *n, int kind, const char *who, int fix, const char *fmt, ...)
{
    if (*n >= max) return;
    CCheck *c = &o[(*n)++];
    c->kind = kind; c->fix = fix;
    pl_strlcpy(c->who, who, sizeof c->who);
    va_list ap; va_start(ap, fmt); vsnprintf(c->msg, sizeof c->msg, fmt, ap); va_end(ap);
}

static XNode *cfg_participant(Coupling *c, const char *name)
{
    XNode *r = coupling_root(c);
    for (int i = 0; r && i < r->nChild; i++) {
        if (strcmp(r->child[i].tag, "participant")) continue;
        char nm[64]; xml_attr(c->cfg->text, &r->child[i], "name", nm, sizeof nm);
        if (!strcmp(nm, name)) return &r->child[i];
    }
    return NULL;
}

static bool part_has(Coupling *c, XNode *part, const char *tag, const char *attr, const char *val, const char *attr2, const char *val2)
{
    for (int i = 0; part && i < part->nChild; i++) {
        if (strcmp(part->child[i].tag, tag)) continue;
        char v[128], v2[128];
        xml_attr(c->cfg->text, &part->child[i], attr, v, sizeof v);
        if (strcmp(v, val)) continue;
        if (!attr2) return true;
        xml_attr(c->cfg->text, &part->child[i], attr2, v2, sizeof v2);
        if (!strcmp(v2, val2)) return true;
    }
    return false;
}

/* names inside a list value: "(Temperature Heat-Flux)" or "( interface )" */
static int list_words(const char *v, char out[][64], int max)
{
    int n = 0;
    while (*v && n < max) {
        while (*v && !(isalnum((unsigned char)*v) || *v == '_' || *v == '-' || *v == '.')) v++;
        int k = 0;
        while (*v && (isalnum((unsigned char)*v) || *v == '_' || *v == '-' || *v == '.') && k < 63) out[n][k++] = *v++;
        out[n][k] = 0;
        if (k) n++;
    }
    return n;
}

static bool adapter_lib_present(void)
{
    glob_t g;
    char pat[PL_PATH];
    snprintf(pat, sizeof pat, "%s/OpenFOAM/*/platforms/*/lib/libpreciceAdapterFunctionObject.so", pl_home());
    bool ok = glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0;
    if (glob(pat, 0, NULL, &g) == 0) globfree(&g);
    return ok;
}


/* ---------------------------------------------------------------- what a participant needs to start
   The tutorials' run.sh scripts fail a second into a coupled run when their solver isn't installed (ccx_preCICE),
   a Python module is missing (fenics), or a downloaded mesh was never fetched. These read run.sh and say so first. */
static bool shell_word_skip(const char *w)
{
    static const char *const skip[] = { "set", ".", "source", "exec", "export", "echo", "if", "then", "fi", "else", "elif", "cd",
        "close_log", "rm", "mkdir", "cp", "mv", "sed", "tee", "exit", "wait", "[", "[[", "for", "do", "done", "while", "case",
        "esac", "true", "false", "local", "unset", "return", "shift", "trap", "printf", "test", "{", "}", "!", "time", "env",
        "cat", "ln", "touch", "grep", "awk", "tar", "wget", "curl", "mpirun", NULL };
    if (!w[0] || w[0] == '#' || w[0] == '$' || w[0] == '"' || w[0] == '\'' || w[0] == '-' || w[0] == '>' || w[0] == '<' || w[0] == '(' || w[0] == ')') return true;
    if (strchr(w, '=')) return true;                         /* VAR=value */
    if (!strncmp(w, "../", 3)) return true;                  /* the tutorials' shared tools */
    for (int i = 0; skip[i]; i++) if (!strcmp(w, skip[i])) return true;
    return false;
}

/* top-level modules a Python script imports (import a, b / from a.b import c), at most `max` */
static int py_imports(const char *path, char mods[][48], int max)
{
    char *t = pl_read_file(path, NULL);
    if (!t) return 0;
    int n = 0;
    for (char *sv = NULL, *l = strtok_r(t, "\n", &sv); l && n < max; l = strtok_r(NULL, "\n", &sv)) {
        while (*l == ' ' || *l == '\t') l++;
        const char *r = NULL;
        if (!strncmp(l, "import ", 7)) r = l + 7;
        else if (!strncmp(l, "from ", 5)) r = l + 5;
        if (!r) continue;
        for (;;) {
            while (*r == ' ' || *r == ',') r++;
            char m[48]; int k = 0;
            while ((isalnum((unsigned char)*r) || *r == '_') && k < 47) m[k++] = *r++;
            m[k] = 0;
            if (!m[0]) break;
            bool dup = false;
            for (int i = 0; i < n; i++) if (!strcmp(mods[i], m)) dup = true;
            if (!dup && n < max) pl_strlcpy(mods[n++], m, 48);
            while (*r && *r != ',' && *r != ' ') r++;              /* .sub, " as x" */
            if (!strncmp(r, " as ", 4)) { r += 4; while (isalnum((unsigned char)*r) || *r == '_') r++; }
            if (*r != ',' || !strncmp(l, "from ", 5)) break;
        }
    }
    free(t);
    return n;
}

/* which of these modules python3 can't find (find_spec: nothing is imported). python3 takes ~0.5 s to answer and the
   checks are drawn every frame, so it runs in a forked child: until it answers, nothing is reported. Answers are
   kept for 30 s per module list. */
static void py_missing(char mods[][48], int n, char *out, int cap)
{
    static struct { char key[1024], res[512]; time_t at; bool done; pid_t pid; int fd; size_t got; } Q = { .fd = -1 };
    char key[1024] = ""; int k = 0;
    for (int i = 0; i < n && k < (int)sizeof key - 50; i++) k += snprintf(key + k, sizeof key - (size_t)k, " %s", mods[i]);
    out[0] = 0;
    if (Q.fd >= 0 && !strcmp(Q.key, key)) {                 /* the answer on its way: take what has come */
        ssize_t r = 0;
        while (Q.got < sizeof Q.res - 1 && (r = read(Q.fd, Q.res + Q.got, sizeof Q.res - 1 - Q.got)) > 0) Q.got += (size_t)r;
        if (r == 0 || Q.got >= sizeof Q.res - 1) {
            close(Q.fd); Q.fd = -1; waitpid(Q.pid, NULL, 0);
            Q.res[Q.got] = 0; pl_trim_inplace(Q.res);
            Q.done = true; Q.at = time(NULL);
        } else return;
    }
    if (Q.done && !strcmp(Q.key, key) && time(NULL) - Q.at < 30) { pl_strlcpy(out, Q.res, (size_t)cap); return; }
    if (Q.fd >= 0) { close(Q.fd); kill(Q.pid, SIGKILL); waitpid(Q.pid, NULL, 0); Q.fd = -1; }   /* another list now */
    int pf[2];
    if (pipe(pf) != 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return; }
    if (pid == 0) {
        close(pf[0]);
        char cmd[1400], res[512] = "";
        snprintf(cmd, sizeof cmd, "python3 -c 'import importlib.util,sys; print(\" \".join(m for m in sys.argv[1:] if importlib.util.find_spec(m) is None))'%s 2>/dev/null", key);
        if (pl_run_wait(cmd, res, sizeof res) != 0) snprintf(res, sizeof res, "python3");   /* no python3 at all */
        ssize_t wr = write(pf[1], res, strlen(res));
        _exit(wr < 0);
    }
    close(pf[1]);
    fcntl(pf[0], F_SETFL, O_NONBLOCK);
    Q.fd = pf[0]; Q.pid = pid; Q.got = 0; Q.done = false;
    pl_strlcpy(Q.key, key, sizeof Q.key);
}

/* what folder d's start command needs that isn't there: missing commands, Python modules and files it tests for.
   Returns how many problems; `what` gets them in words. */
int coupling_missing(const char *d, const char *cmd, char *what, int cap)
{
    what[0] = 0;
    int nProb = 0, w = 0;
    char rs[PL_PATH]; pl_join(rs, sizeof rs, d, "run.sh");
    char *t = cmd && cmd[0] ? strdup(cmd) : pl_read_file(rs, NULL);
    if (!t) return 0;
    char cmds[8][64]; int nc = 0;
    char mods[24][48]; int nm = 0;
    char files[8][128]; int nf = 0;
    for (char *sv = NULL, *l = strtok_r(t, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '#') continue;
        /* [ ! -f all.msh ] and the like: a file the script insists on */
        const char *fx = strstr(l, "! -f ");
        if (!fx) fx = strstr(l, "! -e ");
        if (fx && nf < 8) {
            fx += 5; while (*fx == ' ' || *fx == '"') fx++;
            int k = 0; while (fx[k] && fx[k] != ' ' && fx[k] != '"' && fx[k] != ']' && k < 127) { files[nf][k] = fx[k]; k++; }
            files[nf][k] = 0;
            if (files[nf][0] && files[nf][0] != '$') nf++;
            continue;
        }
        /* redirections aren't commands: 2>&1, >&2, &>log, >(tee ...) */
        for (char *r = l; *r; r++) {
            if ((*r == '>' || *r == '<') && r[1] == '&') { r[1] = ' '; if (isdigit((unsigned char)r[2])) r[2] = ' '; }
            if (*r == '&' && r[1] == '>') *r = ' ';
            if (*r == '>' || *r == '<') *r = ' ';
        }
        /* the words that start commands: the first of the line, and after && || ; | */
        char *z = l;
        bool start = true, afterPy = false;
        while (*z) {
            while (*z == ' ' || *z == '\t') z++;
            if (!*z || *z == '#') break;
            char word[256]; int k = 0;
            while (*z && *z != ' ' && *z != '\t' && *z != ';' && *z != '|' && *z != '&' && k < 255) word[k++] = *z++;
            word[k] = 0;
            bool sep = false;
            while (*z == ';' || *z == '|' || *z == '&') { z++; sep = true; }
            if (afterPy && word[0] != '-') {                 /* python3 solid.py: the script's imports */
                afterPy = false;
                char sp[PL_PATH]; pl_join(sp, sizeof sp, d, word);
                if (pl_ends_with(word, ".py") && pl_is_file(sp)) nm += py_imports(sp, mods + nm, 24 - nm);
            } else if (start && !strcmp(word, "exec")) {
                /* exec <cmd>: the next word starts the command */
            } else if (start) {
                start = false;
                if (!strcmp(word, "python3") || !strcmp(word, "python")) afterPy = true;
                else if (!strncmp(word, "./", 2)) {
                    char sp[PL_PATH]; pl_join(sp, sizeof sp, d, word + 2);
                    if (pl_ends_with(word, ".py") && pl_is_file(sp)) nm += py_imports(sp, mods + nm, 24 - nm);
                } else if (!shell_word_skip(word) && !pl_which(word, NULL, 0) && nc < 8) {
                    bool dup = false; for (int i = 0; i < nc; i++) if (!strcmp(cmds[i], word)) dup = true;
                    if (!dup) pl_strlcpy(cmds[nc++], word, 64);
                }
                if (!strcmp(word, "exec")) start = true;
            }
            if (sep) start = true;
        }
    }
    free(t);
    for (int i = 0; i < nc; i++) {
        w += snprintf(what + w, (size_t)(cap > w ? cap - w : 0), "%s%s is not installed (not on PATH)", w ? "; " : "", cmds[i]);
        nProb++;
    }
    if (nm) {
        char miss[512]; py_missing(mods, nm, miss, sizeof miss);
        if (miss[0]) {
            w += snprintf(what + w, (size_t)(cap > w ? cap - w : 0), "%sPython can't import %s", w ? "; " : "", miss);
            nProb++;
        }
    }
    for (int i = 0; i < nf; i++) {
        char fp[PL_PATH]; pl_join(fp, sizeof fp, d, files[i]);
        if (pl_exists(fp)) continue;
        w += snprintf(what + w, (size_t)(cap > w ? cap - w : 0), "%s%s is missing", w ? "; " : "", files[i]);
        nProb++;
    }
    return nProb;
}

int coupling_check(Coupling *c, CCheck *o, int max)
{
    int n = 0;
    if (!c->tree.ok) { add(o, max, &n, 2, "precice-config.xml", 0, "Not valid XML: %s.", c->tree.err); return n; }
    if (!coupling_root(c)) { add(o, max, &n, 2, "precice-config.xml", 0, "No <precice-configuration> element."); return n; }
    XNode *cs = coupling_scheme(c);
    if (!cs) add(o, max, &n, 2, "precice-config.xml", 0, "No coupling scheme (<coupling-scheme:serial-implicit> or similar).");
    if (c->n < 2) add(o, max, &n, 2, "precice-config.xml", 0, "A coupling needs at least two participants; the configuration has %d.", c->n);
    if (!adapter_lib_present())
        add(o, max, &n, 1, "OpenFOAM", 0, "libpreciceAdapterFunctionObject.so is not in $FOAM_USER_LIBBIN: OpenFOAM participants cannot load the adapter. Build the openfoam-adapter (./Allwmake).");
    for (int i = 0; i < c->n; i++) {
        CParticipant *p = &c->p[i];
        int n0 = n;
        char d[PL_PATH]; coupling_dir(c, p, d, sizeof d);
        if (!d[0]) {
            char ln[64]; lower(p->name, ln, sizeof ln);
            add(o, max, &n, 2, p->name, 0, "No folder found. Make a folder named %s-<solver>/ next to precice-config.xml, or an OpenFOAM case whose system/preciceDict says participant %s.", ln, p->name);
            continue;
        }
        char dl[PL_PATH]; pl_join(dl, sizeof dl, c->path, "download-meshes.sh");
        bool canDownload = pl_is_file(dl);
        if (!p->foam) {
            char why[600];
            if (!p->hasRunSh && !p->cmd[0]) add(o, max, &n, 2, p->name, 0, "%s/ is not an OpenFOAM case and has no run.sh: set the command that starts this solver.", p->dirs[p->dirSel]);
            else if (coupling_missing(d, p->cmd, why, sizeof why) > 0) {
                /* another folder for the same participant that can start: offer it */
                int alt = -1;
                for (int k = 0; k < p->nDirs && alt < 0; k++) {
                    if (k == p->dirSel) continue;
                    char ad[PL_PATH], aw[600]; pl_join(ad, sizeof ad, c->path, p->dirs[k]);
                    if (coupling_missing(ad, NULL, aw, sizeof aw) == 0) alt = k;
                }
                bool files = strstr(why, " is missing") != NULL;
                if (alt >= 0) add(o, max, &n, 2, p->name, 100 + alt, "%s/ can't start: %s. %s/ also provides %s and has what it needs.", p->dirs[p->dirSel], why, p->dirs[alt], p->name);
                else if (files && canDownload) add(o, max, &n, 2, p->name, 2, "%s/ can't start: %s. This tutorial fetches its meshes with download-meshes.sh.", p->dirs[p->dirSel], why);
                else add(o, max, &n, 2, p->name, 0, "%s/ can't start: %s.", p->dirs[p->dirSel], why);
            }
            else add(o, max, &n, 0, p->name, 0, "Runs %s in %s/.", p->cmd[0] ? p->cmd : "./run.sh", p->dirs[p->dirSel]);
            continue;
        }
        /* an OpenFOAM case without a mesh or a way to make one */
        {
            char m1[PL_PATH], m2[PL_PATH], m3[PL_PATH], m4[PL_PATH];
            pl_join(m1, sizeof m1, d, "constant/polyMesh/boundary"); pl_join(m2, sizeof m2, d, "system/blockMeshDict");
            pl_join(m3, sizeof m3, d, "system/snappyHexMeshDict"); pl_join(m4, sizeof m4, d, "constant/polyMesh/blockMeshDict");
            if (!pl_is_file(m1) && !pl_is_file(m2) && !pl_is_file(m3) && !pl_is_file(m4)) {
                if (canDownload) add(o, max, &n, 2, p->name, 2, "%s/ has no mesh (constant/polyMesh) and nothing to make one. This tutorial fetches its meshes with download-meshes.sh.", p->dirs[p->dirSel]);
                else add(o, max, &n, 2, p->name, 0, "%s/ has no mesh (constant/polyMesh) and no blockMeshDict or snappyHexMeshDict to make one.", p->dirs[p->dirSel]);
                continue;
            }
        }
        char q[PL_PATH]; pl_join(q, sizeof q, d, "system/preciceDict");
        FDFile *pd = fd_load(q);
        if (!pd) { add(o, max, &n, 2, p->name, 0, "%s/system/preciceDict is missing: the adapter reads its participant name and interfaces from it.", p->dirs[p->dirSel]); continue; }
        char who[64] = "", v[512];
        if (fd_get(pd, "participant", v, sizeof v)) fd_word(v, who, sizeof who);
        for (char *z = who; *z; z++) if (*z == ';') *z = 0;
        if (strcmp(who, p->name)) add(o, max, &n, 2, p->name, 0, "preciceDict says participant %s, the configuration calls it %s.", who[0] ? who : "(none)", p->name);
        if (fd_get(pd, "preciceConfig", v, sizeof v)) {
            char rel[PL_PATH] = ""; int k = 0;
            for (const char *z = v; *z && k < PL_PATH - 1; z++) if (*z != '"' && *z != ';') rel[k++] = *z;
            rel[k] = 0; pl_trim_inplace(rel);
            char full[PL_PATH], a[PL_PATH], b[PL_PATH], cfgp[PL_PATH];
            if (rel[0] == '/') pl_strlcpy(full, rel, sizeof full); else pl_join(full, sizeof full, d, rel);
            pl_join(cfgp, sizeof cfgp, c->path, "precice-config.xml");
            if (!realpath(full, a) || !realpath(cfgp, b) || strcmp(a, b))
                add(o, max, &n, 2, p->name, 0, "preciceDict points to %s, which is not this precice-config.xml.", rel);
        }
        /* the adapter function object */
        char cdp[PL_PATH]; pl_join(cdp, sizeof cdp, d, "system/controlDict");
        FDFile *cd = fd_load(cdp);
        bool fo = false;
        FDEntry *fn = cd ? fd_find(cd, "functions") : NULL;
        for (int k = 0; fn && k < fn->nChild; k++) {
            FDEntry *t = fd_child(&fn->child[k], "type");
            char tv[96] = "";
            if (t) fd_entry_value(cd, t, tv, sizeof tv);
            if (strstr(tv, "preciceAdapterFunctionObject")) fo = true;
        }
        if (cd && !fo && strstr(cd->text, "preciceAdapterFunctionObject")) fo = true;   /* e.g. via #include */
        if (!fo) add(o, max, &n, 2, p->name, 1, "controlDict has no preciceAdapterFunctionObject in functions, so %s never talks to preCICE.", p->solver);
        fd_free(cd);
        /* interfaces against the configuration and the mesh */
        XNode *cp = cfg_participant(c, p->name);
        char bp[PL_PATH]; pl_join(bp, sizeof bp, d, "constant/polyMesh/boundary");
        FDFile *bnd = fd_load(bp);
        if (!bnd) { pl_join(bp, sizeof bp, d, "system/blockMeshDict"); bnd = fd_load(bp); }
        FDEntry *ifs = fd_find(pd, "interfaces");
        int nIf = 0;
        for (int k = 0; ifs && k < ifs->nChild; k++) {
            FDEntry *it = &ifs->child[k];
            if (it->kind != FD_DICT) continue;
            nIf++;
            char mesh[96] = "", pv[512] = "";
            FDEntry *me = fd_child(it, "mesh");
            if (me) { fd_entry_value(pd, me, v, sizeof v); fd_word(v, mesh, sizeof mesh); }
            if (!part_has(c, cp, "provide-mesh", "name", mesh, NULL, NULL))
                add(o, max, &n, 2, p->name, 0, "Interface %s uses mesh %s, which %s does not provide in precice-config.xml.", it->key, mesh, p->name);
            FDEntry *pe = fd_child(it, "patches");
            char words[16][64];
            if (pe && bnd) {
                fd_entry_value(pd, pe, pv, sizeof pv);
                int nw = list_words(pv, words, 16);
                for (int w = 0; w < nw; w++) {
                    bool found = strstr(bnd->text, words[w]) != NULL;
                    if (!found) add(o, max, &n, 2, p->name, 0, "Interface patch %s does not exist in %s.", words[w], strstr(bp, "polyMesh") ? "constant/polyMesh/boundary" : "blockMeshDict");
                }
            }
            const char *rw[2] = { "readData", "writeData" }, *tag[2] = { "read-data", "write-data" };
            for (int s = 0; s < 2; s++) {
                FDEntry *de = fd_child(it, rw[s]);
                if (!de) continue;
                fd_entry_value(pd, de, pv, sizeof pv);
                int nw = list_words(pv, words, 16);
                for (int w = 0; w < nw; w++)
                    if (!part_has(c, cp, tag[s], "name", words[w], "mesh", mesh))
                        add(o, max, &n, 2, p->name, 0, "preciceDict %s %s on %s, but precice-config.xml has no <%s name=\"%s\" mesh=\"%s\"/> for %s.",
                            s ? "writes" : "reads", words[w], mesh, tag[s], words[w], mesh, p->name);
            }
        }
        if (!nIf) add(o, max, &n, 2, p->name, 0, "preciceDict defines no interfaces.");
        if (!fd_find(pd, "modules")) add(o, max, &n, 1, p->name, 0, "preciceDict has no modules entry (CHT, FSI, FF or Generic).");
        fd_free(bnd);
        fd_free(pd);
        bool problems = false;
        for (int k = n0; k < n; k++) if (o[k].kind > 0 && !strcmp(o[k].who, p->name)) problems = true;
        if (!problems)
            add(o, max, &n, 0, p->name, 0, "OpenFOAM case %s/ runs %s%s; preciceDict, controlDict and the interface patches agree with the configuration.",
                p->dirs[p->dirSel], p->solver, p->procs > 1 ? " in parallel" : "");
    }
    /* exchanges name participants and data that exist */
    for (int i = 0; cs && i < cs->nChild; i++) {
        if (strcmp(cs->child[i].tag, "exchange")) continue;
        char fr[64], to[64];
        xml_attr(c->cfg->text, &cs->child[i], "from", fr, sizeof fr);
        xml_attr(c->cfg->text, &cs->child[i], "to", to, sizeof to);
        if (!cfg_participant(c, fr) || !cfg_participant(c, to))
            add(o, max, &n, 2, "coupling scheme", 0, "An exchange goes from %s to %s, but both must be participants.", fr, to);
    }
    return n;
}

bool coupling_fix_adapter(Coupling *c, int part)
{
    if (part < 0 || part >= c->n) return false;
    char d[PL_PATH], p[PL_PATH];
    coupling_dir(c, &c->p[part], d, sizeof d);
    pl_join(p, sizeof p, d, "system/controlDict");
    FDFile *f = fd_load(p);
    if (!f) return false;
    bool ok = fd_ensure_dict(f, "functions") && fd_put(f, "functions/preCICE_Adapter/type", "preciceAdapterFunctionObject") &&
              fd_put(f, "functions/preCICE_Adapter/libs", "(\"libpreciceAdapterFunctionObject.so\")") && fd_save(f);
    fd_free(f);
    return ok;
}
