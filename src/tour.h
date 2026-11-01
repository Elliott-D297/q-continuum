/* tour.h -- the guided tour (user request, 2026-10-26): shown once on the first start, again from Settings > Help or the
 * palette ("Take the tour"). Each step goes to the page it is about, lights up the control it describes and says in
 * detail how to work it; "Try it" hides the card so the page can be used, and a pill brings it back. */
#ifndef QC_TOUR_H
#define QC_TOUR_H
#include <stdbool.h>

void tour_start(void);
bool tour_active(void);
void tour_draw(void);          /* after the page and the modals, every frame: nothing when the tour is off */

#endif
