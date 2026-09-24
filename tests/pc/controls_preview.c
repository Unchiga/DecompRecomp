/* Renders the Controls window to PPM files for review, using the real menu
 * font renderer without booting guest code. Interaction goes through the
 * window's own keyboard API (Tab moves focus, Enter activates), so the
 * pictures stay correct when the layout changes. */
#include "pc/compat/fs.h"
#include "../../src/pc/platform/menu.c"
#include "pc/platform/controls_window.h"
#include <assert.h>
/* The window lays itself out while drawing, so focus movement needs a frame
 * between key presses, exactly as the running game gives it. */
static MenuCanvas *frame_target;
static void key(int code)
{
    ControlsWindow_Key(code, 1, 0, 0);
    ControlsWindow_Key(code, 0, 0, 0);
    ControlsWindow_Draw(frame_target);
}
/* Init clears the layout with the rest of the state, so draw once before
 * pressing anything, exactly as opening the window does. */
static void reset(void)
{
    ControlsWindow_Init();
    ControlsWindow_Draw(frame_target);
}
static void tab_to(int times)
{
    for (int i = 0; i < times; i++)
        key(CTRL_KEY_TAB);
}
static void save(MenuCanvas *c, const char *path)
{
    frame_target = c;
    ControlsWindow_Draw(c);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", c->width, c->height);
    for (int y = 0; y < c->height; y++)
        for (int x = 0; x < c->width; x++) {
            uint32_t p = c->pixels[y * c->stride + x];
            unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            assert(fwrite(rgb, 1, 3, f) == 3);
        }
    assert(!fclose(f));
}
static MenuCanvas make(int w, int h)
{
    MenuCanvas c = {0};
    c.width = w;
    c.height = h;
    c.stride = w;
    c.pixels = calloc((size_t)w * h, 4);
    assert(c.pixels);
    return c;
}
int main(void)
{
    assert(menus[MENU_GAME].count == 5);
    assert(menus[MENU_GAME].items[0].id == ACT_CONTROLS);
    load_font();
    int width, height;
    ControlsWindow_Size(&width, &height);
    MenuCanvas c = make(width, height);
    frame_target = &c;
    reset();
    save(&c, "tmp/pc/controls-keyboard.ppm");

    for (int i = 0; i < 3; i++) {
        ControllerDevice *d = ControlsRuntime_Device(i);
        d->connected = 1;
        d->style = CTRL_ICON_XBOX;
        snprintf(d->identity, sizeof(d->identity), "preview:%d", i);
        snprintf(d->name, sizeof(d->name), "Xbox Wireless Controller %d", i + 1);
    }
    /* Focus starts on the Keyboard tab: Tab reaches Controller, Enter picks it. */
    tab_to(1);
    key(CTRL_KEY_ENTER);
    ControlsWindow_Draw(&c);
    ControlsRuntime_Device(0)->snapshot.buttons_down = 1; /* South: Cross lights up */
    key(CTRL_KEY_ARROW_DOWN);
    key(CTRL_KEY_ARROW_DOWN);
    save(&c, "tmp/pc/controls-controller.ppm");

    ControlsRuntime_Device(0)->style = CTRL_ICON_PLAYSTATION;
    snprintf(ControlsRuntime_Device(0)->name, sizeof(ControlsRuntime_Device(0)->name), "DualSense (preview)");
    save(&c, "tmp/pc/controls-playstation.ppm");
    ControlsRuntime_Device(0)->style = CTRL_ICON_NINTENDO;
    snprintf(ControlsRuntime_Device(0)->name, sizeof(ControlsRuntime_Device(0)->name), "Switch Pro (preview)");
    save(&c, "tmp/pc/controls-nintendo.ppm");

    /* The device list, opened from a fresh window. */
    reset();
    tab_to(1);
    key(CTRL_KEY_ENTER); /* Controller tab */
    tab_to(1);
    key(CTRL_KEY_ENTER); /* open the device list */
    key(CTRL_KEY_ARROW_DOWN);
    key(CTRL_KEY_ARROW_DOWN);
    key(CTRL_KEY_ARROW_DOWN); /* the first real controller */
    save(&c, "tmp/pc/controls-devices.ppm");
    key(CTRL_KEY_ENTER);

    /* Listening for a new binding. */
    key(CTRL_KEY_ARROW_DOWN);
    key(CTRL_KEY_ENTER);
    ControlsWindow_Tick();
    save(&c, "tmp/pc/controls-capture.ppm");
    key(CTRL_KEY_ESCAPE);

    /* The confirmation dialog, with a draft change behind it. */
    ControlsRuntime_Device(0)->snapshot.buttons_down = 0;
    key(CTRL_KEY_ENTER);
    ControlsWindow_Tick();
    ControlsRuntime_Device(0)->snapshot.buttons_down = 1u << (CTRL_BTN_NORTH - 1);
    ControlsWindow_Tick();
    ControlsWindow_Tick();
    ControlsRuntime_Device(0)->snapshot.buttons_down = 0;
    ControlsWindow_RequestClose();
    save(&c, "tmp/pc/controls-dialog-conflict.ppm");
    key(CTRL_KEY_ESCAPE);

    /* The closing dialog, over a draft with an unsaved change. */
    key(CTRL_KEY_ARROW_DOWN);
    key(CTRL_KEY_DELETE);
    ControlsWindow_RequestClose();
    save(&c, "tmp/pc/controls-dialog-close.ppm");
    key(CTRL_KEY_ESCAPE);

    for (int i = 0; i < 3; i++)
        ControlsRuntime_Device(i)->connected = 0;
    save(&c, "tmp/pc/controls-disconnected.ppm");
    free(c.pixels);

    /* A window at its minimum size: no picture, the table scrolls. */
    ControlsWindow_MinSize(&width, &height);
    MenuCanvas small = make(width, height);
    frame_target = &small;
    reset();
    save(&small, "tmp/pc/controls-small.ppm");
    free(small.pixels);

    c = make(1400, 1000);
    frame_target = &c;
    ui = 2;
    load_font();
    reset();
    save(&c, "tmp/pc/controls-scaled.ppm");
    font_loaded = 0;
    save(&c, "tmp/pc/controls-fallback-font.ppm");
    free(c.pixels);
    return 0;
}
