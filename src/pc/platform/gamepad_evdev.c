/* Game controllers through Linux evdev, found and re-found automatically.
 *
 * evdev reports what a control means rather than where a driver put it, so
 * one table covers Xbox pads over USB, the wireless adapter and Bluetooth
 * (xpad, xone, xpadneo) and most other pads. The first controller joins the
 * keyboard on port 1; a second one is port 2. /dev/input is rescanned about
 * once a second while a port is free, and a pad that disappears frees its
 * port. Everything here runs on the main thread, from the per-frame pump;
 * the VBlank handler only reads the resulting words.
 *
 * Xbox to PlayStation: A cross, B circle, X square, Y triangle, LB/RB L1/R1,
 * triggers L2/R2, stick clicks L3/R3, View/Back select, Menu/Start start.
 * The d-pad and the left stick both drive the digital pad. */
#define _GNU_SOURCE
#include "platform.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum { SELECT = 0x0001, L3 = 0x0002, R3 = 0x0004, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040,
       LEFT = 0x0080, L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800, TRIANGLE = 0x1000, CIRCLE = 0x2000,
       CROSS = 0x4000, SQUARE = 0x8000 };

typedef struct Pad {
    int fd;
    char node[64];
    uint16_t buttons, hat, stick, triggers;
    struct input_absinfo x, y, left_trigger, right_trigger;
    int stick_x, stick_y;
} Pad;

static Pad pads[2] = {{.fd = -1}, {.fd = -1}};
static volatile uint16_t bits[2];
static volatile int connected[2];

/* In xpad's naming BTN_X/BTN_Y are the physical X and Y (the kernel's
 * compass aliases for them are swapped), so use those names. */
static const struct { unsigned code; uint16_t bit; } buttons[] = {
    {BTN_A, CROSS}, {BTN_B, CIRCLE}, {BTN_X, SQUARE}, {BTN_Y, TRIANGLE}, {BTN_TL, L1}, {BTN_TR, R1},
    {BTN_TL2, L2}, {BTN_TR2, R2}, {BTN_SELECT, SELECT}, {BTN_START, START}, {BTN_THUMBL, L3},
    {BTN_THUMBR, R3}, {BTN_DPAD_UP, UP}, {BTN_DPAD_DOWN, DOWN}, {BTN_DPAD_LEFT, LEFT}, {BTN_DPAD_RIGHT, RIGHT}};

static int has(const unsigned long *mask, unsigned code)
{
    return (int)(mask[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long))) & 1);
}

static int already_open(const char *node)
{
    return (pads[0].fd >= 0 && !strcmp(pads[0].node, node)) || (pads[1].fd >= 0 && !strcmp(pads[1].node, node));
}

static void scan(void)
{
    DIR *directory = opendir("/dev/input");
    struct dirent *item;
    while (directory && (item = readdir(directory)) != NULL) {
        unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        char node[64], name[128] = "controller";
        Pad *pad = pads[0].fd < 0 ? &pads[0] : pads[1].fd < 0 ? &pads[1] : NULL;
        int fd;
        if (!pad) {
            break;
        }
        if (strncmp(item->d_name, "event", 5) != 0) {
            continue;
        }
        snprintf(node, sizeof(node), "/dev/input/%.40s", item->d_name);
        if (already_open(node) || (fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0) {
            continue;
        }
        /* A gamepad has the south face button; keyboards and mice do not. */
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !has(keys, BTN_GAMEPAD)) {
            close(fd);
            continue;
        }
        memset(pad, 0, sizeof(*pad));
        pad->fd = fd;
        snprintf(pad->node, sizeof(pad->node), "%s", node);
        ioctl(fd, EVIOCGABS(ABS_X), &pad->x);
        ioctl(fd, EVIOCGABS(ABS_Y), &pad->y);
        ioctl(fd, EVIOCGABS(ABS_Z), &pad->left_trigger);
        ioctl(fd, EVIOCGABS(ABS_RZ), &pad->right_trigger);
        pad->stick_x = (pad->x.minimum + pad->x.maximum) / 2;
        pad->stick_y = (pad->y.minimum + pad->y.maximum) / 2;
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        fprintf(stderr, "memories-pc: controller on port %d: %s (%s)\n", (int)(pad - pads) + 1, name, node);
        connected[pad - pads] = 1;
    }
    if (directory) {
        closedir(directory);
    }
}

static int beyond(const struct input_absinfo *axis, int value, int percent)
{
    int centre = (axis->minimum + axis->maximum) / 2, reach = (axis->maximum - axis->minimum) / 2;
    if (reach <= 0) {
        return 0;
    }
    return value > centre + reach * percent / 100 ? 1 : value < centre - reach * percent / 100 ? -1 : 0;
}

static void handle(Pad *pad, const struct input_event *event)
{
    size_t i;
    if (event->type == EV_KEY) {
        for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (buttons[i].code == event->code) {
                pad->buttons = event->value ? pad->buttons | buttons[i].bit : pad->buttons & (uint16_t)~buttons[i].bit;
            }
        }
    } else if (event->type == EV_ABS) {
        switch (event->code) {
        case ABS_HAT0X:
            pad->hat = (uint16_t)((pad->hat & ~(LEFT | RIGHT)) | (event->value < 0 ? LEFT : event->value > 0 ? RIGHT : 0));
            break;
        case ABS_HAT0Y:
            pad->hat = (uint16_t)((pad->hat & ~(UP | DOWN)) | (event->value < 0 ? UP : event->value > 0 ? DOWN : 0));
            break;
        case ABS_X: pad->stick_x = event->value; break;
        case ABS_Y: pad->stick_y = event->value; break;
        case ABS_Z: /* analogue triggers: pressed past a third of their travel */
        case ABS_RZ: {
            const struct input_absinfo *axis = event->code == ABS_Z ? &pad->left_trigger : &pad->right_trigger;
            uint16_t bit = event->code == ABS_Z ? L2 : R2;
            int pressed = axis->maximum > axis->minimum &&
                          event->value > axis->minimum + (axis->maximum - axis->minimum) / 3;
            pad->triggers = pressed ? pad->triggers | bit : pad->triggers & (uint16_t)~bit;
            break;
        }
        default: break;
        }
        {
            int x = beyond(&pad->x, pad->stick_x, 50), y = beyond(&pad->y, pad->stick_y, 50);
            pad->stick = (uint16_t)((x < 0 ? LEFT : x > 0 ? RIGHT : 0) | (y < 0 ? UP : y > 0 ? DOWN : 0));
        }
    }
}

void Gamepad_Poll(unsigned frame)
{
    int port;
    if (getenv("MEMORIES_NO_GAMEPAD")) {
        return;
    }
    if ((pads[0].fd < 0 || pads[1].fd < 0) && frame % 60 == 1) {
        scan();
    }
    for (port = 0; port < 2; port++) {
        Pad *pad = &pads[port];
        struct input_event events[32];
        ssize_t got;
        while (pad->fd >= 0 && (got = read(pad->fd, events, sizeof(events))) != 0) {
            ssize_t i;
            if (got < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    break;
                }
                fprintf(stderr, "memories-pc: controller on port %d disconnected\n", port + 1);
                close(pad->fd);
                pad->fd = -1;
                pad->buttons = pad->hat = pad->stick = pad->triggers = 0;
                connected[port] = 0;
                break;
            }
            for (i = 0; i < got / (ssize_t)sizeof(events[0]); i++) {
                handle(pad, &events[i]);
            }
        }
        bits[port] = (uint16_t)(pad->buttons | pad->hat | pad->stick | pad->triggers);
    }
}

uint16_t Gamepad_Bits(int port) { return port >= 0 && port < 2 ? bits[port] : 0; }
int Gamepad_Connected(int port) { return port >= 0 && port < 2 && connected[port]; }
