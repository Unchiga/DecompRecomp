#ifndef MEMORIES_PC_MODS_WINDOW_H
#define MEMORIES_PC_MODS_WINDOW_H
#include "menu.h"
void ModsWindow_Init(void);
void ModsWindow_Resize(int width, int height);
void ModsWindow_Size(int *width, int *height);
void ModsWindow_Draw(MenuCanvas *canvas);
/* Returns 1 to close this window. All events belong exclusively to it. */
int ModsWindow_Event(const MenuEvent *event);
#endif
