/* The duel's rule tables as mods change them (tables.h,
 * notes/gameplay-tables.md).
 *
 * Each applied mod's "fusions", "equips", "rituals", "drops" and "decks" are
 * read into rules here, once, in the order the mods load. The game's table
 * readers (duel_card_checks.c, duel_check_ritual.c, duel_shuffle_deck.c,
 * duel_result_runtime.c) ask these first. A drop or deck pool is worked out
 * from what the game loaded when it is drawn from, so a data mod's patch of
 * the same pool comes first and the edits here go on top of it. */
#include "tables.h"
#include "cards.h"
#include "pc/mods/mods.h"
#include "pc/mods/json.h"
#include "pc/debug/log.h"
#include "game/card_constants.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern signed char gDuel_bOpponentID;

const char *const Tables_DuelistNames[TABLES_DUELIST_COUNT] = {
    "Unused", "Simon Muran", "Teana", "Jono", "Villager 1", "Villager 2", "Villager 3", "Seto", "Heishin",
    "Rex Raptor", "Weevil Underwood", "Mai Valentine", "Bandit Keith", "Shadi", "Yami Bakura", "Pegasus",
    "Isis", "Kaiba", "Mage Soldier", "Jono 2nd", "Teana 2nd", "Ocean Mage", "High Mage Secmeton",
    "Forest Mage", "High Mage Anubisius", "Mountain Mage", "High Mage Atenza", "Desert Mage",
    "High Mage Martis", "Meadow Mage", "High Mage Kepura", "Labyrinth Mage", "Seto 2nd", "Guardian Sebek",
    "Guardian Neku", "Heishin 2nd", "Seto 3rd", "DarkNite", "Nitemare", "Duel Master K"};

const char *Tables_DuelistShortName(int duelist)
{
    /* The names longer than 11 letters, shortened. */
    static const struct {
        int duelist;
        const char *name;
    } shorter[] = {{10, "Weevil"},   {11, "Mai"},       {12, "Keith"},    {18, "Soldier"},  {22, "Secmeton"},
                   {24, "Anubisius"}, {25, "Mountain"},  {26, "Atenza"},   {28, "Martis"},   {30, "Kepura"},
                   {31, "Labyrinth"}, {33, "Sebek"},     {34, "Neku"},     {39, "Master K"}};
    unsigned i;
    if (duelist < 1 || duelist >= TABLES_DUELIST_COUNT) return NULL;
    for (i = 0; i < sizeof(shorter) / sizeof(shorter[0]); i++) {
        if (shorter[i].duelist == duelist) return shorter[i].name;
    }
    return Tables_DuelistNames[duelist];
}

int Tables_OpponentId(void)
{
    return gDuel_bOpponentID;
}

static const char *const pool_names[TABLES_POOL_COUNT] = {"deck", "pow", "bcd", "tec"};
static const char *const pool_long_names[TABLES_POOL_COUNT] = {"deck", "sa-pow", "b-c-d", "sa-tec"};

/* A deck is 40 cards, at most three of each, so its pool needs 14. */
#define POOL_TOTAL DUEL_DROP_WEIGHT_TOTAL
#define DECK_POOL_MIN_CARDS ((DECK_SIZE + DECK_CARD_COPY_LIMIT - 1) / DECK_CARD_COPY_LIMIT)

typedef struct {
    unsigned short low, high;   /* the two cards, low <= high */
    int result;                 /* 0: the fusion is forbidden */
    unsigned order;             /* later rules win */
} FusionRule;

enum { TARGET_ANY, TARGET_TYPE, TARGET_CARD };
typedef struct {
    unsigned short equip;
    unsigned char kind, allow;  /* TARGET_*, and whether it may equip */
    int target;                 /* a type or a card id */
    unsigned order;
} EquipRule;

typedef struct {
    unsigned short recipe[6];   /* ritual, three tributes, result, 0 */
    unsigned char removed;
} RitualRule;

typedef struct {
    const char *mod;
    unsigned char duelist, pool, replace, warned;
    int count;
    unsigned short *cards, *weights;
} PoolEdit;

static FusionRule *fusions;
static int fusion_count, fusion_room;
static unsigned char *removed_results;   /* by card id: no retail recipe makes it */
static int removed_room;
static EquipRule *equips;
static int equip_count, equip_room;
static RitualRule *rituals;
static int ritual_count, ritual_room;
static PoolEdit *edits;
static int edit_count, edit_room;
static unsigned char edited[TABLES_DUELIST_COUNT][TABLES_POOL_COUNT];
static unsigned order_counter;
static int fusions_sorted;

/* Grow an array by one; NULL when memory runs out. */
static void *grow(void *array, int *room, int count, size_t size)
{
    void **slot = (void **)array;
    if (count >= *room) {
        int wanted = *room ? *room * 2 : 16;
        void *bigger = realloc(*slot, (size_t)wanted * size);
        if (!bigger) return NULL;
        *slot = bigger;
        *room = wanted;
    }
    return (char *)*slot + (size_t)count * size;
}

static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a)) a++;
        while (*b && !isalnum((unsigned char)*b)) b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
}

/* A card the manifest names, or 0 after saying why it names none. */
static int card(const char *mod, const char *where, const JsonValue *value)
{
    int id = Cards_Reference(value);
    if (id > 0) return id;
    if (Json_TypeOf(value) == JSON_STRING) Mods_Note(mod, "%s: no card \"%s\"", where, Json_String(value, ""));
    else if (Json_TypeOf(value) == JSON_NUMBER) Mods_Note(mod, "%s: no card %ld", where, Json_Number(value, 0));
    else Mods_Note(mod, "%s: a card is a name, an id or a stable identity", where);
    return 0;
}

/* A card or a monster type ("Dragon"), for an equip's targets. */
static int target(const char *mod, const char *where, const JsonValue *value, EquipRule *rule)
{
    int type = Json_TypeOf(value) == JSON_STRING ? Cards_TypeNamed(Json_String(value, "")) : -1;
    if (type >= 0 && Cards_Reference(value) <= 0) {
        rule->kind = TARGET_TYPE;
        rule->target = type;
        return 1;
    }
    rule->kind = TARGET_CARD;
    rule->target = card(mod, where, value);
    return rule->target != 0;
}

/* --- fusions --------------------------------------------------------- */

static void add_fusion(int a, int b, int result)
{
    FusionRule *rule = grow(&fusions, &fusion_room, fusion_count, sizeof(*fusions));
    if (!rule) return;
    rule->low = (unsigned short)(a < b ? a : b);
    rule->high = (unsigned short)(a < b ? b : a);
    rule->result = result;
    rule->order = ++order_counter;
    fusion_count++;
    fusions_sorted = 0;
}

static void read_fusions(const char *mod, const JsonValue *list)
{
    int i;
    char where[64];
    if (list && Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "\"fusions\" is not an array");
        return;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *rule = Json_At(list, i);
        const JsonValue *with = Json_Member(rule, "with");
        const JsonValue *result = Json_Member(rule, "result");
        const JsonValue *removed = Json_Member(rule, "remove");
        int a, b, made = 0;
        snprintf(where, sizeof(where), "fusions[%d]", i);
        if (removed) {
            /* { "remove": card }: no recipe on the disc makes it any more. */
            int id = card(mod, where, removed);
            if (!id) continue;
            if (id >= removed_room) {
                int room = gCard_nCount + 1 > id + 1 ? gCard_nCount + 1 : id + 1;
                unsigned char *bigger = realloc(removed_results, (size_t)room);
                if (!bigger) continue;
                memset(bigger + removed_room, 0, (size_t)(room - removed_room));
                removed_results = bigger;
                removed_room = room;
            }
            removed_results[id] = 1;
            continue;
        }
        if (Json_Count(with) != 2) {
            Mods_Note(mod, "%s: \"with\" names two cards", where);
            continue;
        }
        a = card(mod, where, Json_At(with, 0));
        b = card(mod, where, Json_At(with, 1));
        if (!a || !b) continue;
        if (!result) {
            Mods_Note(mod, "%s: \"result\" is a card, or null to forbid the fusion", where);
            continue;
        }
        if (Json_TypeOf(result) != JSON_NULL && !(Json_TypeOf(result) == JSON_NUMBER && Json_Number(result, 0) == 0)) {
            made = card(mod, where, result);
            if (!made) continue;
        }
        add_fusion(a, b, made);
    }
}

static int by_pair(const void *left, const void *right)
{
    const FusionRule *a = left, *b = right;
    if (a->low != b->low) return a->low < b->low ? -1 : 1;
    if (a->high != b->high) return a->high < b->high ? -1 : 1;
    return a->order > b->order ? -1 : a->order < b->order;   /* the latest first */
}

static const FusionRule *find_fusion(int a, int b)
{
    int low = 0, high = fusion_count;
    FusionRule key;
    key.low = (unsigned short)(a < b ? a : b);
    key.high = (unsigned short)(a < b ? b : a);
    while (low < high) {   /* the first rule for the pair: the latest */
        int middle = (low + high) / 2;
        const FusionRule *rule = &fusions[middle];
        if (rule->low < key.low || (rule->low == key.low && rule->high < key.high)) low = middle + 1;
        else high = middle;
    }
    if (low < fusion_count && fusions[low].low == key.low && fusions[low].high == key.high) return &fusions[low];
    return NULL;
}

int Tables_Fusion(int a, int b, int *result)
{
    const FusionRule *rule;
    int base_a, base_b;
    if (!fusion_count || !Cards_Valid(a) || !Cards_Valid(b)) return 0;
    if (!fusions_sorted) {
        qsort(fusions, (size_t)fusion_count, sizeof(*fusions), by_pair);
        fusions_sorted = 1;
    }
    rule = find_fusion(a, b);
    base_a = Cards_BaseId(a);
    base_b = Cards_BaseId(b);
    /* A copy fuses as its base, as it does in the disc's table; a rule
     * naming a copy itself is surer, so one that names both cards as they
     * are comes first, then one that names one of them (a copy with its
     * partner's base; of two such, the later), then the bases' own. */
    if (!rule) {
        const FusionRule *one = base_b != b ? find_fusion(a, base_b) : NULL;
        const FusionRule *other = base_a != a ? find_fusion(base_a, b) : NULL;
        rule = one && (!other || one->order > other->order) ? one : other;
    }
    if (!rule && base_a != a && base_b != b) rule = find_fusion(base_a, base_b);
    if (!rule) return 0;
    *result = rule->result;
    return 1;
}

int Tables_FilterFusion(int result)
{
    return result > 0 && result < removed_room && removed_results[result] ? 0 : result;
}

/* --- equips ---------------------------------------------------------- */

static void add_equip(int equip, EquipRule rule, unsigned order)
{
    EquipRule *slot = grow(&equips, &equip_room, equip_count, sizeof(*equips));
    if (!slot) return;
    rule.equip = (unsigned short)equip;
    rule.order = order;
    *slot = rule;
    equip_count++;
}

static void read_equips(const char *mod, const JsonValue *list)
{
    int i, j, pass;
    char where[80];
    if (list && Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "\"equips\" is not an array");
        return;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *entry = Json_At(list, i);
        unsigned order = ++order_counter;
        int equip;
        snprintf(where, sizeof(where), "equips[%d]", i);
        equip = card(mod, where, Json_Member(entry, "card"));
        if (!equip) continue;
        if (Cards_Type(equip) != CARD_TYPE_EQUIP) {
            Mods_Note(mod, "%s: \"card\" is not an equip card", where);
            continue;
        }
        if (Json_Bool(Json_Member(entry, "replace"), 0)) {
            EquipRule none = {0};
            none.kind = TARGET_ANY;
            add_equip(equip, none, order);
        }
        for (pass = 0; pass < 2; pass++) {
            const JsonValue *targets = Json_Member(entry, pass ? "remove" : "add");
            for (j = 0; j < Json_Count(targets); j++) {
                EquipRule rule = {0};
                snprintf(where, sizeof(where), "equips[%d].%s[%d]", i, pass ? "remove" : "add", j);
                if (!target(mod, where, Json_At(targets, j), &rule)) continue;
                rule.allow = (unsigned char)!pass;
                add_equip(equip, rule, order);
            }
        }
    }
}

int Tables_Equip(int equip, int monster)
{
    const EquipRule *best = NULL;
    int i, base_equip, base_monster, type;
    if (!equip_count || !Cards_Valid(equip) || !Cards_Valid(monster)) return -1;
    base_equip = Cards_BaseId(equip);
    base_monster = Cards_BaseId(monster);
    type = Cards_Type(monster);
    /* The latest entry that says anything decides; within it a card is
     * surer than a type, and a type than "replace". */
    for (i = 0; i < equip_count; i++) {
        const EquipRule *rule = &equips[i];
        int match;
        if (rule->equip != equip && rule->equip != base_equip) continue;
        match = rule->kind == TARGET_ANY || (rule->kind == TARGET_TYPE && rule->target == type) ||
                (rule->kind == TARGET_CARD && (rule->target == monster || rule->target == base_monster));
        if (!match) continue;
        if (!best || rule->order > best->order || (rule->order == best->order && rule->kind >= best->kind)) best = rule;
    }
    return best ? best->allow : -1;
}

/* --- rituals --------------------------------------------------------- */

static void read_rituals(const char *mod, const JsonValue *list)
{
    int i, j;
    char where[64];
    if (list && Json_TypeOf(list) != JSON_ARRAY) {
        Mods_Note(mod, "\"rituals\" is not an array");
        return;
    }
    for (i = 0; i < Json_Count(list); i++) {
        const JsonValue *entry = Json_At(list, i);
        const JsonValue *tributes = Json_Member(entry, "tributes");
        const JsonValue *result = Json_Member(entry, "result");
        RitualRule rule = {{0}, 0}, *slot;
        int ritual, ok = 1;
        snprintf(where, sizeof(where), "rituals[%d]", i);
        ritual = card(mod, where, Json_Member(entry, "card"));
        if (!ritual) continue;
        if (ritual > CARD_COUNT || Cards_Type(ritual) != CARD_TYPE_RITUAL) {
            /* The duel asks for a ritual by the retail card whose effect it is. */
            Mods_Note(mod, "%s: \"card\" must be one of the disc's ritual cards", where);
            continue;
        }
        rule.recipe[0] = (unsigned short)ritual;
        if (result && Json_TypeOf(result) == JSON_NULL) {
            rule.removed = 1;
        } else {
            if (Json_Count(tributes) != DUEL_RITUAL_TRIBUTE_COUNT) {
                Mods_Note(mod, "%s: \"tributes\" names three monsters", where);
                continue;
            }
            for (j = 0; j < DUEL_RITUAL_TRIBUTE_COUNT && ok; j++) {
                rule.recipe[1 + j] = (unsigned short)card(mod, where, Json_At(tributes, j));
                ok = rule.recipe[1 + j] != 0;
            }
            rule.recipe[4] = (unsigned short)(ok ? card(mod, where, result) : 0);
            if (!ok || !rule.recipe[4]) continue;
        }
        slot = grow(&rituals, &ritual_room, ritual_count, sizeof(*rituals));
        if (!slot) continue;
        *slot = rule;
        ritual_count++;
    }
}

int Tables_Ritual(int ritual, unsigned short recipe[6])
{
    int i;
    for (i = ritual_count - 1; i >= 0; i--) {   /* the latest */
        if (rituals[i].recipe[0] != ritual) continue;
        if (rituals[i].removed) return 0;
        memcpy(recipe, rituals[i].recipe, sizeof(rituals[i].recipe));
        return 1;
    }
    return -1;
}

/* --- drops and decks ------------------------------------------------- */

static int duelist_named(const char *text)
{
    int id;
    if (!text || !*text) return -1;
    if (strspn(text, "0123456789") == strlen(text)) return (id = atoi(text)) < TABLES_DUELIST_COUNT ? id : -1;
    for (id = 0; id < TABLES_DUELIST_COUNT; id++) {
        if (same_letters(text, Tables_DuelistNames[id])) return id;
    }
    return -1;
}

static int pool_named(const char *text)
{
    int pool;
    for (pool = TABLES_POOL_POW; pool < TABLES_POOL_COUNT; pool++) {
        if (same_letters(text, pool_names[pool]) || same_letters(text, pool_long_names[pool])) return pool;
    }
    return -1;
}

/* One pool of one or every opponent: { card: weight, ..., "replace": true }. */
static void read_pool(const char *mod, const char *where, int duelist, int pool, const JsonValue *cards)
{
    PoolEdit edit;
    int i, n = 0, first, last;
    if (Json_TypeOf(cards) != JSON_OBJECT) {
        Mods_Note(mod, "%s: a pool is an object of cards and their weights", where);
        return;
    }
    memset(&edit, 0, sizeof(edit));
    edit.mod = mod;
    edit.pool = (unsigned char)pool;
    edit.replace = (unsigned char)Json_Bool(Json_Member(cards, "replace"), 0);
    edit.cards = calloc((size_t)Json_Count(cards) + 1, sizeof(unsigned short));
    edit.weights = calloc((size_t)Json_Count(cards) + 1, sizeof(unsigned short));
    if (!edit.cards || !edit.weights) {
        free(edit.cards);
        free(edit.weights);
        return;
    }
    for (i = 0; i < Json_Count(cards); i++) {
        const JsonValue *member = Json_At(cards, i);
        const char *name = Json_Name(member);
        long weight;
        int id;
        char at[160];
        if (!strcmp(name, "replace")) continue;
        snprintf(at, sizeof(at), "%s \"%s\"", where, name);
        id = Cards_Named(name);
        if (id <= 0) {
            Mods_Note(mod, "%s: no such card", at);
            continue;
        }
        weight = Json_Number(member, -1);
        if (Json_TypeOf(member) != JSON_NUMBER || weight < 0) {
            Mods_Note(mod, "%s: a weight is a whole number, 0 or more", at);
            continue;
        }
        edit.cards[n] = (unsigned short)id;
        edit.weights[n++] = (unsigned short)(weight > 0xFFFF ? 0xFFFF : weight);
    }
    edit.count = n;
    if (!n && !edit.replace) {
        free(edit.cards);
        free(edit.weights);
        return;
    }
    first = duelist < 0 ? 0 : duelist;
    last = duelist < 0 ? TABLES_DUELIST_COUNT - 1 : duelist;
    for (i = first; i <= last; i++) {
        PoolEdit *slot = grow(&edits, &edit_room, edit_count, sizeof(*edits));
        if (!slot) break;
        *slot = edit;
        slot->duelist = (unsigned char)i;
        edit_count++;
        edited[i][pool] = 1;
    }
    /* The edit's lists are shared by the opponents it names, and live on. */
}

/* "drops": { opponent: { pool: { card: weight } } }, and
 * "decks": { opponent: { card: weight } }. */
static void read_pools(const char *mod, const JsonValue *table, int decks)
{
    int i, j;
    char where[128];
    if (table && Json_TypeOf(table) != JSON_OBJECT) {
        Mods_Note(mod, "\"%s\" is an object of opponents", decks ? "decks" : "drops");
        return;
    }
    for (i = 0; i < Json_Count(table); i++) {
        const JsonValue *entry = Json_At(table, i);
        const char *name = Json_Name(entry);
        int duelist = same_letters(name, "all") ? -1 : duelist_named(name);
        if (duelist == -1 && !same_letters(name, "all")) {
            Mods_Note(mod, "%s: no opponent \"%s\"", decks ? "decks" : "drops", name);
            continue;
        }
        if (decks) {
            snprintf(where, sizeof(where), "decks \"%s\"", name);
            read_pool(mod, where, duelist, TABLES_POOL_DECK, entry);
            continue;
        }
        if (Json_TypeOf(entry) != JSON_OBJECT) {
            Mods_Note(mod, "drops \"%s\": an object of pools (pow, bcd, tec)", name);
            continue;
        }
        for (j = 0; j < Json_Count(entry); j++) {
            const JsonValue *pool = Json_At(entry, j);
            int which = pool_named(Json_Name(pool));
            snprintf(where, sizeof(where), "drops \"%s\" \"%s\"", name, Json_Name(pool));
            if (which < 0) {
                Mods_Note(mod, "%s: the pools are pow, bcd and tec", where);
                continue;
            }
            read_pool(mod, where, duelist, which, pool);
        }
    }
}

/* Scale the chosen weights so they add up to `target` exactly: each gets
 * its share rounded down, and what that leaves goes to the largest
 * remainders (the lower id first between equals). */
typedef struct {
    unsigned remainder;
    int id;
} Share;

static int by_remainder(const void *left, const void *right)
{
    const Share *a = left, *b = right;
    if (a->remainder != b->remainder) return a->remainder > b->remainder ? -1 : 1;
    return a->id - b->id;
}

static Share *shares;
static int share_room;

static int scale(unsigned *weights, const unsigned char *chosen, int count, unsigned target)
{
    unsigned long long sum = 0;
    unsigned given = 0;
    int id, n = 0;
    for (id = 1; id <= count; id++) if (chosen[id]) sum += weights[id];
    if (!sum) return target == 0;
    if (count + 1 > share_room) {
        Share *bigger = realloc(shares, (size_t)(count + 1) * sizeof(*shares));
        if (!bigger) return 0;
        shares = bigger;
        share_room = count + 1;
    }
    for (id = 1; id <= count; id++) {
        unsigned long long part;
        if (!chosen[id] || !weights[id]) continue;
        part = (unsigned long long)weights[id] * target;
        weights[id] = (unsigned)(part / sum);
        given += weights[id];
        shares[n].remainder = (unsigned)(part % sum);
        shares[n++].id = id;
    }
    qsort(shares, (size_t)n, sizeof(*shares), by_remainder);
    for (id = 0; given < target && id < n; id++, given++) weights[shares[id].id]++;
    return 1;
}

/* One mod's edit of a pool, over what it was; 0 (and the pool as it was)
 * when it leaves a pool the game cannot draw from. */
static int apply(const PoolEdit *edit, unsigned *weights, unsigned *before, unsigned char *listed, int count)
{
    unsigned long long given = 0, rest = 0;
    int i, id, cards = 0;
    memcpy(before, weights, (size_t)(count + 1) * sizeof(*weights));
    memset(listed, 0, (size_t)(count + 1));
    if (edit->replace) memset(weights, 0, (size_t)(count + 1) * sizeof(*weights));
    for (i = 0; i < edit->count; i++) {
        if (edit->cards[i] > count) continue;   /* not this run's (the tests) */
        weights[edit->cards[i]] = edit->weights[i];
        listed[edit->cards[i]] = 1;
    }
    for (id = 1; id <= count; id++) {
        if (listed[id]) given += weights[id];
        else rest += weights[id];
    }
    if (given >= POOL_TOTAL || !rest) {
        /* The listed cards are the pool, in proportion. */
        for (id = 1; id <= count; id++) if (!listed[id]) weights[id] = 0;
        if (!scale(weights, listed, count, POOL_TOTAL) || !given) goto refuse;
    } else {
        /* The listed cards have their weights; the rest share what is left. */
        for (id = 1; id <= count; id++) listed[id] = !listed[id];
        if (!scale(weights, listed, count, POOL_TOTAL - (unsigned)given)) goto refuse;
    }
    for (id = 1; id <= count; id++) cards += weights[id] != 0;
    if (edit->pool == TABLES_POOL_DECK && cards < DECK_POOL_MIN_CARDS) goto refuse;
    return 1;
refuse:
    memcpy(weights, before, (size_t)(count + 1) * sizeof(*weights));
    return 0;
}

/* The pools worked out last, one per kind: an opponent draws from the same
 * one forty times, and a duel's drop once. */
typedef struct {
    int duelist, count;
    unsigned checksum;
    unsigned short *weights;
} PoolCache;
static PoolCache caches[TABLES_POOL_COUNT];
static unsigned *work, *spare;
static unsigned char *marks;
static int work_room;

const unsigned short *Tables_PoolFor(int duelist, int pool, const unsigned short *retail)
{
    PoolCache *cache;
    unsigned checksum = 2166136261u;
    int count = gCard_nCount, id, i;
    if (duelist < 0 || duelist >= TABLES_DUELIST_COUNT || pool < 0 || pool >= TABLES_POOL_COUNT) return NULL;
    if (!edited[duelist][pool] || !retail) return NULL;
    for (id = 0; id < CARD_COUNT; id++) checksum = (checksum ^ retail[id]) * 16777619u;
    cache = &caches[pool];
    if (cache->weights && cache->duelist == duelist && cache->count == count && cache->checksum == checksum)
        return cache->weights;
    if (count + 1 > work_room) {
        unsigned *a = realloc(work, (size_t)(count + 1) * sizeof(*work));
        unsigned *b = a ? realloc(spare, (size_t)(count + 1) * sizeof(*spare)) : NULL;
        unsigned char *c = b ? realloc(marks, (size_t)(count + 1)) : NULL;
        if (a) work = a;
        if (b) spare = b;
        if (!c) return NULL;
        marks = c;
        work_room = count + 1;
    }
    if (cache->count != count || !cache->weights) {
        unsigned short *bigger = realloc(cache->weights, (size_t)(count + 1) * sizeof(*cache->weights));
        if (!bigger) return NULL;
        cache->weights = bigger;
    }
    memset(work, 0, (size_t)(count + 1) * sizeof(*work));
    for (id = 1; id <= CARD_COUNT && id <= count; id++) work[id] = retail[id - 1];
    for (i = 0; i < edit_count; i++) {
        PoolEdit *edit = &edits[i];
        if (edit->duelist != duelist || edit->pool != pool) continue;
        if (!apply(edit, work, spare, marks, count) && !edit->warned) {
            edit->warned = 1;
            if (pool) Mods_Note(edit->mod, "%s's %s drops: left as they were; no card would be left to win",
                                Tables_DuelistNames[duelist], pool_names[pool]);
            else Mods_Note(edit->mod, "%s's deck: left as it was; a deck is dealt from at least %d cards",
                           Tables_DuelistNames[duelist], DECK_POOL_MIN_CARDS);
        }
    }
    {
        unsigned total = 0;
        for (id = 1; id <= count; id++) total += work[id];
        /* A pool the disc (or a data mod) left short is the game's affair,
         * but an empty one would stall the draw: keep the game's own. */
        if (!total) return NULL;
    }
    for (id = 0; id <= count; id++) cache->weights[id] = (unsigned short)(work[id] > 0xFFFF ? 0xFFFF : work[id]);
    if (Log_Wanted(LOG_MODS)) {
        int cards = 0, heaviest = 0;
        for (id = 1; id <= count; id++) {
            cards += work[id] != 0;
            if (work[id] > work[heaviest]) heaviest = id;
        }
        LOG(LOG_MODS, "tables: %s's %s as the mods have it: %d cards, the likeliest %d (%u/%d)",
            Tables_DuelistNames[duelist], pool_names[pool], cards, heaviest, work[heaviest], POOL_TOTAL);
    }
    cache->duelist = duelist;
    cache->count = count;
    cache->checksum = checksum;
    return cache->weights;
}

const unsigned short *Tables_Pool(int pool, const unsigned short *retail)
{
    return Tables_PoolFor(gDuel_bOpponentID, pool, retail);
}

/* --- building -------------------------------------------------------- */

static void forget_pools(void)
{
    int pool;
    for (pool = 0; pool < TABLES_POOL_COUNT; pool++) caches[pool].count = -1;
}

void Tables_Add(const char *mod, const JsonValue *manifest)
{
    forget_pools();
    read_fusions(mod, Json_Member(manifest, "fusions"));
    read_equips(mod, Json_Member(manifest, "equips"));
    read_rituals(mod, Json_Member(manifest, "rituals"));
    read_pools(mod, Json_Member(manifest, "drops"), 0);
    read_pools(mod, Json_Member(manifest, "decks"), 1);
}

void Tables_Clear(void)
{
    int i;
    /* Edits of "all" share their lists: free each list once. */
    for (i = 0; i < edit_count; i++) {
        int shared = 0, j;
        for (j = 0; j < i && !shared; j++) shared = edits[j].cards == edits[i].cards;
        if (!shared) {
            free(edits[i].cards);
            free(edits[i].weights);
        }
    }
    fusion_count = equip_count = ritual_count = edit_count = 0;
    if (removed_results) memset(removed_results, 0, (size_t)removed_room);
    memset(edited, 0, sizeof(edited));
    forget_pools();
}

void Tables_Build(void)
{
    static int built;
    int i;
    if (built) return;
    built = 1;
    for (i = 0; i < Mods_LoadedCount(); i++) {
        int mod = Mods_Loaded(i);
        if (Mods_Active(mod)) Tables_Add(Mods_Id(mod), Mods_Manifest(mod));
    }
    if (fusion_count || equip_count || ritual_count || edit_count)
        LOG(LOG_MODS, "tables: %d fusion rules, %d equip rules, %d rituals, %d pool edits",
            fusion_count, equip_count, ritual_count, edit_count);
}
