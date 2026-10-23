/* page_deme.c -- DEME (DEM-Engine, GPU discrete elements) parts of the program pages (page_prog.c): the Setup form over a
 * project's params.ini (presets, Rayleigh time step and other checks; no rebuild), the driver's own results and the
 * GPU load while it runs, the built-in particle viewer (DEME sphere CSVs and SPARTA particle dumps), the starters
 * (settle, drum, hopper, impact) and a driver run alongside an OpenFOAM case (the two stop together, as a coupled
 * Allrun does). The data side (params, progress, frames) is demeparams.c. */
#define _GNU_SOURCE
#include <glob.h>
#include <signal.h>
#include <sys/wait.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "app.h"
#include "assets.h"
#include "demeparams.h"
#include "safety.h"

#define MAX_PROJ 48
#define MAX_FIELDS 64

static struct {
    bool     detected;
    char     demeDir[PL_PATH];             /* the folder with DEMEConfig.cmake */
    char     nvcc[PL_PATH];
    char     gpu[128];
    char     cudaArch[16];
    char     pairDeme[PL_PATH], pairFoam[PL_PATH];   /* a coupled pair running: stop one when the other ends */
    double   pairEndAt;
    int      pairCase;                     /* coupled run: index into A.list */
    /* setup: the project's params.ini and the text in each field */
    DemeParams params; char paramsFor[PL_PATH]; bool hasParams; double paramsCheckAt;
    char     edit[MAX_FIELDS][128];
    DemeProgress prog; char progFor[PL_PATH];
    char     buildErr[300]; double errFor;
    /* GPU load while a run goes */
    pid_t    gpuPid; char gpuFile[PL_PATH]; char gpuLine[96]; double gpuReadAt;
    /* the particle viewer */
    char     vDir[PL_PATH], vPrefix[128], vLoaded[PL_PATH];
    NameList vFiles;
    int      vIdx, vView, vColor, vOrderView, *vOrder;
    bool     vFollow, vPlay, vDrag, vBoundsSet, vViewSet;
    double   vListAt, vPlayAt;
    float    vLo[3], vHi[3];
    DemeFrame frame;
} D = { .pairCase = -1, .vIdx = -1, .vFollow = true, .vOrderView = -1 };

/* ============================================================================ detection */
static void detect(void)
{
    if (D.detected) return;
    D.detected = true;
    const char *pats[] = { "%s/src/DEM-Engine/build/DEME/DEMEConfig.cmake", "%s/DEM-Engine/build/DEME/DEMEConfig.cmake",
                           "%s/src/*/build/DEME/DEMEConfig.cmake", "/usr/local/lib/cmake/DEME/DEMEConfig.cmake", NULL };
    for (int i = 0; pats[i] && !D.demeDir[0]; i++) {
        char pat[PL_PATH]; snprintf(pat, sizeof pat, pats[i], pl_home());
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) { if (g.gl_pathc) pl_dirname(D.demeDir, sizeof D.demeDir, g.gl_pathv[0]); globfree(&g); }
    }
    if (!pl_which("nvcc", D.nvcc, sizeof D.nvcc)) {
        glob_t g;
        if (glob("/usr/local/cuda*/bin/nvcc", 0, NULL, &g) == 0) { if (g.gl_pathc) pl_strlcpy(D.nvcc, g.gl_pathv[g.gl_pathc - 1], sizeof D.nvcc); globfree(&g); }
    }
    char out[256] = "";
    pl_run_wait("{ command -v nvidia-smi >/dev/null && nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader; } 2>/dev/null || "
                "/usr/lib/wsl/lib/nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null", out, sizeof out);
    char *nl = strchr(out, '\n'); if (nl) *nl = 0;
    char *comma = strrchr(out, ',');
    if (comma) {
        *comma = 0;
        int k = 0; for (char *q = comma + 1; *q && k < 15; q++) if (*q >= '0' && *q <= '9') D.cudaArch[k++] = *q;
        D.cudaArch[k] = 0;
    }
    pl_strlcpy(D.gpu, out, sizeof D.gpu);
}

bool app_deme_running(void)
{
    for (int i = 0; i <= MAX_JOBS; i++) if (A.jobs[i].active && !strncmp(A.jobs[i].title, "DEME", 4)) return true;
    return false;
}

static void open_particles(const char *dir)
{
    const char *t = (const char *)asset_deme_view_template_py;
    Str s; str_init(&s);
    const char *at = memmem(t, asset_deme_view_template_py_len, "@OUT@", 5);   /* the asset has no NUL at its end */
    if (!at) { str_free(&s); return; }
    str_catn(&s, t, (size_t)(at - t));
    for (const char *q = dir; *q; q++) { if (*q == '"' || *q == '\\') str_cat(&s, "\\"); str_catn(&s, q, 1); }
    str_catn(&s, at + 5, (size_t)(t + asset_deme_view_template_py_len - (at + 5)));
    char sp[PL_PATH]; pl_join(sp, sizeof sp, dir, "deme_view.py");
    bool ok = pl_write_file(sp, s.s, s.len);
    str_free(&s);
    if (!ok) { ui_toast(TOAST_ERR, "Could not write %s", sp); return; }
    char err[256] = "";
    if (launch_paraview(&A.env, &A.set, sp, sp, err, sizeof err)) ui_toast(TOAST_OK, "Opening the particles in ParaView (press Play to step through the frames)");
    else ui_toast(TOAST_ERR, "%s", err);
}

/* ============================================================================ templates */
typedef struct {
    const char *id, *title, *desc;
    const unsigned char *src; const unsigned int *len;
    const unsigned char *ini; const unsigned int *iniLen;
} DemeTemplate;

static const DemeTemplate TPL[] = {
    { "settle", "Settle in a box", "Spheres poured into an open box settle under gravity: packing, bed height, the basics.",
      asset_settle_template_cpp, &asset_settle_template_cpp_len, asset_params_settle_ini, &asset_params_settle_ini_len },
    { "drum", "Rotating drum", "A part-filled drum turns and the grains tumble: rolling, cascading, cataracting with the speed.",
      asset_drum_template_cpp, &asset_drum_template_cpp_len, asset_params_drum_ini, &asset_params_drum_ini_len },
    { "hopper", "Hopper discharge", "Grains settle in a conical hopper, the gate opens and they drain: flow rate, clogging.",
      asset_hopper_template_cpp, &asset_hopper_template_cpp_len, asset_params_hopper_ini, &asset_params_hopper_ini_len },
    { "impact", "Impact crater", "A bed settles (lunar gravity), then a projectile strikes it: penetration depth and the crater.",
      asset_impact_template_cpp, &asset_impact_template_cpp_len, asset_params_impact_ini, &asset_params_impact_ini_len },
};
#define N_TPL ((int)(sizeof TPL / sizeof TPL[0]))

int deme_template_count(void) { return N_TPL; }
const char *deme_template_id(int i) { return i >= 0 && i < N_TPL ? TPL[i].id : ""; }

/* a DEME project from a starter: CMakeLists.txt naming the driver, src/<id>.cpp + qc_deme.hpp, params.ini */
bool deme_write_project(const char *dir, const char *tplId, char *err, int cap)
{
    const DemeTemplate *T = &TPL[0];
    for (int i = 0; i < N_TPL; i++) if (!strcmp(TPL[i].id, tplId)) T = &TPL[i];
    char src[PL_PATH]; pl_join(src, sizeof src, dir, "src");
    if (!pl_mkdirs(src)) { pl_fmt_ui(err, (size_t)cap, "Could not make %s", src); return false; }
    Str cm; str_init(&cm);
    const char *t = (const char *)asset_CMakeLists_template_txt, *e = t + asset_CMakeLists_template_txt_len;
    for (const char *q = t; q < e; ) {
        const char *at = memmem(q, (size_t)(e - q), "@DRIVER@", 8);   /* the asset has no NUL at its end */
        if (!at) { str_catn(&cm, q, (size_t)(e - q)); break; }
        str_catn(&cm, q, (size_t)(at - q)); str_cat(&cm, T->id); q = at + 8;
    }
    char p[PL_PATH], f[160];
    pl_join(p, sizeof p, dir, "CMakeLists.txt");
    bool ok = pl_write_file(p, cm.s, cm.len);
    str_free(&cm);
    snprintf(f, sizeof f, "%s.cpp", T->id);
    pl_join(p, sizeof p, src, f);
    ok = ok && pl_write_file(p, (const char *)T->src, *T->len);
    pl_join(p, sizeof p, src, "qc_deme.hpp");
    ok = ok && pl_write_file(p, (const char *)asset_qc_deme_hpp, asset_qc_deme_hpp_len);
    pl_join(p, sizeof p, dir, "params.ini");
    ok = ok && pl_write_file(p, (const char *)T->ini, *T->iniLen);
    if (!ok) pl_fmt_ui(err, (size_t)cap, "Could not write into %s", dir);
    return ok;
}

/* ============================================================================ GPU monitor */
/* nvidia-smi samples every 2 s into <run>/.qc-gpu.csv while a DEME run goes; the page reads the last line */
static void gpu_stop(void)
{
    if (D.gpuPid > 0) { kill(-D.gpuPid, SIGTERM); waitpid(D.gpuPid, NULL, WNOHANG); }
    D.gpuPid = 0;
}

static void gpu_start(const char *runDir)
{
    if (D.gpuPid > 0 || getenv("QC_TEST_NO_GPU")) return;
    pl_join(D.gpuFile, sizeof D.gpuFile, runDir, ".qc-gpu.csv");
    const char *smi = access("/usr/lib/wsl/lib/nvidia-smi", X_OK) == 0 ? "/usr/lib/wsl/lib/nvidia-smi" : "nvidia-smi";
    char q[PL_PATH + 8], cmd[PL_PATH + 300];
    pl_shell_quote(q, sizeof q, D.gpuFile);
    snprintf(cmd, sizeof cmd, "exec %s --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits -l 2 > %s 2>/dev/null", smi, q);
    pid_t p = fork();
    if (p == 0) { setsid(); execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
    if (p > 0) D.gpuPid = p;
}

static void gpu_read(void)
{
    if (D.gpuPid <= 0 || GetTime() - D.gpuReadAt < 1.5) return;
    D.gpuReadAt = GetTime();
    char *t = pl_read_file(D.gpuFile, NULL);
    if (!t) return;
    char *last = NULL;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) last = l;
    int u = 0, used = 0, tot = 0;
    if (last && sscanf(last, "%d, %d, %d", &u, &used, &tot) == 3)
        snprintf(D.gpuLine, sizeof D.gpuLine, "GPU %d %%  ·  memory %.1f of %.1f GB", u, used / 1024.0, tot / 1024.0);
    free(t);
}

/* ============================================================================ coupled pairs, every frame */
void deme_pair_tick(double now)
{
    if (D.gpuPid > 0 && !app_deme_running()) gpu_stop();
    if (!D.pairDeme[0]) return;
    Job *dj = NULL, *fj = NULL;
    for (int i = 0; i <= MAX_JOBS; i++) {
        if (!strcmp(A.jobs[i].casePath, D.pairDeme)) dj = &A.jobs[i];
        if (!strcmp(A.jobs[i].casePath, D.pairFoam)) fj = &A.jobs[i];
    }
    bool da = dj && dj->active, fa = fj && fj->active;
    if (da && fa) { D.pairEndAt = 0; return; }
    if (!da && !fa) { D.pairDeme[0] = D.pairFoam[0] = 0; return; }
    if (D.pairEndAt == 0) { D.pairEndAt = now; return; }
    if (now - D.pairEndAt < 10) return;                       /* its partner may still be writing its last exchange */
    Job *other = da ? dj : fj;
    ui_toast(TOAST_WARN, "Coupled run: %s ended, so %s is stopped too", da ? "the OpenFOAM case" : "the DEME driver", da ? "the DEME driver" : "the OpenFOAM case");
    job_stop(other, now);
    D.pairDeme[0] = D.pairFoam[0] = 0;
}

void deme_toolchain(const char **demeDir, const char **nvcc, const char **gpu)
{
    detect();
    *demeDir = D.demeDir; *nvcc = D.nvcc; *gpu = D.gpu;
}

/* ============================================================================ page parts */
/* the project's driver source: src/<program>.cpp, else the first .cpp in src/ */
bool deme_driver_source(const char *proj, const char *target, char *out, int cap)
{
    char src[PL_PATH]; pl_join(src, sizeof src, proj, "src");
    if (target && *target) {
        char f[160]; snprintf(f, sizeof f, "%s.cpp", target);
        pl_join(out, (size_t)cap, src, f);
        if (pl_exists(out)) return true;
    }
    NameList l = { 0 };
    pl_list_dir(src, &l, false, true);
    names_sort(&l);
    bool ok = false;
    for (int i = 0; i < l.n && !ok; i++) if (pl_ends_with(l.names[i], ".cpp") || pl_ends_with(l.names[i], ".cu")) { pl_join(out, (size_t)cap, src, l.names[i]); ok = true; }
    names_free(&l);
    return ok;
}

/* the first compiler or CMake error in log.build, "file:line: error: what" trimmed to what fits */
void deme_build_error(const char *proj, char *out, int cap)
{
    out[0] = 0;
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "log.build");
    char *t = pl_read_file(p, NULL);
    if (!t) return;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n"))
        if (strstr(l, "error:") || strstr(l, "error :") || strstr(l, "CMake Error") || strstr(l, ": fatal error")) {
            while (*l == ' ') l++;
            const char *home = pl_home();
            if (!strncmp(l, proj, strlen(proj))) l += strlen(proj) + 1;               /* relative to the project */
            else if (home[0] && !strncmp(l, home, strlen(home))) { l += strlen(home) - 1; *l = '~'; }
            pl_strlcpy(out, l, (size_t)cap);
            break;
        }
    free(t);
}

static void sync_params(const char *proj)
{
    char p[PL_PATH]; pl_join(p, sizeof p, proj, "params.ini");
    bool look = GetTime() - D.paramsCheckAt > 1;
    if (look) D.paramsCheckAt = GetTime();
    if (strcmp(D.paramsFor, proj) || (!D.params.dirty && look && deme_params_stamp(p) != D.params.stamp)) {
        D.hasParams = deme_params_load(&D.params, p);
        pl_strlcpy(D.paramsFor, proj, sizeof D.paramsFor);
        for (int i = 0; i < D.params.n && i < MAX_FIELDS; i++) pl_strlcpy(D.edit[i], D.params.p[i].value, sizeof D.edit[i]);
    }
}

static void params_changed(void)
{
    if (!deme_params_save(&D.params)) { ui_toast(TOAST_ERR, "Could not save %s", D.params.path); return; }
    for (int i = 0; i < D.params.n && i < MAX_FIELDS; i++) pl_strlcpy(D.edit[i], D.params.p[i].value, sizeof D.edit[i]);
}

/* rough particle count for the fill region the template describes: random packing ~0.58 of the volume */
static double estimate_particles(const DemeParams *d)
{
    double r0 = deme_param_num(d, "radius_min", 0), r1 = fmax(r0, deme_param_num(d, "radius_max", r0));
    if (r0 <= 0) return 0;
    double rm = (r0 + r1) / 2, vp = 4.0 / 3.0 * M_PI * rm * rm * rm, V = 0;
    double w = deme_param_num(d, "box_width", 0);
    if (w > 0 && deme_param_num(d, "fill_height", 0) > 0 && deme_param_num(d, "top_radius", 0) <= 0) V = w * w * deme_param_num(d, "fill_height", 0);
    else if (w > 0 && deme_param_num(d, "bed_depth", 0) > 0) V = w * w * deme_param_num(d, "bed_depth", 0);
    else if (deme_param_num(d, "drum_radius", 0) > 0) {
        double R = deme_param_num(d, "drum_radius", 0);
        V = M_PI * R * R * deme_param_num(d, "drum_length", 0) * deme_param_num(d, "fill_fraction", 0.35);
    } else if (deme_param_num(d, "top_radius", 0) > 0) {
        double R = deme_param_num(d, "top_radius", 0);
        V = 0.6 * M_PI * R * R * deme_param_num(d, "fill_height", 0);                 /* the cone takes some */
    }
    /* the sampler spaces points by the largest diameter: that, not the packing, sets the starting count */
    double start = V / pow(2.02 * r1, 3) * 0.52;
    (void)vp;
    return start;
}

typedef struct { Color c; const char *text; const char *fix; } Check;

/* the Setup checks, from the values: time step vs Rayleigh, the step count, size and template-specific numbers */
static int setup_checks(Check *ck, char texts[][200], double *fixDt)
{
    const DemeParams *d = &D.params;
    int n = 0;
    double r0 = deme_param_num(d, "radius_min", 0), rho = deme_param_num(d, "density", 2500);
    double E = deme_param_num(d, "youngs_modulus", 1e8), nu = deme_param_num(d, "poisson_ratio", 0.3), dt = deme_param_num(d, "dt", 0);
    double tr = deme_rayleigh_dt(r0, rho, E, nu);
    *fixDt = 0;
    if (tr > 0 && dt > 0) {
        double k = dt / tr;
        snprintf(texts[n], 200, "Time step %.3g s is %.0f %% of the Rayleigh step of the smallest grain (%.3g s)%s", dt, k * 100, tr,
                 k > 0.3 ? ": too big, contacts will blow up" : k < 0.05 ? ": safe but slow" : "");
        ck[n] = (Check){ k > 0.3 ? TH.err : k < 0.05 ? TH.warn : TH.ok, texts[n], k > 0.3 || k < 0.05 ? "Use 20 %" : NULL };
        if (ck[n].fix) *fixDt = 0.2 * tr;
        n++;
    }
    double simT = deme_param_num(d, "sim_time", 0) + deme_param_num(d, "settle_time", 0);
    if (dt > 0 && simT > 0) {
        double steps = simT / dt;
        /* measured on an RTX 3070 (settle, 3200 spheres): ~500 steps a second, after 2-3 minutes compiling DEME's kernels */
        char d[32]; pl_duration(d, sizeof d, steps / 500);
        pl_fmt_ui(texts[n], 200, "%.0f steps for %.3g s simulated: about %s of stepping, after 2-3 minutes of start-up", steps, simT, d);
        ck[n] = (Check){ steps / 500 < 3600 ? TH.text2 : TH.warn, texts[n], NULL }; n++;
    }
    double est = estimate_particles(d), mx = deme_param_num(d, "max_particles", 0);
    if (est > 0) {
        double use = mx > 0 && est > mx ? mx : est;
        snprintf(texts[n], 200, "About %.0f particles%s", use, mx > 0 && est > mx ? " (cut to Most particles; the fill region holds more)" : "");
        ck[n] = (Check){ use > 300000 ? TH.warn : TH.text2, texts[n], NULL }; n++;
    }
    if (deme_param_num(d, "drum_radius", 0) > 0) {
        double R = deme_param_num(d, "drum_radius", 0), w = deme_param_num(d, "rpm", 30) * 2 * M_PI / 60, g = deme_param_num(d, "gravity", 9.81);
        double fr = g > 0 ? w * w * R / g : 0;
        snprintf(texts[n], 200, "Froude number %.3g: %s", fr, fr < 1e-3 ? "slumping" : fr < 0.01 ? "rolling" : fr < 0.3 ? "cascading" :
                 fr < 1 ? "cataracting" : "centrifuging (the bed sticks to the wall)");
        ck[n] = (Check){ fr >= 1 ? TH.warn : TH.text2, texts[n], NULL }; n++;
    }
    if (deme_param_num(d, "outlet_radius", 0) > 0) {
        double D0 = 2 * deme_param_num(d, "outlet_radius", 0), dg = 2 * fmax(r0, deme_param_num(d, "radius_max", r0));
        double k = dg > 0 ? D0 / dg : 0;
        snprintf(texts[n], 200, "Outlet %.1f grain diameters wide%s", k, k < 3 ? ": it will jam" : k < 5 ? ": it may arch and clog" : "");
        ck[n] = (Check){ k < 3 ? TH.err : k < 5 ? TH.warn : TH.ok, texts[n], NULL }; n++;
    }
    if (deme_param_num(d, "impact_speed", 0) > 0) {
        double Rp = deme_param_num(d, "projectile_radius", 0), rp = deme_param_num(d, "projectile_density", 7800), v = deme_param_num(d, "impact_speed", 0);
        double m = rp * 4.0 / 3.0 * M_PI * Rp * Rp * Rp;
        double ratio = r0 > 0 ? Rp / fmax(r0, deme_param_num(d, "radius_max", r0)) : 0;
        snprintf(texts[n], 200, "Projectile %.3g g at %.3g m/s: %.3g J, %.1f times the largest grain%s", m * 1000, v, 0.5 * m * v * v, ratio,
                 ratio < 3 ? ": too small to make a clean crater" : "");
        ck[n] = (Check){ ratio < 3 ? TH.warn : TH.text2, texts[n], NULL }; n++;
    }
    return n;
}

static float setup_section(float x, float y, float w)
{
    y += section_head_r(x, y, w, "Setup", "params.ini: the driver reads it when it starts, so a change takes effect on the next run, no rebuild. "
                        "Hover a name for what it does.", D.params.dirty ? "unsaved" : NULL);
    float colW = (w - 24) / 2;
    const char *sec = NULL;
    int col = 0;
    float rowY = y;
    for (int i = 0; i < D.params.n && i < MAX_FIELDS; i++) {
        DemeParam *p = &D.params.p[i];
        if (!sec || strcmp(sec, p->section)) {
            if (col) { rowY += 64; col = 0; }
            if (sec) rowY += 6;
            sec = p->section;
            ui_text(F_SEMI, 14, ui_cap_words(sec[0] ? sec : "Settings"), x, rowY, head_color());
            /* presets beside the section name */
            float px = x + ui_text_w(F_SEMI, 14, ui_cap_words(sec[0] ? sec : "Settings")) + 18;
            if (!strcmp(sec, "Gravity"))
                for (int g = 0; g < DEME_N_GRAVITY; g++) {
                    float bw = ui_button_width(NULL, DEME_GRAVITY[g].name) - 8;
                    if (ui_button(ui_idn(ui_id("dm-grav"), g), (Rectangle){ px, rowY - 6, bw, 28 }, NULL, DEME_GRAVITY[g].name, BTN_SECONDARY)) {
                        deme_apply_preset(&D.params, &DEME_GRAVITY[g]); params_changed();
                    }
                    ui_tooltip((Rectangle){ px, rowY - 6, bw, 28 }, DEME_GRAVITY[g].note);
                    px += bw + 6;
                }
            if (!strcmp(sec, "Material") || !strcmp(sec, "Particles")) {
                const char *names[8]; for (int m = 0; m < DEME_N_MATERIALS; m++) names[m] = DEME_MATERIALS[m].name;
                char pick[64] = "";
                UiId mid = ui_id(!strcmp(sec, "Material") ? "dm-mat" : "dm-mat2");
                if (!strcmp(sec, "Material") && ui_dropdown(mid, (Rectangle){ px, rowY - 6, 220, 28 }, "", names, DEME_N_MATERIALS, 0, pick, sizeof pick, "Material preset"))
                    for (int m = 0; m < DEME_N_MATERIALS; m++) if (!strcmp(pick, DEME_MATERIALS[m].name)) {
                        int k = deme_apply_preset(&D.params, &DEME_MATERIALS[m]); params_changed();
                        ui_toast(TOAST_OK, "%s: %d values set (%s). Starting values: calibrate friction against an angle of repose.", pick, k, DEME_MATERIALS[m].note);
                    }
            }
            rowY += 28;
        }
        float fx = x + col * (colW + 24);
        Rectangle lab = { fx, rowY, colW, 18 };
        ui_text_clip(F_MED, 12.5f, p->label, fx, rowY, colW - ui_text_w(F_REG, 12, p->unit) - 10, TH.text2);
        if (p->unit[0]) ui_text(F_REG, 12, p->unit, fx + colW - ui_text_w(F_REG, 12, p->unit), rowY + 1, TH.text3);
        char tip[400]; pl_fmt_ui(tip, sizeof tip, "%s  (%s in params.ini)", p->help[0] ? p->help : p->label, p->key);
        ui_tooltip(lab, tip);
        if (ui_input(ui_idn(ui_id("dm-param"), i), (Rectangle){ fx, rowY + 20, colW, 34 }, D.edit[i], sizeof D.edit[i], p->key, INPUT_MONO)) {
            pl_trim_inplace(D.edit[i]);
            if (D.edit[i][0]) { deme_param_set(&D.params, p->key, D.edit[i]); params_changed(); }
            else pl_strlcpy(D.edit[i], p->value, sizeof D.edit[i]);
        }
        ui_register(ui_idn(ui_id("dm-param"), i), (Rectangle){ fx, rowY + 20, colW, 34 }, p->key);
        if (++col == 2) { col = 0; rowY += 64; }
    }
    if (col) rowY += 64;
    y = rowY + 8;
    /* what the values mean */
    Check ck[8]; char texts[8][200]; double fixDt = 0;
    int nc = setup_checks(ck, texts, &fixDt);
    for (int i = 0; i < nc; i++) {
        bool bad = ck[i].c.r == TH.err.r && ck[i].c.g == TH.err.g, warn = ck[i].c.r == TH.warn.r && ck[i].c.g == TH.warn.g;
        bool good = ck[i].c.r == TH.ok.r && ck[i].c.g == TH.ok.g;
        ui_icon(bad ? ICON_CIRCLE_X : warn ? ICON_TRIANGLE_ALERT : good ? ICON_CIRCLE_CHECK : ICON_INFO, x + 8, y + 9, 14, ck[i].c);
        float tw = w - 30 - (ck[i].fix ? 120 : 0);
        ui_text_clip(F_REG, 13, ck[i].text, x + 22, y + 1, tw, bad || warn ? ck[i].c : TH.text2);
        if (ck[i].fix && fixDt > 0) {
            if (ui_button(ui_id("dm-fix-dt"), (Rectangle){ x + w - 110, y - 5, 110, 28 }, NULL, ck[i].fix, BTN_SECONDARY)) {
                char v[24]; snprintf(v, sizeof v, "%.2g", fixDt);          /* 3.1e-06, not 3.0572e-06 */
                fixDt = atof(v);
                deme_param_set(&D.params, "dt", v); params_changed();
                ui_toast(TOAST_OK, "Time step set to %.3g s (20 %% of the Rayleigh step)", fixDt);
            }
        }
        y += 26;
    }
    return y + 10;
}

/* ---------------------------------------------------------------- the driver's results and the GPU while it runs */
float deme_run_panel(const char *proj, const char *logPath, float x, float y, float w, bool running)
{
    char rd[PL_PATH]; pl_join(rd, sizeof rd, proj, "run");
    if (strcmp(D.progFor, logPath)) { deme_progress_reset(&D.prog); pl_strlcpy(D.progFor, logPath, sizeof D.progFor); }
    deme_progress_poll(&D.prog, logPath, GetTime());
    if (running) { gpu_start(rd); gpu_read(); }
    DemeProgress *p = &D.prog;
    float y0 = y;
    if (!p->any && running) {
        y += ui_text_wrap(F_REG, 13, "Starting: DEME compiles its GPU kernels before the first step (2-3 minutes), then frames follow.",
                          (Rectangle){ x, y, w, 40 }, 19, TH.text2, true) + 8;
    }
    if (p->any || (running && D.gpuLine[0])) {
        char t2[300];
        pl_fmt_ui(t2, sizeof t2, "%s%sframe %d of %d  ·  kinetic energy %.3g J  ·  fastest %.3g m/s%s%s%s%s",
                  p->n ? TextFormat("%ld particles", p->n) : "", p->n ? "  ·  " : "", p->frame, p->frames, p->ke, p->vmax,
                  D.gpuLine[0] && running ? "  ·  " : "", running ? D.gpuLine : "", p->extra[0] ? "  ·  " : "", p->extra);
        ui_text_clip(F_REG, 12.5f, t2, x, y, w, TH.text2);
        y += 24;
    }
    if (p->nRes) {
        float rh = 30 + 22 * (float)p->nRes;
        Rectangle rr = { x, y, w, rh };
        ui_rect(rr, 8, TH.surface2);
        ui_text(F_SEMI, 13, ui_cap("Results from the driver"), rr.x + 14, rr.y + 10, head_color());
        float ry = rr.y + 32;
        for (int i = 0; i < p->nRes; i++) {
            ui_text_clip(F_REG, 12.5f, p->resName[i], rr.x + 14, ry, rr.width * 0.5f, TH.text2);
            ui_text_clip(F_MONO_MED, 12.5f, p->resVal[i], rr.x + rr.width * 0.5f, ry, rr.width * 0.5f - 10, TH.text);
            ry += 22;
        }
        y += rh + 10;
    }
    if (p->crash && !running) {                           /* what the log says went wrong, in words */
        Color c = p->crash == DEME_CRASH_NO_PARAMS ? TH.warn : TH.err;
        const char *m = deme_crash_text(p->crash);
        float hh = ui_text_wrap(F_REG, 13, m, (Rectangle){ x + 40, y + 12, w - 56, 120 }, 19, TH.text, false) + 24;
        ui_rect((Rectangle){ x, y, w, hh }, 8, ui_alpha(c, 0.12f));
        ui_icon(ICON_TRIANGLE_ALERT, x + 20, y + 21, 16, c);
        ui_text_wrap(F_REG, 13, m, (Rectangle){ x + 40, y + 12, w - 56, 120 }, 19, TH.text, true);
        y += hh + 10;
    }
    return y - y0;
}

/* ---------------------------------------------------------------- particle viewer */
static int frame_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* SPARTA dump.flow.200 before dump.flow.1000: by the number after the prefix, else by name */
static int frame_cmp_num(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    const char *dx = strrchr(x, '.'), *dy = strrchr(y, '.');
    if (dx && dy && pl_is_number(dx + 1) && pl_is_number(dy + 1)) { double u = atof(dx + 1), v = atof(dy + 1); return (u > v) - (u < v); }
    return strcmp(x, y);
}

/* the frames: the CSVs of a folder, or the files <prefix>.* (SPARTA dumps) */
static void frames_list(const char *dir, const char *prefix, bool running)
{
    bool same = !strcmp(D.vDir, dir) && !strcmp(D.vPrefix, prefix ? prefix : "");
    if (same && GetTime() - D.vListAt < (running ? 1.0 : 5.0)) return;
    if (!same) {                                               /* another project or output folder: a fresh view */
        pl_strlcpy(D.vDir, dir, sizeof D.vDir);
        pl_strlcpy(D.vPrefix, prefix ? prefix : "", sizeof D.vPrefix);
        D.vIdx = -1; D.vLoaded[0] = 0; D.vFollow = true; D.vBoundsSet = false; D.vViewSet = false;
    }
    D.vListAt = GetTime();
    names_free(&D.vFiles);
    NameList l = { 0 };
    pl_list_dir(dir, &l, false, true);
    for (int i = 0; i < l.n; i++) {
        const char *n = l.names[i];
        if (D.vPrefix[0] ? (pl_starts_with(n, D.vPrefix) && n[strlen(D.vPrefix)] == '.') : pl_ends_with(n, ".csv")) names_add(&D.vFiles, n);
    }
    names_free(&l);
    if (D.vFiles.n > 1) qsort(D.vFiles.names, (size_t)D.vFiles.n, sizeof D.vFiles.names[0], D.vPrefix[0] ? frame_cmp_num : frame_cmp);
    if (D.vFollow || D.vIdx < 0 || D.vIdx >= D.vFiles.n) D.vIdx = D.vFiles.n - 1;
}

/* draw order: far particles first, per view */
static float *G_DEPTH;
static int depth_cmp(const void *a, const void *b) { float da = G_DEPTH[*(const int *)a], db = G_DEPTH[*(const int *)b]; return (da > db) - (da < db); }

static void frame_load_current(void)
{
    if (D.vIdx < 0 || D.vIdx >= D.vFiles.n) return;
    char p[PL_PATH]; pl_join(p, sizeof p, D.vDir, D.vFiles.names[D.vIdx]);
    if (!strcmp(p, D.vLoaded) && D.vOrderView == D.vView) return;
    if (strcmp(p, D.vLoaded)) {
        if (!deme_frame_load(&D.frame, p)) { D.frame.n = 0; }
        pl_strlcpy(D.vLoaded, p, sizeof D.vLoaded);
        if (D.frame.n) {
            /* the view only grows, so playing through the frames doesn't jump */
            for (int k = 0; k < 3; k++) {
                float lo = D.frame.lo[k] - D.frame.rmax, hi = D.frame.hi[k] + D.frame.rmax;
                if (!D.vBoundsSet || lo < D.vLo[k]) D.vLo[k] = lo;
                if (!D.vBoundsSet || hi > D.vHi[k]) D.vHi[k] = hi;
            }
            D.vBoundsSet = true;
            if (!D.vViewSet) { D.vViewSet = true; if (D.vHi[2] - D.vLo[2] < 1e-9f * (1 + D.vHi[0] - D.vLo[0])) D.vView = 2; }   /* 2-D: from the top */
        }
    }
    free(D.vOrder);
    D.vOrder = malloc(sizeof(int) * (size_t)(D.frame.n ? D.frame.n : 1));
    for (int i = 0; i < D.frame.n; i++) D.vOrder[i] = i;
    G_DEPTH = D.vView == 0 ? D.frame.y : D.vView == 1 ? D.frame.x : D.frame.z;
    if (D.vView == 3) {                                       /* iso: the depth toward the eye at (1, -1, 1) */
        float *dp = malloc(sizeof(float) * (size_t)(D.frame.n ? D.frame.n : 1));
        for (int i = 0; i < D.frame.n; i++) dp[i] = D.frame.x[i] - D.frame.y[i] + D.frame.z[i];
        G_DEPTH = dp;
        qsort(D.vOrder, (size_t)D.frame.n, sizeof(int), depth_cmp);
        free(dp);
    } else if (D.vView == 0 || D.vView == 1) {                       /* side views: the near side is -y / -x, drawn last */
        float *neg = malloc(sizeof(float) * (size_t)(D.frame.n ? D.frame.n : 1));
        for (int i = 0; i < D.frame.n; i++) neg[i] = -G_DEPTH[i];
        G_DEPTH = neg;
        qsort(D.vOrder, (size_t)D.frame.n, sizeof(int), depth_cmp);
        free(neg);
    } else qsort(D.vOrder, (size_t)D.frame.n, sizeof(int), depth_cmp);
    D.vOrderView = D.vView;
}

/* viridis, 5 stops */
static Color cmap(float t)
{
    static const unsigned V[5] = { 0x440154, 0x3B528B, 0x21918C, 0x5EC962, 0xFDE725 };
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float s = t * 4; int i = (int)s; if (i > 3) i = 3;
    float f = s - i;
    Color a = { (unsigned char)(V[i] >> 16), (unsigned char)(V[i] >> 8), (unsigned char)V[i], 255 };
    Color b = { (unsigned char)(V[i + 1] >> 16), (unsigned char)(V[i + 1] >> 8), (unsigned char)V[i + 1], 255 };
    return ui_mix(a, b, f);
}

/* isometric: the eye at (1, -1, 1) (front right, above), z up; u to the right, v up the screen */
static void iso_uv(float x, float y, float z, float *u, float *v)
{
    *u = (x + y) * 0.70710678f;
    *v = (-x + y + 2 * z) * 0.40824829f;
}

static void viewer(Rectangle r)
{
    ui_rect(r, 10, TH.dark ? (Color){ 6, 8, 12, 255 } : (Color){ 20, 24, 32, 255 });
    ui_rect_lines(r, 10, 1, TH.lineSoft);
    if (!D.frame.n) { ui_text(F_REG, 13, "No particles in this frame.", r.x + 20, r.y + 20, TH.text3); return; }
    int a = D.vView == 2 ? 0 : D.vView == 0 ? 0 : 1, b = D.vView == 2 ? 1 : 2;   /* side: x,z  front: y,z  top: x,y */
    float *A0 = a == 0 ? D.frame.x : D.frame.y, *B0 = b == 1 ? D.frame.y : D.frame.z;
    float loA = D.vLo[a], loB = D.vLo[b], spanA = D.vHi[a] - D.vLo[a], spanB = D.vHi[b] - D.vLo[b];
    bool iso = D.vView == 3;
    if (iso) {                                                /* the box's corners on screen give the extent */
        loA = loB = 1e30f; float hiA = -1e30f, hiB = -1e30f;
        for (int k = 0; k < 8; k++) {
            float p[3] = { k & 1 ? D.vHi[0] : D.vLo[0], k & 2 ? D.vHi[1] : D.vLo[1], k & 4 ? D.vHi[2] : D.vLo[2] };
            float u, v; iso_uv(p[0], p[1], p[2], &u, &v);
            loA = fminf(loA, u); hiA = fmaxf(hiA, u); loB = fminf(loB, v); hiB = fmaxf(hiB, v);
        }
        spanA = hiA - loA; spanB = hiB - loB;
    }
    if (spanA <= 0 || spanB <= 0) return;
    Rectangle in = ui_inset(r, 16, 16);
    float sc = fminf(in.width / spanA, in.height / spanB);
    float ox = in.x + (in.width - spanA * sc) / 2, oy = in.y + (in.height + spanB * sc) / 2;
    if (iso) {                                                /* the domain's box, faint, for which way is up */
        static const int E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
        for (int e = 0; e < 12; e++) {
            Vector2 q[2];
            for (int j = 0; j < 2; j++) {
                int k = E[e][j]; float u, v;
                iso_uv(k & 1 ? D.vHi[0] : D.vLo[0], k & 2 ? D.vHi[1] : D.vLo[1], k & 4 ? D.vHi[2] : D.vLo[2], &u, &v);
                q[j] = (Vector2){ ox + (u - loA) * sc, oy - (v - loB) * sc };
            }
            ui_line(q[0].x, q[0].y, q[1].x, q[1].y, 1, ui_alpha(WHITE, 0.18f));
        }
    }
    /* the colour scale: speed over this frame's fastest, height or size over the view's range */
    float c0, c1;
    const float *cv;
    if (D.vColor == 0) { cv = D.frame.v; c0 = 0; c1 = D.frame.vmax > 0 ? D.frame.vmax : 1; }
    else if (D.vColor == 1) { cv = D.frame.z; c0 = D.vLo[2]; c1 = D.vHi[2]; }
    else { cv = D.frame.r; c0 = D.frame.rmin; c1 = D.frame.rmax > D.frame.rmin ? D.frame.rmax : D.frame.rmin + 1e-9f; }
    ui_clip_push(r);
    for (int k = 0; k < D.frame.n; k++) {
        int i = D.vOrder[k];
        float pu = A0[i], pv = B0[i];
        if (iso) iso_uv(D.frame.x[i], D.frame.y[i], D.frame.z[i], &pu, &pv);
        Vector2 c = { ox + (pu - loA) * sc, oy - (pv - loB) * sc };
        float rad = D.frame.r[i] * sc;
        Color col = cmap((cv[i] - c0) / (c1 - c0));
        if (rad < 1.2f) DrawRectangleV((Vector2){ c.x - 1, c.y - 1 }, (Vector2){ 2, 2 }, col);
        else {
            int seg = rad < 3 ? 6 : rad < 8 ? 12 : 20;
            DrawCircleSector(c, rad, 0, 360, seg, col);
            if (rad > 3) DrawCircleSector((Vector2){ c.x - rad * 0.3f, c.y - rad * 0.3f }, rad * 0.35f, 0, 360, 8, ui_alpha(WHITE, 0.25f));
        }
    }
    ui_clip_pop();
    /* scale bar and colour legend */
    double target = spanA / 5, mag = pow(10, floor(log10(target))), bar = mag * (target / mag >= 5 ? 5 : target / mag >= 2 ? 2 : 1);
    float bw = (float)(bar * sc);
    ui_line(r.x + 18, r.y + r.height - 18, r.x + 18 + bw, r.y + r.height - 18, 2, WHITE);
    char bt[32]; snprintf(bt, sizeof bt, bar >= 0.01 ? "%.0f cm" : "%.0f mm", bar >= 0.01 ? bar * 100 : bar * 1000);
    ui_text(F_MONO, 11, bt, r.x + 18, r.y + r.height - 36, WHITE);
    Rectangle lg = { r.x + r.width - 186, r.y + r.height - 22, 160, 8 };
    for (int i = 0; i < 32; i++) DrawRectangleRec((Rectangle){ lg.x + lg.width * i / 32, lg.y, lg.width / 32 + 1, lg.height }, cmap(i / 31.0f));
    static const char *const CN[3] = { "speed, m/s", "height, m", "radius, m" };
    char l0[24], l1[24]; snprintf(l0, sizeof l0, "%.3g", c0); snprintf(l1, sizeof l1, "%.3g", c1);
    ui_text(F_MONO, 11, l0, lg.x, lg.y - 16, WHITE);
    ui_text(F_MONO, 11, l1, lg.x + lg.width - ui_text_w(F_MONO, 11, l1), lg.y - 16, WHITE);
    ui_text(F_REG, 11.5f, CN[D.vColor], lg.x + lg.width / 2 - ui_text_w(F_REG, 11.5f, CN[D.vColor]) / 2, lg.y - 16, ui_alpha(WHITE, 0.8f));
    static const char *const VN[4] = { "side (x, z)", "front (y, z)", "top (x, y)", "iso (z up)" };
    ui_text(F_REG, 12, VN[D.vView], r.x + 18, r.y + 14, ui_alpha(WHITE, 0.7f));
}

static void frame_slider(Rectangle r)
{
    int n = D.vFiles.n;
    UiId id = ui_id("dm-frame");
    ui_register(id, r, "frame");
    Rectangle track = { r.x, r.y + r.height / 2 - 3, r.width, 6 };
    ui_rect(track, 3, TH.lineSoft);
    if (n < 2) return;
    float fx = r.x + r.width * D.vIdx / (n - 1);
    ui_rect((Rectangle){ r.x, track.y, fx - r.x, 6 }, 3, TH.accent);
    bool hov = ui_hovered(r);
    if (hov) ui_cursor(MOUSE_CURSOR_POINTING_HAND);
    if (ui_pressed(r) || (D.vDrag && ui_mouse_down())) {
        D.vDrag = true;
        float u = (ui_mouse().x - r.x) / r.width;
        int k = (int)lroundf((u < 0 ? 0 : u > 1 ? 1 : u) * (n - 1));
        if (k != D.vIdx) { D.vIdx = k; D.vFollow = k == n - 1; D.vPlay = false; }
    }
    if (!ui_mouse_down()) D.vDrag = false;
    DrawCircleV((Vector2){ fx, r.y + r.height / 2 }, hov || D.vDrag ? 9 : 7, TH.accent);
    if (hov) {
        float wh = ui_take_wheel();
        if (wh != 0) { int k = D.vIdx + (wh < 0 ? 1 : -1); if (k >= 0 && k < n) { D.vIdx = k; D.vFollow = k == n - 1; } }
    }
}

/* the built-in viewer: frames of dir (CSVs, or <prefix>.* dumps); compact = viewer and slider only (Overview) */
float deme_particles_panel(const char *dir, const char *prefix, float x, float y, float w, float maxH, bool running, bool compact)
{
    frames_list(dir, prefix, running);
    float y0 = y;
    if (!D.vFiles.n) return 0;
    static const char *views[] = { "Side", "Front", "Top", "Iso" }, *cols[] = { "Speed", "Height", "Size" };
    if (!compact) {
        int ov = D.vView;
        ui_segmented(ui_id("dm-view"), (Rectangle){ x, y, 270, 32 }, views, 4, &D.vView);
        if (ov != D.vView) D.vLoaded[0] = 0;
        ui_segmented(ui_id("dm-color"), (Rectangle){ x + 282, y, 220, 32 }, cols, 3, &D.vColor);
    }
    float bx = compact ? x : x + 514;
    if (ui_button(ui_id("dm-play"), (Rectangle){ bx, y, 96, 32 }, D.vPlay ? ICON_PAUSE : ICON_PLAY, D.vPlay ? "Pause" : "Play", BTN_SECONDARY)) {
        D.vPlay = !D.vPlay; D.vFollow = false;
        if (D.vPlay && D.vIdx >= D.vFiles.n - 1) D.vIdx = 0;
        D.vPlayAt = GetTime();
    }
    bool fol = D.vFollow;
    ui_checkbox(ui_id("dm-follow"), (Rectangle){ bx + 108, y + 6, 150, 20 }, &D.vFollow, running ? "Follow the run" : "Newest frame");
    if (D.vFollow && !fol) { D.vPlay = false; D.vIdx = D.vFiles.n - 1; }
    if (!compact && !(prefix && *prefix)) {                   /* ParaView reads the CSVs through a script; SPARTA dumps it can't */
        float pw = ui_button_width(ICON_EXTERNAL_LINK, "Open in ParaView");
        if (ui_button(ui_id("dm-pv"), (Rectangle){ x + w - pw, y, pw, 32 }, ICON_EXTERNAL_LINK, "Open in ParaView", BTN_SECONDARY)) open_particles(dir);
    }
    y += 44;
    if (D.vPlay) {
        ui_request_frames(2);
        if (GetTime() - D.vPlayAt > 0.1) { D.vPlayAt = GetTime(); if (++D.vIdx >= D.vFiles.n) { D.vIdx = D.vFiles.n - 1; D.vPlay = false; } }
    }
    frame_load_current();
    float vh = fminf(maxH, fmaxf(compact ? 220 : 320, w * 0.5f));
    viewer((Rectangle){ x, y, w, vh });
    y += vh + 12;
    frame_slider((Rectangle){ x + 8, y, w - 16, 22 });
    y += 30;
    char t[300];
    snprintf(t, sizeof t, "Frame %d of %d  ·  %s  ·  %d particles", D.vIdx + 1, D.vFiles.n, D.vIdx >= 0 ? D.vFiles.names[D.vIdx] : "", D.frame.n);
    ui_text_clip(F_REG, 12.5f, t, x, y, w, TH.text3);
    return y + 26 - y0;
}

/* ---------------------------------------------------------------- setup and the coupled pair, for page_prog.c */
float deme_setup_panel(const char *proj, float x, float y, float w)
{
    sync_params(proj);
    if (D.hasParams) return setup_section(x, y, w) - y;
    return ui_text_wrap(F_REG, 13, "This project keeps its settings in the C++ source (no params.ini): use Edit driver on the Run page, "
                        "then run (it rebuilds). Projects made from a starter have a form here instead.", (Rectangle){ x, y, w, 60 }, 19, TH.text3, true) + 10;
}

bool deme_has_params(const char *proj) { sync_params(proj); return D.hasParams; }

float deme_pair_panel(float x, float y, float w, bool running, bool (*startBoth)(const char *foamCase))
{
    float y0 = y;
    y += section_head(x, y, w, "Run with an OpenFOAM case", "Starts this program and the case's run together, for drivers that exchange "
                      "data with OpenFOAM through files. If one ends, the other is stopped 10 s later.");
    const char *names[256]; int nn = 0, map[256];
    for (int i = 0; i < A.nList && nn < 256; i++) if (A.list[i].prog == 0) { map[nn] = i; names[nn++] = A.list[i].name; }
    char pick[256] = "";
    const char *cur = D.pairCase >= 0 && D.pairCase < A.nList ? A.list[D.pairCase].name : "";
    float dw = fminf(420, w - 200);
    if (ui_dropdown(ui_id("dm-pair"), (Rectangle){ x, y, dw, 36 }, cur, names, nn, DD_FILTER, pick, sizeof pick, "OpenFOAM case"))
        for (int k = 0; k < nn; k++) if (!strcmp(names[k], pick)) D.pairCase = map[k];
    float bw = ui_button_width(ICON_WORKFLOW, "Start both");
    bool canPair = D.pairCase >= 0 && D.pairCase < A.nList && !running && !D.pairDeme[0];
    ui_disable(!canPair);
    if (ui_button(ui_id("dm-pair-go"), (Rectangle){ x + dw + 12, y, bw, 36 }, ICON_WORKFLOW, "Start both", BTN_PRIMARY)) {
        char foam[PL_PATH]; pl_strlcpy(foam, A.list[D.pairCase].path, sizeof foam);
        if (startBoth(foam) && A.cs) {
            pl_strlcpy(D.pairDeme, A.cs->s.path, sizeof D.pairDeme);
            pl_strlcpy(D.pairFoam, foam, sizeof D.pairFoam);
            D.pairEndAt = 0;
        }
    }
    ui_disable(false);
    y += 44;
    if (D.pairDeme[0]) { ui_text(F_REG, 12.5f, "A coupled pair is running: they stop together.", x, y, TH.accent); y += 24; }
    return y - y0 + 8;
}
