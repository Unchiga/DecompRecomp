#ifndef MEMORIES_PC_TEXTURE_PACK_H
#define MEMORIES_PC_TEXTURE_PACK_H

/* A texture pack: a directory of PNGs named by where the images come from
 * on the disc, as tools/pc/extract_images.py writes them, with its
 * manifest.json. When an upload tags VRAM words with their disc offsets
 * (texture_dump.c), the words an image covers get its pixels in the shadow,
 * and a primitive whose palette is the one the image was extracted through
 * samples the shadow instead of VRAM. A pack image may be any size: it is
 * resampled to the texture's own size for the console's resolution, and
 * sampled at its own for an internal resolution above it. Packs add up; a
 * mod names its pack with "textures" in its manifest (src/pc/mods). `rank`
 * is the pack's place in the mods' load order: where two packs read the
 * same words the same way, the higher rank's image is the one drawn.
 * Returns the number of images indexed (0 when every one is switched off,
 * below), -1 when the pack cannot be used, and writes into `problems` (may be
 * NULL) a line on the entries that were left out and why, for the Mods
 * window: a file missing or not a PNG, outside the pack, measures out of
 * range. An entry with a "setting" is one part of the pack the owning mod
 * switches with that setting: `part` says 1 while it is on, 0 while off,
 * -1 when the mod declares no such setting (a problem; the entry is used).
 * `part` may be NULL. */
#include <stddef.h>
int TexturePack_Load(const char *directory, unsigned rank, int (*part)(const char *setting, void *context),
                     void *context, char *problems, size_t problems_size);
void TexturePack_Unload(void);
/* Once a frame, on the main thread: reads what uploads asked for (an
 * upload can come from the interrupt tick, where reading is not safe). */
void TexturePack_Service(void);

/* For a renderer that samples the pack's images itself, at their own
 * resolution (gl_picture.c). The entry (its index + 1) whose image replaces
 * the texel a primitive starts at, as the software pass decides it, when
 * that image is loaded; 0 otherwise. The entry's image, and how it maps
 * onto the texture's texels. The maps from every VRAM word to the entry
 * painted there (index + 1, 0 none) and its place in it (row << 16 | word).
 * The generation changes whenever the entries do, the map generation
 * whenever a word of the maps does. */
#include <stdint.h>
int TexturePack_EntryFor(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v);
/* Readings of the same words (one geometry, several depths or palettes)
 * are entries in a row; the maps name the first, the head, whichever the
 * primitive's palette picks. */
int TexturePack_EntryHead(int entry);
int TexturePack_EntryImage(int entry, const unsigned char **rgba, int *width, int *height, int *crop_left,
                           int *crop_width, int *rows, int *texels_per_word);
unsigned TexturePack_Generation(void);
unsigned TexturePack_MapGeneration(void);
const uint16_t *TexturePack_EntryMap(void);
const uint32_t *TexturePack_PlaceMap(void);
#endif
