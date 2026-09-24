/* The mods' rule tables (src/pc/cards/tables.c) against a small made-up
 * card list: fusions, equips, rituals, and drop and deck pools. */
#include "../../src/pc/cards/tables.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

int gCard_nCount = 800;
signed char gDuel_bOpponentID;
static int notes;

/* Cards 1-722 are the disc's, 723-800 copies of card (id - 722). Card 10
 * is "Kuriboh", 11 "Thunder Dragon", 12 "Blue-Eyes White Dragon", 20 an
 * equip "Legendary Sword", 21 a ritual "Black Luster Ritual"; dragons are
 * 12 and 13, everything else a warrior. */
static const char *const named[][2] = {{"Kuriboh", "10"}, {"Thunder Dragon", "11"}, {"Blue-eyes White Dragon", "12"},
                                       {"Legendary Sword", "20"}, {"Black Luster Ritual", "21"}};
int Cards_Valid(int id) { return id >= 1 && id <= gCard_nCount; }
int Cards_BaseId(int id) { return Cards_Valid(id) ? (id > CARD_COUNT ? id - CARD_COUNT : id) : 0; }
int Cards_Type(int id)
{
    id = Cards_BaseId(id);
    return id == 20 ? CARD_TYPE_EQUIP : id == 21 ? CARD_TYPE_RITUAL : id == 12 || id == 13 ? 0 : 3;
}
int Cards_TypeNamed(const char *text) { return same_letters(text, "Dragon") ? 0 : same_letters(text, "Warrior") ? 3 : -1; }
int Cards_Named(const char *text)
{
    size_t i;
    if (strspn(text, "0123456789") == strlen(text)) return Cards_Valid(atoi(text)) ? atoi(text) : -1;
    if (!strcmp(text, "test:copy:1")) return 723;
    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (same_letters(text, named[i][0])) return atoi(named[i][1]);
    }
    return -1;
}
int Cards_Reference(const JsonValue *value)
{
    if (!value || Json_TypeOf(value) == JSON_NULL) return 0;
    if (Json_TypeOf(value) == JSON_NUMBER) return Cards_Valid((int)Json_Number(value, 0)) ? (int)Json_Number(value, 0) : -1;
    return Cards_Named(Json_String(value, ""));
}
void Mods_Note(const char *id, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "note %s: ", id);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    notes++;
}
int Log_Wanted(LogChannel channel) { (void)channel; return 1; }
void Log_Printf(LogChannel channel, const char *format, ...)
{
    va_list arguments;
    (void)channel;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}
int Mods_LoadedCount(void) { return 0; }
int Mods_Loaded(int index) { return index; }
int Mods_Active(int mod) { return mod >= 0; }
const char *Mods_Id(int mod) { (void)mod; return ""; }
const JsonValue *Mods_Manifest(int mod) { (void)mod; return NULL; }

static JsonDocument *documents[16];
static int document_count;
static void add(const char *mod, const char *text)
{
    char error[128];
    JsonDocument *document = Json_Parse(text, error, sizeof(error));
    if (!document) fprintf(stderr, "%s\n", error);
    assert(document);
    documents[document_count++] = document;
    Tables_Add(mod, Json_Root(document));
}

static int fusion(int a, int b)
{
    int result = -1;
    return Tables_Fusion(a, b, &result) ? result : -1;
}

static unsigned total(const unsigned short *weights)
{
    unsigned sum = 0;
    int id;
    for (id = 1; id <= gCard_nCount; id++) sum += weights[id];
    return sum;
}

int main(void)
{
    unsigned short retail[CARD_COUNT] = {0};
    unsigned short own[6];
    const unsigned short *pool;
    int id;

    /* Fusions: added, changed by a later mod, forbidden, and removed by
     * result; either order; a copy fuses as its base. */
    add("a", "{\"fusions\": ["
             "{\"with\": [\"Kuriboh\", \"Thunder Dragon\"], \"result\": \"Blue-Eyes White Dragon\"},"
             "{\"with\": [\"Kuriboh\", 30], \"result\": 31},"
             "{\"with\": [\"Kuriboh\", 40], \"result\": null},"
             "{\"remove\": 50},"
             "{\"with\": [\"Kuriboh\", \"nothing\"], \"result\": 1}]}");
    assert(notes == 1);
    add("b", "{\"fusions\": [{\"with\": [30, \"kuriboh\"], \"result\": 32}]}");
    assert(fusion(10, 11) == 12 && fusion(11, 10) == 12);
    assert(fusion(10, 30) == 32);          /* the later mod wins */
    assert(fusion(40, 10) == 0);           /* forbidden */
    assert(fusion(10, 12) == -1);          /* no rule: the disc decides */
    assert(fusion(CARD_COUNT + 10, 11) == 12);
    assert(Tables_FilterFusion(50) == 0 && Tables_FilterFusion(51) == 51);

    /* Equips: every dragon, less one; a replaced list; a copy of the equip. */
    add("a", "{\"equips\": [{\"card\": \"Legendary Sword\", \"add\": [\"Dragon\", 5], \"remove\": [13]},"
             "{\"card\": \"Kuriboh\", \"add\": [1]}]}");
    assert(notes == 2);                    /* Kuriboh is not an equip */
    assert(Tables_Equip(20, 12) == 1 && Tables_Equip(20, 13) == 0 && Tables_Equip(20, 5) == 1);
    assert(Tables_Equip(20, 6) == -1 && Tables_Equip(CARD_COUNT + 20, 12) == 1);
    add("b", "{\"equips\": [{\"card\": 20, \"replace\": true, \"add\": [6]}]}");
    assert(Tables_Equip(20, 6) == 1 && Tables_Equip(20, 12) == 0 && Tables_Equip(20, 7) == 0);

    /* Rituals: a new recipe, then removed by a later mod. */
    add("a", "{\"rituals\": [{\"card\": \"Black Luster Ritual\", \"tributes\": [1, 2, \"test:copy:1\"], \"result\": 12}]}");
    assert(Tables_Ritual(21, own) == 1 && own[0] == 21 && own[3] == 723 && own[4] == 12 && own[5] == 0);
    assert(Tables_Ritual(22, own) == -1);
    add("b", "{\"rituals\": [{\"card\": 21, \"result\": null}]}");
    assert(Tables_Ritual(21, own) == 0);

    /* Pools. The disc's: cards 101-116 at 128 each. */
    for (id = 101; id <= 116; id++) retail[id - 1] = 128;
    assert(!Tables_PoolFor(7, TABLES_POOL_POW, retail));
    add("a", "{\"drops\": {\"Seto\": {\"pow\": {\"Blue-Eyes White Dragon\": 1024, \"101\": 0}}}}");
    pool = Tables_PoolFor(7, TABLES_POOL_POW, retail);
    assert(pool && total(pool) == 2048 && pool[12] == 1024 && pool[101] == 0);
    for (id = 102; id <= 116; id++) assert(pool[id] == 68 || pool[id] == 69);   /* 1024 over fifteen */
    assert(!Tables_PoolFor(7, TABLES_POOL_BCD, retail) && !Tables_PoolFor(8, TABLES_POOL_POW, retail));
    /* A second mod's edit of the same pool goes on top of the first. */
    add("b", "{\"drops\": {\"all\": {\"sa-pow\": {\"test:copy:1\": 48}}}}");
    pool = Tables_PoolFor(7, TABLES_POOL_POW, retail);
    assert(total(pool) == 2048 && pool[723] == 48 && pool[101] == 0 && pool[12] > 990);
    pool = Tables_PoolFor(8, TABLES_POOL_POW, retail);
    assert(total(pool) == 2048 && pool[723] == 48 && pool[101] > 0);
    /* Replacing the pool: the listed cards, in proportion. */
    add("a", "{\"drops\": {\"Heishin 2nd\": {\"tec\": {\"replace\": true, \"10\": 1, \"11\": 3}}}}");
    pool = Tables_PoolFor(35, TABLES_POOL_TEC, retail);
    assert(pool[10] == 512 && pool[11] == 1536 && total(pool) == 2048);
    /* Too much given: in proportion as well. */
    add("a", "{\"drops\": {\"3\": {\"bcd\": {\"10\": 3000, \"11\": 3000}}}}");
    pool = Tables_PoolFor(3, TABLES_POOL_BCD, retail);
    assert(pool[10] == 1024 && pool[11] == 1024 && total(pool) == 2048);

    /* Decks: a deck of fewer than 14 cards cannot be dealt, and is left. */
    notes = 0;
    add("a", "{\"decks\": {\"Teana\": {\"replace\": true, \"10\": 5, \"11\": 5}}}");
    pool = Tables_PoolFor(2, TABLES_POOL_DECK, retail);
    assert(pool && notes == 1 && pool[101] == 128 && pool[10] == 0 && total(pool) == 2048);
    add("a", "{\"decks\": {\"Pegasus\": {\"Kuriboh\": 300}}, \"drops\": {\"nobody\": {}}}");
    assert(notes == 2);
    pool = Tables_PoolFor(15, TABLES_POOL_DECK, retail);
    assert(pool[10] == 300 && total(pool) == 2048);
    /* The pool follows what the game loaded (a data mod may patch it). */
    retail[100] = 0;
    pool = Tables_PoolFor(15, TABLES_POOL_DECK, retail);
    assert(pool[101] == 0 && pool[10] == 300 && total(pool) == 2048);

    Tables_Clear();
    assert(fusion(10, 11) == -1 && Tables_Equip(20, 12) == -1 && Tables_Ritual(21, own) == -1);
    assert(!Tables_PoolFor(15, TABLES_POOL_DECK, retail));
    while (document_count) Json_Free(documents[--document_count]);
    puts("tables: ok");
    return 0;
}
