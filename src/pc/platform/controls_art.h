#ifndef MEMORIES_CONTROLS_ART_H
#define MEMORIES_CONTROLS_ART_H
#include "menu.h"
/* Alpha-blend the embedded controller at physical canvas coordinates. */
int ControlsArt_Draw(MenuCanvas *canvas, int x, int y, int width);
int ControlsArt_Height(int width);
#endif
