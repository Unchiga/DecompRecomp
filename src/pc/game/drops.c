/* Game > Card drops' record of the cards a duel dealt past the first
 * (src/pc/cards/drops.h). A game unit, like card_storage.c, so the record
 * is in the game's data: a state saved on the results screen keeps the
 * pages and the cards still to be awarded. Game units link in name order;
 * this one sorts after card_storage.c so that no existing variable moves,
 * which would keep states from earlier builds from loading. */
#include "types.h"
#include "pc/cards/drops.h"

CardDropsState gCardDrops = {0};
