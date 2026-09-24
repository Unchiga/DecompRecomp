#define _POSIX_C_SOURCE 200809L
#include "pc/platform/settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>

static int contains(const char *path, const char *wanted)
{
    FILE *file = fopen(path, "r");
    char text[8192];
    size_t length;
    assert(file);
    length = fread(text, 1, sizeof(text) - 1, file);
    text[length] = '\0';
    fclose(file);
    return strstr(text, wanted) != NULL;
}

int main(void)
{
    char path[SCRATCH_MAX];
    int fd;
    FILE *file;
    scratch_template(path, sizeof(path), "memories-settings");
    fd = mkstemp(path);
    assert(fd >= 0);
    file = fdopen(fd, "w");
    assert(file);
    fputs("volume=40\nmusic_volume=70\nunknown=7\n", file);
    assert(!fclose(file));
    assert(!setenv("MEMORIES_SETTINGS", path, 1));
    Settings_Load();
    assert(Settings_Get(SET_MASTER_VOLUME) == 40);
    assert(Settings_Get(SET_MUSIC_VOLUME) == 70);
    assert(Settings_Get(SET_SFX_VOLUME) == 100);
    Settings_Set(SET_ASPECT, 2);
    assert(Settings_Get(SET_ASPECT) == 2);
    Settings_Set(SET_ASPECT, 3);
    assert(Settings_Get(SET_ASPECT) == 2);
    Settings_Set(SET_SPEED, 999);
    assert(Settings_Get(SET_SPEED) == 400);
    assert(Settings_Get(SET_FPS) == 0);
    Settings_Set(SET_FPS, -5);
    assert(Settings_Get(SET_FPS) == -1);
    Settings_Set(SET_FPS, 144);
    assert(Settings_Get(SET_FPS) == 144);
    Settings_Set(SET_SFX_VOLUME, 65);
    assert(Settings_Save());
    assert(contains(path, "master_volume=40\n"));
    assert(contains(path, "volume=40\n"));
    assert(contains(path, "sfx_volume=65\n"));
    assert(contains(path, "unknown=7\n"));
    for (int i = 0; i < 1024; i++) {
        char key[200]; snprintf(key, sizeof(key), "mod.a_very_long_mod_id_that_used_to_exceed_the_old_key_limit.option_%d", i);
        Settings_SetNamed(key, i);
    }
    assert(Settings_Save()); Settings_Load();
    for (int i = 0; i < 1024; i++) {
        char key[200]; snprintf(key, sizeof(key), "mod.a_very_long_mod_id_that_used_to_exceed_the_old_key_limit.option_%d", i);
        assert(Settings_GetNamed(key, -1) == i);
    }
    unlink(path);
    assert(!setenv("MEMORIES_SETTINGS", "/dev/null/settings", 1));
    assert(!Settings_Save());
    return 0;
}
