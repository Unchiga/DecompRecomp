#include "planner.h"

static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }
static int clamp(int n) { return max(0, min(9999, n)); }
static int monster(HmCard c) { return c.id && c.type < 20; }
static int value(HmCard c) { return max(c.attack, c.defense); }
static int occupied(const HmCard *row)
{
    int n = 0, i;
    for (i = 0; i < 5; i++) n += row[i].id != 0;
    return n;
}

int Hm_StarBonus(int a, int b)
{
    int start = a >= 7 ? 7 : 1, size = a >= 7 ? 4 : 6;
    if (a < 1 || a > 10 || b < start || b >= start + size) return 0;
    if ((a - start + 1) % size == b - start) return 500;
    if ((a - start + size - 1) % size == b - start) return -500;
    return 0;
}

int Hm_KeepValue(HmCard c)
{
    if (!c.id) return 0;
    if (monster(c)) return value(c);
    if (c.effect == 337 || c.effect == 348 || c.effect == 672) return 3000;
    if (c.type == 23 || c.type == 21) return 1400;
    return 800;
}

/* Hidden cards arrive redacted (id=-1), with no secret identity or stats. */
static int safety(const HmBoard *b, HmCard c, int defense)
{
    int loss = 0, i;
    if (b->enemy_pinned) return 0;
    for (i = 0; i < 5; i++) if (b->enemy[i].id) {
        HmCard e = b->enemy[i];
        int power = e.id < 0 ? 1800 : e.attack + Hm_StarBonus(e.star, c.star);
        int diff = power - (defense ? c.defense : c.attack);
        if (diff > 0) loss = max(loss, (defense ? 0 : diff) + value(c) / 2);
    }
    return loss;
}

int Hm_ChooseStar(const HmBoard *b, HmCard c, int pinned)
{
    int choice, best = -0x3fffffff, answer = 0, i;
    for (choice = 0; choice < 2; choice++) {
        HmCard current = c;
        int score = 0;
        current.star = choice ? c.star2 : c.star;
        score -= min(safety(b, current, 0), safety(b, current, 1));
        if (!pinned) for (i = 0; i < 5; i++) if (b->enemy[i].id > 0) {
            HmCard e = b->enemy[i];
            int diff = c.attack + Hm_StarBonus(current.star, e.star) -
                ((e.flags & HM_DEFENSE) ? e.defense : e.attack);
            if (diff > 0) score = max(score, 2000 + diff + value(e) / 2);
        }
        if (score > best) { best = score; answer = choice; }
    }
    return answer;
}

typedef struct { int a, t, damage, removed, lost, gain, uncertain; } Attack;
typedef struct {
    const HmBoard *b;
    const HmOptions *o;
    HmDecision best;
    int nodes;
} AttackSearch;

static int attack_options(AttackSearch *s, unsigned used, unsigned dead, Attack *out)
{
    int i, j, n = 0, targets = 0;
    for (j = 0; j < 5; j++) if (s->b->enemy[j].id && !(dead & (1u << j))) targets++;
    for (i = 0; i < 5; i++) {
        HmCard a = s->b->own[i];
        if (!monster(a) || (a.flags & HM_USED) || (used & (1u << i))) continue;
        if (!targets) {
            out[n++] = (Attack){i, -1, a.attack, 0, 0, a.attack * 4, 0};
            continue;
        }
        for (j = 0; j < 5; j++) {
            HmCard e = s->b->enemy[j];
            Attack move = {i, j, 0, 0, 0, 0, 0};
            int diff;
            if (!e.id || (dead & (1u << j))) continue;
            if (e.id < 0) {
                /* A declared risk policy, never a peek at the hidden card. */
                if (!s->o->blind_risk || a.attack < 3000 - s->o->blind_risk * 20) continue;
                move.uncertain = 1;
                move.gain = 100 + s->o->blind_risk * 3;
            } else {
                diff = a.attack + Hm_StarBonus(a.star, e.star) -
                    ((e.flags & HM_DEFENSE) ? e.defense : e.attack);
                if (diff < 0 || (diff == 0 && ((e.flags & HM_DEFENSE) || !s->o->trades))) continue;
                move.damage = (e.flags & HM_DEFENSE) ? 0 : diff;
                move.removed = value(e) + 400;
                move.lost = diff == 0 ? value(a) + 400 : 0;
                move.gain = move.damage * 4 + move.removed - move.lost;
            }
            out[n++] = move;
        }
    }
    /* Stable best-first ordering; economical attacker breaks ties. */
    for (i = 1; i < n; i++) {
        Attack v = out[i];
        j = i;
        while (j && (out[j-1].gain < v.gain ||
               (out[j-1].gain == v.gain && s->b->own[out[j-1].a].attack > s->b->own[v.a].attack))) {
            out[j] = out[j-1]; j--;
        }
        out[j] = v;
    }
    return n;
}

static void attack_search(AttackSearch *s, unsigned used, unsigned dead,
                          int damage, int score, int depth, Attack first)
{
    Attack moves[25];
    int n, i;
    if (s->nodes >= s->o->budget) return;
    s->nodes++;
    n = attack_options(s, used, dead, moves);
    /* All root alternatives, up to three promising continuations per node. */
    if (depth) n = min(n, 3);
    for (i = 0; i < n; i++) {
        Attack m = moves[i], root = depth ? first : m;
        int next_damage = damage + m.damage;
        int next_score = score + m.gain;
        int ranked = next_score + (next_damage >= s->b->enemy_lp ? HM_WIN : 0);
        if (ranked > s->best.score || (ranked == s->best.score && ranked > 0 &&
            s->best.selection[9] && s->b->own[root.a].attack <
            s->b->own[s->best.selection[9]-1].attack)) {
            s->best.score = ranked;
            s->best.result = 2;
            s->best.selection[9] = (unsigned char)(root.a + 1);
            s->best.selection[10] = (unsigned char)(root.t < 0 ? 58 : root.t + 56);
        }
        if (!m.uncertain && depth < 4 && next_damage < s->b->enemy_lp)
            attack_search(s, used | (1u << m.a), dead | (m.t < 0 ? 0 : 1u << m.t),
                          next_damage, next_score, depth + 1, root);
    }
}

static HmDecision attacks(const HmBoard *b, const HmOptions *o)
{
    AttackSearch s = {b, o, {{0}, 0, 0, 0}, 0};
    Attack first = {0};
    if (!b->pinned) attack_search(&s, 0, 0, 0, 0, 0, first);
    s.best.nodes = s.nodes;
    return s.best;
}

static int wall(int id)
{
    static const int ids[] = {72, 362, 366, 2, 40, 83, 88, 121, 156, 170,
                              255, 274, 277, 369, 416, 418, 449, 468, 497, 518, 0};
    int i;
    for (i = 0; ids[i]; i++) if (ids[i] == id) return 1;
    return 0;
}
static int stays(int id)
{
    return id == 380 || id == 374 || id == 67 || id == 713 || id == 217 ||
           id == 364 || id == 1 || id == 392;
}

static int terrain_delta(const HmBoard *b, const HmRules *r, int terrain)
{
    int i, score = 0;
    for (i = 0; i < 5; i++) {
        if (monster(b->own[i])) score += r->terrain(b->own[i].type, terrain) - r->terrain(b->own[i].type, b->terrain);
        if (b->enemy[i].id > 0) score -= r->terrain(b->enemy[i].type, terrain) - r->terrain(b->enemy[i].type, b->terrain);
    }
    return score;
}

static int removal_type(int effect)
{
    switch (effect) {
    case 329: return 0; case 653: return 3; case 656: return 2;
    case 660: return 14; case 662: return 9; case 663: return 18; case 664: return 12;
    default: return -1;
    }
}

/* Returns utility and an own-field target (equips only). Unknown effects are
 * left set, never speculatively activated. The live game resolves all effects. */
static int spell(const HmBoard *b, const HmOptions *o, const HmRules *r,
                 HmCard c, int *target)
{
    int i, score = 0, id = c.effect, amount, type = removal_type(id);
    *target = 0;
    if (id >= 343 && id <= 347) {
        static const int burn[] = {50, 100, 200, 500, 1000};
        amount = burn[id - 343];
        /* Known reflection is actionable information, hidden traps are not. */
        for (i = 0; i < 5; i++) if (b->enemy_back[i].effect == 687) return 0;
        return amount * 4 + (amount >= b->enemy_lp ? HM_WIN : 0);
    }
    if (id >= 338 && id <= 342) {
        static const int heal[] = {200, 500, 1000, 2000, 5000};
        for (i = 0; i < 5; i++) if (b->enemy_back[i].effect == 688) return 0;
        amount = max(0, min(heal[id - 338], b->max_lp - b->lp));
        return amount * (b->lp < 2500 ? 3 : 1);
    }
    if (id == 672) return occupied(b->enemy_back) * 1600;
    if (id >= 330 && id <= 335) return max(0, terrain_delta(b, r, id - 329) * 2);
    if (id == 348) {
        if (b->enemy_pinned) return 0;
        for (i = 0; i < 5; i++) if (b->enemy[i].id)
            score += b->enemy[i].id < 0 ? 600 : b->enemy[i].attack / 2;
        return score;
    }
    if (id == 350) {
        for (i = 0; i < 5; i++) if (b->enemy[i].id < 0) score += 500;
        return score;
    }
    if (id == 337 || id == 336 || id == 661 || type >= 0) {
        for (i = 0; i < 5; i++) {
            HmCard e = b->enemy[i];
            if (!e.id) continue;
            if (id == 337 || id == 336 ||
                (e.id > 0 && (e.type == type || (id == 661 && e.attack >= 1500))))
                score += e.id < 0 ? 1400 : value(e) + 600;
        }
        if (id == 336) {
            int skipped_self = 0;
            for (i = 0; i < 5; i++) {
                if (b->own[i].id) score -= value(b->own[i]) + 600;
                if (b->enemy_back[i].id) score += 1000;
                if (b->back[i].id == c.id && !skipped_self) skipped_self = 1;
                else score -= Hm_KeepValue(b->back[i]);
            }
        }
        return max(0, score);
    }
    if (id == 349 || id == 669) {
        for (i = 0; i < 5; i++) if (b->enemy[i].id) score += 700;
        return score;
    }
    if (id == 320) { /* Stop Defense: only use when it improves our attack sequence. */
        HmBoard next = *b;
        HmOptions cheap = *o;
        HmDecision before, after;
        cheap.budget = min(128, o->budget);
        for (i = 0; i < 5; i++) next.enemy[i].flags &= ~HM_DEFENSE;
        before = attacks(b, &cheap); after = attacks(&next, &cheap);
        return max(0, after.score - before.score);
    }
    if (c.type == 23) {
        for (i = 0; i < 5; i++) if (monster(b->own[i]) && r->equip(c.id, b->own[i].id)) {
            HmBoard next = *b;
            HmOptions cheap = *o;
            int bonus = id == 657 ? 1000 : 500, gain;
            cheap.budget = min(128, o->budget);
            next.own[i].attack = clamp(next.own[i].attack + bonus);
            next.own[i].defense = clamp(next.own[i].defense + bonus);
            gain = bonus + max(0, attacks(&next, &cheap).score - attacks(b, &cheap).score);
            if (gain > score) { score = gain; *target = i + 1; }
        }
        return score;
    }
    if (c.type == 22 && r->ritual(c.id)) return 2000;
    return 0;
}

HmDecision Hm_PlanField(const HmBoard *b, const HmOptions *o, const HmRules *r,
                        const HmDecision *retail)
{
    HmDecision best = {{0}, 0, 0, 0}, a;
    int i, target, score;
    if (retail && !o->attacks && !o->defense && !o->spells) return *retail;
    if (retail && retail->result == 2) {
        int slot = retail->selection[9];
        int is_attack = slot <= 5 && retail->selection[10] >= 56;
        if ((slot >= 6 && !o->spells) || (!is_attack && slot <= 5 && !o->defense)) return *retail;
        if (is_attack && !o->attacks) { best = *retail; best.score = 1; }
    }
    if (!o->defense && !retail) for (i = 0; i < 5; i++)
        if (b->own[i].id && !(b->own[i].flags & HM_USED) && wall(b->own[i].effect)) {
            best.result = 2; best.selection[9] = i + 1; best.selection[11] = 1; return best;
        }
    if (o->attacks) best = attacks(b, o);
    if (o->spells) for (i = 0; i < 5; i++) if (b->back[i].id && !(b->back[i].flags & HM_USED)) {
        score = spell(b, o, r, b->back[i], &target);
        if (score > best.score) {
            a = (HmDecision){{0}, 2, score, best.nodes};
            a.selection[9] = i + 6;
            a.selection[10] = target ? target : i + 6;
            best = a;
        }
    }
    if (best.result) return best;
    if (retail && !o->defense) { best.result = 3; return best; }
    for (i = 0; i < 5; i++) if (b->own[i].id && !(b->own[i].flags & HM_USED)) {
        best.result = 2;
        best.selection[9] = i + 1;
        best.selection[11] = o->defense ?
            safety(b, b->own[i], 1) < safety(b, b->own[i], 0) : !stays(b->own[i].effect);
        return best;
    }
    best.result = 3;
    return best;
}

typedef struct {
    const HmBoard *b;
    const HmOptions *o;
    const HmRules *r;
    HmDecision best;
    int nodes, path[5];
} HandSearch;

static int destination(const HmBoard *b)
{
    int i, best = 0;
    for (i = 0; i < 5; i++) {
        if (!b->own[i].id) return i;
        if (value(b->own[i]) < value(b->own[best])) best = i;
    }
    return best;
}

static void consider_monster(HandSearch *s, HmCard card, int count, int field)
{
    int dest = field >= 0 ? field : destination(s->b);
    HmCard identity = s->r->card(card.id);
    int choice;
    int i, score, hit = 0, old = s->b->own[dest].id ? value(s->b->own[dest]) : 0;
    /* Field snapshots contain the selected star; output byte 7 is instead
     * an index into the card's original pair. Re-establish that pair. */
    card.star = identity.star; card.star2 = identity.star2;
    choice = s->o->guardian ? Hm_ChooseStar(s->b, card, s->b->pinned) : 0;
    if (choice) card.star = card.star2;
    for (i = 0; i < 5; i++) if (s->b->enemy[i].id > 0 && !s->b->pinned) {
        HmCard e = s->b->enemy[i];
        int diff = card.attack + Hm_StarBonus(card.star, e.star) -
            ((e.flags & HM_DEFENSE) ? e.defense : e.attack);
        if (diff > 0) hit = max(hit, value(e) / 2 + ((e.flags & HM_DEFENSE) ? 0 : diff));
        if (!(e.flags & HM_DEFENSE) && diff >= s->b->enemy_lp) hit = HM_WIN;
    }
    if (!occupied(s->b->enemy) && !s->b->pinned) {
        hit = card.attack;
        if (card.attack >= s->b->enemy_lp) hit += HM_WIN;
    }
    score = value(card) - old + hit - min(safety(s->b, card, 0), safety(s->b, card, 1)) / 2;
    score -= s->o->material_cost * count;
    for (i = 0; i < count; i++) score -= Hm_KeepValue(s->b->hand[s->path[i]]) / 12;
    if (!s->best.result || score > s->best.score) {
        s->best = (HmDecision){{0}, 1, score, 0};
        for (i = 0; i < count; i++) s->best.selection[i] = s->path[i] + 11;
        s->best.selection[6] = dest + 1;
        s->best.selection[7] = choice;
        s->best.selection[8] = field < 0;
    }
}

static HmCard combine(HandSearch *s, HmCard a, HmCard b)
{
    HmCard out = {0};
    int id = s->r->fusion(a.id, b.id);
    if (id) {
        out = s->r->card(id);
        if (s->o->effective) {
            int bonus = s->r->terrain(out.type, s->b->terrain);
            out.attack = clamp(out.attack + bonus); out.defense = clamp(out.defense + bonus);
        }
    } else if (monster(a) && b.type == 23 && s->r->equip(b.id, a.id)) {
        out = a;
        out.attack = clamp(out.attack + (b.effect == 657 ? 1000 : 500));
        out.defense = clamp(out.defense + (b.effect == 657 ? 1000 : 500));
    }
    return out;
}

static void fusion_search(HandSearch *s, HmCard card, unsigned used, int count, int field)
{
    int i;
    if (s->nodes >= s->o->budget || count >= s->o->materials || count >= 5) return;
    for (i = 0; i < s->b->hand_count && s->nodes < s->o->budget; i++) if (!(used & (1u << i))) {
        HmCard next;
        s->nodes++;
        next = combine(s, card, s->b->hand[i]);
        if (!monster(next)) continue;
        s->path[count] = i;
        consider_monster(s, next, count + 1, field);
        fusion_search(s, next, used | (1u << i), count + 1, field);
    }
}

HmDecision Hm_PlanHand(const HmBoard *b, const HmOptions *o, const HmRules *r)
{
    HandSearch s = {b, o, r, {{0}, 0, 0, 0}, 0, {0}};
    int i, j, slot, target, score, immediate;
    /* Score every single-card option before spending the bounded fusion budget. */
    for (i = 0; i < b->hand_count; i++) {
        HmCard c = b->hand[i];
        if (!c.id) continue;
        if (monster(c)) {
            if (o->effective) {
                int bonus = r->terrain(c.type, b->terrain);
                c.attack = clamp(c.attack + bonus); c.defense = clamp(c.defense + bonus);
            }
            s.path[0] = i;
            consider_monster(&s, c, 1, -1);
        } else {
            slot = -1;
            for (j = 0; j < 5; j++) if (!b->back[j].id) { slot = j + 6; break; }
            if (c.type == 22 && slot < 0) continue;
            target = 0;
            score = o->spells ? spell(b, o, r, c, &target) : 0;
            immediate = score > 0 && c.type != 22;
            if (score <= 0) {
                if (slot < 0) continue;
                score = c.type == 21 ? 500 : 50;
            }
            score -= o->material_cost;
            if (!s.best.result || score > s.best.score) {
                s.best = (HmDecision){{0}, 1, score, 0};
                s.best.selection[0] = i + 11;
                s.best.selection[6] = target ? target : (slot < 0 ? 6 : slot);
                /* Rituals are set first: the field controller checks tributes. */
                s.best.selection[8] = !immediate;
                if (c.type == 23 && immediate && target && o->guardian) {
                    HmCard equipped = b->own[target - 1], identity = r->card(equipped.id);
                    int bonus = c.effect == 657 ? 1000 : 500;
                    equipped.attack = clamp(equipped.attack + bonus);
                    equipped.defense = clamp(equipped.defense + bonus);
                    equipped.star = identity.star; equipped.star2 = identity.star2;
                    s.best.selection[7] = Hm_ChooseStar(b, equipped, b->pinned);
                }
            }
        }
    }
    if (o->fusion) {
        /* Iterative material limits prevent one early seed consuming the whole
         * budget before the other two-card combinations have been considered. */
        HmOptions bounded = *o;
        int depth;
        s.o = &bounded;
        for (depth = 2; depth <= o->materials && s.nodes < o->budget; depth++) {
            bounded.materials = depth;
            for (i = 0; i < b->hand_count && s.nodes < o->budget; i++) {
                HmCard c = b->hand[i];
                if (!monster(c)) continue;
                if (o->effective) {
                    int bonus = r->terrain(c.type, b->terrain);
                    c.attack = clamp(c.attack + bonus); c.defense = clamp(c.defense + bonus);
                }
                s.path[0] = i;
                fusion_search(&s, c, 1u << i, 1, -1);
            }
            for (i = 0; i < 5 && s.nodes < o->budget; i++) if (monster(b->own[i]))
                fusion_search(&s, b->own[i], 0, 0, i);
        }
    }
    s.best.nodes = s.nodes;
    return s.best;
}
