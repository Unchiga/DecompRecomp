#ifdef MEMORIES_PC
#include "pc/mods/mods.h"
#endif
#ifdef MEMORIES_PC
#include "pc/mods/mods.h"
#endif
#include "../types.h"
#include "duel_side_state.h"
#include "duel_grid.h"
#include "../psyq/stdio.h"

#include "card_constants.h"
#include "duel_action_lock.h"
#include "duel_effect_request.h"
#include "duel_card.h"
#include "duel_card_record_lifecycle.h"
#include "duel_effect_tables.h"

#include "duel_magic_effect_format.h"
#include "duel_magic_effect_dispatch.h"
#ifdef MEMORIES_PC
#include "pc/cards/cards.h"
#endif

void DuelEffect_ApplyHarpiesFeatherDuster(void)
{
    int i;

    if (!DuelEffect_MarkInitialized()) {
        DuelEffectRequest *object = DuelEffect_CreateRequest(0x17);
        int side = D_8009B1D5 ^ 1;

        object->field_04 = D_80090800[side][0].y;
        printf(D_8009AF40, (short)object->field_04);
    } else {
        for (i = 0; i < DUEL_FIELD_ROW_SIZE; i++) {
            int position =
                i + D_8009B1D5 * DUEL_FIELD_SIDE_GRID_SLOT_COUNT;
            DuelCardRecord *entry =
                &D_801A7AD8[D_800907D8[position]];

            if (entry->flags & DUEL_CARD_FLAG_OCCUPIED)
                DuelCard_RemoveFromField(entry);
        }
        gDuel_wCardEffectFlags = 0;
    }
}

#ifdef MEMORIES_PC
static int DuelEffect_UpdateCardEffectRetail(void)
#else
int DuelEffect_UpdateCardEffect(void)
#endif
{
    u16 flags = gDuel_wCardEffectFlags;

    if (flags & DUEL_CARD_EFFECT_FLAG_ACTIVE) {
        u8 *indices = gDuelEffect_abGroupByEffectId;
        DuelEffectHandler *callbacks;
        int index = indices[gDuel_sCardEffectIndex] * DUEL_CARD_EFFECT_HANDLERS_PER_GROUP;

        if (flags & DUEL_CARD_EFFECT_FLAG_SECOND_HANDLER)
            index++;
        callbacks = gDuelEffect_apfnGroupHandler;
        callbacks[index]();
    }
    return gDuel_wCardEffectFlags;
}

#ifdef MEMORIES_PC
static void DuelEffect_StartCardEffectRetail(int value, int flag)
#else
void DuelEffect_StartCardEffect(int value, int flag)
#endif
{
    int index;

#ifdef MEMORIES_PC
    /* A card past the disc's has its base's effect, and the handlers that
       test gDuel_wEffectCardID for a particular card see the base. */
    if (value > 0) {
        value = Cards_EffectId(value);
    }
#endif

    if (((unsigned)(value - DUEL_EFFECT_FIRST_BLOCK_CARD_ID) <
         DUEL_EFFECT_CARD_BLOCK_SIZE) ||
        ((unsigned)(value - DUEL_EFFECT_SECOND_BLOCK_CARD_ID) <
         DUEL_EFFECT_CARD_BLOCK_SIZE) ||
        value == DUEL_DARK_MAGIC_RITUAL_CARD_ID) {
        if (value < DUEL_EFFECT_SECOND_BLOCK_CARD_ID) {
            index = value - DUEL_EFFECT_FIRST_BLOCK_CARD_ID;
        } else {
            index = value -
                (DUEL_EFFECT_SECOND_BLOCK_CARD_ID -
                 DUEL_EFFECT_CARD_BLOCK_SIZE);
            if (value == DUEL_DARK_MAGIC_RITUAL_CARD_ID) {
                index = DUEL_DARK_MAGIC_RITUAL_EFFECT_INDEX;
            }
        }
        gDuel_sCardEffectIndex = index;
        gDuel_wEffectCardID = value;
        gDuel_wCardEffectFlags = DUEL_CARD_EFFECT_FLAG_ACTIVE;
        if (flag) {
            gDuel_wCardEffectFlags =
                DUEL_CARD_EFFECT_FLAG_ACTIVE |
                DUEL_CARD_EFFECT_FLAG_SECOND_HANDLER;
        }
    }
}

#ifdef MEMORIES_PC
void DuelEffect_StartCardEffect(int value, int flag)
{
    MemoriesModEvent event = {MEMORIES_EVENT_EFFECT, MEMORIES_BEFORE, 0, 0, 0, 0, 0};
    event.a = value; event.b = flag;
    Mods_Dispatch(&event);

    if (!event.handled) { DuelEffect_StartCardEffectRetail(event.a, event.b); }
    event.phase = MEMORIES_AFTER; Mods_Dispatch(&event);

}
#endif

#ifdef MEMORIES_PC
int DuelEffect_UpdateCardEffect(void)
{
    MemoriesModEvent event = {MEMORIES_EVENT_EFFECT, MEMORIES_BEFORE, 0, 0, 0, 0, 0};
    event.a = gDuel_wEffectCardID; event.b = gDuel_wCardEffectFlags; event.c = 1;
    Mods_Dispatch(&event);

    if (!event.handled) { event.result = DuelEffect_UpdateCardEffectRetail(); }
    event.phase = MEMORIES_AFTER; Mods_Dispatch(&event);
    return event.result;
}
#endif
