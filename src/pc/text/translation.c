/* Translations (text.h, notes/translation.md). */
#include "text.h"
#include "glyphs.h"
#include "listing.h"
#include "pc/cards/cards.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/debug/log.h"
#include "game/card_constants.h"
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

const unsigned char *Text_Resolve(int id, const unsigned char *retail)
{
    const unsigned char *own = overrides && id >= 0 && id <= 0xFFFF ? overrides[id] : NULL;
    return own ? own : retail;
}

unsigned char *Text_Retarget(unsigned char *cursor, unsigned target)
{
    int i;
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
