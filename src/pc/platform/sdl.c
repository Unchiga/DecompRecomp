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
#include "pc/debug/cheats.h"
#include "pc/guest/state.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *picture, *overlay;
static int picture_w, picture_h;
static uint32_t *picture_pixels, *overlay_pixels;
static MenuCanvas canvas;
static struct { int win_w, win_h; SDL_FRect dst; } layout;
static int menu_visible = 1;
/* 4 puts the 320x240 picture on screen at 1280x960. */
static int scale = 4, pending_scale, quit, state_slot = 1;
static struct { int x, y, w, h; } shown_menu; /* the menu's bounds as last painted */
static volatile uint16_t pad_bits, scripted_bits, mouse_bits;
static uint16_t wheel_bits;
static int wheel_frames;
static volatile uint16_t wheel_now;

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
    pending_scale = Settings_Get(SET_SCALE);
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

static void SDLCALL feed(void *userdata, SDL_AudioStream *to, int additional, int total)
{
    static int16_t buffer[256 * 2];
    (void)userdata;
    (void)total;
    while (additional > 0) {
        mixer(buffer, 256);
        SDL_PutAudioStreamData(to, buffer, (int)sizeof(buffer));
        additional -= (int)sizeof(buffer);
    }
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
    int output_w, output_h, menu = menu_visible ? Menu_Height() : 0;
    int pw = Settings_Get(SET_ASPECT) ? picture_w : picture_h * 4 / 3;
    int ph = picture_h, area_h, mode = Settings_Get(SET_SCALING);
    float factor;
    if (!renderer || picture_w <= 0 || picture_h <= 0 ||
        !SDL_GetRenderOutputSize(renderer, &output_w, &output_h)) {
        return;
    }
    area_h = output_h - menu;
    if (area_h < 1) area_h = 1;
    layout.win_w = output_w;
    layout.win_h = output_h;
    if (mode == 2) {
        layout.dst.x = 0;
        layout.dst.y = (float)menu;
        layout.dst.w = (float)output_w;
        layout.dst.h = (float)area_h;
    } else if (mode == 1) {
        factor = SDL_min((float)output_w / (float)pw, (float)area_h / (float)ph);
        layout.dst.w = (float)pw * factor;
        layout.dst.h = (float)ph * factor;
        layout.dst.x = ((float)output_w - layout.dst.w) * 0.5f;
        layout.dst.y = (float)menu + ((float)area_h - layout.dst.h) * 0.5f;
    } else {
        int k = SDL_min(output_w / pw, area_h / ph);
        if (k < 1) k = 1;
        layout.dst.w = (float)(pw * k);
        layout.dst.h = (float)(ph * k);
        layout.dst.x = (float)(output_w - pw * k) * 0.5f;
        layout.dst.y = (float)menu + (float)(area_h - ph * k) * 0.5f;
    }
    if (overlay && (canvas.width != output_w || canvas.height != output_h)) {
        SDL_DestroyTexture(overlay);
        overlay = NULL;
    }
    if (!overlay) {
        overlay = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                    output_w, output_h);
        SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(overlay, SDL_SCALEMODE_NEAREST);
        overlay_pixels = realloc(overlay_pixels, (size_t)output_w * (size_t)output_h * 4);
        memset(overlay_pixels, 0, (size_t)output_w * (size_t)output_h * 4);
        canvas.pixels = overlay_pixels;
        canvas.stride = output_w;
        canvas.width = output_w;
        canvas.height = output_h;
        canvas.alpha = 1;
        Menu_Draw(&canvas);
        SDL_UpdateTexture(overlay, NULL, overlay_pixels, output_w * 4);
    }
}

static void resize(int w, int h)
{
    if (picture && (picture_w != w || picture_h != h)) {
        SDL_DestroyTexture(picture);
        picture = NULL;
    }
    if (!picture) {
        picture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureScaleMode(picture, SDL_SCALEMODE_NEAREST);
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

static void show(void)
{
    if (!renderer || !picture || !overlay) return;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, picture, NULL, &layout.dst);
    SDL_RenderTexture(renderer, overlay, NULL, NULL);
    SDL_RenderPresent(renderer);
}

/* The menu changed under a still picture: repaint it where it was and
 * where it is, upload that much of the overlay, and show the window. */
static void repaint_menu(void)
{
    int x, y, w, h, x0, y0, x1, y1, row;
    for (row = shown_menu.y; row < shown_menu.y + shown_menu.h && row < layout.win_h; row++) {
        memset(overlay_pixels + (size_t)row * (size_t)layout.win_w, 0, (size_t)layout.win_w * 4);
    }
    Menu_Draw(&canvas);
    Menu_Bounds(&x, &y, &w, &h);
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
        SDL_ConvertEventToRenderCoordinates(renderer, &event);
        translate(&event, &menu_event);
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
            if (down && key == SDLK_ESCAPE) {
                quit = 1;
            }
            /* Save states: F1-F4 pick a slot, F5 saves it, F7 loads it. */
            if (down && key >= SDLK_F1 && key <= SDLK_F4) {
                char title[96];
                state_slot = (int)(key - SDLK_F1) + 1;
                snprintf(title, sizeof(title), "Yu-Gi-Oh! Forbidden Memories - state slot %d (F5 save, F7 load)", state_slot);
                SDL_SetWindowTitle(window, title);
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
    if (getenv("MEMORIES_HEADLESS")) {
        return 0;
    }
    /* X11 (Xwayland under a Wayland session): presents there return at once,
     * while SDL's Wayland path waits for the compositor's frame callback,
     * and that refresh beats against the game's own 59.94 Hz clock into a
     * dropped frame every few seconds. SDL_VIDEO_DRIVER in the environment
     * still wins over this default. */
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11,wayland", SDL_HINT_DEFAULT);
    block_signals(&previous);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        restore_signals(&previous);
        fprintf(stderr, "memories-pc: SDL: %s; set MEMORIES_HEADLESS=1 to run without a window\n", SDL_GetError());
        return -1;
    }
    window = SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    restore_signals(&previous);
    if (!renderer) {
        fprintf(stderr, "memories-pc: SDL: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderVSync(renderer, 0); /* the game paces itself on its own VBlank */
    if (getenv("MEMORIES_TRACE_FRAMES")) {
        fprintf(stderr, "memories-pc: SDL renderer %s, video %s\n", SDL_GetRendererName(renderer),
                SDL_GetCurrentVideoDriver());
    }
    Menu_Init();
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
        SDL_SetWindowSize(window, (picture_h ? picture_h * 4 / 3 : 320) * scale,
                          (picture_h ? picture_h : 240) * scale + Menu_Height());
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
    Menu_Draw(&canvas);
    Menu_Bounds(&shown_menu.x, &shown_menu.y, &shown_menu.w, &shown_menu.h);
    upload_overlay(shown_menu.x, shown_menu.y, shown_menu.w, shown_menu.h);
    show();
    pump();
}

int Platform_ShouldQuit(void) { return quit; }

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
                          : strncmp(name, "left", n) == 0 ? SDLK_LEFT : strncmp(name, "right", n) == 0 ? SDLK_RIGHT
                          : strncmp(name, "up", n) == 0 ? SDLK_UP : strncmp(name, "down", n) == 0 ? SDLK_DOWN
                          : strncmp(name, "return", n) == 0 ? SDLK_RETURN
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
    Gamepad_Poll(frame);
    Cheats_Frame();
    if (window) {
        run_event_script(frame);
    }
    wheel_now = wheel_frames > 0 && wheel_frames-- ? wheel_bits : 0;
    scripted_bits = Platform_ScriptedBits(frame);
}
