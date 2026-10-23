/* palette.h -- the COMPUTER bar (Ctrl+K), see palette.c */
#ifndef PALETTE_H
#define PALETTE_H

#include <stdbool.h>

void palette_open(void);
void palette_draw(bool *close);          /* the MD_PALETTE modal (draw_modals ends it) */

#endif
