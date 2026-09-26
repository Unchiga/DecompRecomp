#ifndef MEMORIES_PC_TEXT_H
#define MEMORIES_PC_TEXT_H
#include <stddef.h>
/* Translations: the game's text as mods rewrite it (notes/translation.md).
 *
 * A mod's "text" names listings (tools/pc/text_listing.py writes the
 * retail one) whose strings stand in for the game's, by string id: the
 * dialogue and menus, the card descriptions, and the card, type and
 * duelist names. "font" names font files for letters the game has none of.
 * The game's own code asks Text_Resolve wherever it turns a string id into
 * text, and Text_Retarget wherever a text jumps. */

/* Read and compile every applied mod's text and fonts; once, at startup,
 * before the cards, whose names may use the fonts' letters. */
void Text_Build(void);

/* Once the cards are built: if a translation renames cards, the
 * alphabetical order the Library and Build Deck sort by is the new names'. */
void Text_SortCards(void);

/* The text for string `id`: a mod's, else `retail`. */
const unsigned char *Text_Resolve(int id, const unsigned char *retail);

/* Whether an applied mod's text rewrites string `id`. */
int Text_Overridden(int id);
/* String `id` of a listing the port writes itself (a menu it adds an entry
 * to), compiled as a mod's text is, so that its jumps land (Text_Retarget);
 * it stands in for nothing by itself. NULL if it does not compile. */
const unsigned char *Text_CompileOwn(const char *listing, int id, size_t *size);

/* Where a jump from the stream at `cursor` to `target` lands: into the
 * translation `cursor` is in, by the translation's own targets, or, for the
 * game's own text, `target` in the cursor's 64 KB bank. */
unsigned char *Text_Retarget(unsigned char *cursor, unsigned target);

#endif
