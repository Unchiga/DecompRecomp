/* Main_Loop (src/game/main_loop.c) as it matches, with one call added: after
 * each frame and before the next mode runner, a pending Debug > Back to title
 * screen is taken (src/pc/platform/title_jump.c). That is the one point where
 * no runner is half way through a step and no nested frame loop is on the
 * stack. */
#define MAIN_MODE_STATE_NEXT_AS_SCALAR
#define MAIN_MODE_STATE_ACTIVE_AS_SCALAR
#include "types.h"
#include "game/main_frame.h"
#include "game/fade.h"
#include "game/main_modes.h"
#include "game/main_debug.h"
#include "game/main_loop.h"
#include "game/main_reset_frontend_runtime.h"
#include "unmatched.h"
#include "game/main_mode_state.h"
#include "pc/platform/title_jump.h"

void Main_Loop(void) {
    Main_PrepareFrontendLoop();
    for (;;) {
        u8 v;
        Main_AdvanceFrame();
        TitleJump_Poll();
        v = D_8009B26C;
        if ((v & 0x80) == 0) {
            D_8009B26C = v | 0x80;
            Main_ResetFrontendRuntime();
        } else {
            gMain_apfnModeRunner[v & 0x1F]();
            if ((D_8009B26C & 0x40) == 0) Fade_WaitOut();
        }
    }
}
