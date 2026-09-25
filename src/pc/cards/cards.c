/* More cards than the disc has: the registry (cards.h, notes/more-cards.md).
 *
 * Cards_Build fills the card tables the game reads (src/pc/game/
 * card_storage.c) from the retail executable, then appends the cards every
 * applied mod's "cards" asks for. The rest of this file is what the game's
 * own code calls where a card past the disc's 722 needs something the disc
 * cannot give it: the retail card to borrow from, a trunk and seen marks
 * that fit nowhere in the save, a name, and a share of the drops. */
#define _POSIX_C_SOURCE 200809L
#include "cards.h"
#include "art.h"
#include "tables.h"
#include "pc/text/glyphs.h"
#include "pc/text/text.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/platform/paths.h"
#include "pc/debug/log.h"
#include "pc/render/texture_dump.h"
#include "pc/rng.h"
#include "pc/compat/posix.h"
#include "game/card_constants.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the retail tables sit in the executable's image (notes/card-catalog.md). */
#define RETAIL_STATS 0x801D4244u        /* s32 [722], by id - 1 */
#define RETAIL_SORT_KEYS 0x801D4D8Eu    /* s16 [722], by id - 1 */
#define RETAIL_LEVEL_ATTR 0x801D5332u   /* u8 [723], by id */
#define RETAIL_NAME_OFFSETS 0x801D5800u /* u16, by 0x8000 + id - 0x8000, from 0x801D0000 */
#define TEXT_BANK 0x801D0000u
#define GLYPH_TABLE 0x801D9000u         /* u32 per glyph code, the Shift-JIS code in the low half */

/* The save's layout (src/game/save_data.h). */
#define SAVE_CHEST 0x50
#define SAVE_DUELIST_CODE 0x334
#define SAVE_SEQUENCE 0x404
#define SAVE_PAIR_BASE 0x801D1200u      /* two-player loads, 0x1000 apart */
#define SAVE_PAIR_STRIDE 0x1000u
#define SAVE_PAIR_COPY 0x680u          /* the copy a trade writes (SAVE_DATA_STATE_SIZE on) */

#define LIBRARY_SEEN_FLAG_BASE 0x120    /* CAMPAIGN_FLAG_LIBRARY_CARD_BASE */
#define KEPT_SAVES 8                    /* sections kept per duelist code */

extern unsigned short gDuel_awPlayerDeck[];   /* the running save's SaveDataState */
extern int gDuel_adwCardStats[];
extern short gCard_asNameSortKey[];
extern unsigned char gDuel_abCardLevelAttr[];
extern int Campaign_TestStoryFlag(int flag);
extern void Library_UpdateCardUsedFlag(int flag);

static char *identities[CARD_TABLE_ID_END];
static const JsonValue *definitions[CARD_TABLE_ID_END];
static unsigned short model_ids[CARD_TABLE_ID_END], effect_ids[CARD_TABLE_ID_END];
const char *Cards_Identity(int id) { return id > CARD_COUNT && Cards_Valid(id) && identities[id] ? identities[id] : ""; }
int Cards_FindIdentity(const char *identity)
{
    int id;
    if (!identity || !*identity) return 0;
    for (id = CARD_ID_END; id <= gCard_nCount; id++) if (identities[id] && !strcmp(identities[id], identity)) return id;
    return 0;
}
int Cards_ModelId(int id) { return Cards_Valid(id) && model_ids[id] ? model_ids[id] : Cards_BaseId(id); }
int Cards_EffectId(int id) { return Cards_Valid(id) && effect_ids[id] ? effect_ids[id] : Cards_BaseId(id); }
static int card_reference(const JsonValue *value)
{
    return Cards_Reference(value);   /* -1 matches no card, and makes none */
}
int Cards_Fusion(int a, int b, int *result)
{
    int side;
    if (!Cards_Valid(a) || !Cards_Valid(b)) return 0;
    for (side = 0; side < 2; side++) {
        const JsonValue *fusions = Json_Member(definitions[side ? b : a], "fusions");
        int i;
        for (i = 0; i < Json_Count(fusions); i++) {
            const JsonValue *rule = Json_At(fusions, i);
            int with = card_reference(Json_Member(rule, "with"));
            if (with == (side ? a : b)) {
                int output = card_reference(Json_Member(rule, "result"));
                if (output && !Cards_Valid(output)) return 0;
                *result = output; return 1;
            }
        }
    }
    return 0;
}

static unsigned char *names[CARD_TABLE_ID_END];     /* own names, glyph codes */
static unsigned char *descriptions[CARD_TABLE_ID_END];  /* own card text, glyph codes */
/* Own artwork: an art record (art.h) shared by an entry's cards, which of its
 * parts are the card's own, and the card's own title plate. */
#define ART_PICTURE 1
#define ART_THUMBNAIL 2
static unsigned char *art_records[CARD_TABLE_ID_END];
static unsigned char art_parts[CARD_TABLE_ID_END];
static unsigned char *plates[CARD_TABLE_ID_END];
static unsigned short *variants[2];                 /* per use: copies, grouped by base */
static unsigned short variant_start[2][CARD_ID_END + 1];

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!Log_Wanted(LOG_MODS)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "cards: %s", message);
}

/* --- names ----------------------------------------------------------- */

/* The characters a name may have, as the Shift-JIS code the glyph table
 * lists them under; the retail names are written in the same full-width
 * forms. A space is glyph 0. */
static unsigned sjis_of(int c)
{
    static const char punctuation[] = "!\"#$%&'()*+,-./:<>?";
    static const unsigned short codes[] = {0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169,
                                           0x816A, 0x8196, 0x817B, 0x8143, 0x817C, 0x8144, 0x815E, 0x8146,
                                           0x8183, 0x8184, 0x8148};
    const char *at;
    if (c >= 'A' && c <= 'Z') return 0x8260u + (unsigned)(c - 'A');
    if (c >= 'a' && c <= 'z') return 0x8281u + (unsigned)(c - 'a');
    if (c >= '0' && c <= '9') return 0x824Fu + (unsigned)(c - '0');
    at = c ? strchr(punctuation, c) : NULL;
    return at ? codes[at - punctuation] : 0;
}

static int glyph_of(int c)
{
    const unsigned *table = (const unsigned *)(uintptr_t)GLYPH_TABLE;
    unsigned sjis;
    int code;
    if (c == ' ') return 0;
    sjis = sjis_of(c);
    if (!sjis) return -1;
    for (code = 1; code < 0x600 && table[code]; code++) {
        if ((table[code] & 0xFFFFu) == sjis) return code;
    }
    return -1;
}

/* A retail name in ASCII, for matching "copy": "Kuriboh" and for the log. */
static void retail_name(int id, char *out, size_t size)
{
    const unsigned short *offsets = (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
    const unsigned *table = (const unsigned *)(uintptr_t)GLYPH_TABLE;
    const unsigned char *text = (const unsigned char *)(uintptr_t)(TEXT_BANK + offsets[id]);
    size_t n = 0;
    while (*text != 0xFF && n + 1 < size) {
        int code = *text++;
        int c = '?';
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *text++;
        if (code == 0) {
            c = ' ';
        } else {
            int probe;
            for (probe = 32; probe < 127; probe++) {
                if (sjis_of(probe) && sjis_of(probe) == (table[code] & 0xFFFFu)) { c = probe; break; }
            }
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
}

/* A glyph code as the text holds it: one byte, or F1-F5 and a byte for the
 * port's own glyphs. Returns the bytes written. */
static size_t put_glyph(unsigned char *out, int code)
{
    if (code >= 0xF0) {
        out[0] = (unsigned char)(0xF0 + (code >> 8));
        out[1] = (unsigned char)code;
        return 2;
    }
    out[0] = (unsigned char)code;
    return 1;
}

/* "{n}" is the card's number within its entry, "{id}" its card id. */
static unsigned char *encode_name(const char *mod, const char *pattern, int n, int id)
{
    char text[128];
    unsigned char *glyphs;
    size_t length = 0;
    const char *p;
    for (p = pattern; *p && length + 8 < sizeof(text); p++) {
        if (!strncmp(p, "{n}", 3)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", n); p += 2; }
        else if (!strncmp(p, "{id}", 4)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", id); p += 3; }
        else text[length++] = *p;
    }
    text[length] = '\0';
    glyphs = malloc(length * 2 + 1);
    if (!glyphs) return NULL;
    {   /* UTF-8: accented letters and the like are glyphs of the port's (glyphs.h). */
        const char *at = text;
        int bad = 0;
        length = 0;
        while (*at) {
            const char *letter = at;
            uint32_t character = Glyphs_NextCharacter(&at);
            int code;
            if (character == GLYPHS_NOT_UTF8) {
                if (!bad++) Mods_Note(mod, "card %d: its name is not UTF-8; save the file as UTF-8. Left out", id);
                continue;
            }
            code = Glyphs_Code(character);
            if (code < 0) {
                Mods_Note(mod, "card %d: the game has no letter \"%.*s\"; left out of its name", id,
                          (int)(at - letter), letter);
                continue;
            }
            length += put_glyph(glyphs + length, code);
        }
    }
    glyphs[length] = 0xFF;
    return glyphs;
}

/* Card text, wrapped as the retail texts are: lines of twenty letters at
 * most, broken at spaces (0xFE between them); "\n" breaks where it stands. */
#define TEXT_LINE_LETTERS 20
#define TEXT_LINES 8
static unsigned char *encode_description(const char *mod, const char *text, int id)
{
    size_t length = strlen(text), n = 0;
    unsigned char *glyphs = malloc(length * 2 + 2);
    const char *word = text;
    int column = 0, lines = 1, warned = 0;
    if (!glyphs) return NULL;
    while (*word) {
        const char *end = word;
        int letters;
        if (*word == '\n') {
            glyphs[n++] = 0xFE; lines++; column = 0; word++;
            continue;
        }
        if (*word == ' ') { word++; continue; }
        while (*end && *end != ' ' && *end != '\n') end++;
        letters = 0;   /* characters, not bytes */
        {
            const char *at;
            for (at = word; at < end; at++) letters += ((unsigned char)*at & 0xC0) != 0x80;
        }
        if (column && column + 1 + letters > TEXT_LINE_LETTERS) {
            glyphs[n++] = 0xFE; lines++; column = 0;
        } else if (column) {
            glyphs[n++] = 0; column++;   /* glyph 0 is the space */
        }
        while (word < end) {
            const char *letter = word;
            uint32_t character = Glyphs_NextCharacter(&word);
            int code;
            if (character == GLYPHS_NOT_UTF8) {
                if (!warned++) Mods_Note(mod, "card %d: its text is not UTF-8; save the file as UTF-8. Left out", id);
                continue;
            }
            code = Glyphs_Code(character);
            if (code < 0) {
                if (!warned++) Mods_Note(mod, "card %d: the game has no letter \"%.*s\"; left out of its text", id,
                                         (int)(word - letter), letter);
                continue;
            }
            n += put_glyph(glyphs + n, code);
            column++;
        }
    }
    glyphs[n] = 0xFF;
    if (lines > TEXT_LINES) Mods_Note(mod, "card %d: its text runs to %d lines; the card view shows %d", id, lines, TEXT_LINES);
    return glyphs;
}

/* The Library's heading, string F8: "<" then the seen count (F8 03: four
 * address bytes and a width byte, 0x80 for zero padding) then "/722>".
 * With more cards than the disc's, the "722" is their number and the count
 * as wide as it. The string is keyed by its id, not its address, so a
 * translation's heading is rewritten the same way: whatever else it says
 * stays, and a heading that jumps (whose operands only mean something where
 * they are) is left as it is. */
#define LIBRARY_HEADING_ID 0xF8
static unsigned char library_heading[64];
static const unsigned char *library_source;

static const unsigned char *heading(const unsigned char *text)
{
    const unsigned char *at;
    char digits[16];
    int width, i, n = 0;
    if (text == library_source) return library_heading;
    for (at = text; *at != 0xFF; at++) {
        if (*at >= 0xF9 && *at <= 0xFD) return text;
    }
    width = snprintf(digits, sizeof(digits), "%d", gCard_nCount);
    at = text;
    while (*at != 0xFF) {
        if ((size_t)n + 16 >= sizeof(library_heading)) return text;
        if (at[0] == 0xF8 && at[1] == 0x03 && !memchr(at + 2, 0xFF, 5)) {
            int wide = at[6] & 0x0F;
            memcpy(library_heading + n, at, 6);
            library_heading[n + 6] = (unsigned char)((at[6] & 0xF0) | (wide > width ? wide : width));
            n += 7;
            at += 7;
        } else if (at[0] == glyph_of('7') && at[1] == glyph_of('2') && at[2] == glyph_of('2')) {
            for (i = 0; i < width; i++) library_heading[n++] = (unsigned char)glyph_of(digits[i]);
            at += 3;
        } else if (at[0] >= 0xF0 && at[0] <= 0xF5) {   /* an added glyph's two bytes */
            library_heading[n++] = *at++;
            if (*at != 0xFF) library_heading[n++] = *at++;
        } else {
            library_heading[n++] = *at++;
        }
    }
    library_heading[n] = 0xFF;
    library_source = text;
    return library_heading;
}

const unsigned char *Cards_Text(int id, const unsigned char *text)
{
    if (id == LIBRARY_HEADING_ID && gCard_nCount > CARD_COUNT && text) return heading(text);
    return text;
}

/* --- building the tables --------------------------------------------- */

static const char *const type_names[] = {"Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast",
                                         "Winged Beast", "Fiend", "Fairy", "Insect", "Dinosaur", "Reptile",
                                         "Fish", "Sea Serpent", "Machine", "Thunder", "Aqua", "Pyro", "Rock",
                                         "Plant", "Magic", "Trap", "Ritual", "Equip"};
static const char *const attribute_names[] = {"Light", "Dark", "Earth", "Water", "Fire", "Wind"};
static const char *const star_names[] = {"", "Mars", "Jupiter", "Saturn", "Uranus", "Pluto",
                                         "Neptune", "Mercury", "Sun", "Moon", "Venus"};

static int same_words(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    }
    return !*a && !*b;
}

/* A number, or one of `names` spelled out. -1 when it is neither. */
static int choice(const JsonValue *value, const char *const *choices, int count)
{
    int i;
    const char *text;
    if (!value) return -1;
    if (Json_TypeOf(value) == JSON_NUMBER) return (int)Json_Number(value, -1);
    text = Json_String(value, NULL);
    if (!text) return -1;
    for (i = 0; i < count; i++) {
        if (same_words(text, choices[i])) return i;
    }
    return (int)Json_Number(value, -1);
}

/* Letters and digits only, lowercased: "Blue-Eyes White Dragon" finds the
 * disc's "Blue-eyes White Dragon". */
static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

/* The retail names, decoded once: a manifest may name hundreds of cards. */
static char (*retail_names)[48];

static int retail_by_name(const char *text)
{
    int id;
    if (!retail_names) {
        retail_names = calloc(CARD_ID_END, sizeof(*retail_names));
        if (!retail_names) return 0;
        for (id = 1; id <= CARD_COUNT; id++) retail_name(id, retail_names[id], sizeof(retail_names[id]));
    }
    for (id = 1; id <= CARD_COUNT; id++) {
        if (same_words(text, retail_names[id])) return id;
    }
    for (id = 1; id <= CARD_COUNT; id++) {
        if (same_letters(text, retail_names[id])) return id;
    }
    return 0;
}

int Cards_Reference(const JsonValue *value)
{
    const char *text;
    int id;
    if (!value || Json_TypeOf(value) == JSON_NULL) return 0;
    if (Json_TypeOf(value) == JSON_NUMBER) {
        id = (int)Json_Number(value, 0);
        return Cards_Valid(id) ? id : -1;
    }
    text = Json_String(value, NULL);
    return text ? Cards_Named(text) : -1;
}

int Cards_Named(const char *text)
{
    int id;
    if (!text || !*text) return -1;
    if (strchr(text, ':')) return (id = Cards_FindIdentity(text)) ? id : -1;
    if (strspn(text, "0123456789") == strlen(text)) return Cards_Valid(id = atoi(text)) ? id : -1;
    return (id = retail_by_name(text)) ? id : -1;
}

int Cards_TypeNamed(const char *text)
{
    int type;
    for (type = 0; text && type < (int)(sizeof(type_names) / sizeof(type_names[0])); type++) {
        if (same_letters(text, type_names[type])) return type;
    }
    return -1;
}

int Cards_Type(int id)
{
    return Cards_Valid(id) ? (int)(((unsigned)gDuel_adwCardStats[id - 1] >> 26) & 0x1F) : -1;
}

typedef struct {
    int use_count[2][CARD_ID_END];  /* copies taking a base's place, per use */
    unsigned char use[CARD_TABLE_ID_END];
} BuildContext;

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static void add_entry(const char *mod, const char *directory, int index, const JsonValue *entry, BuildContext *context)
{
    const JsonValue *copy = Json_Member(entry, "copy");
    const JsonValue *stars = Json_Member(entry, "stars");
    const char *name = Json_String(Json_Member(entry, "name"), NULL);
    const char *setting = Json_String(Json_Member(entry, "count_setting"), NULL);
    const char *description = Json_String(Json_Member(entry, "description"), NULL);
    unsigned char *record = NULL, *title = NULL, *named_plate = NULL;
    int parts = 0;
    int base = 0, count, n, value;
    unsigned stats;
    unsigned char level_attr;
    if (Json_TypeOf(entry) != JSON_OBJECT) {
        Mods_Note(mod, "cards[%d] is not an object", index);
        return;
    }
    if (Json_TypeOf(copy) == JSON_STRING && !isdigit((unsigned char)*Json_String(copy, "")) ) {
        base = retail_by_name(Json_String(copy, ""));
    } else {
        base = (int)Json_Number(copy, 0);
    }
    if (base < 1 || base > CARD_COUNT) {
        Mods_Note(mod, "cards[%d]: \"copy\" must name a card of the disc, 1 to %d", index, CARD_COUNT);
        return;
    }
    count = (int)Json_Number(Json_Member(entry, "count"), 1);
    if (setting && *setting) count = Mods_Setting(mod, setting, count);
    if (count < 0) count = 0;
    if (count > CARD_TABLE_COUNT - gCard_nCount) {
        Mods_Note(mod, "cards[%d]: only %d more cards fit (%d asked)", index, CARD_TABLE_COUNT - gCard_nCount, count);
        count = CARD_TABLE_COUNT - gCard_nCount;
    }
    /* What the entry leaves out is the base's. */
    stats = (unsigned)gDuel_adwCardStats[base - 1];
    level_attr = gDuel_abCardLevelAttr[base];
    if ((value = (int)Json_Number(Json_Member(entry, "attack"), -1)) >= 0) {
        stats = (stats & ~0x1FFu) | (unsigned)clamp(value / 10, 0, 0x1FF);
    }
    if ((value = (int)Json_Number(Json_Member(entry, "defense"), -1)) >= 0) {
        stats = (stats & ~(0x1FFu << 9)) | ((unsigned)clamp(value / 10, 0, 0x1FF) << 9);
    }
    if ((value = choice(Json_Member(entry, "type"), type_names, 24)) >= 0) {
        /* A monster has its base's 3D model and a magic, trap or equip card
         * its base's effect: a copy stays on the same side of that line. */
        int monster = ((stats >> 26) & 0x1F) < CARD_TYPE_MAGIC;
        value = clamp(value, 0, CARD_TYPE_EQUIP);
        if (monster ? value >= CARD_TYPE_MAGIC : value != (int)((stats >> 26) & 0x1F)) {
            Mods_Note(mod, "cards[%d]: %s; \"type\" left out", index,
                      monster ? "a copy of a monster stays a monster" : "a copy of a magic, trap, ritual or equip card keeps its type");
        } else {
            stats = (stats & ~(0x1Fu << 26)) | ((unsigned)value << 26);
        }
    }
    if (Json_Count(stars) == 2) {
        int first = choice(Json_At(stars, 0), star_names, 11), second = choice(Json_At(stars, 1), star_names, 11);
        if (first >= 0) stats = (stats & ~(0xFu << 22)) | ((unsigned)clamp(first, 0, 10) << 22);
        if (second >= 0) stats = (stats & ~(0xFu << 18)) | ((unsigned)clamp(second, 0, 10) << 18);
    }
    if ((value = (int)Json_Number(Json_Member(entry, "level"), -1)) >= 0) {
        level_attr = (unsigned char)((level_attr & 0xF0) | clamp(value, 0, 12));
    }
    if ((value = choice(Json_Member(entry, "attribute"), attribute_names, 6)) >= 0) {
        level_attr = (unsigned char)((level_attr & 0x0F) | (clamp(value, 0, 15) << 4));
    }
    /* Artwork: PNGs relative to the mod's directory, shared by the entry's
     * cards; a card with a name of its own gets a title plate that says it. */
    {
        static const char *const keys[] = {"art", "thumbnail", "title"};
        int k;
        for (k = 0; k < 3 && count; k++) {
            const char *file = Json_String(Json_Member(entry, keys[k]), NULL);
            char path[1200], why[1300];
            int ok;
            if (!file || !*file) continue;
            if (!Paths_Contained(file) || snprintf(path, sizeof(path), "%s/%s", directory, file) >= (int)sizeof(path)) {
                Mods_Note(mod, "cards[%d]: \"%s\": %s is outside the mod", index, keys[k], file);
                continue;
            }
            if (k == 2) {
                if (!title) title = calloc(1, CARD_TITLE_BYTES);
                ok = title && CardArt_TitleFromImage(path, title, why, sizeof(why));
            } else {
                if (!record) record = calloc(1, CARD_ART_RECORD);
                ok = record && (k == 0 ? CardArt_FromImage(path, record, why, sizeof(why))
                                       : CardArt_ThumbnailFromImage(path, record, why, sizeof(why)));
                if (ok) parts |= k == 0 ? ART_PICTURE | ART_THUMBNAIL : ART_THUMBNAIL;
            }
            if (!ok) Mods_Note(mod, "cards[%d]: \"%s\": %s", index, keys[k], why);
        }
    }
    for (n = 1; n <= count; n++) {
        char identity[192], fallback[32];
        const char *key = Json_String(Json_Member(entry, "id"), "");
        int id;
        if (!*key) { snprintf(fallback, sizeof(fallback), "entry-%d", index); key = fallback; }
        if (strlen(key) > 80 || strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(key)) {
            Mods_Note(mod, "cards[%d]: invalid stable id", index); break;
        }
        snprintf(identity, sizeof(identity), "%s:%s:%d", mod, key, n);
        if (Cards_FindIdentity(identity)) { Mods_Note(mod, "duplicate card identity %s", identity); break; }
        id = gCard_nCount + 1;
        identities[id] = strdup(identity);
        if (!identities[id]) { Mods_Note(mod, "out of memory for card identity"); break; }
        gCard_nCount = id;
        definitions[id] = entry;
        value = (int)Json_Number(Json_Member(entry, "model"), base);
        model_ids[id] = (unsigned short)(value >= 1 && value <= CARD_COUNT ? value : base);
        value = (int)Json_Number(Json_Member(entry, "effect"), base);
        effect_ids[id] = (unsigned short)(value >= 1 && value <= CARD_COUNT ? value : base);
        gCard_awBaseId[id] = (unsigned short)base;
        gDuel_adwCardStats[id - 1] = (int)stats;
        gCard_asNameSortKey[id - 1] = gCard_asNameSortKey[base - 1];
        gDuel_abCardLevelAttr[id] = level_attr;
        names[id] = name && *name ? encode_name(mod, name, n, id) : NULL;
        descriptions[id] = description && *description ? encode_description(mod, description, id) : NULL;
        art_records[id] = parts ? record : NULL;
        art_parts[id] = (unsigned char)parts;
        if (title) {
            plates[id] = title;
        } else if (name && *name && (!named_plate || strstr(name, "{n}") || strstr(name, "{id}"))) {
            /* The name as it will read, "{n}" and all, on the card's plate:
             * one plate for the entry's cards unless the name numbers them. */
            char text[128];
            size_t length = 0;
            const char *p;
            named_plate = calloc(1, CARD_TITLE_BYTES);
            for (p = name; *p && length + 8 < sizeof(text); p++) {
                if (!strncmp(p, "{n}", 3)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", n); p += 2; }
                else if (!strncmp(p, "{id}", 4)) { length += (size_t)snprintf(text + length, sizeof(text) - length, "%d", id); p += 3; }
                else text[length++] = *p;
            }
            text[length] = '\0';
            /* Without a serif font the plate is left blank: better no name
             * on the card than its base's. */
            if (named_plate) CardArt_TitleFromName(text, named_plate);
            plates[id] = named_plate;
        } else if (name && *name) {
            plates[id] = named_plate;
        }
        context->use[id] = (unsigned char)((Json_Bool(Json_Member(entry, "drops"), 1) ? 1 : 0) |
                                           (Json_Bool(Json_Member(entry, "opponents"), 0) ? 2 : 0));
        if (context->use[id] & 1) context->use_count[CARDS_USE_DROP][base]++;
        if (context->use[id] & 2) context->use_count[CARDS_USE_OPPONENT][base]++;
    }
    if (count) {
        char text[128];
        retail_name(base, text, sizeof(text));
        say("%s: cards %d-%d are copies of %d %s", mod, gCard_nCount - count + 1, gCard_nCount, base, text);
    }
}

static void add_mod(const char *mod, const char *directory, const struct JsonValue *cards, void *context)
{
    const struct JsonValue *entry;
    int i;
    for (i = 0, entry = Json_At(cards, 0); entry; i++, entry = Json_Next(entry)) add_entry(mod, directory, i, entry, context);
}

void Cards_Build(void)
{
    static int built;
    BuildContext *context;
    int id, use;
    if (built) return;
    built = 1;
    gCard_nCount = CARD_COUNT;
    memcpy(gDuel_adwCardStats, (const void *)(uintptr_t)RETAIL_STATS, CARD_COUNT * sizeof(int));
    memcpy(gCard_asNameSortKey, (const void *)(uintptr_t)RETAIL_SORT_KEYS, CARD_COUNT * sizeof(short));
    memcpy(gDuel_abCardLevelAttr, (const void *)(uintptr_t)RETAIL_LEVEL_ATTR, CARD_ID_END);
    for (id = 0; id <= CARD_COUNT; id++) gCard_awBaseId[id] = (unsigned short)id;
    context = calloc(1, sizeof(*context));
    if (!context) return;
    Mods_VisitCards(add_mod, context);
    Mods_SetCardResolver(Cards_FindIdentity);
    {
        unsigned signature = 0;
        if (gCard_nCount > CARD_COUNT) {
            signature = 2166136261u;
            for (int card = CARD_ID_END; card <= gCard_nCount; card++) {
                const unsigned char *key = (const unsigned char *)identities[card];
                while (*key) signature = (signature ^ *key++) * 16777619u;
                signature = (signature ^ (unsigned)card) * 16777619u;
            }
        }
        Mods_SetCardSignature(signature);
    }
    /* The copies that take a base's place, grouped by base, for PickVariant. */
    for (use = 0; use < 2; use++) {
        int total = 0, base, fill[CARD_ID_END];
        for (base = 0; base <= CARD_COUNT; base++) {
            variant_start[use][base] = (unsigned short)total;
            fill[base] = total;
            total += context->use_count[use][base];
        }
        variant_start[use][CARD_ID_END] = (unsigned short)total;
        if (!total) continue;
        variants[use] = malloc((size_t)total * sizeof(unsigned short));
        if (!variants[use]) continue;   /* PickVariant then keeps every retail card */
        for (id = CARD_ID_END; id <= gCard_nCount; id++) {
            if (context->use[id] & (1 << use)) variants[use][fill[gCard_awBaseId[id]]++] = (unsigned short)id;
        }
    }
    free(context);
    if (gCard_nCount > CARD_COUNT) {
        fprintf(stderr, "memories-pc: %d cards (%d added by mods)\n", gCard_nCount, gCard_nCount - CARD_COUNT);
    }
    /* The mods' fusions, equips, rituals, drops and decks name cards, the
     * new ones included. */
    Tables_Build();
}

/* --- what the game asks -------------------------------------------- */

int Cards_Valid(int id)
{
    return id >= CARD_ID_FIRST && id <= gCard_nCount;
}

int Cards_BaseId(int id)
{
    return Cards_Valid(id) ? gCard_awBaseId[id] : 0;
}

unsigned char *Cards_ChestSlot(void *state, int id)
{
    static unsigned char nowhere;
    uintptr_t at = (uintptr_t)state;
    if (id >= CARD_ID_FIRST && id <= CARD_COUNT) return (unsigned char *)state + SAVE_CHEST + id - CARD_ID_FIRST;
    nowhere = 0;
    if (!Cards_Valid(id)) return &nowhere;
    if (at == (uintptr_t)gDuel_awPlayerDeck) return &gCard_abExtraChest[id];
    if (at == SAVE_PAIR_BASE) return &gCard_abPairChest[0][id];
    if (at == SAVE_PAIR_BASE + SAVE_PAIR_STRIDE) return &gCard_abPairChest[1][id];
    if (at == SAVE_PAIR_BASE + SAVE_PAIR_COPY) return &gCard_abPairPending[0][id];
    if (at == SAVE_PAIR_BASE + SAVE_PAIR_STRIDE + SAVE_PAIR_COPY) return &gCard_abPairPending[1][id];
    return &nowhere;
}

int Cards_Seen(int id)
{
    if (id >= CARD_ID_FIRST && id <= CARD_COUNT) return Campaign_TestStoryFlag(LIBRARY_SEEN_FLAG_BASE + id);
    if (!Cards_Valid(id)) return 0;
    return (gCard_abExtraSeen[id >> 3] >> (id & 7)) & 1;
}

void Cards_MarkSeen(int id)
{
    if (id >= CARD_ID_FIRST && id <= CARD_COUNT) {
        Library_UpdateCardUsedFlag(LIBRARY_SEEN_FLAG_BASE + id);
    } else if (Cards_Valid(id)) {
        gCard_abExtraSeen[id >> 3] |= (unsigned char)(1u << (id & 7));
    }
}

const unsigned char *Cards_NameText(int id)
{
    return Cards_Valid(id) ? names[id] : NULL;
}

int Cards_NameUtf8(int id, char *out, size_t size)
{
    const unsigned char *name;
    size_t n = 0;
    if (!Cards_Valid(id) || !size) return 0;
    name = Cards_NameText(id);
    if (!name) {
        int base = Cards_BaseId(id);
        const unsigned short *offsets = (const unsigned short *)(uintptr_t)RETAIL_NAME_OFFSETS;
        name = Text_Resolve(0x8000 + base, (const unsigned char *)(uintptr_t)(TEXT_BANK + offsets[base]));
    }
    while (name && *name < 0xF6) {
        int code = *name++;
        uint32_t c;
        if (code >= 0xF0) code = ((code - 0xF0) << 8) | *name++;
        c = code ? Glyphs_Character(code) : ' ';
        if (!c) c = '?';
        if (c < 0x80 && n + 1 < size) out[n++] = (char)c;
        else if (c < 0x800 && n + 2 < size) {
            out[n++] = (char)(0xC0 | c >> 6);
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000 && n + 3 < size) {
            out[n++] = (char)(0xE0 | c >> 12);
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x10000 && n + 4 < size) {
            out[n++] = (char)(0xF0 | c >> 18);
            out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            break;
        }
    }
    out[n] = '\0';
    return 1;
}

const unsigned char *Cards_DescriptionText(int id)
{
    return Cards_Valid(id) ? descriptions[id] : NULL;
}

/* The bytes written over the base's are no longer the disc's: a texture
 * pack must not find the base card's picture in them (texture_dump.h),
 * even in the words that happen to match it. */
static void patch(unsigned char *to, const unsigned char *from, size_t bytes)
{
    memcpy(to, from, bytes);
    TextureDump_Written(to, (unsigned)bytes);
}

void Cards_PatchArtRecord(int id, unsigned char *record)
{
    if (!Cards_Valid(id)) return;
    if (art_parts[id] & ART_PICTURE) patch(record, art_records[id], CARD_TITLE_PIXELS);
    /* The plate is not reported: it sits in the middle of the sector that
     * also ends the base's palette, and a write inside a delivery drops all
     * of it (texture_dump.c, forget), so a copy with only a name of its own
     * would lose the base's pack picture. The words of a plate that match
     * the base's are the same inks, so its pack picture there is no harm. */
    if (plates[id]) memcpy(record + CARD_TITLE_PIXELS, plates[id], CARD_TITLE_BYTES);
    if (art_parts[id] & ART_THUMBNAIL) {
        patch(record + CARD_THUMB_PIXELS, art_records[id] + CARD_THUMB_PIXELS, CARD_THUMB_BLOCK);
    }
}

void Cards_PatchThumbnail(int id, unsigned char *block)
{
    if (Cards_Valid(id) && (art_parts[id] & ART_THUMBNAIL)) {
        patch(block, art_records[id] + CARD_THUMB_PIXELS, CARD_THUMB_BLOCK);
    }
}

int Cards_PickVariant(int id, int use)
{
    int first, count, pick;
    if (use < 0 || use > 1 || id < CARD_ID_FIRST || id > CARD_COUNT || !variants[use]) return id;
    first = variant_start[use][id];
    count = variant_start[use][id + 1] - first;
    if (!count) return id;
    pick = Memories_Rand() % (count + 1);
    return pick ? variants[use][first + pick - 1] : id;
}

/* --- beside the save -------------------------------------------------- */

/* cards/<duelist code>.txt in the user directory: a section per save,
 *
 *     save <sequence> <token>
 *     chest2 <identity> <count>
 *     seen2 <identity>
 *     deck2 <slot> <old-id> <base> <identity>
 *     end
 *
 * The token is the save slot's (save_slots.h), drawn afresh at each save, so
 * two slots holding the same duelist at the same sequence -- one game saved
 * twice, then played on from the older -- each find their own. A section
 * whose token a slot still holds is kept; of the rest, the newest
 * KEPT_SAVES. Sections from before tokens have none and are found by
 * sequence alone.
 *
 * Stable identities remap to this run's ids. Legacy numeric sections require
 * explicit migration with the original mods and order; preserve them until then. */

/* The tokens of the save being played, of the two saves a two-player
 * screen loaded, and of every slot (save_cards.c sets them). */
static unsigned play_token, pair_tokens[2], live_tokens[32];
static int live_count;

void Cards_SetSlotTokens(unsigned playing, const unsigned *live, int count)
{
    play_token = playing;
    live_count = count < 0 ? 0 : count > 32 ? 32 : count;
    if (live_count) memcpy(live_tokens, live, (size_t)live_count * sizeof(*live));
}

void Cards_SetPairTokens(unsigned first, unsigned second)
{
    pair_tokens[0] = first;
    pair_tokens[1] = second;
}

static int live(unsigned token)
{
    for (int i = 0; token && i < live_count; i++) if (live_tokens[i] == token) return 1;
    return 0;
}

/* A section's first line: its sequence and token (0 for none). */
static int section_header(const char *line, unsigned *sequence, unsigned *token)
{
    int got = sscanf(line, "save %u %x", sequence, token);
    if (got == 1) *token = 0;
    return got >= 1;
}

/* Which section a save of `sequence` and `token` reads: its own, else the
 * newest no later than it (what the player had when they last played with
 * the mod). 0 when there is none. */
static int choose_section(FILE *file, unsigned sequence, unsigned token, unsigned *chosen, unsigned *chosen_token)
{
    char line[512];
    int have = 0, exact = 0;
    rewind(file);
    while (fgets(line, sizeof(line), file)) {
        unsigned value, tag;
        if (!section_header(line, &value, &tag) || value > sequence || exact) continue;
        if (token && value == sequence && tag == token) {
            *chosen = value; *chosen_token = tag; have = exact = 1;
        } else if (!have || value > *chosen) {
            *chosen = value; *chosen_token = tag; have = 1;
        }
    }
    rewind(file);
    return have;
}

static int state_word(const void *state, int offset)
{
    int value;
    memcpy(&value, (const unsigned char *)state + offset, sizeof(value));
    return value;
}

static int sidecar_path(char *out, size_t size, int code)
{
    char relative[64];
    snprintf(relative, sizeof(relative), "cards/%08X.txt", (unsigned)code);
    return Paths_User(out, size, relative);
}

/* What a section says about the deck: slot, card id and its base. */
typedef struct {
    int count;
    unsigned short slot[DECK_SIZE], id[DECK_SIZE], base[DECK_SIZE], resolved[DECK_SIZE];
    unsigned char stable[DECK_SIZE];
} DeckNotes;

/* Read the section for `sequence` into `chest` (and `seen` and `deck`, if
 * given). A save made while no card mod was applied has no section of its
 * own; it has what the newest earlier one held, which is what the player
 * had when they last played with the mod. Returns the sequence read, or -1. */
static long read_section(int code, unsigned sequence, unsigned token, unsigned char *chest, unsigned char *seen,
                         DeckNotes *deck)
{
    char path[1024], line[512];
    FILE *file;
    unsigned chosen = 0, chosen_token = 0;
    int inside = 0, legacy_warning = 0, have;
    int migrate = getenv("MEMORIES_MIGRATE_CARD_IDS") && !strcmp(getenv("MEMORIES_MIGRATE_CARD_IDS"), "1");
    if (deck) deck->count = 0;
    if (sidecar_path(path, sizeof(path), code)) return -1;
    file = fopen(path, "r");
    if (!file) return -1;
    have = choose_section(file, sequence, token, &chosen, &chosen_token);
    while (have && fgets(line, sizeof(line), file)) {
        unsigned value, tag;
        int id, count, slot, base;
        char identity[192];
        if (section_header(line, &value, &tag)) {
            inside = value == chosen && tag == chosen_token;
        } else if (!inside) {
            continue;
        } else if (sscanf(line, "chest2 %191s %d", identity, &count) == 2) {
            id = Cards_FindIdentity(identity);
            if (id) chest[id] = (unsigned char)clamp(count, 0, CARD_CHEST_QUANTITY_MAX);
        } else if (sscanf(line, "seen2 %191s", identity) == 1) {
            id = Cards_FindIdentity(identity);
            if (seen && id) seen[id >> 3] |= (unsigned char)(1u << (id & 7));
        } else if (sscanf(line, "deck2 %d %d %d %191s", &slot, &id, &base, identity) == 4) {
            if (deck && deck->count < DECK_SIZE && slot >= 0 && slot < DECK_SIZE) {
                int at = deck->count++;
                deck->slot[at] = (unsigned short)slot; deck->id[at] = (unsigned short)id;
                deck->base[at] = (unsigned short)base; deck->stable[at] = 1;
                deck->resolved[at] = (unsigned short)Cards_FindIdentity(identity);
            }
        } else if (sscanf(line, "chest %d %d", &id, &count) == 2) {
            if (!migrate && !legacy_warning++) fprintf(stderr, "memories-pc: legacy card IDs have no identities; restore the original card mods and use MEMORIES_MIGRATE_CARD_IDS=1 to migrate\n");
            if (migrate && id > CARD_COUNT && Cards_Valid(id)) chest[id] = (unsigned char)clamp(count, 0, CARD_CHEST_QUANTITY_MAX);
        } else if (sscanf(line, "seen %d", &id) == 1) {
            if (migrate && seen && id > CARD_COUNT && Cards_Valid(id)) seen[id >> 3] |= (unsigned char)(1u << (id & 7));
        } else if (sscanf(line, "deck %d %d %d", &slot, &id, &base) == 3) {
            if (deck && deck->count < DECK_SIZE && slot >= 0 && slot < DECK_SIZE) {
                deck->slot[deck->count] = (unsigned short)slot;
                deck->id[deck->count] = (unsigned short)id;
                deck->stable[deck->count] = (unsigned char)!migrate;
                deck->resolved[deck->count] = 0;
                deck->base[deck->count++] = (unsigned short)base;
            }
        } else if (!strncmp(line, "end", 3)) {
            inside = 0;
        }
    }
    fclose(file);
    return have ? (long)chosen : -1;
}

/* A deck holding a card this run does not have (the mod that added it is
 * not applied, or adds fewer): each such slot gets the retail card it was a
 * copy of, or is emptied, so a duel never deals a card that is not there. */
static void repair_deck(unsigned short *cards, const DeckNotes *notes)
{
    int slot, i;
    for (slot = 0; slot < DECK_SIZE; slot++) {
        int id = cards[slot], base = 0;
        int remapped = 0;
        for (i = 0; notes && i < notes->count; i++) {
            if (notes->stable[i] && notes->slot[i] == slot && notes->id[i] == id) {
                cards[slot] = notes->resolved[i] ? notes->resolved[i] : notes->base[i];
                remapped = 1; break;
            }
        }
        if (remapped || id == 0 || Cards_Valid(id)) continue;
        for (i = 0; notes && i < notes->count; i++) {
            if (notes->slot[i] == slot && notes->id[i] == id) base = notes->base[i];
        }
        cards[slot] = (unsigned short)(base >= CARD_ID_FIRST && base <= CARD_COUNT ? base : 0);
        fprintf(stderr, "memories-pc: deck slot %d held card %d, which this run does not have; now %d\n",
                slot + 1, id, cards[slot]);
    }
}

static void write_section(int code, unsigned sequence, unsigned token, const unsigned char *chest,
                          const unsigned char *seen, const unsigned short *deck)
{
    char path[1024], temporary[1040], line[512];
    unsigned kept[KEPT_SAVES], kept_tokens[KEPT_SAVES];
    int kept_count = 0, i, id, keep = 0, any = 0, migrate = 0;
    FILE *in, *out;
    for (id = CARD_ID_END; id <= gCard_nCount; id++) {
        if (chest[id] || (seen && (seen[id >> 3] >> (id & 7)) & 1)) { any = 1; break; }
    }
    for (i = 0; deck && i < DECK_SIZE; i++) any |= deck[i] > CARD_COUNT;
    if (sidecar_path(path, sizeof(path), code)) return;
    in = fopen(path, "r");
    if (!in && !any) return;   /* nothing to say about a save without new cards */
    /* Ambiguous legacy (numeric) lines are only translated with explicit
     * migration; otherwise they are carried over unchanged while new progress
     * is still saved. Either way the original is kept once as .legacy. */
    if (in) {
        int legacy = 0;
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "chest ", 6) || !strncmp(line, "seen ", 5) || !strncmp(line, "deck ", 5)) legacy = 1;
        }
        rewind(in);
        migrate = getenv("MEMORIES_MIGRATE_CARD_IDS") && !strcmp(getenv("MEMORIES_MIGRATE_CARD_IDS"), "1");
        if (legacy && !migrate)
            fprintf(stderr, "memories-pc: keeping legacy card sidecar lines in %s pending identity migration\n", path);
        if (legacy) {
            char backup[1040]; FILE *copy;
            snprintf(backup, sizeof(backup), "%s.legacy", path);
            copy = fopen(backup, "rb");
            if (copy) fclose(copy);
            else {
                copy = fopen(backup, "wb");
                if (!copy) { fclose(in); return; }
                while (fgets(line, sizeof(line), in)) fputs(line, copy);
                { int failed = ferror(copy); if (fclose(copy)) failed = 1;
                  if (failed) { remove(backup); fclose(in); return; } }
                rewind(in);
            }
        }
    }
    /* Every section a slot still holds stays, and the newest of the others;
     * this save's own is written again below. */
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            unsigned value, tag;
            if (!section_header(line, &value, &tag) || (value == sequence && tag == token) || live(tag)) continue;
            if (kept_count < KEPT_SAVES - 1) {
                kept[kept_count] = value;
                kept_tokens[kept_count++] = tag;
            } else {
                int oldest = 0;
                for (i = 1; i < kept_count; i++) if (kept[i] < kept[oldest]) oldest = i;
                if (value > kept[oldest]) { kept[oldest] = value; kept_tokens[oldest] = tag; }
            }
        }
        rewind(in);
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    {
        char directory[1024];
        char *slash;
        snprintf(directory, sizeof(directory), "%s", path);
        slash = strrchr(directory, '/');
        if (slash) { *slash = '\0'; Paths_MakeDirs(directory); }
    }
    out = fopen(temporary, "w");
    if (!out) {
        if (in) fclose(in);
        fprintf(stderr, "memories-pc: cannot write %s\n", temporary);
        return;
    }
    fprintf(out, "# The cards mods added, as the saves of duelist %08X hold them.\n", (unsigned)code);
    while (in && fgets(line, sizeof(line), in)) {
        unsigned value, tag;
        if (section_header(line, &value, &tag)) {
            keep = !(value == sequence && tag == token) && live(tag);
            for (i = 0; i < kept_count; i++) keep |= kept[i] == value && kept_tokens[i] == tag;
        }
        if (keep) {
            int old_id, old_count, old_slot, old_base;
            if (!migrate) fputs(line, out);
            else if (sscanf(line, "chest %d %d", &old_id, &old_count) == 2 && *Cards_Identity(old_id))
                fprintf(out, "chest2 %s %d\n", Cards_Identity(old_id), old_count);
            else if (sscanf(line, "seen %d", &old_id) == 1 && *Cards_Identity(old_id))
                fprintf(out, "seen2 %s\n", Cards_Identity(old_id));
            else if (sscanf(line, "deck %d %d %d", &old_slot, &old_id, &old_base) == 3 && *Cards_Identity(old_id))
                fprintf(out, "deck2 %d %d %d %s\n", old_slot, old_id, old_base, Cards_Identity(old_id));
            else if (strncmp(line, "chest ", 6) && strncmp(line, "seen ", 5) && strncmp(line, "deck ", 5)) fputs(line, out);
        }
    }
    if (token) fprintf(out, "save %u %08x\n", sequence, token);
    else fprintf(out, "save %u\n", sequence);
    /* Carry ownership of temporarily missing mods into the new section.
     * Identity-based records cannot collide with another mod's live IDs. */
    if (in) {
        unsigned newest = 0, newest_token = 0; int have, selected_section = 0;
        have = choose_section(in, sequence, token, &newest, &newest_token);
        while (have && fgets(line, sizeof(line), in)) {
            unsigned value, tag; char identity[192];
            if (section_header(line, &value, &tag)) selected_section = value == newest && tag == newest_token;
            else if (!strncmp(line, "end", 3)) selected_section = 0;
            else if (selected_section && (sscanf(line, "chest2 %191s", identity) == 1 || sscanf(line, "seen2 %191s", identity) == 1) &&
                     !Cards_FindIdentity(identity)) fputs(line, out);
        }
    }
    for (id = CARD_ID_END; id <= gCard_nCount; id++) {
        if (chest[id]) fprintf(out, "chest2 %s %d\n", Cards_Identity(id), chest[id]);
    }
    for (id = CARD_ID_END; seen && id <= gCard_nCount; id++) {
        if ((seen[id >> 3] >> (id & 7)) & 1) fprintf(out, "seen2 %s\n", Cards_Identity(id));
    }
    for (i = 0; deck && i < DECK_SIZE; i++) {
        if (deck[i] > CARD_COUNT && Cards_Valid(deck[i])) fprintf(out, "deck2 %d %d %d %s\n", i, deck[i], Cards_BaseId(deck[i]), Cards_Identity(deck[i]));
    }
    fprintf(out, "end\n");
    if (in) fclose(in);
    if (fclose(out) != 0 || rename(temporary, path) != 0) {
        fprintf(stderr, "memories-pc: cannot write %s\n", path);
        remove(temporary);
        return;
    }
    say("saved duelist %08X save %u", (unsigned)code, sequence);
}

static void clear_extra(void)
{
    memset(gCard_abExtraChest, 0, CARD_TABLE_ID_END);
    memset(gCard_abExtraSeen, 0, (CARD_TABLE_ID_END + 7) / 8);
}

void Cards_SaveLoaded(const void *state)
{
    int code = state_word(state, SAVE_DUELIST_CODE);
    unsigned sequence = (unsigned)state_word(state, SAVE_SEQUENCE);
    DeckNotes deck;
    long read;
    clear_extra();
    gCard_nExtraOwner = code;
    /* Read even without a card mod: the deck may need its slots back. */
    read = read_section(code, sequence, play_token, gCard_abExtraChest, gCard_abExtraSeen, &deck);
    repair_deck((unsigned short *)state, &deck);
    if (read >= 0) say("loaded duelist %08X save %u (from the section of save %ld)", (unsigned)code, sequence, read);
}

void Cards_SaveWritten(const void *state, unsigned sequence)
{
    int code = state_word(state, SAVE_DUELIST_CODE);
    if (gCard_nCount <= CARD_COUNT) return;   /* no card mod: the file is left as it is */
    write_section(code, sequence, play_token, gCard_abExtraChest, gCard_abExtraSeen, (const unsigned short *)state);
}

void Cards_PairLoaded(void)
{
    int slot;
    for (slot = 0; slot < 2; slot++) {
        void *state = (void *)(uintptr_t)(SAVE_PAIR_BASE + slot * SAVE_PAIR_STRIDE);
        DeckNotes deck;
        memset(gCard_abPairChest[slot], 0, CARD_TABLE_ID_END);
        read_section(state_word(state, SAVE_DUELIST_CODE), (unsigned)state_word(state, SAVE_SEQUENCE),
                     pair_tokens[slot], gCard_abPairChest[slot], NULL, &deck);
        repair_deck((unsigned short *)state, &deck);
    }
}

void Cards_PairBackup(void)
{
    memcpy(gCard_abPairPending, gCard_abPairChest, sizeof(gCard_abPairChest));
}

void Cards_PairCommit(void)
{
    static unsigned char seen[(CARD_TABLE_ID_END + 7) / 8], chest[CARD_TABLE_ID_END];
    int slot;
    memcpy(gCard_abPairChest, gCard_abPairPending, sizeof(gCard_abPairChest));
    if (gCard_nCount <= CARD_COUNT) return;
    for (slot = 0; slot < 2; slot++) {
        const void *state = (const void *)(uintptr_t)(SAVE_PAIR_BASE + slot * SAVE_PAIR_STRIDE);
        int code = state_word(state, SAVE_DUELIST_CODE);
        unsigned sequence = (unsigned)state_word(state, SAVE_SEQUENCE);
        /* A trade writes the trunk, not the sequence number or what the
         * Library has seen: the same section, with the new trunk. */
        memset(seen, 0, sizeof(seen));
        read_section(code, sequence, pair_tokens[slot], chest, seen, NULL);
        write_section(code, sequence, pair_tokens[slot], gCard_abPairChest[slot], seen, (const unsigned short *)state);
    }
}

void Cards_Frame(void)
{
    const void *state = gDuel_awPlayerDeck;
    int code = state_word(state, SAVE_DUELIST_CODE);
    /* NEW GAME writes a new duelist code into the running save. */
    if (code != gCard_nExtraOwner) {
        clear_extra();
        gCard_nExtraOwner = code;
    }
}
