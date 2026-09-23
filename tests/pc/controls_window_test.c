#define _POSIX_C_SOURCE 200809L
/* Include the UI for deterministic interaction tests without a native display. */
#include "../../src/pc/platform/controls_window.c"
#include <assert.h>
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>
static int test_scale = 1;
int Menu_Scale(void) { return test_scale; }
int Menu_TextWidth(const char *s) { return (int)strlen(s) * 7 * test_scale; }
void Menu_DrawText(MenuCanvas *c, int x, int y, const char *s, uint32_t color)
{
    (void)c;
    (void)x;
    (void)y;
    (void)s;
    (void)color;
}
int Menu_TextWidthScaled(const char *s, int sc) { return (int)strlen(s) * 7 * sc; }
void Menu_DrawTextScaled(MenuCanvas *c, int x, int y, const char *s, uint32_t color, int sc)
{
    (void)sc;
    Menu_DrawText(c, x, y, s, color);
}
int main(void)
{
    char dir[SCRATCH_MAX], path[SCRATCH_MAX + 64];
    scratch_template(dir, sizeof(dir), "memories-window");
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/controls.txt", dir);
    setenv("MEMORIES_CONTROLS", path, 1);
    MenuCanvas c = {0};
    c.width = 900;
    c.height = 650;
    c.stride = c.width;
    c.pixels = calloc((size_t)c.width * c.height, 4);
    assert(c.pixels);
    ControlsWindow_Init();
    ControlsWindow_Draw(&c);
    assert(!dirty());
    MenuEvent binding_click = {.type = MENU_EVENT_BUTTON_DOWN, .button = 1};
    for (int i = 0; i < ui.count; i++)
        if (ui.hits[i].id == ROW + 14 * 2) {
            binding_click.x = ui.hits[i].x + 5;
            binding_click.y = ui.hits[i].y + 5;
        }
    ControlsWindow_Event(&binding_click);
    assert(ui.row == 14 && ui.slot == 0 && ui.capture.state == CAP_IDLE);
    ControlsWindow_Event(&binding_click);
    assert(ui.capture.state == CAP_WAIT_NEUTRAL);
    cancel_capture();
    ui.slot = 0;
    ui.row = 14;
    ControlsRuntime_Key(CTRL_KEY_ENTER, 1);
    start_capture();
    ControlsWindow_Tick();
    assert(ui.capture.state == CAP_WAIT_NEUTRAL);
    ControlsRuntime_Key(CTRL_KEY_ENTER, 0);
    ControlsWindow_Tick();
    assert(ui.capture.state == CAP_AWAIT_INPUT);
    ControlsRuntime_Key(CTRL_KEY_B, 1);
    ControlsWindow_Tick();
    assert(ui.capture.state == CAP_IDLE);
    assert(dirty());
    assert(ui.draft.kb.src[14][0].code == CTRL_KEY_B);
    assert(ControlsRuntime_Config()->kb.src[14][0].code == CTRL_KEY_X);
    ControlsRuntime_Key(CTRL_KEY_B, 0);
    activate(APPLY);
    assert(!dirty());
    assert(ControlsRuntime_Config()->kb.src[14][0].code == CTRL_KEY_B);
    start_capture();
    ControlsWindow_Tick();
    ControlsRuntime_Key(CTRL_KEY_A, 1);
    ControlsWindow_Tick();
    assert(ui.modal == 3);
    activate(NO);
    assert(ui.draft.kb.src[14][0].code == CTRL_KEY_B);
    ControlsRuntime_Key(CTRL_KEY_A, 0);
    start_capture();
    ControlsWindow_Tick();
    ControlsRuntime_Key(CTRL_KEY_A, 1);
    ControlsWindow_Tick();
    activate(YES);
    assert(ui.draft.kb.src[12][0].kind == CTRL_SRC_UNBOUND);
    assert(ui.draft.kb.src[14][0].code == CTRL_KEY_A);
    ControlsWindow_RequestClose();
    assert(ui.modal == 1);
    activate(KEEP);
    assert(!ControlsWindow_ShouldClose());
    activate(CANCEL);
    assert(ControlsWindow_ShouldClose());
    assert(ControlsRuntime_Config()->kb.src[14][0].code == CTRL_KEY_B);
    ControlsRuntime_ResetKeys();
    ControlsWindow_Init();
    activate(DEFAULTS);
    activate(NO);
    assert(!dirty());
    activate(DEFAULTS);
    activate(YES);
    assert(dirty());
    activate(APPLY);
    assert(!dirty());
    for (int i = 0; i < 3; i++) {
        ControllerDevice *d = ControlsRuntime_Device(i);
        d->connected = 1;
        snprintf(d->identity, sizeof(d->identity), "test:%d", i);
        snprintf(d->name, sizeof(d->name), "Synthetic pad %d", i);
    }
    activate(CONTROLLER);
    ui.popup = DEVICE;
    activate(CHOICE + 4);
    assert(current_device() == 2);
    /* A non-PlayStation pad still displays PS1 destinations. Remapping South
     * to Circle must highlight Circle, not the physical south/Cross position. */
    ControllerDevice *selected = ControlsRuntime_Device(2);
    selected->style = CTRL_ICON_XBOX;
    selected->snapshot.buttons_down = 1u << (CTRL_BTN_SOUTH - 1);
    ControlSource south = {CTRL_SRC_BUTTON, CTRL_BTN_SOUTH, 0};
    assert(Controls_MoveSource(profile(1), 13, 0, &south));
    ControlsWindow_Draw(&c);
    /* The drawn buttons, which are smaller than their mouse targets. */
    Rect circle = pad_rect[13], cross = pad_rect[14];
    assert(circle.w && cross.w);
    assert(c.pixels[(circle.y + circle.h / 2) * c.stride + circle.x + circle.w / 2] == held);
    /* Cross keeps the diagram panel's own background: it is not highlighted. */
    assert(c.pixels[(cross.y + cross.h / 2 + 2) * c.stride + cross.x + cross.w / 2 + 5] == panel);
    uint32_t *picture = malloc((size_t)c.stride * c.height * 4);
    assert(picture);
    memcpy(picture, c.pixels, (size_t)c.stride * c.height * 4);
    selected->style = CTRL_ICON_NINTENDO;
    ControlsWindow_Draw(&c);
    assert(!memcmp(picture, c.pixels, (size_t)c.stride * c.height * 4));
    free(picture);
    MenuEvent click = {0};
    click.type = MENU_EVENT_BUTTON_DOWN;
    click.button = 1;
    click.x = cross.x + 5;
    click.y = cross.y + 5;
    ControlsWindow_Event(&click);
    assert(ui.row == 14 && ui.slot == 0);
    assert(ui.capture.state == CAP_IDLE);
    ControlsWindow_Event(&click);
    assert(ui.capture.state == CAP_WAIT_NEUTRAL);
    cancel_capture();
    selected->snapshot.buttons_down = 0;
    ui.row = 15;
    start_capture();
    ControlsWindow_Tick();
    assert(ui.capture.state == CAP_AWAIT_INPUT);
    ControlsRuntime_Device(2)->connected = 0;
    ControlsWindow_Tick();
    assert(!ui.capture.state);
    /* Every pad button owns a mouse target, no two targets overlap, and each
     * target contains the button it highlights: a click can only ever mean
     * one button. */
    ControlsWindow_Init();
    activate(CONTROLLER);
    ControlsWindow_Draw(&c);
    /* L3 and R3 have no place on this artwork and live only in the list. */
    Rect targets[CTRL_DEST_COUNT];
    for (int dest = 0; dest < CTRL_DEST_COUNT; dest++) {
        targets[dest] = (Rect){0, 0, 0, 0};
        for (int i = 0; i < ui.count; i++)
            if (ui.hits[i].id == DIAGRAM + dest)
                targets[dest] = (Rect){ui.hits[i].x, ui.hits[i].y, ui.hits[i].w, ui.hits[i].h};
        if (dest == 1 || dest == 2) {
            assert(!targets[dest].w && !pad_rect[dest].w);
            continue;
        }
        assert(targets[dest].w > 0 && targets[dest].h > 0);
        Rect b = pad_rect[dest];
        assert(b.x + b.w / 2 >= targets[dest].x && b.x + b.w / 2 < targets[dest].x + targets[dest].w);
        assert(b.y + b.h / 2 >= targets[dest].y && b.y + b.h / 2 < targets[dest].y + targets[dest].h);
    }
    for (int a = 0; a < CTRL_DEST_COUNT; a++)
        for (int b = a + 1; b < CTRL_DEST_COUNT; b++) {
            Rect *p = &targets[a], *q = &targets[b];
            if (!p->w || !q->w)
                continue;
            int across = p->x < q->x + q->w && q->x < p->x + p->w;
            int down = p->y < q->y + q->h && q->y < p->y + p->h;
            assert(!(across && down));
        }
    /* Clicking a button's own pixels selects that button, not a neighbour. */
    for (int dest = 0; dest < CTRL_DEST_COUNT; dest++) {
        int px, py;
        if (dest == 1 || dest == 2)
            continue;
        assert(ControlsWindow_Locate(DIAGRAM + dest, &px, &py));
        MenuEvent tap = {.type = MENU_EVENT_BUTTON_DOWN, .button = 1, .x = px, .y = py};
        ControlsWindow_Event(&tap);
        assert(ui.row == dest);
        ControlsWindow_Draw(&c);
    }

    /* Arrows walk the table in reading order, not in wire-bit order, and
     * wrap from the first line (Up) to the last (Select). */
    ControlsWindow_Init();
    ControlsWindow_Draw(&c);
    assert(ui.row == 4);
    ControlsWindow_Key(CTRL_KEY_ARROW_DOWN, 1, 0, 0);
    assert(ui.row == 6 && ui.focus == ROW + 6 * 2);
    ControlsWindow_Key(CTRL_KEY_ARROW_UP, 1, 0, 0);
    ControlsWindow_Key(CTRL_KEY_ARROW_UP, 1, 0, 0);
    assert(ui.row == 0);
    ControlsWindow_Draw(&c);

    /* Delete empties the focused slot; the active configuration is untouched
     * until Apply. */
    assert(ui.draft.kb.src[0][0].kind != CTRL_SRC_UNBOUND);
    ControlsWindow_Key(CTRL_KEY_DELETE, 1, 0, 0);
    assert(ui.draft.kb.src[0][0].kind == CTRL_SRC_UNBOUND);
    assert(ControlsRuntime_Config()->kb.src[0][0].kind != CTRL_SRC_UNBOUND);
    activate(CANCEL);

    /* The pointer lights up whatever it is over, and nothing when it misses. */
    ControlsWindow_Init();
    ControlsWindow_Draw(&c);
    int ox, oy;
    assert(ControlsWindow_Locate(OK, &ox, &oy));
    MenuEvent motion = {.type = MENU_EVENT_MOTION, .x = ox, .y = oy};
    ControlsWindow_Event(&motion);
    assert(ui.hover == OK);
    motion.x = motion.y = 2;
    ControlsWindow_Event(&motion);
    assert(!ui.hover);

    /* Wheel scrolling stays inside the list. */
    MenuEvent wheel = {.type = MENU_EVENT_WHEEL, .wheel = -1};
    for (int i = 0; i < 60; i++)
        ControlsWindow_Event(&wheel);
    ControlsWindow_Draw(&c);
    assert(ui.scroll == ui.scroll_max);
    wheel.wheel = 1;
    for (int i = 0; i < 60; i++)
        ControlsWindow_Event(&wheel);
    assert(ui.scroll == 0);

    /* At the smallest size the picture drops out, the table scrolls, and the
     * footer is still complete. */
    int min_w, min_h;
    ControlsWindow_MinSize(&min_w, &min_h);
    MenuCanvas small = {0};
    small.width = min_w;
    small.height = min_h;
    small.stride = min_w;
    small.pixels = calloc((size_t)min_w * min_h, 4);
    assert(small.pixels);
    ControlsWindow_Init();
    ControlsWindow_Draw(&small);
    assert(ui.lines_shown >= 1 && ui.scroll_max > 0);
    assert(!ControlsWindow_Locate(DIAGRAM + 14, &ox, &oy));
    assert(ControlsWindow_Locate(OK, &ox, &oy) && ControlsWindow_Locate(APPLY, &ox, &oy));
    assert(ControlsWindow_Locate(CANCEL, &ox, &oy) && ControlsWindow_Locate(DEFAULTS, &ox, &oy));
    assert(ox >= 0 && ox < min_w && oy >= 0 && oy < min_h);
    free(small.pixels);

    ControlsWindow_Init();
    for (int sc = 1; sc <= 2; sc++) {
        test_scale = sc;
        ControlsWindow_Draw(&c);
        assert(ui.count > 0);
    }
    test_scale = 1;
    free(c.pixels);
    unlink(path);
    rmdir(dir);
    puts("controls window: capture, draft, conflicts, close and layout passed");
    return 0;
}
