/* Exercise the gameplay and preview entry points on packed retail bytes and
 * mod rules. The event counter proves previews never dispatch game events. */
#include "pc/cards/rules.h"
#include "pc/cards/cards.h"
#include "pc/cards/tables.h"
#include "pc/mods/mod_types.h"
#include "game/duel_card_checks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

unsigned short gDuel_awEquipTable[0x2100 / 2];
unsigned short gDuel_aFusionTable[0x10000 / 2];
static int event_count, mod_result = -1, equip_rule = -1, removed;
int Cards_Valid(int id) { return id > 0 && id <= 800; }
int Cards_BaseId(int id) { return id > 722 ? id - 722 : id; }
int Tables_Fusion(int a, int b, int *result)
{
    if (mod_result < 0 || !((a == 1 && b == 2) || (a == 2 && b == 1))) return 0;
    *result = mod_result; return 1;
}
int Cards_Fusion(int a, int b, int *result)
{
    if (a == 799 || b == 799) { *result = 800; return 1; }
    return 0;
}
int Tables_FilterFusion(int result) { return result == removed ? 0 : result; }
int Tables_Equip(int e, int m) { (void)e; (void)m; return equip_rule; }
void Mods_Dispatch(MemoriesModEvent *event) { (void)event; event_count++; }

int main(void)
{
    unsigned char *bytes = (unsigned char *)gDuel_aFusionTable;
    int a, b;
    /* Odd count's second half is still examined by retail (glitch fusion). */
    gDuel_aFusionTable[1] = 1500;
    bytes[1500] = 1; bytes[1501] = 0; bytes[1502] = 2; bytes[1503] = 3;
    bytes[1504] = 4; bytes[1505] = 5;
    gDuel_awEquipTable[0] = 301; gDuel_awEquipTable[1] = 1; gDuel_awEquipTable[2] = 1;
    assert(CardRules_Fusion(1, 2) == 3 && CardRules_Fusion(2, 1) == 3);
    assert(CardRules_Fusion(1, 4) == 5);
    assert(CardRules_Fusion(723, 724) == 3);
    assert(CardRules_Equip(301, 723) == 723 && !CardRules_Equip(723, 301));
    assert(!event_count);
    for (a = 1; a <= 800; a++) for (b = a; b <= 800; b++)
        assert(CardRules_Fusion(a, b) == Duel_CheckFusion(a, b));
    event_count = 0;
    mod_result = 750;
    assert(CardRules_Fusion(1, 2) == 750);
    mod_result = 0; assert(!CardRules_Fusion(1, 2));
    mod_result = 900; assert(!CardRules_Fusion(1, 2));
    mod_result = -1; removed = 3; assert(!CardRules_Fusion(1, 2));
    assert(CardRules_Fusion(799, 2) == 800);
    equip_rule = 0; assert(!CardRules_Equip(301, 1));
    equip_rule = 1; assert(CardRules_Equip(301, 724) == 724);
    assert(!CardRules_Equip(301, 0) && !CardRules_Fusion(0, 1));
    assert(!event_count);
    puts("fusion rules: gameplay parity, mod rules and side-effect-free preview passed");
    return 0;
}
