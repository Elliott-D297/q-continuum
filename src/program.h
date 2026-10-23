/* program.h -- the simulation programs besides OpenFOAM, so each gets the same workflow (find it, set it up, run it
 * with live progress, see its results): DEME, Chrono, CalculiX, code_aster, code_saturne, MOOSE, Elmer, SPARTA.
 * Free of graphics so the tests drive it. A project is a folder; its inputs are the files (or programs) it can run. */
#ifndef PROGRAM_H
#define PROGRAM_H

#include <stdbool.h>
#include "pl_util.h"

typedef enum {
    PROG_OPENFOAM = 0, PROG_DEME, PROG_CHRONO, PROG_CALCULIX, PROG_ASTER, PROG_SATURNE, PROG_MOOSE, PROG_ELMER, PROG_SPARTA,
    PROG_COUNT
} Program;
#define PROG_NONE (-1)
/* a preCICE coupled run: a folder with precice-config.xml and a folder per participant (coupling.h). Not one of the
   programs above (no inputs of its own to run): its participants are, and it opens with its own pages. */
#define PROG_COUPLED 64

typedef struct {
    const char *name;          /* "CalculiX"                                              */
    const char *key;           /* "calculix": logs (log.calculix.<input>), settings, filter */
    const char *what;          /* one line: what it simulates                             */
    const char *inputWord;     /* "input file", "program", "study"                       */
    const char *exe;           /* the command it runs ("" = the project's own programs)   */
    bool  build;               /* a CMake project: built before it runs                   */
    bool  mpi;                 /* runs on N processes through MPI                         */
    bool  threads;             /* ... or on N threads (OpenMP)                            */
    bool  timeAxis;            /* progress is simulated time (else steps)                 */
} ProgInfo;

const ProgInfo *prog_info(int prog);
int  prog_by_key(const char *key);                    /* PROG_* or PROG_NONE */

/* the program a folder is a project of, PROG_NONE if none; OpenFOAM included (system/controlDict) */
int  prog_detect(const char *dir);
bool prog_is_project(const char *dir);               /* any program, OpenFOAM included */

/* what the project can run, sorted: input files relative to it (CalculiX A.inp, MOOSE A.i, SPARTA in.flow ...),
   the targets of a CMake project, or "DATA/setup.xml" for code_saturne. Returns how many. */
int  prog_inputs(int prog, const char *dir, NameList *out);
/* the text files worth showing and editing (inputs, included files, params.ini, the driver source, run scripts) */
int  prog_files(int prog, const char *dir, NameList *out);

/* the program to run (absolute path), "" with a reason when it isn't installed */
bool prog_exe(int prog, char *out, int cap, char *why, int whyCap);
bool prog_sparta_has(const char *kind, const char *style);   /* in this SPARTA's spa -h style list (true if unreadable) */
bool prog_sparta_missing(char *out, int cap);                /* optional SPARTA packages not built in: "FFT, VTK"; false while still being read */

/* end of the run from the input text: simulated end time (*isSteps false) or step count (*isSteps true); 0 unknown */
double prog_end(int prog, const char *dir, const char *input, bool *isSteps);

/* a CMake project's program: built and newer than its sources and params.ini needs no rebuild */
bool prog_built(int prog, const char *dir, const char *target, bool *stale);
/* the project's run folder for a target (DEME and Chrono run in <project>/run; the others in the project) */
void prog_run_dir(int prog, const char *dir, char *out, int cap);

/* the steps of a run of input on procs processes: shell commands run in dir, each with its log in dir */
typedef struct { char id[32]; char label[64]; char cmd[8192]; char log[96]; bool solver; } ProgStep;
int  prog_run_steps(int prog, const char *dir, const char *input, int procs, bool rebuild, ProgStep *st, int max,
                    char *err, int errCap);
void prog_log_name(int prog, const char *input, char *out, int cap);    /* log.<key>.<input stem> */

/* problems before a run (the program's SENSOR SWEEP): missing program, files the input refers to, absolute paths
   of another folder (a copied code_aster .export), not built ... with a one-line fix where there is a safe one */
enum { PFIX_NONE = 0, PFIX_EXPORT_PATHS, PFIX_WRITE_EXPORT, PFIX_BUILD };
typedef struct { int sev; char text[300]; char file[128]; int line; int fix; char fixLabel[48]; } ProgCheck;   /* sev 2 bad, 1 warn */
int  prog_checks(int prog, const char *dir, const char *input, ProgCheck *out, int max);
bool prog_apply_fix(int prog, const char *dir, const char *input, int fix, char *msg, int cap);

/* result files, newest first */
enum { RK_PARAVIEW = 0, RK_PARTICLES, RK_TABLE, RK_TEXT, RK_IMAGE, RK_CGX, RK_FOLDER, RK_MED };
typedef struct { char rel[256]; int kind; long long size; time_t mtime; int count; } ProgResult;   /* count: frames in a folder */
int  prog_results(int prog, const char *dir, ProgResult *out, int max);
const char *prog_result_kind_name(int kind);

/* one log line of a non-OpenFOAM program (runner.c calls it): true when consumed */
#include "runner.h"
bool prog_parse_line(LogMon *m, const char *line);
/* how far the run in m is: done of total, in steps or simulated time (*steps); false when the end isn't known.
   inputEnd / inputSteps: prog_end of the input, for programs whose log doesn't say */
bool prog_progress(int prog, const LogMon *m, double inputEnd, bool inputSteps, double *done, double *total, bool *steps);

/* a starter project for a program (New case): files written into dir; false with err when it can't */
typedef struct { int prog; const char *id, *name, *note; } ProgTemplate;
extern const ProgTemplate PROG_TEMPLATES[];
extern const int PROG_N_TEMPLATES;
bool prog_template_write(const ProgTemplate *t, const char *dir, char *err, int cap);

/* example folders shipped with the programs (SPARTA examples, MOOSE tutorials, preCICE tutorials ...) */
typedef struct { int prog; char path[PL_PATH]; char name[96]; } ProgExample;
bool prog_example_copy(const char *src, const char *dst, char *err, int cap);   /* an example into a project that runs on its own */
int  prog_examples(ProgExample *out, int max);

#endif
