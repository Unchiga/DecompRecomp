#define _GNU_SOURCE
#include "pc/platform/controls_linux.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static int standard_triggers;
static int event_code = -1, removed, south, left_trigger, right_trigger, stick, reads;
static int fake_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *out;
    (void)fd;
    va_start(ap, request);
    out = va_arg(ap, void *);
    va_end(ap);
    if (_IOC_NR(request) == _IOC_NR(EVIOCGKEY(0))) {
        memset(out, 0, _IOC_SIZE(request));
        unsigned long *keys = out;
        if (south)
            keys[BTN_A / (8 * sizeof(long))] |= 1ul << (BTN_A % (8 * sizeof(long)));
        return 0;
    }
    if (_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_ABS, 0))) {
        static const unsigned common[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_HAT0X, ABS_HAT0Y};
        unsigned long *bits = out;
        memset(out, 0, _IOC_SIZE(request));
        for (unsigned i = 0; i < sizeof(common) / sizeof(common[0]); i++)
            bits[common[i] / (8 * sizeof(long))] |= 1ul << (common[i] % (8 * sizeof(long)));
        unsigned lt = standard_triggers ? ABS_HAT2Y : ABS_Z, rt = standard_triggers ? ABS_HAT2X : ABS_RZ;
        bits[lt / (8 * sizeof(long))] |= 1ul << (lt % (8 * sizeof(long)));
        bits[rt / (8 * sizeof(long))] |= 1ul << (rt % (8 * sizeof(long)));
        return 0;
    }
    int axis = (int)_IOC_NR(request) - (int)_IOC_NR(EVIOCGABS(0));
    if (axis >= 0 && axis <= ABS_MAX) {
        struct input_absinfo *a = out;
        memset(a, 0, sizeof(*a));
        /* Like the kernel: axes the device lacks read back as zeros. */
        if (standard_triggers ? (axis == ABS_Z || axis == ABS_RZ)
                              : (axis == ABS_HAT2Y || axis == ABS_HAT2X))
            return 0;
        a->minimum = -32768;
        a->maximum = 32767;
        if (axis == ABS_Z || axis == ABS_RZ || axis == ABS_HAT2Y || axis == ABS_HAT2X) {
            a->minimum = 0;
            a->maximum = 255;
            a->value = (axis == ABS_RZ || axis == ABS_HAT2X) ? right_trigger : left_trigger;
        } else if (axis == ABS_HAT0X || axis == ABS_HAT0Y) {
            a->minimum = -1;
            a->maximum = 1;
        } else
            a->value = axis == ABS_X ? stick : 0;
        return 0;
    }
    return -1;
}
static ssize_t fake_read(int fd, void *out, size_t capacity)
{
    (void)fd;
    (void)capacity;
    reads++;
    if (removed)
        return 0;
    if (event_code >= 0) {
        struct input_event e = {0};
        e.type = EV_SYN;
        e.code = (unsigned short)event_code;
        event_code = -1;
        memcpy(out, &e, sizeof(e));
        return sizeof(e);
    }
    errno = EAGAIN;
    return -1;
}
static int fake_close(int fd)
{
    (void)fd;
    return 0;
}
static DIR *fake_opendir(const char *path)
{
    (void)path;
    return NULL;
}
#define ioctl fake_ioctl
#define read fake_read
#define close fake_close
#define opendir fake_opendir
#include "../../src/pc/platform/gamepad_evdev.c"
#undef ioctl
#undef read
#undef close
#undef opendir
int ControlsLinux_Identity(const char *p, char *out, unsigned size)
{
    (void)p;
    (void)out;
    (void)size;
    return 0;
}
int main(void)
{
    setenv("MEMORIES_CONTROLS", "/tmp/memories-evdev-test-missing-controls", 1);
    Gamepad_Poll(1);
    pads[0].fd = 99;
    ControllerDevice *d = ControlsRuntime_Device(0);
    d->connected = 1;
    d->threshold = 0.5f;
    strcpy(d->identity, "fake-evdev");
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(!Gamepad_Bits(0));
    south = 1;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(Gamepad_Bits(0) == CTRL_DEST_CROSS);
    south = 0;
    stick = 20000;
    right_trigger = 255;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(Gamepad_Bits(0) == (CTRL_DEST_RIGHT | CTRL_DEST_R2));
    stick = 14000;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(Gamepad_Bits(0) & CTRL_DEST_RIGHT); /* hysteresis */
    stick = 10000;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(!(Gamepad_Bits(0) & CTRL_DEST_RIGHT));
    event_code = SYN_DROPPED;
    Gamepad_Poll(1);
    assert(!Gamepad_Bits(0));
    right_trigger = 0;
    south = 1;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(Gamepad_Bits(0) == CTRL_DEST_CROSS);
    standard_triggers = 1;
    south = 0;
    right_trigger = 255;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(Gamepad_Bits(0) == CTRL_DEST_R2);
    left_trigger = 255;
    right_trigger = 0;
    event_code = SYN_REPORT;
    Gamepad_Poll(1);
    assert(Gamepad_Bits(0) == CTRL_DEST_L2);
    removed = 1;
    Gamepad_Poll(1);
    assert(!Gamepad_Bits(0));
    assert(!Gamepad_Connected(0));
    assert(pads[0].fd == -1);
    assert(reads > 5);
    puts("controls evdev: normalization, paused polling, SYN_DROPPED resync and EOF removal passed");
    return 0;
}
