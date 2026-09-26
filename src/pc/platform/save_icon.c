/* The memory card icon. See save_icon.h. */
#include "save_icon.h"

/* The save header template, 0x200 bytes (save_data.h). A PlayStation save
 * header: "SC", the icon's display flag (0x11 to 0x13: one to three frames),
 * the title, then at 0x60 a 16-colour CLUT and from 0x80 the frames, 16x16
 * at 4 bits a pixel, the low nibble first. */
extern unsigned char gSaveData_aHeaderTemplate[];
#define ICON_FLAG 2
#define ICON_CLUT 0x60
#define ICON_FRAMES 0x80
#define ICON_FRAME_BYTES (SAVE_ICON_SIZE * SAVE_ICON_SIZE / 2)

int SaveIcon_Rgba(int frame, uint8_t rgba[SAVE_ICON_SIZE * SAVE_ICON_SIZE * 4])
{
    const unsigned char *header = gSaveData_aHeaderTemplate;
    const unsigned char *pixels;
    int i;
    if (header[0] != 'S' || header[1] != 'C' || header[ICON_FLAG] < 0x11 || header[ICON_FLAG] > 0x13) return 0;
    if (frame < 0 || frame > header[ICON_FLAG] - 0x11) frame = 0;
    pixels = header + ICON_FRAMES + frame * ICON_FRAME_BYTES;
    for (i = 0; i < SAVE_ICON_SIZE * SAVE_ICON_SIZE; i++) {
        int index = i & 1 ? pixels[i / 2] >> 4 : pixels[i / 2] & 15;
        unsigned colour = header[ICON_CLUT + index * 2] | header[ICON_CLUT + index * 2 + 1] << 8;
        rgba[i * 4 + 0] = (uint8_t)((colour & 31) * 255 / 31);
        rgba[i * 4 + 1] = (uint8_t)((colour >> 5 & 31) * 255 / 31);
        rgba[i * 4 + 2] = (uint8_t)((colour >> 10 & 31) * 255 / 31);
        rgba[i * 4 + 3] = colour ? 255 : 0; /* 0x0000 is the card's transparent colour */
    }
    return 1;
}
