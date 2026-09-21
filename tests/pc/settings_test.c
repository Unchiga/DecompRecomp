#define _POSIX_C_SOURCE 200809L
#include "pc/platform/settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    char path[] = "/tmp/memories-settings-XXXXXX";
    int fd = mkstemp(path);
    FILE *file;
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
    Settings_Set(SET_SFX_VOLUME, 65);
    Settings_Save();
    assert(contains(path, "master_volume=40\n"));
    assert(contains(path, "volume=40\n"));
    assert(contains(path, "sfx_volume=65\n"));
    assert(contains(path, "unknown=7\n"));
    unlink(path);
    return 0;
}
