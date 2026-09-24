/* Compile for i386: exercises the shipped adapter and guest layouts, with
 * presentation stubs. The planner has separate portable/sanitizer fixtures. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../../mods/ai-hard-mode/ai_hard_mode.c"

AiScriptState gAiScript_State;
AiActiveCard gDuel_aActiveCards[112];
s32 gAiScript_aMemory[20];
AiOpponentData gDuel_aOpponentData[40];
s8 gDuel_bOpponentID;
s8 D_8009B360[2];
u8 D_8009B1D5;
DuelSideState D_800E9FF0[2];
DuelSideState *D_8009B1C8;
u8 D_801A8000[6144], D_801A9800[6144], D_800EAE88[12], D_800907CC[10];
DuelDeckCardRecord gDuel_aDeckCardRecords[80];
DuelCardRecord D_801A7AD8[30];
DuelHandSlot D_800EA030[5];
u8 gDuel_bTerrain;
s8 gDuel_aTerrainBoost[20][6];
s32 gDuel_adwCardStats[1024];
static DuelCardDisplayObject objects[10];
static int spawned, released, original_calls, hidden_lookups, observed_visibility, observed_mode;
static int deck_mode, hidden_mode, custom_window = 20, retail_hand = 1;

int Cards_Valid(int id) { return id > 0 && id < 1024; }
int Cards_EffectId(int id) { if (id == 999) hidden_lookups++; return id; }
s32 Duel_CheckFusion(s32 a, s32 b) { (void)a; (void)b; return 0; }
s32 Duel_CheckEquip(s32 a, s32 b) { (void)a; (void)b; return 0; }
s32 Duel_CheckRitual(DuelRitualResult *out, s32 id) { (void)out; (void)id; return 0; }
void func_80028220(void) { }
void AiScript_Init(u8 *script)
{
    memset(&gAiScript_State,0,sizeof(gAiScript_State));
    gAiScript_State.script_base = gAiScript_State.script_cursor = script;
}
s32 AiScript_Run(void) { original_calls++; return 1; }
s32 Ai_GetHandSize(void) { return 20; }
void func_8001BAF0(void) { original_calls++; }
void AiScript_FindStrongest(void)
{
    u8 *p = gAiScript_State.script_cursor;
    observed_visibility = gAiScript_aMemory[p[2]];
    observed_mode = gAiScript_aMemory[p[0]];
    gAiScript_aMemory[p[4]] = 77;
    gAiScript_State.script_cursor += 5;
}
void AiScript_FindWeakest(void) { AiScript_FindStrongest(); }
void AiScript_FindCard(void) { }
void AiScript_MatchType(void) { }
void AiScript_FindDefenseStopper(void) { }
u8 *Duel_SetupCardRecord(s32 index, s32 deck)
{
    D_801A7AD8[index].card_id = gDuel_aDeckCardRecords[deck].id;
    return 0;
}
DuelCardDisplayObject *func_80018004(DuelCardRecord *record, s32 x, s32 y)
{
    (void)record;
    assert(spawned < 5);
    objects[5+spawned].out_x = x; objects[5+spawned].out_y = y;
    return &objects[5+spawned++];
}
void DisplayObject_ReleaseIfPresent(void *p) { assert(p); released++; }
static int fake_setting(const MemoriesModHost *h, const char *key, int fallback)
{
    (void)h;
    if (!strcmp(key,"deck_access")) return deck_mode;
    if (!strcmp(key,"hidden_cards")) return hidden_mode;
    if (!strcmp(key,"deck_window")) return custom_window;
    if (!strcmp(key,"hand_planning")) return retail_hand;
    return fallback;
}
static int fake_hook(const MemoriesModHost *h, void *fn, void *replacement, void **previous)
{
    (void)h; assert(replacement); *previous = fn; return 1;
}
static MemoriesModHost fake_host;

static void reset(void)
{
    int i;
    memset(gDuel_aActiveCards,0,sizeof(gDuel_aActiveCards));
    memset(D_800EAE88,0,12); memset(D_800E9FF0,0,sizeof(D_800E9FF0));
    D_8009B1D5 = 1; D_8009B360[0] = -1; D_8009B360[1] = 8; gDuel_bOpponentID = 8;
    D_8009B1C8 = &D_800E9FF0[1];
    for (i = 0; i < 2; i++) {
        int j;
        D_800E9FF0[i].deck_draw_cursor = 5;
        D_800E9FF0[i].life_points.unsigned_value = D_800E9FF0[i].max_life_points = 8000;
        for (j = 0; j < 5; j++) D_800E9FF0[i].hand[j] = i*40+j;
    }
    for (i = 0; i < 1024; i++) gDuel_adwCardStats[i] = (100 | (100 << 9) | (1 << 22) | (2 << 18));
    for (i = 0; i < 80; i++) {
        gDuel_aDeckCardRecords[i].id = i + 1;
        gDuel_aDeckCardRecords[i].deck_index = i;
        gDuel_aDeckCardRecords[i].data_block_index = i;
    }
    for (i = 0; i < 5; i++) {
        D_800EA030[i].object = (u8 *)&objects[i];
        objects[i].out_x = i*20; objects[i].out_y = 100;
        D_800907CC[i+5] = i+15;
    }
    spawned = released = original_calls = hidden_lookups = 0;
    hidden_mode = deck_mode = 0; retail_hand = 1;
    initialize(D_801A8000);
}

int main(void)
{
    MemoriesMod mod = {0}; HmOptions o; HmBoard b; int i;
    fake_host.api = 4; fake_host.setting = fake_setting; fake_host.hook = fake_hook;
    assert(MemoriesModInit(&fake_host,&mod));
    reset(); assert(window() == 20);
    deck_mode = 1; assert(window() == 5);
    deck_mode = 2; custom_window = 999; assert(window() == 20);
    D_800E9FF0[1].deck_draw_cursor = 39; assert(window() == 6);
    gDuel_aActiveCards[17].card_id = 500;
    initialize(D_801A8000); assert(!gDuel_aActiveCards[17].card_id);
    D_800E9FF0[1].hand[4] = -1; assert(window() == 4);

    reset(); hidden_mode = 1;
    gDuel_aActiveCards[56] = (AiActiveCard){999,9999,9999,HM_DOWN,0,8,9,0};
    o = options(); b = board(&o);
    assert(b.enemy[0].id == -1 && !b.enemy[0].effect && !b.enemy[0].attack && !b.enemy[0].star);
    assert(hidden_lookups == 0);
    hidden_mode = 2; b = board(&o); assert(b.enemy[0].id == 999 && b.enemy[0].attack == 9999);
    assert(hidden_lookups == 1);
    D_800E9FF0[1].swords_turns_remaining = 2; b = board(&o); assert(b.pinned && !b.enemy_pinned);
    hidden_mode = 1;
    for (i = 0; i < 5; i++) D_801A8000[i] = i;
    gAiScript_aMemory[2] = 123;
    strongest(); assert(observed_visibility == 1 && gAiScript_aMemory[2] == 123 && gAiScript_aMemory[4] == 77);
    gAiScript_State.script_cursor = D_801A8000; D_801A8000[4] = 2;
    strongest(); assert(gAiScript_aMemory[2] == 77);
    gAiScript_State.script_cursor = D_801A8000;
    D_801A8000[0] = 2; D_801A8000[4] = 4; gAiScript_aMemory[2] = 0;
    strongest();
    assert(observed_mode == 0 && observed_visibility == 1 && gAiScript_aMemory[2] == 0);
    assert(gAiScript_State.script_cursor == D_801A8000 + 5);

    /* A selected held material is reserved. The lowest-value other held card
     * swaps with an upcoming card; both deck identity and art index follow. */
    reset(); gDuel_adwCardStats[40] = 300; gDuel_adwCardStats[41] = 50;
    gDuel_adwCardStats[42] = 70; gDuel_adwCardStats[43] = 400; gDuel_adwCardStats[44] = 250;
    D_800EAE88[0] = 12; D_800EAE88[1] = 16; D_800EAE88[2] = 17;
    gDuel_aActiveCards[16].deck_index = 50; gDuel_aActiveCards[17].deck_index = 51;
    swap_selection();
    assert(D_800EAE88[0] == 12 && D_800EAE88[1] == 13 && D_800EAE88[2] == 15);
    assert(gDuel_aDeckCardRecords[41].id == 42);
    assert(gDuel_aDeckCardRecords[42].id == 51 && gDuel_aDeckCardRecords[50].id == 43);
    assert(gDuel_aDeckCardRecords[42].data_block_index == 50);
    assert(gDuel_aDeckCardRecords[42].deck_index == 42 && gDuel_aDeckCardRecords[50].deck_index == 50);
    assert(gDuel_aDeckCardRecords[44].id == 52 && spawned == 2 && released == 2);
    assert(((DuelCardDisplayObject *)D_800EA030[2].object)->out_x == 40);

    reset(); gDuel_aActiveCards[11].card_id = 10;
    assert(run() == 1 && D_800EAE88[0] == 11 && !original_calls);
    retail_hand = 0; assert(run() == 1 && original_calls == 1);
    D_8009B360[1] = -1; assert(run() == 1 && original_calls == 2);
    puts("AI hard mode adapter: hand windows, redaction, Swords, retail fallback and real deck swaps passed");
    return 0;
}
