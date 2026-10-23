/* undo.h -- undo and redo for dictionary edits in the open case (see undo.c) */
#ifndef UNDO_H
#define UNDO_H

#include <stdbool.h>

void        undo_init(void);                 /* installs the edit hook */
void        undo_clear(void);
bool        undo_apply(bool redo);           /* false when there is nothing to do or the file changed since */
const char *undo_label(bool redo);
void        undo_group_begin(const char *what);   /* edits until _end are one step, named what */
void        undo_group_end(void);           /* what the next undo / redo would change, NULL if nothing */

#endif
