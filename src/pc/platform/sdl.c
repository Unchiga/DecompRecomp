/* SDL3 owns the window, input, controllers and audio. Presentation uses an
 * explicit OpenGL path by default, avoiding SDL_Render's per-frame texture
 * management and making the accelerated path predictable across drivers.
 * SDL_Render remains a fallback for machines where a GL context cannot be
 * created. The picture and transparent menu are two GPU textures.
 *
 * The interrupt clock (platform_common.c) is a signal on the main thread,
 * so signals are blocked while SDL creates its threads, which inherit the
 * mask. See platform.h for the contract. */
#define _GNU_SOURCE
#include "platform.h"
#include "paths.h"
#include "menu.h"
#include "mods_window.h"
#include "controls_window.h"
#include "controls_linux.h"
#include "settings.h"
#include "pc/audio/spu.h"
#include "pc/debug/cheats.h"
#include "pc/debug/log.h"
#include "pc/debug/hud.h"
#include "pc/guest/state.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "pc/compat/signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "pc/compat/posix.h"

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *picture, *overlay;
static SDL_GLContext gl_context;
static GLuint gl_picture, gl_overlay;
static int use_gl;
static int picture_w, picture_h;
static uint32_t *picture_pixels, *overlay_pixels;
static int picture_scale = 1; /* picture pixels per game pixel (internal resolution) */
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
static volatile uint16_t scripted_bits, mouse_bits;
static uint16_t wheel_bits;
static int wheel_frames;
static volatile uint16_t wheel_now;
static int pointer_x, pointer_y, pointer_inside, cursor_hidden;
static unsigned last_pointer_motion, current_frame;
static int focus_clock_rate = 100, focus_paused;
static char base_title[160];
static float known_refresh; /* what the wayland driver reported before a fallback to x11 */

static void show(void);
static void repaint_menu(void);
/* Menu changes from events are coalesced: a pointer sweeping the bar
 * reports hundreds of motions a second, and each used to repaint and
 * present a frame. Now they mark the menu dirty and it is repainted once,
 * with the next game frame or at the end of an idle pump. */
static int menu_dirty;
static void save_window_image(void);
static int window_shot_pending;

/* The menu's size: the setting, or from the height the window has or is
 * about to have. */
static void update_menu_scale(int window_h)
{
    int wanted = Settings_Get(SET_MENU_SCALE);
    Menu_SetScale(wanted ? wanted : Menu_AutoScale(window_h));
}
static void relayout(void);
static void show_cursor(void);
static void block_signals(sigset_t *previous);
static void restore_signals(const sigset_t *previous);

/* SDL_Render may leave its own OpenGL context current. Game texture uploads
 * must never inherit that context (or its pixel unpack stride). */
static void restore_game_context(void)
{
    if (use_gl && !SDL_GL_MakeCurrent(window, gl_context)) {
        fprintf(stderr, "memories-pc: restoring game OpenGL context: %s\n", SDL_GetError());
        quit = 1;
    }
}
static SDL_Window *mods_window;
static SDL_Renderer *mods_renderer;
static SDL_Texture *mods_texture;
static MenuCanvas mods_canvas;
static void close_mods(void)
{
    if (mods_texture) SDL_DestroyTexture(mods_texture);
    if (mods_renderer) SDL_DestroyRenderer(mods_renderer);
    if (mods_window) SDL_DestroyWindow(mods_window);
    free(mods_canvas.pixels);
    mods_texture = NULL; mods_renderer = NULL; mods_window = NULL;
    mods_canvas.pixels = NULL;
    restore_game_context();
}
static void draw_mods(void)
{
    ModsWindow_Draw(&mods_canvas);
    SDL_UpdateTexture(mods_texture, NULL, mods_canvas.pixels, mods_canvas.stride * 4);
    SDL_RenderClear(mods_renderer);
    SDL_RenderTexture(mods_renderer, mods_texture, NULL, NULL);
    SDL_RenderPresent(mods_renderer);
    restore_game_context();
}
void Platform_OpenMods(void)
{
    sigset_t previous;
    if (mods_window) { SDL_RaiseWindow(mods_window); return; }
    ModsWindow_Init();
    ModsWindow_Size(&mods_canvas.width, &mods_canvas.height);
    mods_canvas.stride = mods_canvas.width;
    mods_canvas.pixels = calloc((size_t)mods_canvas.width * mods_canvas.height, 4);
    /* Driver worker threads must inherit the blocked game timer signals. */
    block_signals(&previous);
    mods_window = SDL_CreateWindow("MODS", mods_canvas.width, mods_canvas.height, 0);
    mods_renderer = mods_window ? SDL_CreateRenderer(mods_window, NULL) : NULL;
    mods_texture = mods_renderer ? SDL_CreateTexture(mods_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, mods_canvas.width, mods_canvas.height) : NULL;
    restore_signals(&previous);
    /* The canvas is opaque; SDL would otherwise blend an ARGB texture by alpha. */
    if (mods_texture) SDL_SetTextureBlendMode(mods_texture, SDL_BLENDMODE_NONE);
    if (!mods_canvas.pixels || !mods_texture) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MODS", "Could not open the mods window.", window);
        close_mods(); return;
    }
    show_cursor();
    draw_mods();
}

static void controls_key_labels(void);
static void controls_sync_keys(void);
static SDL_Window *controls_window;
static SDL_Renderer *controls_renderer;
static SDL_Texture *controls_texture;
static MenuCanvas controls_canvas;
static void close_controls(void)
{
    if (controls_texture)
        SDL_DestroyTexture(controls_texture);
    if (controls_renderer)
        SDL_DestroyRenderer(controls_renderer);
    if (controls_window)
        SDL_DestroyWindow(controls_window);
    free(controls_canvas.pixels);
    memset(&controls_canvas, 0, sizeof(controls_canvas));
    controls_window = NULL;
    controls_renderer = NULL;
    controls_texture = NULL;
    mouse_bits = wheel_now = 0;
    wheel_frames = 0;
    ControlsRuntime_Block(0);
    restore_game_context();
}
static void draw_controls(void)
{
    ControlsWindow_Draw(&controls_canvas);
    SDL_UpdateTexture(controls_texture,NULL,controls_canvas.pixels,controls_canvas.stride*4);
    SDL_RenderClear(controls_renderer);SDL_RenderTexture(controls_renderer,controls_texture,NULL,NULL);
    SDL_RenderPresent(controls_renderer);restore_game_context();
}
/* The window is resizable: rebuild the canvas and texture for the new size
 * and repaint, keeping the old ones when the new pair cannot be made. */
static void resize_controls(int w, int h)
{
    sigset_t previous;
    if(!controls_window||w<1||h<1||(w==controls_canvas.width&&h==controls_canvas.height))return;
    uint32_t *pixels=calloc((size_t)w*h,4);
    block_signals(&previous);
    SDL_Texture *texture=pixels?SDL_CreateTexture(controls_renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,w,h):NULL;
    restore_signals(&previous);
    if(!texture){free(pixels);return;}
    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_NONE);
    block_signals(&previous);
    SDL_DestroyTexture(controls_texture);
    restore_signals(&previous);
    free(controls_canvas.pixels);
    controls_texture=texture;
    controls_canvas.pixels=pixels;
    controls_canvas.width=controls_canvas.stride=w;
    controls_canvas.height=h;
    draw_controls();
}
void Platform_OpenControls(void)
{
    sigset_t previous;
    if(controls_window){SDL_RaiseWindow(controls_window);return;}
    controls_key_labels();
    ControlsWindow_Init();controls_sync_keys();ControlsWindow_Size(&controls_canvas.width,&controls_canvas.height);
    SDL_Rect usable;
    if(SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window),&usable)) {
        if(controls_canvas.width>usable.w)controls_canvas.width=usable.w;
        if(controls_canvas.height>usable.h-50)controls_canvas.height=usable.h-50;
    }
    controls_canvas.stride=controls_canvas.width;
    controls_canvas.pixels=calloc((size_t)controls_canvas.width*controls_canvas.height,4);
    block_signals(&previous);
    controls_window=SDL_CreateWindow("Controls",controls_canvas.width,controls_canvas.height,SDL_WINDOW_RESIZABLE);
    controls_renderer=controls_window?SDL_CreateRenderer(controls_window,NULL):NULL;
    controls_texture=controls_renderer?SDL_CreateTexture(controls_renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,controls_canvas.width,controls_canvas.height):NULL;
    restore_signals(&previous);
    if(!controls_texture||!controls_canvas.pixels) {close_controls();return;}
    SDL_SetTextureBlendMode(controls_texture,SDL_BLENDMODE_NONE);
    int min_w,min_h;
    ControlsWindow_MinSize(&min_w,&min_h);
    SDL_SetWindowMinimumSize(controls_window,min_w,min_h);
    mouse_bits=wheel_now=0;wheel_frames=0;show_cursor();draw_controls();
}

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

/* XWayland reports no refresh rate for the current or desktop mode, but its
 * fullscreen mode list has them: take the fastest at the desktop's size.
 * Unknown (0) makes the "display refresh" cap show every game frame and
 * lets vsync pace the game only at 100% or slower. */
static void update_display_refresh(void)
{
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    float refresh = mode ? mode->refresh_rate : 0.0f;
    if (refresh <= 0.0f && desktop) refresh = desktop->refresh_rate;
    if (refresh <= 0.0f) refresh = known_refresh;
    if (refresh <= 0.0f && desktop) {
        int count = 0, i;
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display, &count);
        for (i = 0; modes && i < count; i++) {
            if (modes[i]->w == desktop->w && modes[i]->h == desktop->h && modes[i]->refresh_rate > refresh) {
                refresh = modes[i]->refresh_rate;
            }
        }
        SDL_free(modes);
    }
    Platform_SetPresentRefresh(refresh);
    LOG(LOG_WINDOW, "display refresh %.2f Hz", refresh);
}

/* A blocking vsync present may only pace the game while game frames come no
 * faster than the display refreshes; faster than that it would hold the
 * game to the refresh rate, so presents go out unsynced and the frame-rate
 * cap alone limits them. Re-checked at every present: the speed can change
 * at any time (Tab turbo). */
static int swap_interval = -1;

static void apply_swap_interval(void)
{
    int wanted = Settings_Get(SET_VSYNC) && Platform_VSyncPacesGame() ? 1 : 0;
    if (wanted == swap_interval) return;
    swap_interval = wanted;
    if (use_gl) SDL_GL_SetSwapInterval(wanted);
    else if (renderer) SDL_SetRenderVSync(renderer, wanted);
    LOG(LOG_WINDOW, "vsync %s (game %.2f Hz, display %.2f Hz)", wanted ? "on" : "off",
        Platform_GameHz(), Platform_PresentRefresh());
}

static void update_menu_visibility(void)
{
    int wanted = !Settings_Get(SET_FULLSCREEN) || Settings_Get(SET_SHOW_MENU_FULLSCREEN) ||
                 (pointer_inside && pointer_y < Menu_Height()) || Menu_IsOpen() || menu_reveal_frames > 0;
    if (wanted == menu_visible) return;
    menu_visible = wanted;
    Menu_SetVisible(wanted);
    relayout();
    menu_dirty = 1;
}

static int screenshot_path(char *path, size_t size, const char *extension)
{
    const char *directory = getenv("MEMORIES_SCREENSHOT_DIR");
    time_t now = time(NULL);
    struct tm local;
    char stamp[32];
    char user[1024];
    if (!directory || !*directory) {
        if (Paths_User(user, sizeof(user), "screenshots")) return 0;
        directory = user;
    }
    mkdir(directory, 0777);
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);
    return snprintf(path, size, "%s/%s-%u.%s", directory, stamp, current_frame, extension) < (int)size;
}

static void save_surface(SDL_Surface *surface, const char *path)
{
    if (!surface) {
        fprintf(stderr, "memories-pc: screenshot failed: %s\n", SDL_GetError());
        return;
    }
    if (SDL_SaveBMP(surface, path)) fprintf(stderr, "memories-pc: screenshot: %s\n", path);
    else fprintf(stderr, "memories-pc: screenshot failed: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
}

/* GL: read the composed frame from the back buffer, before it is swapped. */
static void save_window_image(void)
{
    SDL_Surface *surface;
    char path[1024];
    int w, h, y;
    uint32_t *pixels, *row;
    if (!screenshot_path(path, sizeof(path), "bmp") || !SDL_GetWindowSizeInPixels(window, &w, &h)) return;
    surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        pixels = surface->pixels;
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        row = malloc((size_t)surface->pitch);
        if (row) {
            for (y = 0; y < h / 2; y++) {
                uint8_t *a = (uint8_t *)surface->pixels + (size_t)y * surface->pitch;
                uint8_t *b = (uint8_t *)surface->pixels + (size_t)(h - 1 - y) * surface->pitch;
                memcpy(row, a, (size_t)surface->pitch);
                memcpy(a, b, (size_t)surface->pitch);
                memcpy(b, row, (size_t)surface->pitch);
            }
            free(row);
        }
    }
    save_surface(surface, path);
}

void Platform_Screenshot(int window_image)
{
    SDL_Surface *surface;
    char path[1024];
    if ((!renderer && !use_gl) || !picture_pixels) return;
    if (window_image && use_gl) {
        window_shot_pending = 1; /* taken by the next show(), before its swap */
        return;
    }
    if (!screenshot_path(path, sizeof(path), "bmp")) return;
    {
        surface = window_image ? SDL_RenderReadPixels(renderer, NULL) :
                  SDL_CreateSurfaceFrom(picture_w, picture_h, SDL_PIXELFORMAT_XRGB8888,
                                        picture_pixels, picture_w * 4);
    }
    save_surface(surface, path);
}

/* Arrows d-pad; X cross, S circle, Z square, A triangle; Q/W L1/R1, E/R
 * L2/R2, T/Y L3/R3; Enter start, right Shift select. */
static int controls_key(SDL_Scancode code)
{
    switch (code) {
    case SDL_SCANCODE_CAPSLOCK:
        return CTRL_KEY_CAPS_LOCK;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return CTRL_KEY_NUM_LOCK;
    case SDL_SCANCODE_PRINTSCREEN:
        return CTRL_KEY_PRINT_SCREEN;
    case SDL_SCANCODE_SCROLLLOCK:
        return CTRL_KEY_SCROLL_LOCK;
    case SDL_SCANCODE_PAUSE:
        return CTRL_KEY_PAUSE;
    case SDL_SCANCODE_SPACE:
        return CTRL_KEY_SPACE;
    case SDL_SCANCODE_ESCAPE:
        return CTRL_KEY_ESCAPE;
    case SDL_SCANCODE_TAB:
        return CTRL_KEY_TAB;
    case SDL_SCANCODE_BACKSPACE:
        return CTRL_KEY_BACKSPACE;
    case SDL_SCANCODE_RETURN:
        return CTRL_KEY_ENTER;
    case SDL_SCANCODE_MINUS:
        return CTRL_KEY_MINUS;
    case SDL_SCANCODE_EQUALS:
        return CTRL_KEY_EQUAL;
    case SDL_SCANCODE_LEFTBRACKET:
        return CTRL_KEY_LBRACKET;
    case SDL_SCANCODE_RIGHTBRACKET:
        return CTRL_KEY_RBRACKET;
    case SDL_SCANCODE_BACKSLASH:
        return CTRL_KEY_BACKSLASH;
    case SDL_SCANCODE_SEMICOLON:
        return CTRL_KEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE:
        return CTRL_KEY_APOSTROPHE;
    case SDL_SCANCODE_GRAVE:
        return CTRL_KEY_GRAVE;
    case SDL_SCANCODE_COMMA:
        return CTRL_KEY_COMMA;
    case SDL_SCANCODE_PERIOD:
        return CTRL_KEY_PERIOD;
    case SDL_SCANCODE_SLASH:
        return CTRL_KEY_SLASH;
    case SDL_SCANCODE_UP:
        return CTRL_KEY_ARROW_UP;
    case SDL_SCANCODE_DOWN:
        return CTRL_KEY_ARROW_DOWN;
    case SDL_SCANCODE_LEFT:
        return CTRL_KEY_ARROW_LEFT;
    case SDL_SCANCODE_RIGHT:
        return CTRL_KEY_ARROW_RIGHT;
    case SDL_SCANCODE_INSERT:
        return CTRL_KEY_INSERT;
    case SDL_SCANCODE_HOME:
        return CTRL_KEY_HOME;
    case SDL_SCANCODE_END:
        return CTRL_KEY_END;
    case SDL_SCANCODE_PAGEUP:
        return CTRL_KEY_PAGE_UP;
    case SDL_SCANCODE_PAGEDOWN:
        return CTRL_KEY_PAGE_DOWN;
    case SDL_SCANCODE_DELETE:
        return CTRL_KEY_DELETE;
    case SDL_SCANCODE_LSHIFT:
        return CTRL_KEY_LEFT_SHIFT;
    case SDL_SCANCODE_RSHIFT:
        return CTRL_KEY_RIGHT_SHIFT;
    case SDL_SCANCODE_LCTRL:
        return CTRL_KEY_LEFT_CTRL;
    case SDL_SCANCODE_RCTRL:
        return CTRL_KEY_RIGHT_CTRL;
    case SDL_SCANCODE_LALT:
        return CTRL_KEY_LEFT_ALT;
    case SDL_SCANCODE_RALT:
        return CTRL_KEY_RIGHT_ALT;
    case SDL_SCANCODE_LGUI:
        return CTRL_KEY_LEFT_SUPER;
    case SDL_SCANCODE_RGUI:
        return CTRL_KEY_RIGHT_SUPER;
    case SDL_SCANCODE_A:
        return CTRL_KEY_A;
    case SDL_SCANCODE_B:
        return CTRL_KEY_B;
    case SDL_SCANCODE_C:
        return CTRL_KEY_C;
    case SDL_SCANCODE_D:
        return CTRL_KEY_D;
    case SDL_SCANCODE_E:
        return CTRL_KEY_E;
    case SDL_SCANCODE_F:
        return CTRL_KEY_F;
    case SDL_SCANCODE_G:
        return CTRL_KEY_G;
    case SDL_SCANCODE_H:
        return CTRL_KEY_H;
    case SDL_SCANCODE_I:
        return CTRL_KEY_I;
    case SDL_SCANCODE_J:
        return CTRL_KEY_J;
    case SDL_SCANCODE_K:
        return CTRL_KEY_K;
    case SDL_SCANCODE_L:
        return CTRL_KEY_L;
    case SDL_SCANCODE_M:
        return CTRL_KEY_M;
    case SDL_SCANCODE_N:
        return CTRL_KEY_N;
    case SDL_SCANCODE_O:
        return CTRL_KEY_O;
    case SDL_SCANCODE_P:
        return CTRL_KEY_P;
    case SDL_SCANCODE_Q:
        return CTRL_KEY_Q;
    case SDL_SCANCODE_R:
        return CTRL_KEY_R;
    case SDL_SCANCODE_S:
        return CTRL_KEY_S;
    case SDL_SCANCODE_T:
        return CTRL_KEY_T;
    case SDL_SCANCODE_U:
        return CTRL_KEY_U;
    case SDL_SCANCODE_V:
        return CTRL_KEY_V;
    case SDL_SCANCODE_W:
        return CTRL_KEY_W;
    case SDL_SCANCODE_X:
        return CTRL_KEY_X;
    case SDL_SCANCODE_Y:
        return CTRL_KEY_Y;
    case SDL_SCANCODE_Z:
        return CTRL_KEY_Z;
    case SDL_SCANCODE_0:
        return CTRL_KEY_0;
    case SDL_SCANCODE_1:
        return CTRL_KEY_1;
    case SDL_SCANCODE_2:
        return CTRL_KEY_2;
    case SDL_SCANCODE_3:
        return CTRL_KEY_3;
    case SDL_SCANCODE_4:
        return CTRL_KEY_4;
    case SDL_SCANCODE_5:
        return CTRL_KEY_5;
    case SDL_SCANCODE_6:
        return CTRL_KEY_6;
    case SDL_SCANCODE_7:
        return CTRL_KEY_7;
    case SDL_SCANCODE_8:
        return CTRL_KEY_8;
    case SDL_SCANCODE_9:
        return CTRL_KEY_9;
    case SDL_SCANCODE_F1:
        return CTRL_KEY_F1;
    case SDL_SCANCODE_F2:
        return CTRL_KEY_F2;
    case SDL_SCANCODE_F3:
        return CTRL_KEY_F3;
    case SDL_SCANCODE_F4:
        return CTRL_KEY_F4;
    case SDL_SCANCODE_F5:
        return CTRL_KEY_F5;
    case SDL_SCANCODE_F6:
        return CTRL_KEY_F6;
    case SDL_SCANCODE_F7:
        return CTRL_KEY_F7;
    case SDL_SCANCODE_F8:
        return CTRL_KEY_F8;
    case SDL_SCANCODE_F9:
        return CTRL_KEY_F9;
    case SDL_SCANCODE_F10:
        return CTRL_KEY_F10;
    case SDL_SCANCODE_F11:
        return CTRL_KEY_F11;
    case SDL_SCANCODE_F12:
        return CTRL_KEY_F12;
    case SDL_SCANCODE_KP_0:
        return CTRL_KEY_KP_0;
    case SDL_SCANCODE_KP_1:
        return CTRL_KEY_KP_1;
    case SDL_SCANCODE_KP_2:
        return CTRL_KEY_KP_2;
    case SDL_SCANCODE_KP_3:
        return CTRL_KEY_KP_3;
    case SDL_SCANCODE_KP_4:
        return CTRL_KEY_KP_4;
    case SDL_SCANCODE_KP_5:
        return CTRL_KEY_KP_5;
    case SDL_SCANCODE_KP_6:
        return CTRL_KEY_KP_6;
    case SDL_SCANCODE_KP_7:
        return CTRL_KEY_KP_7;
    case SDL_SCANCODE_KP_8:
        return CTRL_KEY_KP_8;
    case SDL_SCANCODE_KP_9:
        return CTRL_KEY_KP_9;
    case SDL_SCANCODE_KP_COMMA:
        return CTRL_KEY_KP_COMMA;
    case SDL_SCANCODE_KP_PERIOD:
        return CTRL_KEY_KP_DOT;
    case SDL_SCANCODE_KP_DIVIDE:
        return CTRL_KEY_KP_SLASH;
    case SDL_SCANCODE_KP_MULTIPLY:
        return CTRL_KEY_KP_ASTERISK;
    case SDL_SCANCODE_KP_MINUS:
        return CTRL_KEY_KP_MINUS;
    case SDL_SCANCODE_KP_PLUS:
        return CTRL_KEY_KP_PLUS;
    case SDL_SCANCODE_KP_ENTER:
        return CTRL_KEY_KP_ENTER;
    case SDL_SCANCODE_KP_EQUALS:
        return CTRL_KEY_KP_EQUAL;
    default:
        return 0;
    }
}
static void controls_sync_keys(void)
{
    int count=0;const bool *held=SDL_GetKeyboardState(&count);
    for(int sc=1;sc<count;sc++) {int key=controls_key((SDL_Scancode)sc);if(key)ControlsRuntime_Key(key,held[sc]);}
}
static void controls_key_labels(void)
{
    for(int sc=1;sc<SDL_SCANCODE_COUNT;sc++) {
        int key=controls_key((SDL_Scancode)sc);
        if(key)Controls_SetKeyLabel(key,SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)sc,SDL_KMOD_NONE,false)));
    }
}
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

static SDL_Gamepad *pads[CONTROLS_DEVICES];
static void open_gamepad(SDL_JoystickID id)
{
    if (getenv("MEMORIES_NO_GAMEPAD"))
        return;
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (pads[i] && SDL_GetGamepadID(pads[i]) == id)
            return;
    for (int i = 0; i < CONTROLS_DEVICES; i++)
        if (!pads[i]) {
            pads[i] = SDL_OpenGamepad(id);
            if (!pads[i])
                return;
            ControllerDevice *d = ControlsRuntime_Device(i);
            memset(d, 0, sizeof(*d));
            d->connected = 1;
            d->threshold = 1.0f / 3;
            snprintf(d->name, sizeof(d->name), "%s", SDL_GetGamepadName(pads[i]));
            const char *serial = SDL_GetGamepadSerial(pads[i]);
            if (!ControlsLinux_Identity(SDL_GetGamepadPath(pads[i]), d->identity, sizeof(d->identity))) {
                snprintf(d->identity, sizeof(d->identity), "pad:%04x:%04x:%s", SDL_GetGamepadVendor(pads[i]),
                         SDL_GetGamepadProduct(pads[i]), serial ? serial : "");
                d->ambiguous = !serial || !*serial;
                if (d->ambiguous) {
                    static uint64_t session;
                    if (!session)
                        session = ControlsRuntime_Now();
                    snprintf(d->identity, sizeof(d->identity), "session:sdl:%llu:%u",
                             (unsigned long long)session, (unsigned)id);
                }
            }
            SDL_GamepadType type = SDL_GetGamepadType(pads[i]);
            d->style = type == SDL_GAMEPAD_TYPE_XBOX360 || type == SDL_GAMEPAD_TYPE_XBOXONE ? CTRL_ICON_XBOX
                       : type >= SDL_GAMEPAD_TYPE_PS3 && type <= SDL_GAMEPAD_TYPE_PS5 ? CTRL_ICON_PLAYSTATION
                       : type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO &&
                               type <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
                           ? CTRL_ICON_NINTENDO
                           : CTRL_ICON_GENERIC;
            return;
        }
}
static void scan_gamepads(void)
{
    int count=0;SDL_JoystickID *ids;
    if(getenv("MEMORIES_NO_GAMEPAD"))return;
    ids=SDL_GetGamepads(&count);
    for(int i=0;ids && i<count;i++)open_gamepad(ids[i]);
    SDL_free(ids);
}
static void close_gamepad(SDL_JoystickID id)
{
    for(int i=0;i<CONTROLS_DEVICES;i++) if(pads[i]&&SDL_GetGamepadID(pads[i])==id) {
        SDL_CloseGamepad(pads[i]);pads[i]=NULL;memset(ControlsRuntime_Device(i),0,sizeof(ControllerDevice));
        ControlsRuntime_Gate();
    }
}
void Gamepad_Poll(unsigned frame)
{
    static uint64_t last_scan;
    static const SDL_GamepadButton buttons[CTRL_BTN_COUNT]={
        SDL_GAMEPAD_BUTTON_INVALID,SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_GUIDE,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,SDL_GAMEPAD_BUTTON_LEFT_STICK,SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_MISC1,SDL_GAMEPAD_BUTTON_MISC2};
    uint64_t now=ControlsRuntime_Now();(void)frame;
    if(now-last_scan>=1000000){scan_gamepads();last_scan=now;}
    for(int i=0;i<CONTROLS_DEVICES;i++) if(pads[i]) {
        if(!SDL_GamepadConnected(pads[i])){close_gamepad(SDL_GetGamepadID(pads[i]));continue;}
        ControllerDevice *d=ControlsRuntime_Device(i);ControllerSnapshot *snap=&d->snapshot;memset(snap,0,sizeof(*snap));
        for(int b=1;b<CTRL_BTN_COUNT;b++) if(SDL_GetGamepadButton(pads[i],buttons[b])) {
            if(b>=CTRL_BTN_DPAD_UP && b<=CTRL_BTN_DPAD_RIGHT) {
                static const unsigned char hats[]={1,4,8,2};snap->hat_down|=hats[b-CTRL_BTN_DPAD_UP];
            } else snap->buttons_down|=1u<<(b-1);
        }
        for(int a=0;a<4;a++) {int value=SDL_GetGamepadAxis(pads[i],(SDL_GamepadAxis)a);snap->axis[a+1]=value/(value<0?32768.0f:32767.0f);}
        for(int a=0;a<2;a++) {int value=SDL_GetGamepadAxis(pads[i],(SDL_GamepadAxis)(SDL_GAMEPAD_AXIS_LEFT_TRIGGER+a));snap->trigger[a+1]=value>0?value/32767.0f:0;}
    }
    ControlsRuntime_Update();
}
uint16_t Gamepad_Bits(int p){return ControlsRuntime_Pad(p);}
int Gamepad_Connected(int p){return ControlsRuntime_Connected(p);}

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
    int output_w, output_h, window_w, window_h, menu;
    int aspect = Settings_Get(SET_ASPECT);
    /* Widescreen pictures arrive 4/3 as wide as the 4:3 ones (libgpu.c), so
     * both keep the same pixel shape; the internal resolution scales both. */
    int pw = (aspect == 1 ? picture_w : aspect == 2 ? picture_h * 16 / 9 : picture_h * 4 / 3) / picture_scale;
    int ph = picture_h / picture_scale, area_h, mode = Settings_Get(SET_SCALING);
    float factor;
    if ((!renderer && !use_gl) || picture_w <= 0 || picture_h <= 0 ||
        !SDL_GetWindowSize(window, &window_w, &window_h) ||
        !(use_gl ? SDL_GetWindowSizeInPixels(window, &output_w, &output_h) :
                    SDL_GetRenderOutputSize(renderer, &output_w, &output_h)) ||
        window_w <= 0 || window_h <= 0) {
        return;
    }
    update_menu_scale(window_h);
    menu = menu_visible ? Menu_Height() : 0;
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
    if (use_gl && gl_overlay && (canvas.width != window_w || canvas.height != window_h)) {
        glDeleteTextures(1, &gl_overlay);
        gl_overlay = 0;
    }
    if ((use_gl && !gl_overlay) || (!use_gl && !overlay)) {
        if (use_gl) {
            glGenTextures(1, &gl_overlay);
            glBindTexture(GL_TEXTURE_2D, gl_overlay);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, window_w, window_h, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        } else {
            overlay = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        window_w, window_h);
            SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(overlay, SDL_SCALEMODE_LINEAR);
        }
        overlay_pixels = realloc(overlay_pixels, (size_t)window_w * (size_t)window_h * 4);
        memset(overlay_pixels, 0, (size_t)window_w * (size_t)window_h * 4);
        canvas.pixels = overlay_pixels;
        canvas.stride = window_w;
        canvas.width = window_w;
        canvas.height = window_h;
        canvas.alpha = 1;
        Menu_Draw(&canvas);
        if (use_gl) {
            glBindTexture(GL_TEXTURE_2D, gl_overlay);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, window_w, window_h,
                            GL_BGRA, GL_UNSIGNED_BYTE, overlay_pixels);
        } else {
            SDL_UpdateTexture(overlay, NULL, overlay_pixels, window_w * 4);
        }
    }
}

static void display_picture_size(int *w, int *h)
{
    int aspect = Settings_Get(SET_ASPECT);
    *h = picture_h > 0 ? picture_h / picture_scale : 240;
    *w = aspect == 2 ? *h * 16 / 9
                     : aspect == 1 ? (picture_w > 0 ? picture_w / picture_scale : 320)
                                   : *h * 4 / 3;
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
        update_menu_scale(ph * scale + 26 * Menu_AutoScale(ph * scale));
        SDL_SetWindowSize(window, pw * scale, ph * scale + Menu_Height());
        SDL_SetWindowPosition(window, x == -1 ? SDL_WINDOWPOS_CENTERED : x,
                              y == -1 ? SDL_WINDOWPOS_CENTERED : y);
    }
    if (use_gl && gl_picture) {
        glBindTexture(GL_TEXTURE_2D, gl_picture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (picture) {
        SDL_SetTextureScaleMode(picture, Settings_Get(SET_FILTER) ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    }
    apply_swap_interval();
    relayout();
    show();
}

static void resize(int w, int h)
{
    if (use_gl && gl_picture && (picture_w != w || picture_h != h)) {
        glDeleteTextures(1, &gl_picture);
        gl_picture = 0;
    }
    if (picture && (picture_w != w || picture_h != h)) {
        SDL_DestroyTexture(picture);
        picture = NULL;
    }
    if (use_gl && !gl_picture) {
        glGenTextures(1, &gl_picture);
        glBindTexture(GL_TEXTURE_2D, gl_picture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        Settings_Get(SET_FILTER) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        picture_pixels = realloc(picture_pixels, (size_t)w * (size_t)h * 4);
        picture_w = w;
        picture_h = h;
    } else if (!use_gl && !picture) {
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
    if (use_gl) {
        /* GL has no source-row pitch for this upload; rows are contiguous in
         * the full-width canvas, so upload whole rows for the dirty span. */
        glBindTexture(GL_TEXTURE_2D, gl_overlay);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, layout.win_w);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE,
                        overlay_pixels + (size_t)y * (size_t)layout.win_w + (size_t)x);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        SDL_UpdateTexture(overlay, &rect, overlay_pixels + (size_t)y * (size_t)layout.win_w + (size_t)x,
                          layout.win_w * 4);
    }
}

static void gl_quad(GLuint texture, float x, float y, float w, float h)
{
    glBindTexture(GL_TEXTURE_2D, texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x, y);
    glTexCoord2f(1, 0); glVertex2f(x + w, y);
    glTexCoord2f(1, 1); glVertex2f(x + w, y + h);
    glTexCoord2f(0, 1); glVertex2f(x, y + h);
    glEnd();
}

static void draw_overlay(int *x, int *y, int *w, int *h)
{
    int hx, hy, hw, hh;
    if (Settings_Get(SET_SHOW_HUD) == 2) {
        Hud_Draw(&canvas);
        Menu_Draw(&canvas); /* dropdowns stay above the full statistics panel */
    } else {
        Menu_Draw(&canvas);
        Hud_Draw(&canvas);
    }
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
    if (use_gl) {
        int output_w, output_h;
        if (!gl_picture || !gl_overlay || !SDL_GetWindowSizeInPixels(window, &output_w, &output_h)) return;
        glViewport(0, 0, output_w, output_h);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, layout.win_w, layout.win_h, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
        glColor4f(1, 1, 1, 1);
        gl_quad(gl_picture, layout.dst.x, layout.dst.y, layout.dst.w, layout.dst.h);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl_quad(gl_overlay, 0, 0, (float)layout.win_w, (float)layout.win_h);
        if (window_shot_pending) {
            window_shot_pending = 0;
            save_window_image(); /* the back buffer holds this frame until the swap */
        }
        apply_swap_interval();
        SDL_GL_SwapWindow(window);
        if (swap_interval == 1) Platform_NotifyPresent(real_now_us(), 1);
        return;
    }
    if (!renderer || !picture || !overlay) return;
    physical.x = layout.dst.x * layout.pixel_x;
    physical.y = layout.dst.y * layout.pixel_y;
    physical.w = layout.dst.w * layout.pixel_x;
    physical.h = layout.dst.h * layout.pixel_y;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, picture, NULL, &physical);
    SDL_RenderTexture(renderer, overlay, NULL, NULL);
    apply_swap_interval();
    SDL_RenderPresent(renderer);
    if (swap_interval == 1) Platform_NotifyPresent(real_now_us(), 1);
}

/* Repaint the menu where it was and where it is, and upload that much of
 * the overlay. */
static unsigned compose_count; /* MEMORIES_TRACE=window: compositions per 120 frames */

static void compose_menu(void)
{
    int x, y, w, h, x0, y0, x1, y1, row;
    menu_dirty = 0;
    compose_count++;
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
}

/* The menu changed under a still picture: repaint it and show the window. */
static void repaint_menu(void)
{
    compose_menu();
    show();
}

/* The overlay only changes with the menu, or when the HUD's text does;
 * otherwise the texture already holds it. */
static void compose_menu_if_changed(void)
{
    static unsigned hud_signature;
    unsigned now = Hud_Signature();
    if (menu_dirty || now != hud_signature) {
        hud_signature = now;
        compose_menu();
    }
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

static int dispatch_controls(const SDL_Event *event, const MenuEvent *menu_event)
{
    if (controls_window && SDL_GetWindowFromEvent(event) == controls_window) {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
            if (event->key.repeat)
                return 1;
            int key = controls_key(event->key.scancode), down = event->type == SDL_EVENT_KEY_DOWN;
            ControlsRuntime_Key(key, down);
            int mods = (event->key.mod & SDL_KMOD_SHIFT ? 1 : 0) |
                       (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI) ? 2 : 0);
            /* Right Shift alone is the default Select binding. */
            if ((event->key.mod & ~SDL_KMOD_RSHIFT &
                 (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0)
                mods = 0;
            ControlsWindow_Key(key, down, event->key.repeat, mods);
            ControlsWindow_Tick();
        } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            ControlsWindow_RequestClose();
        else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
            ControlsWindow_FocusLost();
        else if (event->type == SDL_EVENT_WINDOW_FOCUS_GAINED)
            controls_sync_keys();
        else if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event->type == SDL_EVENT_WINDOW_RESIZED)
            resize_controls(event->window.data1, event->window.data2);
        else
            ControlsWindow_Event(menu_event);
        if (ControlsWindow_ShouldClose())
            close_controls();
        return 1;
    }
    if (controls_window && (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_MOUSE_WHEEL))
        return 1;

    return 0;
}

static void pump(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        MenuEvent menu_event;
        translate(&event, &menu_event);
        if(event.type==SDL_EVENT_KEYMAP_CHANGED){controls_key_labels();continue;}
        if(event.type==SDL_EVENT_GAMEPAD_ADDED){open_gamepad(event.gdevice.which);continue;}
        if(event.type==SDL_EVENT_GAMEPAD_REMOVED){close_gamepad(event.gdevice.which);continue;}
        if(event.type==SDL_EVENT_KEY_UP)ControlsRuntime_Key(controls_key(event.key.scancode),0);
        if(dispatch_controls(&event, &menu_event))continue;
        if (mods_window && SDL_GetWindowFromEvent(&event) == mods_window) {
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || ModsWindow_Event(&menu_event)) close_mods();
            else draw_mods();
            continue;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            static unsigned logged_frame = ~0u;
            pointer_x = menu_event.x;
            pointer_y = menu_event.y;
            pointer_inside = 1;
            if (current_frame - logged_frame >= 3) { /* a sample, not every motion */
                logged_frame = current_frame;
                LOG(LOG_MENU, "pointer at %d,%d", pointer_x, pointer_y);
            }
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
            static unsigned logged;
            if (current_frame - logged >= 30) {
                logged = current_frame;
                LOG(LOG_WINDOW, "menu dirty from SDL event %#x (menu event %d)", (unsigned)event.type, (int)menu_event.type);
            }
            menu_dirty = 1;
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT: quit = 1; break;
        case SDL_EVENT_WINDOW_EXPOSED: show(); break;
        case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            relayout();
            menu_dirty = 1;
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
            if(!controls_window)ControlsRuntime_ResetKeys(); mouse_bits=wheel_now=0;wheel_frames=0;
            if (Settings_Get(SET_MUTE_ON_FOCUS_LOSS)) Spu_SetOutputVolume(0);
            if (Settings_Get(SET_PAUSE_ON_FOCUS_LOSS) && Platform_ClockRate() != 0) {
                focus_clock_rate = Platform_ClockRate();
                focus_paused = 1;
                Platform_SetClockRate(0);
            }
            show_cursor();
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (Settings_Get(SET_MUTE_ON_FOCUS_LOSS)) {
                Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME));
            }
            if (focus_paused && Platform_ClockRate() == 0) {
                Platform_SetClockRate(focus_clock_rate);
                focus_paused = 0;
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
                menu_dirty = 1;
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
            ControlsRuntime_Key(controls_key(event.key.scancode),down);
            break;
        }
        default: break;
        }
    }
    Gamepad_Poll(current_frame);
    if(controls_window) {
        static uint64_t last_draw;
        ControlsWindow_Tick();
        if(ControlsWindow_ShouldClose())close_controls();
        else if(ControlsRuntime_Now()-last_draw>=16000){draw_controls();last_draw=ControlsRuntime_Now();}
    }

}

static void create_window(const char *title)
{
    update_menu_scale(240 * scale + 26 * Menu_AutoScale(240 * scale));
    window = SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(),
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_OPENGL);
    gl_context = window ? SDL_GL_CreateContext(window) : NULL;
    use_gl = gl_context != NULL;
    if (!use_gl) {
        if (window) SDL_DestroyWindow(window);
        window = SDL_CreateWindow(title, 320 * scale, 240 * scale + Menu_Height(),
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    }
}

static void destroy_window(void)
{
    if (gl_context) SDL_GL_DestroyContext(gl_context);
    if (window) SDL_DestroyWindow(window);
    gl_context = NULL;
    window = NULL;
    use_gl = 0;
}

static int software_gl_renderer(void)
{
    const char *name = (const char *)glGetString(GL_RENDERER);
    return name && (strstr(name, "llvmpipe") || strstr(name, "softpipe") || strstr(name, "SwiftShader") ||
                    strstr(name, "Software Rasterizer"));
}

void Platform_ShowError(const char *title, const char *message)
{
    const char *headless = getenv("MEMORIES_HEADLESS");
    fprintf(stderr, "memories-pc: %s\n", message);
    if (!headless || !*headless || !strcmp(headless, "0")) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, NULL);
    }
}

int Platform_Open(const char *title)
{
    sigset_t previous;
    ControlsRuntime_Init();
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
    create_window(title);
    /* A software GL renderer cannot present a scaled 4K frame in the 4 ms a
     * 400% game frame allows. The 32-bit build on an NVIDIA Wayland desktop
     * gets llvmpipe through Mesa's EGL while the X11 (XWayland) path reaches
     * the real driver, so retry there. SDL_VIDEODRIVER pins a choice. */
    if (use_gl && software_gl_renderer() && !getenv("SDL_VIDEODRIVER") &&
        SDL_GetCurrentVideoDriver() && !strcmp(SDL_GetCurrentVideoDriver(), "wayland")) {
        const char *software = (const char *)glGetString(GL_RENDERER);
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        fprintf(stderr, "memories-pc: OpenGL on wayland is %s (software); retrying with the x11 driver\n",
                software ? software : "unknown");
        known_refresh = mode ? mode->refresh_rate : 0.0f; /* XWayland will not know it */
        destroy_window();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, NULL);
            SDL_InitSubSystem(SDL_INIT_VIDEO);
        }
        create_window(title);
    }
    if (window) {
        int x = Settings_Get(SET_WINDOW_X), y = Settings_Get(SET_WINDOW_Y);
        SDL_SetWindowPosition(window, x == -1 ? SDL_WINDOWPOS_CENTERED : x,
                              y == -1 ? SDL_WINDOWPOS_CENTERED : y);
    }
    renderer = window && !use_gl ? SDL_CreateRenderer(window, NULL) : NULL;
    restore_signals(&previous);
    if (!use_gl && !renderer) {
        fprintf(stderr, "memories-pc: SDL: %s\n", SDL_GetError());
        return -1;
    }
    update_display_refresh();
    if (use_gl) {
        LOG(LOG_WINDOW, "OpenGL renderer %s, video %s", glGetString(GL_RENDERER), SDL_GetCurrentVideoDriver());
    } else {
        LOG(LOG_WINDOW, "SDL fallback renderer %s, video %s", SDL_GetRendererName(renderer), SDL_GetCurrentVideoDriver());
    }
    Menu_Init();
    apply_display_settings();
    menu_visible = !Settings_Get(SET_FULLSCREEN) || Settings_Get(SET_SHOW_MENU_FULLSCREEN);
    Menu_SetVisible(menu_visible);
    update_title();
    return 0;
}

/* Before a frame: input and the menu, then any window change. */
static void begin_present(int w, int h, int at_scale)
{
    pump(); /* before the frame, so its input and menu state are current */
    if (pending_scale) {
        scale = pending_scale;
        pending_scale = 0;
        display_settings_pending = 1;
    }
    if (picture_scale != at_scale) {
        picture_scale = at_scale;
        display_settings_pending = 1;
    }
    if (display_settings_pending) {
        display_settings_pending = 0;
        apply_display_settings();
    }
    resize(w, h);
}

/* After the picture's pixels are in place: to the texture and the window. */
static void finish_present(int w, int h)
{
    if (use_gl) {
        glBindTexture(GL_TEXTURE_2D, gl_picture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, picture_pixels);
    } else {
        SDL_UpdateTexture(picture, NULL, picture_pixels, w * 4);
    }
    compose_menu_if_changed();
    show();
}

int Platform_PresentPicture(const uint32_t *pixels, int stride, int x, int y, int w, int h, int at_scale)
{
    int j;
    if ((!renderer && !use_gl) || w <= 0 || h <= 0 || at_scale < 1) return 0;
    begin_present(w, h, at_scale);
    for (j = 0; j < h; j++) {
        memcpy(picture_pixels + (size_t)j * (size_t)w, pixels + (size_t)(y + j) * (size_t)stride + x, (size_t)w * 4);
    }
    finish_present(w, h);
    return 1;
}

void Platform_Present(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24)
{
    int i, j;
    if ((!renderer && !use_gl) || w <= 0 || h <= 0) {
        return;
    }
    begin_present(w, h, 1);
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
    finish_present(w, h);
}

int Platform_ShouldQuit(void) { return quit; }
int Platform_Widescreen(void) { return Settings_Get(SET_ASPECT) == 2; }
int Platform_StateSlot(void) { return state_slot; }
void Platform_SetStateSlot(int slot)
{
    if (slot < 1 || slot > 4) return;
    state_slot = slot;
    update_title();
}
/* No frame is coming (paused, or a frame that is not shown): answer the
 * menu now, at most once per call. */
void Platform_PumpEvents(void)
{
    static uint64_t last_repaint_us;
    uint64_t now;
    if (!window) return;
    pump();
    /* A running game shows the change with its next frame; paused, the wait
     * loop pumps every half millisecond, so keep hover repaints to ~120/s. */
    if (menu_dirty && overlay_pixels && Platform_ClockRate() == 0 &&
        (now = real_now_us()) - last_repaint_us >= 8000) {
        last_repaint_us = now;
        repaint_menu();
    }
}

uint16_t Platform_Pad(int port)
{
    return port == 0 ? (uint16_t)(ControlsRuntime_Keyboard() | (ControlsRuntime_Blocked()?0:(mouse_bits | wheel_now)) | scripted_bits | Gamepad_Bits(0))
                     : Gamepad_Bits(1);
}

int Platform_PadConnected(int port) { return port == 0 || Gamepad_Connected(port); }

/* MEMORIES_SDL_SCRIPT="200:click:20:13,260:move:60:69,420:key:escape": events
 * pushed into SDL's queue at presented frames, for testing the menu without
 * a pointer (external synthetic X input does not reach SDL correctly).
 * "300:shot" saves the composed window (menu included) like Shift+F12. */
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
        if (strcmp(kind, "shot") == 0) {
            Platform_Screenshot(1);
        } else if (strcmp(kind, "key") == 0) {
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
            event.key.scancode = SDL_GetScancodeFromKey(event.key.key, NULL);
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
    static unsigned composed_then;
    static int crash_tested;
    static int hang_tested;
    current_frame = frame;
    Log_Drain();
    if (frame % 120 == 0) {
        LOG(LOG_WINDOW, "overlay composed %u times in 120 frames", compose_count - composed_then);
        composed_then = compose_count;
    }
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
