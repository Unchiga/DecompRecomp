#ifndef MEMORIES_PC_LISTING_H
#define MEMORIES_PC_LISTING_H
/* A text listing (notes/translation.md, tools/pc/text_listing.py) compiled
 * into the bytes the game's text boxes read.
 *
 * The strings are laid out one after another in one buffer. Their jumps
 * cannot be the retail 16-bit offsets into a 64 KB bank, since the buffer is
 * neither where the bank was nor bound to its size: a jump's operand is an
 * index into the unit's own targets instead, which Text_Retarget follows
 * for any stream inside the buffer (translation.c). A target the listing
 * does not define is the retail address it names, so a partial translation
 * still reaches the game's own text, and the player's name, which the game
 * writes into its bank, is reached where it is. */
#include <stddef.h>
#include <stdint.h>

enum { TEXT_BANK_DIALOG, TEXT_BANK_DESCRIPTIONS, TEXT_BANK_NAMES, TEXT_BANK_COUNT };

typedef struct {
    uint16_t id;
    uint32_t offset;       /* into data */
} TextString;

typedef struct {
    unsigned char *data;
    size_t size;
    unsigned char **targets;   /* by operand */
    int target_count;
    TextString *strings;
    int string_count;
} TextUnit;

/* The glyph code for a character (-1 for none), and what to say about a
 * line of the listing. */
typedef int (*TextGlyphEncoder)(uint32_t character);
typedef void (*TextReport)(void *context, int line, const char *message);

/* `bases` are the retail banks' addresses (the dialogue, descriptions and
 * names banks), for targets the listing leaves undefined. NULL if nothing
 * could be compiled; problems with single lines are reported and skipped. */
TextUnit *TextListing_Compile(const char *text, size_t length, const uint32_t bases[TEXT_BANK_COUNT],
                              TextGlyphEncoder encode, TextReport report, void *context);
void TextListing_Free(TextUnit *unit);

/* The bank a string id belongs in, or -1 for an id no bank has. */
int TextListing_Bank(unsigned id);

#endif
