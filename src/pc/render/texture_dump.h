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

/* Provenance: where each VRAM word's bytes came from on the disc, so that a
 * drawn texture can be named by origin (archive, offset, stride, size,
 * palette), the identity a texture pack goes by. The disc layer reports
 * every copy of sector data into game memory; an upload looks its pixels
 * up in those; moves carry the tags, fills and drawing clear them. A
 * primitive whose texels and palette are all tagged adds a line to
 * assets.txt beside the PNGs, which tools/pc/extract_images.py replays. */
extern uint32_t *TextureDump_Tags; /* per VRAM word, disc byte offset + 1; NULL when off */
/* The disc layer, which names the archives (Memories_DiscFileInfo). */
void TextureDump_SetDiscFiles(int (*file_info)(const char *path, int *lba, unsigned *size));
void TextureDump_Delivered(const void *destination, unsigned bytes, int lba, unsigned offset_in_sector);
void TextureDump_Loaded(int x, int y, int w, int h, const uint16_t *pixels);
void TextureDump_Moved(int sx, int sy, int dx, int dy, int w, int h);
void TextureDump_Cleared(int x, int y, int w, int h);
#endif
