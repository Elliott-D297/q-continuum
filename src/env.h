/* env.h -- what is installed (OpenFOAM, ParaView, editors), user settings, external launchers */
#ifndef ENV_H
#define ENV_H

#include <stdbool.h>
#include "pl_util.h"

typedef struct {
    char root[PL_PATH];        /* /usr/lib/openfoam/openfoam2606 */
    char bashrc[PL_PATH];      /* <root>/etc/bashrc              */
    char version[32];          /* "v2606"                        */
    bool hasTutorials;
} FoamInstall;

typedef struct {
    FoamInstall inst[16];
    int  nInst;
    bool wsl;
    char pvLinux[PL_PATH];     /* "" if none */
    char pvWin[PL_PATH];       /* /mnt/c/Program Files/ParaView 5.x/bin/paraview.exe */
    char code[PL_PATH];
    bool haveWt;
    int  nCpu;
    char user[64];
    char preciceEnv[PL_PATH];  /* script that puts preCICE on the paths ("" = on the system paths or absent) */
    char preciceVersion[32];   /* "3.4.0", "" if not found                                               */
    char preciceTutorials[PL_PATH];   /* a checkout of github.com/precice/tutorials, "" if none          */
    char salome[PL_PATH];      /* SALOME launcher: ~/salome/SALOME-x/salome, or salome_cfd/salome_meca  */
    char salomeVersion[32];
    char claude[PL_PATH];      /* Claude Code CLI, "" if not installed                                 */
    char pvbatch[PL_PATH];     /* ParaView's batch renderer (movies), "" if none                       */
    char ffmpeg[PL_PATH];      /* for encoding movies: on the PATH or in a conda environment            */
} Env;

enum { PV_AUTO = 0, PV_LINUX, PV_WINDOWS, PV_CUSTOM };

typedef struct {
    char  workspace[PL_PATH];
    char  bashrc[PL_PATH];      /* selected OpenFOAM environment ("" = auto) */
    int   pvMode;
    char  pvCustom[PL_PATH];    /* command, %f = .foam file                  */
    char  editor[PL_PATH];      /* command, %f = file                         */
    int   nProcs;
    float uiScale;
    int   theme;                /* 0 LCARS, 1 classic dark, 2 classic light   */
    float runSplit;             /* Run page: chart share of chart + log (0 = default) */
    bool  logFold;              /* log console: fold finished time steps            */
    int   whatsNewSeen;         /* the What's new revision last shown (app.c WHATS_NEW_REV) */
    int   deleteConfirm;        /* deleting a case: 0 type its name, 1 just confirm, 2 don't ask (it goes to the Trash) */
    bool  history;              /* record each case's setup in its .qc-history (git)  */
    bool  reviewSave;           /* show what a save changes before writing it (ROADMAP 3.5) */
    int   queueCores;           /* cores queued runs may fill (0 = all)                */
    bool  queuePaused;          /* the queue starts nothing until resumed              */
    int   notify;               /* desktop notification when a run ends: 0 never, 1 when the window isn't in front, 2 always */
    bool  notifySound;
    int   notifyMinSec;         /* only for runs that took at least this long          */
    char  lastCase[PL_PATH];    /* the session to come back to: case, page title, region */
    char  lastPage[48];
    char  lastRegion[64];
    bool  sounds;               /* the LCARS theme's sound cues                        */
    bool  meshView;             /* Mesh > View and the Overview's cross section drawn  */
    int   soundLevel;           /*   0 quiet, 1 medium, 2 loud                         */
    NameList recent;            /* cases opened from outside the workspace    */
    NameList demeDirs;          /* more folders to look for DEME projects in   */
    char  progDir[16][PL_PATH]; /* where new projects of each program go (by PROG_*), "" = its default (app_prog_dir) */
    /* AWAY MISSION (5.1): the cluster, as the user gives it; nothing is connected to, no passwords kept */
    char  awayHost[128], awayUser[64], awayDir[256], awayModules[512], awayPartition[64], awayAccount[64], awayTime[24];
    char  awayExport[PL_PATH];  /* where packages go ("" = ~/aerofly-export) */
    int   awayNodes, awayTasks;
    bool  fullscreen;           /* F11: opened full screen last time */
} Settings;

void env_detect(Env *e);
const FoamInstall *env_active(const Env *e, const Settings *s);   /* NULL if nothing found */
void env_tutorials_dir(const Env *e, const Settings *s, char *out, int cap);

void settings_defaults(Settings *s, const Env *e);
void settings_load(Settings *s, const Env *e);
extern bool g_settings_readonly;
void settings_save(const Settings *s);
void settings_add_recent(Settings *s, const char *path);

bool launch_paraview(const Env *e, const Settings *s, const char *foamFile, const char *script, char *err, int cap);   /* script: a pvpython file run in the GUI, or NULL */
void launch_folder(const Env *e, const char *dir);
void launch_editor(const Env *e, const Settings *s, const char *file);
bool launch_terminal(const Env *e, const Settings *s, const char *dir);
const char *env_videos_dir(const Env *e);       /* where movies go: Windows' Videos folder under WSL, else ~/Videos */
void notify_desktop(const Env *e, const char *title, const char *body, bool sound);
bool launch_terminal_run(const Env *e, const Settings *s, const char *dir, const char *run);   /* runs `run` first, then a shell */
const char *paraview_description(const Env *e, const Settings *s);

#endif
