/* textdiff.h -- what a save would change (ROADMAP 3.5): a line diff of the text on disk against the text about to be
 * written, with a few lines of context, and a one-line summary per changed entry ("deltaT: 0.005 -> 0.0025"). No
 * graphics: the tests drive it. */
#ifndef TEXTDIFF_H
#define TEXTDIFF_H

#include <stdbool.h>

typedef struct {
    char kind;                   /* ' ' context, '-' removed, '+' added, '@' a skip between hunks */
    int  aLine, bLine;           /* 1-based line numbers in the old and the new text (0: not in it) */
    const char *s; int len;      /* the line, pointing into the text it came from */
} DiffLine;

typedef struct {
    DiffLine *l; int n, cap;
    int added, removed;
} Diff;

/* the diff of a against b with ctx lines of context around each change; false only when out of memory */
bool diff_texts(const char *a, const char *b, int ctx, Diff *out);
void diff_free(Diff *d);

/* "key: old -> new" for each changed entry (a removed line and an added line that start with the same word),
   "key: added" / "key: removed" for the others; returns how many it wrote, at most max */
int  diff_summary(const Diff *d, char (*out)[160], int max);

#endif
