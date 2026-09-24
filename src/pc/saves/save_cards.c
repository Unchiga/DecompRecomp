/* Between the save slot menu and what a save holds of the cards mods add
 * (cards.h): which slot's token a load, a save or a two-player screen goes
 * with. Called from the memory card dialog's port path
 * (src/game/mem_card_dialog_runtime.c) once the menu has finished. */
#include "save_cards.h"
#include "save_menu.h"
#include "save_slots.h"
#include "pc/cards/cards.h"

static unsigned token_of(int slot) { return slot >= 0 ? SaveSlots_Token(slot) : 0; }

static void set_tokens(void)
{
    unsigned live[SAVE_SLOT_COUNT];
    for (int slot = 0; slot < SAVE_SLOT_COUNT; slot++) live[slot] = SaveSlots_Token(slot);
    Cards_SetSlotTokens(token_of(SaveMenu_CurrentSlot()), live, SAVE_SLOT_COUNT);
}

void SaveCards_Loaded(void) { set_tokens(); }

void SaveCards_Saved(const void *state, unsigned sequence)
{
    set_tokens();
    Cards_SaveWritten(state, sequence);
}

void SaveCards_PairLoaded(void)
{
    set_tokens();
    Cards_SetPairTokens(token_of(SaveMenu_PairSlot(0)), token_of(SaveMenu_PairSlot(1)));
}
