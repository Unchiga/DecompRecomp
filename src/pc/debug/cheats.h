#ifndef MEMORIES_PC_DEBUG_CHEATS_H
#define MEMORIES_PC_DEBUG_CHEATS_H
/* Development helpers behind the window's Game > Cheats menu. They write the game's
 * own save workspace, so a save made afterwards keeps the result. */

/* Put `count` copies of every card in the chest (the trunk), capped at
 * the game's own limit. Takes effect at once; open BUILD DECK to see it. */
void Cheats_GiveAllCards(int count);
/* Unlock every CPU opponent in the live save. Reopen Free Duel to refresh
 * its portraits and selection grid; save normally to keep the unlocks. */
void Cheats_UnlockAllFreeDuelists(void);
/* Once a frame: MEMORIES_DEBUG_CHEST=N gives N of every card, and
 * MEMORIES_DEBUG_DECK="723-762" (ids and ranges, repeated to forty) sets the
 * deck, the first time a save is live in the workspace. */
void Cheats_Frame(void);
#endif
