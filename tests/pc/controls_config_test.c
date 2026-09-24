#define _POSIX_C_SOURCE 200809L
#include "pc/platform/controls_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>
int main(void)
{
    char dir[SCRATCH_MAX], path[SCRATCH_MAX + 64], error[256] = {0};
    scratch_template(dir, sizeof(dir), "memories-controls");
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/controls.txt", dir);
    assert(!setenv("MEMORIES_CONTROLS", path, 1));
    ControlsConfig a, b;
    Controls_InitDefaults(&a);
    assert(ControlsConfig_Load(&b, error, sizeof(error)) == 0);
    assert(Controls_Equal(&a, &b));
    a.kb.src[14][0] = (ControlSource){CTRL_SRC_KEY, CTRL_KEY_B, 0};
    a.ctrl[0].src[15][1] = (ControlSource){CTRL_SRC_AXIS, CTRL_AXIS_RIGHT_Y, -1};
    a.profile_count = 1;
    strcpy(a.profiles[0].identity, "linux:usb with spaces/serial:abc");
    a.profiles[0].bindings = a.ctrl[0];
    a.profiles[0].icon = CTRL_ICON_NINTENDO;
    a.port[0].mode = 2;
    strcpy(a.port[0].identity, a.profiles[0].identity);
    Controls_SetKeyLabel(CTRL_KEY_B,"Localized B");
    assert(ControlsConfig_Save(&a, error, sizeof(error)));
    assert(ControlsConfig_Load(&b, error, sizeof(error)) == 1);
    assert(Controls_Equal(&a, &b));
    for (int code = CTRL_BTN_TOUCHPAD; code < CTRL_BTN_COUNT; code++) {
        a.ctrl[1].src[14][0] = (ControlSource){CTRL_SRC_BUTTON, (uint16_t)code, 0};
        assert(ControlsConfig_Save(&a, error, sizeof(error)));
        assert(ControlsConfig_Load(&b, error, sizeof(error)) == 1);
        assert(Controls_Equal(&a, &b));
    }
    FILE *f = fopen(path, "a");
    assert(f);
    fputs("bind 0 0 key.x\n", f);
    fclose(f);
    assert(ControlsConfig_Load(&b, error, sizeof(error)) == -1);
    ControlsConfig defaults;
    Controls_InitDefaults(&defaults);
    assert(Controls_Equal(&b, &defaults));
    f = fopen(path, "w");
    assert(f);
    fputs("controls 99 0\n", f);
    fclose(f);
    assert(ControlsConfig_Load(&b, error, sizeof(error)) == -2);
    assert(!ControlsConfig_Save(&a, error, sizeof(error)));
    f = fopen(path, "r");
    assert(f);
    char line[80];
    assert(fgets(line, sizeof(line), f));
    fclose(f);
    assert(!strcmp(line, "controls 99 0\n"));
    assert(!unlink(path));
    assert(!mkdir(path, 0700)); /* replacement must fail; directory survives */
    assert(!ControlsConfig_Save(&a, error, sizeof(error)));
    assert(!rmdir(path));
    assert(!rmdir(dir));
    puts("controls config: round trip, corruption, future version and write failure passed");
    return 0;
}
