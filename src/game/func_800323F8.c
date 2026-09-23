/*
 * func_800323F8 sets up the Build Deck screen: it initialises the two
 * 0x6344-byte deck records (recent-drop ranks, the 40-card deck list with
 * its stats, and the 722-card chest list with its counts), sorts both lists,
 * then creates the screen's eleven display objects. It stands in its own
 * file because it is measured under gcc_2_8_1_g8_split_no_strength_reduce;
 * its neighbours use gcc_2_8_1_g0_split and gcc_2_8_1_g8.
 *
 * Levers measured on this body:
 * - -fno-strength-reduce: the target's loops keep explicit counters and
 *   unbiased cursors, so the drop table is walked backwards by hand and each
 *   list has its own record cursor;
 * - one counter `j` serves the rank loop and both list loops, and the rank
 *   loop's inner index reuses `n`, the chest total. Those shared lifetimes
 *   are what order the global allocation of $a2-$t1; the two list heads are
 *   separate names (`deck_list`, `chest_list`) for the same reason;
 * - the card-stat address is built as v--, v *= 4, v += all_stats after the
 *   store of v, and `v` is reused for the chest count cursor, which keeps
 *   both in $a0;
 * - `icons` and `all_stats` are locals set in that order ahead of `lists`,
 *   which is the order retail materialises $fp, $s7 and $s0 in;
 * - the final row count goes through the s32 local `rows`. loop.c merges
 *   two loads of one constant only when the first is at least as wide as the
 *   second, so an SImode 0x2D2 after the HImode one stays unmerged, neither
 *   load is worth hoisting alone, and $s7 stays with the card stats.
 *
 * The list and pane fields are written through CardList and
 * BuildDeckTransitionState, whose layouts are asserted in their headers. Each
 * conversion was measured on its own and in groups: the object is unchanged.
 * Two rules came out of that, and both cost an object to learn:
 * - the cast goes inline at the access. Naming a `CardList *` for the pane and
 *   writing the fields through it is one pseudo more than this body has room
 *   for, and it changes the allocation;
 * - the base pointer has to stay the one the access already uses. `chest_total`
 *   and `deck_total` are therefore still raw: they sit at +0x5A9C and +0x5AA0
 *   of the record, `lists` is the record plus 4, and no struct has `lists` as
 *   its base, so reaching them as members means addressing through `state`
 *   instead -- which moves the store to another register. Spelling either of
 *   them as a member changes the object on its own.
 */
#define GRAPHICS_VIEWPORT_IN_DATA
#include "../types.h"
#include "graphics_frame.h"
#include "duel_reward_setup.h"
#include "duel_card.h"
#include "build_deck_transition_state.h"
#include "build_deck_card_counts.h"
#include "card_list_sort.h"
#include "func_80031874.h"
#include "display_object_core.h"
#include "display_object_helpers.h"
#include "sound.h"
#include "card_type_icon_table.h"
#include "func_800323F8.h"
#ifdef MEMORIES_PC
#include "pc/cards/cards.h"
#endif

#define CARD_LIST_VIEW(list) ((CardList *)(list))
#define DISPLAY_OBJECT_VIEW(object) ((DisplayObject *)(object))
#define BUILD_DECK_TRANSITION_STATE_VIEW(state) \
    ((BuildDeckTransitionState *)(state))
#define CARD_STAT_WORD(record) (*(s32 *)(record))

void func_800323F8(u8 *base, void *deck, s32 other, s32 flags)
{
    u8 *state;
    u8 *lists;
    u8 *object;
    u8 *label;
    u8 *deck_list;
    u8 *chest_list;
    u8 *entry;
    u8 *quantity;
    u8 *counts;
    u8 *icon;
    u16 *drops;
    u16 *cards;
    u16 *drop;
    u8 *rank;
    u8 *rank_row;
    s32 *all_stats;
    s32 icons;
    s32 rows;
    s32 *stats;
    s32 pane;
    s32 j;
    s32 icon_addr;
    s32 id;
    s32 on;
    s32 held;
    s32 deck_total;
    s32 n;
    s32 v;

    func_80032328();
    SD_BGMPlay(0x70E0);
    gGraphics_sViewportY = 0;
    gGraphics_sViewportX = 0;
    state = base;
    func_80032370();
    pane = 0;
    on = 1;
    icons = (s32)D_80090DD8;
    all_stats = gDuel_adwCardStats;
    lists = state + 4;
    gBuildDeck_pState = BUILD_DECK_TRANSITION_STATE_VIEW(state);
#ifdef MEMORIES_PC
    /* The same through the workspace's members. The PC port's lists and card
       tables are longer than the console offsets above (card_constants.h),
       it has CARD_COUNT_LIVE cards, and the trunk of a card past the disc's
       is kept outside the save (Cards_ChestSlot). */
    BUILD_DECK_TRANSITION_STATE_VIEW(state)[0].pad_6343 = flags;
    BUILD_DECK_TRANSITION_STATE_VIEW(state)[0].deck_cards = deck;
    BUILD_DECK_TRANSITION_STATE_VIEW(state)[1].deck_cards = (u16 *)other;
    BUILD_DECK_TRANSITION_STATE_VIEW(state)[0].pane_index = 0;
    BUILD_DECK_TRANSITION_STATE_VIEW(state)[1].pane_index = 0;
    for (pane = 0; pane < 2; pane++) {
        BuildDeckTransitionState *ws = BUILD_DECK_TRANSITION_STATE_VIEW(state) + pane;
        CardList *chest = &ws->lists[0];
        CardList *decks = &ws->lists[1];

        if (ws->deck_cards == 0) {
            continue;
        }
        drops = (u16 *)gDuel_awRecentCardDrops;
        ws->card_sort_rank[0] = 0;
        for (j = 1; j < CARD_ID_END_LIVE; j++) {
            ws->card_sort_rank[j] = 0;
            for (n = 15; n >= 0; n--) {
                if ((s16)drops[n] == j) {
                    ws->card_sort_rank[j] = n + 1;
                }
            }
        }
        deck_total = 0;
        decks->kind = on;
        decks->first_target = 0;
        decks->first = 0;
        decks->cursor = 0;
        decks->sort_choice = 0;
        decks->sort_mode = ((u8 *)(decks->kind * 16 + icons))[1] & 0xF;
        cards = ws->deck_cards;
        for (j = 0; j < DECK_SIZE; j++, cards++) {
            entry = (u8 *)&decks->entries[j] + 8;
            entry[5] = 0;
            *(s16 *)(entry - 4) = 0;
            v = *cards;
            if (v != 0) {
                stats = &all_stats[v - 1];
                *(s16 *)(entry - 4) = v;
                entry[5] = on;
                entry[2] = (*stats >> 26) & 0x1F;
                *(s16 *)(entry - 2) = (*stats & 0x1FF) * 10;
                deck_total++;
                *(s16 *)entry = ((*stats >> 9) & 0x1FF) * 10;
            }
        }
        decks->entries[DECK_SIZE].id = 0xFFFF;
        ws->deck_total = deck_total;
        decks->row_count = DECK_SIZE;
        decks->sort_row_count = DECK_SIZE;
        func_80032C48(decks);
        func_8003201C(ws);
        n = 0;
        held = 0x80;
        chest->kind = 0;
        chest->first_target = 0;
        chest->first = 0;
        chest->row_count = CARD_COUNT_LIVE;
        chest->cursor = 0;
        chest->sort_choice = 0;
        chest->sort_mode = ((u8 *)(chest->kind * 16 + icons))[1] & 0xF;
        stats = all_stats;
        for (j = 0; j < CARD_COUNT_LIVE; stats++, j++) {
            id = j + 1;
            entry = (u8 *)&chest->entries[j] + 0xD;
            quantity = Cards_ChestSlot(ws->deck_cards, id);
            entry[0] = 0;
            *(s16 *)(entry - 9) = id;
            entry[-3] = (*stats >> 26) & 0x1F;
            *(s16 *)(entry - 7) = (*stats & 0x1FF) * 10;
            *(s16 *)(entry - 5) = ((*stats >> 9) & 0x1FF) * 10;
            ws->chest_card_quantities[id] = *quantity;
            if (*quantity != 0) {
                entry[0] = on;
                n += *quantity;
            } else if (ws->deck_card_quantities[id] != 0) {
                entry[0] = held;
            }
        }
        chest->entries[CARD_COUNT_LIVE].id = 0;
        ws->chest_total = n;
        chest->sort_row_count = CARD_COUNT_LIVE;
        chest->row_count = CARD_COUNT_LIVE;
        func_80032C48(chest);
    }
#else
    state[0x6343] = flags;
    *(void **)state = deck;
    *(s32 *)(state + 0x6344) = other;
    BUILD_DECK_TRANSITION_STATE_VIEW(state)->pane_index = 0;
    state[0xC686] = 0;
    do {
        if (*(s32 *)state != 0) {
            drops = (u16 *)gDuel_awRecentCardDrops;
            lists[0x6066] = 0;
            j = 1;
            rank = state + j;
            do {
                rank[0x606A] = 0;
                n = 15;
                rank_row = rank;
                drop = drops + 15;
                do {
                    if ((s16)*drop == j) {
                        rank_row[0x606A] = n + 1;
                    }
                    n--;
                    drop--;
                } while (n >= 0);
                j++;
                rank++;
            } while (j < 0x2D3);
            deck_list = state + 0x2D50;
            deck_total = 0;
            CARD_LIST_VIEW(lists)[1].kind = on;
            j = deck_total;
            CARD_LIST_VIEW(lists)[1].first_target = 0;
            CARD_LIST_VIEW(lists)[1].first = 0;
            CARD_LIST_VIEW(lists)[1].cursor = 0;
            CARD_LIST_VIEW(lists)[1].sort_choice = 0;
            icon = (u8 *)(CARD_LIST_VIEW(lists)[1].kind * 16 + icons);
            CARD_LIST_VIEW(lists)[1].sort_mode = icon[1] & 0xF;
            entry = state + 0x2D58;
            cards = *(u16 **)state;
            for (; j < 0x28; j++, entry += 0x10, cards++) {
                entry[5] = 0;
                *(s16 *)(entry - 4) = 0;
                v = *cards;
                if (v != 0) {
                    *(s16 *)(entry - 4) = v;
                    v--;
                    v *= 4;
                    v += (s32)all_stats;
                    entry[5] = on;
                    entry[2] = (CARD_STAT_WORD(v) >> 26) & 0x1F;
                    *(s16 *)(entry - 2) = (CARD_STAT_WORD(v) & 0x1FF) * 10;
                    deck_total++;
                    *(s16 *)entry =
                        ((CARD_STAT_WORD(v) >> 9) & 0x1FF) * 10;
                }
            }
            *(s16 *)(deck_list + 0x284) = -1;
            *(s32 *)(lists + 0x5A9C) = deck_total;
            CARD_LIST_VIEW(lists)[1].row_count = 0x28;
            CARD_LIST_VIEW(lists)[1].sort_row_count = 0x28;
            func_80032C48(CARD_LIST_VIEW(state + 0x2D50));
            func_8003201C(BUILD_DECK_TRANSITION_STATE_VIEW(state));
            chest_list = lists;
            n = 0;
            j = n;
            held = 0x80;
            CARD_LIST_VIEW(lists)->kind = 0;
            CARD_LIST_VIEW(lists)->first_target = 0;
            CARD_LIST_VIEW(lists)->first = 0;
            CARD_LIST_VIEW(lists)->row_count = 0x2D2;
            CARD_LIST_VIEW(lists)->cursor = 0;
            CARD_LIST_VIEW(lists)->sort_choice = 0;
            icon_addr = CARD_LIST_VIEW(lists)->kind * 16;
            icon_addr += icons;
            CARD_LIST_VIEW(lists)->sort_mode = ((u8 *)icon_addr)[1] & 0xF;
            entry = lists + 0xD;
            stats = all_stats;
            quantity = *(u8 **)state + 0x50;
            for (; j < 0x2D2; stats++, j++, entry += 0x10, quantity++) {
                id = j + 1;
                entry[0] = 0;
                *(s16 *)(entry - 9) = id;
                entry[-3] = (*stats >> 26) & 0x1F;
                *(s16 *)(entry - 7) = (*stats & 0x1FF) * 10;
                *(s16 *)(entry - 5) = ((*stats >> 9) & 0x1FF) * 10;
                v = (s32)state + id;
                counts = (u8 *)v;
                counts[0x5D97] = *quantity;
                if (*quantity != 0) {
                    entry[0] = on;
                    n += *quantity;
                } else if (counts[0x5AC4] != 0) {
                    entry[0] = held;
                }
            }
            *(s16 *)(chest_list + 0x2D24) = 0;
            *(s32 *)(lists + 0x5A98) = n;
            rows = 0x2D2;
            CARD_LIST_VIEW(lists)->sort_row_count = rows;
            CARD_LIST_VIEW(lists)->row_count = rows;
            func_80032C48(CARD_LIST_VIEW(lists));
        }
        pane++;
        lists += 0x6344;
        state += 0x6344;
    } while (pane < 2);
#endif

    state = (u8 *)gBuildDeck_pState;
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0, 0, 0, 4, 0, 0xC, 0x208);
    label = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 6);
    label[0x67] = 0;
    *(s32 *)(label + 0x30) = *(s32 *)(object + 0x30);
    *(s32 *)(label + 0x4C) = (s32)func_80031874;
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x140, 0, 0, 4, 1, 0xC, 0x208);
    label = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 6);
    label[0x67] = 1;
    *(s32 *)(label + 0x30) = *(s32 *)(object + 0x30);
    *(s32 *)(label + 0x4C) = (s32)func_80031874;
    BUILD_DECK_TRANSITION_STATE_VIEW(state)->state = 2;
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x136, 0x29, 0, 4, 0xC, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), 8);
#ifdef MEMORIES_PC
    BUILD_DECK_TRANSITION_STATE_VIEW(state)->lists[0].scroll_box = DISPLAY_OBJECT_VIEW(object);
#else
    *(u8 **)(state + 0x2D3C) = object;
#endif
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x26A, 0x29, 0, 4, 0xC, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), 8);
#ifdef MEMORIES_PC
    BUILD_DECK_TRANSITION_STATE_VIEW(state)->lists[1].scroll_box = DISPLAY_OBJECT_VIEW(object);
#else
    *(u8 **)(state + 0x5A88) = object;
#endif
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0, 0x2A, 0, 4, 2, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), 0xA);
#ifdef MEMORIES_PC
    BUILD_DECK_TRANSITION_STATE_VIEW(state)->lists[0].cursor_box = DISPLAY_OBJECT_VIEW(object);
#else
    *(u8 **)(state + 0x2D38) = object;
#endif
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x148, 0x2A, 0, 4, 3, 0xC, 0x218);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), 0xA);
#ifdef MEMORIES_PC
    BUILD_DECK_TRANSITION_STATE_VIEW(state)->lists[1].cursor_box = DISPLAY_OBJECT_VIEW(object);
#else
    *(u8 **)(state + 0x5A84) = object;
#endif
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0, 0, 0, 4, 9, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), 0xA);
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x140, 0, 0, 4, 0xA, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), 0xA);
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0, 0, 0, 4, 0xB, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), -4);
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x140, 0, 0, 4, 0xB, 0xC, 0x208);
    *(u16 *)(object + 8) |= 0x20;
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), -4);
    object = DisplayObject_AcquireSlot(DisplayObject_FindFreeGeneralSlot(), 2);
    DisplayObject_ConfigureSpriteAtPosition(object, 0x140, 0, 3, 0, 3, 0xB, 0x2F8);
    DisplayObject_SetDepthOffset(DISPLAY_OBJECT_VIEW(object), -4);
    BuildDeck_RefreshCountDisplay(BUILD_DECK_TRANSITION_STATE_VIEW(state));
}
