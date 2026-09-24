#ifndef AI_HARD_MODE_PLANNER_H
#define AI_HARD_MODE_PLANNER_H

/* Independent of guest pointers: fixtures exercise the exact shipped planner. */
#define HM_ROW 5
#define HM_HAND 20
#define HM_USED 0x4000
#define HM_DOWN 0x1000
#define HM_DEFENSE 0x0800
#define HM_WIN 1000000

typedef struct {
    int id, effect, type, attack, defense, star, star2, flags;
} HmCard;
typedef struct {
    HmCard own[5], enemy[5], back[5], enemy_back[5], hand[HM_HAND];
    int hand_count, lp, max_lp, enemy_lp, pinned, enemy_pinned, terrain;
} HmBoard;
typedef struct {
    int hand, fusion, guardian, attacks, defense, spells, effective;
    int materials, budget, blind_risk, material_cost, trades;
} HmOptions;
typedef struct {
    HmCard (*card)(int id);
    int (*fusion)(int a, int b);
    int (*equip)(int equipment, int monster);
    int (*terrain)(int type, int terrain);
    int (*ritual)(int card);
} HmRules;
typedef struct {
    /* Byte-for-byte contract with the game's twelve-byte pending selection. */
    unsigned char selection[12];
    int result, score, nodes;
} HmDecision;

int Hm_StarBonus(int a, int b);
int Hm_KeepValue(HmCard card);
int Hm_ChooseStar(const HmBoard *, HmCard, int pinned);
HmDecision Hm_PlanHand(const HmBoard *, const HmOptions *, const HmRules *);
HmDecision Hm_PlanField(const HmBoard *, const HmOptions *, const HmRules *,
                        const HmDecision *retail);
#endif
