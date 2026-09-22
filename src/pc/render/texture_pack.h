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
#endif
