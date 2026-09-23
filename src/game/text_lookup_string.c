#include "../types.h"
#include "text_constants.h"
#ifdef MEMORIES_PC
#include "pc/cards/cards.h"
#endif

#ifdef MEMORIES_PC
/* The PC port's Text_LookupString follows: the few strings that spell out
   how many cards there are are its own (cards.h). */
static u32 Text_LookupStringRetail(s32 arg0, s32 arg1)
#else
u32 Text_LookupString(s32 arg0, s32 arg1)
#endif
{
    s32 index = arg1;
    if (index > 0xCFFF)
        return ((u32)D_801C0000 & TEXT_BANK_ADDRESS_MASK) |
            D_801C0000[index - 0xD000];
    if (index > (TEXT_GLOBAL_STRING_ID_BASE - 1))
        return ((u32)D_801D5800 & TEXT_BANK_ADDRESS_MASK) |
            D_801D5800[index - TEXT_GLOBAL_STRING_ID_BASE];
    if (index >= 0x500)
        index -= 0x100;
    return ((u32)D_801B0000 & TEXT_BANK_ADDRESS_MASK) | D_801C0000[index];
}

#ifdef MEMORIES_PC
u32 Text_LookupString(s32 arg0, s32 arg1)
{
    return (u32)Cards_Text((const u8 *)Text_LookupStringRetail(arg0, arg1));
}
#endif
