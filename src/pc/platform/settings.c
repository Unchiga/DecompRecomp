#include "settings.h"
#include "paths.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "pc/compat/posix.h"

#define MAX_UNKNOWN 64
#define MAX_KEY 256
#define MAX_LINE 1024
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
    /* 0 is corrected 4:3, 1 uses source pixels, and 2 widens the view to 16:9. */
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
    /* 0 the console's Gaussian filter, 1 a sharper cubic (SpuInterpolation). */
    [SET_AUDIO_INTERPOLATION] = {"audio_interpolation", NULL, "MEMORIES_AUDIO_INTERPOLATION", NULL, 0, 0, 1},
    /* Pixels drawn per VRAM word each way (soft_gpu.h, SoftGpu_SetScale): 1 is the console's. */
    [SET_INTERNAL_SCALE] = {"internal_scale", NULL, "MEMORIES_INTERNAL_SCALE", NULL, 1, 1, 8},
    /* 1: the title three seconds after the credits end (credits.c); 0: a black screen, as the console. */
    [SET_RETURN_AFTER_CREDITS] = {"return_after_credits", NULL, "MEMORIES_RETURN_AFTER_CREDITS", NULL, 1, 0, 1},
    /* 1: Game > Deck slots and F6 keep and switch decks (src/pc/saves/deck_menu.c). */
    [SET_DECK_SLOTS] = {"deck_slots", NULL, "MEMORIES_DECK_SLOTS", NULL, 1, 0, 1},
};

/* A key the fixed list does not know: a mod's, or one this build dropped.
 * Whole numbers are kept as values so a mod can read and write them; the
 * rest of a settings file is carried through as the lines it arrived as. */
typedef struct { char key[MAX_KEY]; int value; } Named;

static int values[SET_COUNT];
static int stored[SET_COUNT];
static char unknown[MAX_UNKNOWN][MAX_LINE];
static int unknown_count;
static Named *named;
static int named_count, named_capacity, named_error;
static void (*observers[MAX_OBSERVERS])(SettingId, int);
static int observer_count;

static const char *settings_path(void)
{
    static char path[1024];
    const char *named_path = getenv("MEMORIES_SETTINGS");
    if (named_path && *named_path) return named_path;
    if (!path[0] && Paths_User(path, sizeof(path), "settings.txt")) return "settings.txt";
    return path;
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
    if (errno || end == text || *end || parsed < INT_MIN || parsed > INT_MAX) return 0;
    *value = (int)parsed;
    return 1;
}

/* The named entry for a key, made if it is missing and there is room. */
static Named *find_named(const char *key, int make)
{
    int i;
    for (i = 0; i < named_count; i++) {
        if (!strcmp(named[i].key, key)) return &named[i];
    }
    if (!make) return NULL;
    if (strlen(key) >= MAX_KEY) { named_error = 1; return NULL; }
    /* A name must read back as itself: one line, split at the first '='. */
    if (!*key || strpbrk(key, "=\r\n") || isspace((unsigned char)key[0]) || isspace((unsigned char)key[strlen(key) - 1]))
        return NULL;
    if (named_count == named_capacity) {
        int capacity = named_capacity ? named_capacity * 2 : 128;
        Named *grown = realloc(named, (size_t)capacity * sizeof(*named));
        if (!grown) { named_error = 1; return NULL; }
        named = grown;
        named_capacity = capacity;
    }
    snprintf(named[named_count].key, MAX_KEY, "%s", key);
    named[named_count].value = 0;
    return &named[named_count++];
}

static void set_named(const char *key, int value)
{
    Named *entry = find_named(key, 1);
    if (entry) entry->value = value;
}

void Settings_Load(void)
{
    FILE *file;
    char line[MAX_LINE];
    int id;
    unknown_count = named_count = named_error = 0;
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
                if (id < 0 && *key && parse_value(equals, &value) && strlen(key) < MAX_KEY) {
                    set_named(key, value);
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

    if (named_error) return 0; /* Never report success after dropping a setting. */
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
    for (i = 0; i < named_count; i++) fprintf(file, "%s=%d\n", named[i].key, named[i].value);
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

int Settings_GetNamed(const char *key, int fallback)
{
    Named *entry = key ? find_named(key, 0) : NULL;
    return entry ? entry->value : fallback;
}

void Settings_SetNamed(const char *key, int value)
{
    if (key && *key) set_named(key, value);
}

void Settings_Observe(void (*changed)(SettingId id, int value))
{
    int i;
    if (!changed) return;
    for (i = 0; i < observer_count; i++) if (observers[i] == changed) return;
    if (observer_count < MAX_OBSERVERS) observers[observer_count++] = changed;
}

void Settings_VisitNamed(void (*visit)(const char *, int, void *), void *context)
{
    int i;
    for (i = 0; i < named_count; i++) visit(named[i].key, named[i].value, context);
}
