#include "cheats.h"
#include "types.h"
#include "game/card_constants.h"
#include "game/save_data.h"
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
    fprintf(stderr, "memories-pc: chest now holds %d of every card\n", count);
}

void Cheats_Frame(void)
{
    static int wanted = -2; /* -2 unread, -1 off, else pending count */
    const SaveDataWorkspace *save = (const SaveDataWorkspace *)D_801D0000;
    if (wanted == -2) {
        const char *value = getenv("MEMORIES_DEBUG_CHEST");
        wanted = value && *value ? atoi(value) : -1;
    }
    if (wanted < 0) {
        return;
    }
    /* A live save has a deck; before that the workspace is scratch. */
    if (save->state.player_deck[0] != 0) {
        Cheats_GiveAllCards(wanted);
        wanted = -1;
    }
}
