/* page_env.c -- the Environment panel, opened from "Environment ready" in the sidebar: what this machine has for the app
 * (OpenFOAM, MPI, ParaView, movies, SALOME, preCICE, CalculiX, DEME and CUDA, the other programs (SPARTA ...), Claude Code, the editor, cores, memory,
 * disk, sound), each found or not with what it is used for. Settings is where things are changed; this is where you look. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include "app.h"
#include "sound.h"
#include "program.h"

typedef struct { int state; char name[48], detail[PL_PATH + 64], use[160]; } EnvRow;   /* state: 1 found, 0 optional and absent, -1 needed and absent */

static struct { EnvRow r[32]; int n; bool built, pending; double builtAt; UiScroll scroll; } E;

static void row(int state, const char *name, const char *use, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void row(int state, const char *name, const char *use, const char *fmt, ...)
{
    if (E.n >= 32) return;
    EnvRow *r = &E.r[E.n++];
    r->state = state;
    pl_strlcpy(r->name, name, sizeof r->name);
    pl_strlcpy(r->use, use, sizeof r->use);
    va_list ap; va_start(ap, fmt); vsnprintf(r->detail, sizeof r->detail, fmt, ap); va_end(ap);
}

static double meminfo(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[160]; double kb = 0;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, n) && line[n] == ':') { kb = atof(line + n + 1); break; }
    fclose(f);
    return kb / 1048576.0;                                   /* GB */
}

static void build(void)
{
    E.n = 0; E.pending = false;
    const Env *e = &A.env;
    const FoamInstall *fi = env_active(e, &A.set);
    char p[PL_PATH];
    if (fi) row(1, "OpenFOAM", "Meshing, solvers and utilities. Which install is used is chosen in Settings.", "%s  ·  %s%s%s", fi->version, fi->root,
                fi->hasTutorials ? "  ·  tutorials" : "", e->nInst > 1 ? TextFormat("  ·  %d installs found", e->nInst) : "");
    else row(-1, "OpenFOAM", "Needed for everything: point Settings at its etc/bashrc.", "not found");
    bool mpi = pl_which("mpirun", p, sizeof p);
    row(mpi ? 1 : 0, "MPI", "Parallel runs (decomposePar, -parallel).", "%s", mpi ? p : "mpirun not on the PATH (OpenFOAM's environment may add it)");
    if (e->pvLinux[0] || e->pvWin[0]) row(1, "ParaView", "Looking at results.", "%s", paraview_description(e, &A.set));
    else row(0, "ParaView", "Looking at results.", "not found (paraview.org)");
    if (e->pvbatch[0] && e->ffmpeg[0]) row(1, "Movies", "Make a movie: pvbatch renders, ffmpeg encodes.", "%s  ·  %s", e->pvbatch, e->ffmpeg);
    else row(0, "Movies", "Make a movie: pvbatch renders, ffmpeg encodes.", "%s%s", e->pvbatch[0] ? "" : "no pvbatch  ", e->ffmpeg[0] ? "" : "no ffmpeg");
    if (e->salome[0]) row(1, "SALOME", "Meshes from CAD (Mesh > SALOME).", "%s  ·  %s", e->salomeVersion[0] ? e->salomeVersion : "?", e->salome);
    else row(0, "SALOME", "Meshes from CAD (Mesh > SALOME).", "not found");
    if (e->preciceVersion[0]) row(1, "preCICE", "Coupled runs (Coupling page).", "%s%s%s", e->preciceVersion, e->preciceTutorials[0] ? "  ·  tutorials in " : "", e->preciceTutorials);
    else row(0, "preCICE", "Coupled runs (Coupling page).", "not found");
    char ccx[PL_PATH] = "", ccxp[PL_PATH] = "";
    bool hc = pl_which("ccx", ccx, sizeof ccx), hcp = pl_which("ccx_preCICE", ccxp, sizeof ccxp);
    row(hc || hcp ? 1 : 0, "CalculiX", "Structural solver; ccx_preCICE couples it to OpenFOAM.", "%s%s%s", hc ? ccx : "no ccx", "  ·  ", hcp ? ccxp : "no ccx_preCICE");
    const char *dd, *nv, *gpu;
    deme_toolchain(&dd, &nv, &gpu);
    row(dd[0] && nv[0] ? 1 : 0, "DEME", "GPU discrete elements (DEME page).", "%s  ·  CUDA %s  ·  %s", dd[0] ? dd : "DEM-Engine not found",
        nv[0] ? nv : "not found", gpu[0] ? gpu : "no NVIDIA GPU");
    /* the other programs, each found where a run would look for it */
    static const struct { int prog; const char *use; } PR[] = {
        { PROG_SPARTA,  "Rarefied gas, DSMC (spa_mpi)." },             { PROG_CHRONO,  "Multibody dynamics (built Chrono, for its projects)." },
        { PROG_MOOSE,   "Multiphysics FEM (a MOOSE application)." },   { PROG_ELMER,   "Multiphysics FEM (ElmerSolver)." },
        { PROG_ASTER,   "Structural FEM (run_aster, salome_meca)." },  { PROG_SATURNE, "Finite-volume CFD (code_saturne)." } };
    for (size_t i = 0; i < sizeof PR / sizeof PR[0]; i++) {
        char exe[PL_PATH], why[300] = "";
        bool ok = prog_exe(PR[i].prog, exe, sizeof exe, why, sizeof why);
        if (ok && PR[i].prog == PROG_SPARTA) {
            char miss[96];
            E.pending = !prog_sparta_missing(miss, sizeof miss);
            if (E.pending) row(1, "SPARTA", PR[i].use, "%s  ·  reading which packages it has ...", exe);
            else row(1, "SPARTA", PR[i].use, "%s  ·  %s%s", exe, miss[0] ? "without the optional packages " : "every optional package", miss);
        } else row(ok ? 1 : 0, prog_info(PR[i].prog)->name, PR[i].use, "%s", ok ? exe : why);
    }
    row(e->claude[0] ? 1 : 0, "Claude Code", "ParaView with Claude, Ask Claude on errors.", "%s", e->claude[0] ? e->claude : "not installed");
    row(1, "Editor", "Opening dictionaries and drivers in an editor (Settings > Text editor).", "%s",
        A.set.editor[0] ? A.set.editor : e->code[0] ? e->code : "the desktop default (xdg-open)");
    char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
    struct statvfs v; double freeGB = statvfs(ws, &v) == 0 ? (double)v.f_bavail * v.f_frsize / 1e9 : -1;
    row(1, "This machine", "What the pre-run check sizes runs against.", "%d cores  ·  %.1f GB memory, %.1f GB free  ·  %s%.0f GB free in the workspace",
        e->nCpu, meminfo("MemTotal"), meminfo("MemAvailable"), e->wsl ? "WSL  ·  " : "", freeGB);
    int pk = app_sound_pack(), ds = sound_device_state();
    static const char *const PK[SOUND__N] = { "LCARS", "Stardew", "Pokémon", "JARVIS" };
    if (pk < 0) row(1, "Sound", "Theme sounds (Settings > Appearance).", "off: the classic themes are silent");
    else row(ds == 2 || ds == 3 ? 0 : 1, "Sound", "Theme sounds (Settings > Appearance).", "%s sounds %s  ·  %s", PK[pk], A.set.sounds ? "on" : "off",
             e->wsl ? "played by Windows" : ds == 1 ? "audio device open" : ds == 0 ? "opens on the first sound" : "no audio device (libpulse)");
    E.built = true; E.builtAt = GetTime();
}

void env_modal(bool *close)
{
    if (!E.built || (E.pending && GetTime() - E.builtAt > 1)) build();   /* SPARTA's package list arrives in the background */
    Rectangle b = ui_modal_begin("Environment", "What this machine has for Q-Continuum, and what each part is for. Change things in Settings.",
                                 820, 790, close);
    Rectangle body = { b.x, b.y, b.width, b.height - 52 };
    float top = ui_scroll_begin(&E.scroll, body), y = top + 2, x = b.x, w = b.width;
    for (int i = 0; i < E.n; i++) {
        const EnvRow *r = &E.r[i];
        Color c = r->state > 0 ? TH.ok : r->state < 0 ? TH.err : TH.text3;
        ui_icon(r->state > 0 ? ICON_CIRCLE_CHECK : r->state < 0 ? ICON_CIRCLE_X : ICON_CIRCLE_DOT, x + 9, y + 10, 15, c);
        ui_text(F_SEMI, 14, r->name, x + 28, y + 2, TH.text);
        ui_text_clip(F_REG, 12.5f, r->use, x + 180, y + 3, w - 190, TH.text3);
        ui_text_clip(F_MONO, 12, r->detail, x + 28, y + 24, w - 36, r->state < 0 ? TH.err : TH.text2);
        y += 50;
        if (i < E.n - 1) ui_divider(x, y - 6, w);
    }
    ui_scroll_end(&E.scroll, y - top);
    float by = b.y + b.height - 38;
    float bw = ui_button_width(ICON_SETTINGS, "Settings");
    if (ui_button(ui_id("env-settings"), (Rectangle){ b.x + b.width - bw, by, bw, 38 }, ICON_SETTINGS, "Settings", BTN_PRIMARY)) { *close = true; app_go(PG_SETTINGS); }
    float tw = ui_button_width(ICON_TERMINAL, "Terminal with OpenFOAM");
    if (ui_button(ui_id("env-term"), (Rectangle){ b.x + b.width - bw - 10 - tw, by, tw, 38 }, ICON_TERMINAL, "Terminal with OpenFOAM", BTN_SECONDARY)) {
        char ws[PL_PATH]; pl_expand_home(ws, sizeof ws, A.set.workspace);
        launch_terminal(&A.env, &A.set, ws);
    }
    ui_tooltip((Rectangle){ b.x + b.width - bw - 10 - tw, by, tw, 38 }, "A shell in the workspace with OpenFOAM's environment loaded");
    float cw = ui_button_width(ICON_REFRESH_CW, "Check again");
    if (ui_button(ui_id("env-recheck"), (Rectangle){ b.x, by, cw, 38 }, ICON_REFRESH_CW, "Check again", BTN_GHOST)) {
        env_detect(&A.env);
        build();
        ui_toast(TOAST_OK, "Checked again");
    }
    if (*close) E.built = false;                             /* fresh the next time it opens */
}
