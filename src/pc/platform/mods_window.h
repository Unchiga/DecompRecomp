#ifndef MEMORIES_PC_MODS_WINDOW_H
#define MEMORIES_PC_MODS_WINDOW_H
#include "menu.h"
void ModsWindow_Init(void);
void ModsWindow_Resize(int width, int height);
void ModsWindow_Size(int *width, int *height);
void ModsWindow_Draw(MenuCanvas *canvas);
/* Returns 1 to close this window. All events belong exclusively to it. */
int ModsWindow_Event(const MenuEvent *event);
/* Whether `event` (already handled) changed what the window shows. Pointer
 * motion only matters while dragging a slider; backends skip the redraw
 * otherwise and draw at most once per batch of events. */
int ModsWindow_Redraws(const MenuEvent *event);
/* The window's close button: 1 to close now, 0 when unsaved changes need a
 * confirmation first (the window then asks). */
int ModsWindow_RequestClose(void);
#endif
