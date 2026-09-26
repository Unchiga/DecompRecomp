#include "pc/saves/deck_menu.h"
#include "pc/platform/settings.h"
#include "pc/guest/state.h"
#include "types.h"
#include <assert.h>
#include <string.h>

u8 D_8009B26C, gDialog_bChoiceEnabled;
u16 D_8009B27C;
s8 gDialog_bChoiceCount, gDialog_bChoice;
static int enabled = 1, translated, remapped;
static unsigned char text[128];

int Settings_Get(SettingId id) { assert(id == SET_DECK_SLOTS); return enabled; }
int Text_Overridden(int id) { assert(id == 0x11); return translated; }
const unsigned char *Text_CompileOwn(const char *listing, int id, size_t *size)
{
    assert(id == 0x11 && strstr(listing, "DECK SLOTS"));
    *size = sizeof(text);
    return text;
}
struct MemoriesState { int loading, present; unsigned char data[12]; };
int Memories_StateLoading(const MemoriesState *state) { return state->loading; }
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    size_t i, at = 0;
    assert(!strcmp(tag, "deck-shop") && count == 3);
    if (state->loading && !state->present) return 0;
    for (i = 0; i < count; i++) {
        assert(at + fields[i].size <= sizeof(state->data));
        if (state->loading) memcpy(fields[i].data, state->data + at, fields[i].size);
        else memcpy(state->data + at, fields[i].data, fields[i].size);
        at += fields[i].size;
    }
    state->present = 1;
    return state->loading;
}
void Memories_StateRemapRange(MemoriesState *state, uint32_t from, uint32_t to, uint32_t size)
{
    assert(state->loading && from && to == (uint32_t)(uintptr_t)text && size == sizeof(text));
    remapped++;
}

int main(void)
{
    MemoriesState five = {0}, four = {0}, old = {1, 0, {0}};
    D_8009B26C = 2;
    D_8009B27C = 0xc00d;
    assert(DeckMenu_ShopMenu());
    assert(DeckMenu_Text(0x11) == text);
    assert(DeckMenu_ShopChoice(2) == DECK_MENU_SHOP_SLOTS);
    assert(DeckMenu_ShopChoice(3) == 2 && DeckMenu_ShopChoice(4) == 3);
    gDialog_bChoiceCount = 4;
    gDialog_bChoiceEnabled = 15;
    gDialog_bChoice = 2;
    DeckMenu_ShopRestore();
    assert(gDialog_bChoiceCount == 5 && gDialog_bChoice == 3 && gDialog_bChoiceEnabled == 31);
    gDialog_bChoiceEnabled = 3; /* Return to Title's Yes/No prompt */
    gDialog_bChoice = 2;
    DeckMenu_ShopRestore();
    assert(gDialog_bChoice == 3 && gDialog_bChoiceEnabled == 31);
    DeckMenu_State(&five);

    enabled = 0;
    assert(!DeckMenu_ShopMenu());
    DeckMenu_State(&four);
    five.loading = 1;
    DeckMenu_State(&five);
    /* The saved menu controls numbering, regardless of the current setting
     * or the most recently visited shop. */
    assert(DeckMenu_ShopChoice(2) == DECK_MENU_SHOP_SLOTS && remapped == 1);
    four.loading = 1;
    DeckMenu_State(&four);
    assert(DeckMenu_ShopChoice(2) == 2 && DeckMenu_Text(0x11) == 0);
    enabled = 1;
    assert(DeckMenu_ShopMenu());
    DeckMenu_State(&old);
    assert(DeckMenu_ShopChoice(2) == 2);
    translated = 1;
    assert(!DeckMenu_ShopMenu());
    return 0;
}
