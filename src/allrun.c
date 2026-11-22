/* allrun.c -- Allrun / Allclean export (see allrun.h) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "allrun.h"

void allrun_step(Str *out, const char *label, const char *cmd, const char *log)
{
    str_catf(out, "\necho \"== $(date +%%H:%%M:%%S) %s\"\n", label);
    if (log && log[0]) str_catf(out, "( %s\n) > %s 2>&1 || { echo \"%s failed: see %s\"; exit 1; }\n", cmd, log, label, log);
    else str_catf(out, "( %s\n) || exit 1\n", cmd);
}

void recipe_record(const char *casePath, const Job *j, bool append)
{
    char d[PL_PATH], p[PL_PATH]; pl_join(d, sizeof d, casePath, ".qc-recipe"); pl_mkdirs(d); pl_join(p, sizeof p, d, "mesh.sh");
    char *old = append ? pl_read_file(p, NULL) : NULL;
    Str o; str_init(&o);
    if (old) str_cat(&o, old);
    else str_cat(&o, "# Q-Continuum: how this case's mesh was last made (each Generate, import or Mesh > Tools job), for Allrun\n");
    time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&now));
    str_catf(&o, "\n# %s, %s\n", j->title, ts);
    for (int i = 0; i < j->n; i++) allrun_step(&o, j->steps[i].label, j->steps[i].cmd, j->steps[i].log);
    pl_write_file(p, o.s, o.len);
    str_free(&o); free(old);
}

bool recipe_mesh_steps(const char *casePath, Str *out)
{
    char p[PL_PATH]; pl_join(p, sizeof p, casePath, ".qc-recipe/mesh.sh");
    char *t = pl_read_file(p, NULL);
    if (t) { str_cat(out, t); free(t); return true; }
    pl_join(p, sizeof p, casePath, "system/blockMeshDict");
    if (pl_is_file(p)) { str_cat(out, "# the mesh: blockMesh (no other way of making it was recorded)\n"); allrun_step(out, "blockMesh", "blockMesh", "log.blockMesh"); return true; }
    return false;
}

bool allrun_write(const char *casePath, const char *runSteps, const char *what, char *err, int cap)
{
    Str mesh; str_init(&mesh);
    bool haveMesh = recipe_mesh_steps(casePath, &mesh);
    time_t now = time(NULL); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&now));
    Str a; str_init(&a);
    str_catf(&a, "#!/bin/bash\n# %s, %s: %s\n", ALLRUN_MARK, ts, what);
    str_cat(&a, "# The mesh as the app last made it, then the run, each step into its own log; a failing step stops it.\n"
                "# Run it in a shell with OpenFOAM loaded (. /usr/lib/openfoam/openfoam2606/etc/bashrc); Allclean puts the case back.\n"
                "cd \"${0%/*}\" || exit 1\n"
                "command -v foamListTimes > /dev/null 2>&1 || { echo \"Load OpenFOAM first: . <OpenFOAM>/etc/bashrc\"; exit 1; }\n");
    if (haveMesh) { str_cat(&a, "\n# ---------------------------------------------------------------- the mesh\n"); str_cat(&a, mesh.s ? mesh.s : ""); }
    else str_cat(&a, "\n# the mesh: brought with the case (constant/polyMesh); nothing makes it again\n");
    str_cat(&a, "\n# ---------------------------------------------------------------- the run\n");
    str_cat(&a, runSteps ? runSteps : "");
    str_cat(&a, "\necho \"== done\"\n");
    Str c; str_init(&c);
    str_catf(&c, "#!/bin/bash\n# %s, %s: the results, processor folders and logs go%s\n", ALLRUN_MARK, ts, haveMesh ? "; the mesh too (Allrun makes it again)" : "");
    str_cat(&c, "cd \"${0%/*}\" || exit 1\n"
                "for d in [0-9]* ; do case \"$d\" in 0) ;; *[!0-9.e+-]*) ;; *) rm -rf \"$d\";; esac; done\n"
                "rm -rf processor* postProcessing log.* 0/polyMesh .qc-prepoly\n");
    if (haveMesh) str_cat(&c, "rm -rf constant/polyMesh\n");
    char z[PL_PATH]; pl_join(z, sizeof z, casePath, "0.orig");
    if (pl_is_dir(z)) str_cat(&c, "[ -d 0.orig ] && { rm -rf 0 && cp -r 0.orig 0; }\n");
    str_cat(&c, "echo \"== cleaned\"\n");
    bool ok = true;
    const char *names[2] = { "Allrun", "Allclean" }; Str *body[2] = { &a, &c };
    for (int k = 0; k < 2 && ok; k++) {
        char p[PL_PATH]; pl_join(p, sizeof p, casePath, names[k]);
        char *old = pl_read_file(p, NULL);
        if (old && !strstr(old, ALLRUN_MARK)) {            /* the case's own: kept once */
            char bak[PL_PATH + 8]; snprintf(bak, sizeof bak, "%s.orig", p);
            if (!pl_exists(bak)) pl_write_file(bak, old, strlen(old));
        }
        free(old);
        ok = pl_write_file(p, body[k]->s, body[k]->len) && chmod(p, 0755) == 0;
        if (!ok) snprintf(err, (size_t)cap, "Could not write %s", names[k]);
    }
    str_free(&a); str_free(&c); str_free(&mesh);
    return ok;
}
