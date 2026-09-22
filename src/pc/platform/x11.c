#define _GNU_SOURCE
#include "platform.h"
#include "paths.h"
#include "pc/audio/spu.h"
#include "pc/debug/cheats.h"
#include "pc/debug/log.h"
#include "pc/debug/hud.h"
#include "pc/guest/state.h"
#include "menu.h"
#include "mods_window.h"
#include "controls_window.h"
#include <X11/XKBlib.h>
#include "settings.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

/* The window shows one ARGB32 frame: the game picture scaled by an integer
 * below the menu bar, both composited here in software. The frame lives in
 * MIT-SHM memory the X server reads directly, so showing it is a request
 * rather than a copy of five megabytes down a socket at 60 Hz, and menu
 * activity repaints and re-shows only the rectangle it touches (the menu's
 * own bounds), never the whole frame. Without the extension the same buffer
 * goes through XPutImage. */
static Display *display;
static Window window;
static GC context;
static XImage *image;
static XShmSegmentInfo shm;
static int shm_ok;
static Atom close_atom;
/* 4 puts the 320x240 picture on screen at 1280x960. */
static int scale = 4, pending_scale, image_w, image_h, quit;
static MenuCanvas canvas;
static struct { const uint16_t *vram; int stride, x, y, w, h, rgb24; } last;
static struct { int x, y, w, h; } shown_menu; /* the menu's bounds as last painted */
static volatile uint16_t scripted_bits;
static int state_slot = 1;
static unsigned current_frame;
static char base_title[160];

static void update_title(void)
{
    char title[256], suffix[32] = "";
    int clock_rate = Platform_ClockRate();
    if (!display) return;
    if (clock_rate == 0) snprintf(suffix, sizeof(suffix), " [paused]");
    else if (clock_rate == -1) snprintf(suffix, sizeof(suffix), " [uncapped]");
    else if (clock_rate != 100) snprintf(suffix, sizeof(suffix), " [%d%%]", clock_rate);
    snprintf(title, sizeof(title), "%s - state slot %d (F5 save, F7 load)%s",
             base_title, state_slot, suffix);
    XStoreName(display, window, title);
}

/* Arrows d-pad; X cross, S circle, Z square, A triangle; Q/W L1/R1, E/R
 * L2/R2, T/Y L3/R3; Enter start, right Shift select. */
static int physical_keys[256];
static void controls_keys_init(void)
{
    static const struct {
        char name[5];
        int key;
    } names[] = {
        {"AD01", CTRL_KEY_Q},           {"AD02", CTRL_KEY_W},           {"AD03", CTRL_KEY_E},
        {"AD04", CTRL_KEY_R},           {"AD05", CTRL_KEY_T},           {"AD06", CTRL_KEY_Y},
        {"AD07", CTRL_KEY_U},           {"AD08", CTRL_KEY_I},           {"AD09", CTRL_KEY_O},
        {"AD10", CTRL_KEY_P},           {"AC01", CTRL_KEY_A},           {"AC02", CTRL_KEY_S},
        {"AC03", CTRL_KEY_D},           {"AC04", CTRL_KEY_F},           {"AC05", CTRL_KEY_G},
        {"AC06", CTRL_KEY_H},           {"AC07", CTRL_KEY_J},           {"AC08", CTRL_KEY_K},
        {"AC09", CTRL_KEY_L},           {"AB01", CTRL_KEY_Z},           {"AB02", CTRL_KEY_X},
        {"AB03", CTRL_KEY_C},           {"AB04", CTRL_KEY_V},           {"AB05", CTRL_KEY_B},
        {"AB06", CTRL_KEY_N},           {"AB07", CTRL_KEY_M},           {"AE01", CTRL_KEY_1},
        {"AE02", CTRL_KEY_2},           {"AE03", CTRL_KEY_3},           {"AE04", CTRL_KEY_4},
        {"AE05", CTRL_KEY_5},           {"AE06", CTRL_KEY_6},           {"AE07", CTRL_KEY_7},
        {"AE08", CTRL_KEY_8},           {"AE09", CTRL_KEY_9},           {"AE10", CTRL_KEY_0},
        {"CAPS", CTRL_KEY_CAPS_LOCK},   {"NMLK", CTRL_KEY_NUM_LOCK},    {"PRSC", CTRL_KEY_PRINT_SCREEN},
        {"SCLK", CTRL_KEY_SCROLL_LOCK}, {"PAUS", CTRL_KEY_PAUSE},       {"TLDE", CTRL_KEY_GRAVE},
        {"AE11", CTRL_KEY_MINUS},       {"AE12", CTRL_KEY_EQUAL},       {"AD11", CTRL_KEY_LBRACKET},
        {"AD12", CTRL_KEY_RBRACKET},    {"BKSL", CTRL_KEY_BACKSLASH},   {"AC10", CTRL_KEY_SEMICOLON},
        {"AC11", CTRL_KEY_APOSTROPHE},  {"AB08", CTRL_KEY_COMMA},       {"AB09", CTRL_KEY_PERIOD},
        {"AB10", CTRL_KEY_SLASH},       {"SPCE", CTRL_KEY_SPACE},       {"ESC", CTRL_KEY_ESCAPE},
        {"TAB", CTRL_KEY_TAB},          {"BKSP", CTRL_KEY_BACKSPACE},   {"RTRN", CTRL_KEY_ENTER},
        {"LFSH", CTRL_KEY_LEFT_SHIFT},  {"RTSH", CTRL_KEY_RIGHT_SHIFT}, {"LCTL", CTRL_KEY_LEFT_CTRL},
        {"RCTL", CTRL_KEY_RIGHT_CTRL},  {"LALT", CTRL_KEY_LEFT_ALT},    {"RALT", CTRL_KEY_RIGHT_ALT},
        {"LWIN", CTRL_KEY_LEFT_SUPER},  {"RWIN", CTRL_KEY_RIGHT_SUPER}, {"UP", CTRL_KEY_ARROW_UP},
        {"DOWN", CTRL_KEY_ARROW_DOWN},  {"LEFT", CTRL_KEY_ARROW_LEFT},  {"RGHT", CTRL_KEY_ARROW_RIGHT},
        {"INS", CTRL_KEY_INSERT},       {"DELE", CTRL_KEY_DELETE},      {"HOME", CTRL_KEY_HOME},
        {"END", CTRL_KEY_END},          {"PGUP", CTRL_KEY_PAGE_UP},     {"PGDN", CTRL_KEY_PAGE_DOWN},
        {"KPDL", CTRL_KEY_KP_DOT},      {"KPDV", CTRL_KEY_KP_SLASH},    {"KPMU", CTRL_KEY_KP_ASTERISK},
        {"KPSU", CTRL_KEY_KP_MINUS},    {"KPAD", CTRL_KEY_KP_PLUS},     {"KPEN", CTRL_KEY_KP_ENTER},
        {"KPEQ", CTRL_KEY_KP_EQUAL},    {"KPCM", CTRL_KEY_KP_COMMA},    {"FK01", CTRL_KEY_F1},
        {"FK02", CTRL_KEY_F2},          {"FK03", CTRL_KEY_F3},          {"FK04", CTRL_KEY_F4},
        {"FK05", CTRL_KEY_F5},          {"FK06", CTRL_KEY_F6},          {"FK07", CTRL_KEY_F7},
        {"FK08", CTRL_KEY_F8},          {"FK09", CTRL_KEY_F9},          {"FK10", CTRL_KEY_F10},
        {"FK11", CTRL_KEY_F11},         {"FK12", CTRL_KEY_F12},         {"KP0", CTRL_KEY_KP_0},
        {"KP1", CTRL_KEY_KP_1},         {"KP2", CTRL_KEY_KP_2},         {"KP3", CTRL_KEY_KP_3},
        {"KP4", CTRL_KEY_KP_4},         {"KP5", CTRL_KEY_KP_5},         {"KP6", CTRL_KEY_KP_6},
        {"KP7", CTRL_KEY_KP_7},         {"KP8", CTRL_KEY_KP_8},         {"KP9", CTRL_KEY_KP_9},

    };
    XkbDescPtr kb = XkbGetMap(display, 0, XkbUseCoreKbd);
    if (!kb)
        return;
    if (XkbGetNames(display, XkbKeyNamesMask, kb) == Success && kb->names) {
        for (int k = kb->min_key_code; k <= kb->max_key_code && k < 256; k++) {
            char name[5] = {0};
            memcpy(name, kb->names->keys[k].name, 4);
            for (int j = 3; j >= 0 && name[j] == ' '; j--)
                name[j] = 0;
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
                if (!strcmp(name, names[i].name)) {
                    physical_keys[k] = names[i].key;
                    const char *label = XKeysymToString(XkbKeycodeToKeysym(display, (KeyCode)k, 0, 0));
                    if (label)
                        Controls_SetKeyLabel(names[i].key, label);
                }
        }
    }
    XkbFreeKeyboard(kb, XkbAllComponentsMask, True);
}
static void controls_sync_keys(void)
{
    char held[32];XQueryKeymap(display,held);
    for(int k=0;k<256;k++)if(physical_keys[k])ControlsRuntime_Key(physical_keys[k],(held[k/8]>>(k%8))&1);
}
/* Mouse: right circle (cancel), middle triangle; left is reserved for the
 * native menu bar and does not press a gameplay button. The wheel taps
 * d-pad up and down. */
static const uint16_t mouse_buttons[4] = {0, 0, 0x1000, 0x2000};
static volatile uint16_t mouse_bits;
static uint16_t wheel_bits;
static int wheel_frames;
static volatile uint16_t wheel_now;

int Platform_Scale(void) { return scale; }
void Platform_ApplyDisplaySettings(void) {}
int Platform_HasWindowModes(void) { return 0; }
void Platform_AudioStats(int *queued_frames, unsigned *underruns)
{
    if (queued_frames) *queued_frames = 0;
    if (underruns) *underruns = 0;
}

void Platform_Screenshot(int window_image)
{
    const char *directory = getenv("MEMORIES_SCREENSHOT_DIR");
    char path[1024], stamp[32];
    struct tm local;
    time_t now;
    FILE *file;
    int i, j;
    (void)window_image;
    if (!last.vram || last.w <= 0 || last.h <= 0) return;
    char user[1024];
    if (!directory || !*directory) {
        if (Paths_User(user, sizeof(user), "screenshots")) return;
        directory = user;
    }
    mkdir(directory, 0777);
    now = time(NULL);
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &local);
    if (snprintf(path, sizeof(path), "%s/%s-%u.ppm", directory, stamp, current_frame) >= (int)sizeof(path)) return;
    file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P6\n%d %d\n255\n", last.w, last.h);
    for (j = 0; j < last.h; j++) {
        const uint16_t *row = last.vram + ((last.y + j) & 511) * last.stride;
        for (i = 0; i < last.w; i++) {
            if (last.rgb24) {
                fwrite((const uint8_t *)(row + last.x) + i * 3, 1, 3, file);
            } else {
                uint16_t c = row[(last.x + i) & 1023];
                fputc((c & 0x1f) << 3, file);
                fputc(((c >> 5) & 0x1f) << 3, file);
                fputc(((c >> 10) & 0x1f) << 3, file);
            }
        }
    }
    fclose(file);
    fprintf(stderr, "memories-pc: screenshot: %s\n", path);
}

void Platform_SetScale(int wanted)
{
    if (wanted < 1 || wanted > 8) {
        return;
    }
    if (display) {
        pending_scale = wanted; /* applied with the next frame */
    } else {
        scale = wanted;
    }
}

static void destroy_image(void)
{
    if (!image) {
        return;
    }
    if (shm_ok) {
        XShmDetach(display, &shm);
        shmdt(shm.shmaddr);
        image->data = NULL;
    }
    XDestroyImage(image);
    image = NULL;
    shm_ok = 0;
}

static void create_image(int width, int height)
{
    int screen = DefaultScreen(display), major, minor;
    Bool pixmaps;
    destroy_image();
    if (!getenv("MEMORIES_NO_SHM") && XShmQueryVersion(display, &major, &minor, &pixmaps)) {
        image = XShmCreateImage(display, DefaultVisual(display, screen), (unsigned)DefaultDepth(display, screen),
                                ZPixmap, NULL, &shm, (unsigned)width, (unsigned)height);
        if (image) {
            shm.shmid = shmget(IPC_PRIVATE, (size_t)image->bytes_per_line * (size_t)height, IPC_CREAT | 0600);
            shm.shmaddr = shm.shmid >= 0 ? shmat(shm.shmid, NULL, 0) : (char *)-1;
            shm.readOnly = False;
            if (shm.shmaddr != (char *)-1 && XShmAttach(display, &shm)) {
                image->data = shm.shmaddr;
                XSync(display, False);
                shmctl(shm.shmid, IPC_RMID, NULL); /* freed with the last detach */
                shm_ok = 1;
            } else {
                if (shm.shmaddr != (char *)-1) {
                    shmdt(shm.shmaddr);
                }
                if (shm.shmid >= 0) {
                    shmctl(shm.shmid, IPC_RMID, NULL);
                }
                XDestroyImage(image);
                image = NULL;
            }
        }
    }
    if (!image) {
        image = XCreateImage(display, DefaultVisual(display, screen), (unsigned)DefaultDepth(display, screen), ZPixmap,
                             0, malloc((size_t)width * (size_t)height * 4), (unsigned)width, (unsigned)height, 32, 0);
    }
    image_w = width;
    image_h = height;
    canvas.pixels = (uint32_t *)image->data;
    canvas.stride = image->bytes_per_line / 4;
    canvas.width = width;
    canvas.height = height;
    memset(image->data, 0, (size_t)image->bytes_per_line * (size_t)height);
}

/* Scale the presented VRAM rectangle into frame rows [top, bottom). Each
 * source line is converted once, widened, then copied down. */
static void scale_game(int top, int bottom)
{
    int menu = Menu_Height(), j;
    if (!last.vram) {
        return;
    }
    if (top < menu) {
        top = menu;
    }
    if (bottom > menu + last.h * scale) {
        bottom = menu + last.h * scale;
    }
    for (j = (top - menu) / scale; j * scale + menu < bottom; j++) {
        const uint16_t *row = last.vram + ((last.y + j) & 511) * last.stride;
        int first = menu + j * scale, k, i;
        uint32_t *line = canvas.pixels + (size_t)first * (size_t)canvas.stride, *at = line;
        for (i = 0; i < last.w; i++) {
            uint32_t colour;
            if (last.rgb24) {
                const uint8_t *bytes = (const uint8_t *)(row + last.x) + i * 3;
                colour = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
            } else {
                uint16_t c = row[(last.x + i) & 1023];
                uint32_t r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
                colour = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
            for (k = 0; k < scale; k++) {
                *at++ = colour;
            }
        }
        for (k = 1; k < scale && first + k < bottom; k++) {
            memcpy(line + (size_t)k * (size_t)canvas.stride, line, (size_t)image_w * 4);
        }
    }
}

static void show(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > image_w) { w = image_w - x; }
    if (y + h > image_h) { h = image_h - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    if (shm_ok) {
        XShmPutImage(display, window, context, image, x, y, x, y, (unsigned)w, (unsigned)h, False);
    } else {
        XPutImage(display, window, context, image, x, y, x, y, (unsigned)w, (unsigned)h);
    }
    /* Wait for the server to have read the frame before it is drawn over. */
    XSync(display, False);
}

/* A new game frame: everything is repainted and shown. */
static void present_frame(void)
{
    int hx, hy, hw, hh;
    scale_game(0, image_h);
    if (Settings_Get(SET_SHOW_HUD) == 2) {
        Hud_Draw(&canvas);
        Menu_Draw(&canvas);
    } else {
        Menu_Draw(&canvas);
        Hud_Draw(&canvas);
    }
    Menu_Bounds(&shown_menu.x, &shown_menu.y, &shown_menu.w, &shown_menu.h);
    Hud_Bounds(&hx, &hy, &hw, &hh);
    if (hw && hh && (!shown_menu.w || !shown_menu.h)) {
        shown_menu.x = hx; shown_menu.y = hy; shown_menu.w = hw; shown_menu.h = hh;
    } else if (hw && hh) {
        int x1 = shown_menu.x + shown_menu.w > hx + hw ? shown_menu.x + shown_menu.w : hx + hw;
        int y1 = shown_menu.y + shown_menu.h > hy + hh ? shown_menu.y + shown_menu.h : hy + hh;
        shown_menu.x = shown_menu.x < hx ? shown_menu.x : hx;
        shown_menu.y = shown_menu.y < hy ? shown_menu.y : hy;
        shown_menu.w = x1 - shown_menu.x; shown_menu.h = y1 - shown_menu.y;
    }
    show(0, 0, image_w, image_h);
}

/* The menu changed under a still picture: repaint the game beneath where
 * it was and where it is, the menu over that, and show just that much. */
static void repaint_menu(void)
{
    int old_x = shown_menu.x, old_y = shown_menu.y;
    int old_w = shown_menu.w, old_h = shown_menu.h;
    int x, y, w, h, hx, hy, hw, hh, x0, y0, x1, y1;
    if (old_w && old_h) {
        scale_game(old_y, old_y + old_h);
    }
    if (Settings_Get(SET_SHOW_HUD) == 2) {
        Hud_Draw(&canvas);
        Menu_Draw(&canvas);
    } else {
        Menu_Draw(&canvas);
        Hud_Draw(&canvas);
    }
    Menu_Bounds(&x, &y, &w, &h);
    Hud_Bounds(&hx, &hy, &hw, &hh);
    if (hw && hh && (!w || !h)) { x = hx; y = hy; w = hw; h = hh; }
    else if (hw && hh) {
        int right = x + w > hx + hw ? x + w : hx + hw, bottom = y + h > hy + hh ? y + h : hy + hh;
        x = x < hx ? x : hx; y = y < hy ? y : hy; w = right - x; h = bottom - y;
    }
    x0 = !old_w || x < old_x ? x : old_x;
    y0 = !old_h || y < old_y ? y : old_y;
    x1 = x + w > old_x + old_w ? x + w : old_x + old_w;
    y1 = y + h > old_y + old_h ? y + h : old_y + old_h;
    shown_menu.x = x;
    shown_menu.y = y;
    shown_menu.w = w;
    shown_menu.h = h;
    show(x0, y0, x1 - x0, y1 - y0);
}

int Platform_Open(const char *title)
{
    XSizeHints hints;
    ControlsRuntime_Init();
    Menu_LoadSettings(); /* the volume and scale apply with or without a window */
    snprintf(base_title, sizeof(base_title), "%s", title);
    if (getenv("MEMORIES_HEADLESS")) {
        return 0;
    }
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "memories-pc: no X display; set MEMORIES_HEADLESS=1 to run without a window\n");
        return -1;
    }
    window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 320u * (unsigned)scale,
                                 240u * (unsigned)scale + (unsigned)Menu_Height(), 0, 0, 0);
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = 320 * scale;
    hints.min_height = hints.max_height = 240 * scale + Menu_Height();
    XSetWMNormalHints(display, window, &hints);
    XStoreName(display, window, title);
    XSelectInput(display, window, KeyPressMask | KeyReleaseMask | FocusChangeMask | ButtonPressMask | ButtonReleaseMask |
                                  PointerMotionMask | LeaveWindowMask | StructureNotifyMask | ExposureMask);
    close_atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &close_atom, 1);
    XMapWindow(display, window);
    context = XCreateGC(display, window, 0, NULL);
    controls_keys_init();
    Menu_Init();
    Menu_SetItemEnabled(MENU_ITEM_FULLSCREEN, 0);
    Menu_SetItemEnabled(MENU_ITEM_BORDERLESS, 0);
    Menu_SetItemEnabled(MENU_ITEM_SCALING_INTEGER, 0);
    Menu_SetItemEnabled(MENU_ITEM_SCALING_FIT, 0);
    Menu_SetItemEnabled(MENU_ITEM_SCALING_STRETCH, 0);
    Menu_SetItemEnabled(MENU_ITEM_ASPECT_4_3, 0);
    Menu_SetItemEnabled(MENU_ITEM_ASPECT_SQUARE, 0);
    Menu_SetItemEnabled(MENU_ITEM_ASPECT_WIDESCREEN, 0);
    Menu_SetItemEnabled(MENU_ITEM_FILTER, 0);
    Menu_SetItemEnabled(MENU_ITEM_VSYNC, 0);
    update_title();
    return 0;
}

/* XEvent to the menu's own event. */
static const MenuEvent *translate(const XEvent *event)
{
    static MenuEvent out;
    memset(&out, 0, sizeof(out));
    switch (event->type) {
    case ButtonPress: case ButtonRelease:
        out.x = event->xbutton.x;
        out.y = event->xbutton.y;
        if (event->xbutton.button == Button4 || event->xbutton.button == Button5) {
            if (event->type == ButtonPress) {
                out.type = MENU_EVENT_WHEEL;
                out.wheel = event->xbutton.button == Button4 ? 1 : -1;
            }
        } else {
            out.type = event->type == ButtonPress ? MENU_EVENT_BUTTON_DOWN : MENU_EVENT_BUTTON_UP;
            out.button = (int)event->xbutton.button;
        }
        break;
    case MotionNotify: out.type = MENU_EVENT_MOTION; out.x = event->xmotion.x; out.y = event->xmotion.y; break;
    case LeaveNotify: out.type = MENU_EVENT_LEAVE; break;
    case KeyPress: case KeyRelease: {
        KeySym key = XLookupKeysym((XKeyEvent *)&event->xkey, 0);
        out.type = event->type == KeyPress ? MENU_EVENT_KEY_DOWN : MENU_EVENT_KEY_UP;
        out.key = key == XK_Escape ? MENU_KEY_ESCAPE : key == XK_F10 ? MENU_KEY_F10 : key == XK_Left ? MENU_KEY_LEFT
                : key == XK_Right ? MENU_KEY_RIGHT : key == XK_Up ? MENU_KEY_UP : key == XK_Down ? MENU_KEY_DOWN
                : key == XK_Return || key == XK_KP_Enter || key == XK_space ? MENU_KEY_ENTER : MENU_KEY_OTHER;
        break;
    }
    default: break;
    }
    return &out;
}

static Window mods_window;
static XImage *mods_image;
static MenuCanvas mods_canvas;
static void close_mods(void)
{
    if (mods_image) XDestroyImage(mods_image);
    if (mods_window) XDestroyWindow(display, mods_window);
    mods_image = NULL; mods_window = 0;
}
static void draw_mods(void)
{
    ModsWindow_Draw(&mods_canvas);
    XPutImage(display, mods_window, context, mods_image, 0, 0, 0, 0,
        (unsigned)mods_canvas.width, (unsigned)mods_canvas.height);
    XFlush(display);
}
void Platform_OpenMods(void)
{
    int screen;
    XSizeHints hints = {0};
    if (!display) return;
    if (mods_window) { XMapRaised(display, mods_window); return; }
    ModsWindow_Init();
    ModsWindow_Size(&mods_canvas.width, &mods_canvas.height);
    screen = DefaultScreen(display);
    mods_image = XCreateImage(display, DefaultVisual(display, screen),
        (unsigned)DefaultDepth(display, screen), ZPixmap, 0, NULL,
        (unsigned)mods_canvas.width, (unsigned)mods_canvas.height, 32, 0);
    if (!mods_image) return;
    mods_image->data = calloc((size_t)mods_image->bytes_per_line, mods_canvas.height);
    if (!mods_image->data || mods_image->bits_per_pixel != 32) { close_mods(); return; }
    mods_canvas.pixels = (uint32_t *)mods_image->data;
    mods_canvas.stride = mods_image->bytes_per_line / 4;
    mods_window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0,
        (unsigned)mods_canvas.width, (unsigned)mods_canvas.height, 0, 0, 0);
    XStoreName(display, mods_window, "MODS");
    XSetTransientForHint(display, mods_window, window);
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = mods_canvas.width;
    hints.min_height = hints.max_height = mods_canvas.height;
    XSetWMNormalHints(display, mods_window, &hints);
    XSetWMProtocols(display, mods_window, &close_atom, 1);
    XSelectInput(display, mods_window, ExposureMask | KeyPressMask | KeyReleaseMask |
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask | LeaveWindowMask);
    XMapRaised(display, mods_window);
    draw_mods();
}

static Window controls_window;
static XImage *controls_image;
static MenuCanvas controls_canvas;
static void close_controls(void)
{
    if (controls_image)
        XDestroyImage(controls_image);
    if (controls_window)
        XDestroyWindow(display, controls_window);
    controls_image = NULL;
    controls_window = 0;
    mouse_bits = wheel_now = 0;
    wheel_frames = 0;
    ControlsRuntime_Block(0);
}
/* Rebuild the image for a new window size; keep the old one on failure. */
static void resize_controls(int w, int h)
{
    if(!controls_window||w<1||h<1||(w==controls_canvas.width&&h==controls_canvas.height))return;
    int screen=DefaultScreen(display);
    XImage *image=XCreateImage(display,DefaultVisual(display,screen),(unsigned)DefaultDepth(display,screen),ZPixmap,0,NULL,
        (unsigned)w,(unsigned)h,32,0);
    if(!image)return;
    image->data=calloc((size_t)image->bytes_per_line,h);
    if(!image->data||image->bits_per_pixel!=32){XDestroyImage(image);return;}
    XDestroyImage(controls_image);
    controls_image=image;
    controls_canvas.pixels=(uint32_t *)image->data;
    controls_canvas.stride=image->bytes_per_line/4;
    controls_canvas.width=w;
    controls_canvas.height=h;
}
static void draw_controls(void)
{
    ControlsWindow_Draw(&controls_canvas);
    XPutImage(display,controls_window,context,controls_image,0,0,0,0,(unsigned)controls_canvas.width,(unsigned)controls_canvas.height);
    XFlush(display);
}
void Platform_OpenControls(void)
{
    if(!display)return;
    if(controls_window){XMapRaised(display,controls_window);return;}
    ControlsWindow_Init();controls_sync_keys();ControlsWindow_Size(&controls_canvas.width,&controls_canvas.height);
    int screen=DefaultScreen(display),sw=DisplayWidth(display,screen),sh=DisplayHeight(display,screen)-60;
    if(controls_canvas.width>sw)controls_canvas.width=sw;
    if(controls_canvas.height>sh)controls_canvas.height=sh;
    controls_image=XCreateImage(display,DefaultVisual(display,screen),(unsigned)DefaultDepth(display,screen),ZPixmap,0,NULL,
        (unsigned)controls_canvas.width,(unsigned)controls_canvas.height,32,0);
    if(!controls_image){ControlsRuntime_Block(0);return;}
    controls_image->data=calloc((size_t)controls_image->bytes_per_line,controls_canvas.height);
    if(!controls_image->data||controls_image->bits_per_pixel!=32){close_controls();return;}
    controls_canvas.pixels=(uint32_t *)controls_image->data;controls_canvas.stride=controls_image->bytes_per_line/4;
    controls_window=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,(unsigned)controls_canvas.width,(unsigned)controls_canvas.height,0,0,0);
    XStoreName(display,controls_window,"Controls");XSetTransientForHint(display,controls_window,window);
    XSizeHints hints={0};hints.flags=PMinSize;ControlsWindow_MinSize(&hints.min_width,&hints.min_height);
    XSetWMNormalHints(display,controls_window,&hints);XSetWMProtocols(display,controls_window,&close_atom,1);
    XSelectInput(display,controls_window,ExposureMask|KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|FocusChangeMask|StructureNotifyMask);
    XMapRaised(display,controls_window);mouse_bits=wheel_now=0;wheel_frames=0;draw_controls();
}

static void pump(void)
{
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        if(event.type==MappingNotify){XRefreshKeyboardMapping(&event.xmapping);controls_keys_init();continue;}
        if(event.type==KeyRelease && XPending(display)) {
            XEvent next;XPeekEvent(display,&next);
            if(next.type==KeyPress && next.xkey.time==event.xkey.time && next.xkey.keycode==event.xkey.keycode){XNextEvent(display,&next);continue;}
        }
        if(event.type==KeyRelease)ControlsRuntime_Key(physical_keys[event.xkey.keycode&255],0);
        if(controls_window && event.xany.window==controls_window) {
            if(event.type==KeyPress || event.type==KeyRelease) {
                int key=physical_keys[event.xkey.keycode&255],down=event.type==KeyPress;
                ControlsRuntime_Key(key,down);
                int mods=(event.xkey.state&ShiftMask?1:0)|(event.xkey.state&(ControlMask|Mod1Mask|Mod4Mask)?2:0);
                if(key==CTRL_KEY_RIGHT_SHIFT && !mods)mods=0;
                ControlsWindow_Key(key,down,0,mods);
                ControlsWindow_Tick();
            } else if(event.type==ClientMessage && (Atom)event.xclient.data.l[0]==close_atom)ControlsWindow_RequestClose();
            else if(event.type==FocusOut)ControlsWindow_FocusLost();
            else if(event.type==FocusIn)controls_sync_keys();
            else if(event.type==ConfigureNotify)resize_controls(event.xconfigure.width,event.xconfigure.height);
            else ControlsWindow_Event(translate(&event));
            if(ControlsWindow_ShouldClose())close_controls();
            continue;
        }
        if(controls_window && (event.type==KeyPress||event.type==KeyRelease||event.type==ButtonPress||event.type==ButtonRelease))continue;
        if(event.type==FocusOut){if(!controls_window)ControlsRuntime_ResetKeys();mouse_bits=wheel_now=0;wheel_frames=0;}

        if (mods_window && event.xany.window == mods_window) {
            if ((event.type == ClientMessage && (Atom)event.xclient.data.l[0] == close_atom) ||
                ModsWindow_Event(translate(&event))) close_mods();
            else draw_mods();
            continue;
        }
        if (Menu_Event(translate(&event), &quit)) {
            repaint_menu(); /* the menu answers now, not at the next frame */
            continue;
        }
        if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == close_atom) {
            quit = 1;
        } else if (event.type == Expose) {
            if (image) {
                show(event.xexpose.x, event.xexpose.y, event.xexpose.width, event.xexpose.height);
            }
        } else if (event.type == ButtonPress || event.type == ButtonRelease) {
            unsigned button = event.xbutton.button;
            if (event.xbutton.y < Menu_Height()) {
                continue;
            }
            if (button >= 1 && button <= 3) {
                mouse_bits = event.type == ButtonPress ? (uint16_t)(mouse_bits | mouse_buttons[button])
                                                       : (uint16_t)(mouse_bits & ~mouse_buttons[button]);
            } else if (event.type == ButtonPress && (button == 4 || button == 5)) {
                wheel_bits = button == 4 ? 0x0010 : 0x0040; /* one notch: a short tap */
                wheel_frames = 3;
            }
        } else if (event.type == KeyPress || event.type == KeyRelease) {
            KeySym key = XLookupKeysym(&event.xkey, 0);
            /* Auto-repeat arrives as release+press with one timestamp. */
            if (event.type == KeyRelease && XPending(display)) {
                XEvent next;
                XPeekEvent(display, &next);
                if (next.type == KeyPress && next.xkey.time == event.xkey.time &&
                    next.xkey.keycode == event.xkey.keycode) {
                    XNextEvent(display, &next);
                    continue;
                }
            }
            if (key == XK_Escape && event.type == KeyPress) {
                quit = 1;
            }
            if (key == XK_Tab) {
                Platform_SetClockRate(event.type == KeyPress ? 400 : Settings_Get(SET_SPEED));
                continue;
            }
            if (event.type == KeyPress && key == XK_F3) {
                Settings_Set(SET_SHOW_HUD, (Settings_Get(SET_SHOW_HUD) + 1) % 3);
                Settings_Save();
                repaint_menu();
                continue;
            }
            if (event.type == KeyPress && (key == XK_p || key == XK_P)) {
                Platform_SetClockRate(Platform_ClockRate() == 0 ? Settings_Get(SET_SPEED) : 0);
                continue;
            }
            if (event.type == KeyPress && key == XK_period && Platform_ClockRate() == 0) {
                Platform_StepFrame();
                continue;
            }
            if (event.type == KeyPress && (key == XK_m || key == XK_M)) {
                Spu_SetMuted(!Spu_Muted());
                continue;
            }
            /* F3 belongs to the HUD; slot 3 remains available from File. */
            if (event.type == KeyPress && key >= XK_F1 && key <= XK_F4) {
                if (key != XK_F3) Platform_SetStateSlot((int)(key - XK_F1) + 1);
            } else if (event.type == KeyPress && (key == XK_F5 || key == XK_F7)) {
                Memories_StateRequest(key == XK_F5 ? 1 : 2, state_slot);
            }
            ControlsRuntime_Key(physical_keys[event.xkey.keycode&255],event.type==KeyPress);

        }
    }
    Gamepad_Poll(current_frame);
    if(controls_window) {
        static uint64_t last_draw;ControlsWindow_Tick();
        if(ControlsWindow_ShouldClose())close_controls();
        else if(ControlsRuntime_Now()-last_draw>=16000){draw_controls();last_draw=ControlsRuntime_Now();}
    }

}

int Platform_PresentPicture(const uint32_t *pixels, int stride, int x, int y, int w, int h, int scale)
{
    (void)pixels; (void)stride; (void)x; (void)y; (void)w; (void)h; (void)scale;
    return 0; /* the X11 backend shows VRAM as it is */
}

void Platform_Present(const uint16_t *vram, int stride, int x, int y, int w, int h, int rgb24)
{
    int width, height;
    if (!display || w <= 0 || h <= 0) {
        return;
    }
    last.vram = vram;
    last.stride = stride;
    last.x = x;
    last.y = y;
    last.w = w;
    last.h = h;
    last.rgb24 = rgb24;
    if (pending_scale) {
        scale = pending_scale;
        pending_scale = 0;
    }
    width = w * scale;
    height = h * scale + Menu_Height();
    if (!image || image_w != width || image_h != height) {
        XSizeHints hints;
        create_image(width, height);
        hints.flags = PMinSize | PMaxSize;
        hints.min_width = hints.max_width = width;
        hints.min_height = hints.max_height = height;
        XSetWMNormalHints(display, window, &hints);
        XResizeWindow(display, window, (unsigned)width, (unsigned)height);
    }
    present_frame();
    pump();
}

int Platform_ShouldQuit(void)
{
    return quit;
}
int Platform_StateSlot(void) { return state_slot; }
void Platform_SetStateSlot(int slot)
{
    if (slot < 1 || slot > 4) return;
    state_slot = slot;
    update_title();
}

void Platform_PumpEvents(void) { if (display) pump(); }

uint16_t Platform_Pad(int port)
{
    return port == 0 ? (uint16_t)(ControlsRuntime_Keyboard() | (ControlsRuntime_Blocked()?0:(mouse_bits | wheel_now)) | scripted_bits | Gamepad_Bits(0))
                     : Gamepad_Bits(1);
}

int Platform_PadConnected(int port) { return port == 0 || Gamepad_Connected(port); }

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
    if (shown_rate != Platform_ClockRate()) {
        shown_rate = Platform_ClockRate();
        update_title();
    }
    Gamepad_Poll(frame);
    Cheats_Frame();
    wheel_now = wheel_frames > 0 && wheel_frames-- ? wheel_bits : 0;
    scripted_bits = Platform_ScriptedBits(frame);
}
