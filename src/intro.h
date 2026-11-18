/* intro.h -- the start-up animation, one for each theme (intro.c) */
#ifndef INTRO_H
#define INTRO_H
#include <stdbool.h>

void   intro_start(void);             /* play it (from the start)                                        */
void   intro_freeze(double t);        /* tests: hold the animation at t seconds (--intro t)              */
void   intro_update(void);            /* each frame after ui_begin_frame: the clock, a key or click skips */
bool   intro_hides_app(void);         /* the animation covers the whole window this frame                */
bool   intro_active(void);
void   intro_draw(float W, float H);  /* after the app is drawn: the scene, or the veil it fades out under */
double intro_length(int theme);       /* seconds, before the fade into the app                           */
#endif
