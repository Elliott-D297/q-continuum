/* page_away.c -- AWAY MISSION (ROADMAP 5.1): a case packed for the aerofly cluster (OpenFOAM v2412, Slurm).
 *
 * One job does it all, in the background: the case's setup is staged into <export>/<case>-<stamp>/ (no results, no
 * processor*, no logs), a Slurm batch script with the same steps the app runs here (srun for the solver) is written
 * beside it, the staged copy is checked with the local v2412 (mesh, then the solver's -dry-run, in a scratch copy),
 * and the folder is packed into <case>-<stamp>.tar.gz. The dialog then gives the scp / ssh / rsync commands to copy.
 * Nothing connects to the cluster and no credentials are kept: it is only reachable from its own network.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "explain.h"

static struct {
    bool  init;
    char  stamp[32], stage[PL_PATH], tar[PL_PATH], checkLog[PL_PATH];
    int   procs;
    char  procsText[16], nodesText[16];
    bool  started;                           /* a prepare job of ours was started for the open case */
    char  forCase[PL_PATH];
} W;

static const FoamInstall *v2412(void)
{
    for (int i = 0; i < A.env.nInst; i++) if (!strcmp(A.env.inst[i].version, "v2412")) return &A.env.inst[i];
    return NULL;
}

static void export_dir(char *out, int cap)
{
    char d[PL_PATH];
    if (A.set.awayExport[0]) pl_expand_home(d, sizeof d, A.set.awayExport);
    else pl_fmt(d, sizeof d, "%s/aerofly-export", pl_home());
    pl_mkdirs(d);
    char r[PL_PATH];                                          /* absolute: the job runs in the case folder */
    if (realpath(d, r)) pl_strlcpy(out, r, (size_t)cap); else pl_strlcpy(out, d, (size_t)cap);
}

/* the batch script: Slurm header, the modules (or a stop that says they're missing), the app's steps */
static void slurm_script(Case *c, int np, Str *s)
{
    char when[32]; time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    str_catf(s, "#!/bin/bash\n#SBATCH --job-name=%.40s\n#SBATCH --ntasks=%d\n#SBATCH --nodes=%d\n#SBATCH --time=%s\n",
             c->s.name, np, A.set.awayNodes > 0 ? A.set.awayNodes : 1, A.set.awayTime[0] ? A.set.awayTime : "24:00:00");
    if (A.set.awayPartition[0]) str_catf(s, "#SBATCH --partition=%s\n", A.set.awayPartition);
    if (A.set.awayAccount[0]) str_catf(s, "#SBATCH --account=%s\n", A.set.awayAccount);
    str_catf(s, "#SBATCH --output=slurm-%%j.out\n\n# %s for aerofly (OpenFOAM v2412), written by Q-Continuum's AWAY MISSION on %s.\n"
                "# Steps as Q-Continuum runs them locally; the solver is started by srun on the job's %d task%s.\n\n",
             c->s.name, when, np, np == 1 ? "" : "s");
    if (A.set.awayModules[0]) {
        str_cat(s, "# the cluster's modules (Settings > Cluster)\n");
        for (const char *p = A.set.awayModules; *p; ) {           /* ';' separates lines in the setting */
            const char *e = strchr(p, ';'); size_t n = e ? (size_t)(e - p) : strlen(p);
            char line[300]; snprintf(line, sizeof line, "%.*s", (int)n, p); pl_trim_inplace(line);
            if (line[0]) str_catf(s, "%s\n", line);
            p = e ? e + 1 : p + n;
        }
    } else str_cat(s, "# The module names on aerofly weren't given (Settings > Cluster): load OpenFOAM v2412 here, for example\n"
                      "#   module load <the OpenFOAM v2412 module>\n");
    str_catf(s, "\ncommand -v %s >/dev/null || { echo \"%s isn't on the PATH: load the OpenFOAM v2412 module at the top of this script\"; exit 1; }\n"
                "[ \"$WM_PROJECT_VERSION\" = v2412 ] || echo \"Note: this is OpenFOAM $WM_PROJECT_VERSION; the case was checked with v2412\"\n"
                "cd \"${SLURM_SUBMIT_DIR:-.}\" || exit 1\n", c->s.app, c->s.app);
    if (np > 1)                                                /* the decomposition follows the job's task count */
        str_catf(s, "\n# the job's task count decides the decomposition (change --ntasks above and this follows)\n"
                    "NP=${SLURM_NTASKS:-%d}\nfor d in system/decomposeParDict system/*/decomposeParDict; do\n"
                    "    [ -f \"$d\" ] || continue\n    foamDictionary \"$d\" -entry numberOfSubdomains -set \"$NP\" >/dev/null 2>&1\n"
                    "    case \"$(foamDictionary \"$d\" -entry method -value 2>/dev/null)\" in\n"
                    "        simple|hierarchical) foamDictionary \"$d\" -entry method -set scotch >/dev/null 2>&1;;   # their split was for another count\n"
                    "    esac\ndone\n", np);
    run_steps_script(c, np, s);
    str_cat(s, "\necho \"== $(date +%H:%M:%S) done\"\n");
}

/* the job: stage, script, v2412 check, pack */
static void prepare(Case *c)
{
    char exp[PL_PATH]; export_dir(exp, sizeof exp);
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(W.stamp, sizeof W.stamp, "%Y%m%d-%H%M%S", &tm);
    char base[PL_PATH];
    pl_fmt(base, sizeof base, "%s-%s", c->s.name, W.stamp);
    pl_fmt(W.stage, sizeof W.stage, "%s/%s", exp, base);
    pl_fmt(W.tar, sizeof W.tar, "%s/%s.tar.gz", exp, base);
    pl_fmt(W.checkLog, sizeof W.checkLog, "%s/%s.v2412-check.log", exp, base);
    int np = W.procs > 0 ? W.procs : 1;
    Str scr; str_init(&scr);
    slurm_script(c, np, &scr);
    /* the script goes in through a file next to the stage, not the command line */
    char sp[PL_PATH]; pl_fmt(sp, sizeof sp, "%s/%s.slurm", exp, base);
    if (!pl_mkdirs(exp) || !pl_write_file(sp, scr.s, scr.len)) { ui_toast(TOAST_ERR, "Can't write in %s", exp); str_free(&scr); return; }
    str_free(&scr);
    char qe[PL_PATH + 8], qs[PL_PATH + 8], qt[PL_PATH + 8], ql[PL_PATH + 8], qsp[PL_PATH + 8], qb[PL_PATH + 8];
    pl_shell_quote(qe, sizeof qe, exp); pl_shell_quote(qs, sizeof qs, W.stage); pl_shell_quote(qt, sizeof qt, W.tar);
    pl_shell_quote(ql, sizeof ql, W.checkLog); pl_shell_quote(qsp, sizeof qsp, sp); pl_shell_quote(qb, sizeof qb, base);
    Str ex; str_init(&ex);
    NameList l = { 0 };
    pl_list_dir(c->s.path, &l, true, false);
    for (int i = 0; i < l.n; i++) if (pl_is_number(l.names[i]) && atof(l.names[i]) > c->s.startTime + 1e-12) str_catf(&ex, "--exclude=./%s ", l.names[i]);
    names_free(&l);
    const FoamInstall *old = v2412();
    char qrc[PL_PATH + 8] = "''"; if (old) pl_shell_quote(qrc, sizeof qrc, old->bashrc);
    char qapp[160]; pl_shell_quote(qapp, sizeof qapp, c->s.app);
    Str cmd; str_init(&cmd);
    str_catf(&cmd,
        "set -e\necho '== stage the setup in %s'\nmkdir -p %s\n"
        "tar --exclude='./processor*' --exclude='./log.*' --exclude=./postProcessing --exclude='./.qc-*' --exclude=./.qcontinuum "
        "--exclude='./*.foam' --exclude=./.q-continuum.ini --exclude=./movie %s -cf - . | tar -C %s -xf -\n"
        "mv %s %s/aerofly.slurm && chmod +x %s/aerofly.slurm\necho \"   $(du -sh %s | cut -f1) staged, aerofly.slurm written\"\n",
        W.stage, qs, ex.s ? ex.s : "", qs, qsp, qs, qs, qs);
    if (old)
        str_catf(&cmd,
            "echo '== check with OpenFOAM v2412 (this machine) in a scratch copy: mesh, then %s -dry-run'\n"
            "CHK=$(mktemp -d)\ncp -a %s/. \"$CHK\"/\nset +e\n"
            "bash -c '\n"   /* v2412's bashrc over this job's v2606 one swaps the environment */
            "  RC=$1; DIR=$2; APP=$3; set --\n"   /* sourced with arguments, OpenFOAM's bashrc reads them as its own settings: bash crashed */
            "  . \"$RC\" || exit 9\n  cd \"$DIR\" || exit 9\n  echo \"OpenFOAM $WM_PROJECT_VERSION\"\n"
            "  if [ -x ./Allrun.pre ]; then ./Allrun.pre || exit 1\n"
            "  elif [ ! -f constant/polyMesh/owner ] && [ -f system/blockMeshDict ]; then blockMesh || exit 1; fi\n"
            "  [ -d 0.orig ] && [ ! -d 0 ] && cp -r 0.orig 0\n"
            "  \"$APP\" -dry-run' qc-check %s \"$CHK\" %s > %s 2>&1\nrc=$?\nset -e\n"
            "cp %s \"$CHK\"/log.v2412-check 2>/dev/null; rm -rf \"$CHK\"\n"
            "if [ $rc = 0 ] && ! grep -q 'FOAM FATAL' %s; then echo '   v2412: passed (mesh and one time step)'; "
            "else echo \"   v2412: FAILED, see %s\"; fi\n",
            c->s.app, qs, qrc, qapp, ql, ql, ql, W.checkLog);
    else str_cat(&cmd, "echo '== no OpenFOAM v2412 on this machine: the version check is skipped'\n");
    str_catf(&cmd, "echo '== pack'\ntar -C %s -czf %s %s\necho \"   $(du -h %s | cut -f1) %s\"\n", qe, qt, qb, qt, W.tar);
    str_free(&ex);
    app_start_tool("Away mission", "away", "Prepare for aerofly", cmd.s, "log.qc-away");
    str_free(&cmd);
    W.started = A.cj->active;
    pl_strlcpy(W.forCase, c->s.path, sizeof W.forCase);
}

static void copy_row(UiId id, const char *label, const char *cmd, float x, float *y, float w)
{
    ui_text(F_MED, 13, label, x, *y, TH.text2);
    *y += 22;
    Rectangle r = { x, *y, w - 44, 34 };
    ui_rect(r, 8, TH.input); ui_rect_lines(r, 8, 1, TH.lineSoft);
    ui_text_clip(F_MONO, 12.5f, cmd, r.x + 10, r.y + 10, r.width - 20, TH.text);
    ui_tooltip(r, cmd);
    if (ui_button(id, (Rectangle){ x + w - 36, *y + 2, 32, 30 }, ICON_COPY, NULL, BTN_ICON)) { SetClipboardText(cmd); ui_toast(TOAST_OK, "Copied: %s", label); }
    *y += 46;
}

static void field(UiId id, const char *label, char *buf, int cap, const char *ph, float x, float y, float w, bool *changed)
{
    ui_text(F_MED, 13, label, x, y + 9, TH.text2);
    if (ui_input(id, (Rectangle){ x + 150, y, w - 150, 34 }, buf, cap, ph, INPUT_LIVE | INPUT_MONO)) *changed = true;
}

void away_modal(bool *close)
{
    Case *c = A.cs;
    if (!c || c->s.prog) { *close = true; return; }
    if (!W.init || strcmp(W.forCase, c->s.path)) {
        W.init = true; W.started = false; W.stage[0] = W.tar[0] = 0;
        pl_strlcpy(W.forCase, c->s.path, sizeof W.forCase);
        int np = A.runProcs > 0 ? A.runProcs : A.set.nProcs;
        if (A.set.awayTasks > 0) np = A.set.awayTasks;
        snprintf(W.procsText, sizeof W.procsText, "%d", np);
        snprintf(W.nodesText, sizeof W.nodesText, "%d", A.set.awayNodes > 0 ? A.set.awayNodes : 1);
    }
    char title[96]; pl_fmt_ui(title, sizeof title, "%s: %s", term(T_AWAY), c->s.name);
    Rectangle b = ui_modal_begin(title, "Packs the case for aerofly (OpenFOAM v2412, Slurm): the setup without results, a batch script with the steps "
                                 "Q-Continuum runs here, checked with v2412 on this machine first. Nothing connects to the cluster: copy the commands.",
                                 980, 700, close);
    float x = b.x, y = b.y + 4, lw = 430;
    bool ch = false;
    ui_text(F_SEMI, 15, "Cluster", x, y, TH.text); y += 30;
    field(ui_id("aw-host"), "Host", A.set.awayHost, sizeof A.set.awayHost, "aerofly", x, y, lw, &ch); y += 42;
    field(ui_id("aw-user"), "User", A.set.awayUser, sizeof A.set.awayUser, A.env.user, x, y, lw, &ch); y += 42;
    field(ui_id("aw-dir"), "Remote folder", A.set.awayDir, sizeof A.set.awayDir, "~/cases", x, y, lw, &ch); y += 42;
    field(ui_id("aw-mod"), "Modules", A.set.awayModules, sizeof A.set.awayModules, "module load ...; module load ...", x, y, lw, &ch); y += 38;
    ui_text_wrap(F_REG, 12, "Lines run before OpenFOAM, separated by ';'. Not known yet for aerofly: ask the admins or run "
                 "'module avail openfoam' there. Until then the script stops with a message saying so.", (Rectangle){ x, y, lw, 60 }, 17, TH.text3, true);
    y += 44;
    field(ui_id("aw-part"), "Partition", A.set.awayPartition, sizeof A.set.awayPartition, "(the default)", x, y, lw, &ch); y += 42;
    field(ui_id("aw-acct"), "Account", A.set.awayAccount, sizeof A.set.awayAccount, "(none)", x, y, lw, &ch); y += 42;
    field(ui_id("aw-time"), "Time limit", A.set.awayTime, sizeof A.set.awayTime, "24:00:00", x, y, lw, &ch); y += 42;
    bool pc = false;
    field(ui_id("aw-np"), "Processes", W.procsText, sizeof W.procsText, "4", x, y, lw, &pc); y += 42;
    field(ui_id("aw-nodes"), "Nodes", W.nodesText, sizeof W.nodesText, "1", x, y, lw, &ch);
    W.procs = atoi(W.procsText);
    if (pc) { A.set.awayTasks = W.procs; ch = true; }
    A.set.awayNodes = atoi(W.nodesText);
    if (ch) settings_save(&A.set);

    /* right: prepare, then the commands */
    float rx = x + lw + 40, rw = b.width - lw - 40, ry = b.y + 4;
    ui_text(F_SEMI, 15, "Package", rx, ry, TH.text); ry += 30;
    const FoamInstall *old = v2412();
    char info[400], exp[PL_PATH]; export_dir(exp, sizeof exp);
    pl_fmt_ui(info, sizeof info, "Into %s. %s", exp, old ? "Checked with OpenFOAM v2412 from this machine (mesh, one time step)." :
              "No OpenFOAM v2412 on this machine: the version check is skipped.");
    ry += ui_text_wrap(F_REG, 13, info, (Rectangle){ rx, ry, rw, 80 }, 19, TH.text2, true) + 10;
    bool busy = A.cj->active && !strcmp(A.cj->casePath, c->s.path);
    ui_disable(busy || !(W.procs > 0) || !c->s.app[0]);
    float bw = ui_button_width(ICON_HARD_DRIVE, "Prepare the package");
    if (ui_button(ui_id("aw-go"), (Rectangle){ rx, ry, bw, 38 }, ICON_HARD_DRIVE, "Prepare the package", BTN_PRIMARY)) prepare(c);
    ui_disable(false);
    if (busy) ui_text(F_REG, 13, W.started ? "Preparing... (the log is on the Run page)" : "A run is going in this case", rx + bw + 14, ry + 11, TH.text3);
    ry += 54;
    bool done = W.started && !busy && W.tar[0];
    if (done) {
        bool okTar = pl_exists(W.tar);
        char *lg = pl_read_file(W.checkLog, NULL);
        char jl[PL_PATH]; pl_join(jl, sizeof jl, c->s.path, "log.qc-away");
        char *job = pl_read_file(jl, NULL);
        bool passed = job && strstr(job, "v2412: passed");
        free(job);
        bool checked = lg != NULL, fatal = checked && !passed;
        if (!okTar) ui_text_wrap(F_REG, 13, "The package wasn't made: the Run page shows the log (log.qc-away).", (Rectangle){ rx, ry, rw, 60 }, 19, TH.err, true), ry += 30;
        else {
            char t[PL_PATH + 64]; pl_fmt_ui(t, sizeof t, "Ready: %s", W.tar);
            ry += ui_text_wrap(F_REG, 13, t, (Rectangle){ rx, ry, rw, 60 }, 19, TH.ok, true) + 6;
        }
        if (checked && !fatal) { ui_icon(ICON_CIRCLE_CHECK, rx + 8, ry + 9, 14, TH.ok); ui_text(F_REG, 13, "v2412 check passed: the mesh, and one time step of the solver", rx + 24, ry, TH.ok); ry += 28; }
        else if (fatal) {
            const char *m = lg ? strstr(lg, "FOAM FATAL") : NULL; const char *nl = m ? strchr(m, '\n') : NULL;
            char msg[400] = ""; if (nl) { const char *e = strchr(nl + 1, '\n'); snprintf(msg, sizeof msg, "%.*s", e ? (int)(e - nl - 1) : 200, nl + 1); }
            Explanation x2; char rel[PL_PATH]; pl_fmt(rel, sizeof rel, "../%s", pl_basename(W.checkLog));
            bool ex = foam_explain(W.stage, rel, msg, &x2);
            char t[900]; pl_fmt_ui(t, sizeof t, "v2412 check FAILED: %s%s%s", ex ? x2.title : msg[0] ? msg : "the mesh or the solver stopped (see the check log)",
                                   ex ? ". " : "", ex ? x2.text : "");
            ry += ui_text_wrap(F_REG, 13, t, (Rectangle){ rx, ry, rw, 140 }, 19, TH.err, true) + 8;
        }
        free(lg);
        ry += 8;
        const char *host = A.set.awayHost[0] ? A.set.awayHost : "aerofly", *user = A.set.awayUser[0] ? A.set.awayUser : A.env.user;
        const char *dir = A.set.awayDir[0] ? A.set.awayDir : "~/cases";
        char stg[PL_PATH]; pl_strlcpy(stg, pl_basename(W.stage), sizeof stg);
        char c1[PL_PATH * 2], c2[PL_PATH * 2], c3[PL_PATH * 3];
        pl_fmt(c1, sizeof c1, "scp '%s' %s@%s:%s/", W.tar, user, host, dir);
        pl_fmt(c2, sizeof c2, "ssh %s@%s 'cd %s && tar xzf %s.tar.gz && cd %s && sbatch aerofly.slurm'", user, host, dir, stg, stg);
        pl_fmt(c3, sizeof c3, "rsync -av --exclude='processor*' %s@%s:%s/%s/ '%s-results/'", user, host, dir, stg, W.stage);
        copy_row(ui_id("aw-c1"), "1. Copy it there (from the cluster's network)", c1, rx, &ry, rw);
        copy_row(ui_id("aw-c2"), "2. Unpack and submit", c2, rx, &ry, rw);
        copy_row(ui_id("aw-c3"), "3. Bring the results back (next to the package, not into the case)", c3, rx, &ry, rw);
        ui_text_wrap(F_REG, 12, "squeue -u $USER shows the job; slurm-<id>.out and the log.* files in the case folder say how it went.",
                     (Rectangle){ rx, ry, rw, 40 }, 17, TH.text3, true);
    }
}
