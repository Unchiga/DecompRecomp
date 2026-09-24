/* API 4 adapter. No opponent tables, bytecode or real field stats are patched.
 * Planning owns only the pending selection; the game executes every action. */
#include "planner.h"
#define AI_GET_HAND_SIZE_RETURNS_S32 /* Native override serves word-sized callers. */
#include "game/ai.h"
#include "game/ai_opponent_data.h"
#include "game/ai_script_commands.h"
#include "game/duel_card.h"
#include "game/duel_card_checks.h"
#include "game/duel_card_state_helpers.h"
#include "game/duel_deck_card.h"
#include "game/duel_hand.h"
#define D_8009B360_AS_SIDE_ARRAY
#include "game/duel_side_state.h"
#define DUEL_TERRAIN_SCALAR_IN_DATA
#include "game/duel_terrain_boost.h"
#include "game/duel_check_ritual.h"
#define DUEL_PACKAGE_STAGE_RAW_ARENAS
#include "game/duel_load_package_stage.h"
#include "game/func_8001B938.h"
#include "game/func_80018004.h"
#include "game/display_object_core.h"
#include "pc/cards/cards.h"
#include "pc/mods/modapi.h"
#include <string.h>

extern unsigned char D_800EAE88[];
static const MemoriesModHost *host;
static void *original_init, *original_run, *original_window, *original_swap;
static void *original_search[5];

static int setting(const char *key, int fallback, int low, int high)
{
    int n = host->setting(host, key, fallback);
    return n < low ? low : n > high ? high : n;
}
static int active(void)
{
    return D_8009B1D5 < 2 && D_8009B360[D_8009B1D5] >= 0 &&
        gDuel_bOpponentID >= 1 && gDuel_bOpponentID < AI_OPPONENT_COUNT &&
        (gAiScript_State.script_base == D_801A8000 || gAiScript_State.script_base == D_801A9800);
}
static int sees_hidden(void)
{
    int mode = setting("hidden_cards", 0, 0, 2), id = gDuel_bOpponentID;
    if (mode) return mode == 2;
    return id == 8 || id == 15 || (id >= 35 && id <= 38);
}
static HmOptions options(void)
{
    HmOptions o;
    o.hand = setting("hand_planning", 1, 0, 1);
    o.fusion = setting("fusion_search", 1, 0, 1);
    o.guardian = setting("guardian_stars", 1, 0, 1);
    o.attacks = setting("attack_planning", 1, 0, 1);
    o.defense = setting("defensive_positions", 1, 0, 1);
    o.spells = setting("spell_timing", 1, 0, 1);
    o.effective = setting("effective_stats", 1, 0, 1);
    o.materials = setting("fusion_materials", 4, 2, 5);
    o.budget = setting("search_budget", 6000, 128, 20000);
    o.blind_risk = setting("blind_risk", 50, 0, 100);
    o.material_cost = setting("material_cost", 150, 0, 1000);
    o.trades = setting("equal_trades", 1, 0, 1);
    return o;
}
static int actual_hand(int side)
{
    int i, n = 0;
    for (i = 0; i < 5; i++) n += D_800E9FF0[side].hand[i] >= 0;
    return n;
}
static int available(int side)
{
    int h = actual_hand(side), left = 40 - D_800E9FF0[side].deck_draw_cursor;
    if (left < 0) left = 0;
    if (left > 40) left = 40;
    /* The retail materializer's deck encoding starts at slot 16. */
    return h < 5 ? h : h + left;
}
static s32 window(void)
{
    int n, mode;
    if (!active()) return ((s32 (*)(void))original_window)();
    mode = setting("deck_access", 0, 0, 2);
    n = mode == 1 ? 5 : mode == 2 ? setting("deck_window", 20, 5, 20) :
        ((s32 (*)(void))original_window)();
    if (n < 0) n = 0;
    if (n > HM_HAND) n = HM_HAND;
    if (n > available(D_8009B1D5)) n = available(D_8009B1D5);
    return (s8)n;
}
static void clear_tails(void)
{
    int side;
    for (side = 0; side < 2; side++) {
        int i, n = actual_hand(D_8009B1D5 ^ side);
        int left = 40 - D_800E9FF0[D_8009B1D5 ^ side].deck_draw_cursor;
        if (left > 0 && left <= 40) n += left;
        for (i = 11 + n; i < 56; i++)
            memset(&gDuel_aActiveCards[i + side * 55], 0, sizeof(AiActiveCard));
    }
}
static void initialize(u8 *script)
{
    ((void (*)(u8 *))original_init)(script);
    if (active()) clear_tails();
}
static HmCard card_info(int id)
{
    HmCard c = {0};
    unsigned stats;
    if (!Cards_Valid(id)) return c;
    stats = (unsigned)gDuel_adwCardStats[id - 1];
    c.id = id; c.effect = Cards_EffectId(id);
    c.type = (stats >> CARD_STAT_TYPE_SHIFT) & CARD_STAT_TYPE_MASK;
    c.attack = (stats & CARD_STAT_VALUE_MASK) * CARD_STAT_SCALE;
    c.defense = ((stats >> CARD_STAT_DEFENSE_SHIFT) & CARD_STAT_VALUE_MASK) * CARD_STAT_SCALE;
    c.star = (stats >> CARD_STAT_GUARDIAN_STAR_1_SHIFT) & 15;
    c.star2 = (stats >> CARD_STAT_GUARDIAN_STAR_2_SHIFT) & 15;
    return c;
}
static int terrain(int type, int field)
{
    return type >= 0 && type < 20 && field >= 1 && field <= 6 ?
        gDuel_aTerrainBoost[type][field - 1] * CARD_STAT_SCALE : 0;
}
static int ritual(int id) { return Duel_CheckRitual(0, id); }
static const HmRules rules = {card_info, Duel_CheckFusion, Duel_CheckEquip, terrain, ritual};

static HmCard snapshot(int index, int effective, int secret)
{
    AiActiveCard a = gDuel_aActiveCards[index];
    HmCard c = {0};
    if (!a.card_id) return c;
    /* Redact before looking up metadata, including custom effect identities. */
    if (secret && !sees_hidden() && (a.flags & HM_DOWN)) {
        c.id = -1; c.flags = a.flags;
        c.type = index >= 61 ? 21 : 0;
        return c;
    }
    c = card_info(a.card_id);
    c.flags = a.flags;
    if (effective) { c.attack = a.attack; c.defense = a.defense; }
    c.star = a.guardian_star;
    return c;
}
static HmBoard board(const HmOptions *o)
{
    HmBoard b = {0};
    int i, side = D_8009B1D5;
    b.hand_count = window(); b.terrain = gDuel_bTerrain;
    b.lp = D_800E9FF0[side].life_points.unsigned_value;
    b.max_lp = D_800E9FF0[side].max_life_points;
    b.enemy_lp = D_800E9FF0[side ^ 1].life_points.unsigned_value;
    b.pinned = D_800E9FF0[side].swords_turns_remaining != 0;
    b.enemy_pinned = D_800E9FF0[side ^ 1].swords_turns_remaining != 0;
    for (i = 0; i < 5; i++) {
        b.own[i] = snapshot(1 + i, o->effective, 0);
        b.enemy[i] = snapshot(56 + i, o->effective, 1);
        b.back[i] = snapshot(6 + i, o->effective, 0);
        b.enemy_back[i] = snapshot(61 + i, o->effective, 1);
    }
    for (i = 0; i < b.hand_count; i++) b.hand[i] = card_info(gDuel_aActiveCards[11+i].card_id);
    return b;
}

/* Keep aliased VM operands intact: a visibility register may also supply
 * mode/type/set arguments or be the output. Substitute a spare register in
 * a temporary operand stream, restoring both the spare and the real cursor. */
static void search(int which, int hide_operand, int dest_operand)
{
    u8 operands[5], *cursor;
    int reg, i, used, saved, count = dest_operand + 1;
    if (!active()) { ((void (*)(void))original_search[which])(); return; }
    cursor = gAiScript_State.script_cursor;
    memcpy(operands, cursor, count);
    for (reg = 0; reg < AI_SCRIPT_MEMORY_COUNT; reg++) {
        used = 0;
        for (i = 0; i < count; i++) if (operands[i] == reg) used = 1;
        if (!used) break;
    }
    if (reg == AI_SCRIPT_MEMORY_COUNT) { ((void (*)(void))original_search[which])(); return; }
    saved = gAiScript_aMemory[reg];
    operands[hide_operand] = reg;
    gAiScript_aMemory[reg] = !sees_hidden();
    gAiScript_State.script_cursor = operands;
    ((void (*)(void))original_search[which])();
    gAiScript_State.script_cursor = cursor + count;
    gAiScript_aMemory[reg] = saved;
}
static void strongest(void) { search(0, 2, 4); }
static void weakest(void) { search(1, 2, 4); }
static void find_card(void) { search(2, 2, 3); }
static void match_type(void) { search(3, 2, 4); }
static void defense_stopper(void) { search(4, 0, 1); }

static void retail_star(HmBoard *b)
{
    int i, dest = D_800EAE88[6], id = 0;
    HmCard c;
    if (dest >= 1 && dest <= 5 && !D_800EAE88[8]) id = b->own[dest-1].id;
    for (i = 0; i < 5 && D_800EAE88[i]; i++) {
        int slot = D_800EAE88[i] - 11, next;
        if (slot < 0 || slot >= b->hand_count) return;
        if (!id) id = b->hand[slot].id;
        else {
            next = rules.fusion(id, b->hand[slot].id);
            if (next) id = next;
            else if (!rules.equip(b->hand[slot].id, id)) return;
        }
    }
    c = card_info(id);
    if (c.id && c.type < 20) D_800EAE88[7] = Hm_ChooseStar(b, c, b->pinned);
}

static s32 run(void)
{
    HmOptions o;
    HmBoard b;
    HmDecision d, retail = {{0}, 0, 0, 0};
    int hand, fallback = 0;
    if (!active()) return ((s32 (*)(void))original_run)();
    o = options(); hand = gAiScript_State.script_base == D_801A8000;
    if ((hand && !o.hand) || (!hand && (!o.attacks || !o.defense || !o.spells))) {
        retail.result = ((s32 (*)(void))original_run)();
        if (!retail.result) return 0;
        memcpy(retail.selection, D_800EAE88, 12);
        fallback = 1;
        /* Discard temporary target marks left by the retail interpreter. */
        func_80028220(); clear_tails();
    }
    b = board(&o);
    if (hand && !o.hand) {
        if (o.guardian) retail_star(&b);
        memcpy(retail.selection, D_800EAE88, 12);
        d = retail;
    } else {
        d = hand ? Hm_PlanHand(&b, &o, &rules) : Hm_PlanField(&b, &o, &rules, fallback ? &retail : 0);
    }
    if (!d.result) return ((s32 (*)(void))original_run)();
    memcpy(D_800EAE88, d.selection, 12);
    if (setting("trace", 0, 0, 1))
        host->log(host, "opponent=%d phase=%s result=%d card=%d target=%d score=%d nodes=%d",
                  gDuel_bOpponentID, hand ? "hand" : "field", d.result,
                  hand ? d.selection[0] : d.selection[9], hand ? d.selection[6] : d.selection[10], d.score, d.nodes);
    return d.result;
}

/* Same materialization contract as func_8001BAF0, choosing the least valuable
 * unreserved held card instead of the first. Every swap stays within this
 * side's existing combined-deck records; no card is created or discarded. */
static void swap_selection(void)
{
    int held[5], i, j, chosen, side = D_8009B1D5;
    if (!active() || !setting("preserve_hand", 1, 0, 1)) {
        ((void (*)(void))original_swap)(); return;
    }
    for (i = 0; i < 5; i++) held[i] = D_8009B1C8->hand[i];
    for (i = 0; i < 5 && D_800EAE88[i]; i++)
        if (D_800EAE88[i] >= 11 && D_800EAE88[i] < 16) held[D_800EAE88[i]-11] = -1;
    for (i = 0; i < 5 && D_800EAE88[i]; i++) {
        int slot = D_800EAE88[i], index, v, record;
        DuelDeckCardRecord *a, *other, tmp;
        DuelCardDisplayObject *old;
        if (slot < 16) continue;
        if (slot >= 11 + available(side)) return;
        index = gDuel_aActiveCards[slot].deck_index;
        if (index < side * 40 || index >= side * 40 + 40) return;
        chosen = -1;
        for (j = 0; j < 5; j++) if (held[j] >= side*40 && held[j] < side*40+40 &&
            (chosen < 0 || Hm_KeepValue(card_info(gDuel_aDeckCardRecords[held[j]].id)) <
                           Hm_KeepValue(card_info(gDuel_aDeckCardRecords[held[chosen]].id)))) chosen = j;
        if (chosen < 0) return;
        a = &gDuel_aDeckCardRecords[held[chosen]]; other = &gDuel_aDeckCardRecords[index];
        v = a->deck_index; a->deck_index = other->deck_index; other->deck_index = v;
        tmp = *a; *a = *other; *other = tmp;
        record = D_800907CC[chosen + side * 5];
        old = (DuelCardDisplayObject *)D_800EA030[chosen].object;
        Duel_SetupCardRecord(record, a->deck_index);
        D_800EA030[chosen].object = (u8 *)func_80018004(&D_801A7AD8[record], old->out_x, old->out_y);
        DisplayObject_ReleaseIfPresent(old);
        D_8009B1C8->hand[chosen] = a->deck_index;
        D_800EAE88[i] = chosen + 11;
        held[chosen] = -1;
    }
}

int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 4) return 0;
    host = from; mod->api = 4;
    return host->hook(host, (void *)AiScript_Init, (void *)initialize, &original_init) &&
        host->hook(host, (void *)AiScript_Run, (void *)run, &original_run) &&
        host->hook(host, (void *)Ai_GetHandSize, (void *)window, &original_window) &&
        host->hook(host, (void *)func_8001BAF0, (void *)swap_selection, &original_swap) &&
        host->hook(host, (void *)AiScript_FindStrongest, (void *)strongest, &original_search[0]) &&
        host->hook(host, (void *)AiScript_FindWeakest, (void *)weakest, &original_search[1]) &&
        host->hook(host, (void *)AiScript_FindCard, (void *)find_card, &original_search[2]) &&
        host->hook(host, (void *)AiScript_MatchType, (void *)match_type, &original_search[3]) &&
        host->hook(host, (void *)AiScript_FindDefenseStopper, (void *)defense_stopper, &original_search[4]);
}
