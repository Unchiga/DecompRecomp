/* The SDL3 backend: window, input, controllers and audio through one
 * library that exists for Linux, Windows and macOS, chosen over a hand-made
 * layer per system. The picture is a 320x240 (whatever the game presents)
 * streaming texture that the GPU scales with nearest filtering, and the
 * menu bar is a second, transparent texture blended over it, so the CPU
 * never scales a frame and menu activity uploads only the rectangle it
 * touched. Both textures are drawn to the window each time anything
 * changes; that is a couple of GPU quads.
 *
 * The interrupt clock (platform_common.c) is a signal on the main thread,
 * so signals are blocked while SDL creates its threads, which inherit the
 * mask. See platform.h for the contract. */
#define _GNU_SOURCE
#include "platform.h"
#include "menu.h"
#include "settings.h"
#include "pc/audio/spu.h"
#include "pc/debug/cheats.h"
#include "pc/debug/log.h"
#include "pc/debug/hud.h"
#include "pc/guest/state.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *picture, *overlay;
static int picture_w, picture_h;
static uint32_t *picture_pixels, *overlay_pixels;
static MenuCanvas canvas;
/* Menu/input geometry is in SDL window coordinates (logical pixels). The
 * renderer target may have more physical pixels on a high-DPI display. */
static struct { int win_w, win_h; float pixel_x, pixel_y; SDL_FRect dst; } layout;
static int menu_visible = 1;
static int display_settings_pending;
static int menu_reveal_frames;
/* 4 puts the 320x240 picture on screen at 1280x960. */
static int scale = 4, pending_scale, quit, state_slot = 1;
static struct { int x, y, w, h; } shown_menu; /* the menu's bounds as last painted */
static volatile uint16_t pad_bits, scripted_bits, mouse_bits;
static uint16_t wheel_bits;
static int wheel_frames;
static volatile uint16_t wheel_now;
static int pointer_x, pointer_y, pointer_inside, cursor_hidden;
static unsigned last_pointer_motion, current_frame;
static int focus_clock_rate = 100;
static char base_title[160];

static void show(void);
static void repaint_menu(void);
static void relayout(void);
static void pump(void);

static void update_title(void)
{
    char title[256], suffix[32] = "";
    int clock_rate = Platform_ClockRate();
    if (!window) return;
    if (clock_rate == 0) snprintf(suffix, sizeof(suffix), " [paused]");
    else if (clock_rate == -1) snprintf(suffix, sizeof(suffix), " [uncapped]");
    else if (clock_rate != 100) snprintf(suffix, sizeof(suffix), " [%d%%]", clock_rate);
    snprintf(title, sizeof(title), "%s - state slot %d (F5 save, F7 load)%s",
             base_title, state_slot, suffix);
    SDL_SetWindowTitle(window, title);
}

static void show_cursor(void)
{
    if (cursor_hidden) {
        SDL_ShowCursor();
        cursor_hidden = 0;
    }
}

static uint64_t real_now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

static void update_display_refresh(void)
{
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    Platform_SetPresentRefresh(mode ? mode->refresh_rate : 0.0f);
}

static void update_menu_visibility(void)
{
    int wanted = !Settings_Get(SET_FULLSCREEN) || Settings_Get(SET_SHOW_MENU_FULLSCREEN) ||
                 (pointer_inside && pointer_y < Menu_Height()) || Menu_IsOpen() || menu_reveal_frames > 0;
    if (wanted == menu_visible) return;
    menu_visible = wanted;
    Menu_SetVisible(wanted);
    relayout();
    if (overlay) repaint_menu();
}

static int screenshot_path(char *path, size_t size, const char *extension)
{
    const char *directory = getenv("MEMORIES_SCREENSHOT_DIR");
    time_t now = time(NULL);
    struct tm local;
    char stamp[32];
    if (!directory || !*directory) {
        mkdir("saves", 0777);
        directory = "saves/screenshots";
    }
    mkdir(directory, 0777);
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);
    return snprintf(path, size, "%s/%s-%u.%s", directory, stamp, current_frame, extension) < (int)size;
}

void Platform_Screenshot(int window_image)
{
    SDL_Surface *surface;
    char path[1024];
    if (!renderer || !picture_pixels || !screenshot_path(path, sizeof(path), "bmp")) return;
    surface = window_image ? SDL_RenderReadPixels(renderer, NULL) :
              SDL_CreateSurfaceFrom(picture_w, picture_h, SDL_PIXELFORMAT_XRGB8888,
                                    picture_pixels, picture_w * 4);
    if (!surface) {
        fprintf(stderr, "memories-pc: screenshot failed: %s\n", SDL_GetError());
        return;
    }
    if (SDL_SaveBMP(surface, path)) fprintf(stderr, "memories-pc: screenshot: %s\n", path);
    else fprintf(stderr, "memories-pc: screenshot failed: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
}

/* Arrows d-pad; X cross, S circle, Z square, A triangle; Q/W L1/R1, E/R
 * L2/R2, T/Y L3/R3; Enter start, right Shift select. */
static const struct { SDL_Keycode key; uint16_t bit; } keymap[] = {
    {SDLK_RSHIFT, 0x0001}, {SDLK_T, 0x0002}, {SDLK_Y, 0x0004}, {SDLK_RETURN, 0x0008},
    {SDLK_UP, 0x0010}, {SDLK_RIGHT, 0x0020}, {SDLK_DOWN, 0x0040}, {SDLK_LEFT, 0x0080},
    {SDLK_E, 0x0100}, {SDLK_R, 0x0200}, {SDLK_Q, 0x0400}, {SDLK_W, 0x0800},
    {SDLK_A, 0x1000}, {SDLK_S, 0x2000}, {SDLK_X, 0x4000}, {SDLK_Z, 0x8000}};
/* Mouse: right circle (cancel), middle triangle; left is the menu bar's. */
static const uint16_t mouse_buttons[4] = {0, 0, 0x1000, 0x2000};

static void block_signals(sigset_t *previous)
{
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, previous);
}

static void restore_signals(const sigset_t *previous) { pthread_sigmask(SIG_SETMASK, previous, NULL); }

int Platform_Scale(void) { return scale; }
int Platform_HasWindowModes(void) { return 1; }

void Platform_ApplyDisplaySettings(void)
{
    display_settings_pending = 1;
}

void Platform_SetScale(int wanted)
{
    if (wanted < 1 || wanted > 8) {
        return;
    }
    if (window) {
        pending_scale = wanted; /* applied with the next frame */
    } else {
        scale = wanted;
    }
}

/* --- controllers ------------------------------------------------------ */

static SDL_Gamepad *pads[2];
static uint16_t pad_state[2];

static void open_gamepad(SDL_JoystickID id)
{
    int port;
    if (getenv("MEMORIES_NO_GAMEPAD")) {
        return;
    }
    for (port = 0; port < 2; port++) {
        if (!pads[port]) {
            pads[port] = SDL_OpenGamepad(id);
            if (pads[port]) {
                fprintf(stderr, "memories-pc: controller on port %d: %s\n", port + 1, SDL_GetGamepadName(pads[port]));
            }
            return;
        }
    }
}

static void close_gamepad(SDL_JoystickID id)
{
    int port;
    for (port = 0; port < 2; port++) {
        if (pads[port] && SDL_GetGamepadID(pads[port]) == id) {
            SDL_CloseGamepad(pads[port]);
            pads[port] = NULL;
            pad_state[port] = 0;
        }
    }
}

void Gamepad_Poll(unsigned frame)
{
    static const struct { SDL_GamepadButton button; uint16_t bit; } buttons[] = {
        {SDL_GAMEPAD_BUTTON_BACK, 0x0001}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0002},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0004}, {SDL_GAMEPAD_BUTTON_START, 0x0008},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0010}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0020},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0040}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0080},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0400}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0800},
        {SDL_GAMEPAD_BUTTON_NORTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x4000}, {SDL_GAMEPAD_BUTTON_WEST, 0x8000}};
    const int third = 32767 / 3;
    int port;
    (void)frame;
    for (port = 0; port < 2; port++) {
        uint16_t bits = 0;
        size_t i;
        int x, y;
        if (!pads[port]) {
            continue;
        }
        for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (SDL_GetGamepadButton(pads[port], buttons[i].button)) {
                bits |= buttons[i].bit;
            }
        }
        x = SDL_GetGamepadAxis(pads[port], SDL_GAMEPAD_AXIS_LEFTX);
        y = SDL_GetGamepadAxis(pads[port], SDL_GAMEPAD_AXIS_LEFTY);
        bits |= y < -third ? 0x0010 : y > third ? 0x0040 : 0;
        bits |= x > third ? 0x0020 : x < -third ? 0x0080 : 0;
        if (SDL_GetGamepadAxis(pads[port], SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > third) bits |= 0x0100;
        if (SDL_GetGamepadAxis(pads[port], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > third) bits |= 0x0200;
        pad_state[port] = bits;
    }
}

uint16_t Gamepad_Bits(int port) { return port >= 0 && port < 2 ? pad_state[port] : 0; }
int Gamepad_Connected(int port) { return port >= 0 && port < 2 && pads[port] != NULL; }

/* --- audio ------------------------------------------------------------ */

static void (*mixer)(int16_t *, size_t);
static SDL_AudioStream *stream;
static volatile unsigned audio_underruns;

static void SDLCALL feed(void *userdata, SDL_AudioStream *to, int additional, int total)
{
    static int16_t buffer[256 * 2];
    (void)userdata;
    (void)total;
    if (SDL_GetAudioStreamQueued(to) == 0) audio_underruns++;
    while (additional > 0) {
        mixer(buffer, 256);
        SDL_PutAudioStreamData(to, buffer, (int)sizeof(buffer));
        additional -= (int)sizeof(buffer);
    }
}

void Platform_AudioStats(int *queued_frames, unsigned *underruns)
{
    if (queued_frames) *queued_frames = stream ? SDL_GetAudioStreamQueued(stream) / (int)(sizeof(int16_t) * 2) : 0;
    if (underruns) *underruns = audio_underruns;
}

int Platform_StartAudio(void (*mix)(int16_t *, size_t))
{
    const char *dump = getenv("MEMORIES_DUMP_AUDIO");
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
    sigset_t previous;
    if (dump || getenv("MEMORIES_NO_AUDIO") || getenv("MEMORIES_HEADLESS") || !window) {
        return Platform_StartSilentAudio(mix, dump);
    }
    mixer = mix;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
    block_signals(&previous);
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (stream) {
        SDL_ResumeAudioStreamDevice(stream);
    }
    restore_signals(&previous);
    if (!stream) {
        fprintf(stderr, "memories-pc: no audio device (%s); continuing silently\n", SDL_GetError());
        return Platform_StartSilentAudio(mix, NULL);
    }
    return 0;
}

/* --- window ----------------------------------------------------------- */

static void relayout(void)
{
    static int logged_window_w, logged_window_h, logged_output_w, logged_output_h;
    int output_w, output_h, window_w, window_h, menu = menu_visible ? Menu_Height() : 0;
    int pw = Settings_Get(SET_ASPECT) ? picture_w : picture_h * 4 / 3;
    int ph = picture_h, area_h, mode = Settings_Get(SET_SCALING);
    float factor;
    if (!renderer || picture_w <= 0 || picture_h <= 0 ||
        !SDL_GetWindowSize(window, &window_w, &window_h) ||
        !SDL_GetRenderOutputSize(renderer, &output_w, &output_h) || window_w <= 0 || window_h <= 0) {
        return;
    }
    area_h = window_h - menu;
    if (area_h < 1) area_h = 1;
    layout.win_w = window_w;
    layout.win_h = window_h;
    layout.pixel_x = (float)output_w / (float)window_w;
    layout.pixel_y = (float)output_h / (float)window_h;
    if (window_w != logged_window_w || window_h != logged_window_h ||
        output_w != logged_output_w || output_h != logged_output_h) {
        LOG(LOG_WINDOW, "layout %dx%d logical -> %dx%d pixels (%.2fx, %.2fy)",
            window_w, window_h, output_w, output_h, layout.pixel_x, layout.pixel_y);
        logged_window_w = window_w; logged_window_h = window_h;
        logged_output_w = output_w; logged_output_h = output_h;
    }
    if (mode == 2) {
        layout.dst.x = 0;
        layout.dst.y = (float)menu;
        layout.dst.w = (float)window_w;
        layout.dst.h = (float)area_h;
    } else if (mode == 1) {
        factor = SDL_min((float)window_w / (float)pw, (float)area_h / (float)ph);
        layout.dst.w = (float)pw * factor;
        layout.dst.h = (float)ph * factor;
        layout.dst.x = ((float)window_w - layout.dst.w) * 0.5f;
        layout.dst.y = (float)menu + ((float)area_h - layout.dst.h) * 0.5f;
    } else {
        int k = SDL_min(window_w / pw, area_h / ph);
        if (k < 1) k = 1;
        layout.dst.w = (float)(pw * k);
        layout.dst.h = (float)(ph * k);
        layout.dst.x = (float)(window_w - pw * k) * 0.5f;
        layout.dst.y = (float)menu + (float)(area_h - ph * k) * 0.5f;
    }
    if (overlay && (canvas.width != window_w || canvas.height != window_h)) {
        SDL_DestroyTexture(overlay);
        overlay = NULL;
    }
    if (!overlay) {
        overlay = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                    window_w, window_h);
        SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(overlay, SDL_SCALEMODE_LINEAR);
        overlay_pixels = realloc(overlay_pixels, (size_t)window_w * (size_t)window_h * 4);
        memset(overlay_pixels, 0, (size_t)window_w * (size_t)window_h * 4);
        canvas.pixels = overlay_pixels;
        canvas.stride = window_w;
        canvas.width = window_w;
        canvas.height = window_h;
        canvas.alpha = 1;
        Menu_Draw(&canvas);
        SDL_UpdateTexture(overlay, NULL, overlay_pixels, window_w * 4);
    }
}

static void display_picture_size(int *w, int *h)
{
    *h = picture_h > 0 ? picture_h : 240;
    *w = Settings_Get(SET_ASPECT) ? (picture_w > 0 ? picture_w : 320) : *h * 4 / 3;
}

static void apply_display_settings(void)
{
    int fullscreen = Settings_Get(SET_FULLSCREEN), pw, ph;
    display_picture_size(&pw, &ph);
    if (fullscreen == 2) {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *current = SDL_GetCurrentDisplayMode(display);
        SDL_DisplayMode closest;
        float refresh = current ? current->refresh_rate : 0.0f;
        if (SDL_GetClosestFullscreenDisplayMode(display, pw * scale, ph * scale, refresh, true, &closest)) {
            SDL_SetWindowFullscreenMode(window, &closest);
            SDL_SetWindowFullscreen(window, true);
        } else {
            fprintf(stderr, "memories-pc: no exclusive fullscreen mode; using desktop fullscreen\n");
            Settings_Set(SET_FULLSCREEN, 1);
            Settings_Save();
            SDL_SetWindowFullscreenMode(window, NULL);
            SDL_SetWindowFullscreen(window, true);
        }
    } else if (fullscreen == 1) {
        SDL_SetWindowFullscreenMode(window, NULL);
        SDL_SetWindowFullscreen(window, true);
    } else {
        int x = Settings_Get(SET_WINDOW_X), y = Settings_Get(SET_WINDOW_Y);
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowBordered(window, !Settings_Get(SET_BORDERLESS));
        SDL_SetWindowSize(window, pw * scale, ph * scale + Menu_Height());
        SDL_SetWindowPosition(window, x == -1 ? SDL_WINDOWPOS_CENTERED : x,
                              y == -1 ? SDL_WINDOWPOS_CENTERED : y);
    }
    if (picture) {
        SDL_SetTextureScaleMode(picture, Settings_Get(SET_FILTER) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    }
    SDL_SetRenderVSync(renderer, Settings_Get(SET_VSYNC) ? 1 : 0);
    relayout();
    show();
}

static void resize(int w, int h)
{
    if (picture && (picture_w != w || picture_h != h)) {
        SDL_DestroyTexture(picture);
        picture = NULL;
    }
    if (!picture) {
        picture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(picture, Settings_Get(SET_FILTER) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        picture_pixels = realloc(picture_pixels, (size_t)w * (size_t)h * 4);
        picture_w = w;
        picture_h = h;
    }
    relayout();
}

static void upload_overlay(int x, int y, int w, int h)
{
    SDL_Rect rect;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > layout.win_w) { w = layout.win_w - x; }
    if (y + h > layout.win_h) { h = layout.win_h - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    SDL_UpdateTexture(overlay, &rect, overlay_pixels + (size_t)y * (size_t)layout.win_w + (size_t)x,
                      layout.win_w * 4);
}

static void draw_overlay(int *x, int *y, int *w, int *h)
{
    int hx, hy, hw, hh;
    Menu_Draw(&canvas);
    Hud_Draw(&canvas);
    Menu_Bounds(x, y, w, h);
    Hud_Bounds(&hx, &hy, &hw, &hh);
    if (!*w || !*h) { *x = hx; *y = hy; *w = hw; *h = hh; return; }
    if (hw && hh) {
        int x1 = *x + *w > hx + hw ? *x + *w : hx + hw;
        int y1 = *y + *h > hy + hh ? *y + *h : hy + hh;
        *x = *x < hx ? *x : hx;
        *y = *y < hy ? *y : hy;
        *w = x1 - *x;
        *h = y1 - *y;
    }
}

static void show(void)
{
    SDL_FRect physical;
    if (!renderer || !picture || !overlay) return;
    physical.x = layout.dst.x * layout.pixel_x;
    physical.y = layout.dst.y * layout.pixel_y;
    physical.w = layout.dst.w * layout.pixel_x;
    physical.h = layout.dst.h * layout.pixel_y;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, picture, NULL, &physical);
    SDL_RenderTexture(renderer, overlay, NULL, NULL);
    SDL_RenderPresent(renderer);
    if (Settings_Get(SET_VSYNC)) Platform_NotifyPresent(real_now_us(), 1);
}

/* The menu changed under a still picture: repaint it where it was and
 * where it is, upload that much of the overlay, and show the window. */
static void repaint_menu(void)
{
    int x, y, w, h, x0, y0, x1, y1, row;
    for (row = shown_menu.y; row < shown_menu.y + shown_menu.h && row < layout.win_h; row++) {
        memset(overlay_pixels + (size_t)row * (size_t)layout.win_w, 0, (size_t)layout.win_w * 4);
    }
    draw_overlay(&x, &y, &w, &h);
    x0 = x < shown_menu.x ? x : shown_menu.x;
    y0 = y < shown_menu.y ? y : shown_menu.y;
    x1 = x + w > shown_menu.x + shown_menu.w ? x + w : shown_menu.x + shown_menu.w;
    y1 = y + h > shown_menu.y + shown_menu.h ? y + h : shown_menu.y + shown_menu.h;
    shown_menu.x = x;
    shown_menu.y = y;
    shown_menu.w = w;
    shown_menu.h = h;
    upload_overlay(x0, y0, x1 - x0, y1 - y0);
    show();
}

static MenuKey menu_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_ESCAPE: return MENU_KEY_ESCAPE;
    case SDLK_F10: return MENU_KEY_F10;
    case SDLK_LEFT: return MENU_KEY_LEFT;
    case SDLK_RIGHT: return MENU_KEY_RIGHT;
    case SDLK_UP: return MENU_KEY_UP;
    case SDLK_DOWN: return MENU_KEY_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return MENU_KEY_ENTER;
    default: return MENU_KEY_OTHER;
    }
}

static void translate(const SDL_Event *event, MenuEvent *out)
{
    memset(out, 0, sizeof(*out));
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        out->type = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? MENU_EVENT_BUTTON_DOWN : MENU_EVENT_BUTTON_UP;
        out->button = event->button.button == SDL_BUTTON_LEFT ? 1 : event->button.button == SDL_BUTTON_MIDDLE ? 2
                    : event->button.button == SDL_BUTTON_RIGHT ? 3 : 0;
        out->x = (int)event->button.x;
        out->y = (int)event->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        out->type = MENU_EVENT_MOTION;
        out->x = (int)event->motion.x;
        out->y = (int)event->motion.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        out->type = MENU_EVENT_WHEEL;
        out->wheel = event->wheel.y > 0 ? 1 : event->wheel.y < 0 ? -1 : 0;
        out->x = (int)event->wheel.mouse_x;
        out->y = (int)event->wheel.mouse_y;
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: out->type = MENU_EVENT_LEAVE; break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        out->type = event->type == SDL_EVENT_KEY_DOWN ? MENU_EVENT_KEY_DOWN : MENU_EVENT_KEY_UP;
        out->key = menu_key(event->key.key);
        break;
    default: break;
    }
}

static void pump(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        MenuEvent menu_event;
        translate(&event, &menu_event);
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            pointer_x = menu_event.x;
            pointer_y = menu_event.y;
            pointer_inside = 1;
            last_pointer_motion = current_frame;
            show_cursor();
            if (Settings_Get(SET_FULLSCREEN) && pointer_y < Menu_Height()) menu_reveal_frames = 120;
        } else if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER) {
            pointer_inside = 1;
        } else if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
            pointer_inside = 0;
            show_cursor();
        }
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10) menu_reveal_frames = 120;
        update_menu_visibility();
        if (menu_event.type != MENU_EVENT_NONE && Menu_Event(&menu_event, &quit)) {
            repaint_menu(); /* the menu answers now, not at the next frame */
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT: quit = 1; break;
        case SDL_EVENT_WINDOW_EXPOSED: show(); break;
        case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            relayout();
            repaint_menu();
            break;
        case SDL_EVENT_WINDOW_MOVED:
            if (!Settings_Get(SET_FULLSCREEN)) {
                Settings_Set(SET_WINDOW_X, event.window.data1);
                Settings_Set(SET_WINDOW_Y, event.window.data2);
                Settings_Save();
            }
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            update_display_refresh();
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (Settings_Get(SET_MUTE_ON_FOCUS_LOSS)) Spu_SetOutputVolume(0);
            if (Settings_Get(SET_PAUSE_ON_FOCUS_LOSS) && Platform_ClockRate() != 0) {
                focus_clock_rate = Platform_ClockRate();
                Platform_SetClockRate(0);
            }
            show_cursor();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (Settings_Get(SET_MUTE_ON_FOCUS_LOSS)) {
                Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME));
            }
            if (Settings_Get(SET_PAUSE_ON_FOCUS_LOSS) && Platform_ClockRate() == 0) {
                Platform_SetClockRate(focus_clock_rate);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED: open_gamepad(event.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: close_gamepad(event.gdevice.which); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.y >= Menu_Height() && menu_event.button >= 2 && menu_event.button <= 3) {
                mouse_bits = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                 ? (uint16_t)(mouse_bits | mouse_buttons[menu_event.button])
                                 : (uint16_t)(mouse_bits & ~mouse_buttons[menu_event.button]);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (menu_event.wheel) {
                wheel_bits = menu_event.wheel > 0 ? 0x0010 : 0x0040; /* one notch: a short tap */
                wheel_frames = 3;
            }
            break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            SDL_Keycode key = event.key.key;
            int down = event.type == SDL_EVENT_KEY_DOWN;
            size_t i;
            if (event.key.repeat) {
                break;
            }
            if (down && (key == SDLK_F11 || (key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))) {
                Settings_Set(SET_FULLSCREEN, Settings_Get(SET_FULLSCREEN) ? 0 : 1);
                Settings_Save();
                Platform_ApplyDisplaySettings();
                break;
            }
            if (down && key == SDLK_F12) {
                Platform_Screenshot((event.key.mod & SDL_KMOD_SHIFT) != 0);
                break;
            }
            if (down && key == SDLK_F3) {
                Settings_Set(SET_SHOW_HUD, (Settings_Get(SET_SHOW_HUD) + 1) % 3);
                Settings_Save();
                repaint_menu();
                break;
            }
            if (key == SDLK_TAB) {
                Platform_SetClockRate(down ? 400 : Settings_Get(SET_SPEED));
                break;
            }
            if (down && key == SDLK_P) {
                Platform_SetClockRate(Platform_ClockRate() == 0 ? Settings_Get(SET_SPEED) : 0);
                break;
            }
            if (down && key == SDLK_PERIOD && Platform_ClockRate() == 0) {
                Platform_StepFrame();
                break;
            }
            if (down && key == SDLK_M) {
                Spu_SetMuted(!Spu_Muted());
                break;
            }
            if (down && key == SDLK_ESCAPE) {
                if (Settings_Get(SET_FULLSCREEN)) {
                    Settings_Set(SET_FULLSCREEN, 0);
                    Settings_Save();
                    Platform_ApplyDisplaySettings();
                } else {
                    quit = 1;
                }
            }
            /* F3 belongs to the HUD; slot 3 remains available from File. */
            if (down && key >= SDLK_F1 && key <= SDLK_F4) {
                if (key != SDLK_F3) Platform_SetStateSlot((int)(key - SDLK_F1) + 1);
            } else if (down && (key == SDLK_F5 || key == SDLK_F7)) {
                Memories_StateRequest(key == SDLK_F5 ? 1 : 2, state_slot);
            }
            for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
                if (keymap[i].key == key) {
                    pad_bits = down ? (uint16_t)(pad_bits | keymap[i].bit) : (uint16_t)(pad_bits & ~keymap[i].bit);
                }
            }
            break;
        }
        default: break;
        }
    }
}

int Platform_Open(const char *title)
{
    sigset_t previous;
    Menu_LoadSettings(); /* the volume and scale apply with or without a window */
    snprintf(base_title, sizeof(base_title), "%s", title);
    if (getenv("MEMORIES_HEADLESS")) {
        return 0;
    }
    /* Let SDL select the native backend. In particular, do not force an
     * XWayland window on a Wayland desktop: that gives the window a separate
     * cursor/DPI scale from the rest of the desktop. SDL_VIDEODRIVER remains
     * available for diagnostics and compatibility overrides. */
    block_signals(&previous);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        restore_signals(&previous);
        fprintf(stderr, "memories-pc: SDL: %s; set MEMORIES_HEADLESS=1 to run without a window\n", SDL_GetError());
        return -1;
    }
    window = SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window) {
        int x = Settings_Get(SET_WINDOW_X), y = Settings_Get(SET_WINDOW_Y);
        SDL_SetWindowPosition(window, x == -1 ? SDL_WINDOWPOS_CENTERED : x,
                              y == -1 ? SDL_WINDOWPOS_CENTERED : y);
    }
    renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    restore_signals(&previous);
    if (!renderer) {
        fprintf(stderr, "memories-pc: SDL: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderVSync(renderer, 0); /* the game paces itself on its own VBlank */
    update_display_refresh();
    LOG(LOG_WINDOW, "SDL renderer %s, video %s", SDL_GetRendererName(renderer), SDL_GetCurrentVideoDriver());
    Menu_Init();
    apply_display_settings();
    menu_visible = !Settings_Get(SET_FULLSCREEN) || Settings_Get(SET_SHOW_MENU_FULLSCREEN);
    Menu_SetVisible(menu_visible);
    update_title();
    return 0;
}

void Platform_Present(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24)
{
    int i, j;
    if (!renderer || w <= 0 || h <= 0) {
        return;
    }
    if (pending_scale) {
        scale = pending_scale;
        pending_scale = 0;
        display_settings_pending = 1;
    }
    if (display_settings_pending) {
        display_settings_pending = 0;
        apply_display_settings();
    }
    resize(w, h);
    for (j = 0; j < h; j++) {
        const uint16_t *row = vram + ((y + j) & 511) * stride;
        uint32_t *out = picture_pixels + (size_t)j * (size_t)w;
        for (i = 0; i < w; i++) {
            if (rgb24) {
                const uint8_t *bytes = (const uint8_t *)(row + x) + i * 3;
                out[i] = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
            } else {
                uint16_t c = row[(x + i) & 1023];
                uint32_t r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
                out[i] = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
        }
    }
    SDL_UpdateTexture(picture, NULL, picture_pixels, w * 4);
    draw_overlay(&shown_menu.x, &shown_menu.y, &shown_menu.w, &shown_menu.h);
    upload_overlay(shown_menu.x, shown_menu.y, shown_menu.w, shown_menu.h);
    show();
    pump();
}

int Platform_ShouldQuit(void) { return quit; }
int Platform_StateSlot(void) { return state_slot; }
void Platform_SetStateSlot(int slot)
{
    if (slot < 1 || slot > 4) return;
    state_slot = slot;
    update_title();
}
void Platform_PumpEvents(void) { if (window) pump(); }

uint16_t Platform_Pad(int port)
{
    return port == 0 ? (uint16_t)(pad_bits | mouse_bits | wheel_now | scripted_bits | Gamepad_Bits(0))
                     : Gamepad_Bits(1);
}

int Platform_PadConnected(int port) { return port == 0 || Gamepad_Connected(port); }

/* MEMORIES_SDL_SCRIPT="200:click:20:13,260:move:60:69,420:key:escape": events
 * pushed into SDL's queue at presented frames, for testing the menu without
 * a pointer (external synthetic X input does not reach SDL correctly). */
static void run_event_script(unsigned frame)
{
    static const char *script;
    static int loaded;
    if (!loaded) {
        loaded = 1;
        script = getenv("MEMORIES_SDL_SCRIPT");
    }
    while (script && *script) {
        char *end, kind[8];
        unsigned long at = strtoul(script, &end, 10);
        SDL_Event event;
        int x = 0, y = 0;
        size_t n;
        if (*end != ':' || at > frame) {
            return;
        }
        script = end + 1;
        n = strcspn(script, ":,");
        snprintf(kind, sizeof(kind), "%.*s", (int)(n < 7 ? n : 7), script);
        script += n;
        memset(&event, 0, sizeof(event));
        if (strcmp(kind, "key") == 0) {
            const char *name = script + 1;
            n = strcspn(name, ",");
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.down = true;
            event.key.key = strncmp(name, "escape", n) == 0 ? SDLK_ESCAPE : strncmp(name, "f10", n) == 0 ? SDLK_F10
                          : strncmp(name, "f3", n) == 0 ? SDLK_F3
                          : strncmp(name, "f11", n) == 0 ? SDLK_F11
                          : strncmp(name, "f12", n) == 0 ? SDLK_F12
                          : strncmp(name, "left", n) == 0 ? SDLK_LEFT : strncmp(name, "right", n) == 0 ? SDLK_RIGHT
                          : strncmp(name, "up", n) == 0 ? SDLK_UP : strncmp(name, "down", n) == 0 ? SDLK_DOWN
                          : strncmp(name, "return", n) == 0 ? SDLK_RETURN
                          : strncmp(name, "tab", n) == 0 ? SDLK_TAB : strncmp(name, "p", n) == 0 ? SDLK_P
                          : strncmp(name, "m", n) == 0 ? SDLK_M
                          : strncmp(name, "period", n) == 0 ? SDLK_PERIOD
                          : SDLK_UNKNOWN;
            event.key.windowID = SDL_GetWindowID(window);
            SDL_PushEvent(&event);
            event.type = SDL_EVENT_KEY_UP;
            event.key.down = false;
            SDL_PushEvent(&event);
            script = name + n;
        } else {
            x = (int)strtol(script + 1, &end, 10);
            y = (int)strtol(end + 1, &end, 10);
            script = end;
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = (float)x;
            event.motion.y = (float)y;
            event.motion.windowID = SDL_GetWindowID(window);
            SDL_PushEvent(&event);
            if (strcmp(kind, "click") == 0) {
                memset(&event, 0, sizeof(event));
                event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.down = true;
                event.button.clicks = 1;
                event.button.x = (float)x;
                event.button.y = (float)y;
                event.button.windowID = SDL_GetWindowID(window);
                SDL_PushEvent(&event);
                event.type = SDL_EVENT_MOUSE_BUTTON_UP;
                event.button.down = false;
                SDL_PushEvent(&event);
            }
        }
        if (*script == ',') {
            script++;
        }
    }
}

void Platform_Frame(unsigned frame)
{
    static int shown_rate = -2;
    static int crash_tested;
    static int hang_tested;
    current_frame = frame;
    Log_Drain();
    if (!crash_tested && frame >= 60 && getenv("MEMORIES_CRASH_TEST")) {
        crash_tested = 1;
        *(volatile int *)(uintptr_t)0 = 1;
    }
    if (!hang_tested && frame >= 60 && getenv("MEMORIES_HANG_TEST")) {
        volatile unsigned spin = 0;
        hang_tested = 1;
        for (;;) spin++;
    }
    Gamepad_Poll(frame);
    Cheats_Frame();
    if (window) {
        run_event_script(frame);
    }
    if (menu_reveal_frames > 0) menu_reveal_frames--;
    update_menu_visibility();
    if (shown_rate != Platform_ClockRate()) {
        shown_rate = Platform_ClockRate();
        update_title();
    }
    if (window && Settings_Get(SET_HIDE_CURSOR) && pointer_inside && !cursor_hidden &&
        pointer_x >= layout.dst.x && pointer_x < layout.dst.x + layout.dst.w &&
        pointer_y >= layout.dst.y && pointer_y < layout.dst.y + layout.dst.h &&
        frame - last_pointer_motion >= 120) {
        SDL_HideCursor();
        cursor_hidden = 1;
    } else if ((!Settings_Get(SET_HIDE_CURSOR) || pointer_y < Menu_Height()) && cursor_hidden) {
        show_cursor();
    }
    wheel_now = wheel_frames > 0 && wheel_frames-- ? wheel_bits : 0;
    scripted_bits = Platform_ScriptedBits(frame);
}
