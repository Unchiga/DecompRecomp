/* Legacy X11 controller adapter. Device discovery is independent of ports. */
#define _GNU_SOURCE
#include "controls_linux.h"
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

typedef struct {
    int fd, dropped;
    char node[64];
    struct input_absinfo axes[6];
} Pad;
static Pad pads[CONTROLS_DEVICES];
static int initialized;
static const unsigned axis_codes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ};
static const struct {
    unsigned code;
    int token;
} buttons[] = {{BTN_A, CTRL_BTN_SOUTH},
               {BTN_B, CTRL_BTN_EAST},
               {BTN_X, CTRL_BTN_WEST},
               {BTN_Y, CTRL_BTN_NORTH},
               {BTN_SELECT, CTRL_BTN_BACK},
               {BTN_MODE, CTRL_BTN_GUIDE},
               {BTN_START, CTRL_BTN_START},
               {BTN_TL, CTRL_BTN_LEFT_SHOULDER},
               {BTN_TR, CTRL_BTN_RIGHT_SHOULDER},
               {BTN_THUMBL, CTRL_BTN_LEFT_STICK},
               {BTN_THUMBR, CTRL_BTN_RIGHT_STICK}};
static int has(const unsigned long *mask, unsigned code)
{
    return (int)((mask[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1);
}
static float normalize(const struct input_absinfo *a, int trigger)
{
    double min = a->minimum, max = a->maximum, value = a->value;
    if (max <= min)
        return 0;
    if (value < min)
        value = min;
    if (value > max)
        value = max;
    if (trigger)
        return (float)((value - min) / (max - min));
    double center = (min + max) / 2;
    return (float)((value - center) / (value < center ? center - min : max - center));
}
static void synchronize(int i)
{
    Pad *p = &pads[i];
    ControllerSnapshot *s = &ControlsRuntime_Device(i)->snapshot;
    unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    struct input_absinfo hat = {0};
    memset(s, 0, sizeof(*s));
    if (ioctl(p->fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
        return;
    for (unsigned b = 0; b < sizeof(buttons) / sizeof(buttons[0]); b++)
        if (has(keys, buttons[b].code))
            s->buttons_down |= 1u << (buttons[b].token - 1);
    if (has(keys, BTN_DPAD_UP))
        s->hat_down |= 1;
    if (has(keys, BTN_DPAD_RIGHT))
        s->hat_down |= 2;
    if (has(keys, BTN_DPAD_DOWN))
        s->hat_down |= 4;
    if (has(keys, BTN_DPAD_LEFT))
        s->hat_down |= 8;
    if (ioctl(p->fd, EVIOCGABS(ABS_HAT0X), &hat) == 0)
        s->hat_down |= hat.value < 0 ? 8 : hat.value > 0 ? 2 : 0;
    if (ioctl(p->fd, EVIOCGABS(ABS_HAT0Y), &hat) == 0)
        s->hat_down |= hat.value < 0 ? 1 : hat.value > 0 ? 4 : 0;
    for (int a = 0; a < 6; a++) {
        int ok = ioctl(p->fd, EVIOCGABS(axis_codes[a]), &p->axes[a]) == 0;
        /* The kernel gamepad convention uses HAT2Y/HAT2X for lower triggers;
         * xpad and hid-playstation use Z/RZ. Prefer the existing mapping. */
        if (!ok && a >= 4)
            ok = ioctl(p->fd, EVIOCGABS(a == 4 ? ABS_HAT2Y : ABS_HAT2X), &p->axes[a]) == 0;
        if (ok) {
            if (a < 4)
                s->axis[a + 1] = normalize(&p->axes[a], 0);
            else
                s->trigger[a - 3] = normalize(&p->axes[a], 1);
        }
    }
    if (has(keys, BTN_TL2))
        s->trigger[1] = 1;
    if (has(keys, BTN_TR2))
        s->trigger[2] = 1;
}
static void scan(void)
{
    DIR *dir = opendir("/dev/input");
    struct dirent *item;
    if (!dir)
        return;
    while ((item = readdir(dir))) {
        if (strncmp(item->d_name, "event", 5))
            continue;
        char node[64];
        snprintf(node, sizeof(node), "/dev/input/%.40s", item->d_name);
        int free_slot = -1, found = 0;
        for (int i = 0; i < CONTROLS_DEVICES; i++) {
            if (pads[i].fd < 0 && free_slot < 0)
                free_slot = i;
            if (pads[i].fd >= 0 && !strcmp(pads[i].node, node))
                found = 1;
        }
        if (found || free_slot < 0)
            continue;
        int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !has(keys, BTN_GAMEPAD)) {
            close(fd);
            continue;
        }
        Pad *p = &pads[free_slot];
        memset(p, 0, sizeof(*p));
        p->fd = fd;
        snprintf(p->node, sizeof(p->node), "%s", node);
        ControllerDevice *d = ControlsRuntime_Device(free_slot);
        memset(d, 0, sizeof(*d));
        d->connected = 1;
        d->threshold = 0.5f;
        snprintf(d->name, sizeof(d->name), "Controller");
        ioctl(fd, EVIOCGNAME(sizeof(d->name) - 1), d->name);
        struct input_id id = {0};
        ioctl(fd, EVIOCGID, &id);
        d->ambiguous = !ControlsLinux_Identity(node, d->identity, sizeof(d->identity));
        if (d->ambiguous)
            snprintf(d->identity, sizeof(d->identity), "session:evdev:%llu:%d",
                     (unsigned long long)ControlsRuntime_Now(), free_slot);
        d->style = id.vendor == 0x045e   ? CTRL_ICON_XBOX
                   : id.vendor == 0x054c ? CTRL_ICON_PLAYSTATION
                   : id.vendor == 0x057e ? CTRL_ICON_NINTENDO
                                         : CTRL_ICON_GENERIC;
        synchronize(free_slot);
        ControlsRuntime_Gate();
    }
    closedir(dir);
}
void Gamepad_Poll(unsigned frame)
{
    static uint64_t last_scan;
    (void)frame;
    if (!initialized) {
        for (int i = 0; i < CONTROLS_DEVICES; i++)
            pads[i].fd = -1;
        initialized = 1;
    }
    if (!getenv("MEMORIES_NO_GAMEPAD")) {
        uint64_t now = ControlsRuntime_Now();
        if (now - last_scan >= 1000000) {
            scan();
            last_scan = now;
        }
        for (int i = 0; i < CONTROLS_DEVICES; i++)
            if (pads[i].fd >= 0) {
                struct input_event events[64];
                ssize_t bytes;
                int changed = 0;
                while ((bytes = read(pads[i].fd, events, sizeof(events))) > 0) {
                    for (ssize_t j = 0; j < bytes / (ssize_t)sizeof(events[0]); j++) {
                        if (events[j].type == EV_SYN && events[j].code == SYN_DROPPED) {
                            pads[i].dropped = 1;
                            memset(&ControlsRuntime_Device(i)->snapshot, 0, sizeof(ControllerSnapshot));
                        }
                        if (events[j].type == EV_SYN && events[j].code == SYN_REPORT) {
                            pads[i].dropped = 0;
                            changed = 1;
                        }
                    }
                }
                if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EINTR)) {
                    close(pads[i].fd);
                    pads[i].fd = -1;
                    memset(ControlsRuntime_Device(i), 0, sizeof(ControllerDevice));
                    ControlsRuntime_Gate();
                } else if (changed && !pads[i].dropped)
                    synchronize(i);
            }
    }
    ControlsRuntime_Update();
}
uint16_t Gamepad_Bits(int p) { return ControlsRuntime_Pad(p); }
int Gamepad_Connected(int p) { return ControlsRuntime_Connected(p); }
