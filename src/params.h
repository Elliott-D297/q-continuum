/* params.h -- the Setup page of the programs besides OpenFOAM: the values in an input file, found with their exact
 * place in the text, so one can be changed and written back with every other byte kept (comments, layout, order).
 * MOOSE [blocks] key = value, Elmer sections, CalculiX cards and data lines, code_aster keyword arguments and .export
 * lines, code_saturne XML leaves, SPARTA commands, and key = value .ini files. No graphics: the tests drive it. */
#ifndef PARAMS_H
#define PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include "pl_util.h"

typedef struct {
    char group[128];             /* "Executioner", "Material 1", "*MATERIAL STEEL > *ELASTIC", "res = THER_LINEAIRE" */
    char key[96];                /* "end_time", "Heat Conductivity", "E", "INCREMENT > PAS", "timestep"              */
    char value[256];             /* the text as it is in the file (a cut-off long value is shown, never edited)     */
    int  start, len;             /* the value's bytes in the text                                                 */
    int  line;                   /* 1-based                                                                       */
    bool numeric;
    bool editable;               /* false: too long to edit in a field (whole value in value[]?)                   */
    int  importance;             /* 2: shown in Basic (time, step, materials, loads); 1 the rest                  */
    const char *unit, *hint;     /* from the program's table of well-known keys, NULL when unknown                 */
} Param;

typedef struct {
    char   path[PL_PATH];
    int    prog;                 /* PROG_*; -1 = plain key = value (.ini)                                           */
    char  *text; int len;
    Param *p; int n, cap;
    long long stamp;             /* mtime (ns) + size when loaded: an outside change is noticed                     */
    bool   dirty;
    char  *orig;                 /* the text as loaded or last saved (the review before writing diffs against it)    */
} ParamFile;

bool params_load(ParamFile *f, int prog, const char *path);       /* false: unreadable (f is left empty) */
bool params_from_text(ParamFile *f, int prog, const char *path, const char *text);
void params_free(ParamFile *f);
void params_rescan(ParamFile *f);
bool params_set(ParamFile *f, int i, const char *value);         /* replaces the span; false when not editable */
bool params_save(ParamFile *f);
bool params_changed_on_disk(const ParamFile *f);
long long params_stamp(const char *path);
int  params_find(const ParamFile *f, const char *group, const char *key);   /* group NULL = any; -1 none */

#endif
