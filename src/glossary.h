/* glossary.h -- the CFD and meshing glossary (mesh overhaul plan step 1, gap L1), and the [[term]] markup that puts its
 * entries under any wrapped text in the app.
 *
 * Every entry is written for a reader who has had undergraduate fluid mechanics (Reynolds number, boundary layers,
 * no-slip, the Moody chart) and no CFD: what the term is in fluid-mechanics words, a physical picture, a number to anchor
 * it, and (for the expert) where it lives in OpenFOAM. Keywords were checked against OpenFOAM v2606's annotated
 * dictionaries and sources.
 *
 * Markup: "[[y+]]" or "[[first-layer|first cell]]" inside a string given to ui_text_wrap draws the shown words with a
 * dotted underline; hovering shows the entry, clicking opens the Glossary at it. The key is an entry's key or alias;
 * without "|shown words" the key is shown with its hyphens as spaces ([[prism-layers]] reads "prism layers"). */
#ifndef GLOSSARY_H
#define GLOSSARY_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *key;        /* stable id, used in markup and in tests          */
    const char *name;       /* the heading                                       */
    const char *aliases;    /* other words for it, '|' separated (may be "")    */
    const char *what;       /* what it is, in fluid-mechanics words (the hover) */
    const char *picture;    /* a physical picture                               */
    const char *number;     /* a number or rule of thumb to anchor it           */
    const char *foam;       /* where it is in OpenFOAM (may be "")             */
    const char *see;        /* related keys, '|' separated (may be "")         */
} GlossTerm;

int gloss_count(void);
const GlossTerm *gloss_at(int i);
const GlossTerm *gloss_find(const char *word);            /* key or alias, any case; NULL if unknown */
int gloss_index(const GlossTerm *t);
/* entries matching q (name, key, alias, then the text), best first; returns how many were written */
int gloss_search(const char *q, int *out, int max);
/* the n-th related key of t (from t->see) into out; false past the end */
bool gloss_see(const GlossTerm *t, int n, char *out, size_t cap);

/* ---- markup */
typedef struct { int start, end; char key[48]; } GlossSpan;   /* [start, end) in the plain text */
/* s without its [[ ]] markup into out; the marked spans into spans (up to max); returns the span count.
   An unclosed "[[" is kept as text. */
int gloss_strip(const char *s, char *out, size_t cap, GlossSpan *spans, int max);

#endif
