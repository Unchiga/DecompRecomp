/* The text listing compiler (src/pc/text/listing.c): line breaks, items,
 * labels and targets, buffers, glyphs past the retail font, and what it
 * says about lines it cannot read. tools/pc/text_listing.py check covers the
 * same grammar against the whole retail text. */
#include "pc/text/listing.h"
#include "pc/text/glyphs.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint32_t bases[TEXT_BANK_COUNT] = {0x801B0000u, 0x801C0000u, 0x801D0000u};
static int reports;

/* Lower-case letters are 1-26, a space 0; an e with an acute accent is an
 * added glyph, 0x123. */
static int encode(uint32_t character)
{
    if (character == ' ') return 0;
    if (character >= 'a' && character <= 'z') return (int)(character - 'a' + 1);
    if (character == 0xE9) return 0x123;
    return -1;
}

static void report(void *context, int line, const char *message)
{
    (void)context;
    fprintf(stderr, "line %d: %s\n", line, message);
    reports++;
}

static TextUnit *compile(const char *text)
{
    return TextListing_Compile(text, strlen(text), bases, encode, report, NULL);
}

static const unsigned char *string(const TextUnit *unit, unsigned id)
{
    int i;
    for (i = 0; i < unit->string_count; i++) {
        if (unit->strings[i].id == id) return unit->data + unit->strings[i].offset;
    }
    return NULL;
}

static unsigned operand(const unsigned char *at)
{
    return at[0] | (unsigned)at[1] << 8;
}

int main(void)
{
    TextUnit *unit;
    const unsigned char *at;

    /* A line break inside a string is the game's; the one before the next
     * item is the listing's. Comments and blank lines between items. */
    unit = compile("# a comment\n@bank dialog\n\n[0500]\nab\nc{page}d\n\n[0501 0502]\nx{end}\n");
    assert(unit && reports == 0);
    at = string(unit, 0x500);
    assert(at && !memcmp(at, "\x01\x02\xFE\x03\xFA\x04\xFE", 7));   /* ...then 0501's own text */
    assert(!memcmp(string(unit, 0x501), "\x18\xFF", 2) && string(unit, 0x502) == string(unit, 0x501));
    TextListing_Free(unit);

    /* A label on a line of its own adds no line break; text on the label's
     * line does, like any line. {cont} and {nl}. */
    unit = compile("@bank dialog\n[0001]\na{cont}\n{:L0010}\nb{end}\n[0002]\n{:L0020}c\nd{nl}\n[0003]\ne{end}\n");
    assert(unit && reports == 0);
    assert(!memcmp(string(unit, 0x001), "\x01\x02\xFF", 3));
    assert(!memcmp(string(unit, 0x002), "\x03\xFE\x04\xFE\x05\xFF", 6));
    TextListing_Free(unit);

    /* Jumps: a label the listing defines is in the unit, one it does not is
     * the retail address; the operand is an index into the targets. */
    unit = compile("@bank dialog\n[0010]\n{jump L0040}\n{:L0040}\na{call L125A}{if 006E L0040}{end}\n"
                   "[0011]\n{choice 02}a\nb\n{choose 80 L0040 0}\n");
    assert(unit && reports == 0 && unit->target_count == 5);
    at = string(unit, 0x010);
    assert(at[0] == 0xFD && unit->targets[operand(at + 1)] == at + 3);
    assert(at[3] == 0x01 && at[4] == 0xFC && unit->targets[operand(at + 5)] == (unsigned char *)(uintptr_t)0x801B125Au);
    assert(at[7] == 0xF9 && at[8] == 0x6E && at[9] == 0x00 && unit->targets[operand(at + 10)] == at + 3);
    at = string(unit, 0x011);
    assert(!memcmp(at, "\xFB\x02\x01\xFE\x02\xFE\xFB\x80", 8));
    assert(unit->targets[operand(at + 8)] == string(unit, 0x010) + 3);
    assert(unit->targets[operand(at + 10)] == (unsigned char *)(uintptr_t)0x801B0000u);
    TextListing_Free(unit);

    /* A buffer is not a string of the unit's; codes with operands; an
     * added glyph is written F1 and its low byte. */
    unit = compile("@bank dialog\n[00FE]\n{buffer}\n\n@bank names\n[8001]\n{\xC3\xA9}\xC3\xA9 {sp}{end}\n"
                   "@bank descriptions\n[D101]\n{state 05 01 01}{f8 03 08 56 1D 80 83}{g 5A}{end}\n");
    assert(unit && reports == 1);   /* {é} is not a code */
    assert(!string(unit, 0x0FE));
    assert(!memcmp(string(unit, 0x8001), "\xF1\x23\x00\x00\xFF", 5));
    assert(!memcmp(string(unit, 0xD101), "\xF7\x05\x01\x01\xF8\x03\x08\x56\x1D\x80\x83\x5A\xFF", 13));
    TextListing_Free(unit);

    /* What cannot be read is said and left out; the rest still compiles. */
    reports = 0;
    unit = compile("[0500]\nno bank\n@bank dialog\n[8001]\nwrong bank{end}\n[0500]\nA{nope}{g 700}\nb{end} c\n");
    assert(unit && reports == 6);
    assert(!memcmp(string(unit, 0x500), "\xFE\x02\xFF", 3));
    TextListing_Free(unit);

    puts("text listing: ok");
    return 0;
}
