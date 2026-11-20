/* working.h -- "show the working" (mesh overhaul plan step 1): a calculation the app makes, written out the way a hand
 * calculation is, Given / Equations / Working / Result, with units. The page builds one each frame from the numbers it
 * used and draws it with teach_working() (collapsed by default); wk_text() gives the same as plain text (Copy, tests,
 * docs). Text may carry [[term]] glossary markup (glossary.h). */
#ifndef WORKING_H
#define WORKING_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { WK_GIVEN = 0, WK_EQ, WK_STEP, WK_RESULT, WK_NOTE } WkKind;

typedef struct {
    unsigned char kind;
    char sym[24];        /* GIVEN / RESULT: the symbol                     */
    char val[40];        /* GIVEN / RESULT: the value, formatted           */
    char unit[16];
    char text[240];      /* GIVEN / RESULT: what it is; EQ / STEP: the line; NOTE: the note */
    char what[160];      /* EQ: what the equation is                       */
} WkLine;

#define WK_MAX 24
typedef struct { char title[96]; WkLine l[WK_MAX]; int n; } Working;

void wk_init(Working *w, const char *title);
void wk_given(Working *w, const char *sym, double v, const char *unit, const char *what);
void wk_given_s(Working *w, const char *sym, const char *val, const char *unit, const char *what);
void wk_eq(Working *w, const char *eq, const char *what);
void wk_step(Working *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void wk_result(Working *w, const char *sym, double v, const char *unit, const char *what);
void wk_result_s(Working *w, const char *sym, const char *val, const char *unit, const char *what);
void wk_note(Working *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* a number to 4 significant figures, without an exponent from 0.001 to 99999, else as 1.23e-05 */
void wk_num(char *b, size_t cap, double v);
/* a length in m as the value and unit people read: 1.5 m, 2.5 mm, 34 µm */
void wk_len(double m, char *val, size_t vcap, char *unit, size_t ucap);
/* the whole working as plain text, markup removed; returns its length */
int  wk_text(const Working *w, char *out, size_t cap);

/* the STL mesher's surface cell: base size (m), target as a % of it, octree meshers rounding it to a whole refinement
   level (snappyHexMesh, cfMesh), and how many cells that puts along the part's length L (m, 0: left out) */
void wk_surface_cell(Working *w, double base, double pct, bool octree, double L);

/* the prism-layer stack the STL mesher makes (geometric series): N layers, each r times the one below, total T (m),
   on surface cells h (m) */
void wk_layer_stack(Working *w, double T, int N, double r, double h);

#endif
