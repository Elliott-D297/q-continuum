/* salometool.h -- Q-Continuum's SALOME service (assets/salome/salome_tool.py) run in the background: a JSON request in,
 * a JSON answer out (qjson.h). One request at a time; each carries a tag so the page that asked takes its own answer. */
#ifndef SALOMETOOL_H
#define SALOMETOOL_H

#include <stdbool.h>
#include "qjson.h"

/* start a request (a JSON object as text) for a case; false (with a toast) when SALOME is missing or one is running */
bool        stool_run(const char *casePath, const char *tag, const char *request);
bool        stool_busy(const char *tag);        /* this tag's request still running (NULL: any)              */
double      stool_seconds(void);                /* how long the running one has taken                        */
/* the finished answer of this tag, once (the caller frees it with qj_free); NULL while running or when none. A run that
   wrote no answer comes back as {"ok": false, "error": <the log's last line>} */
QJ         *stool_take(const char *tag);
void        stool_tick(void);                   /* every frame: notices the end of a run                     */
/* a JSON string's body for a path or name (quotes and backslashes escaped) */
void        stool_quote(char *out, int cap, const char *s);

#endif
