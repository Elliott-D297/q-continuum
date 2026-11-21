/* teach.h -- the shared teaching pieces (mesh overhaul plan step 1): glossary terms under the app's wrapped text, the
 * Glossary dialog, and the "show the working" panel. The content is in glossary.c and working.c (core, tested). */
#ifndef TEACH_H
#define TEACH_H

#include <stdbool.h>
#include "working.h"

void  teach_init(void);                          /* hooks [[term]] markup up to the glossary */
void  teach_glossary_open(const char *key);      /* the Glossary dialog, at key (NULL: where it was) */
void  teach_glossary_modal(bool *close);         /* draw_modals, MD_GLOSSARY */
/* "Show the working": a toggle (collapsed at first) and, open, the calculation; returns the height used */
void  teach_working_set(const char *id, bool open);   /* open or close a working panel (it starts closed) */
float teach_working(const char *id, float x, float y, float w, const Working *wk);

#endif
