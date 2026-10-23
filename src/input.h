/* input.h -- every mouse and keyboard read in the app goes through here, so a test can drive the app.
 *
 * Interactive: the reads are raylib's. Scripted (tests, screenshots): the real mouse and keyboard are ignored and
 * only queued events count. Events are scheduled by frame: offset 0 is the next frame drawn, 1 the one after, so a
 * click is a move + press on frame 0 and a release on frame 1, as a real click spans two frames.
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include "raylib.h"

void    inp_set_scripted(bool on);       /* ignore the real mouse and keyboard */
bool    inp_scripted(void);
void    inp_begin_frame(void);           /* latch this frame's input; once per frame, before any read */
bool    inp_activity(void);              /* anything happened this frame (keeps the frame rate up) */
bool    inp_pending(void);               /* scripted events still queued */

Vector2 inp_mouse(void);                 /* screen pixels */
Vector2 inp_mouse_delta(void);
bool    inp_button_pressed(void);        /* left button */
bool    inp_button_released(void);
bool    inp_button_down(void);
float   inp_wheel(void);

bool    inp_key_pressed(int key);
bool    inp_key_repeat(int key);         /* pressed or auto-repeated */
bool    inp_key_down(int key);
int     inp_char(void);                  /* next typed character this frame, 0 when none */
int     inp_any_key(void);               /* a key pressed this frame, 0 when none */

/* scripted events, `at` frames from now */
void    inp_queue_move(int at, float x, float y);
void    inp_queue_button(int at, bool down);
void    inp_queue_wheel(int at, float dy);
void    inp_queue_key(int at, int key);          /* pressed (and down) on that frame */
void    inp_queue_hold(int at, int key);         /* held down on that frame (Ctrl, Shift) */
void    inp_queue_char(int at, int c);
void    inp_queue_text(int at, const char *utf8);

/* helpers built on the queue */
void    inp_click(float x, float y);                                /* move + press now, release next frame */
void    inp_drag(float x0, float y0, float x1, float y1);          /* press at 0, 8 frames moving, release at 1 */
void    inp_type_enter(const char *text);                         /* the text now, Enter on the next frame */
int     inp_key_code(const char *name);                           /* "ENTER", "F5", "A", "ESCAPE" -> raylib key, 0 unknown */

#endif
