#ifndef MEMORIES_PC_OVERLAY_TEXT_H
#define MEMORIES_PC_OVERLAY_TEXT_H
#include "pc/platform/menu.h"
/* UTF-8 text rasterized at the requested window pixel size, using the same
 * font fallbacks (including mod fonts) as game text. Clips at right. */
int OverlayText_Width(const char *text, int pixels);
/* Paints colour at alpha (0-255) over one canvas pixel, for an opaque canvas
 * or a transparent overlay alike. */
void OverlayText_Blend(const MenuCanvas *, int x, int y, uint32_t colour, unsigned alpha);
void OverlayText_Draw(MenuCanvas *, int x, int middle, int right, const char *text, int pixels, uint32_t colour);
#endif
