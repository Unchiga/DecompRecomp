/* Game > Deck slots. See deck_menu.h. */
#include "deck_menu.h"
#include "pc/platform/menu.h"
#include "deck_slots.h"
#include "save_menu.h"
#include "save_slots.h"
#include "types.h"
#include "game/save_data.h"
#include "game/build_deck_transition_state.h"
#include "pc/cards/cards.h"
#include "pc/platform/paths.h"
#include "pc/platform/platform.h"
#include "pc/platform/settings.h"
#include "pc/text/text.h"
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
extern u8 gDuel_bEffectState; /* duel_effect.h: the card viewer and the like */
void SD_BGMFadeOut(void);
void Fade_WaitOut(void);

/* The campaign's card shop (the only way to Build Deck in the present, which
 * has no map): Script_OpSavePrompt, scene-script command 13 in the low bits
 * of D_8009B27C (script_state.h). Its menu waits for a choice once opened
 * (0x4000) and while no other state of it runs: sliding in, the memory card,
 * the incomplete-deck notice, the title confirm, leaving, and their steps. */
extern u16 D_8009B27C;
extern u8 gDialog_bChoiceEnabled; /* dialog_choice.h: the menu on screen */
extern s8 gDialog_bChoiceCount, gDialog_bChoice;
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
enum { ASK_SAVE, ASK_CLEAR, ASK_DISCARD };
#define MONSTER_TYPE_END 20 /* types 0-19 are monsters, then magic, trap, ritual, equip */

static struct {
    int view, cursor, top, ask, choice, close_after;
    char message[160];
    char title[64];
    char path[1024];
    DeckSlot slots[DECK_SLOT_COUNT];
    int status[DECK_SLOT_COUNT], current[DECK_SLOT_COUNT];
    char label[DECK_SLOT_COUNT][96];
    unsigned changes;
} menu;

static int requested, allowed, holding, item_enabled = -1, shown_rows = DECK_SLOT_COUNT;
static unsigned last_poll = 0xffffff00u, previous_bits;

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

/* Build Deck, set up (0x40): its step table (duel_transition_step_table.c)
 * waits for input in steps 2 and 3, one per pane; not while the not-ready
 * confirm (0x4000), a pane's slide or an effect (the card viewer) runs. */
static int build_deck_idle(void)
{
    const BuildDeckTransitionState *screen = gBuildDeck_pState;
    unsigned step;
    if ((D_8009B26C & 0x1F) != MODE_BUILD_DECK || !(D_8009B26C & 0x40) || !screen) return 0;
    step = screen->state & 0x3F;
    return (step == 2 || step == 3) && !(screen->state & 0x4000) && screen->transition_ticks == 0 &&
           gDuel_bEffectState == 0;
}

/* Whether Build Deck holds changes it has not written yet. It works on a
 * copy and writes the trunk and the deck back as it closes (func_800339D0),
 * so while a trunk count differs from the save's, a card has moved. */
static int build_deck_staged(void)
{
    const BuildDeckTransitionState *screen = gBuildDeck_pState;
    int id;
    for (id = CARD_ID_FIRST; id <= CARD_COUNT_LIVE; id++) {
        if (screen->chest_card_quantities[id] != *Cards_ChestSlot(screen->deck_cards, id)) return 1;
    }
    return 0;
}

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
    if (mode == MODE_BUILD_DECK) return build_deck_idle();
    return mode == MODE_CAMPAIGN_MAP || mode == MODE_FREE_DUEL;
}

/* A deck used in Build Deck: the screen shows its copy of the old one, so
 * it is made again from the save, as entering it does. Once the list is
 * closed and its buttons are up (none of them reaches the new screen), the
 * way out's music and fade (Main_RunBuildDeckMenu), then the mode published
 * again: Main_Loop resets the screen's objects and effects
 * (Main_ResetFrontendRuntime) and the mode's runner sets it up from the
 * save (func_800323F8). Where it returns to (D_8009B269) and the shop's
 * narrow dialogs (D_8009B2F8) are left as they were. */
static int reopen_build_deck;

static void reopen(void)
{
    reopen_build_deck = 0;
    if ((D_8009B26C & 0x1F) != MODE_BUILD_DECK || !(D_8009B26C & 0x40)) return;
    fprintf(stderr, "memories-pc: Build Deck made again from the save for the deck used\n");
    SD_BGMFadeOut();
    Fade_WaitOut();
    D_8009B26C = MODE_BUILD_DECK;
}

/* The card shop's menu, string 0x11, with DECK SLOTS under BUILD DECK: the
 * retail listing's lines (tools/pc/text_listing.py) and one more, as wide as
 * BUILD DECK and indented as it is. {choice} offers five and {choose} jumps
 * as retail's do. The game keeps four entries' worth of enabled bits
 * (Text_HandleChoiceCommand), so DeckMenu_ShopRestore sets the fifth. */
#define SHOP_MENU_TEXT 0x11
#define SHOP_MENU_SLOTS 2
static const char shop_listing[] = "@bank dialog\n"
                                   "\n"
                                   "[0011]\n"
                                   "{choice 4D 9F}{f8 02 2C}SAVE\n"
                                   "{f8 02 14}BUILD DECK\n"
                                   "{f8 02 14}DECK SLOTS\n"
                                   "RETURN TO TITLE\n"
                                   "{f8 02 14}LEAVE SHOP\n"
                                   "{choose 80 0 0 0 0 0}\n";
static int shop_extra; /* the shop's menu on screen has DECK SLOTS */

static int in_shop(void)
{
    return (D_8009B26C & 0x1F) == MODE_CAMPAIGN && (D_8009B27C & 0x1F) == SCRIPT_COMMAND_SHOP;
}

static const unsigned char *shop_text(void)
{
    static const unsigned char *text;
    static int tried;
    if (!tried) {
        tried = 1;
        text = Text_CompileOwn(shop_listing, SHOP_MENU_TEXT);
        if (!text) fprintf(stderr, "memories-pc: the card shop's menu with DECK SLOTS did not compile\n");
    }
    return text;
}

int DeckMenu_ShopMenu(void)
{
    shop_extra = Settings_Get(SET_DECK_SLOTS) && !Text_Overridden(SHOP_MENU_TEXT) && shop_text();
    return shop_extra;
}

const unsigned char *DeckMenu_Text(int id)
{
    return id == SHOP_MENU_TEXT && shop_extra && in_shop() ? shop_text() : NULL;
}

int DeckMenu_ShopChoice(int choice)
{
    if (!shop_extra) return choice;
    if (choice == SHOP_MENU_SLOTS) return DECK_MENU_SHOP_SLOTS;
    return choice > SHOP_MENU_SLOTS ? choice - 1 : choice;
}

void DeckMenu_ShopRestore(void)
{
    unsigned enabled = gDialog_bChoiceEnabled;
    if (!shop_extra) return;
    /* The game's entries from 2 on move down one; the new one is on. */
    gDialog_bChoiceCount = 5;
    if (gDialog_bChoice >= SHOP_MENU_SLOTS) gDialog_bChoice++;
    gDialog_bChoiceEnabled = (u8)((enabled & 3u) | 1u << SHOP_MENU_SLOTS | (enabled & 0xCu) << 1);
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
    const DeckSlot *kept = &menu.slots[slot];
    int i, best = 0, monsters = 0, card, count;
    char name[64];
    menu.current[slot] = DeckSlots_Same(kept, workspace()->state.player_deck);
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

static void show(void)
{
    char relative[64], name[16];
    int skipped, slot;
    previous_bits = Platform_Pad(0); /* a button already down is not a press */
    holding = 1;
    menu.top = 0;
    if (!allowed) {
        message(1, "Deck slots open on the main menu, the map, a card shop, Build Deck or Free Duel, with a game loaded.");
        return;
    }
    snprintf(relative, sizeof(relative), "decks/%08X.txt", (unsigned)workspace()->state.duelist_code);
    if (Paths_User(menu.path, sizeof(menu.path), relative)) {
        message(1, "The deck slot file has no place in the user folder.");
        return;
    }
    DeckSlots_Read(menu.path, menu.slots, Cards_FindIdentity, &skipped);
    if (skipped) fprintf(stderr, "memories-pc: %d line(s) of %s are not forty cards; those slots read as empty\n",
                         skipped, menu.path);
    SaveSlots_StateName((const unsigned char *)&workspace()->state, name, sizeof(name));
    snprintf(menu.title, sizeof(menu.title), "Deck slots: %s", name[0] ? name : "(no name)");
    refresh();
    menu.cursor = 0;
    for (slot = 0; slot < DECK_SLOT_COUNT; slot++) {
        if (menu.current[slot]) {
            menu.cursor = slot;
            break;
        }
    }
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

static int store(void)
{
    char folder[1024], comment[128], name[16], *slash, *back;
    snprintf(folder, sizeof(folder), "%s", menu.path);
    slash = strrchr(folder, '/');
    back = strrchr(folder, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) {
        *slash = 0;
        Paths_MakeDirs(folder);
    }
    SaveSlots_StateName((const unsigned char *)&workspace()->state, name, sizeof(name));
    snprintf(comment, sizeof(comment), "Deck slots of %s (duelist code %08X), kept by the PC port.",
             name[0] ? name : "(no name)", (unsigned)workspace()->state.duelist_code);
    if (DeckSlots_Write(menu.path, menu.slots, identity, comment)) {
        fprintf(stderr, "memories-pc: cannot write %s\n", menu.path);
        return -1;
    }
    return 0;
}

static void ask(int what);

/* `discard`: the player said the cards moved in Build Deck may go back. */
static void use(int slot, int discard)
{
    char text[160], name[64];
    int card, count, result;
    if (!menu.slots[slot].used) {
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, "This slot is empty. Square keeps your current deck in it.");
        return;
    }
    /* In Build Deck with cards moved, the save still has the deck from before
     * them: using it is putting them back. */
    if (menu.current[slot] && !(build_deck_idle() && build_deck_staged())) {
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, "That is already your deck.");
        return;
    }
    result = DeckSlots_Check(workspace()->state.player_deck, &menu.slots[slot], trunk, NULL, &card, &count);
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
    if (build_deck_idle() && build_deck_staged() && !discard) {
        ask(ASK_DISCARD);
        return;
    }
    if (!menu.current[slot]) DeckSlots_Use(workspace()->state.player_deck, &menu.slots[slot], trunk, NULL);
    fprintf(stderr, "memories-pc: deck slot %d is the deck now\n", slot + 1);
    SD_SEPlayFull(SOUND_CONFIRM);
    refresh();
    if (build_deck_idle()) {
        reopen_build_deck = 1;
        snprintf(text, sizeof(text), "Your deck is now the one in slot %d. Build Deck opens again with it.",
                 slot + 1);
        message(1, text);
        return;
    }
    snprintf(text, sizeof(text), "Your deck is now the one in slot %d.", slot + 1);
    message(0, text);
}

static void keep(int slot)
{
    char text[96];
    DeckSlot before = menu.slots[slot];
    if (build_deck_idle() && build_deck_staged()) {
        /* The save still has the deck from before the cards moved. */
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, "Build Deck writes the cards you moved as you leave it: leave it first to keep this deck.");
        return;
    }
    menu.slots[slot].used = 1;
    memcpy(menu.slots[slot].cards, workspace()->state.player_deck, sizeof(menu.slots[slot].cards));
    if (store()) {
        menu.slots[slot] = before;
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, "The deck slot file could not be written.");
        return;
    }
    SD_SEPlayFull(SOUND_CONFIRM);
    refresh();
    snprintf(text, sizeof(text), "Your deck is kept in slot %d.", slot + 1);
    message(0, text);
}

static void clear(int slot)
{
    DeckSlot before = menu.slots[slot];
    menu.slots[slot].used = 0;
    if (store()) {
        menu.slots[slot] = before;
        SD_SEPlayFull(SOUND_BUZZER);
        message(0, "The deck slot file could not be written.");
        return;
    }
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
            if (menu.ask == ASK_SAVE) keep(slot);
            else if (menu.ask == ASK_DISCARD) use(slot, 1);
            else clear(slot);
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
        DeckMenu_Close();
    } else if (pressed & PAD_CROSS) {
        use(slot, 0);
    } else if (pressed & PAD_SQUARE) {
        /* Ask before replacing a different deck; keeping the same one again
         * (in its new order) needs no question. */
        if (menu.slots[slot].used && !menu.current[slot]) ask(ASK_SAVE);
        else keep(slot);
    } else if (pressed & PAD_TRIANGLE) {
        if (menu.slots[slot].used) ask(ASK_CLEAR);
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
        if (reopen_build_deck && !holding) reopen();
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
    if (!live(frame)) {
        /* Main_Loop is not running (the title's own loop, a jump to it, a
         * long disc wait): nothing can answer the screen, so it is not kept
         * open over the pads. */
        if (requested) fprintf(stderr, "memories-pc: deck slots open on the main menu, the map, a card shop, Build Deck or Free Duel\n");
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
    if (!menu.slots[slot].used) return "Empty";
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
        uint32_t colour = menu.slots[slot].used ? COLOUR_TEXT : COLOUR_DIM;
        uint32_t right_colour = menu.current[slot] ? COLOUR_CURRENT
                                : menu.slots[slot].used && menu.status[slot] != DECK_OK ? COLOUR_WARN : colour;
        if (slot == menu.cursor) fill(canvas, px + 6 * s, ry, pw - 12 * s, row_h - 2 * s, 0x3a5aa8u, 200);
        snprintf(line, sizeof(line), "%2d   %s", slot + 1, menu.label[slot]);
        text(canvas, px + 16 * s, cy, line, colour);
        text(canvas, px + pw - 16 * s - width(right), cy, right, right_colour);
    }
    if (menu.top > 0) text(canvas, px + pw - 30 * s, py + 20 * s, "^", COLOUR_DIM);
    if (menu.top + rows < DECK_SLOT_COUNT) text(canvas, px + pw - 18 * s, py + 20 * s, "v", COLOUR_DIM);
    text(canvas, px + 14 * s, py + ph - 16 * s,
         "Cross: use   Square: keep your deck here   Triangle: clear   Circle: close", COLOUR_DIM);
    if (menu.view == VIEW_CONFIRM) {
        int bw = 120 * s, bh = 108 * s, bx0 = px + 24 * s, by = py + (ph - bh) / 2, cw = pw - 48 * s, bx;
        const char *labels[2] = {"Yes", "No"};
        frame_box(canvas, bx0, by, cw, bh, s);
        snprintf(line, sizeof(line),
                 menu.ask == ASK_SAVE      ? "Slot %d holds another deck. Replace it with yours?"
                 : menu.ask == ASK_DISCARD ? "Put back the cards you moved in Build Deck and use slot %d?"
                                           : "Clear slot %d?",
                 menu.cursor + 1);
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
