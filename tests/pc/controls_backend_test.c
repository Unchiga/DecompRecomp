/* Real SDL3 devices and OpenGL contexts; no guest/game assets needed. */
#include "../../src/pc/platform/sdl.c"
#include <assert.h>
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>
int Menu_Scale(void) { return 1; }
int Menu_TextWidthScaled(const char *s, int sc) { return (int)strlen(s) * 7 * sc; }
void Menu_DrawTextScaled(MenuCanvas *c, int x, int y, const char *s, uint32_t color, int sc)
{
    (void)c;
    (void)x;
    (void)y;
    (void)s;
    (void)color;
    (void)sc;
}
void Monitor_Modal(int on) { (void)on; }
void ModsWindow_Init(void) {}
void ModsWindow_Size(int *w, int *h)
{
    *w = 780;
    *h = 294;
}
void ModsWindow_Resize(int w, int h)
{
    (void)w;
    (void)h;
}
void ModsWindow_Draw(MenuCanvas *c) { memset(c->pixels, 0x55, (size_t)c->stride * c->height * 4); }
static void update(void)
{
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    Gamepad_Poll(1);
}
static void controls_click(int x, int y)
{
    SDL_Event event;
    MenuEvent menu;
    memset(&event, 0, sizeof(event));
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = SDL_GetWindowID(controls_window);
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = (float)x;
    event.button.y = (float)y;
    translate(&event, &menu);
    assert(dispatch_controls(&event, &menu));
    ControlsWindow_Tick();
    draw_controls();
}
/* Click a widget where the window actually drew it, whatever the layout. */
static void controls_click_id(int id)
{
    int x, y;
    assert(ControlsWindow_Locate(id, &x, &y));
    controls_click(x, y);
}
static void controls_press(SDL_Scancode code, int down)
{
    SDL_Event event;
    MenuEvent menu;
    memset(&event, 0, sizeof(event));
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = SDL_GetWindowID(controls_window);
    event.key.scancode = code;
    event.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
    event.key.down = down != 0;
    translate(&event, &menu);
    assert(dispatch_controls(&event, &menu));
}
int main(void)
{
    char dir[SCRATCH_MAX], path[SCRATCH_MAX + 64], error[256];
    scratch_template(dir, sizeof(dir), "memories-backend");
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/controls.txt", dir);
    setenv("MEMORIES_CONTROLS", path, 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    int software = getenv("MEMORIES_TEST_SOFTWARE") != NULL;
    window = SDL_CreateWindow("Controls integration", 320, 240, software ? 0 : SDL_WINDOW_OPENGL);
    if (!window) fprintf(stderr, "SDL window: %s\n", SDL_GetError());
    assert(window);
    if (!software) {
        gl_context = SDL_GL_CreateContext(window);
        assert(gl_context);
        use_gl = 1;
    }
    SDL_JoystickID ids[4];
    SDL_Joystick *sticks[4];
    const char *types[] = {"PS5", "SwitchPro", "XboxOne", "Standard"};
    const CtrlIconStyle styles[] = {CTRL_ICON_PLAYSTATION, CTRL_ICON_NINTENDO, CTRL_ICON_XBOX, CTRL_ICON_GENERIC};
    for (int i = 0; i < 4; i++) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.nhats = 1;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.name = types[i];
        desc.vendor_id = 0x1209;
        desc.product_id = (Uint16)(0xf000 + i);
        ids[i] = SDL_AttachVirtualJoystick(&desc);
        assert(ids[i]);
        char mapping[512];
        /* Deliberately nonstandard face order and digital triggers: exercise
         * SDL mapping rather than assuming raw Xbox button/axis numbering. */
        snprintf(mapping, sizeof(mapping),
                 "none,%s,a:b3,b:b2,x:b1,y:b0,back:b4,start:b6,leftx:a0,lefty:a1,"
                 "rightx:a2,righty:a3,lefttrigger:b9,righttrigger:a5,dpup:h0.1,"
                 "touchpad:b20,paddle1:b16,paddle2:b17,paddle3:b18,"
                 "paddle4:b19,misc3:b22,misc4:b23,misc5:b24,misc6:b25,type:%s,", types[i], types[i]);
        assert(SDL_SetGamepadMapping(ids[i], mapping));
        sticks[i] = SDL_OpenJoystick(ids[i]);
        assert(sticks[i]);
        assert(SDL_SetJoystickVirtualAxis(sticks[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768));
        assert(SDL_SetJoystickVirtualAxis(sticks[i], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768));
    }
    /* Register the fixtures first so real connected pads cannot shift their slots. */
    for (int i = 0; i < 4; i++)
        open_gamepad(ids[i]);
    update();
    assert(pads[0] && pads[1] && pads[2]);
    for (int i = 0; i < 4; i++) {
        ControllerDevice *d = ControlsRuntime_Device(i);
        assert(d->style == styles[i]);
        assert(SDL_SetJoystickVirtualButton(sticks[i], 3, true));
        assert(SDL_SetJoystickVirtualButton(sticks[i], 9, true));
        assert(SDL_SetJoystickVirtualHat(sticks[i], 0, SDL_HAT_UP));
        update();
        assert(d->snapshot.buttons_down == (1u << (CTRL_BTN_SOUTH - 1)));
        assert(d->snapshot.trigger[CTRL_TRIGGER_LEFT] == 1);
        assert(d->snapshot.hat_down == CTRL_HAT_UP);
        assert(SDL_SetJoystickVirtualButton(sticks[i], 3, false));
        assert(SDL_SetJoystickVirtualButton(sticks[i], 9, false));
        assert(SDL_SetJoystickVirtualHat(sticks[i], 0, SDL_HAT_CENTERED));
        const int raw[] = {20, 16, 17, 18, 19, 22, 23, 24, 25};
        for (int b = 0; b < 9; b++) {
            assert(SDL_SetJoystickVirtualButton(sticks[i], raw[b], true));
            update();
            assert(d->snapshot.buttons_down == (1u << (CTRL_BTN_TOUCHPAD + b - 1)));
            ControlSource sources[64];
            assert(ControlsRuntime_Sources(d, sources, 64, 0) == 1);
            assert(sources[0].code == CTRL_BTN_TOUCHPAD + b);
            assert(SDL_SetJoystickVirtualButton(sticks[i], raw[b], false));
        }
        update();
    }
    ControlsConfig cfg = *ControlsRuntime_Config();
    cfg.port[0].mode = 2;
    strcpy(cfg.port[0].identity, ControlsRuntime_Device(2)->identity);
    assert(ControlsRuntime_Apply(&cfg, error, sizeof(error)));
    update();
    assert(ControlsRuntime_Assigned(&cfg, 0) == 2);
    assert(SDL_SetJoystickVirtualButton(sticks[2], 3, true));
    update();
    assert(Gamepad_Bits(0) == 0x4000);
    assert(Gamepad_Bits(1) == 0);
    assert(SDL_SetJoystickVirtualButton(sticks[2], 3, false));
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_LEFTX, 25000));
    update();
    assert(Gamepad_Bits(0) == 0x20);
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_LEFTX, 0));
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767));
    update();
    assert(Gamepad_Bits(0) == 0x200);
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768));
    update();
    for (int i = 0; i < 4; i++) {
        GLint unpack = 320;
        if (!software) glPixelStorei(GL_UNPACK_ROW_LENGTH, 320);
        Platform_OpenMods();
        Platform_OpenControls();
        assert(controls_window && mods_window);
        assert(software || SDL_GL_GetCurrentContext() == gl_context);
        if (!software) glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 320);
        assert(SDL_SetJoystickVirtualButton(sticks[2], 3, true));
        update();
        assert(Gamepad_Bits(0) == 0);
        draw_controls();
        draw_mods();
        assert(software || SDL_GL_GetCurrentContext() == gl_context);
        close_mods();
        close_controls();
        update();
        assert(Gamepad_Bits(0) == 0);
        assert(SDL_SetJoystickVirtualButton(sticks[2], 3, false));
        update();
        assert(SDL_SetJoystickVirtualButton(sticks[2], 3, true));
        update();
        assert(Gamepad_Bits(0) == 0x4000);
        assert(SDL_SetJoystickVirtualButton(sticks[2], 3, false));
        update();
        if (!software) glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 320);
        assert(!quit);
    }
    /* Dispatch real SDL events through the same gate used by pump(). */
    Platform_OpenControls();
    controls_click_id(CONTROLS_UI_BINDING + 14 * 2); /* the Cross binding */
    controls_click_id(CONTROLS_UI_REBIND);
    controls_press(SDL_SCANCODE_F5, 1);
    controls_press(SDL_SCANCODE_F5, 0); /* reserved; cannot escape UI */
    assert(ControlsRuntime_Config()->kb.src[14][0].code == CTRL_KEY_X);
    controls_press(SDL_SCANCODE_B, 1);
    controls_press(SDL_SCANCODE_B, 0);
    controls_click_id(CONTROLS_UI_APPLY);
    controls_click_id(CONTROLS_UI_CONTROLLER);
    controls_click_id(CONTROLS_UI_BINDING + 14 * 2);
    controls_click_id(CONTROLS_UI_REBIND);
    assert(SDL_SetJoystickVirtualButton(sticks[2], 20, true));
    update();
    ControlsWindow_Tick();
    draw_controls();
    assert(SDL_SetJoystickVirtualButton(sticks[2], 20, false));
    update();
    controls_click_id(CONTROLS_UI_APPLY);
    cfg = *ControlsRuntime_Config();
    assert(ControlsRuntime_Profile(&cfg, 0, 0)->src[14][0].code == CTRL_BTN_TOUCHPAD);
    /* The window is resizable: the canvas follows, and the layout still fits
     * inside it. The window manager cannot go below the minimum. */
    {
        int min_w = 0, min_h = 0, x = 0, y = 0;
        SDL_Event resize;
        MenuEvent none;
        assert(SDL_GetWindowMinimumSize(controls_window, &min_w, &min_h) && min_w > 0 && min_h > 0);
        memset(&resize, 0, sizeof(resize));
        memset(&none, 0, sizeof(none));
        resize.type = SDL_EVENT_WINDOW_RESIZED;
        resize.window.windowID = SDL_GetWindowID(controls_window);
        resize.window.data1 = min_w;
        resize.window.data2 = min_h;
        assert(dispatch_controls(&resize, &none));
        assert(controls_canvas.width == min_w && controls_canvas.height == min_h);
        assert(ControlsWindow_Locate(CONTROLS_UI_OK, &x, &y));
        assert(x > 0 && x < min_w && y > 0 && y < min_h);
    }
    assert(ControlsRuntime_Config()->kb.src[14][0].code == CTRL_KEY_B);
    assert(!quit);
    close_controls();
    assert(SDL_DetachVirtualJoystick(ids[2]));
    update();
    assert(!Gamepad_Connected(0));
    assert(!Gamepad_Bits(0));
    for (int i = 0; i < 4; i++) {
        SDL_CloseJoystick(sticks[i]);
        if (i != 2)
            assert(SDL_DetachVirtualJoystick(ids[i]));
    }
    update();
    destroy_window();
    SDL_Quit();
    unlink(path);
    rmdir(dir);
    printf("Rendering: %s\n", software ? "software (GL ownership not tested)" : "OpenGL");
    puts("controls SDL: PS5/Nintendo/Xbox/generic mappings, extra buttons, digital triggers, hats, selection, removal and UI "
         "passed");
    return 0;
}
