#ifndef MEMORIES_PC_GLYPHS_H
#define MEMORIES_PC_GLYPHS_H
/* The letters text can be written in (notes/translation.md).
 *
 * The game's font has 91 glyphs, full-width Latin with no accents, each a
 * code below 0x5C whose entry in the glyph table (0x801D9000) holds its
 * Shift-JIS character. The port adds glyphs past them, from code 0x100 on
 * (written F1-F5 and a low byte, which the game already reads as a glyph):
 * an accented letter is the retail letter with its mark drawn on, and any
 * other character is set in a font (a mod's, else the system's sans-serif)
 * with the retail glyphs' outline and shading. Their pictures are made the
 * first time they are drawn, from the font the game has in VRAM, and kept
 * in a texture bank of the software GPU, so nothing of the console's VRAM
 * is used. */
#include <stdint.h>

#define GLYPHS_EXTENDED_FIRST 0x100
#define GLYPHS_EXTENDED_LIMIT 0x600   /* F5 FF is the last code a text can hold */
/* The texture bank the added glyphs live in (soft_gpu.h; 3D Monsters uses
 * the low ones). */
#define GLYPHS_BANK 15

/* The glyph code for a Unicode character: a retail one, or an added one
 * made now; -1 when the port can make none. */
int Glyphs_Code(uint32_t character);
/* The character a retail glyph code writes, 0 for none. */
uint32_t Glyphs_Character(int code);
/* The glyph table's word for a code (retail or added), as TextBox_BuildStep
 * reads it: Shift-JIS in the low half, the small font's index and the
 * mouth-animation kind above. */
uint32_t Glyphs_Word(int code);
/* The retail glyph an added one stands for where the port draws none (the
 * 8x8 font, the mouth animation); a retail code is itself. */
int Glyphs_Base(int code);

/* What a glyph sorts as: its letter in lower case, an accented letter as
 * its plain one; anything else as its character. */
uint32_t Glyphs_SortCharacter(int code);

/* For func_80035E20: if `sjis` is an added glyph's, where its picture is
 * (texture page with the bank's bits, and u, v in it) for the small
 * (8x12) or large (16x16) font, made from the retail font on `font_page`
 * the first time. Returns 0 for a retail glyph. */
int Glyphs_Cell(uint32_t sjis, int large, int font_page, int *tpage, int *u, int *v);

/* A font file (a mod's) to set characters in before the system's. */
void Glyphs_AddFont(const char *path);

/* UTF-8: the next character of `*text`, advancing it; GLYPHS_NOT_UTF8,
 * past the bad bytes, where the text is not UTF-8 (a file saved as
 * Windows-1252, say), for the caller to say so. */
#define GLYPHS_NOT_UTF8 0xFFFFFFFFu
uint32_t Glyphs_NextCharacter(const char **text);

/* For HD text (hd_text.h): the character a glyph cell holds, 0 for none
 * a font can set: a retail letter, digit or ASCII punctuation at u, v of
 * the font's page (not in_bank), or an added glyph at u, v of the bank's
 * page `page`. `large` is the 16x16 font, else the 8x12. */
uint32_t Glyphs_CellCharacter(int in_bank, int page, int large, int u, int v);
/* Where the retail font has a character's glyph in its page; 0 if not. */
int Glyphs_RetailCell(uint32_t character, int large, int *u, int *v);
/* The font a character is set in (an FT_Face), NULL for none. */
void *Glyphs_Face(uint32_t character);

#endif
