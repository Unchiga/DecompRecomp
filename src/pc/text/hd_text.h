#ifndef MEMORIES_PC_HD_TEXT_H
#define MEMORIES_PC_HD_TEXT_H
/* HD text (Video > HD text): at an internal resolution above the console's,
 * the OpenGL picture (gl_picture.c) draws the text's glyphs from pictures
 * set in a font at that resolution instead of the retail 8x12 and 16x16
 * cells. The pictures are 4-bit indices like the cells, with the cells'
 * dark outline and their shading row by row, so they go through
 * the text's own palettes: colours, fades, flashes and semi-transparency are
 * the game's. func_80035E20 marks the glyph primitives (HD_TEXT_MARK), and
 * only those are drawn from the pictures; a glyph no font can set (an icon)
 * stays as it was. The software picture, and 1x, never change. */
#include <stdint.h>

/* The mark on a glyph's texture-page word: bit 15, which the hardware
 * leaves unused (bits 11-14 are the texture bank, soft_gpu.h). */
#define HD_TEXT_MARK 0x8000

int HdText_Enabled(void);

/* The picture of the glyph cell at u, v of a 4-bit texture page (in VRAM,
 * or in texture bank `bank`), `large` for the 16x16 font else the 8x12, at
 * `factor` pixels per texel: where it is in the atlas, in texels (a cell is
 * 16 x 16 of them; multiply by the factor for pixels). Made the first time,
 * and again when the cell's pixels change. Returns 0 when there is none. */
int HdText_Cell(int bank, int page_x, int page_y, int large, int u, int v, int factor, int *atlas_u, int *atlas_v);

/* The atlas: 8-bit indices, *side x *side pixels (0 before the first
 * cell). The rows from *first to *last have changed since the last call
 * (none when *last < *first); the generation changes when the atlas is
 * made anew (another factor). */
const uint8_t *HdText_Atlas(int *side, int *first, int *last, unsigned *generation);
#endif
