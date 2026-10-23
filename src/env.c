/* env.c -- installation detection, settings file, external launchers */
#define _GNU_SOURCE
#include "env.h"
#include "program.h"

#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================ detection */
static void version_from_dir(const char *dir, char *out, int cap)
{
    const char *b = pl_basename(dir);
    const char *p = b;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) { pl_strlcpy(out, b, (size_t)cap); return; }
    bool esi = strcasestr(b, "openfoam2") || strstr(b, "-v");
    snprintf(out, (size_t)cap, "%s%s", esi ? "v" : "", p);
}

static int version_key(const char *v)
{
    const char *p = v;
    while (*p && !isdigit((unsigned char)*p)) p++;
    int n = atoi(p);
    return n < 100 ? n * 100 : n;           /* "12" (Foundation) sorts below "v2606" (ESI) */
}

static void add_install(Env *e, const char *root)
{
    char rc[PL_PATH];
    pl_join(rc, sizeof rc, root, "etc/bashrc");
    if (!pl_is_file(rc) || e->nInst >= 16) return;
    char real[PL_PATH];
    if (!realpath(root, real)) pl_strlcpy(real, root, sizeof real);
    for (int i = 0; i < e->nInst; i++) if (!strcmp(e->inst[i].root, real)) return;
    FoamInstall *f = &e->inst[e->nInst++];
    pl_strlcpy(f->root, real, sizeof f->root);
    pl_join(f->bashrc, sizeof f->bashrc, real, "etc/bashrc");
    version_from_dir(real, f->version, sizeof f->version);
    char t[PL_PATH]; pl_join(t, sizeof t, real, "tutorials");
    f->hasTutorials = pl_is_dir(t);
}

static void glob_installs(Env *e, const char *pattern)
{
    glob_t g;
    if (glob(pattern, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) add_install(e, g.gl_pathv[i]);
        globfree(&g);
    }
}

void env_detect(Env *e)
{
    memset(e, 0, sizeof *e);
    const char *only = getenv("QC_TEST_FOAM_ROOTS");    /* testing: search just these (":"-separated; "" = none installed) */
    if (only) {
        char buf[PL_PATH * 4]; pl_strlcpy(buf, only, sizeof buf);
        for (char *sv = NULL, *r = strtok_r(buf, ":", &sv); r; r = strtok_r(NULL, ":", &sv)) add_install(e, r);
    }
    const char *wm = only ? NULL : getenv("WM_PROJECT_DIR");
    if (wm) add_install(e, wm);
    if (!only) {
        glob_installs(e, "/usr/lib/openfoam/openfoam*");
        glob_installs(e, "/opt/openfoam*");
        glob_installs(e, "/opt/OpenFOAM*");
        glob_installs(e, "/opt/OpenFOAM/OpenFOAM-*");
        char pat[PL_PATH];
        snprintf(pat, sizeof pat, "%s/OpenFOAM/OpenFOAM-*", pl_home()); glob_installs(e, pat);
        snprintf(pat, sizeof pat, "%s/openfoam*", pl_home()); glob_installs(e, pat);
    }
    for (int i = 0; i < e->nInst; i++)                          /* newest first */
        for (int k = i + 1; k < e->nInst; k++)
            if (version_key(e->inst[k].version) > version_key(e->inst[i].version)) {
                FoamInstall t = e->inst[i]; e->inst[i] = e->inst[k]; e->inst[k] = t;
            }

    e->wsl = pl_is_wsl();
    pl_which("paraview", e->pvLinux, sizeof e->pvLinux);
    if (e->wsl) {
        glob_t g;
        if (glob("/mnt/c/Program Files/ParaView*/bin/paraview.exe", 0, NULL, &g) == 0) {
            if (g.gl_pathc) pl_strlcpy(e->pvWin, g.gl_pathv[g.gl_pathc - 1], sizeof e->pvWin);
            globfree(&g);
        }
        e->haveWt = pl_which("wt.exe", NULL, 0);
    }
    pl_which("code", e->code, sizeof e->code);
    /* preCICE: our own build in ~/opt/precice-<ver>/ (with env.sh), or a system install */
    {
        glob_t g; char pat[PL_PATH];
        snprintf(pat, sizeof pat, "%s/opt/precice-*/env.sh", pl_home());
        if (glob(pat, 0, NULL, &g) == 0) {
            if (g.gl_pathc) {
                pl_strlcpy(e->preciceEnv, g.gl_pathv[g.gl_pathc - 1], sizeof e->preciceEnv);
                const char *v = strstr(e->preciceEnv, "precice-");
                if (v) { int k = 0; v += 8; while (v[k] && v[k] != '/' && k < 31) { e->preciceVersion[k] = v[k]; k++; } e->preciceVersion[k] = 0; }
            }
            globfree(&g);
        }
        if (!e->preciceVersion[0] && pl_which("precice-version", NULL, 0)) pl_strlcpy(e->preciceVersion, "system", sizeof e->preciceVersion);
        const char *tut[] = { "src/precice-tutorials", "precice-tutorials", "src/tutorials-precice", NULL };
        for (int i = 0; tut[i]; i++) {
            char t[PL_PATH]; pl_join(t, sizeof t, pl_home(), tut[i]);
            char q[PL_PATH]; pl_join(q, sizeof q, t, "flow-over-heated-plate");
            if (pl_is_dir(q)) { pl_strlcpy(e->preciceTutorials, t, sizeof e->preciceTutorials); break; }
        }
    }
    /* Claude Code: on the PATH, or where its installers put it (a menu launch may not have ~/.local/bin on the PATH) */
    if (!pl_which("claude", e->claude, sizeof e->claude)) {
        const char *cand[] = { "%s/.local/bin/claude", "%s/.claude/local/claude", "%s/.npm-global/bin/claude", NULL };
        for (int i = 0; cand[i] && !e->claude[0]; i++) {
            char p[PL_PATH]; snprintf(p, sizeof p, cand[i], pl_home());
            if (access(p, X_OK) == 0) pl_strlcpy(e->claude, p, sizeof e->claude);
        }
    }
    /* SALOME: a native/universal install (its 'salome' launcher), else the Apptainer launchers on the PATH */
    {
        glob_t g; char pat[PL_PATH];
        snprintf(pat, sizeof pat, "%s/salome/SALOME-*/salome", pl_home());
        if (glob(pat, 0, NULL, &g) == 0) {
            if (g.gl_pathc) {
                pl_strlcpy(e->salome, g.gl_pathv[g.gl_pathc - 1], sizeof e->salome);
                const char *v = strstr(e->salome, "SALOME-");
                if (v) { int k = 0; v += 7; while (v[k] && v[k] != '/' && k < 31) { e->salomeVersion[k] = v[k]; k++; } e->salomeVersion[k] = 0; }
            }
            globfree(&g);
        }
        if (!e->salome[0] && pl_which("salome_cfd", e->salome, sizeof e->salome)) pl_strlcpy(e->salomeVersion, "SALOME_CFD container", sizeof e->salomeVersion);
        if (!e->salome[0] && pl_which("salome_meca", e->salome, sizeof e->salome)) pl_strlcpy(e->salomeVersion, "salome_meca container", sizeof e->salomeVersion);
    }
    pl_which("pvbatch", e->pvbatch, sizeof e->pvbatch);
    if (!pl_which("ffmpeg", e->ffmpeg, sizeof e->ffmpeg)) {          /* conda: ~/miniforge/envs/ffmpeg/bin/ffmpeg and the like */
        const char *pats[] = { "%s/miniforge*/envs/*/bin/ffmpeg", "%s/miniconda*/envs/*/bin/ffmpeg", "%s/anaconda*/envs/*/bin/ffmpeg",
                               "%s/miniforge*/bin/ffmpeg", "%s/miniconda*/bin/ffmpeg", NULL };
        for (int i = 0; pats[i] && !e->ffmpeg[0]; i++) {
            char pat[PL_PATH]; snprintf(pat, sizeof pat, pats[i], pl_home());
            glob_t g;
            if (glob(pat, 0, NULL, &g) == 0) { if (g.gl_pathc) pl_strlcpy(e->ffmpeg, g.gl_pathv[0], sizeof e->ffmpeg); globfree(&g); }
        }
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    e->nCpu = n > 0 ? (int)n : 1;
    const char *tc = getenv("QC_TEST_CPUS");
    if (tc && atoi(tc) > 0) e->nCpu = atoi(tc);
    const char *u = getenv("USER");
    pl_strlcpy(e->user, u ? u : "user", sizeof e->user);
}

const FoamInstall *env_active(const Env *e, const Settings *s)
{
    if (s->bashrc[0]) {
        for (int i = 0; i < e->nInst; i++) if (!strcmp(e->inst[i].bashrc, s->bashrc)) return &e->inst[i];
        static FoamInstall custom;                               /* user-supplied path */
        if (pl_is_file(s->bashrc)) {
            memset(&custom, 0, sizeof custom);
            pl_strlcpy(custom.bashrc, s->bashrc, sizeof custom.bashrc);
            char etc[PL_PATH]; pl_dirname(etc, sizeof etc, s->bashrc);
            pl_dirname(custom.root, sizeof custom.root, etc);
            version_from_dir(custom.root, custom.version, sizeof custom.version);
            char t[PL_PATH]; pl_join(t, sizeof t, custom.root, "tutorials");
            custom.hasTutorials = pl_is_dir(t);
            return &custom;
        }
    }
    return e->nInst ? &e->inst[0] : NULL;
}

void env_tutorials_dir(const Env *e, const Settings *s, char *out, int cap)
{
    const FoamInstall *f = env_active(e, s);
    if (!f) { out[0] = 0; return; }
    pl_join(out, (size_t)cap, f->root, "tutorials");
}

/* ============================================================================ settings */
static void settings_path(char *out, size_t cap)
{
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(out, cap, "%s/q-continuum/settings.ini", x);
    else snprintf(out, cap, "%s/.config/q-continuum/settings.ini", pl_home());
}

void settings_defaults(Settings *s, const Env *e)
{
    names_free(&s->recent);
    names_free(&s->demeDirs);
    memset(s, 0, sizeof *s);
    const FoamInstall *f = e->nInst ? &e->inst[0] : NULL;
    /* OpenFOAM's own convention: $FOAM_RUN = ~/OpenFOAM/$USER-<version>/run */
    snprintf(s->workspace, sizeof s->workspace, "%s/OpenFOAM/%s-%s/run", pl_home(), e->user, f ? f->version : "v2606");
    s->pvMode = PV_AUTO;
    s->nProcs = e->nCpu >= 4 ? 4 : e->nCpu;
    s->uiScale = 1.0f;
    s->theme = 0;
    s->history = true;
    s->reviewSave = true;
    s->notify = 1;
    s->notifySound = true;
    s->sounds = true;
    s->soundLevel = 1;
    s->notifyMinSec = 60;
}

void settings_load(Settings *s, const Env *e)
{
    settings_defaults(s, e);
    char p[PL_PATH]; settings_path(p, sizeof p);
    char *t = pl_read_file(p, NULL);
    if (!t) {                                   /* carry settings over from the app's earlier name */
        char old[PL_PATH];
        const char *x = getenv("XDG_CONFIG_HOME");
        if (x && *x) snprintf(old, sizeof old, "%s/plenum/settings.ini", x);
        else snprintf(old, sizeof old, "%s/.config/plenum/settings.ini", pl_home());
        t = pl_read_file(old, NULL);
    }
    if (!t) return;
    char *save = NULL;
    for (char *ln = strtok_r(t, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(ln, '=');
        if (!eq || ln[0] == '#') continue;
        *eq = 0;
        char *k = ln, *v = eq + 1;
        pl_trim_inplace(k); pl_trim_inplace(v);
        if (!strcmp(k, "workspace")) pl_strlcpy(s->workspace, v, sizeof s->workspace);
        else if (!strcmp(k, "bashrc")) pl_strlcpy(s->bashrc, v, sizeof s->bashrc);
        else if (!strcmp(k, "paraview_mode")) s->pvMode = atoi(v);
        else if (!strcmp(k, "paraview_command")) pl_strlcpy(s->pvCustom, v, sizeof s->pvCustom);
        else if (!strcmp(k, "editor")) pl_strlcpy(s->editor, v, sizeof s->editor);
        else if (!strcmp(k, "processes")) s->nProcs = atoi(v);
        else if (!strcmp(k, "ui_scale")) s->uiScale = (float)atof(v);
        else if (!strcmp(k, "run_split")) s->runSplit = (float)atof(v);
        else if (!strcmp(k, "log_fold")) s->logFold = atoi(v) != 0;
        else if (!strcmp(k, "whats_new_seen")) s->whatsNewSeen = atoi(v);
        else if (!strcmp(k, "history")) s->history = atoi(v) != 0;
        else if (!strcmp(k, "delete_confirm")) s->deleteConfirm = atoi(v);
        else if (!strcmp(k, "review_save")) s->reviewSave = atoi(v) != 0;
        else if (!strcmp(k, "queue_cores")) s->queueCores = atoi(v);
        else if (!strcmp(k, "queue_paused")) s->queuePaused = atoi(v) != 0;
        else if (!strcmp(k, "notify")) s->notify = atoi(v);
        else if (!strcmp(k, "notify_sound")) s->notifySound = atoi(v) != 0;
        else if (!strcmp(k, "sounds")) s->sounds = atoi(v) != 0;
        else if (!strcmp(k, "last_case")) pl_strlcpy(s->lastCase, v, sizeof s->lastCase);
        else if (!strcmp(k, "last_page")) pl_strlcpy(s->lastPage, v, sizeof s->lastPage);
        else if (!strcmp(k, "last_region")) pl_strlcpy(s->lastRegion, v, sizeof s->lastRegion);
        else if (!strcmp(k, "sound_level")) { s->soundLevel = atoi(v); if (s->soundLevel < 0 || s->soundLevel > 2) s->soundLevel = 1; }
        else if (!strcmp(k, "notify_min_seconds")) s->notifyMinSec = atoi(v);
        else if (!strcmp(k, "away_host")) pl_strlcpy(s->awayHost, v, sizeof s->awayHost);
        else if (!strcmp(k, "away_user")) pl_strlcpy(s->awayUser, v, sizeof s->awayUser);
        else if (!strcmp(k, "away_dir")) pl_strlcpy(s->awayDir, v, sizeof s->awayDir);
        else if (!strcmp(k, "away_modules")) pl_strlcpy(s->awayModules, v, sizeof s->awayModules);
        else if (!strcmp(k, "away_partition")) pl_strlcpy(s->awayPartition, v, sizeof s->awayPartition);
        else if (!strcmp(k, "away_account")) pl_strlcpy(s->awayAccount, v, sizeof s->awayAccount);
        else if (!strcmp(k, "away_time")) pl_strlcpy(s->awayTime, v, sizeof s->awayTime);
        else if (!strcmp(k, "away_export")) pl_strlcpy(s->awayExport, v, sizeof s->awayExport);
        else if (!strcmp(k, "away_nodes")) s->awayNodes = atoi(v);
        else if (!strcmp(k, "away_tasks")) s->awayTasks = atoi(v);
        else if (!strcmp(k, "fullscreen")) s->fullscreen = atoi(v) != 0;
        else if (!strcmp(k, "style")) s->theme = atoi(v);       /* 0 LCARS, 1 classic dark, 2 classic light */
        else if (!strcmp(k, "recent") && pl_is_dir(v)) names_add(&s->recent, v);
        else if (!strcmp(k, "deme_dir") && pl_is_dir(v)) names_add(&s->demeDirs, v);
        else if (pl_starts_with(k, "folder_"))
            for (int g = 1; g < PROG_COUNT && g < 16; g++) if (!strcmp(k + 7, prog_info(g)->key)) pl_strlcpy(s->progDir[g], v, sizeof s->progDir[g]);
    }
    free(t);
    if (s->nProcs < 1) s->nProcs = 1;
    if (s->uiScale < 0.75f || s->uiScale > 2.5f) s->uiScale = 1.0f;
    if (s->pvMode < 0 || s->pvMode > PV_CUSTOM) s->pvMode = PV_AUTO;
    if (s->runSplit < 0.049f || s->runSplit > 0.95f) s->runSplit = 0;
    if (s->deleteConfirm < 0 || s->deleteConfirm > 2) s->deleteConfirm = 0;
}

bool g_settings_readonly = false;                           /* test runs (--screenshot) never touch the user's settings */

void settings_save(const Settings *s)
{
    if (g_settings_readonly) return;
    char p[PL_PATH], d[PL_PATH];
    settings_path(p, sizeof p);
    pl_dirname(d, sizeof d, p);
    pl_mkdirs(d);
    Str b; str_init(&b);
    str_cat(&b, "# Q-Continuum settings\n");
    str_catf(&b, "workspace=%s\nbashrc=%s\nparaview_mode=%d\nparaview_command=%s\neditor=%s\nprocesses=%d\nui_scale=%.2f\nstyle=%d\n",
             s->workspace, s->bashrc, s->pvMode, s->pvCustom, s->editor, s->nProcs, s->uiScale, s->theme);
    if (s->runSplit > 0) str_catf(&b, "run_split=%.3f\n", s->runSplit);
    str_catf(&b, "delete_confirm=%d\nlog_fold=%d\nwhats_new_seen=%d\nreview_save=%d\n", s->deleteConfirm, s->logFold ? 1 : 0, s->whatsNewSeen,
             s->reviewSave ? 1 : 0);
    str_catf(&b, "sounds=%d\nsound_level=%d\n", s->sounds ? 1 : 0, s->soundLevel);
    if (s->lastCase[0]) str_catf(&b, "last_case=%s\nlast_page=%s\nlast_region=%s\n", s->lastCase, s->lastPage, s->lastRegion);
    str_catf(&b, "history=%d\nqueue_cores=%d\nqueue_paused=%d\nnotify=%d\nnotify_sound=%d\nnotify_min_seconds=%d\n", s->history ? 1 : 0,
             s->queueCores, s->queuePaused ? 1 : 0, s->notify, s->notifySound ? 1 : 0, s->notifyMinSec);
    str_catf(&b, "away_host=%s\naway_user=%s\naway_dir=%s\naway_modules=%s\naway_partition=%s\naway_account=%s\naway_time=%s\n"
                 "away_export=%s\naway_nodes=%d\naway_tasks=%d\nfullscreen=%d\n", s->awayHost, s->awayUser, s->awayDir, s->awayModules, s->awayPartition,
             s->awayAccount, s->awayTime, s->awayExport, s->awayNodes, s->awayTasks, s->fullscreen ? 1 : 0);
    for (int i = 0; i < s->recent.n; i++) str_catf(&b, "recent=%s\n", s->recent.names[i]);
    for (int i = 0; i < s->demeDirs.n; i++) str_catf(&b, "deme_dir=%s\n", s->demeDirs.names[i]);
    for (int g = 1; g < PROG_COUNT && g < 16; g++) if (s->progDir[g][0]) str_catf(&b, "folder_%s=%s\n", prog_info(g)->key, s->progDir[g]);
    pl_write_file(p, b.s, b.len);
    str_free(&b);
}

void settings_add_recent(Settings *s, const char *path)
{
    for (int i = 0; i < s->recent.n; i++) if (!strcmp(s->recent.names[i], path)) return;
    names_add(&s->recent, path);
}

/* ============================================================================ launchers */
static void subst_file(char *out, size_t cap, const char *cmd, const char *quotedFile)
{
    const char *f = strstr(cmd, "%f");
    if (f) snprintf(out, cap, "%.*s%s%s", (int)(f - cmd), cmd, quotedFile, f + 2);
    else snprintf(out, cap, "%s %s", cmd, quotedFile);
}

static int pv_effective(const Env *e, const Settings *s)
{
    if (s->pvMode != PV_AUTO) return s->pvMode;
    if (e->wsl && e->pvWin[0]) return PV_WINDOWS;       /* native Windows ParaView is fastest under WSL */
    if (e->pvLinux[0]) return PV_LINUX;
    if (e->pvWin[0]) return PV_WINDOWS;
    return PV_AUTO;
}

const char *paraview_description(const Env *e, const Settings *s)
{
    static char d[PL_PATH + 64];
    switch (pv_effective(e, s)) {
    case PV_LINUX: snprintf(d, sizeof d, "Linux ParaView (%s)", e->pvLinux[0] ? e->pvLinux : "paraview"); break;
    case PV_WINDOWS: snprintf(d, sizeof d, "Windows ParaView (%s)", e->pvWin[0] ? e->pvWin : "not found"); break;
    case PV_CUSTOM: snprintf(d, sizeof d, "Custom: %s", s->pvCustom); break;
    default: snprintf(d, sizeof d, "Not found"); break;
    }
    return d;
}

bool launch_paraview(const Env *e, const Settings *s, const char *foamFile, const char *script, char *err, int cap)
{
    char q[PL_PATH + 8], qs[PL_PATH + 8], cmd[PL_PATH * 4];
    pl_shell_quote(q, sizeof q, foamFile);
    if (script) pl_shell_quote(qs, sizeof qs, script);
    switch (pv_effective(e, s)) {
    case PV_LINUX:
        /* with a script, the script opens the case itself */
        if (script) snprintf(cmd, sizeof cmd, "%s --script=%s", e->pvLinux[0] ? e->pvLinux : "paraview", qs);
        else snprintf(cmd, sizeof cmd, "%s %s", e->pvLinux[0] ? e->pvLinux : "paraview", q);
        break;
    case PV_WINDOWS: {
        if (!e->pvWin[0]) { snprintf(err, (size_t)cap, "Windows ParaView was not found under C:\\Program Files"); return false; }
        char qe[PL_PATH + 8]; pl_shell_quote(qe, sizeof qe, e->pvWin);
        if (script) snprintf(cmd, sizeof cmd, "%s --script=\"$(wslpath -w %s)\"", qe, qs);
        else snprintf(cmd, sizeof cmd, "%s \"$(wslpath -w %s)\"", qe, q);
        break;
    }
    case PV_CUSTOM:
        if (!s->pvCustom[0]) { snprintf(err, (size_t)cap, "Set a ParaView command in Settings"); return false; }
        subst_file(cmd, sizeof cmd, s->pvCustom, q);
        if (script) { size_t n = strlen(cmd); snprintf(cmd + n, sizeof cmd - n, " --script=%s", qs); }
        break;
    default:
        snprintf(err, (size_t)cap, e->wsl ? "ParaView not found: install it on Windows, or run: sudo apt install paraview"
                                          : "ParaView not found: sudo apt install paraview");
        return false;
    }
    if (script) {
        /* ParaView's GUI runs the script without __file__: it finds the case from the working directory, so start in it */
        char dir[PL_PATH], qd[PL_PATH + 8], full[PL_PATH * 5 + 32];
        pl_strlcpy(dir, foamFile, sizeof dir);
        char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
        pl_shell_quote(qd, sizeof qd, dir);
        if (!pl_fmt(full, sizeof full, "cd %s && %s", qd, cmd)) { snprintf(err, (size_t)cap, "The case path is too long to open in ParaView"); return false; }
        pl_spawn_detached(full);
        return true;
    }
    pl_spawn_detached(cmd);
    return true;
}

void launch_folder(const Env *e, const char *dir)
{
    char q[PL_PATH + 8], cmd[PL_PATH * 2];
    pl_shell_quote(q, sizeof q, dir);
    if (e->wsl) snprintf(cmd, sizeof cmd, "explorer.exe \"$(wslpath -w %s)\"", q);
    else snprintf(cmd, sizeof cmd, "xdg-open %s", q);
    pl_spawn_detached(cmd);
}

void launch_editor(const Env *e, const Settings *s, const char *file)
{
    char q[PL_PATH + 8], cmd[PL_PATH * 3];
    pl_shell_quote(q, sizeof q, file);
    if (s->editor[0]) subst_file(cmd, sizeof cmd, s->editor, q);
    else if (e->code[0]) snprintf(cmd, sizeof cmd, "code %s", q);
    else snprintf(cmd, sizeof cmd, "xdg-open %s", q);
    pl_spawn_detached(cmd);
}

const char *env_videos_dir(const Env *e)
{
    static char dir[PL_PATH];
    if (dir[0]) return dir;
    const char *test = getenv("QC_VIDEOS");                         /* testing: anywhere but the user's own folder */
    if (test && *test) { pl_strlcpy(dir, test, sizeof dir); return dir; }
    if (e->wsl) {                                                    /* asked once: it may be moved to OneDrive */
        char out[PL_PATH] = "";
        pl_run_wait("cd /mnt/c 2>/dev/null; p=$(/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive "
                    "-Command \"[Environment]::GetFolderPath('MyVideos')\" 2>/dev/null | tr -d '\\r'); [ -n \"$p\" ] && wslpath -u \"$p\"",
                    out, sizeof out);
        pl_trim_inplace(out);
        if (out[0] && pl_is_dir(out)) { pl_strlcpy(dir, out, sizeof dir); return dir; }
    }
    pl_join(dir, sizeof dir, pl_home(), "Videos");
    return dir;
}

/* ============================================================================ desktop notifications */
/* text for a PowerShell single-quoted string: ' doubles; control characters become spaces */
static void ps_quote(Str *b, const char *s)
{
    str_cat(b, "'");
    for (const char *p = s; *p; p++) {
        if (*p == '\'') str_cat(b, "''");
        else if ((unsigned char)*p < 32) str_cat(b, " ");
        else str_catn(b, p, 1);
    }
    str_cat(b, "'");
}

/* UTF-8 -> UTF-16LE -> base64, for powershell -EncodedCommand (no quoting through bash and cmd at all) */
static void ps_encode(const char *script, Str *out)
{
    Str u; str_init(&u);
    for (const unsigned char *p = (const unsigned char *)script; *p; ) {
        unsigned cp; int n;
        if (*p < 0x80) { cp = *p; n = 1; }
        else if ((*p >> 5) == 6 && p[1]) { cp = ((p[0] & 31u) << 6) | (p[1] & 63u); n = 2; }
        else if ((*p >> 4) == 14 && p[1] && p[2]) { cp = ((p[0] & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); n = 3; }
        else { cp = '?'; n = 1; }
        char le[2] = { (char)(cp & 0xFF), (char)(cp >> 8) };
        str_catn(&u, le, 2);
        p += n;
    }
    static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *d = (const unsigned char *)u.s;
    for (size_t i = 0; i < u.len; i += 3) {
        unsigned v = (unsigned)d[i] << 16 | (i + 1 < u.len ? (unsigned)d[i + 1] << 8 : 0) | (i + 2 < u.len ? d[i + 2] : 0);
        char q[4] = { B[(v >> 18) & 63], B[(v >> 12) & 63], i + 1 < u.len ? B[(v >> 6) & 63] : '=', i + 2 < u.len ? B[v & 63] : '=' };
        str_catn(out, q, 4);
    }
    str_free(&u);
}

/* a notification on the desktop: a Windows toast under WSL, notify-send on a Linux desktop */
void notify_desktop(const Env *e, const char *title, const char *body, bool sound)
{
    if (e->wsl && pl_is_file("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")) {
        Str ps; str_init(&ps);
        str_cat(&ps, "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null\n"
                     "$x = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)\n"
                     "$t = $x.GetElementsByTagName('text')\n$t.Item(0).AppendChild($x.CreateTextNode(");
        ps_quote(&ps, title);
        str_cat(&ps, ")) > $null\n$t.Item(1).AppendChild($x.CreateTextNode(");
        ps_quote(&ps, body);
        str_cat(&ps, ")) > $null\n");
        if (!sound) str_cat(&ps, "$a = $x.CreateElement('audio'); $a.SetAttribute('silent', 'true'); $x.DocumentElement.AppendChild($a) > $null\n");
        /* shown as from Windows PowerShell: an app id every Windows has, so nothing needs registering */
        str_cat(&ps, "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("
                     "'{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe').Show("
                     "[Windows.UI.Notifications.ToastNotification]::new($x))\n");
        Str enc; str_init(&enc);
        ps_encode(ps.s, &enc);
        Str cmd; str_init(&cmd);
        str_catf(&cmd, "cd /mnt/c 2>/dev/null; /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive "
                       "-WindowStyle Hidden -EncodedCommand %s > /dev/null 2>&1", enc.s);
        pl_spawn_detached(cmd.s);
        str_free(&ps); str_free(&enc); str_free(&cmd);
        return;
    }
    if (pl_which("notify-send", NULL, 0)) {
        char qt[300], qb[700], cmd[1200];
        pl_shell_quote(qt, sizeof qt, title);
        pl_shell_quote(qb, sizeof qb, body);
        snprintf(cmd, sizeof cmd, "notify-send -a Q-Continuum %s %s%s", qt, qb,
                 sound ? "; command -v canberra-gtk-play >/dev/null && canberra-gtk-play -i complete" : "");
        pl_spawn_detached(cmd);
    }
}

bool launch_terminal(const Env *e, const Settings *s, const char *dir) { return launch_terminal_run(e, s, dir, NULL); }

bool launch_terminal_run(const Env *e, const Settings *s, const char *dir, const char *run)
{
    const char *test = getenv("QC_TEST_TERMINAL");               /* testing: record the command instead of opening a window */
    if (test && *test) {
        char b[PL_PATH * 2]; int n = snprintf(b, sizeof b, "dir=%s\nrun=%s\n", dir, run ? run : "");
        return pl_write_file(test, b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
    }
    const FoamInstall *f = env_active(e, s);
    /* the commands go into a script file: Windows Terminal reads every ';' on its command line as "open another tab", so
       "cd x; source y; claude ..." became several tabs, each a PowerShell running a fragment and failing */
    char sdir[PL_PATH], sp[PL_PATH];
    const char *xc = getenv("XDG_CACHE_HOME");
    if (xc && *xc) snprintf(sdir, sizeof sdir, "%s/q-continuum/terminal", xc); else snprintf(sdir, sizeof sdir, "%s/.cache/q-continuum/terminal", pl_home());
    pl_mkdirs(sdir);
    static int seq;
    if (!pl_fmt(sp, sizeof sp, "%s/t%d-%d.sh", sdir, (int)getpid(), ++seq)) return false;
    char qd[PL_PATH + 8], qb[PL_PATH + 8] = "";
    pl_shell_quote(qd, sizeof qd, dir);
    if (f) pl_shell_quote(qb, sizeof qb, f->bashrc);
    Str sc; str_init(&sc);
    str_catf(&sc, "#!/bin/bash\nrm -f \"$0\"\ncd %s || exit 1\n", qd);
    if (f) str_catf(&sc, "source %s > /dev/null 2>&1\n", qb);
    if (run && *run) str_catf(&sc, "%s\n", run);
    str_cat(&sc, "exec bash -i\n");
    bool ok = pl_write_file(sp, sc.s, sc.len);
    str_free(&sc);
    if (!ok) return false;
    char qs[PL_PATH + 8]; pl_shell_quote(qs, sizeof qs, sp);
    char cmd[PL_PATH * 4];
    if (e->wsl) {
        const char *distro = getenv("WSL_DISTRO_NAME");
        char d[96] = "";
        if (distro) snprintf(d, sizeof d, "-d %s ", distro);
        if (e->haveWt) snprintf(cmd, sizeof cmd, "wt.exe -w 0 nt --title Q-Continuum wsl.exe %s-e bash %s", d, qs);
        else snprintf(cmd, sizeof cmd, "cmd.exe /c start \"Q-Continuum\" wsl.exe %s-e bash %s", d, qs);
    } else {
        const char *terms[] = { "gnome-terminal", "konsole", "xfce4-terminal", "x-terminal-emulator", "xterm", NULL };
        const char *t = NULL;
        for (int i = 0; terms[i]; i++) if (pl_which(terms[i], NULL, 0)) { t = terms[i]; break; }
        if (!t) return false;
        if (!strcmp(t, "gnome-terminal")) snprintf(cmd, sizeof cmd, "gnome-terminal -- bash %s", qs);
        else snprintf(cmd, sizeof cmd, "%s -e bash %s", t, qs);
    }
    pl_spawn_detached(cmd);
    return true;
}
