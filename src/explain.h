/* explain.h -- OpenFOAM error messages in plain words, with the case's own numbers and where to change them.
 *
 * The start of the error translator: each entry matches a FOAM FATAL message (as the log parser collected it) and
 * writes what went wrong, why, and what to change, using the case's dictionaries for the numbers.
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include <stdbool.h>

typedef struct {
    char title[120];            /* "No results were written"                                   */
    char text[700];             /* the explanation, with numbers                               */
    char file[160];             /* where to change it, relative to the case ("" = nowhere)     */
    int  line;                  /* 1-based line of the entry in that file (0 = unknown)        */
    char key[64];               /* the entry ("writeInterval")                                 */
    /* a safe fix the app can apply with one click (3.2); FIX_NONE when there is none */
    int  fix;                   /* FIX_PUT: set put[i] in fixFile; FIX_RENAME: rename the entry fixPath to fixValue;
                                   FIX_ADD_BC: add the default condition for patch fixValue to fixFile; FIX_MESH: make the mesh */
    char fixLabel[120];         /* "Set the type to zeroGradient"                              */
    char fixFile[160];
    char fixPath[200], fixValue[200];
    int  nPut;
    char putPath[3][200], putVal[3][200];
} Explanation;

enum { FIX_NONE = 0, FIX_PUT, FIX_RENAME, FIX_ADD_BC, FIX_MESH };

/* true when the message is one the catalogue knows */
bool foam_explain(const char *casePath, const char *logName, const char *fatalMsg, Explanation *out);
/* FIX_PUT and FIX_RENAME on the fix's file (the caller loads it and saves it); false if it didn't apply */
bool explain_apply(void *fdfile, const Explanation *x);
/* what a missing scheme usually is: section "divSchemes", key "div(phi,U)"; NULL when there's no safe default */
const char *scheme_default(const char *section, const char *key, bool steady);
/* the closest of the words to w (edit distance), NULL if none is within half of w's length */
const char *closest_word(const char *w, const char *const *words, int n);

#endif
