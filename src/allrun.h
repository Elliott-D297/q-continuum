/* allrun.h -- Allrun / Allclean export and the command-line mode (mesh overhaul plan step 11, gap E5). Every mesh job the app
 * starts (Generate, an import, Mesh > Tools) leaves its steps in <case>/.qc-recipe/mesh.sh, so the mesh can be made again
 * outside the app exactly as it was made inside it; the run's steps come from the Run page (run_steps_script). Plain C. */
#ifndef ALLRUN_H
#define ALLRUN_H

#include <stdbool.h>
#include "pl_util.h"
#include "runner.h"

#define ALLRUN_MARK "Written by Q-Continuum (Run > Export Allrun)"

/* a mesh job's steps kept as the case's mesh recipe (append: a Mesh > Tools job after the mesh) */
void recipe_record(const char *casePath, const Job *j, bool append);
/* the recipe as shell steps into out; a case without one but with system/blockMeshDict: blockMesh. false: nothing makes its mesh */
bool recipe_mesh_steps(const char *casePath, Str *out);
/* one step of a script: echo its label, run it into its log, stop on failure */
void allrun_step(Str *out, const char *label, const char *cmd, const char *log);
/* Allrun (the mesh recipe, then runSteps) and Allclean in the case; a hand-made Allrun is kept once as Allrun.orig. false with err */
bool allrun_write(const char *casePath, const char *runSteps, const char *what, char *err, int cap);

#endif
