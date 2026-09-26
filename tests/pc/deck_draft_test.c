/* Exercise the draft transitions with a simulated save menu and slot file.
 * Including the controller lets these tests drive its otherwise private
 * save notification boundary without a window or retail game image. */
#include "pc/saves/deck_menu.c"
#include <assert.h>

u8 D_801D0000[sizeof(SaveDataWorkspace)];
/* Rendering and input are outside these draft tests. */
u8 D_8009B269, D_8009B26C, D_8009B26E, gMain_bMenuID, gDuel_bEffectState;
u16 D_8009B27C;
BuildDeckTransitionState *gBuildDeck_pState; /* controlled by the transition tests below */
u32 D_801D9000[1];
s32 gDuel_adwCardStats[1];
unsigned Memories_PresentedFrames(void) { return 0; }
int SaveMenu_Active(void) { return 0; }
uint16_t Platform_Pad(int port) { (void)port; return 0; }
void SD_SEPlayFull(u32 id) { (void)id; }
void Menu_SetItemEnabled(int id, int enabled) { (void)id; (void)enabled; }
int Menu_Scale(void) { return 1; }
int Menu_Height(void) { return 26; }
void Menu_DrawTextScaled(MenuCanvas *canvas, int x, int y, const char *text, uint32_t colour, int scale)
{ (void)canvas; (void)x; (void)y; (void)text; (void)colour; (void)scale; assert(0); }
int Menu_TextWidthScaled(const char *text, int scale) { (void)text; (void)scale; assert(0); return 0; }
int Cards_Valid(int id) { (void)id; assert(0); return 0; }
unsigned char *Cards_ChestSlot(void *state, int id) { (void)state; (void)id; assert(0); return NULL; }
const unsigned char *Cards_NameText(int id) { (void)id; assert(0); return NULL; }
int Cards_BaseId(int id) { return id; }
u32 Text_LookupString(s32 bank, s32 id) { (void)bank; (void)id; assert(0); return 0; }
char SaveSlots_Ascii(unsigned sjis) { (void)sjis; return '?'; }
int DeckSlots_Check(const unsigned short *current, const DeckSlot *want, DeckTrunkFn trunk,
                    void *context, int *card, int *count)
{ (void)current; (void)want; (void)trunk; (void)context; (void)card; (void)count; assert(0); return DECK_INVALID; }
void DeckSlots_Use(unsigned short *current, const DeckSlot *want, DeckTrunkFn trunk, void *context)
{ (void)current; (void)want; (void)trunk; (void)context; assert(0); }

static int enabled = 1, writes;
static unsigned saves, loads;
static DeckSlot disk[DECK_SLOT_COUNT];
int Settings_Get(SettingId id) { assert(id == SET_DECK_SLOTS); return enabled; }
unsigned SaveMenu_SaveCount(void) { return saves; }
unsigned SaveMenu_LoadCount(void) { return loads; }
int Cards_FindIdentity(const char *name) { (void)name; return 0; }
const char *Cards_Identity(int id) { (void)id; return NULL; }
int Paths_User(char *out, size_t size, const char *relative)
{ snprintf(out, size, "test/%s", relative); return 0; }
int Paths_MakeDirs(const char *path) { (void)path; return 0; }
void SaveSlots_StateName(const unsigned char *state, char *out, size_t size)
{ (void)state; snprintf(out, size, "Test"); }
int DeckSlots_Read(const char *path, DeckSlot *slots, DeckFindFn find, int *skipped)
{ (void)path; (void)find; *skipped = 0; memcpy(slots, disk, sizeof(disk)); return 1; }
int DeckSlots_Write(const char *path, const DeckSlot *slots, DeckIdentityFn identity, const char *comment)
{ (void)path; (void)identity; (void)comment; memcpy(disk, slots, sizeof(disk)); writes++; return 0; }
int DeckSlots_Same(const DeckSlot *slot, const unsigned short *deck)
{ return slot->used && !memcmp(slot->cards, deck, sizeof(slot->cards)); }

struct MemoriesState { int loading, present; unsigned char data[sizeof(draft)]; };
int Memories_StateLoading(const MemoriesState *state) { return state->loading; }
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    assert(!strcmp(tag, "deck-slots") && count == 1 && fields[0].size == sizeof(draft));
    if (state->loading) {
        if (!state->present) return 0;
        memcpy(fields[0].data, state->data, sizeof(draft));
        return 1;
    }
    state->present = 1;
    memcpy(state->data, fields[0].data, sizeof(draft));
    return 0;
}

int main(void)
{
    MemoriesState snapshot = {0}, old = {1, 0, {0}};
    unsigned short *deck = workspace()->state.player_deck;
    int i;
    workspace()->state.duelist_code = 123;
    for (i = 0; i < DECK_SLOT_CARDS; i++) deck[i] = (unsigned short)(i + 1);
    reconcile();
    assert(draft.active == 0 && draft.dirty && writes == 0);
    saves++;
    follow_saves();
    assert(writes == 1 && !draft.dirty);
    DeckMenu_State(&snapshot); /* a clean draft when this state was taken */

    deck[0] = 100;
    DeckMenu_BuildDeckLeft();
    assert(draft.slots[0].cards[0] == 100 && draft.dirty && writes == 1);
    saves++;
    follow_saves();
    assert(writes == 2 && disk[0].cards[0] == 100);

    /* Restoring the older state must put its draft back on the next save,
     * even though that draft was clean at the time of the snapshot. */
    deck[0] = 1;
    snapshot.loading = 1;
    picking = PICK_OPEN;
    requested = holding = 1;
    menu.view = VIEW_LIST;
    DeckMenu_State(&snapshot);
    assert(draft.dirty && picking == PICK_NONE && !requested && !DeckMenu_Active());
    saves++;
    follow_saves();
    assert(writes == 3 && disk[0].cards[0] == 1);

    /* The picker may not trap a player who left Build Deck with 39 cards. */
    deck[39] = 0;
    assert(!DeckMenu_BuildDeckEntry());
    deck[39] = 40;
    assert(DeckMenu_BuildDeckEntry());
    DeckMenu_State(&old);
    assert(draft.code == 0 && picking == PICK_NONE);
    enabled = 0;
    assert(!DeckMenu_BuildDeckEntry());
    /* Idle guards and returning through the retail exit to the list. */
    {
        static BuildDeckTransitionState screen;
        enabled = 1; gBuildDeck_pState = &screen; D_8009B26C = 0x47;
        screen.state = 2;
        assert(build_deck_idle());
        screen.state = 3; assert(build_deck_idle());
        screen.state |= 0x4000; assert(!build_deck_idle());
        screen.state = 2; screen.transition_ticks = 1; assert(!build_deck_idle());
        screen.transition_ticks = 0; gDuel_bEffectState = 1; assert(!build_deck_idle());
        gDuel_bEffectState = 0;
        picking = PICK_NONE; menu.view = VIEW_CLOSED; requested = 1;
        D_8009B269 = MODE_CAMPAIGN;
        DeckMenu_Poll(DECK_MENU_MAIN_LOOP);
        assert(screen.state == 4 && screen.next_state == 2 && list_after_build_deck);
        D_8009B26C = MODE_CAMPAIGN;
        DeckMenu_BuildDeckLeft();
        assert(D_8009B26C == MODE_BUILD_DECK && D_8009B269 == MODE_CAMPAIGN && !list_after_build_deck);
        /* Incomplete-deck EXIT returns to its caller, never to the picker. */
        deck[39] = 0; list_after_build_deck = 1; D_8009B26C = MODE_CAMPAIGN;
        DeckMenu_BuildDeckLeft(); assert(D_8009B26C == MODE_CAMPAIGN && !list_after_build_deck);
        /* Choosing BUILD DECK in the notice cancels the queued list. */
        D_8009B26C = 0x47; screen.state = 3; list_after_build_deck = 1;
        DeckMenu_Poll(DECK_MENU_MAIN_LOOP); assert(!list_after_build_deck);
        list_after_build_deck = 1; DeckMenu_State(&snapshot); assert(!list_after_build_deck);
    }
    return 0;
}
