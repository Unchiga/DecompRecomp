#define _GNU_SOURCE
#include "platform.h"
#include "pc/debug/cheats.h"
#include "pc/guest/state.h"
#include "menu.h"
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
static volatile uint16_t pad_bits, scripted_bits;
static int state_slot = 1;

/* Arrows d-pad; X cross, S circle, Z square, A triangle; Q/W L1/R1, E/R
 * L2/R2, T/Y L3/R3; Enter start, right Shift select. */
static const struct { KeySym key; uint16_t bit; } keymap[] = {
    {XK_Shift_R, 0x0001}, {XK_t, 0x0002}, {XK_y, 0x0004}, {XK_Return, 0x0008},
    {XK_Up, 0x0010}, {XK_Right, 0x0020}, {XK_Down, 0x0040}, {XK_Left, 0x0080},
    {XK_e, 0x0100}, {XK_r, 0x0200}, {XK_q, 0x0400}, {XK_w, 0x0800},
    {XK_a, 0x1000}, {XK_s, 0x2000}, {XK_x, 0x4000}, {XK_z, 0x8000}};
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
    scale_game(0, image_h);
    Menu_Draw(&canvas);
    Menu_Bounds(&shown_menu.x, &shown_menu.y, &shown_menu.w, &shown_menu.h);
    show(0, 0, image_w, image_h);
}

/* The menu changed under a still picture: repaint the game beneath where
 * it was and where it is, the menu over that, and show just that much. */
static void repaint_menu(void)
{
    int x, y, w, h, x0, y0, x1, y1;
    Menu_Bounds(&x, &y, &w, &h);
    x0 = x < shown_menu.x ? x : shown_menu.x;
    y0 = y < shown_menu.y ? y : shown_menu.y;
    x1 = x + w > shown_menu.x + shown_menu.w ? x + w : shown_menu.x + shown_menu.w;
    y1 = y + h > shown_menu.y + shown_menu.h ? y + h : shown_menu.y + shown_menu.h;
    scale_game(y0, y1);
    Menu_Draw(&canvas);
    shown_menu.x = x;
    shown_menu.y = y;
    shown_menu.w = w;
    shown_menu.h = h;
    show(x0, y0, x1 - x0, y1 - y0);
}

int Platform_Open(const char *title)
{
    XSizeHints hints;
    Menu_LoadSettings(); /* the volume and scale apply with or without a window */
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
    XSelectInput(display, window, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                                  PointerMotionMask | LeaveWindowMask | StructureNotifyMask | ExposureMask);
    close_atom = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &close_atom, 1);
    XMapWindow(display, window);
    context = XCreateGC(display, window, 0, NULL);
    Menu_Init();
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

static void pump(void)
{
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
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
            size_t i;
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
            /* Save states: F1-F4 pick a slot, F5 saves it, F7 loads it. */
            if (event.type == KeyPress && key >= XK_F1 && key <= XK_F4) {
                char title[96];
                state_slot = (int)(key - XK_F1) + 1;
                snprintf(title, sizeof(title), "Yu-Gi-Oh! Forbidden Memories - state slot %d (F5 save, F7 load)", state_slot);
                XStoreName(display, window, title);
            } else if (event.type == KeyPress && (key == XK_F5 || key == XK_F7)) {
                Memories_StateRequest(key == XK_F5 ? 1 : 2, state_slot);
            }
            for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
                if (keymap[i].key == key) {
                    pad_bits = event.type == KeyPress ? (uint16_t)(pad_bits | keymap[i].bit)
                                                      : (uint16_t)(pad_bits & ~keymap[i].bit);
                }
            }
        }
    }
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

uint16_t Platform_Pad(int port)
{
    return port == 0 ? (uint16_t)(pad_bits | mouse_bits | wheel_now | scripted_bits | Gamepad_Bits(0))
                     : Gamepad_Bits(1);
}

int Platform_PadConnected(int port) { return port == 0 || Gamepad_Connected(port); }

void Platform_Frame(unsigned frame)
{
    Gamepad_Poll(frame);
    Cheats_Frame();
    wheel_now = wheel_frames > 0 && wheel_frames-- ? wheel_bits : 0;
    scripted_bits = Platform_ScriptedBits(frame);
}
