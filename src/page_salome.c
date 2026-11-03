/* page_salome.c -- the Mesh page's SALOME tab: a Python script in salome/mesh.py builds the geometry and a
 * NETGEN mesh, SALOME runs it without its window, ideasUnvToFoam turns salome/mesh.unv into constant/polyMesh.
 * Import a mesh (user request, 2026-10-26): a mesh made by hand in SALOME (MED or UNV) or Gmsh (.msh), copied into the
 * case's salome/ folder and converted (a MED through SALOME to UNV first); "Import a mesh" on the Cases page makes a case
 * for one. Its groups become the patches, whose roles are set on Physics > Setup. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include "app.h"
#include "assets.h"

#define SCRIPT "salome/mesh.py"
#define UNV    "salome/mesh.unv"

/* names containing "wall" become walls; the converter makes every group a plain patch */
#define WALLS "for p in $(foamDictionary constant/polyMesh/boundary -entry entry0 -keywords 2>/dev/null); do\n" \
              "  case \"${p,,}\" in *wall*) foamDictionary constant/polyMesh/boundary -entry \"entry0/$p/type\" -set wall > /dev/null 2>&1 && echo \"patch $p -> wall\";; esac\n" \
              "done\n"

/* then: what to do again once the old results are cleaned (the Clean dialog opens when there are some) */
static bool can_mesh(Case *c, void (*then)(void))
{
    if (!app_require_foam()) return false;
    if (!app_can_start()) return false;
    case_rescan(c);
    if (case_has_results(&c->s)) {
        app_clean_then("The results were computed on the mesh that is about to be replaced, so they go first.", then);
        return false;
    }
    return true;
}

static void add_convert(void)
{
    job_add(A.cj, "ideasUnvToFoam", "ideasUnvToFoam", "ideasUnvToFoam " UNV " && echo && " "{\n" WALLS "}", "log.ideasUnvToFoam", false);
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
}

/* run the script headless, then convert and check */
static void run_script(Case *c);
static void run_script_again(void) { if (A.cs) run_script(A.cs); }
static void run_script(Case *c)
{
    if (!A.env.salome[0]) { ui_toast(TOAST_ERR, "SALOME not found: see below"); return; }
    if (!can_mesh(c, run_script_again)) return;
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

static void convert_only(Case *c);
static void convert_again(void) { if (A.cs) convert_only(A.cs); }
static void convert_only(Case *c)
{
    if (!can_mesh(c, convert_again)) return;
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

/* ============================================================================ importing a mesh */
#define IMPORTED "salome/imported.txt"         /* what was imported, from where: written when the import starts */

static struct {
    pid_t picker;                              /* the file dialog, while it is open */
    char  out[PL_PATH];
    bool  newCase;                             /* the pick makes a new case (Cases page) */
    char  path[PL_PATH];                       /* the path typed or picked */
    char  pathCase[PL_PATH];                   /* the case path is for: another case starts empty */
    char  pickCase[PL_PATH];                   /* the case the file dialog was opened for */
} I = { .picker = -1 };

static const char *mesh_kind(const char *p)
{
    const char *dot = strrchr(p, '.');
    if (!dot) return NULL;
    if (!strcasecmp(dot, ".unv")) return "unv";
    if (!strcasecmp(dot, ".med") || !strcasecmp(dot, ".rmed")) return "med";
    if (!strcasecmp(dot, ".msh")) return "msh";
    return NULL;
}

/* SALOME's own Python reads the MED and writes UNV: OpenFOAM has no MED reader. The mesh with the most cells, with its groups */
static const char MED2UNV[] =
    "import os, sys\n"
    "import salome\n"
    "salome.salome_init()\n"
    "from salome.smesh import smeshBuilder\n"
    "smesh = smeshBuilder.New()\n"
    "src, dst = os.environ['QC_IN'], os.environ['QC_OUT']\n"
    "meshes, status = smesh.CreateMeshesFromMED(src)\n"
    "if not meshes:\n"
    "    print('Q-Continuum: no mesh in %s' % src); sys.exit(1)\n"
    "m = max(meshes, key=lambda m: m.NbVolumes())\n"
    "if m.NbVolumes() == 0:\n"
    "    print('Q-Continuum: %s has only a surface mesh: compute a 3-D (volume) mesh in SALOME first' % m.GetName()); sys.exit(1)\n"
    "print('mesh %s: %d volumes, %d faces; groups: %s' % (m.GetName(), m.NbVolumes(), m.NbFaces(), ', '.join(g.GetName() for g in m.GetGroups()) or 'none'))\n"
    "m.ExportUNV(dst)\n"
    "print('wrote %s' % dst)\n";

/* the setup's marker line (simsetup.c MARK_START): these stand-ins are its to replace when it writes the case files */
#define STANDIN "// Written by Q-Continuum's simulation setup: a stand-in until the setup writes this file\n"
static void least_case(const char *dir)
{
    static const char *const F[][2] = {
        { "system/controlDict", STANDIN "FoamFile { version 2.0; format ascii; class dictionary; object controlDict; }\n\napplication     simpleFoam;\n"
          "startFrom       latestTime;\nstartTime       0;\nstopAt          endTime;\nendTime         1000;\ndeltaT          1;\nwriteControl    timeStep;\n"
          "writeInterval   100;\nwriteFormat     binary;\nrunTimeModifiable true;\n" },
        { "system/fvSchemes", STANDIN "FoamFile { version 2.0; format ascii; class dictionary; object fvSchemes; }\n\nddtSchemes { default steadyState; }\n"
          "gradSchemes { default Gauss linear; }\ndivSchemes { default none; }\nlaplacianSchemes { default Gauss linear corrected; }\n"
          "interpolationSchemes { default linear; }\nsnGradSchemes { default corrected; }\n" },
        { "system/fvSolution", STANDIN "FoamFile { version 2.0; format ascii; class dictionary; object fvSolution; }\n\nsolvers {}\nSIMPLE {}\n" } };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        char p[PL_PATH], d[PL_PATH]; pl_join(p, sizeof p, dir, F[i][0]);
        if (pl_exists(p)) continue;
        pl_dirname(d, sizeof d, p); pl_mkdirs(d);
        pl_write_file(p, F[i][1], strlen(F[i][1]));
    }
}

/* copy, convert (a MED through SALOME), give "*wall*" groups the wall type, check */
static bool import_mesh(Case *c, const char *src);
static void import_again(void) { if (A.cs && I.path[0]) import_mesh(A.cs, I.path); }
static bool import_mesh(Case *c, const char *src)
{
    const char *k = mesh_kind(src);
    if (!k) { ui_toast(TOAST_ERR, "Import a .unv or .med file (SALOME: Mesh > Export), or a Gmsh .msh"); return false; }
    if (!pl_is_file(src)) { ui_toast(TOAST_ERR, "%s does not exist", src); return false; }
    if (!strcmp(k, "med") && !A.env.salome[0]) { ui_toast(TOAST_ERR, "A MED file needs SALOME to read it: export UNV from SALOME instead, or install SALOME"); return false; }
    if (src != I.path) pl_strlcpy(I.path, src, sizeof I.path);   /* import_again takes it from there */
    pl_strlcpy(I.pathCase, c->s.path, sizeof I.pathCase);
    if (!can_mesh(c, import_again)) return false;
    least_case(c->s.path);
    char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, c->s.path, "salome"); pl_mkdirs(d);
    Str t; str_init(&t);
    str_catf(&t, "# Q-Continuum: the mesh imported into this case (Mesh > SALOME > Import a mesh)\nsource = %s\nkind = %s\n", src, k);
    pl_join(p, sizeof p, c->s.path, IMPORTED);
    pl_write_file(p, t.s, t.len);
    str_free(&t);
    char qs[PL_PATH + 8], cmd[PL_PATH * 2 + 800];
    pl_shell_quote(qs, sizeof qs, src);
    job_reset(A.cj, "Mesh");
    snprintf(cmd, sizeof cmd, "rm -rf constant/polyMesh 0/polyMesh && cp -f %s salome/imported.%s && echo \"copied $(basename %s) into salome/imported.%s\"", qs, k, qs, k);
    job_add(A.cj, "copy", "Copy the mesh into the case", cmd, "log.meshImport", false);
    if (!strcmp(k, "med")) {
        pl_join(p, sizeof p, d, ".qc-med2unv.py");
        pl_write_file(p, MED2UNV, strlen(MED2UNV));
        char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, A.env.salome);
        snprintf(cmd, sizeof cmd,
                 "cd salome && rm -f imported.unv && env -u LD_LIBRARY_PATH -u PYTHONPATH -u PYTHONHOME QC_IN=imported.med QC_OUT=imported.unv "
                 "%s -t --shutdown-servers=1 .qc-med2unv.py\nrc=$?; [ -f imported.unv ] || { echo \"Q-Continuum: SALOME did not write "
                 "salome/imported.unv (exit code $rc): the lines above say why\"; exit 1; }", q);
        job_add(A.cj, "salome", "SALOME: MED to UNV", cmd, "log.salome", false);
    }
    if (!strcmp(k, "msh")) job_add(A.cj, "gmshToFoam", "gmshToFoam", "gmshToFoam salome/imported.msh && echo && {\n" WALLS "}", "log.gmshToFoam", false);
    else job_add(A.cj, "ideasUnvToFoam", "ideasUnvToFoam", "ideasUnvToFoam salome/imported.unv && echo && {\n" WALLS "}", "log.ideasUnvToFoam", false);
    job_add(A.cj, "checkMesh", "checkMesh", "checkMesh", "log.checkMesh", false);
    if (!job_start(A.cj, c->s.path, app_bashrc(), GetTime())) { ui_toast(TOAST_ERR, "Could not start the import"); return false; }
    ui_toast(TOAST_INFO, "Importing %s: its boundary condition files (0/) are written when it is done", pl_basename(src));
    return true;
}

static void start_picker(bool newCase)
{
    if (I.picker > 0) return;
    snprintf(I.out, sizeof I.out, "/tmp/qc-pickmesh-%d.txt", (int)getpid());
    unlink(I.out);
    char q[PL_PATH + 8]; pl_shell_quote(q, sizeof q, I.out);
    char cmd[PL_PATH + 1400];
    if (A.env.wsl)
        snprintf(cmd, sizeof cmd,
                 "p=$(powershell.exe -NoProfile -STA -Command \"Add-Type -AssemblyName System.Windows.Forms; "
                 "\\$f = New-Object System.Windows.Forms.Form -Property @{TopMost=\\$true}; "
                 "\\$d = New-Object System.Windows.Forms.OpenFileDialog; \\$d.Title = 'Choose a mesh to import'; "
                 "\\$d.Filter = 'Meshes (*.unv;*.med;*.msh)|*.unv;*.UNV;*.med;*.MED;*.msh|All files (*.*)|*.*'; "
                 "if (\\$d.ShowDialog(\\$f) -eq 'OK') { \\$d.FileName }\" 2>/dev/null | tr -d '\\r'); "
                 "[ -n \"$p\" ] && wslpath -u \"$p\" > %s; true", q);
    else
        snprintf(cmd, sizeof cmd,
                 "{ zenity --file-selection --title='Choose a mesh to import' --file-filter='Meshes | *.unv *.UNV *.med *.MED *.msh' 2>/dev/null "
                 "|| kdialog --getopenfilename ~ '*.unv *.med *.msh' 2>/dev/null; } > %s; true", q);
    I.picker = pl_spawn_shell(cmd, "/dev/null");
    I.newCase = newCase;
    pl_strlcpy(I.pickCase, A.cs && !newCase ? A.cs->s.path : "", sizeof I.pickCase);
    if (I.picker <= 0) ui_toast(TOAST_ERR, "Could not open a file dialog: type the mesh's path instead");
}

/* a case for a mesh file, named after it, then the import */
static void new_case_for(const char *src)
{
    if (!mesh_kind(src)) { ui_toast(TOAST_ERR, "Import a .unv or .med file (SALOME: Mesh > Export), or a Gmsh .msh"); return; }
    char ws[PL_PATH], name[128], dir[PL_PATH];
    pl_expand_home(ws, sizeof ws, A.set.workspace);
    pl_strlcpy(name, pl_basename(src), sizeof name);
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    for (char *q = name; *q; q++) if (*q == ' ' || *q == '/' || *q == '\'' || *q == '"') *q = '_';
    pl_join(dir, sizeof dir, ws, name);
    for (int k = 2; pl_exists(dir) && k < 100; k++) { char n2[160]; snprintf(n2, sizeof n2, "%s-%d", name, k); pl_join(dir, sizeof dir, ws, n2); }
    least_case(dir);
    app_rescan_cases(true);
    app_open_case(dir, PG_MESH);
    A.meshTab = 1;
    if (A.cs) import_mesh(A.cs, src);
}

void salome_import_new_case(void) { start_picker(true); }
void salome_choose(void) { start_picker(false); }

void salome_tick(void)
{
    if (I.picker <= 0) return;
    ui_request_frames(2);
    int st;
    if (waitpid(I.picker, &st, WNOHANG) != I.picker) return;
    I.picker = -1;
    char *p = pl_read_file(I.out, NULL);
    unlink(I.out);
    if (!p) return;
    pl_trim_inplace(p);
    if (p[0]) {
        if (I.newCase) new_case_for(p);
        else if (!A.cs || strcmp(A.cs->s.path, I.pickCase))  /* the case changed while the dialog was open */
            ui_toast(TOAST_INFO, "Not imported: %s was chosen for %s, which is no longer open", pl_basename(p), pl_basename(I.pickCase));
        else { pl_strlcpy(I.path, p, sizeof I.path); pl_strlcpy(I.pathCase, A.cs->s.path, sizeof I.pathCase); import_mesh(A.cs, p); }
    }
    free(p);
}

/* the imported mesh in SALOME's window, to look at its groups (and change it, then Export and import again) */
static void open_imported(Case *c, const char *kind)
{
    char dir[PL_PATH], lp[PL_PATH], qd[PL_PATH + 8], qs[PL_PATH + 8], cmd[PL_PATH * 2 + 128];
    pl_join(dir, sizeof dir, c->s.path, "salome");
    pl_join(lp, sizeof lp, dir, ".qc-open-import.py");
    char py[600];
    snprintf(py, sizeof py, "import salome\nsalome.salome_init()\nfrom salome.smesh import smeshBuilder\nsmesh = smeshBuilder.New()\n"
             "smesh.%s('imported.%s')\nif salome.sg.hasDesktop(): salome.sg.updateObjBrowser()\n",
             !strcmp(kind, "med") ? "CreateMeshesFromMED" : "CreateMeshesFromUNV", kind);
    if (!pl_write_file(lp, py, strlen(py))) { ui_toast(TOAST_ERR, "Could not write %s", lp); return; }
    pl_shell_quote(qd, sizeof qd, dir);
    pl_shell_quote(qs, sizeof qs, A.env.salome);
    snprintf(cmd, sizeof cmd, "cd %s && %s start -g .qc-open-import.py > /dev/null 2>&1", qd, qs);
    pl_spawn_detached(cmd);
    ui_toast(TOAST_INFO, "Starting SALOME with the imported mesh");
}

static float import_section(Case *c, float x, float y, float w, bool running)
{
    float y0 = y;
    y += section_head(x, y, w, "Import a mesh", "A mesh made in SALOME (Mesh > Export > MED or UNV file) or Gmsh (.msh). It is copied into the "
                                               "case's salome/ folder and converted; its face groups become the patches, and 0/ gets a boundary "
                                               "condition file per field with each patch's role guessed from its name. Change them on "
                                               "Boundary conditions or Physics > Setup.");
    char imp[PL_PATH], src[PL_PATH] = "", kind[16] = "";
    pl_join(imp, sizeof imp, c->s.path, IMPORTED);
    char *t = pl_read_file(imp, NULL);
    if (t) {
        char *a = strstr(t, "\nsource = "), *b = strstr(t, "\nkind = ");
        if (a) { a += 10; size_t n = strcspn(a, "\n"); snprintf(src, sizeof src, "%.*s", (int)n, a); }
        if (b) { b += 8; size_t n = strcspn(b, "\n"); snprintf(kind, sizeof kind, "%.*s", (int)n, b); }
        free(t);
    }
    if (strcmp(I.pathCase, c->s.path)) {                   /* another case: not the last one's file */
        pl_strlcpy(I.pathCase, c->s.path, sizeof I.pathCase);
        I.path[0] = 0;
    }
    if (!I.path[0] && src[0]) pl_strlcpy(I.path, src, sizeof I.path);
    ui_disable(running);
    float bw = ui_button_width(ICON_FOLDER_OPEN, "Choose mesh...");
    if (ui_button(ui_id("sa-imp-pick"), (Rectangle){ x, y, bw, 38 }, ICON_FOLDER_OPEN, "Choose mesh...", BTN_PRIMARY)) start_picker(false);
    float bw2 = ui_button_width(ICON_REFRESH_CW, src[0] ? "Import again" : "Import");
    char path[PL_PATH]; pl_strlcpy(path, I.path, sizeof path);
    Rectangle in = { x + bw + 12, y, w - bw - bw2 - 24, 38 };
    if (ui_input(ui_id("sa-imp-path"), in, path, sizeof path, "/path/to/mesh.med (.unv, .msh)", INPUT_MONO)) {
        pl_strlcpy(I.path, path, sizeof I.path); pl_trim_inplace(I.path);
        if (I.path[0]) import_mesh(c, I.path);
    } else pl_strlcpy(I.path, path, sizeof I.path);
    if (ui_button(ui_id("sa-imp-go"), (Rectangle){ x + w - bw2, y, bw2, 38 }, ICON_REFRESH_CW, src[0] ? "Import again" : "Import", BTN_SECONDARY) && I.path[0])
        import_mesh(c, I.path);
    ui_disable(false);
    y += 50;
    if (src[0]) {
        char m[PL_PATH + 200];
        pl_fmt_ui(m, sizeof m, "Imported from %s%s", src, pl_is_file(src) ? "" : " (no longer there: the copy in salome/ is used)");
        ui_text_clip(F_REG, 13, m, x, y, w - 200, TH.text3);
        if (strcmp(kind, "msh")) {                          /* SALOME reads MED and UNV, not Gmsh's .msh */
            ui_disable(running || !A.env.salome[0]);
            float ow = ui_button_width(ICON_EXTERNAL_LINK, "Open in SALOME");
            if (ui_button(ui_id("sa-imp-gui"), (Rectangle){ x + w - ow, y - 6, ow, 32 }, ICON_EXTERNAL_LINK, "Open in SALOME", BTN_GHOST)) open_imported(c, kind);
            ui_disable(false);
        }
        y += 34;
    }
    if (I.path[0] && mesh_kind(I.path) && !strcmp(mesh_kind(I.path), "med") && !A.env.salome[0]) {
        const char *d = "OpenFOAM can't read MED: SALOME converts it. Export UNV from SALOME instead (Mesh > Export > UNV file), or install SALOME.";
        float ch = callout_height(w, NULL, d);
        callout((Rectangle){ x, y, w, ch }, TOAST_WARN, ICON_TRIANGLE_ALERT, NULL, d);
        y += ch + 12;
    }
    return y - y0 + 10;
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

    y += import_section(c, x, y, w, running);
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
