/* page_salome.c -- the Mesh page's SALOME tab: a Python script in salome/mesh.py builds the geometry and a
 * NETGEN mesh, SALOME runs it without its window, ideasUnvToFoam turns salome/mesh.unv into constant/polyMesh. */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "assets.h"

#define SCRIPT "salome/mesh.py"
#define UNV    "salome/mesh.unv"

/* names containing "wall" become walls; the converter makes every group a plain patch */
#define WALLS "for p in $(foamDictionary constant/polyMesh/boundary -entry entry0 -keywords 2>/dev/null); do\n" \
              "  case \"${p,,}\" in *wall*) foamDictionary constant/polyMesh/boundary -entry \"entry0/$p/type\" -set wall > /dev/null 2>&1 && echo \"patch $p -> wall\";; esac\n" \
              "done\n"

static bool can_mesh(Case *c)
{
    if (!app_require_foam()) return false;
    if (!app_can_start()) return false;
    if (c->s.procTimes > 0) { ui_toast(TOAST_ERR, "processor* holds results for the current mesh. Clean the case first."); return false; }
    return true;
}

static void add_convert(void)
{
    job_add(A.cj, "ideasUnvToFoam", "ideasUnvToFoam", "ideasUnvToFoam " UNV " && echo && " "{\n" WALLS "}", "log.ideasUnvToFoam", false);
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
}

/* run the script headless, then convert and check */
static void run_script(Case *c)
{
    if (!A.env.salome[0]) { ui_toast(TOAST_ERR, "SALOME not found: see below"); return; }
    if (!can_mesh(c)) return;
    app_save_all(true);
    char q[PL_PATH + 8], cmd[PL_PATH + 600];
    pl_shell_quote(q, sizeof q, A.env.salome);
    /* SALOME brings its own Python and libraries: OpenFOAM's must not leak into it */
    snprintf(cmd, sizeof cmd,
             "cd salome && touch .qc_start && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME %s -t --shutdown-servers=1 mesh.py\n"
             "rc=$?; [ mesh.unv -nt .qc_start ]; new=$?; rm -f .qc_start\n"
             "[ $new = 0 ] || { echo \"Q-Continuum: the script did not write salome/mesh.unv (SALOME exit code $rc)\"; exit 1; }", q);
    job_reset(A.cj, "Mesh");
    job_add(A.cj, "salome", "SALOME script", cmd, "log.salome", false);
    add_convert();
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Running " SCRIPT " in SALOME");
    else ui_toast(TOAST_ERR, "Could not start SALOME");
}

static void convert_only(Case *c)
{
    if (!can_mesh(c)) return;
    job_reset(A.cj, "Mesh");
    add_convert();
    if (job_start(A.cj, c->s.path, app_bashrc(), GetTime())) ui_toast(TOAST_INFO, "Converting " UNV);
}

static void open_gui(Case *c)
{
    char dir[PL_PATH], qd[PL_PATH + 8], qs[PL_PATH + 8], cmd[PL_PATH * 2 + 64], lp[PL_PATH], sp[PL_PATH];
    pl_join(dir, sizeof dir, c->s.path, "salome");
    pl_shell_quote(qd, sizeof qd, dir);
    pl_shell_quote(qs, sizeof qs, A.env.salome);
    /* SALOME's GUI runs a script with __file__ unset, so a script that finds its folder through __file__ (ours did)
       stopped on its first line and the window stayed empty: run it through a launcher that sets it */
    pl_join(sp, sizeof sp, c->s.path, SCRIPT);
    pl_join(lp, sizeof lp, dir, ".qc-open.py");
    Str py; str_init(&py);
    str_cat(&py, "__file__ = \"");
    for (const char *q = sp; *q; q++) { if (*q == '\\' || *q == '"') str_catf(&py, "\\%c", *q); else str_catf(&py, "%c", *q); }
    str_cat(&py, "\"\nexec(compile(open(__file__).read(), __file__, \"exec\"))\n");
    bool ok = pl_write_file(lp, py.s, strlen(py.s));
    str_free(&py);
    if (!ok) { ui_toast(TOAST_ERR, "Could not write %s", lp); return; }
    snprintf(cmd, sizeof cmd, "cd %s && %s start -g .qc-open.py > /dev/null 2>&1", qd, qs);
    pl_spawn_detached(cmd);
    ui_toast(TOAST_INFO, "Starting SALOME: its window shows the geometry and mesh the script builds");
}

static void create_script(Case *c)
{
    char dir[PL_PATH], p[PL_PATH];
    pl_join(dir, sizeof dir, c->s.path, "salome");
    pl_mkdirs(dir);
    pl_join(p, sizeof p, c->s.path, SCRIPT);
    if (pl_is_file(p)) return;                                   /* never replace the user's script */
    if (pl_write_file(p, (const char *)asset_mesh_template_py, asset_mesh_template_py_len)) {
        case_rescan(c);
        ui_toast(TOAST_OK, "Wrote " SCRIPT ": a pipe T-junction to start from");
    } else ui_toast(TOAST_ERR, "Could not write " SCRIPT);
}

static void edit_script(void)
{
    snprintf(A.filesSel, sizeof A.filesSel, "%s", SCRIPT);
    A.filesRaw = true;
    app_go(PG_FILES);
}

float page_salome(Rectangle r, float x, float y, float w)
{
    Case *c = A.cs;
    float y0 = y;
    char p[PL_PATH];
    pl_join(p, sizeof p, c->s.path, SCRIPT);
    bool haveScript = pl_is_file(p);
    pl_join(p, sizeof p, c->s.path, UNV);
    bool haveUnv = pl_is_file(p);
    bool running = A.cj->active;

    if (!A.env.salome[0]) {
        const char *msg = "Install the SALOME universal binary (salome-platform.org, Downloads) into ~/salome, so that "
                          "~/salome/SALOME-x.y.z/salome exists, or put a salome_cfd / salome_meca launcher on the PATH. Restart Q-Continuum afterwards.";
        float ch = callout_height(w, "SALOME not found", msg);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, "SALOME not found", msg);
        y += ch + 20;
    } else {
        char t[PL_PATH + 64]; snprintf(t, sizeof t, "SALOME %s  ·  %s", A.env.salomeVersion, A.env.salome);
        ui_text_clip(F_MONO, 12.5f, t, x, y, w, TH.text3);
        y += 30;
    }

    y += section_head(x, y, w, "Mesh script",
                      "SALOME builds the geometry and a tetrahedral mesh from " SCRIPT ". Face groups become OpenFOAM patches with the same names; "
                      "patches with \"wall\" in the name become walls.");
    if (!haveScript) {
        float bw = ui_button_width(ICON_PLUS, "Create from template");
        ui_disable(running);
        if (ui_button(ui_id("sa-new"), (Rectangle){ x, y, bw, 40 }, ICON_PLUS, "Create from template", BTN_PRIMARY)) create_script(c);
        ui_disable(false);
        ui_text_wrap(F_REG, 13, "The template meshes a pipe T-junction with NETGEN. Change its PARAMETERS, GEOMETRY and PATCHES sections for your part.",
                     (Rectangle){ x + bw + 16, y + 2, w - bw - 16, 40 }, 19, TH.text3, true);
        y += 64;
    } else {
        float bx = x, bw;
        ui_disable(running || !A.env.salome[0]);
        bw = ui_button_width(ICON_PLAY, "Run script and convert");
        if (ui_button(ui_id("sa-run"), (Rectangle){ bx, y, bw, 40 }, ICON_PLAY, "Run script and convert", BTN_PRIMARY)) run_script(c);
        bx += bw + 10;
        ui_disable(false);
        bw = ui_button_width(ICON_PENCIL, "Edit script");
        if (ui_button(ui_id("sa-edit"), (Rectangle){ bx, y, bw, 40 }, ICON_PENCIL, "Edit script", BTN_SECONDARY)) edit_script();
        bx += bw + 10;
        ui_disable(!A.env.salome[0]);
        bw = ui_button_width(ICON_EXTERNAL_LINK, "Open in SALOME");
        if (ui_button(ui_id("sa-gui"), (Rectangle){ bx, y, bw, 40 }, ICON_EXTERNAL_LINK, "Open in SALOME", BTN_GHOST)) open_gui(c);
        bx += bw + 10;
        ui_disable(running || !haveUnv);
        bw = ui_button_width(ICON_REFRESH_CW, "Convert mesh.unv only");
        if (ui_button(ui_id("sa-conv"), (Rectangle){ bx, y, bw, 40 }, ICON_REFRESH_CW, "Convert mesh.unv only", BTN_GHOST)) convert_only(c);
        ui_disable(false);
        y += 52;
        ui_text_wrap(F_REG, 13, "Open in SALOME shows the script's result in SALOME's window, for checking groups and sizes. A mesh made by hand there "
                     "converts too: Mesh > Export > UNV file into the case's salome/mesh.unv, then Convert mesh.unv only.",
                     (Rectangle){ x, y, w, 60 }, 19, TH.text3, true);
        y += 50;
    }

    if (running && !strcmp(A.cj->title, "Mesh")) {
        char t[200]; snprintf(t, sizeof t, "Step %d of %d: %s. SALOME takes about a minute to start; the Run page shows the log.",
                              A.cj->cur + 1, A.cj->n, A.cj->steps[A.cj->cur < A.cj->n ? A.cj->cur : 0].label);
        float ch = callout_height(w, NULL, t);
        callout((Rectangle){ x, y, w, ch }, TOAST_INFO, ICON_LOADER_CIRCLE, NULL, t);
        y += ch + 16;
    }

    if (c->s.meshed) {
        y += 10;
        y += page_mesh_patches(x, y, w);
    }
    (void)r;
    return y - y0;
}
