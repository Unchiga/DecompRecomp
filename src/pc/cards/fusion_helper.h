#ifndef MEMORIES_PC_FUSION_HELPER_H
#define MEMORIES_PC_FUSION_HELPER_H
#include "pc/platform/menu.h"
/* View > Fusion helper (SET_FUSION_HELPER): the best fusion in the human
 * player's hand, and what the cards picked so far make. */
void FusionHelper_Viewport(int x, int y, int w, int h);
unsigned FusionHelper_Signature(void);
void FusionHelper_Draw(MenuCanvas *, int *x, int *y, int *w, int *h);
#endif
