#ifndef MEMORIES_PC_DEBUG_HUD_H
#define MEMORIES_PC_DEBUG_HUD_H
#include "pc/platform/menu.h"

void Hud_Draw(MenuCanvas *canvas);
void Hud_Bounds(int *x, int *y, int *w, int *h);
/* Changes whenever what the HUD would draw changes; the full statistics
 * level changes every frame. */
unsigned Hud_Signature(void);

#endif
