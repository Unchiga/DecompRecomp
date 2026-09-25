#ifndef MEMORIES_PC_CARD_RULES_H
#define MEMORIES_PC_CARD_RULES_H
/* The data-rule path used by Duel_CheckFusion/Equip, without gameplay events.
 * Safe to query speculatively; live disc tables and card/table mods apply. */
int CardRules_Fusion(int a, int b);
int CardRules_Equip(int equipment, int monster);
#endif
