#ifndef MEMORIES_PC_DEBUG_CHEATS_H
#define MEMORIES_PC_DEBUG_CHEATS_H
/* Development helpers behind the window's Debug menu. They write the game's
 * own save workspace, so a save made afterwards keeps the result. */

/* Put `count` copies of every card in the chest (the trunk), capped at
 * the game's own limit. Takes effect at once; open BUILD DECK to see it. */
void Cheats_GiveAllCards(int count);
/* Once a frame: MEMORIES_DEBUG_CHEST=N gives N of every card the first
 * time a save is live in the workspace. */
void Cheats_Frame(void);
#endif
