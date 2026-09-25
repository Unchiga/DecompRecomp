#ifndef MEMORIES_PC_SETTINGS_H
#define MEMORIES_PC_SETTINGS_H

/* The port's stored settings (settings.txt in the user directory, see
 * paths.h; MEMORIES_SETTINGS names another file). Every key has a default, a
 * range and an environment override MEMORIES_<KEY IN UPPER CASE> (legacy
 * names are also honoured). Settings are never part of save states. */
typedef enum {
    SET_MASTER_VOLUME,
    SET_MUSIC_VOLUME,
    SET_SFX_VOLUME,
    SET_STREAM_VOLUME,
    SET_SCALE,
    SET_FULLSCREEN,
    SET_BORDERLESS,
    SET_SCALING,
    SET_ASPECT,
    SET_FILTER,
    SET_VSYNC,
    SET_SPEED,
    SET_FPS,
    SET_SHOW_MENU_FULLSCREEN,
    SET_PAUSE_ON_FOCUS_LOSS,
    SET_MUTE_ON_FOCUS_LOSS,
    SET_HIDE_CURSOR,
    SET_SHOW_HUD,
    SET_MENU_SCALE,
    SET_WINDOW_X,
    SET_WINDOW_Y,
    SET_AUDIO_INTERPOLATION,
    SET_INTERNAL_SCALE,
    SET_RETURN_AFTER_CREDITS,
    SET_DECK_SLOTS,
    SET_BRIGHTNESS,
    SET_CONTRAST,
    SET_SATURATION,
    SET_GAMMA,
    SET_CRT,
    SET_FLASH,
    SET_XBR,
    SET_HD_TEXT,
    SET_FUSION_HELPER,
    SET_HD_HUD,
    SET_OPPONENT_NAME,
    SET_COUNT
} SettingId;

void Settings_Load(void);
/* Returns nonzero after settings were successfully written. */
int Settings_Save(void);
int Settings_Get(SettingId id);
void Settings_Set(SettingId id, int value);
const char *Settings_Key(SettingId id);
int Settings_Min(SettingId id);
int Settings_Max(SettingId id);
void Settings_Observe(void (*changed)(SettingId id, int value));

/* Keys outside the fixed list, kept in the same file and written back
 * unchanged if nothing claims them: whether a mod is applied (`mod.<id>`)
 * and a mod's own settings (`mod.<id>.<key>`). Unlike the fixed settings
 * these have no range, so a mod validates its own values. */
int Settings_GetNamed(const char *key, int fallback);
void Settings_SetNamed(const char *key, int value);

void Settings_VisitNamed(void (*visit)(const char *, int, void *), void *context);

#endif
