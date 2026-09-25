#include "pc/cards/fusion.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static FusionCard cards[801];
static int recipes[801][801];
static FusionCard card(int id) { return cards[id]; }
static int fusion(int a, int b) { return recipes[a][b]; }
static int equip(int e, int m) { return (e == 657 || e == 301 || e == 800) && cards[m].type < 20 ? m : 0; }
static const FusionRules rules = {card, fusion, equip};
static void recipe(int a, int b, int r) { recipes[a][b] = recipes[b][a] = r; }
static void init(void)
{
    int i;
    memset(recipes, 0, sizeof(recipes));
    for (i = 1; i <= 800; i++) cards[i] = (FusionCard){i, 0, 1000, 1000, 0, 0};
    cards[657].type = cards[301].type = cards[800].type = 23;
}

int main(void)
{
    FusionCard hand[5], out;
    FusionLine selected, best, route;
    int pick[] = {4, 0, 2, 1, 3};
    init();
    recipe(1, 2, 723); recipe(723, 3, 724);
    cards[723].attack = 2000; cards[724].attack = 3000;
    hand[0] = card(1); hand[1] = card(2); hand[2] = card(3); hand[3] = card(657); hand[4] = card(301);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(!selected.count && best.card.id == 724 && best.count == 5);
    assert(Fusion_Attack(best.card) == 4500);
    /* Equip before fusion is discarded; equips afterwards accumulate. */
    assert(Fusion_Step(&rules, card(1), card(657), &out) && Fusion_Attack(out) == 2000);
    assert(Fusion_Step(&rules, out, card(2), &out) && Fusion_Attack(out) == 2000);
    assert(Fusion_Step(&rules, card(301), out, &out) && Fusion_Attack(out) == 2500);
    /* A mod copy of Megamorph is +500: placement uses the actual card id. */
    assert(Fusion_Step(&rules, out, card(800), &out) && Fusion_Attack(out) == 3000);
    /* Failed spell/monster combinations keep the monster in either order;
     * two failed monsters or two failed spells leave the incoming card. */
    cards[300].type = 20;
    assert(!Fusion_Step(&rules, card(1), card(300), &out) && out.id == 1);
    assert(!Fusion_Step(&rules, card(300), card(1), &out) && out.id == 1);
    assert(!Fusion_Step(&rules, card(1), card(3), &out) && out.id == 3);
    assert(!Fusion_Step(&rules, card(300), card(301), &out) && out.id == 301);
    /* Zero printed stats still receive modifiers, negatives clamp to zero. */
    cards[1].attack = 0; cards[1].defense = 9800; cards[1].terrain = -500;
    assert(Fusion_Step(&rules, card(1), card(657), &out));
    assert(Fusion_Attack(out) == 500 && Fusion_Defense(out) == 9999);
    out.modifier = -1000; assert(Fusion_Attack(out) == 0);
    /* Real pick order, failed step replaces carry, incoming live stats stay. */
    Fusion_Plan(&rules, hand, pick, 3, 0, &selected, &best);
    assert(selected.card.id == 3 && selected.failed == 4);
    assert(best.slots[0] == 4 && best.slots[1] == 0 && best.slots[2] == 2);
    /* A bad committed prefix may be discarded to reach a later fusion. */
    init(); recipe(2, 3, 724); cards[724].attack = 3500;
    hand[0] = card(1); hand[1] = card(2); hand[2] = card(3); hand[3] = (FusionCard){0}; hand[4] = (FusionCard){0};
    pick[0] = 0;
    Fusion_Plan(&rules, hand, pick, 1, 0, &selected, &best);
    assert(best.count == 3 && best.slots[0] == 0 && best.card.id == 724 && best.failed == 2);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(best.count == 2); /* Never recommend the unnecessary discard. */
    /* Non-fusing high-stat card is not called a fusion. */
    hand[0].attack = 9999;
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(best.card.id == 724);
    /* Two identical cards are distinct slots; one cannot be used twice. */
    init(); recipe(1, 1, 723); cards[723].attack = 4000;
    memset(hand, 0, sizeof(hand)); hand[0] = card(1);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best); assert(!best.count);
    hand[4] = card(1);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(best.count == 2 && best.slots[0] == 0 && best.slots[1] == 4);
    /* Ranking DEF changes the winner, with deterministic minimal materials. */
    recipe(1, 2, 724); cards[724].defense = 4500; hand[2] = card(2);
    Fusion_Plan(&rules, hand, NULL, 0, 1, &selected, &best);
    assert(best.card.id == 724 && best.count == 2);
    /* Malformed selection is rejected; spells are never ranked as monsters. */
    pick[0] = pick[1] = 4;
    Fusion_Plan(&rules, hand, pick, 2, 0, &selected, &best); assert(!selected.count && !best.count);
    pick[0] = 5;
    Fusion_Plan(&rules, hand, pick, 1, 0, &selected, &best); assert(!selected.count && !best.count);
    recipe(1, 1, 301); recipe(1, 2, 301);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(!best.count || best.card.type < 20);
    /* Toward the target: white on the way, green when made, red when a pick
     * leaves the clean route, and either order of a pair counts. */
    init(); recipe(1, 2, 723); recipe(723, 3, 724);
    cards[723].attack = 2000; cards[724].attack = 3000;
    memset(hand, 0, sizeof(hand)); hand[0] = card(1); hand[1] = card(2); hand[2] = card(3); hand[3] = card(4);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(best.card.id == 724);
    assert(Fusion_Toward(&rules, hand, NULL, 0, best.card, &route) == 1 && route.count == 3);
    pick[0] = 1; pick[1] = 0; pick[2] = 2;
    assert(Fusion_Toward(&rules, hand, pick, 1, best.card, &route) == 1 && route.slots[1] == 0);
    assert(Fusion_Toward(&rules, hand, pick, 2, best.card, &route) == 1 && route.slots[2] == 2);
    assert(Fusion_Toward(&rules, hand, pick, 3, best.card, &route) == 2);
    pick[0] = 3;
    assert(!Fusion_Toward(&rules, hand, pick, 1, best.card, &route));  /* not a material */
    pick[0] = 0; pick[1] = 2;
    assert(!Fusion_Toward(&rules, hand, pick, 2, best.card, &route));  /* failed step */
    /* An equip target is the monster with its bonus, not the bare monster. */
    init(); memset(hand, 0, sizeof(hand)); hand[0] = card(1); hand[1] = card(657);
    Fusion_Plan(&rules, hand, NULL, 0, 0, &selected, &best);
    assert(best.card.id == 1 && Fusion_Attack(best.card) == 2000);
    pick[0] = 0;
    assert(Fusion_Toward(&rules, hand, pick, 1, best.card, &route) == 1);
    pick[1] = 1;
    assert(Fusion_Toward(&rules, hand, pick, 2, best.card, &route) == 2);
    puts("fusion planner: passed");
    return 0;
}
