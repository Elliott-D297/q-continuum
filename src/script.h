/* script.h -- replay a test script: clicks by widget id or visible text, typing, keys, waits, assertions, screenshots.
 *
 *   q-continuum --script journey.qcs [--hidden] [--case <dir>]
 *
 * One command per line; '#' starts a comment. Targets are a widget id ("clone-go", "step#10" for ui_idn(id, 10)),
 * "label:<button label>" or "text:<visible text>". A target that is off screen is scrolled into view with the wheel,
 * as a user would. Conditions (wait-until, assert) are listed in script.c. The exit status is 0 when every
 * assertion held and every target was found. A summary line reports clicks, keystrokes and wall time.
 */
#ifndef SCRIPT_H
#define SCRIPT_H

#include <stdbool.h>

bool script_load(const char *path);        /* false when the file can't be read (message printed) */
bool script_active(void);
void script_tick(int frame);               /* before the frame is drawn: queue input, check conditions */
void script_after_frame(int frame);        /* after it is drawn: screenshots */
bool script_done(void);
int  script_exit_code(void);

#endif
