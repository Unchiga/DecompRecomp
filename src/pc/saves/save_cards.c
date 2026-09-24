/* Between the save slot menu and what a save holds of the cards mods add
 * (cards.h): which slot's token a load, a save or a two-player screen goes
 * with. Called from the memory card dialog's port path
 * (src/game/mem_card_dialog_runtime.c) once the menu has finished. */
#include "save_cards.h"
#include "save_menu.h"
#include "save_slots.h"
#include "pc/cards/cards.h"
#include "pc/mods/mods.h"

static unsigned token_of(int slot) { return slot >= 0 ? SaveSlots_Token(slot) : 0; }

static void set_tokens(void)
{
    unsigned live[SAVE_SLOT_COUNT];
    for (int slot = 0; slot < SAVE_SLOT_COUNT; slot++) live[slot] = SaveSlots_Token(slot);
    Cards_SetSlotTokens(token_of(SaveMenu_CurrentSlot()), live, SAVE_SLOT_COUNT);
}

/* A mod hears of it after the fact (mod_types.h, MEMORIES_EVENT_SLOT_*). */
static void tell_mods(unsigned type, unsigned sequence)
{
    int slot = SaveMenu_CurrentSlot();
    MemoriesModEvent event = {type, MEMORIES_AFTER, slot, (int)token_of(slot), (int)sequence, 0, 0};
    Mods_Dispatch(&event);
}

void SaveCards_Loaded(void) { set_tokens(); }

void SaveCards_Applied(const void *state)
{
    const unsigned char *bytes = state;
    tell_mods(MEMORIES_EVENT_SLOT_LOAD, bytes[0x404] | bytes[0x405] << 8 | bytes[0x406] << 16 | (unsigned)bytes[0x407] << 24);
}

void SaveCards_Saved(const void *state, unsigned sequence)
{
    set_tokens();
    Cards_SaveWritten(state, sequence);
    tell_mods(MEMORIES_EVENT_SLOT_SAVE, sequence);
}

void SaveCards_PairLoaded(void)
{
    set_tokens();
    Cards_SetPairTokens(token_of(SaveMenu_PairSlot(0)), token_of(SaveMenu_PairSlot(1)));
}
