#ifndef MEMORIES_PC_TEXTURE_DUMP_H
#define MEMORIES_PC_TEXTURE_DUMP_H
#include <stdint.h>
#include <stddef.h>
#include "soft_gpu.h"

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
void TextureDump_Restart(void);
/* source: VRAM or a texture bank; u0..u1 and v0..v1 inclusive texel bounds
 * within the 256x256 page. */
void TextureDump_Primitive(const uint16_t *source, int page_x, int page_y, int depth, int clut_x, int clut_y,
                           int u0, int v0, int u1, int v1);

/* Provenance: where each VRAM word's bytes came from on the disc, so that a
 * drawn texture can be named by origin (archive, offset, stride, size,
 * palette), the identity a texture pack goes by. The disc layer reports
 * every copy of sector data into game memory; an upload looks its pixels
 * up in those, and a word counts only while it still holds the bytes the
 * disc delivered (the game's own code rewrites buffers unannounced); moves
 * carry the tags, fills and drawing clear them. A
 * primitive whose texels and palette are all tagged adds a line to
 * assets.txt beside the PNGs, which tools/pc/extract_images.py replays. */
extern uint32_t *TextureDump_Tags; /* per VRAM word, disc byte offset + 1; NULL when off */
/* Provenance without the dump: a texture pack needs the tags too. */
int TextureDump_EnableTags(void);

/* The shadow a texture pack draws from: one cell per 4-bit texel of VRAM
 * (four per word; an 8-bit texel is two cells, a 16-bit one four), holding
 * the replacement colour as a 15-bit word with bit 15 set, 0 where nothing
 * replaces the texel. 0x8000 alone is a texel painted transparent, so
 * opaque black, which a PS1 word can only be with its semi-transparency
 * bit, is TEXTURE_SHADOW_BLACK: the renderer draws it as 0x8000 where the
 * game's word has that bit and as the darkest red, 0x0001, where it has
 * not (0x0000 would be transparent). The pack (texture_pack.c, which reads the PNGs) fills
 * cells through `paint`, called after an upload has tagged its words; the
 * cells follow the words through moves and clears. `prepare` runs once per
 * textured primitive, before it samples, with one texel it will sample,
 * and says whether this primitive may take from the shadow (1: its palette
 * is the one the shadow's image was painted for) or only from the scaled
 * picture's sampler (2: the words have an image for its palette too, but
 * the shadow holds another reading's colours). Both NULL when no pack is
 * loaded. */
#define TEXTURE_SHADOW_WIDTH (SOFT_GPU_WIDTH * 4)
#define TEXTURE_SHADOW_BLACK 0x8001
extern uint16_t *TextureDump_Shadow;
extern void (*TextureDump_Paint)(int x, int y, int w, int h);
extern int (*TextureDump_Prepare)(int page_x, int page_y, int depth, int clut_x, int clut_y, int u, int v);
int TextureDump_EnableShadow(void);
/* The pack's image at its own resolution, for the scaled picture: u and v
 * are texel coordinates within the page in 16.16, page_x/page_y/depth the
 * primitive's page. Returns 0 when the texel is not replaced, 1 with the
 * colour as 0x00RRGGBB, 2 when it is painted transparent. NULL: no pack. */
extern int (*TextureDump_Sample)(int page_x, int page_y, int depth, int u, int v, uint32_t *rgb);
/* The pack's own record of what it painted where, kept in step with the
 * words: cleared (a fill, a state load, an upload not from the disc) and
 * moved. NULL: no pack. */
extern void (*TextureDump_Forget)(int x, int y, int w, int h);
extern void (*TextureDump_Follow)(int sx, int sy, int dx, int dy, int w, int h);
/* The disc offset of an upload's first byte by its content, for an upload
 * the recent reads cannot trace (texture_pack.c, recall); 0 unknown. NULL:
 * no pack. */
extern uint32_t (*TextureDump_Recall)(const uint16_t *pixels, size_t words);
/* VRAM restored from a state, its tags cleared: the pack finds what it can
 * of its pictures there again. NULL: no pack. */
extern void (*TextureDump_Restored)(void);
static inline uint16_t *TextureDump_Cell(int x, int y, int sub)
{
    return &TextureDump_Shadow[(y & (SOFT_GPU_HEIGHT - 1)) * TEXTURE_SHADOW_WIDTH + (x & (SOFT_GPU_WIDTH - 1)) * 4 + sub];
}
/* The disc layer, which names the archives (Memories_DiscFileInfo). */
void TextureDump_SetDiscFiles(int (*file_info)(const char *path, int *lba, unsigned *size));
/* That lookup, for the pack: 0 and the file's first sector when it is on the disc. */
int TextureDump_DiscFile(const char *path, int *lba, unsigned *size);
void TextureDump_Delivered(const void *destination, unsigned bytes, int lba, unsigned offset_in_sector);
/* Bytes of game memory written by anything but a delivery (Memories_GuestWritten). */
void TextureDump_Written(const void *destination, unsigned bytes);
void TextureDump_Loaded(int x, int y, int w, int h, const uint16_t *pixels);
void TextureDump_Moved(int sx, int sy, int dx, int dy, int w, int h);
void TextureDump_Cleared(int x, int y, int w, int h);
#endif
