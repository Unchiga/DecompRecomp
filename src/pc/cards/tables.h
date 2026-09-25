#ifndef MEMORIES_PC_TABLES_H
#define MEMORIES_PC_TABLES_H
/* The duel's rule tables as mods change them (notes/gameplay-tables.md).
 *
 * A mod's manifest may carry "fusions", "equips", "rituals", "drops" and
 * "decks": edits to the tables the duel reads from the disc, written with
 * card names, stable identities or ids. They are read once at startup, after
 * the cards (cards.h), in the order the mods load; where two mods set the
 * same thing the later one wins, and drop and deck edits of the same
 * opponent add up rather than replace each other.
 *
 * Nothing here changes the tables the game loaded. The game's own code asks
 * these functions first, where it reads a table, and falls back to what the
 * disc (or a data mod's patch of it) says. */

/* Read every applied mod's tables; once, from Cards_Build. */
void Tables_Build(void);

/* Tables_Add without a mod list: one manifest's tables, for the tests. */
struct JsonValue;
void Tables_Add(const char *mod, const struct JsonValue *manifest);
void Tables_Clear(void);

/* A fusion rule for these two cards (in either order, or for their retail
 * bases): 1 with its result in *result, which is 0 when the rule forbids
 * the fusion; 0, leaving *result alone, when no rule applies. */
int Tables_Fusion(int a, int b, int *result);
/* What a retail recipe makes, unless a mod removed every recipe for it. */
int Tables_FilterFusion(int result);

/* Whether `equip` may equip `monster`: 1 or 0 by a mod's rule, -1 when no
 * rule says, and the disc's table decides. */
int Tables_Equip(int equip, int monster);

/* A ritual's recipe: 1 with the ritual card, its three tributes, its result
 * and a 0 after them in `recipe` (the layout of the game's ritual table),
 * 0 when a mod removed the ritual, -1 when the disc's recipe stands. */
int Tables_Ritual(int ritual, unsigned short recipe[6]);

/* A weighted pool as the running opponent's mods have it: TABLES_POOL_DECK
 * (the cards an opponent's deck is dealt from), or a drop pool (S/A-POW,
 * B/C/D, S/A-TEC, in Duel_SelectCardDrop's order). `retail` is the pool the
 * game loaded (CARD_COUNT weights by id - 1). Returns weights by card id,
 * gCard_nCount + 1 of them adding up to DUEL_DROP_WEIGHT_TOTAL, or NULL when
 * no mod edits this pool and the game reads its own. */
enum { TABLES_POOL_DECK, TABLES_POOL_POW, TABLES_POOL_BCD, TABLES_POOL_TEC, TABLES_POOL_COUNT };
const unsigned short *Tables_Pool(int pool, const unsigned short *retail);
/* The same for a given opponent (0-39), for the tests and tools. */
const unsigned short *Tables_PoolFor(int duelist, int pool, const unsigned short *retail);

/* The opponent names a manifest may use, by duelist id; "all" means every
 * one of them. */
#define TABLES_DUELIST_COUNT 40
extern const char *const Tables_DuelistNames[TABLES_DUELIST_COUNT];
/* The name the duel shows for an opponent in place of COM (hd_text.h):
 * the whole name up to 11 letters, else the part that tells them apart
 * (High Mage Anubisius: H.M. Anubisius). NULL for no opponent (a 2P duel). */
const char *Tables_DuelistShortName(int duelist);
/* The opponent of the duel under way (gDuel_bOpponentID): 1-39, negative
 * in a 2P duel. */
int Tables_OpponentId(void);

#endif
