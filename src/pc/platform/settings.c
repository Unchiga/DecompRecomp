#include "settings.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"

#define MAX_UNKNOWN 64
#define MAX_LINE 256
#define MAX_OBSERVERS 8

typedef struct {
    const char *key;
    const char *legacy_key;
    const char *env;
    const char *legacy_env;
    int def, min, max;
} SettingInfo;

static const SettingInfo info[SET_COUNT] = {
    [SET_MASTER_VOLUME] = {"master_volume", "volume", "MEMORIES_MASTER_VOLUME", "MEMORIES_VOLUME", 100, 0, 100},
    [SET_MUSIC_VOLUME] = {"music_volume", NULL, "MEMORIES_MUSIC_VOLUME", NULL, 100, 0, 100},
    [SET_SFX_VOLUME] = {"sfx_volume", NULL, "MEMORIES_SFX_VOLUME", NULL, 100, 0, 100},
    [SET_STREAM_VOLUME] = {"stream_volume", NULL, "MEMORIES_STREAM_VOLUME", NULL, 100, 0, 100},
    [SET_SCALE] = {"scale", "scale", "MEMORIES_SCALE", "MEMORIES_SCALE", 4, 1, 8},
    [SET_FULLSCREEN] = {"fullscreen", NULL, "MEMORIES_FULLSCREEN", NULL, 0, 0, 2},
    [SET_BORDERLESS] = {"borderless", NULL, "MEMORIES_BORDERLESS", NULL, 0, 0, 1},
    [SET_SCALING] = {"scaling", NULL, "MEMORIES_SCALING", NULL, 0, 0, 2},
    /* 0 is corrected 4:3, 1 uses source pixels, and 2 opens a 16:9 canvas. */
    [SET_ASPECT] = {"aspect", NULL, "MEMORIES_ASPECT", NULL, 0, 0, 2},
    [SET_FILTER] = {"filter", NULL, "MEMORIES_FILTER", NULL, 0, 0, 1},
    [SET_VSYNC] = {"vsync", NULL, "MEMORIES_VSYNC", NULL, 0, 0, 1},
    [SET_SPEED] = {"speed", NULL, "MEMORIES_SPEED", NULL, 100, -1, 400},
    /* Presented frames per second: 0 follows the display's refresh rate, -1 shows every game frame. */
    [SET_FPS] = {"fps", NULL, "MEMORIES_FPS", NULL, 0, -1, 1000},
    [SET_SHOW_MENU_FULLSCREEN] = {"show_menu_fullscreen", NULL, "MEMORIES_SHOW_MENU_FULLSCREEN", NULL, 0, 0, 1},
    [SET_PAUSE_ON_FOCUS_LOSS] = {"pause_on_focus_loss", NULL, "MEMORIES_PAUSE_ON_FOCUS_LOSS", NULL, 0, 0, 1},
    [SET_MUTE_ON_FOCUS_LOSS] = {"mute_on_focus_loss", NULL, "MEMORIES_MUTE_ON_FOCUS_LOSS", NULL, 0, 0, 1},
    [SET_HIDE_CURSOR] = {"hide_cursor", NULL, "MEMORIES_HIDE_CURSOR", NULL, 1, 0, 1},
    [SET_SHOW_HUD] = {"show_hud", NULL, "MEMORIES_SHOW_HUD", NULL, 0, 0, 2},
    /* Menu and HUD size: 0 follows the window's height, else a multiple. */
    [SET_MENU_SCALE] = {"menu_scale", NULL, "MEMORIES_MENU_SCALE", NULL, 0, 0, 4},
    [SET_WINDOW_X] = {"window_x", NULL, "MEMORIES_WINDOW_X", NULL, -1, -16384, 16384},
    [SET_WINDOW_Y] = {"window_y", NULL, "MEMORIES_WINDOW_Y", NULL, -1, -16384, 16384},
    [SET_MOD_3D_MONSTERS] = {"3d_monsters", "3d_monsters", "MEMORIES_3D_MONSTERS", "MEMORIES_MODS_MONSTERS", 0, 0, 1},
    [SET_MOD_HAND_CAMERA] = {"hand_camera", "hand_camera", "MEMORIES_HAND_CAMERA", NULL, 1, 0, 1},
};

static int values[SET_COUNT];
static int stored[SET_COUNT];
static char unknown[MAX_UNKNOWN][MAX_LINE];
static int unknown_count;
static void (*observers[MAX_OBSERVERS])(SettingId, int);
static int observer_count;

static const char *settings_path(void)
{
    const char *path = getenv("MEMORIES_SETTINGS");
    return path && *path ? path : "saves/settings.txt";
}

static int clamp(SettingId id, int value)
{
    if (id == SET_SPEED && value != -1 && value < 25) {
        return 25;
    }
    if (value < info[id].min) return info[id].min;
    if (value > info[id].max) return info[id].max;
    return value;
}

static int find_key(const char *key)
{
    int id;
    for (id = 0; id < SET_COUNT; id++) {
        if (!strcmp(key, info[id].key) ||
            (info[id].legacy_key && !strcmp(key, info[id].legacy_key))) {
            return id;
        }
    }
    return -1;
}

static int parse_value(const char *text, int *value)
{
    char *end;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (errno || end == text || *end) return 0;
    *value = (int)parsed;
    return 1;
}

void Settings_Load(void)
{
    FILE *file;
    char line[MAX_LINE];
    int id;
    unknown_count = 0;
    for (id = 0; id < SET_COUNT; id++) values[id] = stored[id] = info[id].def;
    file = fopen(settings_path(), "r");
    if (file) {
        while (fgets(line, sizeof(line), file)) {
            char copy[MAX_LINE], *equals, *key, *end;
            int value;
            memcpy(copy, line, sizeof(copy));
            copy[sizeof(copy) - 1] = '\0';
            equals = strchr(copy, '=');
            if (equals) {
                *equals++ = '\0';
                key = copy;
                while (isspace((unsigned char)*key)) key++;
                end = key + strlen(key);
                while (end > key && isspace((unsigned char)end[-1])) *--end = '\0';
                id = find_key(key);
                if (id >= 0 && parse_value(equals, &value)) {
                    values[id] = stored[id] = clamp((SettingId)id, value);
                    continue;
                }
            }
            if (unknown_count < MAX_UNKNOWN) {
                size_t length = strlen(line);
                if (length && line[length - 1] == '\n') line[length - 1] = '\0';
                snprintf(unknown[unknown_count++], MAX_LINE, "%s", line);
            }
        }
        fclose(file);
    }
    for (id = 0; id < SET_COUNT; id++) {
        const char *text = getenv(info[id].env);
        int value;
        if ((!text || !*text) && info[id].legacy_env) text = getenv(info[id].legacy_env);
        if (text && *text && parse_value(text, &value)) values[id] = clamp((SettingId)id, value);
    }
}

int Settings_Save(void)
{
    const char *path = settings_path();
    char temporary[1024];
    FILE *file;
    int id, i;
    if (!getenv("MEMORIES_SETTINGS")) mkdir("saves", 0777);
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary)) return 0;
    file = fopen(temporary, "w");
    if (!file) return 0;
    for (id = 0; id < SET_COUNT; id++) {
        fprintf(file, "%s=%d\n", info[id].key, stored[id]);
        /* TODO remove legacy keys after one compatibility release. */
        if (info[id].legacy_key && strcmp(info[id].legacy_key, info[id].key)) {
            fprintf(file, "%s=%d\n", info[id].legacy_key, stored[id]);
        }
    }
    for (i = 0; i < unknown_count; i++) fprintf(file, "%s\n", unknown[i]);
    {
        int failed = ferror(file);
        if (fclose(file)) failed = 1;
        if (failed || rename(temporary, path)) { remove(temporary); return 0; }
    }
    return 1;
}

int Settings_Get(SettingId id)
{
    return id >= 0 && id < SET_COUNT ? values[id] : 0;
}

void Settings_Set(SettingId id, int value)
{
    int i;
    if (id < 0 || id >= SET_COUNT) return;
    value = clamp(id, value);
    stored[id] = value;
    if (values[id] == value) return;
    values[id] = value;
    for (i = 0; i < observer_count; i++) observers[i](id, value);
}

const char *Settings_Key(SettingId id) { return id >= 0 && id < SET_COUNT ? info[id].key : NULL; }
int Settings_Min(SettingId id) { return id >= 0 && id < SET_COUNT ? info[id].min : 0; }
int Settings_Max(SettingId id) { return id >= 0 && id < SET_COUNT ? info[id].max : 0; }

void Settings_Observe(void (*changed)(SettingId id, int value))
{
    int i;
    if (!changed) return;
    for (i = 0; i < observer_count; i++) if (observers[i] == changed) return;
    if (observer_count < MAX_OBSERVERS) observers[observer_count++] = changed;
}
