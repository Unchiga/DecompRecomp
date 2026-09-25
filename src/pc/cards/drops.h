#ifndef MEMORIES_PC_DROPS_H
#define MEMORIES_PC_DROPS_H
/* Game > Card drops (SET_CARD_DROPS): a won duel deals 1 to 99 cards from
 * the opponent's drop table instead of the console's one, and RESULTS OF
 * DUEL gains pages listing the ones past the first (notes/card-drops.md).
 *
 * The first card is the game's own: SPOILS shows it and the game awards it.
 * The others are listed on the added pages only, never that first copy, and
 * are awarded just before it when the player leaves the screen. */
#include "types.h"

#define CARD_DROPS_MAX 99
#define CARD_DROPS_PER_PAGE 7          /* one per plate of the SPECIAL ARTS page */
#define CARD_DROPS_FIRST_PAGE 3        /* result pages 0-2 are the game's */
#define CARD_DROPS_TEXT_ID 0xFFFF      /* the string id the added pages show */
#define CARD_DROPS_TEXT_SIZE 1024

/* The cards dealt past the first, kept with the game's own variables
 * (src/pc/game/drops.c) so a state saved on the results screen still
 * has them. */
typedef struct {
    s16 count;                         /* entries in `cards` */
    s16 page;                          /* the added page `text` holds */
    u16 cards[CARD_DROPS_MAX - 1];     /* in the order they were dealt */
    u8 fresh[CARD_DROPS_MAX - 1];      /* the player had none of it before the duel */
    u8 text[CARD_DROPS_TEXT_SIZE];     /* that page, in the game's text codes */
} CardDropsState;
extern CardDropsState gCardDrops;

/* The results screen opens: nothing dealt yet. */
void CardDrops_Begin(void);
/* The duel's drop from table `pool`, as Duel_SelectCardDrop: the card
 * SPOILS shows. With more than one card set, the others are dealt first
 * and kept for the added pages. */
int CardDrops_Roll(int pool);
/* The player leaves the screen with a save: award the kept cards (the game
 * then awards the first). */
void CardDrops_Award(void);
/* The result page one step left (-1) or right (+1) of `page`: SPOILS, the
 * added pages, then the game's two statistics pages. */
int CardDrops_TurnPage(int page, int step);
/* Compose added page `page` (CARD_DROPS_FIRST_PAGE on) for the text box. */
void CardDrops_ComposePage(int page);
/* Text_Resolve's question: the composed page for CARD_DROPS_TEXT_ID. */
const unsigned char *CardDrops_Text(int id);
#endif
