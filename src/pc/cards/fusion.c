#include "fusion.h"
#include <string.h>

static int clamp(int value) { return value < 0 ? 0 : value > 9999 ? 9999 : value; }
int Fusion_Attack(FusionCard card) { return clamp(card.attack + card.modifier + card.terrain); }
int Fusion_Defense(FusionCard card) { return clamp(card.defense + card.modifier + card.terrain); }

int Fusion_Step(const FusionRules *rules, FusionCard a, FusionCard b, FusionCard *out)
{
    int result = rules->fusion(a.id, b.id);
    if (result) {
        *out = rules->card(result); /* A new monster loses prior equip bonuses. */
        return out->id != 0;
    }
    if (rules->equip(b.id, a.id)) {
        *out = a;
        /* Placement tests the actual equip id, not its inherited base/effect. */
        out->modifier += b.id == 657 ? 1000 : 500;
        return 1;
    }
    if (rules->equip(a.id, b.id)) {
        *out = b;
        out->modifier += a.id == 657 ? 1000 : 500;
        return 1;
    }
    /* Placement discards an incoming non-monster when a monster stands. */
    *out = a.type < 20 && b.type >= 20 ? a : b;
    return 0;
}

static int better(const FusionLine *a, const FusionLine *b, int defense)
{
    int av, bv;
    if (!b->count) return 1;
    av = defense ? Fusion_Defense(a->card) : Fusion_Attack(a->card);
    bv = defense ? Fusion_Defense(b->card) : Fusion_Attack(b->card);
    if (av != bv) return av > bv;
    if (a->count != b->count) return a->count < b->count;
    av = defense ? Fusion_Attack(a->card) : Fusion_Defense(a->card);
    bv = defense ? Fusion_Attack(b->card) : Fusion_Defense(b->card);
    return av > bv;
}

static void search(const FusionRules *rules, const FusionCard *hand, FusionLine line,
                   unsigned used, int defense, FusionLine *best)
{
    int slot;
    if (line.count == FUSION_HAND) return;
    for (slot = 0; slot < FUSION_HAND; slot++) {
        FusionLine next = line;
        int fused = 0;
        if ((used & (1u << slot)) || !hand[slot].id) continue;
        if (!line.count) next.card = hand[slot];
        else {
            fused = Fusion_Step(rules, line.card, hand[slot], &next.card);
            if (!fused) next.failed |= 1 << next.count;
        }
        next.slots[next.count++] = slot;
        if (next.count >= 2 && fused && next.card.type < 20 && better(&next, best, defense)) *best = next;
        search(rules, hand, next, used | (1u << slot), defense, best);
    }
}

/* The line the player's picks make; 0 when a pick is invalid or repeated. */
static int begin(const FusionRules *rules, const FusionCard *hand, const int *prefix, int count,
                 FusionLine *line, unsigned *used, int *fused)
{
    int i;
    memset(line, 0, sizeof(*line));
    *used = 0; *fused = 0;
    if (count < 0 || count > FUSION_HAND || (count && !prefix)) return 0;
    for (i = 0; i < count; i++) {
        int slot = prefix[i];
        if (slot < 0 || slot >= FUSION_HAND || (*used & (1u << slot)) || !hand[slot].id) return 0;
        if (!i) line->card = hand[slot];
        else {
            *fused = Fusion_Step(rules, line->card, hand[slot], &line->card);
            if (!*fused) line->failed |= 1 << i;
        }
        line->slots[line->count++] = slot;
        *used |= 1u << slot;
    }
    return 1;
}

void Fusion_Plan(const FusionRules *rules, const FusionCard hand[FUSION_HAND],
                 const int *prefix, int count, int defense, FusionLine *selected, FusionLine *best)
{
    FusionLine line;
    unsigned used;
    int fused;
    memset(best, 0, sizeof(*best));
    if (!begin(rules, hand, prefix, count, &line, &used, &fused)) {
        memset(selected, 0, sizeof(*selected));
        return;
    }
    *selected = line;
    if (count >= 2 && fused && line.card.type < 20) *best = line;
    search(rules, hand, line, used, defense, best);
}

static int same(FusionCard a, FusionCard b)
{
    return a.id == b.id && Fusion_Attack(a) == Fusion_Attack(b) && Fusion_Defense(a) == Fusion_Defense(b);
}

static void toward(const FusionRules *rules, const FusionCard *hand, FusionLine line, unsigned used,
                   FusionCard target, FusionLine *route)
{
    int slot;
    for (slot = 0; slot < FUSION_HAND; slot++) {
        FusionLine next = line;
        if ((used & (1u << slot)) || !hand[slot].id) continue;
        if (route->count && next.count + 1 >= route->count) return;
        if (!Fusion_Step(rules, line.card, hand[slot], &next.card)) continue;
        next.slots[next.count++] = slot;
        if (same(next.card, target)) *route = next;
        else toward(rules, hand, next, used | (1u << slot), target, route);
    }
}

int Fusion_Toward(const FusionRules *rules, const FusionCard hand[FUSION_HAND],
                  const int *prefix, int count, FusionCard target, FusionLine *route)
{
    FusionLine line;
    unsigned used;
    int fused, slot;
    memset(route, 0, sizeof(*route));
    if (!target.id || !begin(rules, hand, prefix, count, &line, &used, &fused) || line.failed) return 0;
    if (count >= 2 && fused && same(line.card, target)) { *route = line; return 2; }
    if (count) toward(rules, hand, line, used, target, route);
    else for (slot = 0; slot < FUSION_HAND; slot++) {
        /* Nothing picked: any card can start the line. */
        FusionLine first = {0};
        if (!hand[slot].id) continue;
        first.card = hand[slot];
        first.slots[first.count++] = slot;
        toward(rules, hand, first, 1u << slot, target, route);
    }
    return route->count ? 1 : 0;
}
