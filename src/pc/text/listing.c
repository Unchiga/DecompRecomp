/* A text listing compiled into the bytes the game reads (listing.h).
 * The grammar is tools/pc/text_listing.py's, which writes the listings and
 * checks them against the retail bytes; notes/translation.md describes it. */
#include "listing.h"
#include "glyphs.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const bank_names[TEXT_BANK_COUNT] = {"dialog", "descriptions", "names"};

int TextListing_Bank(unsigned id)
{
    if (id <= 0xFF || (id >= 0x500 && id <= 0x5F9)) return TEXT_BANK_DIALOG;
    if (id >= 0xD000 && id <= 0xD3FF) return TEXT_BANK_DESCRIPTIONS;
    if (id >= 0x8000 && id <= 0x835F) return TEXT_BANK_NAMES;
    return -1;
}

typedef struct {
    int bank;
    unsigned name;       /* LXXXX: XXXX */
    size_t position;
} Label;

typedef struct {
    size_t position;     /* of the u16 operand */
    int bank, line;
    int is_label;
    unsigned value;
} Fixup;

typedef struct {
    unsigned char *data;
    size_t size, room;
    Label *labels;
    int label_count, label_room;
    Fixup *fixups;
    int fixup_count, fixup_room;
    TextString *strings;
    int string_count, string_room;
    TextGlyphEncoder encode;
    TextReport report;
    void *context;
    int line, failed;
} Compiler;

static void say(Compiler *c, const char *format, ...)
{
    char message[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (c->report) c->report(c->context, c->line, message);
}

static void *grow(void *array, int *room, int count, size_t size)
{
    void **slot = (void **)array;
    if (count >= *room) {
        int wanted = *room ? *room * 2 : 64;
        void *bigger = realloc(*slot, (size_t)wanted * size);
        if (!bigger) return NULL;
        *slot = bigger;
        *room = wanted;
    }
    return (char *)*slot + (size_t)count * size;
}

static void emit(Compiler *c, int byte)
{
    if (c->size >= c->room) {
        size_t wanted = c->room ? c->room * 2 : 4096;
        unsigned char *bigger = realloc(c->data, wanted);
        if (!bigger) { c->failed = 1; return; }
        c->data = bigger;
        c->room = wanted;
    }
    c->data[c->size++] = (unsigned char)byte;
}

static int hex(const char *word, unsigned *value)
{
    char *end;
    unsigned long parsed;
    if (!word || !*word) return 0;
    parsed = strtoul(word, &end, 16);
    if (*end || parsed > 0xFFFFFFFFul) return 0;
    *value = (unsigned)parsed;
    return 1;
}

static int hex_byte(Compiler *c, const char *word)
{
    unsigned value;
    if (!hex(word, &value) || value > 0xFF) {
        say(c, "\"%s\" is not a byte in hexadecimal", word ? word : "");
        return -1;
    }
    return (int)value;
}

/* A target operand: a label (LXXXX) or a retail offset in hexadecimal. */
static void target(Compiler *c, int bank, const char *word)
{
    Fixup *fixup;
    unsigned value;
    int is_label = word && word[0] == 'L';
    if (!word || !hex(word + is_label, &value) || value > 0xFFFF) {
        say(c, "\"%s\" is not a place to jump to", word ? word : "");
        emit(c, 0);
        emit(c, 0);
        return;
    }
    fixup = grow(&c->fixups, &c->fixup_room, c->fixup_count, sizeof(*c->fixups));
    if (!fixup) { c->failed = 1; return; }
    fixup->position = c->size;
    fixup->bank = bank;
    fixup->line = c->line;
    fixup->is_label = is_label;
    fixup->value = value;
    c->fixup_count++;
    emit(c, 0);
    emit(c, 0);
}

static void glyph(Compiler *c, int code)
{
    if (code >= 0xF0) {
        emit(c, 0xF0 + (code >> 8));
        emit(c, code & 0xFF);
    } else {
        emit(c, code);
    }
}

/* One {code}: 1 when it ends the item, -1 for a {buffer}, which ends it
 * too and makes it no string of the unit's. */
static int code(Compiler *c, int bank, char *text)
{
    char *words[24];
    int count = 0, i;
    char *at = strtok(text, " ");
    while (at && count < 24) {
        words[count++] = at;
        at = strtok(NULL, " ");
    }
    if (!count) {
        say(c, "an empty {}");
        return 0;
    }
#define IS(name) (!strcmp(words[0], name))
    if (IS("nl")) { emit(c, 0xFE); return 0; }
    if (IS("end")) { emit(c, 0xFF); return 1; }
    if (IS("page")) { emit(c, 0xFA); return 0; }
    if (IS("sp")) { emit(c, 0x00); return 0; }
    if (IS("cont")) return 0;
    if (IS("buffer")) return -1;
    if (IS("g") && count == 2) {
        unsigned value;
        if (!hex(words[1], &value) || value >= GLYPHS_EXTENDED_LIMIT) say(c, "{g %s}: not a glyph code", words[1]);
        else glyph(c, (int)value);
        return 0;
    }
    if (IS("fx") && count == 3) {
        emit(c, 0xF6);
        for (i = 1; i < 3; i++) emit(c, hex_byte(c, words[i]));
        return 0;
    }
    if (IS("state") && count >= 2) {
        emit(c, 0xF7);
        for (i = 1; i < count; i++) emit(c, hex_byte(c, words[i]));
        return 0;
    }
    if ((IS("set") && count == 2) || (IS("if") && count == 3)) {
        unsigned value;
        if (!hex(words[1], &value) || value > 0xFFFF) {
            say(c, "{%s %s}: a flag is four hexadecimal digits", words[0], words[1]);
            value = 0;
        }
        emit(c, 0xF9);
        emit(c, (int)(value & 0xFF));
        emit(c, (int)(value >> 8));
        if (IS("if")) target(c, bank, words[2]);
        return 0;
    }
    if ((IS("choice") || IS("choose")) && count >= 2) {
        int control = hex_byte(c, words[1]), first = 2;
        emit(c, 0xFB);
        emit(c, control);
        if (control >= 0 && (control & 0x08) && count >= 3) {
            emit(c, hex_byte(c, words[2]));
            first = 3;
        }
        for (i = first; i < count; i++) target(c, bank, words[i]);
        return IS("choose");
    }
    if ((IS("call") || IS("jump")) && count == 2) {
        emit(c, IS("call") ? 0xFC : 0xFD);
        target(c, bank, words[1]);
        return IS("jump");
    }
    if (IS("f8") && count >= 2) {
        int index = hex_byte(c, words[1]);
        emit(c, 0xF8);
        emit(c, index);
        if (index == 0x17 || index == 0x18 || index == 0x27 || index == 0x28) {
            for (i = 2; i < count; i++) target(c, bank, words[i]);
            return index != 0x27;
        }
        for (i = 2; i < count; i++) emit(c, hex_byte(c, words[i]));
        return index == 0x2A;
    }
#undef IS
    say(c, "unknown code {%s}", words[0]);
    return 0;
}

/* The text of one line of an item: 1 when it ended the item. */
static int line_text(Compiler *c, int bank, const char *text, size_t length, int *buffer)
{
    const char *at = text, *end = text + length;
    while (at < end) {
        if (*at == '{') {
            char inside[256];
            const char *close = memchr(at, '}', (size_t)(end - at));
            size_t size;
            int result;
            if (!close) {
                say(c, "a { without its }");
                return 0;
            }
            size = (size_t)(close - at - 1);
            if (size >= sizeof(inside)) size = sizeof(inside) - 1;
            memcpy(inside, at + 1, size);
            inside[size] = '\0';
            at = close + 1;
            result = code(c, bank, inside);
            if (result < 0) *buffer = 1;   /* and, like {end}, the item is over */
            if (result != 0) {
                /* Nothing but blanks may follow an item's end. */
                while (at < end && (*at == ' ' || *at == '\t')) at++;
                if (at < end) say(c, "text after the end of a string is left out");
                return 1;
            }
        } else {
            const char *letter = at;
            int glyph_code = c->encode(Glyphs_NextCharacter(&at));
            if (glyph_code < 0) {
                say(c, "no letter for \"%.*s\"; left out", (int)(at - letter), letter);
                continue;
            }
            glyph(c, glyph_code);
        }
    }
    return 0;
}

static void define_label(Compiler *c, int bank, unsigned name)
{
    Label *label = grow(&c->labels, &c->label_room, c->label_count, sizeof(*c->labels));
    if (!label) { c->failed = 1; return; }
    label->bank = bank;
    label->name = name;
    label->position = c->size;
    c->label_count++;
}

/* Labels {:LXXXX} at the start of `text`; returns where they end. */
static const char *labels(Compiler *c, int bank, const char *text, const char *end)
{
    while (end - text >= 8 && !strncmp(text, "{:L", 3)) {
        unsigned name;
        char digits[5];
        memcpy(digits, text + 3, 4);
        digits[4] = '\0';
        if (text[7] != '}' || !hex(digits, &name)) break;
        define_label(c, bank, name);
        text += 8;
        while (text < end && (*text == ' ' || *text == '\t')) text++;
    }
    return text;
}

TextUnit *TextListing_Compile(const char *text, size_t length, const uint32_t bases[TEXT_BANK_COUNT],
                              TextGlyphEncoder encode, TextReport report, void *context)
{
    Compiler c;
    TextUnit *unit;
    const char *at = text, *end = text + length;
    int bank = -1, inside = 0, pending = 0, buffer = 0, first_string = 0, i;
    memset(&c, 0, sizeof(c));
    c.encode = encode;
    c.report = report;
    c.context = context;
    if (length >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) at += 3;   /* a byte-order mark */
    while (at < end && !c.failed) {
        const char *line = at, *stop = memchr(at, '\n', (size_t)(end - at));
        size_t size;
        int starts;
        if (!stop) stop = end;
        at = stop < end ? stop + 1 : end;
        size = (size_t)(stop - line);
        if (size && line[size - 1] == '\r') size--;
        c.line++;
        starts = (size && line[0] == '[') || (size >= 2 && line[0] == '{' && line[1] == ':');
        if (inside && starts) {
            /* The line break before an item is the listing's, not the game's. */
            if (buffer) c.string_count = first_string;
            inside = 0;
        }
        if (!inside) {
            const char *rest = line, *line_end = line + size;
            if (size >= 6 && !strncmp(line, "@bank ", 6)) {
                bank = -1;
                for (i = 0; i < TEXT_BANK_COUNT; i++) {
                    if (size - 6 == strlen(bank_names[i]) && !strncmp(line + 6, bank_names[i], size - 6)) bank = i;
                }
                if (bank < 0) say(&c, "no bank \"%.*s\"", (int)(size - 6), line + 6);
                continue;
            }
            if (!starts) continue;   /* blank lines and comments */
            if (bank < 0) {
                say(&c, "a string before any @bank");
                continue;
            }
            first_string = c.string_count;
            buffer = 0;
            pending = 0;
            if (line[0] == '[') {
                const char *close = memchr(line, ']', size);
                const char *word = line + 1;
                if (!close) {
                    say(&c, "a [ without its ]");
                    continue;
                }
                while (word < close) {
                    char *after;
                    unsigned long id = strtoul(word, &after, 16);
                    TextString *string;
                    if (after == word) break;
                    word = after;
                    while (word < close && *word == ' ') word++;
                    if (TextListing_Bank((unsigned)id) != bank) {
                        say(&c, "string %lX is not in the %s bank", id, bank_names[bank]);
                        continue;
                    }
                    string = grow(&c.strings, &c.string_room, c.string_count, sizeof(*c.strings));
                    if (!string) { c.failed = 1; break; }
                    string->id = (uint16_t)id;
                    string->offset = (uint32_t)c.size;
                    c.string_count++;
                }
                rest = close + 1;
                while (rest < line_end && *rest == ' ') rest++;
                labels(&c, bank, rest, line_end);
                inside = 1;
                continue;   /* the text starts on the next line */
            }
            rest = labels(&c, bank, line, line_end);
            inside = 1;
            /* Text may follow the labels on their line; the line break
             * after it is the game's, as after any line of text. */
            if (rest < line_end) {
                if (line_text(&c, bank, rest, (size_t)(line_end - rest), &buffer)) {
                    inside = 0;
                    continue;
                }
                pending = 1;
            }
            continue;
        }
        if (pending) emit(&c, 0xFE);
        pending = 0;
        if (line_text(&c, bank, line, size, &buffer)) {
            if (buffer) c.string_count = first_string;
            inside = 0;
            continue;
        }
        pending = 1;
    }
    if (inside && buffer) c.string_count = first_string;
    unit = calloc(1, sizeof(*unit));
    if (c.failed || !unit) {
        free(unit);
        free(c.data);
        free(c.labels);
        free(c.fixups);
        free(c.strings);
        return NULL;
    }
    emit(&c, 0xFF);   /* a stream that runs off the last item ends there */
    unit->data = c.data;
    unit->size = c.size;
    unit->strings = c.strings;
    unit->string_count = c.string_count;
    unit->targets = calloc((size_t)(c.fixup_count ? c.fixup_count : 1), sizeof(*unit->targets));
    if (c.fixup_count > 0xFFFF || !unit->targets) {
        say(&c, "too many jumps in one listing");
        free(c.labels);
        free(c.fixups);
        TextListing_Free(unit);
        return NULL;
    }
    for (i = 0; i < c.fixup_count; i++) {
        Fixup *fixup = &c.fixups[i];
        unsigned char *where = NULL;
        int j;
        if (fixup->is_label) {
            for (j = 0; j < c.label_count; j++) {
                if (c.labels[j].bank == fixup->bank && c.labels[j].name == fixup->value) {
                    where = c.data + c.labels[j].position;
                    break;
                }
            }
        }
        /* Undefined, or a plain offset: the retail text at that offset. */
        if (!where) where = (unsigned char *)(uintptr_t)(bases[fixup->bank] + fixup->value);
        unit->targets[i] = where;
        c.data[fixup->position] = (unsigned char)(i & 0xFF);
        c.data[fixup->position + 1] = (unsigned char)(i >> 8);
    }
    unit->target_count = c.fixup_count;
    free(c.labels);
    free(c.fixups);
    return unit;
}

void TextListing_Free(TextUnit *unit)
{
    if (!unit) return;
    free(unit->data);
    free(unit->targets);
    free(unit->strings);
    free(unit);
}
