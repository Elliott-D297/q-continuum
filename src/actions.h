/* actions.h -- the action registry (see actions.c) */
#ifndef ACTIONS_H
#define ACTIONS_H

#include <stdbool.h>

typedef struct {
    const char *id;               /* "run.start": scripts say "action run.start"                     */
    const char *label;            /* NULL: a second shortcut of the action before it                  */
    const char *shortcut;         /* as shown, "Ctrl+S"                                               */
    int key, mods;                /* raylib key, M_CTRL | M_SHIFT | M_ALT                             */
    const char *widget;           /* the button that does the same (tests), or NULL                   */
    const char *shell;            /* the terminal equivalent, where there is one                      */
    const char *help;
    const char *(*blocked)(void); /* why it can't run now, NULL when it can (NULL: always can)        */
    void (*run)(void);
    bool whileTyping;             /* the shortcut works with a text field focused (Ctrl+S, F1, F5)     */
} Action;

int           actions_count(void);
const Action *actions_get(int i);
const Action *actions_find(const char *id);
const char   *action_blocked(const Action *a);
bool          action_run(const Action *a, bool quiet);   /* false (and a notice unless quiet) when blocked */
void          actions_keys(void);                        /* dispatch this frame's shortcut */
void          actions_sheet(bool *close);                /* the F1 modal (draw_modals ends it) */
void          app_fullscreen(bool on);                   /* F11 (view.fullscreen); A.set.fullscreen follows */

#endif
