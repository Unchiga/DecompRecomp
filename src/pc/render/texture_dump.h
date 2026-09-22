#ifndef MEMORIES_PC_TEXTURE_DUMP_H
#define MEMORIES_PC_TEXTURE_DUMP_H
#include <stdint.h>

/* Every texture the software GPU draws, as a PNG named by its hash, for
 * texture packs: MEMORIES_DUMP_TEXTURES=<directory> turns it on. A texture
 * is the rectangle of texels one textured primitive covers, decoded through
 * its palette; the hash covers the texel indices (or 16-bit colours) and the
 * palette entries, so the same sprite drawn with another palette is another
 * image, and the same one drawn again is the same file. The directory also
 * gets textures.txt, one line per image: hash, size, depth, page and palette
 * coordinates. A replacement pack names its files by the same hash. */
extern int TextureDump_Enabled;
void TextureDump_Init(void);
/* source: VRAM or a texture bank; u0..u1 and v0..v1 inclusive texel bounds
 * within the 256x256 page. */
void TextureDump_Primitive(const uint16_t *source, int page_x, int page_y, int depth, int clut_x, int clut_y,
                           int u0, int v0, int u1, int v1);
#endif
