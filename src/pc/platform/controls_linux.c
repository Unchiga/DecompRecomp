#include "controls_linux.h"
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
int ControlsLinux_Identity(const char *path, char *out, unsigned size)
{
    struct input_id id;
    char serial[128] = {0}, phys[128] = {0};
    int fd = path ? open(path, O_RDONLY | O_NONBLOCK) : -1;
    if (fd < 0)
        return 0;
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        close(fd);
        return 0;
    }
    ioctl(fd, EVIOCGUNIQ(sizeof(serial) - 1), serial);
    ioctl(fd, EVIOCGPHYS(sizeof(phys) - 1), phys);
    close(fd);
    if (!serial[0] && !phys[0])
        return 0;
    int n = snprintf(out, size, "linux:%04x:%04x:%04x:%s:%s", id.bustype, id.vendor, id.product,
                     serial[0] ? "serial" : "path", serial[0] ? serial : phys);
    return n >= 0 && (unsigned)n < size;
}
