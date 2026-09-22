#define _POSIX_C_SOURCE 200809L
#include "pc/platform/controls_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void)
{
    char dir[] = "/tmp/memories-runtime-XXXXXX", path[256], error[256];
    assert(mkdtemp(dir));
    snprintf(path, sizeof(path), "%s/controls.txt", dir);
    setenv("MEMORIES_CONTROLS", path, 1);
    ControlsConfig cfg = *ControlsRuntime_Config();
    for (int i = 0; i < 3; i++) {
        ControllerDevice *d = ControlsRuntime_Device(i);
        d->connected = 1;
        snprintf(d->identity, sizeof(d->identity), "test:%d", i);
        d->threshold = 1.0f / 3;
    }
    ControlsRuntime_Update();
    assert(ControlsRuntime_Assigned(&cfg, 0) == 0);
    assert(ControlsRuntime_Assigned(&cfg, 1) == 1);
    cfg.port[0].mode = 2;
    strcpy(cfg.port[0].identity, "test:2");
    assert(ControlsRuntime_Apply(&cfg, error, sizeof(error)));
    ControlsRuntime_Update();
    assert(ControlsRuntime_Assigned(&cfg, 0) == 2);
    ControllerDevice *d = ControlsRuntime_Device(2);
    d->snapshot.buttons_down = 1u << (CTRL_BTN_SOUTH - 1);
    ControlsRuntime_Update();
    assert(ControlsRuntime_Pad(0) == CTRL_DEST_CROSS);
    assert(!ControlsRuntime_Pad(1));
    ControlsRuntime_Block(1);
    ControlsRuntime_Update();
    assert(!ControlsRuntime_Pad(0));
    ControlsRuntime_Block(0);
    ControlsRuntime_Update();
    assert(!ControlsRuntime_Pad(0));
    d->snapshot.buttons_down = 0;
    ControlsRuntime_Update();
    d->snapshot.buttons_down = 1;
    ControlsRuntime_Update();
    assert(ControlsRuntime_Pad(0) == CTRL_DEST_CROSS);
    d->connected = 0;
    ControlsRuntime_Update();
    assert(!ControlsRuntime_Pad(0));
    assert(!ControlsRuntime_Connected(0));
    assert(ControlsRuntime_Assigned(&cfg, 0) == -1);
    d->connected = 1;
    d->snapshot.buttons_down = 0;
    ControlsRuntime_Update();
    assert(ControlsRuntime_Assigned(&cfg, 0) == 2);
    cfg.port[1] = cfg.port[0];
    assert(!ControlsRuntime_Apply(&cfg, error, sizeof(error)));
    cfg = *ControlsRuntime_Config();
    cfg.port[0].mode = 0;
    assert(ControlsRuntime_Apply(&cfg, error, sizeof(error)));
    ControlsRuntime_Update();
    ControlsRuntime_Key(CTRL_KEY_X, 1);
    ControlsRuntime_Update();
    assert(ControlsRuntime_Keyboard() == CTRL_DEST_CROSS);
    assert(!ControlsRuntime_Pad(0));
    ControlsRuntime_Key(CTRL_KEY_X, 0);
    ControlsRuntime_Update();
    assert(!ControlsRuntime_Keyboard());
    cfg = *ControlsRuntime_Config();
    cfg.port[0].mode = 2;
    strcpy(cfg.port[0].identity, "test:2");
    ControlsProfile *profile = ControlsRuntime_Profile(&cfg, 0, 1);
    assert(cfg.profile_count == 1);
    profile->src[14][0] = (ControlSource){CTRL_SRC_BUTTON, CTRL_BTN_GUIDE, 0};
    assert(ControlsRuntime_Apply(&cfg, error, sizeof(error)));
    ControlsRuntime_Update();
    d->snapshot.buttons_down = 1u << (CTRL_BTN_GUIDE - 1);
    ControlsRuntime_Update();
    assert(ControlsRuntime_Pad(0) == CTRL_DEST_CROSS);
    unlink(path);
    rmdir(dir);
    puts("controls runtime: selection, hotplug, profiles and release gate passed");
    return 0;
}
