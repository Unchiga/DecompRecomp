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
 * src/pc/overrides/main_loop.c: no runner is half way through a step and no
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
#include "types.h"
#include "game/display_object_core.h"
#include "game/fade.h"
#include "game/file_transfer.h"
#include "game/func_80035A64.h"
#include "game/sound.h"
#include <stdio.h>
#include <stdlib.h>

extern u8 D_8009B268;
extern u8 D_8009B26C;
extern u8 D_8009B26D;
extern int D_800E9DC0[];                    /* the jmp_buf Main_Init set up */
void Psx_longjmp(int *env, int value);    /* the game's longjmp (src/pc/guest/setjmp_i386.S) */
unsigned Memories_PresentedFrames(void);

#define MAIN_MODE_MENU 8
#define MAIN_MENU_TITLE 0

/* Main_Loop is running: the title's own loop (Main_RunFrontendLoop, before
 * Main_Loop and again after each jump) is where the request would go anyway,
 * so the item is off there and a request made there is not kept for later. */
static int active, requested;

void TitleJump_Request(void)
{
    if (active) requested = 1;
}

void TitleJump_Poll(void)
{
    static const char *at;
    if (!active) {
        active = 1;
        Menu_SetItemEnabled(MENU_ITEM_TITLE, 1);
    }
    if (!at) at = getenv("MEMORIES_TITLE_AT") ? getenv("MEMORIES_TITLE_AT") : "";
    if (*at) {
        char *end;
        unsigned long frame = strtoul(at, &end, 10);
        if (end == at) {
            at = "";
        } else if (Memories_PresentedFrames() >= frame) {
            at = *end == ',' ? end + 1 : end;
            requested = 1;
        }
    }
    if (!requested) return;
    if (SaveMenu_Active()) {
        if (requested == 1) fprintf(stderr, "memories-pc: back to the title screen once the save menu closes\n");
        requested = 2;
        return;
    }
    requested = 0;
    fprintf(stderr, "memories-pc: back to the title screen from mode %u\n", D_8009B26C & 0x1F);
    File_WaitForTransfers();
    SD_BGMFadeOut();
    Fade_WaitOut();
    DisplayObject_Reset();
    func_80035A64();
    D_8009B268 = 1;
    D_8009B26D = MAIN_MENU_TITLE;
    D_8009B26C = MAIN_MODE_MENU;
    active = 0;
    Menu_SetItemEnabled(MENU_ITEM_TITLE, 0);
    Psx_longjmp(D_800E9DC0, 1);
}
