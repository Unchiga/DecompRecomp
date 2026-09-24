/* UTF-8, for the text a mod writes (glyphs.h). Apart from glyphs.c so the
 * listing compiler can be tested without the renderer. */
#include "glyphs.h"

uint32_t Glyphs_NextCharacter(const char **text)
{
    const unsigned char *at = (const unsigned char *)*text;
    uint32_t character;
    int more, i;
    if (*at < 0x80) { *text += 1; return *at; }
    if ((*at & 0xE0) == 0xC0) { character = *at & 0x1F; more = 1; }
    else if ((*at & 0xF0) == 0xE0) { character = *at & 0x0F; more = 2; }
    else if ((*at & 0xF8) == 0xF0) { character = *at & 0x07; more = 3; }
    else { *text += 1; return GLYPHS_NOT_UTF8; }
    for (i = 1; i <= more; i++) {
        if ((at[i] & 0xC0) != 0x80) { *text += i; return GLYPHS_NOT_UTF8; }
        character = (character << 6) | (at[i] & 0x3F);
    }
    *text += more + 1;
    /* Too long a spelling, a surrogate or past Unicode: no character. */
    if (character < (more == 1 ? 0x80u : more == 2 ? 0x800u : 0x10000u) ||
        (character >= 0xD800 && character <= 0xDFFF) || character > 0x10FFFF)
        return GLYPHS_NOT_UTF8;
    return character;
}
