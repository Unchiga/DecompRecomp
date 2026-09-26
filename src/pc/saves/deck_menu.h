#ifndef MEMORIES_PC_DECK_MENU_H
#define MEMORIES_PC_DECK_MENU_H
/* Game > Deck slots (F6): keep up to ten decks beside a save and switch to
 * one in a step (deck_slots.h has the rules and the file). The screen is
 * drawn in the port's overlay, like the save slot menu, and read with the
 * pad or the keys mapped to it; while it is open the game gets no buttons.
 * It opens with a game loaded, on the main menu (the one with Campaign), the
 * campaign map, a card shop's menu or Free Duel's opponent select, where
 * nothing holds a copy of the deck; the setting `deck_slots` (Game menu)
 * turns it off. */
struct MenuCanvas; /* pc/platform/menu.h */

/* The menu item and F6. */
void DeckMenu_Request(void);
/* Esc while it is open closes it instead of the game. */
int DeckMenu_Active(void);
void DeckMenu_Close(void);
/* Whether the game's pads read as released: while the screen is open, and
 * until the buttons that closed it are let go. */
int DeckMenu_HoldsPads(void);
/* Every presented frame (libetc.c): the menu item's state, and
 * MEMORIES_DECKS_AT=N[,N...] requests at frames, for checks. */
void DeckMenu_Frame(unsigned frame);
/* Opens, reads the pad and changes the deck, once a frame at a point where no
 * screen is half way through a step: Main_Loop between two mode runners
 * (DECK_MENU_MAIN_LOOP), and the title's own loop before its menu updates
 * (DECK_MENU_TITLE_MENU), which is where the menu with Campaign runs until
 * the player first leaves it. */
enum { DECK_MENU_MAIN_LOOP, DECK_MENU_TITLE_MENU };
void DeckMenu_Poll(int where);

void DeckMenu_Draw(struct MenuCanvas *canvas, int *x, int *y, int *w, int *h);
unsigned DeckMenu_Signature(void);

/* The campaign's card shop (Script_OpSavePrompt) offers DECK SLOTS under
 * BUILD DECK while the setting is on, unless a translation rewrites the
 * shop's menu. DeckMenu_ShopMenu decides it as the menu is made (1: five
 * entries); the menu's text then comes from DeckMenu_Text. The game's code
 * numbers the entries as its own four: DeckMenu_ShopChoice turns the
 * cursor into that numbering (DECK_MENU_SHOP_SLOTS for the new one), and
 * DeckMenu_ShopRestore puts the new entry back wherever the game sets the
 * menu's choices up (after its text, the memory card and the title
 * confirm). */
enum { DECK_MENU_SHOP_SLOTS = 0x10 };
int DeckMenu_ShopMenu(void);
int DeckMenu_ShopChoice(int choice);
void DeckMenu_ShopRestore(void);
/* String `id` as the shop's menu has it, or NULL (Text_Resolve). */
const unsigned char *DeckMenu_Text(int id);

/* The decks are a draft kept with the save: written to the file when the
 * game is saved (the save slot menu), read again when one is loaded, and
 * held in save states. One slot is active, the one holding the save's deck.
 * Main_RunBuildDeckMenu asks DeckMenu_BuildDeckEntry before it copies the
 * deck: while it returns 1 the list is up to pick the deck to edit, which
 * becomes the active one (Circle goes back where Build Deck was entered
 * from). As it leaves, DeckMenu_BuildDeckLeft puts the deck it wrote in
 * the active slot, when it is forty cards. */
int DeckMenu_BuildDeckEntry(void);
void DeckMenu_BuildDeckLeft(void);
/* The same for the chest Main_RunDuel opens before a campaign duel (its
 * step 0), only once F6 in that chest asked for it: Circle on the list keeps
 * the deck, and DeckMenu_DuelChestLeft
 * returns 1 when F6 in the chest asked for the list again (step 0 anew). */
int DeckMenu_DuelChestEntry(void);
int DeckMenu_DuelChestLeft(void);
struct MemoriesState;
void DeckMenu_State(struct MemoriesState *state);
#endif
