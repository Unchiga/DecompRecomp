#ifndef MEMORIES_PC_SAVE_CARDS_H
#define MEMORIES_PC_SAVE_CARDS_H
/* See save_cards.c. */
/* A load from the menu finished: the game's own Cards_SaveLoaded follows,
 * and once the game holds the loaded save (`state`), SaveCards_Applied. */
void SaveCards_Loaded(void);
void SaveCards_Applied(const void *state);
/* A save to the menu's slot finished; `state` and `sequence` are what it
 * wrote. */
void SaveCards_Saved(const void *state, unsigned sequence);
/* One side of a two-player load finished. */
void SaveCards_PairLoaded(void);
#endif
