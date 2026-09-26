#ifndef MEMORIES_PC_SAVE_ICON_H
#define MEMORIES_PC_SAVE_ICON_H
/* The game's memory card icon, the one its saves carry: the header template
 * SaveData_BuildPayload copies (gSaveData_aHeaderTemplate, save_data.h).
 * The window and the taskbar show it, read from the game being played, so
 * no picture of the game's is in the port. */
#include <stdint.h>

#define SAVE_ICON_SIZE 16

/* Frame `frame` (0 to 2; the icon blinks) as 16x16 RGBA, 4 bytes a pixel;
 * 0 when the template does not hold an icon (the game not loaded yet). */
int SaveIcon_Rgba(int frame, uint8_t rgba[SAVE_ICON_SIZE * SAVE_ICON_SIZE * 4]);
#endif
