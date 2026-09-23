#ifndef MEMORIES_PC_TEXTURE_PACK_H
#define MEMORIES_PC_TEXTURE_PACK_H

/* A texture pack: a directory of PNGs named by where the images come from
 * on the disc, as tools/pc/extract_images.py writes them, with its
 * manifest.json. When an upload tags VRAM words with their disc offsets
 * (texture_dump.c), the words an image covers get its pixels in the shadow,
 * and a primitive whose palette is the one the image was extracted through
 * samples the shadow instead of VRAM. A pack image may be any size: it is
 * resampled to the texture's own size on load, until the renderer draws at
 * a higher resolution. One pack at a time; a mod names it with "textures"
 * in its manifest (src/pc/mods). Returns the number of images indexed. */
int TexturePack_Load(const char *directory);
void TexturePack_Unload(void);
/* Once a frame, on the main thread: reads what uploads asked for (an
 * upload can come from the interrupt tick, where reading is not safe). */
void TexturePack_Service(void);

/* For a renderer that samples the pack's images itself, at their own
 * resolution (gl_picture.c). The entry (its index + 1) whose image replaces
 * the texel a primitive starts at, as the software pass decides it, when
 * that image is loaded; 0 otherwise. The entry's image, and how it maps
 * onto the texture's texels. The maps from every VRAM word to the entry
 * painted there (index + 1, 0 none) and its place in it (row << 16 | word),
 * with a generation that changes whenever they, or the entries, do. */
#include <stdint.h>
int TexturePack_EntryFor(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v);
int TexturePack_EntryImage(int entry, const unsigned char **rgba, int *width, int *height, int *crop_left,
                           int *crop_width, int *rows, int *texels_per_word);
unsigned TexturePack_Generation(void);
const uint16_t *TexturePack_EntryMap(void);
const uint32_t *TexturePack_PlaceMap(void);
#endif
