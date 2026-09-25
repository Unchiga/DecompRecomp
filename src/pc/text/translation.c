/* Translations (text.h, notes/translation.md). */
#include "text.h"
#include "glyphs.h"
#include "listing.h"
#include "pc/cards/cards.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/platform/settings.h"
#include "pc/cards/tables.h"
#include "pc/debug/log.h"
#include "game/card_constants.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTES_PER_FILE 20

static const uint32_t bases[TEXT_BANK_COUNT] = {0x801B0000u, 0x801C0000u, 0x801D0000u};

static const unsigned char **overrides;   /* by string id */
static TextUnit **units;
static int unit_count;

typedef struct {
    const char *mod, *file;
    int notes;
} Reporting;

static void report(void *context, int line, const char *message)
{
    Reporting *reporting = context;
    LOG(LOG_MODS, "text: %s: %s:%d: %s", reporting->mod, reporting->file, line, message);
    if (reporting->notes++ < NOTES_PER_FILE) Mods_Note(reporting->mod, "%s, line %d: %s", reporting->file, line, message);
    else if (reporting->notes == NOTES_PER_FILE + 1) Mods_Note(reporting->mod, "%s: more problems left out", reporting->file);
}

static char *read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(file);
    if (text) {
        text[size] = '\0';
        *length = (size_t)size;
    }
    return text;
}

/* A mod's files of one key ("text", "font"): a name, or a list of them. */
static int mod_files(int mod, const char *key, int index, char *path, size_t size, const char **name)
{
    const JsonValue *value = Json_Member(Mods_Manifest(mod), key);
    const JsonValue *entry = Json_TypeOf(value) == JSON_ARRAY ? Json_At(value, index) : index ? NULL : value;
    const char *file = Json_String(entry, NULL);
    if (!entry) return 0;
    *name = file ? file : "";
    if (!file || !*file || !Paths_Contained(file) ||
        snprintf(path, size, "%s/%s", Mods_Directory(mod), file) >= (int)size) {
        Mods_Note(Mods_Id(mod), "\"%s\": %s is not a file in the mod", key, *name);
        path[0] = '\0';
    }
    return 1;
}

static void add_unit(int mod, const char *path, const char *name)
{
    Reporting reporting = {Mods_Id(mod), name, 0};
    size_t length = 0;
    char *text = read_file(path, &length);
    TextUnit *unit, **bigger;
    int i;
    if (!text) {
        Mods_Note(Mods_Id(mod), "\"text\": cannot read %s", name);
        return;
    }
    unit = TextListing_Compile(text, length, bases, units, unit_count, Glyphs_Code, report, &reporting);
    free(text);
    if (!unit) {
        /* The compiler has said why, when it knows (a UTF-16 file). */
        if (!reporting.notes) Mods_Note(Mods_Id(mod), "\"text\": %s could not be read", name);
        return;
    }
    if (unit->not_utf8_lines) {
        /* Last, so it is what the Mods window shows: the likeliest reason
         * the whole translation looks wrong. */
        LOG(LOG_MODS, "text: %s: %s: %d lines are not UTF-8, from line %d", Mods_Id(mod), name, unit->not_utf8_lines,
            unit->first_not_utf8_line);
        Mods_Note(Mods_Id(mod), "%s is not UTF-8 (%d lines, from line %d): save it as UTF-8", name,
                  unit->not_utf8_lines, unit->first_not_utf8_line);
    }
    bigger = realloc(units, (size_t)(unit_count + 1) * sizeof(*units));
    if (!overrides) overrides = calloc(0x10000, sizeof(*overrides));
    if (!bigger || !overrides) {
        TextListing_Free(unit);
        return;
    }
    units = bigger;
    units[unit_count++] = unit;
    /* A later file, or a later mod, has the last word on a string. */
    for (i = 0; i < unit->string_count; i++) overrides[unit->strings[i].id] = unit->data + unit->strings[i].offset;
    LOG(LOG_MODS, "text: %s: %s: %d strings, %lu bytes, %d jumps", Mods_Id(mod), name, unit->string_count,
        (unsigned long)unit->size, unit->target_count);
}

void Text_Build(void)
{
    static int built;
    char path[1200];
    int i, index;
    if (built) return;
    built = 1;
    /* Fonts first: the text may need their letters. */
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const char *name;
        if (!Mods_Active(mod)) continue;
        for (index = 0; mod_files(mod, "font", index, path, sizeof(path), &name); index++) {
            if (path[0]) Glyphs_AddFont(path);
        }
    }
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        const char *name;
        if (!Mods_Active(mod)) continue;
        for (index = 0; mod_files(mod, "text", index, path, sizeof(path), &name); index++) {
            if (path[0]) add_unit(mod, path, name);
        }
    }
}

/* Video > Opponent's name for COM (hd_text.h) names the sides after the
 * duel too: strings 0x3D and 0x3E (YOU, or 1P in a 2P duel) and 0x3F (COM,
 * or 2P) become You and the opponent's short name, as the life-point panel
 * has them. The result screens call them by their place in the dialogue
 * bank (TEXT_*_AT, Text_Retarget) rather than by id. Only against the
 * computer (an opponent id), and only when every letter has a glyph of one
 * byte; else the game's own. */
#define TEXT_YOU_FIRST 0x3D
#define TEXT_COM 0x3F
#define TEXT_YOU_FIRST_AT 0x0504
#define TEXT_YOU_AT 0x050E
#define TEXT_COM_AT 0x051C
/* The result screens' strings, and how far a letter of their font goes. */
#define TEXT_RESULTS_FIRST 0x40
#define TEXT_RESULTS_LAST 0x45
#define TEXT_RESULTS_COPY 1024
#define TEXT_RESULTS_LETTER 7

/* The opponent's name as the result screens' small font can show it in
 * COM's column: the panel's name when it is letters and spaces only and at
 * most TEXT_RESULTS_WIDE of them, else its longest word of letters alone
 * (G. Sebek: Sebek, Teana 2nd: Teana, Simon Muran: Simon); the font has no
 * full stop and other digits. NULL when even that is too long. */
#define TEXT_RESULTS_WIDE 9

static const char *results_name(void)
{
    static char name[TEXT_RESULTS_WIDE + 1];
    const char *whole = Tables_DuelistShortName(Tables_OpponentId()), *c, *best = NULL;
    int plain = 1, best_length = 0;
    if (!whole) return NULL;
    for (c = whole; *c; c++) plain &= isalpha((unsigned char)*c) || *c == ' ';
    if (plain && strlen(whole) <= TEXT_RESULTS_WIDE) return whole;
    for (c = whole; *c;) {
        const char *start = c;
        int letters = 1;
        while (*c && *c != ' ') letters &= isalpha((unsigned char)*c++) != 0;
        if (letters && c - start > best_length) best = start, best_length = (int)(c - start);
        while (*c == ' ') c++;
    }
    if (!best || best_length > TEXT_RESULTS_WIDE) return NULL;
    memcpy(name, best, (size_t)best_length);
    name[best_length] = '\0';
    return name;
}

static int side_letters(void)
{
    const char *name = results_name();
    return name ? (int)strlen(name) : 0;
}

static const unsigned char *side_name(int id)
{
    static unsigned char texts[2][40];
    const char *name;
    unsigned char *out;
    int i, n = 0;
    if (id < TEXT_YOU_FIRST || id > TEXT_COM || !Settings_Get(SET_OPPONENT_NAME)) return NULL;
    if (!Tables_DuelistShortName(Tables_OpponentId())) return NULL;
    name = id == TEXT_COM ? results_name() : "You";
    if (!name) return NULL;
    out = texts[id == TEXT_COM];
    for (i = 0; name[i] && n < (int)sizeof(texts[0]) - 1; i++) {
        int code = Glyphs_Code((unsigned char)name[i]);
        if (code < 0 || code >= 0xF0) return NULL;
        out[n++] = (unsigned char)code;
    }
    out[n] = 0xFF; /* the string's end */
    return out;
}

/* The result screens set COM's column with {f8 02 NN}, a step right from
 * the end of YOU's, then call COM. For a longer name the copy of the
 * string steps that much less, so the name ends where COM did; a name the
 * step cannot make room for stays COM (results_room). */
static unsigned char results[TEXT_RESULTS_LAST - TEXT_RESULTS_FIRST + 1][TEXT_RESULTS_COPY];
static int results_room;

static const unsigned char *results_copy(int id, const unsigned char *retail)
{
    unsigned char *copy = results[id - TEXT_RESULTS_FIRST];
    int shift = (side_letters() - 3) * TEXT_RESULTS_LETTER, i, room = 0x7FFF;
    if (!side_name(TEXT_COM) || shift <= 0 || ((uintptr_t)retail & 0xFFFF0000u) != bases[TEXT_BANK_DIALOG] ||
        ((uintptr_t)retail & 0xFFFF) + TEXT_RESULTS_COPY > 0x10000) {
        results_room = 0x7FFF;
        return NULL;
    }
    memcpy(copy, retail, TEXT_RESULTS_COPY);
    /* {f8 02 NN} with a call to COM in the bytes after it. */
    for (i = 0; i + 7 < TEXT_RESULTS_COPY; i++) {
        int k, calls = 0;
        if (copy[i] != 0xF8 || copy[i + 1] != 0x02) continue;
        for (k = i + 3; k < i + 8; k++) {
            if ((copy[k] == (TEXT_COM_AT & 0xFF) && copy[k + 1] == TEXT_COM_AT >> 8) ||
                (copy[k] == TEXT_COM_AT >> 8 && copy[k + 1] == (TEXT_COM_AT & 0xFF))) {
                calls = 1;
            }
        }
        if (!calls) continue;
        if (copy[i + 2] < room) room = copy[i + 2];
        copy[i + 2] = (unsigned char)(copy[i + 2] > shift ? copy[i + 2] - shift : 0);
    }
    results_room = room;
    return room >= shift ? copy : NULL;
}

const unsigned char *Text_Resolve(int id, const unsigned char *retail)
{
    const unsigned char *own = overrides && id >= 0 && id <= 0xFFFF ? overrides[id] : NULL;
    const unsigned char *side = side_name(id);
    if (side) return side;
    if (!own && id >= TEXT_RESULTS_FIRST && id <= TEXT_RESULTS_LAST) {
        const unsigned char *copy = results_copy(id, retail);
        if (copy) return copy;
    }
    return own ? own : retail;
}

unsigned char *Text_Retarget(unsigned char *cursor, unsigned target)
{
    int i;
    int copied = cursor >= results[0] && cursor < results[0] + sizeof(results);
    if (copied || ((uintptr_t)cursor & 0xFFFF0000u) == bases[TEXT_BANK_DIALOG]) {
        /* The retail result screens calling YOU or COM (COM only when its
         * column made room for the name). */
        const unsigned char *side = side_name(target == TEXT_COM_AT ? TEXT_COM
                                              : target == TEXT_YOU_AT || target == TEXT_YOU_FIRST_AT ? TEXT_YOU_FIRST
                                                                                                    : -1);
        if (side && (target != TEXT_COM_AT || side_letters() <= 3 ||
                     results_room >= (side_letters() - 3) * TEXT_RESULTS_LETTER)) {
            return (unsigned char *)side;
        }
        /* A jump from a copy lands in the bank it was copied from. */
        if (copied) return (unsigned char *)(uintptr_t)(bases[TEXT_BANK_DIALOG] | (target & 0xFFFF));
    }
    for (i = 0; i < unit_count; i++) {
        TextUnit *unit = units[i];
        if (cursor >= unit->data && cursor <= unit->data + unit->size) {
            /* An operand past the unit's targets (a hand-edited byte) ends
             * the stream rather than jumping anywhere. */
            return target < (unsigned)unit->target_count ? unit->targets[target] : unit->data + unit->size - 1;
        }
    }
    return (unsigned char *)(((uintptr_t)cursor & 0xFFFF0000u) | (target & 0xFFFF));
}

/* --- the order of the card names --------------------------------------- */

extern short gCard_asNameSortKey[];
#define NAME_OFFSETS 0x801D5800u
#define NAME_BANK 0x801D0000u
#define SORT_LETTERS 40

typedef struct {
    int id;
    uint32_t key[SORT_LETTERS];
} Sorted;

static int by_name(const void *left, const void *right)
{
    const Sorted *a = left, *b = right;
    int i;
    for (i = 0; i < SORT_LETTERS; i++) {
        if (a->key[i] != b->key[i]) return a->key[i] < b->key[i] ? -1 : 1;
        if (!a->key[i]) break;
    }
    return a->id - b->id;
}

void Text_SortCards(void)
{
    Sorted *cards;
    int id, renamed = 0;
    if (!overrides) return;
    for (id = 1; id <= CARD_COUNT; id++) renamed |= overrides[0x8000 + id] != NULL;
    if (!renamed) return;
    cards = calloc((size_t)gCard_nCount, sizeof(*cards));
    if (!cards) return;
    for (id = 1; id <= gCard_nCount; id++) {
        int base = Cards_BaseId(id), n = 0;
        const unsigned char *name = Cards_NameText(id);
        if (!name) {
            const unsigned char *retail = (const unsigned char *)(uintptr_t)(
                NAME_BANK + ((const uint16_t *)(uintptr_t)NAME_OFFSETS)[base]);
            name = Text_Resolve(0x8000 + base, retail);
        }
        cards[id - 1].id = id;
        while (n < SORT_LETTERS - 1 && *name < 0xF6) {
            int code = *name++;
            if (code >= 0xF0) code = ((code - 0xF0) << 8) | *name++;
            cards[id - 1].key[n++] = Glyphs_SortCharacter(code);
        }
    }
    qsort(cards, (size_t)gCard_nCount, sizeof(*cards), by_name);
    for (id = 0; id < gCard_nCount; id++) gCard_asNameSortKey[cards[id].id - 1] = (short)(id + 1);
    free(cards);
    LOG(LOG_MODS, "text: cards sorted by their translated names");
}
