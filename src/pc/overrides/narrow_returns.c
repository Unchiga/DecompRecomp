/* Two functions whose matching definitions return less than some of their
 * callers read (tools: an IR comparison of declared and defined return
 * types over src/game and the linked overlays found only these):
 *
 *   Ai_GetHandSize        defined s8, read as s32 by ai_script_control_flow.c
 *                         (the declaration there saves the retail build two
 *                         sign extensions)
 *   MemCard_FindLoadedEntry  defined void, read as s32 by func_8003DC1C.c,
 *                         which takes MemCard_FindEntry's index left in $v0
 *
 * MIPS returns a byte already extended in $v0, and GCC's x86 code happens to
 * extend it too; clang leaves the upper 24 bits of %eax undefined, so on
 * Windows the AI's hand range ended wherever the pointer bits said and the
 * opponent's turn ran off guest RAM. These native versions return the full
 * sign-extended word, which serves callers of either declaration. */
#define AI_GET_HAND_SIZE_RETURNS_S32          /* the callers' declarations */
#define MEM_CARD_FIND_LOADED_ENTRY_RESULT_VIEW
#include "types.h"
#include "game/ai_opponent_data.h"
#include "game/mem_card_directory.h"

/* This replaces a game function and must retain API 4's hook entry layout. */
__attribute__((patchable_function_entry(8, 6), noinline))
s32 Ai_GetHandSize(void)
{
    return (s8)gDuel_aOpponentData[gDuel_bOpponentID].values[0];
}

s32 MemCard_FindLoadedEntry(u8 *name)
{
    return MemCard_FindEntry(name, gMemCard_pDirEntries, gMemCard_nDirEntries);
}
