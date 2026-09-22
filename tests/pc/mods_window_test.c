#include "pc/platform/mods_window.h"
#include "pc/platform/settings.h"
#include "pc/mods/mods.h"
#include <assert.h>
#include <string.h>

/* Geometry at scale 1 with a 7px-per-character font (see mods_window.c):
 * the lists start at y=96 with 4px padding and 28px rows, the arrow buttons
 * sit centred in the gutter, and the footer buttons are 28px tall ending
 * 16px above the bottom. */
#define ROW0_Y 114
#define ARROW_APPLY_Y 124
#define ARROW_REMOVE_Y 160
#define FOOTER_Y(h) ((h) - 30)
#define LEFT_ROW_X 40
#define RIGHT_ROW_X(w) ((w) - 40)
#define CLOSE_X(w) ((w) - 30)
#define PRIMARY_X(w) ((w) - 70)
#define CANCEL_X(w) ((w) - 206)

/* `stored` is what the settings hold, `live` what is actually in place: a
 * mod that needs a restart is recorded without being put in place. */
static int stored[2], live[2], restart_required, restarts, saves, save_ok = 1;
static const char *mod_status[2] = {"", ""};
int Menu_Scale(void) { return 1; }
int Menu_TextWidth(const char *s) { return (int)strlen(s) * 7; }
void Menu_DrawText(MenuCanvas *c, int x, int y, const char *s, uint32_t color)
{ (void)c; (void)x; (void)y; (void)s; (void)color; }
int Mods_Count(void) { return 2; }
const char *Mods_Name(int mod) { return mod ? "Hand camera" : "3D Monsters"; }
const char *Mods_Status(int mod) { return mod_status[mod]; }
int Mods_Enabled(int mod) { return stored[mod]; }
int Mods_RequiresRestart(int mod) { (void)mod; return restart_required; }
void Mods_SetEnabled(int mod, int on)
{
    stored[mod] = on;
    if (!restart_required) live[mod] = on;
}
int Settings_Save(void) { ++saves; return save_ok; }
int Platform_RestartGame(void) { ++restarts; return -1; }
static int event(MenuEventType type, int x, int y, MenuKey key)
{
    MenuEvent e = {0};
    e.type = type; e.x = x; e.y = y; e.button = 1; e.key = key;
    return ModsWindow_Event(&e);
}
static int click(int x, int y)
{
    int r = event(MENU_EVENT_BUTTON_DOWN, x, y, MENU_KEY_OTHER);
    event(MENU_EVENT_BUTTON_UP, x, y, MENU_KEY_OTHER);
    return r;
}
static void drag(int from_x, int from_y, int to_x, int to_y)
{
    event(MENU_EVENT_BUTTON_DOWN, from_x, from_y, MENU_KEY_OTHER);
    event(MENU_EVENT_MOTION, to_x, to_y, MENU_KEY_OTHER);
    event(MENU_EVENT_BUTTON_UP, to_x, to_y, MENU_KEY_OTHER);
}
static int key(MenuKey k) { return event(MENU_EVENT_KEY_DOWN, 0, 0, k); }
int main(void)
{
    int w, h;
    ModsWindow_Init(); ModsWindow_Size(&w, &h);
    assert(w == 602 && h == 264); /* the footer's widest message row sets the width */
    /* Select 3D Monsters in Available and press the apply arrow. */
    click(LEFT_ROW_X, ROW0_Y); click(w / 2, ARROW_APPLY_Y);
    assert(live[0] && saves == 1 && !restarts);
    /* The apply arrow does nothing while the selection is already applied. */
    click(w / 2, ARROW_APPLY_Y);
    assert(saves == 1);
    /* Drag it back to Available, then to Applied again. */
    drag(RIGHT_ROW_X(w), ROW0_Y, LEFT_ROW_X, ROW0_Y + 30);
    assert(!live[0] && saves == 2);
    drag(LEFT_ROW_X, ROW0_Y, RIGHT_ROW_X(w), ROW0_Y);
    assert(live[0] && saves == 3);
    /* A click, a release inside the same row, or a drop outside a list must not apply a mod. */
    click(LEFT_ROW_X, ROW0_Y);
    drag(LEFT_ROW_X, ROW0_Y, LEFT_ROW_X + 20, ROW0_Y + 2);
    drag(LEFT_ROW_X, ROW0_Y, w / 2, FOOTER_Y(h));
    assert(!live[1] && saves == 3);
    /* Keyboard: Right applies, Left removes. */
    key(MENU_KEY_RIGHT);
    assert(live[1] && saves == 4);
    key(MENU_KEY_LEFT);
    assert(!live[1] && saves == 5);
    key(MENU_KEY_LEFT);
    assert(saves == 5);
    /* A restart-only mod asks first; Cancel leaves it, the primary button confirms. */
    restart_required = 1;
    click(w / 2, ARROW_APPLY_Y);
    assert(!restarts && saves == 5);
    assert(!click(CANCEL_X(w), FOOTER_Y(h)));
    assert(!restarts && !stored[1]);
    click(w / 2, ARROW_APPLY_Y);
    click(PRIMARY_X(w), FOOTER_Y(h)); /* Confirm, simulated exec failure. */
    assert(restarts == 1 && stored[1] && !live[1]);
    /* Escape cancels a pending warning rather than closing. A mod that
     * needs a restart reads back as applied once it is recorded, so it is
     * removing one that asks the second time. */
    click(RIGHT_ROW_X(w), ROW0_Y); click(w / 2, ARROW_REMOVE_Y);
    assert(!key(MENU_KEY_ESCAPE) && restarts == 1);
    /* Failed persistence rolls back a live change. */
    restart_required = 0; save_ok = 0;
    ModsWindow_Init(); click(RIGHT_ROW_X(w), ROW0_Y); click(w / 2, ARROW_REMOVE_Y);
    assert(live[0] && stored[0]);
    /* A mod that failed to load says so instead of being applied. */
    mod_status[1] = "its library could not be opened";
    stored[1] = live[1] = 0;
    ModsWindow_Init(); save_ok = 1; saves = 0;
    click(LEFT_ROW_X, ROW0_Y); click(w / 2, ARROW_APPLY_Y);
    assert(!saves && !live[1]);
    mod_status[1] = "";
    /* Close and Escape both close the window. */
    assert(click(CLOSE_X(w), FOOTER_Y(h)));
    assert(key(MENU_KEY_ESCAPE));
    return 0;
}
