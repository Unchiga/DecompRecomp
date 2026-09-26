/* The campaign shop's added DECK SLOTS entry (deck_menu.h). */
#include "deck_menu.h"
#include "pc/platform/settings.h"
#include "pc/text/text.h"
#include "pc/guest/state.h"
#include "types.h"
#include <stdint.h>
#include <stdio.h>

extern u8 D_8009B26C;
extern u16 D_8009B27C;
extern u8 gDialog_bChoiceEnabled;
extern s8 gDialog_bChoiceCount, gDialog_bChoice;
#define MODE_CAMPAIGN 2
#define SCRIPT_COMMAND_SHOP 13

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
static size_t shop_text_size;
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
        text = Text_CompileOwn(shop_listing, SHOP_MENU_TEXT, &shop_text_size);
        if (!text) fprintf(stderr, "memories-pc: the card shop's menu with DECK SLOTS did not compile\n");
    }
    return text;
}

/* Restore this before the game image: its text streams contain pointers
 * into the compiled listing, whose heap address changes between sessions. */
void DeckMenu_ShopState(MemoriesState *state)
{
    uint32_t base = shop_text_size ? (uint32_t)(uintptr_t)shop_text() : 0;
    uint32_t size = (uint32_t)shop_text_size;
    MemoriesStateField fields[] = {{&shop_extra, sizeof(shop_extra)}, {&base, sizeof(base)}, {&size, sizeof(size)}};
    if (Memories_StateLoading(state)) shop_extra = 0; /* older states had four entries */
    if (Memories_StateChunk(state, "deck-shop", fields, 3) && base && size) {
        const unsigned char *text = shop_text();
        if (text && size == shop_text_size) {
            Memories_StateRemapRange(state, base, (uint32_t)(uintptr_t)text, size);
        }
    }
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
    if (!shop_extra) return;
    /* Nested memory-card and title prompts overwrite the global enabled
     * mask. All five entries in this menu are enabled, as in its listing. */
    gDialog_bChoiceCount = 5;
    if (gDialog_bChoice >= SHOP_MENU_SLOTS) gDialog_bChoice++;
    gDialog_bChoiceEnabled = 0x1F;
}

