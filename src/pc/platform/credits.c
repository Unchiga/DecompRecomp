/* After the credits. The retail game's last mode never leaves: once the save
 * and the secret number are done, Main_RunCredits (src/game/main_run_credits.c)
 * runs the credits scene in its phase 2 and, every frame after, asks
 * Model_IsCreditsPresentationComplete and drops the answer, so the screen
 * stays black until the console is reset. With the `return_after_credits`
 * setting (on by default; Game > Title screen after the credits) the port
 * publishes the main menu's mode three seconds after the presentation is
 * complete, as a menu choice would, and the game goes back to the title. The
 * game's own code is not changed. */
#include "credits.h"
#include "settings.h"
#include <stdio.h>

extern unsigned char D_8009B26C; /* main_mode_state.h: the active mode, 0x80 once it runs */
extern unsigned char D_8009B26E; /* Main_RunCredits: its phase, 0x80 once the phase started */
extern signed char D_8009AF9A;   /* -2 once the credits presentation is complete */

#define MAIN_MODE_MENU 8
#define MAIN_MODE_CREDITS 15
#define CREDITS_PHASE_SCENE 2
#define CREDITS_HOLD_FRAMES 180

void Credits_Frame(void)
{
    static unsigned held;
    if (!Settings_Get(SET_RETURN_AFTER_CREDITS) || (D_8009B26C & 0x9F) != (0x80 | MAIN_MODE_CREDITS) ||
        (D_8009B26E & 0x8F) != (0x80 | CREDITS_PHASE_SCENE) || D_8009AF9A != -2) {
        held = 0;
        return;
    }
    if (++held < CREDITS_HOLD_FRAMES) return;
    held = 0;
    fprintf(stderr, "memories-pc: the credits are over; back to the title (return_after_credits)\n");
    D_8009B26C = MAIN_MODE_MENU;
}
