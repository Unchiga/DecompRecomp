#ifndef MEMORIES_PC_DECK_MENU_H
#define MEMORIES_PC_DECK_MENU_H
/* Game > Deck slots (F6): keep up to ten decks beside a save and switch to
 * one in a step (deck_slots.h has the rules and the file). The screen is
 * drawn in the port's overlay, like the save slot menu, and read with the
 * pad or the keys mapped to it; while it is open the game gets no buttons.
 * It opens with a game loaded, on the main menu (the one with Campaign), the
 * campaign map or Free Duel's opponent select, where nothing holds a copy of
 * the deck; the setting `deck_slots` (Game menu) turns it off. */
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
#endif
