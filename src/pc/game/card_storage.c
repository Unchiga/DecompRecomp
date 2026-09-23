/* The PC port's storage indexed by card id (src/pc/cards/cards.h).
 *
 * On the console these tables sit in the executable's image, packed back to
 * back with room for the disc's 722 cards and not one more. Here they are
 * defined as game variables with room for CARD_TABLE_COUNT, which the build
 * then does not pin to the retail addresses (tools/pc/build_game32.py pins
 * only what game code leaves undefined). Cards_Build fills them at startup.
 *
 * This file is compiled as a game unit, so its variables are in the game's
 * data sections: at fixed addresses, and inside every save state. Each is
 * initialized, which keeps it out of COMMON (a tentative definition would
 * be pinned like an undefined one). */
#include "types.h"
#include "game/card_constants.h"
#include "game/build_deck_transition_state.h"

int gCard_nCount = CARD_COUNT;

/* The retail tables, by id - 1 and by id (src/game/duel_card.h). */
s32 gDuel_adwCardStats[CARD_TABLE_COUNT] = {0};
s16 gCard_asNameSortKey[CARD_TABLE_COUNT] = {0};
u8 gDuel_abCardLevelAttr[CARD_TABLE_ID_END] = {0};

u16 gCard_awBaseId[CARD_TABLE_ID_END] = {0};

/* What the running save holds of the cards past CARD_COUNT. */
u8 gCard_abExtraChest[CARD_TABLE_ID_END] = {0};
u8 gCard_abExtraSeen[(CARD_TABLE_ID_END + 7) / 8] = {0};
int gCard_nExtraOwner = 0;
u8 gCard_abPairChest[2][CARD_TABLE_ID_END] = {{0}};
u8 gCard_abPairPending[2][CARD_TABLE_ID_END] = {{0}};

/* Build Deck's two panes (func_800323F8.h, BUILD_DECK_WORKSPACE): a card
 * list and three card tables each, with room for every card. */
BuildDeckTransitionState gBuildDeck_aWorkspace[2] = {{0}};

/* The Library's screen record (library_runtime.h): its motion and object
 * fields up to +0x54, then four bytes per card id. On the console that is
 * 0xB60 bytes of the 0xBA0 up to D_800EAD88. */
u8 D_800EA1E8[0x54 + 4 * CARD_TABLE_ID_END + 0x40] __attribute__((aligned(8))) = {0};

/* The Library grid's cursor (card_grid.h), wider than the console's s8. */
s16 gCardGrid_bCursorColumn = 0;
s16 gCardGrid_bCursorRow = 0;

/* The trade screen's two rows of cards and counts (main_menu/trade_helpers.h),
 * which the console keeps at 0x801845FC. */
struct { s16 id; u16 count; } gTrade_aInventory[2][CARD_TABLE_COUNT] = {{{0}}};
