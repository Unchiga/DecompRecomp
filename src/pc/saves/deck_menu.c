/* Game > Deck slots. See deck_menu.h. */
#include "deck_menu.h"
#include "pc/platform/menu.h"
#include "deck_slots.h"
#include "save_menu.h"
#include "save_slots.h"
#include "types.h"
#include "game/save_data.h"
#include "pc/cards/cards.h"
#include "pc/platform/paths.h"
#include "pc/platform/platform.h"
#include "pc/platform/settings.h"
#include "pc/guest/state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u8 D_8009B26C;            /* the active mode (main_mode_state.h) */
extern u8 gMain_bMenuID;         /* the main menu's entry: 0-4 the title's, 5-10 a loaded game's */
extern u32 D_801D9000[];         /* gText_adwGlyphCodeTable: glyph -> Shift-JIS, low half */
extern s32 gDuel_adwCardStats[]; /* ATK in bits 0-8 (x10), type in 26-30 */
void SD_SEPlayFull(u32 id);
u32 Text_LookupString(s32 bank, s32 id);
unsigned Memories_PresentedFrames(void);

/* Where the deck can change: nothing on these screens keeps a copy of it
 * (main_modes.h). Build Deck works on its own copy, a duel on its shuffle. */
#define MODE_CAMPAIGN 2
#define MODE_CAMPAIGN_MAP 5
#define MODE_FREE_DUEL 6
#define MODE_BUILD_DECK 7
#define MODE_MENU 8

/* The campaign's card shop (the only way to Build Deck in the present, which
 * has no map): Script_OpSavePrompt, scene-script command 13 in the low bits
 * of D_8009B27C (script_state.h). Its menu waits for a choice once opened
 * (0x4000) and while no other state of it runs: sliding in, the memory card,
 * the incomplete-deck notice, the title confirm, leaving, and their steps. */
extern u16 D_8009B27C;
#define SCRIPT_COMMAND_SHOP 13
#define SHOP_OPEN 0x4000u
#define SHOP_BUSY (0x2000u | 0x1000u | 0x0800u | 0x0400u | 0x0200u | 0x0080u)

/* Pad bits as Platform_Pad reports them (the controller's own order). */
#define PAD_START 0x0008u
#define PAD_UP 0x0010u
#define PAD_RIGHT 0x0020u
#define PAD_DOWN 0x0040u
#define PAD_LEFT 0x0080u
#define PAD_TRIANGLE 0x1000u
#define PAD_CIRCLE 0x2000u
#define PAD_CROSS 0x4000u
#define PAD_SQUARE 0x8000u

/* The save slot menu's sounds (save_menu.h). */
enum { SOUND_MOVE = 6, SOUND_CONFIRM = 7, SOUND_CANCEL = 8, SOUND_BUZZER = 9 };

enum { VIEW_CLOSED, VIEW_LIST, VIEW_CONFIRM, VIEW_MESSAGE };
enum { ASK_CLEAR };
#define MONSTER_TYPE_END 20 /* types 0-19 are monsters, then magic, trap, ritual, equip */

static struct {
    int view, cursor, top, ask, choice, close_after;
    char message[160];
    char title[64];
    int status[DECK_SLOT_COUNT], current[DECK_SLOT_COUNT];
    char label[DECK_SLOT_COUNT][96];
    unsigned changes;
} menu;

static int requested, allowed, holding, item_enabled = -1, shown_rows = DECK_SLOT_COUNT;
static unsigned last_poll = 0xffffff00u, previous_bits;

/* --- the decks: a draft beside the save -----------------------------------
 * The slots of the save being played (its duelist code) are a draft. Using
 * a deck, making one or clearing one changes it, and so does leaving Build
 * Deck: the active slot takes the deck the game wrote. The file is written
 * only when the game is saved and read again when a save is loaded, so the
 * decks go with the save (unsaved, both are lost together); a save state
 * holds the draft (DeckMenu_State). Which slot is active is not kept in the
 * file: it is the one holding the save's forty cards (reconcile). */
static struct {
    uint32_t code;   /* whose draft: the duelist code, 0 for none read */
    int32_t active;  /* the slot that is the deck, -1 none */
    int32_t dirty;   /* the file does not have it yet */
    DeckSlot slots[DECK_SLOT_COUNT];
} draft;
static unsigned seen_saves, seen_loads;

/* Build Deck asks for a deck as it is entered (DeckMenu_BuildDeckEntry). */
enum { PICK_NONE, PICK_OPEN, PICK_CHOSEN };
static int picking;
extern u8 D_8009B269; /* main_mode_state.h: where Build Deck returns to */


static SaveDataWorkspace *workspace(void) { return (SaveDataWorkspace *)D_801D0000; }
static int live(unsigned frame) { return frame - last_poll <= 2; }
static void changed(void) { menu.changes++; }

static unsigned char *trunk(void *context, int id)
{
    (void)context;
    return Cards_Valid(id) ? Cards_ChestSlot(&workspace()->state, id) : NULL;
}

/* Identities in the slot file only for the cards mods add, and only when
 * they read back as one field. */
static const char *identity(int id)
{
    const char *name = Cards_Identity(id);
    return name && *name && !strpbrk(name, ",\n\r") ? name : NULL;
}

static int game_loaded(void) { return workspace()->state.player_deck[0] != 0; }

static int screen_allowed(int where)
{
    int mode = D_8009B26C & 0x1F;
    /* The main menu shows Campaign, Free Duel... only for a loaded game; a
     * save left in the workspace by a jump to the title is not one. */
    if (where == DECK_MENU_TITLE_MENU || mode == MODE_MENU) return gMain_bMenuID >= 5;
    if (mode == MODE_CAMPAIGN) {
        unsigned state = D_8009B27C;
        return (state & 0x1F) == SCRIPT_COMMAND_SHOP && (state & SHOP_OPEN) && !(state & SHOP_BUSY);
    }
    /* Build Deck being entered, before it copies the deck (0x40 clear). */
    if (mode == MODE_BUILD_DECK) return picking == PICK_OPEN && !(D_8009B26C & 0x40);
    return mode == MODE_CAMPAIGN_MAP || mode == MODE_FREE_DUEL;
}

static void card_name(int id, char *out, size_t size)
{
    const unsigned char *text = Cards_NameText(id);
    size_t n = 0;
    if (!text) {
        /* The game's own lookup: the global bank, string 0x8000 + id. Not
         * its arithmetic here: at -O2 clang folds `(uintptr_t)D_801D5800 &
         * 0xFFFF0000` (a pinned symbol) to 0 and keeps only the offset. */
        text = (const unsigned char *)(uintptr_t)Text_LookupString(0, 0x8000 + Cards_BaseId(id));
    }
    for (; *text < 0xFE && n + 1 < size; text++) {
        unsigned code = D_801D9000[*text] & 0xFFFFu;
        out[n++] = code ? SaveSlots_Ascii(code) : ' ';
    }
    out[n] = 0;
}

static int card_type(int id) { return (int)((unsigned)gDuel_adwCardStats[id - 1] >> 26 & 0x1F); }
static int card_attack(int id) { return (int)((unsigned)gDuel_adwCardStats[id - 1] & 0x1FF) * 10; }

static void describe(int slot)
{
    const DeckSlot *kept = &draft.slots[slot];
    int i, best = 0, monsters = 0, card, count;
    char name[64];
    menu.current[slot] = slot == draft.active;
    menu.status[slot] = DeckSlots_Check(workspace()->state.player_deck, kept, trunk, NULL, &card, &count);
    menu.label[slot][0] = 0;
    if (!kept->used || menu.status[slot] == DECK_INVALID) return;
    for (i = 0; i < DECK_SLOT_CARDS; i++) {
        int id = kept->cards[i];
        if (card_type(id) >= MONSTER_TYPE_END) continue;
        monsters++;
        if (!best || card_attack(id) > card_attack(best)) best = id;
    }
    name[0] = 0;
    if (best) card_name(best, name, sizeof(name));
    snprintf(menu.label[slot], sizeof(menu.label[slot]), "%s%s%d monsters, %d other", name, best ? "   " : "",
             monsters, DECK_SLOT_CARDS - monsters);
}

static void refresh(void)
{
    int slot;
    for (slot = 0; slot < DECK_SLOT_COUNT; slot++) describe(slot);
    changed();
}

static void keep_cursor_shown(void)
{
    int rows = shown_rows < 1 ? 1 : shown_rows;
    if (menu.cursor < menu.top) menu.top = menu.cursor;
    if (menu.cursor >= menu.top + rows) menu.top = menu.cursor - rows + 1;
}

static void message(int close_after, const char *text)
{
    snprintf(menu.message, sizeof(menu.message), "%s", text);
    menu.close_after = close_after;
    menu.view = VIEW_MESSAGE;
    changed();
}

static int draft_path(uint32_t code, char *path, size_t size)
{
    char relative[64];
    snprintf(relative, sizeof(relative), "decks/%08X.txt", (unsigned)code);
    return Paths_User(path, size, relative);
}

static int deck_complete(void)
{
    int i;
    for (i = 0; i < DECK_SLOT_CARDS; i++) {
        if (!workspace()->state.player_deck[i]) return 0;
    }
    return 1;
}

/* The draft of the save being played, and its active slot: the one it
 * names if that still holds the deck, else the first that does. A deck in
 * no slot takes the first empty one. */
static void reconcile(void)
{
    const unsigned short *deck = workspace()->state.player_deck;
    uint32_t code = workspace()->state.duelist_code;
    char path[1024];
    int slot, skipped = 0;
    if (draft.code != code) {
        memset(&draft, 0, sizeof(draft));
        draft.code = code;
        draft.active = -1;
        if (!draft_path(code, path, sizeof(path))) {
            DeckSlots_Read(path, draft.slots, Cards_FindIdentity, &skipped);
            if (skipped) {
                fprintf(stderr, "memories-pc: %d line(s) of %s are not forty cards; those slots read as empty\n",
                        skipped, path);
            }
        }
    }
    if (draft.active >= 0 && draft.active < DECK_SLOT_COUNT && DeckSlots_Same(&draft.slots[draft.active], deck))
        return;
    draft.active = -1;
    for (slot = 0; slot < DECK_SLOT_COUNT; slot++) {
        if (DeckSlots_Same(&draft.slots[slot], deck)) {
            draft.active = slot;
            return;
        }
    }
    if (!deck_complete()) return;
    for (slot = 0; slot < DECK_SLOT_COUNT; slot++) {
        if (!draft.slots[slot].used) {
            draft.slots[slot].used = 1;
            memcpy(draft.slots[slot].cards, deck, sizeof(draft.slots[slot].cards));
            draft.active = slot;
            draft.dirty = 1;
            fprintf(stderr, "memories-pc: the deck goes in slot %d\n", slot + 1);
            return;
        }
    }
}

static int store(void)
{
    char path[1024], folder[1024], comment[128], name[16], *slash, *back;
    if (draft_path(draft.code, path, sizeof(path))) return -1;
    snprintf(folder, sizeof(folder), "%s", path);
    slash = strrchr(folder, '/');
    back = strrchr(folder, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) {
        *slash = 0;
        Paths_MakeDirs(folder);
    }
    SaveSlots_StateName((const unsigned char *)&workspace()->state, name, sizeof(name));
    snprintf(comment, sizeof(comment), "Deck slots of %s (duelist code %08X), kept by the PC port.",
             name[0] ? name : "(no name)", (unsigned)draft.code);
    if (DeckSlots_Write(path, draft.slots, identity, comment)) {
        fprintf(stderr, "memories-pc: cannot write %s\n", path);
        return -1;
    }
    return 0;
}

/* The save slot menu saved or loaded a game since the last frame. */
static void follow_saves(void)
{
    unsigned saves = SaveMenu_SaveCount(), loads = SaveMenu_LoadCount();
    if (loads != seen_loads) {
        seen_loads = loads;
        draft.code = 0; /* read again for the save loaded */
    }
    if (saves != seen_saves) {
        seen_saves = saves;
        if (!Settings_Get(SET_DECK_SLOTS) || !game_loaded()) return;
        reconcile();
        if (draft.dirty && !store()) {
            draft.dirty = 0;
            fprintf(stderr, "memories-pc: deck slots saved with the game\n");
        }
    }
}

void DeckMenu_State(MemoriesState *state)
{
    MemoriesStateField field = {&draft, sizeof(draft)};
    int loaded = Memories_StateChunk(state, "deck-slots", &field, 1);
    if (Memories_StateLoading(state)) {
        if (!loaded) draft.code = 0; /* older state: read the file again */
        else draft.dirty = 1; /* the file may have changed since this snapshot */
        seen_saves = SaveMenu_SaveCount();
        seen_loads = SaveMenu_LoadCount();
        picking = PICK_NONE;
        requested = allowed = holding = 0;
        previous_bits = 0;
        DeckMenu_Close();
    }
    changed();
}

int DeckMenu_BuildDeckEntry(void)
{
    /* A short deck cannot be put in a slot. Let the player repair it in
     * Build Deck directly instead of trapping them in the slot picker. */
    if (!Settings_Get(SET_DECK_SLOTS) || !game_loaded() || !deck_complete()) {
        picking = PICK_NONE;
        requested = 0;
        DeckMenu_Close();
        return 0;
    }
    if (picking == PICK_CHOSEN) {
        picking = PICK_NONE;
        return 0;
    }
    if (picking == PICK_NONE) {
        picking = PICK_OPEN;
        requested = 1;
    }
    return 1;
}

void DeckMenu_BuildDeckLeft(void)
{
    const unsigned short *deck = workspace()->state.player_deck;
    if (!Settings_Get(SET_DECK_SLOTS) || !game_loaded()) return;
    if (draft.code == (uint32_t)workspace()->state.duelist_code && draft.active >= 0 && draft.active < DECK_SLOT_COUNT && deck_complete()) {
        DeckSlot *slot = &draft.slots[draft.active];
        if (memcmp(slot->cards, deck, sizeof(slot->cards))) {
            memcpy(slot->cards, deck, sizeof(slot->cards));
            draft.dirty = 1;
            fprintf(stderr, "memories-pc: slot %d takes the deck Build Deck wrote\n", draft.active + 1);
        }
        return;
    }
    reconcile(); /* not picked here, or not forty cards: the slots stay */
}

static void cancel_pick(void)
{
    picking = PICK_NONE;
    D_8009B26C = D_8009B269; /* as Build Deck's own way out, already faded */
    DeckMenu_Close();
}

static void choose(void)
{
    picking = PICK_CHOSEN;
    DeckMenu_Close();
}

static void show(void)
{
    char name[16];
    previous_bits = Platform_Pad(0); /* a button already down is not a press */
    holding = 1;
    menu.top = 0;
    if (!allowed) {
        message(1, "Decks open on the main menu, the map, a card shop or Free Duel, and as you enter Build Deck.");
        return;
    }
    reconcile();
    SaveSlots_StateName((const unsigned char *)&workspace()->state, name, sizeof(name));
    snprintf(menu.title, sizeof(menu.title), picking == PICK_OPEN ? "Build Deck: which deck? (%s)" : "Decks: %s",
             name[0] ? name : "(no name)");
    refresh();
    menu.cursor = draft.active >= 0 ? draft.active : 0;
    keep_cursor_shown();
    menu.view = VIEW_LIST;
    changed();
}

void DeckMenu_Close(void)
{
    if (menu.view == VIEW_CLOSED) return;
    menu.view = VIEW_CLOSED;
    changed();
}

int DeckMenu_Active(void) { return menu.view != VIEW_CLOSED; }
int DeckMenu_HoldsPads(void) { return menu.view != VIEW_CLOSED || holding; }

void DeckMenu_Request(void)
{
    if (Settings_Get(SET_DECK_SLOTS)) requested = 1;
}

/* Cross on a slot: the active one; an empty one becomes a copy of the deck;
 * another deck is used, the one it replaces staying in its own slot. */
static void use(int slot)
{
    char text[160], name[64];
    int card, count, result;
    reconcile();
    if (slot == draft.active) {
        if (picking == PICK_OPEN) {
            SD_SEPlayFull(SOUND_CONFIRM);
            choose();
            return;
        }
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, "That is already your deck.");
        return;
    }
    if (draft.active < 0) {
        /* Every slot holds another deck, or the deck is not forty cards. */
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, deck_complete() ? "Your deck is in no slot and all ten are used: clear one to keep it."
                                   : "Your deck is not forty cards yet: finish it in Build Deck first.");
        return;
    }
    if (!draft.slots[slot].used) {
        draft.slots[slot].used = 1;
        memcpy(draft.slots[slot].cards, workspace()->state.player_deck, sizeof(draft.slots[slot].cards));
        draft.active = slot;
        draft.dirty = 1;
        SD_SEPlayFull(SOUND_CONFIRM);
        refresh();
        if (picking == PICK_OPEN) {
            choose();
            return;
        }
        snprintf(text, sizeof(text), "Slot %d is a new deck, a copy of yours. Build Deck changes it.", slot + 1);
        message(0, text);
        return;
    }
    result = DeckSlots_Check(workspace()->state.player_deck, &draft.slots[slot], trunk, NULL, &card, &count);
    name[0] = 0;
    if (card && Cards_Valid(card)) card_name(card, name, sizeof(name));
    if (result == DECK_MISSING) {
        snprintf(text, sizeof(text), "Missing %d x %s: not in your trunk or deck.", count, name);
    } else if (result == DECK_TRUNK_FULL) {
        snprintf(text, sizeof(text), "Your trunk has no room for %d more %s.", count, name);
    } else if (result != DECK_OK) {
        snprintf(text, sizeof(text), "Slot %d does not hold forty cards this game has, at most three of each.",
                 slot + 1);
    }
    if (result != DECK_OK) {
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, text);
        return;
    }
    DeckSlots_Use(workspace()->state.player_deck, &draft.slots[slot], trunk, NULL);
    draft.active = slot;
    fprintf(stderr, "memories-pc: deck slot %d is the deck now\n", slot + 1);
    SD_SEPlayFull(SOUND_CONFIRM);
    refresh();
    if (picking == PICK_OPEN) {
        choose();
        return;
    }
    snprintf(text, sizeof(text), "Your deck is now the one in slot %d.", slot + 1);
    message(0, text);
}

static void clear(int slot)
{
    draft.slots[slot].used = 0;
    draft.dirty = 1;
    SD_SEPlayFull(SOUND_CONFIRM);
    refresh();
    menu.view = VIEW_LIST;
}

static void ask(int what)
{
    menu.ask = what;
    menu.choice = 1; /* No */
    menu.view = VIEW_CONFIRM;
    SD_SEPlayFull(SOUND_CONFIRM);
    changed();
}

static void press(unsigned pressed)
{
    int slot = menu.cursor;
    switch (menu.view) {
    case VIEW_MESSAGE:
        if (!(pressed & (PAD_CROSS | PAD_CIRCLE | PAD_START))) return;
        SD_SEPlayFull(SOUND_CONFIRM);
        if (menu.close_after) DeckMenu_Close();
        else menu.view = VIEW_LIST;
        changed();
        return;
    case VIEW_CONFIRM:
        if (pressed & (PAD_LEFT | PAD_RIGHT)) {
            menu.choice ^= 1;
            SD_SEPlayFull(SOUND_MOVE);
            changed();
        } else if (pressed & PAD_CIRCLE || (pressed & (PAD_CROSS | PAD_START) && menu.choice)) {
            SD_SEPlayFull(SOUND_CANCEL);
            menu.view = VIEW_LIST;
            changed();
        } else if (pressed & (PAD_CROSS | PAD_START)) {
            clear(slot);
        }
        return;
    default:
        break;
    }
    if (pressed & (PAD_UP | PAD_DOWN)) {
        menu.cursor = (menu.cursor + (pressed & PAD_UP ? DECK_SLOT_COUNT - 1 : 1)) % DECK_SLOT_COUNT;
        keep_cursor_shown();
        SD_SEPlayFull(SOUND_MOVE);
        changed();
    } else if (pressed & (PAD_CIRCLE | PAD_START)) {
        SD_SEPlayFull(SOUND_CANCEL);
        if (picking == PICK_OPEN) cancel_pick();
        else DeckMenu_Close();
    } else if (pressed & PAD_CROSS) {
        use(slot);
    } else if (pressed & PAD_TRIANGLE) {
        /* The active deck is the game's: it cannot go. */
        if (draft.slots[slot].used && slot != draft.active) ask(ASK_CLEAR);
        else SD_SEPlayFull(SOUND_BUZZER);
    }
}

void DeckMenu_Poll(int where)
{
    unsigned bits, pressed;
    last_poll = Memories_PresentedFrames();
    allowed = Settings_Get(SET_DECK_SLOTS) && game_loaded() && !SaveMenu_Active() && screen_allowed(where);
    bits = Platform_Pad(0);
    pressed = bits & ~previous_bits;
    previous_bits = bits;
    if (requested) {
        requested = 0;
        if (menu.view == VIEW_CLOSED) {
            show();
            return;
        }
    }
    if (menu.view == VIEW_CLOSED) {
        /* The game sees the pad again once the buttons that closed the menu
         * are up, so it does not take them as its own presses. */
        if (!bits) holding = 0;
        /* The list Build Deck asked for, closed without a deck (Esc). */
        if (picking == PICK_OPEN) cancel_pick();
        return;
    }
    /* A save state loaded, or the screen changed, under an open list: the
     * deck may not change here any more. */
    if (!allowed && !(menu.view == VIEW_MESSAGE && menu.close_after)) {
        DeckMenu_Close();
        return;
    }
    press(pressed);
}

void DeckMenu_Frame(unsigned frame)
{
    static const char *at;
    int enabled;
    if (!at) at = getenv("MEMORIES_DECKS_AT") ? getenv("MEMORIES_DECKS_AT") : "";
    while (*at) {
        char *end;
        unsigned long when = strtoul(at, &end, 10);
        if (end == at) {
            at = "";
        } else if (frame >= when) {
            at = *end == ',' ? end + 1 : end;
            DeckMenu_Request();
        } else {
            break;
        }
    }
    follow_saves();
    if (!live(frame)) {
        /* Main_Loop is not running (the title's own loop, a jump to it, a
         * long disc wait): nothing can answer the screen, so it is not kept
         * open over the pads. */
        if (requested) {
            fprintf(stderr, "memories-pc: decks open on the main menu, the map, a card shop or Free Duel\n");
        }
        requested = 0;
        holding = 0;
        DeckMenu_Close();
    }
    enabled = Settings_Get(SET_DECK_SLOTS) && live(frame) && allowed;
    if (enabled != item_enabled) {
        item_enabled = enabled;
        Menu_SetItemEnabled(MENU_ITEM_DECKS, enabled);
    }
}

unsigned DeckMenu_Signature(void)
{
    return menu.view == VIEW_CLOSED ? 0 : menu.changes * 8u + (unsigned)menu.view * 2u + 1u;
}

/* Drawing, in the save slot menu's look (save_menu.c). */

#define COLOUR_TEXT 0xf2f2f4u
#define COLOUR_DIM 0x8a8a92u
#define COLOUR_WARN 0xf0a070u
#define COLOUR_TITLE 0xffd870u
#define COLOUR_CURRENT 0x90d890u

static int ui_scale;

static uint32_t blend(uint32_t under, uint32_t over, unsigned alpha)
{
    unsigned inverse = 255 - alpha;
    unsigned r = ((over >> 16 & 255) * alpha + (under >> 16 & 255) * inverse) / 255;
    unsigned g = ((over >> 8 & 255) * alpha + (under >> 8 & 255) * inverse) / 255;
    unsigned b = ((over & 255) * alpha + (under & 255) * inverse) / 255;
    unsigned a = alpha + (under >> 24) * inverse / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

static void fill(MenuCanvas *canvas, int x, int y, int w, int h, uint32_t colour, unsigned alpha)
{
    int row, column;
    for (row = y < 0 ? 0 : y; row < y + h && row < canvas->height; row++) {
        for (column = x < 0 ? 0 : x; column < x + w && column < canvas->width; column++) {
            uint32_t *pixel = canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)column;
            *pixel = blend(*pixel, colour, alpha);
        }
    }
}

static void frame_box(MenuCanvas *canvas, int x, int y, int w, int h, int s)
{
    fill(canvas, x, y, w, h, 0x0c0e18u, 232);
    fill(canvas, x, y, w, s, 0x6078c0u, 255);
    fill(canvas, x, y + h - s, w, s, 0x6078c0u, 255);
    fill(canvas, x, y, s, h, 0x6078c0u, 255);
    fill(canvas, x + w - s, y, s, h, 0x6078c0u, 255);
}

static void text(MenuCanvas *canvas, int x, int y, const char *line, uint32_t colour)
{
    Menu_DrawTextScaled(canvas, x, y, line, colour, ui_scale);
}

static int width(const char *line) { return Menu_TextWidthScaled(line, ui_scale); }

static void centred(MenuCanvas *canvas, int x, int w, int y, const char *line, uint32_t colour)
{
    text(canvas, x + (w - width(line)) / 2, y, line, colour);
}

static const char *status_text(int slot)
{
    if (!draft.slots[slot].used) return "Empty";
    if (menu.current[slot]) return "your deck";
    switch (menu.status[slot]) {
    case DECK_MISSING: return "cards missing";
    case DECK_TRUNK_FULL: return "trunk full";
    case DECK_INVALID: return "not a valid deck";
    default: return "";
    }
}

void DeckMenu_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int s, row_h, rows, pw, ph, px, py, i, list_y;
    char line[160];
    *x = *y = *w = *h = 0;
    if (menu.view == VIEW_CLOSED || !canvas || !canvas->pixels) return;
    s = canvas->height / 420 > Menu_Scale() ? canvas->height / 420 : Menu_Scale();
    ui_scale = s;
    row_h = 22 * s;
    rows = (canvas->height - Menu_Height() - 96 * s) / row_h;
    rows = rows < 3 ? 3 : rows > DECK_SLOT_COUNT ? DECK_SLOT_COUNT : rows;
    if (rows != shown_rows) {
        shown_rows = rows;
        keep_cursor_shown();
    }
    pw = canvas->width - 16 * s < 640 * s ? canvas->width - 16 * s : 640 * s;
    ph = 44 * s + rows * row_h + 34 * s;
    px = (canvas->width - pw) / 2;
    py = Menu_Height() + (canvas->height - Menu_Height() - ph) / 2;
    frame_box(canvas, px, py, pw, ph, s);
    if (menu.view == VIEW_MESSAGE && menu.close_after) {
        /* Opened where it cannot be used: only the message. */
        centred(canvas, px, pw, py + ph / 2, menu.message, COLOUR_TEXT);
        centred(canvas, px, pw, py + ph / 2 + 22 * s, "Press Cross", COLOUR_DIM);
        *x = px, *y = py, *w = pw, *h = ph;
        return;
    }
    text(canvas, px + 14 * s, py + 20 * s, menu.title, COLOUR_TITLE);
    list_y = py + 38 * s;
    for (i = 0; i < rows && menu.top + i < DECK_SLOT_COUNT; i++) {
        int slot = menu.top + i, ry = list_y + i * row_h, cy = ry + row_h / 2;
        const char *right = status_text(slot);
        uint32_t colour = draft.slots[slot].used ? COLOUR_TEXT : COLOUR_DIM;
        uint32_t right_colour = menu.current[slot] ? COLOUR_CURRENT
                                : draft.slots[slot].used && menu.status[slot] != DECK_OK ? COLOUR_WARN : colour;
        if (slot == menu.cursor) fill(canvas, px + 6 * s, ry, pw - 12 * s, row_h - 2 * s, 0x3a5aa8u, 200);
        snprintf(line, sizeof(line), "%2d   %s", slot + 1, menu.label[slot]);
        text(canvas, px + 16 * s, cy, line, colour);
        text(canvas, px + pw - 16 * s - width(right), cy, right, right_colour);
    }
    if (menu.top > 0) text(canvas, px + pw - 30 * s, py + 20 * s, "^", COLOUR_DIM);
    if (menu.top + rows < DECK_SLOT_COUNT) text(canvas, px + pw - 18 * s, py + 20 * s, "v", COLOUR_DIM);
    text(canvas, px + 14 * s, py + ph - 16 * s,
         picking == PICK_OPEN ? "Cross: edit this deck (an empty slot: a copy of yours)   Triangle: clear   Circle: back"
                              : "Cross: use (an empty slot: a copy of yours)   Triangle: clear   Circle: close",
         COLOUR_DIM);
    if (draft.dirty) {
        /* Kept with the game: lost with it when it is not saved. */
        const char *note = "saved with the game";
        text(canvas, px + pw - 40 * s - width(note), py + 20 * s, note, COLOUR_WARN);
    }
    if (menu.view == VIEW_CONFIRM) {
        int bw = 120 * s, bh = 108 * s, bx0 = px + 24 * s, by = py + (ph - bh) / 2, cw = pw - 48 * s, bx;
        const char *labels[2] = {"Yes", "No"};
        frame_box(canvas, bx0, by, cw, bh, s);
        snprintf(line, sizeof(line), "Clear slot %d?", menu.cursor + 1);
        centred(canvas, bx0, cw, by + 30 * s, line, COLOUR_TEXT);
        bx = bx0 + (cw - 2 * bw - 16 * s) / 2;
        for (i = 0; i < 2; i++) {
            int chosen = i == menu.choice, left = bx + i * (bw + 16 * s);
            fill(canvas, left, by + 62 * s, bw, 24 * s, chosen ? 0x3a5aa8u : 0x22263au, 255);
            centred(canvas, left, bw, by + 74 * s, labels[i], chosen ? COLOUR_TEXT : COLOUR_DIM);
        }
    } else if (menu.view == VIEW_MESSAGE) {
        int mw = pw - 96 * s, mh = 64 * s, mx = px + 48 * s, my = py + (ph - mh) / 2;
        frame_box(canvas, mx, my, mw, mh, s);
        centred(canvas, mx, mw, my + 24 * s, menu.message, COLOUR_TEXT);
        centred(canvas, mx, mw, my + 46 * s, "Press Cross", COLOUR_DIM);
    }
    *x = px;
    *y = py;
    *w = pw;
    *h = ph;
}
