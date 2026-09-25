/* The window's menu bar, composited in software (see menu.h).
 *
 * Nothing here touches the X server: Menu_Draw paints into the frame the
 * platform is about to show, and Menu_Bounds tells the platform which part
 * of that frame the menu covers so that a hover or a slider drag repaints
 * and uploads only that rectangle, not a whole 1280x960 frame. The bar is
 * repainted with every game frame, which is about a thousand pixels of
 * text over a flat fill and costs nothing measurable.
 *
 * Text is anti-aliased through FreeType, in whatever face fontconfig names
 * for "sans-serif" at 13 px, cached as coverage bitmaps once at start. A
 * machine without either falls back to the 5x7 bitmap font at the end of
 * the file, drawn at twice its size. */
#include "menu.h"
#include "platform.h"
#include "settings.h"
#include "pc/audio/spu.h"
#include "pc/audio/replace.h"
#include "pc/debug/cheats.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include "pc/mods/mods.h"
#include "pc/render/texture_pack.h"
#include "paths.h"
#include "pc/sdk/display.h"
#include "title_jump.h"
#include "pc/saves/deck_menu.h"
#ifdef _WIN32
#include "win32.h"
#else
#include <fontconfig/fontconfig.h>
#endif
#include "pc/debug/crash.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include "pc/compat/font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Geometry, in multiples of `ui` (Menu_SetScale): 1 suits a 720p window,
 * a 4K display wants 3. */
static int ui = 1;
#define MENU_H (26 * ui)
#define ITEM_H (26 * ui)
#define BAR_PAD (12 * ui)    /* left and right of a bar label */
#define ITEM_PAD (12 * ui)   /* dropdown edge to the mark column */
#define MARK_W (22 * ui)     /* the check or radio column */
#define SHORTCUT_GAP (28 * ui)
#define DROP_PAD (4 * ui)    /* dropdown border to the first item */
#define SEP_H (9 * ui)
#define SLIDER_W (160 * ui)
#define SLIDER_H (4 * ui)
#define KNOB_R (6 * ui)
#define SHADOW (6 * ui)
#define FONT_PX (13 * ui)

/* Colours: a dark bar that stays out of the picture's way. */
#define C_BAR 0x1e1f22u
#define C_BAR_EDGE 0x0b0b0dU
#define C_BAR_HOVER 0x33353aU
#define C_ACCENT 0x3b82f6u
#define C_TEXT 0xe8e8ea
#define C_TEXT_DIM 0x8b8d93
#define C_TEXT_ON_ACCENT 0xffffffu
#define C_DROP 0x27282cU
#define C_DROP_EDGE 0x3f4147u
#define C_SEP 0x3f4147u
#define C_TRACK 0x4c4e55u
#define C_KNOB 0xf2f2f4u
#define C_KNOB_EDGE 0x1e1f22u
#define C_MARK 0x9a9ca3u

/* ITEM_SUBMENU opens submenus[value] beside its row; one level deep. */
typedef enum { ITEM_ACTION, ITEM_CHECK, ITEM_RADIO, ITEM_SLIDER, ITEM_SEPARATOR, ITEM_SUBMENU } ItemKind;
enum { ITEM_DISABLED = 1, ITEM_GROUP_BREAK = 2 };

enum {
    ACT_SAVE_STATE = 1, ACT_LOAD_STATE, ACT_SCREENSHOT, ACT_EXIT, ACT_GIVE_CARDS,
    ACT_MODS, ACT_CONTROLS, ACT_RELOAD_SETTINGS, ACT_PAUSE, ACT_FRAME_STEP, ACT_DUMP_FRAME, ACT_DUMP_VRAM,
    SLIDER_MASTER, SLIDER_MUSIC, SLIDER_SFX, CHECK_MUTE,
    CHECK_HUD, CHECK_HUD_FULL, RADIO_STATE_SLOT, ACT_UNLOCK_FREE_DUELISTS, ACT_RESET_COLOR,
    CHECK_TRACE = 300  /* value is a LogChannel */
};

typedef struct {
    const char *label;
    const char *shortcut;
    ItemKind kind;
    int id;
    SettingId setting;
    int value;
    int flags;
} Item;
typedef struct { const char *label; Item items[16]; int count; int x, w; } Menu;

enum { MENU_FILE, MENU_VIDEO, MENU_AUDIO, MENU_GAME, MENU_VIEW, MENU_DEBUG, MENU_COUNT };
enum { SUB_SCALE, SUB_MENU_SIZE, SUB_SPEED, SUB_FPS, SUB_CHEATS, SUB_TRACE, SUB_SCALING, SUB_ASPECT, SUB_RESOLUTION, SUB_COLOR, SUB_EFFECTS, SUB_JUMP, SUB_ANTIALIAS, SUB_FILTER, SUB_COUNT };
static Menu menus[MENU_COUNT] = {
    {"File", {{"Save state", "F5", ITEM_ACTION, ACT_SAVE_STATE, -1},
              {"Load state", "F7", ITEM_ACTION, ACT_LOAD_STATE, -1},
              {"State slot 1", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 1, ITEM_GROUP_BREAK},
              {"State slot 2", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 2},
              {"State slot 3", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 3},
              {"State slot 4", 0, ITEM_RADIO, RADIO_STATE_SLOT, -1, 4},
              {"Screenshot", "F12", ITEM_ACTION, ACT_SCREENSHOT, -1, 0, ITEM_GROUP_BREAK},
              {"Reload settings", 0, ITEM_ACTION, ACT_RELOAD_SETTINGS, -1},
              {"Exit", "Esc", ITEM_ACTION, ACT_EXIT, -1, 0, ITEM_GROUP_BREAK}}, 9},
    {"Video", {{"Window scale", 0, ITEM_SUBMENU, 0, -1, SUB_SCALE},
              {"Menu size", 0, ITEM_SUBMENU, 0, -1, SUB_MENU_SIZE},
              {"Fullscreen", "F11", ITEM_CHECK, MENU_ITEM_FULLSCREEN, SET_FULLSCREEN, 0, ITEM_GROUP_BREAK},
              {"Borderless fullscreen", 0, ITEM_CHECK, MENU_ITEM_BORDERLESS, SET_BORDERLESS},
              {"Scaling", 0, ITEM_SUBMENU, 0, -1, SUB_SCALING, ITEM_GROUP_BREAK},
              {"Aspect Ratio", 0, ITEM_SUBMENU, 0, -1, SUB_ASPECT},
              {"Resolution", 0, ITEM_SUBMENU, 0, -1, SUB_RESOLUTION},
              {"HD text", 0, ITEM_CHECK, 0, SET_HD_TEXT},
              {"Anti-aliasing", 0, ITEM_SUBMENU, 0, -1, SUB_ANTIALIAS},
              {"Filtering", 0, ITEM_SUBMENU, MENU_ITEM_FILTER, -1, SUB_FILTER, ITEM_GROUP_BREAK},
              {"VSync", 0, ITEM_CHECK, MENU_ITEM_VSYNC, SET_VSYNC},
              {"Color", 0, ITEM_SUBMENU, 0, -1, SUB_COLOR, ITEM_GROUP_BREAK},
              {"Effects", 0, ITEM_SUBMENU, 0, -1, SUB_EFFECTS}}, 13},
    {"Audio", {{"Master", 0, ITEM_SLIDER, SLIDER_MASTER, SET_MASTER_VOLUME},
               {"Music", 0, ITEM_SLIDER, SLIDER_MUSIC, SET_MUSIC_VOLUME},
               {"Sound FX", 0, ITEM_SLIDER, SLIDER_SFX, SET_SFX_VOLUME},
               {0, 0, ITEM_SEPARATOR, 0, -1},
               {"Mute all", "M", ITEM_CHECK, CHECK_MUTE, -1},
               {"Mute on focus loss", 0, ITEM_CHECK, 0, SET_MUTE_ON_FOCUS_LOSS},
               {"Console sound (Gaussian)", 0, ITEM_RADIO, 0, SET_AUDIO_INTERPOLATION, 0, ITEM_GROUP_BREAK},
               {"Sharper sound (cubic)", 0, ITEM_RADIO, 0, SET_AUDIO_INTERPOLATION, 1}}, 8},
    /* Game speed scales the game clock (music keeps its tempo); the frame
     * rate is how many of those game frames reach the window. Tab holds 400%. */
    {"Game", {{"Controls...", 0, ITEM_ACTION, ACT_CONTROLS, -1},
              {"Mods", 0, ITEM_ACTION, ACT_MODS, -1},
              {"Game speed", 0, ITEM_SUBMENU, 0, -1, SUB_SPEED},
              {"Frame rate", 0, ITEM_SUBMENU, 0, -1, SUB_FPS},
              {"Title screen after the credits", 0, ITEM_CHECK, 0, SET_RETURN_AFTER_CREDITS, 0, ITEM_GROUP_BREAK},
              {"Deck slots...", "F6", ITEM_ACTION, MENU_ITEM_DECKS, -1, 0, ITEM_GROUP_BREAK | ITEM_DISABLED},
              {"Use deck slots", 0, ITEM_CHECK, 0, SET_DECK_SLOTS},
              {"Cheats", 0, ITEM_SUBMENU, 0, -1, SUB_CHEATS, ITEM_GROUP_BREAK}}, 8},
    {"View", {{"Fusion helper", 0, ITEM_CHECK, 0, SET_FUSION_HELPER}}, 1},
    {"Debug", {{"Jump to", 0, ITEM_SUBMENU, 0, -1, SUB_JUMP},
               {"Show HUD", "F3", ITEM_CHECK, CHECK_HUD, -1, 0, ITEM_GROUP_BREAK},
               {"Full stats", 0, ITEM_CHECK, CHECK_HUD_FULL, -1},
               {"Pause", "P", ITEM_CHECK, ACT_PAUSE, -1, 0, ITEM_GROUP_BREAK},
               {"Frame step", ".", ITEM_ACTION, ACT_FRAME_STEP, -1},
               {"Dump frame (PPM)", 0, ITEM_ACTION, ACT_DUMP_FRAME, -1, 0, ITEM_GROUP_BREAK},
               {"Dump VRAM (PPM)", 0, ITEM_ACTION, ACT_DUMP_VRAM, -1},
               {"Trace", 0, ITEM_SUBMENU, 0, -1, SUB_TRACE, ITEM_GROUP_BREAK}}, 8},
};
static Menu submenus[SUB_COUNT] = {
    {"Window scale", {{"1x", 0, ITEM_RADIO, MENU_ITEM_SCALE_1, SET_SCALE, 1},
                      {"2x", 0, ITEM_RADIO, MENU_ITEM_SCALE_2, SET_SCALE, 2},
                      {"3x", 0, ITEM_RADIO, MENU_ITEM_SCALE_3, SET_SCALE, 3},
                      {"4x", 0, ITEM_RADIO, MENU_ITEM_SCALE_4, SET_SCALE, 4},
                      {"5x", 0, ITEM_RADIO, MENU_ITEM_SCALE_5, SET_SCALE, 5},
                      {"6x", 0, ITEM_RADIO, MENU_ITEM_SCALE_6, SET_SCALE, 6}}, 6},
    {"Menu size", {{"Automatic", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 0},
                   {"1x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 1, ITEM_GROUP_BREAK},
                   {"2x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 2},
                   {"3x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 3},
                   {"4x", 0, ITEM_RADIO, 0, SET_MENU_SCALE, 4}}, 5},
    {"Game speed", {{"50%", 0, ITEM_RADIO, 0, SET_SPEED, 50},
                    {"100%", 0, ITEM_RADIO, 0, SET_SPEED, 100},
                    {"150%", 0, ITEM_RADIO, 0, SET_SPEED, 150},
                    {"200%", 0, ITEM_RADIO, 0, SET_SPEED, 200},
                    {"300%", 0, ITEM_RADIO, 0, SET_SPEED, 300},
                    {"400%", "Tab", ITEM_RADIO, 0, SET_SPEED, 400},
                    {"Uncapped", 0, ITEM_RADIO, 0, SET_SPEED, -1}}, 7},
    {"Frame rate", {{"Display refresh", 0, ITEM_RADIO, 0, SET_FPS, 0},
                    {"30", 0, ITEM_RADIO, 0, SET_FPS, 30},
                    {"60", 0, ITEM_RADIO, 0, SET_FPS, 60},
                    {"120", 0, ITEM_RADIO, 0, SET_FPS, 120},
                    {"144", 0, ITEM_RADIO, 0, SET_FPS, 144},
                    {"240", 0, ITEM_RADIO, 0, SET_FPS, 240},
                    {"Every game frame", 0, ITEM_RADIO, 0, SET_FPS, -1}}, 7},
    {"Cheats", {{"Give 3 of every card", 0, ITEM_ACTION, ACT_GIVE_CARDS, -1},
                {"Unlock all Free Duel CPU duelists", 0, ITEM_ACTION, ACT_UNLOCK_FREE_DUELISTS, -1}}, 2},
    {"Trace", {{"Frames", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_FRAMES},
               {"Disc", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_DISC},
               {"SPU", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_SPU},
               {"Input", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_INPUT},
               {"State", 0, ITEM_CHECK, CHECK_TRACE, -1, LOG_STATE}}, 5},
    {"Scaling", {{"Integer scaling", 0, ITEM_RADIO, MENU_ITEM_SCALING_INTEGER, SET_SCALING, 0},
                  {"Fit to window", 0, ITEM_RADIO, MENU_ITEM_SCALING_FIT, SET_SCALING, 1},
                  {"Stretch", 0, ITEM_RADIO, MENU_ITEM_SCALING_STRETCH, SET_SCALING, 2}}, 3},
    {"Aspect Ratio", {{"4:3 aspect", 0, ITEM_RADIO, MENU_ITEM_ASPECT_4_3, SET_ASPECT, 0},
                       {"Square pixels", 0, ITEM_RADIO, MENU_ITEM_ASPECT_SQUARE, SET_ASPECT, 1},
                       {"Widescreen (16:9)", 0, ITEM_RADIO, MENU_ITEM_ASPECT_WIDESCREEN, SET_ASPECT, 2}}, 3},
    {"Resolution", {{"Console resolution", 0, ITEM_RADIO, 0, SET_INTERNAL_SCALE, 1},
                     {"Internal 2x", 0, ITEM_RADIO, 0, SET_INTERNAL_SCALE, 2},
                     {"Internal 4x", 0, ITEM_RADIO, 0, SET_INTERNAL_SCALE, 4}}, 3},
    /* The present pass (src/pc/render/present_pass.c); 100 is the picture as is. */
    {"Color", {{"Brightness", 0, ITEM_SLIDER, 0, SET_BRIGHTNESS},
               {"Contrast", 0, ITEM_SLIDER, 0, SET_CONTRAST},
               {"Saturation", 0, ITEM_SLIDER, 0, SET_SATURATION},
               {"Gamma", 0, ITEM_SLIDER, 0, SET_GAMMA},
               {"Reset", 0, ITEM_ACTION, ACT_RESET_COLOR, -1, 0, ITEM_GROUP_BREAK}}, 5},
    {"Effects", {{"CRT scanlines", 0, ITEM_CHECK, 0, SET_CRT},
                 {"Reduce flashes", 0, ITEM_CHECK, 0, SET_FLASH},
                 {"xBR pixel smoothing", 0, ITEM_CHECK, 0, SET_XBR}}, 3},
    {"Jump to", {{"Title Screen", 0, ITEM_ACTION, MENU_ITEM_TITLE, -1, 0, ITEM_DISABLED}}, 1},
    {"Anti-aliasing", {{"Off", 0, ITEM_RADIO, 0, SET_MSAA, 0},
                       {"2x", 0, ITEM_RADIO, 0, SET_MSAA, 2},
                       {"4x", 0, ITEM_RADIO, 0, SET_MSAA, 4},
                       {"8x", 0, ITEM_RADIO, 0, SET_MSAA, 8}}, 4},
    {"Filtering", {{"Nearest", 0, ITEM_RADIO, MENU_ITEM_FILTER_NEAREST, SET_FILTER, 0},
                   {"Smooth (bilinear)", 0, ITEM_RADIO, MENU_ITEM_FILTER_LINEAR, SET_FILTER, 1},
                   {"Sharp bilinear", 0, ITEM_RADIO, MENU_ITEM_FILTER_SHARP, SET_FILTER, 2}}, 3},
};

static int open_menu = -1, hot_item = -1, hover_bar = -1, grabbed, ready, visible = 1;
/* The open submenu (index into submenus), its row in the bar menu, and its hot row. */
static int open_sub = -1, sub_item = -1, hot_sub = -1;
static int consumed_press[8];

/* --- text ------------------------------------------------------------ */

typedef struct { unsigned char *coverage; int w, h, left, top, advance; } Glyph;
static Glyph glyphs[96];
static int font_ascent, font_descent, font_loaded;

typedef struct { char code; unsigned char rows[7]; } BitmapGlyph;
/* --- the fallback font ----------------------------------------------- */

/* 5x7 glyphs, one bit per pixel, the top row first. */
static const BitmapGlyph bitmap_font[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
    {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
    {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}, {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}, {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}}, {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04}}, {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}},
    {'a', {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}}, {'b', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1e}},
    {'c', {0x00, 0x00, 0x0e, 0x10, 0x10, 0x11, 0x0e}}, {'d', {0x01, 0x01, 0x0d, 0x13, 0x11, 0x11, 0x0f}},
    {'e', {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e}}, {'f', {0x06, 0x09, 0x08, 0x1c, 0x08, 0x08, 0x08}},
    {'g', {0x00, 0x0f, 0x11, 0x11, 0x0f, 0x01, 0x0e}}, {'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'i', {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e}}, {'j', {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0c}},
    {'k', {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}}, {'l', {0x0c, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'m', {0x00, 0x00, 0x1a, 0x15, 0x15, 0x15, 0x15}}, {'n', {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11}},
    {'o', {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e}}, {'p', {0x00, 0x00, 0x1e, 0x11, 0x1e, 0x10, 0x10}},
    {'q', {0x00, 0x00, 0x0d, 0x13, 0x0f, 0x01, 0x01}}, {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e}}, {'t', {0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06}},
    {'u', {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d}}, {'v', {0x00, 0x00, 0x11, 0x11, 0x11, 0x0a, 0x04}},
    {'w', {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a}}, {'x', {0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11}},
    {'y', {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e}}, {'z', {0x00, 0x00, 0x1f, 0x02, 0x04, 0x08, 0x1f}},
    {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}}, {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}},
    {':', {0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
};
static const int bitmap_font_count = (int)(sizeof(bitmap_font) / sizeof(bitmap_font[0]));

static void load_font(void)
{
    FT_Library library;
    FT_Face face;
#ifdef _WIN32
    const char *file = Win32_FontPath(0);
#else
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
#endif
    int c;
    for (c = 0; c < 96; c++) {
        free(glyphs[c].coverage);
        memset(&glyphs[c], 0, sizeof(glyphs[c]));
    }
    font_loaded = 0;
#ifdef _WIN32
    if (!file || FT_Init_FreeType(&library)) {
        return;
    }
    if (FT_New_Face(library, file, 0, &face) || FT_Set_Pixel_Sizes(face, 0, FONT_PX)) {
        return;
    }
#else
    if (!FcInit() || FT_Init_FreeType(&library)) {
        return;
    }
    pattern = FcNameParse((const FcChar8 *)"sans-serif");
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    match = FcFontMatch(NULL, pattern, &result);
    if (!match || FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch ||
        FT_New_Face(library, (const char *)file, 0, &face) || FT_Set_Pixel_Sizes(face, 0, FONT_PX)) {
        return;
    }
#endif
    for (c = 32; c < 127; c++) {
        Glyph *g = &glyphs[c - 32];
        FT_Bitmap *b;
        if (FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) {
            continue;
        }
        b = &face->glyph->bitmap;
        g->w = (int)b->width;
        g->h = (int)b->rows;
        g->left = face->glyph->bitmap_left;
        g->top = face->glyph->bitmap_top;
        g->advance = (int)((face->glyph->advance.x + 32) >> 6);
        g->coverage = malloc((size_t)g->w * (size_t)g->h + 1);
        if (g->coverage) {
            int row;
            for (row = 0; row < g->h; row++) {
                memcpy(g->coverage + row * g->w, b->buffer + row * b->pitch, (size_t)g->w);
            }
        }
    }
    font_ascent = (int)(face->size->metrics.ascender >> 6);
    font_descent = (int)(-face->size->metrics.descender >> 6);
    font_loaded = 1;
#ifndef _WIN32
    FcPatternDestroy(pattern);
    FcPatternDestroy(match);
#endif
    FT_Done_Face(face);
    FT_Done_FreeType(library);
}

static void layout_bar(void);

int Menu_Scale(void) { return ui; }

void Menu_SetScale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    if (scale == ui) return;
    ui = scale;
    if (ready) {
        load_font();
        layout_bar();
    }
}

/* One step below the original automatic size, with a 1x minimum:
 * a 4x window is 1, a 4K display 2. Manual sizes are unchanged. */
int Menu_AutoScale(int window_h)
{
    int scale = (window_h + 240) / 480;
    scale = scale < 1 ? 1 : scale > 3 ? 3 : scale;
    return scale > 1 ? scale - 1 : 1;
}

static MenuCanvas *canvas;

static int text_width(const char *text)
{
    int width = 0;
    if (!font_loaded) {
        return (int)strlen(text) * 12 * ui;
    }
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        width += c >= 32 && c < 127 ? glyphs[c - 32].advance : glyphs[0].advance;
    }
    return width;
}

/* Source over destination. On an opaque canvas the destination's alpha is
 * ignored and the result is fully opaque (a backend may upload the canvas to
 * a texture that blends by alpha); on an overlay both alphas take part (straight, not premultiplied)
 * so the platform can blend the result over the picture. */
static inline uint32_t blend(uint32_t under, uint32_t over, unsigned alpha)
{
    unsigned inverse = 255 - alpha, r, g, b;
    if (canvas->alpha) {
        unsigned da = under >> 24, out_a = alpha + da * inverse / 255, back = da * inverse / 255;
        if (!out_a) {
            return 0;
        }
        r = ((over >> 16 & 0xff) * alpha + (under >> 16 & 0xff) * back) / out_a;
        g = ((over >> 8 & 0xff) * alpha + (under >> 8 & 0xff) * back) / out_a;
        b = ((over & 0xff) * alpha + (under & 0xff) * back) / out_a;
        return out_a << 24 | r << 16 | g << 8 | b;
    }
    r = ((over >> 16 & 0xff) * alpha + (under >> 16 & 0xff) * inverse + 127) / 255;
    g = ((over >> 8 & 0xff) * alpha + (under >> 8 & 0xff) * inverse + 127) / 255;
    b = ((over & 0xff) * alpha + (under & 0xff) * inverse + 127) / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static void put(int x, int y, uint32_t colour, unsigned alpha)
{
    uint32_t *at;
    if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height || !alpha) {
        return;
    }
    at = canvas->pixels + (size_t)y * (size_t)canvas->stride + (size_t)x;
    *at = alpha >= 255 ? (colour | 0xff000000u) : blend(*at, colour, alpha);
}

static void fill(int x, int y, int w, int h, uint32_t colour, unsigned alpha)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas->width) { w = canvas->width - x; }
    if (y + h > canvas->height) { h = canvas->height - y; }
    for (j = 0; j < h; j++) {
        uint32_t *row = canvas->pixels + (size_t)(y + j) * (size_t)canvas->stride + (size_t)x;
        if (alpha >= 255) {
            for (i = 0; i < w; i++) row[i] = colour | 0xff000000u;
        } else {
            for (i = 0; i < w; i++) row[i] = blend(row[i], colour, alpha);
        }
    }
}

static void clear_alpha_rect(int x, int y, int w, int h)
{
    int row;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas->width) w = canvas->width - x;
    if (y + h > canvas->height) h = canvas->height - y;
    if (w <= 0 || h <= 0) return;
    for (row = y; row < y + h; row++) {
        memset(canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)x, 0, (size_t)w * 4);
    }
}

static void outline(int x, int y, int w, int h, uint32_t colour)
{
    fill(x, y, w, 1, colour, 255);
    fill(x, y + h - 1, w, 1, colour, 255);
    fill(x, y, 1, h, colour, 255);
    fill(x + w - 1, y, 1, h, colour, 255);
}

/* An anti-aliased disc: coverage from the distance to the edge. */
static void disc(int cx, int cy, int radius, uint32_t colour)
{
    int x, y;
    for (y = -radius - 1; y <= radius + 1; y++) {
        for (x = -radius - 1; x <= radius + 1; x++) {
            float d = (float)__builtin_sqrtf((float)(x * x + y * y)) - (float)radius + 0.5f;
            unsigned alpha = d <= 0 ? 255 : d >= 1 ? 0 : (unsigned)((1 - d) * 255);
            put(cx + x, cy + y, colour, alpha);
        }
    }
}

static void draw_bitmap_text(int x, int middle, const char *text, uint32_t colour)
{
    for (; *text; text++, x += 12 * ui) {
        int i;
        for (i = 0; i < bitmap_font_count; i++) {
            int row, column;
            if (bitmap_font[i].code != *text) {
                continue;
            }
            for (row = 0; row < 7; row++) {
                for (column = 0; column < 5; column++) {
                    if (bitmap_font[i].rows[row] & (0x10 >> column)) {
                        fill(x + column * 2 * ui, middle - 7 * ui + row * 2 * ui, 2 * ui, 2 * ui, colour, 255);
                    }
                }
            }
            break;
        }
    }
}

/* Text with its vertical centre on `middle`. */
static void draw_text(int x, int middle, const char *text, uint32_t colour)
{
    int baseline;
    if (!font_loaded) {
        draw_bitmap_text(x, middle, text, colour);
        return;
    }
    baseline = middle + (font_ascent - font_descent + 1) / 2;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        const Glyph *g = &glyphs[c >= 32 && c < 127 ? c - 32 : 0];
        int row, column;
        for (row = 0; row < g->h && g->coverage; row++) {
            for (column = 0; column < g->w; column++) {
                put(x + g->left + column, baseline - g->top + row, colour, g->coverage[row * g->w + column]);
            }
        }
        x += g->advance;
    }
}

void Menu_DrawText(MenuCanvas *into, int x, int y, const char *text, uint32_t colour)
{
    canvas = into;
    draw_text(x, y, text, colour);
}

int Menu_TextWidth(const char *text) { return text_width(text); }

int Menu_TextWidthScaled(const char *text, int scale) { return text_width(text) * scale / ui; }
void Menu_DrawTextScaled(MenuCanvas *into, int x, int middle, const char *text, uint32_t colour, int scale)
{
    canvas = into;
    if (scale == ui) { draw_text(x, middle, text, colour); return; }
    if (!font_loaded) {
        int previous = ui; ui = scale; draw_bitmap_text(x, middle, text, colour); ui = previous; return;
    }
    int baseline = middle + (font_ascent - font_descent + 1) * scale / (2 * ui);
    int advance = 0;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        const Glyph *g = &glyphs[c >= 32 && c < 127 ? c - 32 : 0];
        for (int row = 0; row < g->h * scale / ui && g->coverage; row++)
            for (int col = 0; col < g->w * scale / ui; col++)
                put(x + advance * scale / ui + g->left * scale / ui + col,
                    baseline - g->top * scale / ui + row, colour,
                    g->coverage[(row * ui / scale) * g->w + col * ui / scale]);
        advance += g->advance;
    }
}

void Menu_LoadSettings(void)
{
    Paths_MigrateLegacySaves(); /* what older builds left in ./saves */
    Settings_Load();
    Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME));
    Spu_SetBusVolume(SPU_BUS_MUSIC, Settings_Get(SET_MUSIC_VOLUME));
    Spu_SetBusVolume(SPU_BUS_SFX, Settings_Get(SET_SFX_VOLUME));
    Spu_SetBusVolume(SPU_BUS_STREAM, Settings_Get(SET_STREAM_VOLUME));
    Spu_SetInterpolation((SpuInterpolation)Settings_Get(SET_AUDIO_INTERPOLATION));
    Platform_SetScale(Settings_Get(SET_SCALE));
    Memories_SetInternalScale(Settings_Get(SET_INTERNAL_SCALE));
    Platform_SetClockRate(Settings_Get(SET_SPEED));
    Platform_SetPresentCap(Settings_Get(SET_FPS));
    Mods_SetTexturePack(TexturePack_Load, TexturePack_Unload);
    Mods_SetAudio(AudioReplace_Load, AudioReplace_Unload);
    Mods_Load(); /* the mods the settings say are applied, once they are read */
}

static void setting_changed(SettingId id, int value)
{
    switch (id) {
    case SET_MASTER_VOLUME: Spu_SetOutputVolume(value); break;
    case SET_MUSIC_VOLUME: Spu_SetBusVolume(SPU_BUS_MUSIC, value); break;
    case SET_SFX_VOLUME: Spu_SetBusVolume(SPU_BUS_SFX, value); break;
    case SET_STREAM_VOLUME: Spu_SetBusVolume(SPU_BUS_STREAM, value); break;
    case SET_AUDIO_INTERPOLATION: Spu_SetInterpolation((SpuInterpolation)value); break;
    case SET_SCALE: Platform_SetScale(value); break;
    case SET_INTERNAL_SCALE: Memories_SetInternalScale(value); break;
    case SET_SPEED: Platform_SetClockRate(value); break;
    case SET_FPS: Platform_SetPresentCap(value); break;
    case SET_MENU_SCALE: Platform_ApplyDisplaySettings(); break;
    default: break;
    }
}

/* --- layout ---------------------------------------------------------- */

int Menu_Height(void) { return MENU_H; }

static void layout_bar(void)
{
    int i, at = 0;
    for (i = 0; i < MENU_COUNT; i++) {
        menus[i].w = text_width(menus[i].label) + BAR_PAD * 2;
        menus[i].x = at;
        at += menus[i].w;
    }
}

void Menu_Init(void)
{
    load_font();
    layout_bar();
    Settings_Observe(setting_changed);
    ready = 1;
}

void Menu_SetItemEnabled(int id, int enabled)
{
    int menu, item;
    for (menu = 0; menu < MENU_COUNT + SUB_COUNT; menu++) {
        Menu *m = menu < MENU_COUNT ? &menus[menu] : &submenus[menu - MENU_COUNT];
        for (item = 0; item < m->count; item++) {
            if (m->items[item].id == id) {
                if (enabled) m->items[item].flags &= ~ITEM_DISABLED;
                else m->items[item].flags |= ITEM_DISABLED;
            }
        }
    }
}

void Menu_SetVisible(int wanted) { visible = !!wanted; }
int Menu_IsOpen(void) { return open_menu >= 0; }

static int item_height(const Item *item)
{
    return item->kind == ITEM_SEPARATOR ? SEP_H : ITEM_H + (item->flags & ITEM_GROUP_BREAK ? SEP_H : 0);
}

/* Level 0 is the open bar menu, level 1 the open submenu. */
static const Menu *level_menu(int level) { return level ? &submenus[open_sub] : &menus[open_menu]; }

static int item_top(int level, int index);

/* A menu's box, without its shadow; a submenu sits beside its parent row,
 * or to its left when the window is too narrow. */
static void drop_geometry(int level, int *x, int *y, int *w, int *h)
{
    const Menu *menu = level_menu(level);
    int i, widest = 0, shortcuts = 0, height = DROP_PAD * 2;
    for (i = 0; i < menu->count; i++) {
        const Item *item = &menu->items[i];
        int wide;
        height += item_height(item);
        if (item->kind == ITEM_SEPARATOR) {
            continue;
        }
        wide = text_width(item->label);
        if (item->kind == ITEM_SLIDER) {
            wide += 12 * ui + SLIDER_W + 12 * ui + text_width("100");
        }
        widest = wide > widest ? wide : widest;
        if (item->shortcut) {
            int s = text_width(item->shortcut);
            shortcuts = s > shortcuts ? s : shortcuts;
        } else if (item->kind == ITEM_SUBMENU) {
            shortcuts = shortcuts > 8 * ui ? shortcuts : 8 * ui;
        }
    }
    *w = ITEM_PAD + MARK_W + widest + (shortcuts ? SHORTCUT_GAP + shortcuts : 0) + ITEM_PAD;
    *h = height;
    if (level == 0) {
        *x = menu->x;
        *y = MENU_H;
        if (*w < menu->w) {
            *w = menu->w;
        }
    } else {
        int px, py, pw, ph;
        drop_geometry(0, &px, &py, &pw, &ph);
        *x = px + pw - 2;
        *y = item_top(0, sub_item) - DROP_PAD;
        if (canvas && *x + *w + SHADOW > canvas->width && px - *w + 2 >= 0) *x = px - *w + 2;
    }
}

void Menu_Bounds(int *x, int *y, int *w, int *h)
{
    int level;
    if (!visible) {
        *x = *y = *w = *h = 0;
        return;
    }
    *x = 0;
    *y = 0;
    *w = canvas ? canvas->width : 0;
    *h = MENU_H;
    for (level = 0; level < (open_sub >= 0 ? 2 : open_menu >= 0 ? 1 : 0); level++) {
        int dx, dy, dw, dh;
        drop_geometry(level, &dx, &dy, &dw, &dh);
        if (dy + dh + SHADOW > *h) *h = dy + dh + SHADOW;
        if (dx + dw + SHADOW > *w) *w = dx + dw + SHADOW;
    }
}

/* Row `index` of a menu: its top. */
static int item_top(int level, int index)
{
    int x, y, w, h, i;
    const Menu *menu = level_menu(level);
    drop_geometry(level, &x, &y, &w, &h);
    y += DROP_PAD;
    for (i = 0; i < index; i++) {
        y += item_height(&menu->items[i]);
    }
    if (menu->items[index].flags & ITEM_GROUP_BREAK) y += SEP_H;
    return y;
}

static int inside_drop(int level, int px, int py)
{
    int x, y, w, h;
    drop_geometry(level, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int item_at(int level, int px, int py)
{
    int x, y, w, h, i;
    const Menu *menu = level_menu(level);
    drop_geometry(level, &x, &y, &w, &h);
    if (px < x || px >= x + w || py < y + DROP_PAD || py >= y + h - DROP_PAD) {
        return -1;
    }
    y += DROP_PAD;
    for (i = 0; i < menu->count; i++) {
        const Item *item = &menu->items[i];
        int height = item_height(item);
        if ((item->flags & ITEM_GROUP_BREAK) && py < y + SEP_H) return -1;
        if (py < y + height) {
            return item->kind == ITEM_SEPARATOR ? -1 : i;
        }
        y += height;
    }
    return -1;
}

static int bar_item_at(int x, int y)
{
    int i;
    if (y < 0 || y >= MENU_H) {
        return -1;
    }
    for (i = 0; i < MENU_COUNT; i++) {
        if (x >= menus[i].x && x < menus[i].x + menus[i].w) {
            return i;
        }
    }
    return -1;
}

static void slider_geometry(int level, int index, int *sx, int *middle)
{
    int x, y, w, h;
    drop_geometry(level, &x, &y, &w, &h);
    *sx = x + ITEM_PAD + MARK_W + text_width(level_menu(level)->items[index].label) + 12 * ui;
    *middle = item_top(level, index) + ITEM_H / 2;
}

/* A small triangle pointing right, for rows that open a submenu. */
static void draw_arrow(int x, int middle, uint32_t colour)
{
    int span = 4 * ui, c;
    for (c = 0; c < span; c++) {
        int half = (span - c) * 7 * ui / (2 * span);
        fill(x + c, middle - half, 1, 2 * half + 1, colour, 255);
    }
}

/* --- drawing --------------------------------------------------------- */

static void draw_check(int x, int middle, int checked)
{
    int size = 14 * ui, top = middle - size / 2;
    if (checked) {
        int i;
        fill(x, top, size, size, C_ACCENT, 255);
        /* A tick: a short stroke down-right, a long one up-right. */
        for (i = 0; i < 3 * ui; i++) {
            fill(x + 3 * ui + i, top + 7 * ui + i, 2 * ui, 2 * ui, C_TEXT_ON_ACCENT, 255);
        }
        for (i = 0; i < 6 * ui; i++) {
            fill(x + 5 * ui + i, top + 9 * ui - i, 2 * ui, 2 * ui, C_TEXT_ON_ACCENT, 255);
        }
    } else {
        int i;
        for (i = 0; i < ui; i++) outline(x + i, top + i, size - 2 * i, size - 2 * i, C_MARK);
    }
}

static void draw_radio(int x, int middle, int on)
{
    if (on) {
        disc(x + 7 * ui, middle, 7 * ui, C_ACCENT);
        disc(x + 7 * ui, middle, 3 * ui, C_TEXT_ON_ACCENT);
    } else {
        disc(x + 7 * ui, middle, 7 * ui, C_MARK);
        disc(x + 7 * ui, middle, 6 * ui, C_DROP);
    }
}

static int item_state(const Item *item)
{
    if (item->id == CHECK_MUTE) return Spu_Muted();
    if (item->id == CHECK_HUD) return Settings_Get(SET_SHOW_HUD) != 0;
    if (item->id == CHECK_HUD_FULL) return Settings_Get(SET_SHOW_HUD) == 2;
    if (item->id == ACT_PAUSE) return Platform_ClockRate() == 0;
    if (item->id == RADIO_STATE_SLOT) return Platform_StateSlot() == item->value;
    if (item->id == CHECK_TRACE) return Log_Enabled((LogChannel)item->value);
    if (item->setting >= 0 && item->kind == ITEM_CHECK) return Settings_Get(item->setting) != 0;
    if (item->setting >= 0 && item->kind == ITEM_RADIO) return Settings_Get(item->setting) == item->value;
    return 0;
}

void Menu_Draw(MenuCanvas *into)
{
    int i, level;
    canvas = into;
    if (!ready || !visible) {
        return;
    }
    if (canvas->alpha) {
        clear_alpha_rect(0, 0, canvas->width, MENU_H);
        for (level = 0; level < (open_sub >= 0 ? 2 : open_menu >= 0 ? 1 : 0); level++) {
            int x, y, w, h;
            drop_geometry(level, &x, &y, &w, &h);
            clear_alpha_rect(x, y, w + SHADOW, h + SHADOW);
        }
    }
    fill(0, 0, canvas->width, MENU_H - 1, C_BAR, 255);
    fill(0, MENU_H - 1, canvas->width, 1, C_BAR_EDGE, 255);
    for (i = 0; i < MENU_COUNT; i++) {
        const Menu *menu = &menus[i];
        uint32_t ink = C_TEXT;
        if (i == open_menu) {
            fill(menu->x, 0, menu->w, MENU_H - 1, C_ACCENT, 255);
            ink = C_TEXT_ON_ACCENT;
        } else if (i == hover_bar) {
            fill(menu->x, 0, menu->w, MENU_H - 1, C_BAR_HOVER, 255);
        }
        draw_text(menu->x + BAR_PAD, MENU_H / 2, menu->label, ink);
    }
    for (level = 0; level < (open_sub >= 0 ? 2 : open_menu >= 0 ? 1 : 0); level++) {
        const Menu *menu = level_menu(level);
        int x, y, w, h, top, hot_row = level ? hot_sub : hot_item;
        drop_geometry(level, &x, &y, &w, &h);
        /* The shadow: SHADOW copies of the box, each offset one more pixel,
         * blended over each other. Only the part outside the box shows (the
         * box is opaque), so blend just each copy's right and bottom strips:
         * blending whole boxes was 12 alpha passes over a 4K dropdown every
         * frame, and the game crawled while a menu was open. */
        for (i = SHADOW; i > 0; i--) {
            unsigned alpha = (unsigned)(10 + (SHADOW - i) * 8 / ui);
            fill(x + w, y + i, i, h, 0x000000u, alpha);
            fill(x + i, y + h, w, i, 0x000000u, alpha);
        }
        fill(x, y, w, h, C_DROP, 255);
        outline(x, y, w, h, C_DROP_EDGE);
        top = y + DROP_PAD;
        for (i = 0; i < menu->count; i++) {
            const Item *item = &menu->items[i];
            int middle, hot = i == hot_row;
            int disabled = item->flags & ITEM_DISABLED;
            uint32_t ink = disabled ? C_TEXT_DIM : hot ? C_TEXT_ON_ACCENT : C_TEXT;
            uint32_t dim = disabled ? C_TEXT_DIM : hot ? C_TEXT_ON_ACCENT : C_TEXT_DIM;
            if (item->kind == ITEM_SEPARATOR) {
                fill(x + ITEM_PAD, top + SEP_H / 2, w - ITEM_PAD * 2, 1, C_SEP, 255);
                top += SEP_H;
                continue;
            }
            if (item->flags & ITEM_GROUP_BREAK) {
                fill(x + ITEM_PAD, top + SEP_H / 2, w - ITEM_PAD * 2, 1, C_SEP, 255);
                top += SEP_H;
            }
            middle = top + ITEM_H / 2;
            if (hot && !disabled) {
                fill(x + 3 * ui, top, w - 6 * ui, ITEM_H, C_ACCENT, 255);
            }
            if (item->kind == ITEM_CHECK) {
                draw_check(x + ITEM_PAD, middle, item_state(item));
            } else if (item->kind == ITEM_RADIO) {
                draw_radio(x + ITEM_PAD, middle, item_state(item));
            }
            draw_text(x + ITEM_PAD + MARK_W, middle, item->label, ink);
            if (item->shortcut) {
                draw_text(x + w - ITEM_PAD - text_width(item->shortcut), middle, item->shortcut, dim);
            } else if (item->kind == ITEM_SUBMENU) {
                draw_arrow(x + w - ITEM_PAD - 6 * ui, middle, ink);
            }
            if (item->kind == ITEM_SLIDER) {
                int sx, sm, knob, filled;
                char value[8];
                int minimum = Settings_Min(item->setting), maximum = Settings_Max(item->setting);
                int setting = Settings_Get(item->setting);
                slider_geometry(level, i, &sx, &sm);
                knob = sx + KNOB_R + (setting - minimum) * (SLIDER_W - KNOB_R * 2) / (maximum - minimum);
                filled = knob - sx;
                fill(sx, sm - SLIDER_H / 2, SLIDER_W, SLIDER_H, C_TRACK, 255);
                fill(sx, sm - SLIDER_H / 2, filled, SLIDER_H, hot ? C_TEXT_ON_ACCENT : C_ACCENT, 255);
                disc(knob, sm, KNOB_R, C_KNOB_EDGE);
                disc(knob, sm, KNOB_R - 1, C_KNOB);
                snprintf(value, sizeof(value), "%d", setting);
                draw_text(sx + SLIDER_W + 12 * ui, middle, value, setting ? ink : dim);
            }
            top += ITEM_H;
        }
    }
}

/* --- behaviour ------------------------------------------------------- */

static void set_slider(const Item *item, int value)
{
    Settings_Set(item->setting, value);
}

static void slider_from_pointer(int level, int index, int px)
{
    const Item *item = &level_menu(level)->items[index];
    int sx, middle, span = SLIDER_W - KNOB_R * 2;
    int minimum = Settings_Min(item->setting), maximum = Settings_Max(item->setting);
    slider_geometry(level, index, &sx, &middle);
    set_slider(item, minimum + ((px - sx - KNOB_R) * (maximum - minimum) + span / 2) /
               (span > 0 ? span : 1));
}

static void close_submenu(void)
{
    open_sub = -1;
    sub_item = -1;
    hot_sub = -1;
}

static void close_menu(void)
{
    open_menu = -1;
    hot_item = -1;
    grabbed = 0;
    close_submenu();
}

static void open_submenu(int index)
{
    const Item *item = &menus[open_menu].items[index];
    if (open_sub == item->value && sub_item == index) return;
    close_submenu();
    open_sub = item->value;
    sub_item = index;
    hot_item = index;
}

/* The row the keyboard works on: the submenu's when one is open. */
static int active_level(void) { return open_sub >= 0 ? 1 : 0; }
static int *active_hot(void) { return open_sub >= 0 ? &hot_sub : &hot_item; }

static void activate(const Item *item, int *quit)
{
    if (item->flags & ITEM_DISABLED) return;
    switch (item->id) {
    case ACT_MODS: Platform_OpenMods(); break;
    case ACT_CONTROLS: Platform_OpenControls(); break;
    case ACT_SAVE_STATE: Memories_StateRequest(1, Platform_StateSlot()); break;
    case ACT_LOAD_STATE: Memories_StateRequest(2, Platform_StateSlot()); break;
    case ACT_SCREENSHOT: Platform_Screenshot(0); break;
    case ACT_EXIT: *quit = 1; break;
    case ACT_GIVE_CARDS: Cheats_GiveAllCards(3); break;
    case ACT_UNLOCK_FREE_DUELISTS: Cheats_UnlockAllFreeDuelists(); break;
    case ACT_RESET_COLOR:
        Settings_Set(SET_BRIGHTNESS, 100);
        Settings_Set(SET_CONTRAST, 100);
        Settings_Set(SET_SATURATION, 100);
        Settings_Set(SET_GAMMA, 100);
        Settings_Save();
        break;
    case MENU_ITEM_TITLE: TitleJump_Request(); break;
    case MENU_ITEM_DECKS: DeckMenu_Request(); break;
    case ACT_RELOAD_SETTINGS:
        Menu_LoadSettings();
        Platform_ApplyDisplaySettings();
        break;
    case ACT_PAUSE:
        Platform_SetClockRate(Platform_ClockRate() == 0 ? Settings_Get(SET_SPEED) : 0);
        break;
    case ACT_FRAME_STEP: Platform_StepFrame(); break;
    case ACT_DUMP_FRAME: case ACT_DUMP_VRAM: {
        char path[640];
        snprintf(path, sizeof(path), "%s/%s.ppm", Crash_ReportDir, item->id == ACT_DUMP_VRAM ? "vram" : "frame");
        Memories_DumpFrame(path, item->id == ACT_DUMP_VRAM);
        break;
    }
    case CHECK_MUTE: Spu_SetMuted(!Spu_Muted()); break;
    case CHECK_HUD:
        Settings_Set(SET_SHOW_HUD, Settings_Get(SET_SHOW_HUD) ? 0 : 1);
        Settings_Save();
        break;
    case CHECK_HUD_FULL:
        Settings_Set(SET_SHOW_HUD, Settings_Get(SET_SHOW_HUD) == 2 ? 1 : 2);
        Settings_Save();
        break;
    case RADIO_STATE_SLOT: Platform_SetStateSlot(item->value); break;
    case CHECK_TRACE: Log_Enable((LogChannel)item->value, !Log_Enabled((LogChannel)item->value)); break;
    default:
        if (item->setting >= 0 && item->kind == ITEM_CHECK) {
            Settings_Set(item->setting, !Settings_Get(item->setting));
        } else if (item->setting >= 0 && item->kind == ITEM_RADIO) {
            Settings_Set(item->setting, item->value);
        }
        Settings_Save();
        if (item->id >= MENU_ITEM_SCALE_1 && item->id <= MENU_ITEM_VSYNC) {
            Platform_ApplyDisplaySettings();
        }
    }
    close_menu();
}

/* The next selectable row after `from` in `direction`, wrapping. */
static int step_item(int level, int from, int direction)
{
    const Menu *menu = level_menu(level);
    int i, index = from;
    for (i = 0; i < menu->count; i++) {
        index = (index + direction + menu->count) % menu->count;
        if (menu->items[index].kind != ITEM_SEPARATOR && !(menu->items[index].flags & ITEM_DISABLED)) {
            return index;
        }
    }
    return -1;
}

int Menu_Event(const MenuEvent *event, int *quit)
{
    if (!ready || !visible) {
        return 0;
    }
    switch (event->type) {
    case MENU_EVENT_BUTTON_DOWN: {
        int px = event->x, py = event->y, bar = bar_item_at(px, py);
        LOG(LOG_MENU, "press b%d at %d,%d open=%d", event->button, px, py, open_menu);
        if (event->button < 1 || event->button > 3) {
            return 0;
        }
        /* While a menu is open the window belongs to it: nothing reaches the
         * pad, not even the wheel, which would otherwise step the game's
         * cursor and play its sound behind the menu. */
        if (py >= MENU_H && open_menu < 0) {
            return 0;
        }
        if (py < MENU_H) {
            if (event->button == 1) {
                if (bar >= 0 && bar != open_menu) {
                    close_menu();
                    open_menu = bar;
                } else {
                    close_menu();
                }
            }
        } else if (open_sub >= 0 && inside_drop(1, px, py)) {
            int index = item_at(1, px, py);
            if (index >= 0 && event->button == 1) {
                const Item *item = &submenus[open_sub].items[index];
                if (item->kind == ITEM_SLIDER && !(item->flags & ITEM_DISABLED)) {
                    hot_sub = index;
                    slider_from_pointer(1, index, px);
                    grabbed = 2;
                } else {
                    activate(item, quit);
                }
            }
        } else if (inside_drop(0, px, py)) {
            int index = item_at(0, px, py);
            if (index >= 0 && event->button == 1) {
                const Item *item = &menus[open_menu].items[index];
                if (item->kind == ITEM_SUBMENU) {
                    open_submenu(index);
                } else if (item->kind == ITEM_SLIDER && !(item->flags & ITEM_DISABLED)) {
                    hot_item = index;
                    slider_from_pointer(0, index, px);
                    grabbed = 1;
                } else {
                    activate(item, quit);
                }
            }
        } else {
            close_menu(); /* a click outside an open menu only closes it */
        }
        consumed_press[event->button] = 1;
        return 1;
    }
    case MENU_EVENT_BUTTON_UP:
        if (event->button < 1 || event->button > 3 || !consumed_press[event->button]) {
            return 0;
        }
        consumed_press[event->button] = 0;
        if (grabbed && event->button == 1) {
            grabbed = 0;
            Settings_Save();
        }
        return 1;
    case MENU_EVENT_WHEEL:
        if (open_menu >= 0 && *active_hot() >= 0) {
            const Item *item = &level_menu(active_level())->items[*active_hot()];
            if (item->kind == ITEM_SLIDER && !(item->flags & ITEM_DISABLED)) {
                set_slider(item, Settings_Get(item->setting) + 5 * event->wheel);
                Settings_Save();
                return 1;
            }
        }
        return open_menu >= 0 || event->y < MENU_H;
    case MENU_EVENT_MOTION: {
        int px = event->x, py = event->y;
        int bar = bar_item_at(px, py), was_hot = hot_item, was_sub = hot_sub, was_open = open_sub;
        int was_bar = hover_bar, index;
        if (grabbed) {
            slider_from_pointer(grabbed - 1, grabbed == 2 ? hot_sub : hot_item, px);
            return 1;
        }
        hover_bar = bar;
        if (open_menu >= 0) {
            if (bar >= 0 && bar != open_menu) {
                close_menu(); /* dragging along the bar opens the next menu */
                open_menu = bar;
                return 1;
            }
            if (open_sub >= 0 && inside_drop(1, px, py)) {
                hot_sub = item_at(1, px, py);
            } else if ((index = item_at(0, px, py)) >= 0) {
                hot_item = index;
                if (menus[open_menu].items[index].kind == ITEM_SUBMENU) open_submenu(index);
                else close_submenu();
            } else if (open_sub < 0) {
                hot_item = -1; /* with a submenu open its parent row stays lit */
            } else {
                hot_sub = -1;
            }
            return was_hot != hot_item || was_sub != hot_sub || was_open != open_sub || was_bar != hover_bar;
        }
        return was_bar != hover_bar;
    }
    case MENU_EVENT_LEAVE:
        if (hover_bar >= 0 && open_menu < 0) {
            hover_bar = -1;
            return 1;
        }
        return 0;
    case MENU_EVENT_KEY_DOWN:
        LOG(LOG_MENU, "key %d open=%d sub=%d hot=%d/%d", (int)event->key, open_menu, open_sub, hot_item, hot_sub);
        if (open_menu < 0) {
            if (event->key == MENU_KEY_F10) {
                open_menu = 0;
                hot_item = step_item(0, -1, 1);
                return 1;
            }
            return 0;
        }
        switch (event->key) {
        case MENU_KEY_ESCAPE:
            if (open_sub >= 0) close_submenu();
            else close_menu();
            return 1;
        case MENU_KEY_LEFT: case MENU_KEY_RIGHT: {
            int level = active_level(), *hot = active_hot();
            const Item *item = *hot >= 0 ? &level_menu(level)->items[*hot] : NULL;
            if (item && item->kind == ITEM_SLIDER) {
                set_slider(item, Settings_Get(item->setting) + (event->key == MENU_KEY_RIGHT ? 5 : -5));
                Settings_Save();
            } else if (event->key == MENU_KEY_RIGHT && item && item->kind == ITEM_SUBMENU) {
                open_submenu(*hot);
                hot_sub = step_item(1, -1, 1);
            } else if (event->key == MENU_KEY_LEFT && open_sub >= 0) {
                close_submenu();
            } else {
                int next = (open_menu + (event->key == MENU_KEY_RIGHT ? 1 : MENU_COUNT - 1)) % MENU_COUNT;
                close_menu();
                open_menu = next;
                hot_item = step_item(0, -1, 1);
            }
            return 1;
        }
        case MENU_KEY_UP: *active_hot() = step_item(active_level(), *active_hot() < 0 ? 0 : *active_hot(), -1); return 1;
        case MENU_KEY_DOWN: *active_hot() = step_item(active_level(), *active_hot(), 1); return 1;
        case MENU_KEY_ENTER: {
            int level = active_level(), *hot = active_hot();
            const Item *item = *hot >= 0 ? &level_menu(level)->items[*hot] : NULL;
            if (item && item->kind == ITEM_SUBMENU) {
                open_submenu(*hot);
                hot_sub = step_item(1, -1, 1);
            } else if (item && item->kind != ITEM_SLIDER) {
                activate(item, quit);
            }
            return 1;
        }
        default: return 1; /* the open menu swallows other keys */
        }
    case MENU_EVENT_KEY_UP:
        return open_menu >= 0;
    default:
        return 0;
    }
}
