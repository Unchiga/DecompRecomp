#ifndef MEMORIES_DECOMP_FUNC_800323F8_H
#define MEMORIES_DECOMP_FUNC_800323F8_H

#include "../types.h"

/* Sets up the Build Deck screen over the workspace at `base`: `deck` is the
 * first pane's deck (forty card ids, then the 722 chest counts at +0x50),
 * `other` the second pane's deck or 0, and `flags` the byte kept at +0x6343.
 * Two callers: Main_RunBuildDeckMenu and Main_RunDuel. */
void func_800323F8(u8 *base, void *deck, s32 other, s32 flags);

/* The workspace both callers pass: the buffer the console lends it, except
 * on the PC port, whose two panes have room for more cards
 * (card_constants.h) and live in src/pc/game/card_storage.c. */
#ifdef MEMORIES_PC
extern u8 gBuildDeck_aWorkspace[];
#define BUILD_DECK_WORKSPACE(console) gBuildDeck_aWorkspace
#else
#define BUILD_DECK_WORKSPACE(console) (console)
#endif

#endif
