#ifndef MEMORIES_PC_SETTINGS_H
#define MEMORIES_PC_SETTINGS_H

/* The port's stored settings (saves/settings.txt; MEMORIES_SETTINGS names
 * another file). Every key has a default, a range and an environment
 * override MEMORIES_<KEY IN UPPER CASE> (legacy names are also honoured).
 * Settings are never part of save states. */
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
    SET_MOD_3D_MONSTERS,
    SET_MOD_HAND_CAMERA,
    SET_AUDIO_INTERPOLATION,
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

#endif
