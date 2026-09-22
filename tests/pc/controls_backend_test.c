/* Real SDL3 devices and OpenGL contexts; no guest/game assets needed. */
#include "../../src/pc/platform/sdl.c"
#include <assert.h>
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
void ModsWindow_Init(void) {}
void ModsWindow_Size(int *w, int *h)
{
    *w = 780;
    *h = 294;
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
    char dir[] = "/tmp/memories-backend-XXXXXX", path[256], error[256];
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/controls.txt", dir);
    setenv("MEMORIES_CONTROLS", path, 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    window = SDL_CreateWindow("Controls integration", 320, 240, SDL_WINDOW_OPENGL);
    assert(window);
    gl_context = SDL_GL_CreateContext(window);
    assert(gl_context);
    use_gl = 1;
    SDL_JoystickID ids[3];
    SDL_Joystick *sticks[3];
    for (int i = 0; i < 3; i++) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.name = "Controls virtual test";
        ids[i] = SDL_AttachVirtualJoystick(&desc);
        assert(ids[i]);
        sticks[i] = SDL_OpenJoystick(ids[i]);
        assert(sticks[i]);
        assert(SDL_SetJoystickVirtualAxis(sticks[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768));
        assert(SDL_SetJoystickVirtualAxis(sticks[i], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768));
    }
    /* Register the fixtures first so real connected pads cannot shift their slots. */
    for (int i = 0; i < 3; i++)
        open_gamepad(ids[i]);
    update();
    assert(pads[0] && pads[1] && pads[2]);
    ControlsConfig cfg = *ControlsRuntime_Config();
    cfg.port[0].mode = 2;
    strcpy(cfg.port[0].identity, ControlsRuntime_Device(2)->identity);
    assert(ControlsRuntime_Apply(&cfg, error, sizeof(error)));
    update();
    assert(ControlsRuntime_Assigned(&cfg, 0) == 2);
    assert(SDL_SetJoystickVirtualButton(sticks[2], SDL_GAMEPAD_BUTTON_SOUTH, true));
    update();
    assert(Gamepad_Bits(0) == 0x4000);
    assert(Gamepad_Bits(1) == 0);
    assert(SDL_SetJoystickVirtualButton(sticks[2], SDL_GAMEPAD_BUTTON_SOUTH, false));
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_LEFTX, 25000));
    update();
    assert(Gamepad_Bits(0) == 0x20);
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_LEFTX, 0));
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767));
    update();
    assert(Gamepad_Bits(0) == 0x200);
    assert(SDL_SetJoystickVirtualAxis(sticks[2], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768));
    update();
    for (int i = 0; i < 3; i++) {
        GLint unpack;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 320);
        Platform_OpenMods();
        Platform_OpenControls();
        assert(controls_window && mods_window);
        assert(SDL_GL_GetCurrentContext() == gl_context);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
        assert(unpack == 320);
        assert(SDL_SetJoystickVirtualButton(sticks[2], SDL_GAMEPAD_BUTTON_SOUTH, true));
        update();
        assert(Gamepad_Bits(0) == 0);
        draw_controls();
        draw_mods();
        assert(SDL_GL_GetCurrentContext() == gl_context);
        close_mods();
        close_controls();
        update();
        assert(Gamepad_Bits(0) == 0);
        assert(SDL_SetJoystickVirtualButton(sticks[2], SDL_GAMEPAD_BUTTON_SOUTH, false));
        update();
        assert(SDL_SetJoystickVirtualButton(sticks[2], SDL_GAMEPAD_BUTTON_SOUTH, true));
        update();
        assert(Gamepad_Bits(0) == 0x4000);
        assert(SDL_SetJoystickVirtualButton(sticks[2], SDL_GAMEPAD_BUTTON_SOUTH, false));
        update();
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack);
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
    for (int i = 0; i < 3; i++) {
        SDL_CloseJoystick(sticks[i]);
        if (i < 2)
            assert(SDL_DetachVirtualJoystick(ids[i]));
    }
    update();
    destroy_window();
    SDL_Quit();
    unlink(path);
    rmdir(dir);
    puts("controls SDL: third-device selection, buttons/axes/triggers, removal, input gate and GL ownership "
         "passed");
    return 0;
}
