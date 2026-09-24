/* Debug > Back to title screen: the game's own way back, taken from anywhere.
 *
 * Retail already has one: when a campaign loss is over, Main_RunGameOver
 * (src/game/main_mode_runners.c) fades the music and the screen out, asks
 * for the title menu (D_8009B268 = 1, D_8009B26D = 0, mode 8) and longjmps
 * to the point Main_Init set up after the boot sequence
 * (src/game/main_init.c), which resets the frontend runtime, loads the main
 * menu package and runs the title. The debug menu's exit (DebugMenu_Exit)
 * does the same with DisplayObject_Reset and func_80035A64 first.
 *
 * A request is only taken between two mode runners, from the Main_Loop in
 * src/game/main_loop.c: no runner is half way through a step and no
 * nested frame loop (a fade, a disc wait, name entry) is on the stack, the
 * state the retail longjmp leaves from. The disc is left idle first, so no
 * transfer the old mode asked for lands on the title's package. While the
 * save slot menu (src/pc/saves) is open the request waits for it to close,
 * so it is never left drawn over the title. Progress not saved is lost, as
 * with a reset.
 *
 * MEMORIES_TITLE_AT=N[,N...] requests it at presented frames N (checks). */
#include "pc/platform/title_jump.h"
#include "pc/platform/menu.h"
#include "pc/saves/save_menu.h"
#include "pc/guest/state.h"
#include <stdio.h>
#include <stdlib.h>

/* Main_Loop is running: the title's own loop (Main_RunFrontendLoop, before
 * Main_Loop and again after each jump) is where the request would go anyway,
 * so the item is off there and a request made there is not kept for later. */
static int active, requested;

void TitleJump_SetActive(int enabled)
{
    active = !!enabled;
    if (!active) requested = 0;
    Menu_SetItemEnabled(MENU_ITEM_TITLE, active);
}

void TitleJump_Request(void)
{
    if (active) requested = 1;
}

void TitleJump_Frame(unsigned presented)
{
    static const char *at;
    if (!at) at = getenv("MEMORIES_TITLE_AT") ? getenv("MEMORIES_TITLE_AT") : "";
    while (*at) {
        char *end;
        unsigned long frame = strtoul(at, &end, 10);
        if (end == at) {
            at = "";
        } else if (presented >= frame) {
            at = *end == ',' ? end + 1 : end;
            TitleJump_Request();
        } else {
            break;
        }
    }
}

void TitleJump_Poll(void)
{
    if (!active) TitleJump_SetActive(1);
    if (!requested) return;
    if (SaveMenu_Active()) {
        if (requested == 1) fprintf(stderr, "memories-pc: back to the title screen once the save menu closes\n");
        requested = 2;
        return;
    }
    /* Disc waits and fades present frames and accept menu input. Disable
     * now, so another click during the jump cannot reset the next game. */
    TitleJump_SetActive(0);
    TitleJump_Execute();
}

void TitleJump_State(MemoriesState *state)
{
    MemoriesStateField field = {&active, sizeof(active)};
    if (Memories_StateLoading(state)) {
        active = 0;
        Memories_StateChunk(state, "title-jump", &field, 1);
        /* Requests belong to the UI's current timeline, not the save. */
        requested = 0;
        TitleJump_SetActive(active);
    } else {
        Memories_StateChunk(state, "title-jump", &field, 1);
    }
}
