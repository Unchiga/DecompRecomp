#ifndef MEMORIES_PC_CARDS_H
#define MEMORIES_PC_CARDS_H
#include <stddef.h>
/* More cards than the disc has (notes/more-cards.md).
 *
 * A mod's "cards" adds cards after the 722 retail ones. Each new card is a
 * copy of a retail card, its *base*: it has the base's artwork, 3D model,
 * card text, fusions, equips and effect, and its own ATK, DEF, type,
 * guardian stars, level, attribute and, optionally, name. The disc has none
 * of these cards, so wherever the game goes to the disc or to a table laid
 * out by the disc it asks for the base instead (Cards_BaseId).
 *
 * The tables the game indexes by card id (stats, level and attribute, the
 * name sort key) and the workspaces sized by the number of cards live with
 * the game's own variables (src/pc/game/card_storage.c), so save states
 * carry them. What a save holds of the new cards -- how many the player owns
 * and whether the Library has seen them -- has no room in the memory card's
 * block, so it is kept beside it in the user directory, by duelist code and
 * save sequence (cards.c). */
#include "game/card_constants.h"

/* The number of cards this run has: CARD_COUNT without a card mod. */
extern int gCard_nCount;
/* Per card id: the retail card it is a copy of (itself for retail cards). */
extern unsigned short gCard_awBaseId[];
/* The trunk and the Library's seen marks of the running save, for ids past
 * CARD_COUNT: quantities by id, and one bit per id. */
extern unsigned char gCard_abExtraChest[];
extern unsigned char gCard_abExtraSeen[];
/* Whose they are: the duelist code of the save they belong to. */
extern int gCard_nExtraOwner;
/* The same trunks for the two saves a two-player trade or duel loads, and
 * for the copies of them a trade is made on before it is written. */
extern unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END];
extern unsigned char gCard_abPairPending[2][CARD_TABLE_ID_END];

/* At startup, once the mods are applied: the retail tables from the game's
 * executable, then every applied mod's cards. */
void Cards_Build(void);
const char *Cards_Identity(int id);
int Cards_FindIdentity(const char *identity);
int Cards_ModelId(int id);
int Cards_EffectId(int id);
int Cards_Fusion(int a, int b, int *result);

/* A card a manifest names: its id, a stable identity ("mod:entry:n"), or a
 * retail card's name ("Blue-Eyes White Dragon"; case, spaces and punctuation
 * do not matter). 0 for null or no value, -1 when it names no card. */
struct JsonValue;
int Cards_Reference(const struct JsonValue *value);
int Cards_Named(const char *text);
/* A monster type by name ("Winged Beast"), or -1; and the type of a card. */
int Cards_TypeNamed(const char *text);
int Cards_Type(int id);

/* The retail card `id` is a copy of, or `id` itself; 0 for no card. */
int Cards_BaseId(int id);
/* Whether `id` names a card this run has. */
int Cards_Valid(int id);

/* The byte that holds how many of `id` the save at `state` (a SaveDataState:
 * the deck, then the trunk at +0x50) has in its trunk. For the running save
 * and the two a two-player screen loads, ids past CARD_COUNT have a trunk of
 * their own here; for any other copy of a save there is none, and the byte
 * returned for them reads zero and forgets what is written to it. */
unsigned char *Cards_ChestSlot(void *state, int id);

/* The Library's "seen" mark for `id` (campaign flag 0x120 + id for retail
 * cards, this module's bits past them). */
int Cards_Seen(int id);
void Cards_MarkSeen(int id);

/* A card's own name, in the game's glyph codes and ending in 0xFF, or NULL
 * when it has the name of its base. */
const unsigned char *Cards_NameText(int id);
/* A card's name as the game shows it now (a mod's, a translation's, or the
 * retail one) in the game's glyph codes, ending in 0xFF; NULL for no card. */
const unsigned char *Cards_NameCodes(int id);
/* The same in UTF-8. 0 for no such card. */
int Cards_NameUtf8(int id, char *out, size_t size);

/* A card's own text (glyph codes, 0xFE between lines, 0xFF at the end), or
 * NULL when it has its base's. */
const unsigned char *Cards_DescriptionText(int id);

/* A card's own artwork over its base's, as the game loads it: the art record
 * func_80029164 read (the picture, the title plate, the thumbnail), and the
 * 0x580-byte thumbnail block the duel copies for the hand and field. */
void Cards_PatchArtRecord(int id, unsigned char *record);
void Cards_PatchThumbnail(int id, unsigned char *block);

/* The game's text for string `id`, found at `text` (a translation's or the
 * disc's), or the port's own version of it where the string counts the
 * disc's 722 cards (the Library's "<seen/722>", string F8). */
const unsigned char *Cards_Text(int id, const unsigned char *text);

/* A card the game rolled from a disc table of retail cards (a duel reward,
 * an opponent's deck): `id` or one of the copies of it that asked to take
 * its place there. `use` is CARDS_USE_DROP or CARDS_USE_OPPONENT. Draws
 * from the game's own random numbers only when there is a choice, so
 * without such copies the game's sequence is untouched. */
#define CARDS_USE_DROP 0
#define CARDS_USE_OPPONENT 1
int Cards_PickVariant(int id, int use);

/* The running save was just loaded (`state` is its SaveDataState), or was
 * just written with this sequence number: read or write what it holds of the
 * new cards, under the token Cards_SetSlotTokens gave last. */
void Cards_SaveLoaded(const void *state);
void Cards_SaveWritten(const void *state, unsigned sequence);
/* The save slot tokens (save_slots.h): the save being played, and every
 * token a slot holds now, whose sections are kept; and the two saves of a
 * two-player screen. */
void Cards_SetSlotTokens(unsigned playing, const unsigned *live, int count);
void Cards_SetPairTokens(unsigned first, unsigned second);
/* A two-player screen loaded both saves (0x801D1200 and 0x1000 on). */
void Cards_PairLoaded(void);
/* A trade: its copies of the two saves (+0x680) start as the saves are, and
 * once both memory cards took them the saves are the copies, and what they
 * hold of the new cards is written beside them. */
void Cards_PairBackup(void);
void Cards_PairCommit(void);

/* The Library draws its grid: the panels behind the sections the retail
 * art does not number (library_panels.c). */
void Cards_DrawLibraryPanels(void);

/* Once a frame: a save the game started fresh (NEW GAME) owns none of the
 * new cards the previous one had. */
void Cards_Frame(void);

#endif
