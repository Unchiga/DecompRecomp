#include "pc/compat/fs.h"
#include "cheats.h"
#include "types.h"
#include "game/card_constants.h"
#include "game/save_data.h"
#include "pc/cards/cards.h"
#include <stdio.h>
#include <stdlib.h>

/* The chest lives in the persistent save state at 0x801D0250: one byte per
 * card, ids 1..722, read by the Library, BUILD DECK and the duel's deck
 * checks, and written out whole by SAVE. */
void Cheats_GiveAllCards(int count)
{
    int id;
    if (count < 0) {
        count = 0;
    }
    if (count > CARD_CHEST_QUANTITY_MAX) {
        count = CARD_CHEST_QUANTITY_MAX;
    }
    for (id = 0; id < CARD_COUNT; id++) {
        gLibrary_abCardChest[id] = (u8)count;
    }
    /* And the cards mods added, whose trunk is kept outside the save. */
    for (id = CARD_ID_END; id <= gCard_nCount; id++) {
        *Cards_ChestSlot(gDuel_awPlayerDeck, id) = (u8)count;
    }
    fprintf(stderr, "memories-pc: chest now holds %d of every card\n", count);
}

void Cheats_UnlockAllFreeDuelists(void)
{
    const SaveDataWorkspace *save = (const SaveDataWorkspace *)D_801D0000;
    int opponent;
    /* Before a game is started or loaded, this workspace is scratch. */
    if (save->state.player_deck[0] == 0) {
        fprintf(stderr, "memories-pc: start or load a game before unlocking Free Duel opponents\n");
        return;
    }
    /* Match FreeDuel_Init's locked range. The other grid entries (including
     * Master K) are already available; these flags do not mark story wins. */
    for (opponent = FREE_DUEL_STORY_OPPONENT_FIRST_INDEX;
         opponent < FREE_DUEL_STORY_OPPONENT_INDEX_END; opponent++) {
        Library_UpdateCardUsedFlag(FREE_DUEL_UNLOCK_FLAG_BASE + opponent);
    }
    fprintf(stderr, "memories-pc: all CPU duelists unlocked; reopen Free Duel to refresh the roster, then save to keep them\n");
}

/* MEMORIES_DEBUG_DECK: the forty cards of the deck, as ids and ranges
 * ("723-762", "1,2,723"), repeated until the deck is full. */
static void set_deck(const char *list)
{
    SaveDataWorkspace *save = (SaveDataWorkspace *)D_801D0000;
    int ids[DECK_SIZE], count = 0, i;
    const char *p = list;
    while (*p && count < DECK_SIZE) {
        char *end;
        long first = strtol(p, &end, 10), last;
        if (end == p) break;
        last = first;
        if (*end == '-') last = strtol(end + 1, &end, 10);
        for (; first <= last && count < DECK_SIZE; first++) {
            if (first >= CARD_ID_FIRST && first <= gCard_nCount) ids[count++] = (int)first;
        }
        p = *end == ',' ? end + 1 : end;
    }
    if (!count) return;
    for (i = 0; i < DECK_SIZE; i++) save->state.player_deck[i] = (u16)ids[i % count];
    fprintf(stderr, "memories-pc: deck set from %s\n", list);
}

void Cheats_Frame(void)
{
    static int wanted = -2; /* -2 unread, -1 off, else pending count */
    static const char *deck;
    const SaveDataWorkspace *save = (const SaveDataWorkspace *)D_801D0000;
    if (wanted == -2) {
        const char *value = getenv("MEMORIES_DEBUG_CHEST");
        wanted = value && *value ? atoi(value) : -1;
        deck = getenv("MEMORIES_DEBUG_DECK");
        if (deck && !*deck) deck = NULL;
    }
    if (wanted < 0 && !deck) {
        return;
    }
    /* A live save has a deck; before that the workspace is scratch. */
    if (save->state.player_deck[0] != 0) {
        if (wanted >= 0) Cheats_GiveAllCards(wanted);
        if (deck) set_deck(deck);
        wanted = -1;
        deck = NULL;
    }
}
