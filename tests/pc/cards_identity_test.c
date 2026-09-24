/* Test save remapping independently of the retail data/image renderer. */
#include "../../src/pc/cards/cards.c"
#include "scratch.h"
#include <assert.h>
#include <unistd.h>
int gCard_nCount = 724, gCard_nExtraOwner;
unsigned short gCard_awBaseId[CARD_TABLE_ID_END], gDuel_awPlayerDeck[1024];
unsigned char gCard_abExtraChest[CARD_TABLE_ID_END], gCard_abExtraSeen[(CARD_TABLE_ID_END + 7) / 8];
unsigned char gCard_abPairChest[2][CARD_TABLE_ID_END], gCard_abPairPending[2][CARD_TABLE_ID_END];
int Log_Wanted(LogChannel channel)
{
    (void)channel;
    return 0;
}
void Log_Printf(LogChannel channel, const char *format, ...)
{
    (void)channel;
    (void)format;
}
int main(void)
{
    char directory[SCRATCH_MAX];
    unsigned char state[2048] = {0};
    int code = 123;
    unsigned sequence = 1;
    scratch_template(directory, sizeof(directory), "memories-card-identities");
    assert(mkdtemp(directory));
    setenv("MEMORIES_USER_DIR", directory, 1);
    memcpy(state + SAVE_DUELIST_CODE, &code, 4);
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    identities[723] = "alpha:dragon:1";
    identities[724] = "beta:mage:1";
    gCard_awBaseId[723] = 1;
    gCard_awBaseId[724] = 2;
    gCard_abExtraChest[723] = 3;
    gCard_abExtraChest[724] = 5;
    gCard_abExtraSeen[723 >> 3] |= 1u << (723 & 7);
    ((unsigned short *)state)[0] = 723;
    Cards_SaveWritten(state, 1);
    identities[723] = "beta:mage:1";
    identities[724] = "alpha:dragon:1";
    gCard_awBaseId[723] = 2;
    gCard_awBaseId[724] = 1;
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[724] == 3 && gCard_abExtraChest[723] == 5);
    assert(((unsigned short *)state)[0] == 724);
    assert((gCard_abExtraSeen[724 >> 3] >> (724 & 7)) & 1);
    /* Temporarily remove alpha, save beta, then restore alpha: ownership survives. */
    gCard_nCount = 723;
    identities[724] = NULL;
    ((unsigned short *)state)[0] = 723;
    Cards_SaveLoaded(state);
    assert(((unsigned short *)state)[0] == 1);
    Cards_SaveWritten(state, 2);
    gCard_nCount = 724;
    identities[724] = "alpha:dragon:1";
    sequence = 2;
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[724] == 3 && gCard_abExtraChest[723] == 5);
    /* Ambiguous legacy IDs are preserved, never assigned to a new card. */
    char path[1024], backup[1040];
    assert(!sidecar_path(path, sizeof(path), code));
    FILE *file = fopen(path, "w");
    assert(file);
    fputs("save 2\nchest 723 9\ndeck 0 723 1\nend\n", file);
    fclose(file);
    ((unsigned short *)state)[0] = 723;
    Cards_SaveLoaded(state);
    assert(!gCard_abExtraChest[723] && ((unsigned short *)state)[0] == 1);
    /* New progress is still saved beside the unmigrated legacy lines. */
    gCard_abExtraChest[724] = 4;
    Cards_SaveWritten(state, 3);
    file = fopen(path, "r");
    char text[1024] = {0};
    assert(file);
    fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    assert(strstr(text, "chest 723 9"));
    assert(strstr(text, "chest2 alpha:dragon:1 4"));
    gCard_abExtraChest[724] = 0;
    setenv("MEMORIES_MIGRATE_CARD_IDS", "1", 1);
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[723] == 9);
    Cards_SaveWritten(state, 3);
    snprintf(backup, sizeof(backup), "%s.legacy", path);
    file = fopen(backup, "r");
    assert(file);
    fclose(file);
    unsetenv("MEMORIES_MIGRATE_CARD_IDS");
    sequence = 3;
    memcpy(state + SAVE_SEQUENCE, &sequence, 4);
    Cards_SaveLoaded(state);
    assert(gCard_abExtraChest[723] == 9);
    return 0;
}
