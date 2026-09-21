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
#include "pc/debug/cheats.h"
#include "pc/guest/state.h"
#include "pc/mods/mods.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Geometry. */
#define MENU_H 26
#define ITEM_H 26
#define BAR_PAD 12    /* left and right of a bar label */
#define ITEM_PAD 12   /* dropdown edge to the mark column */
#define MARK_W 22     /* the check or radio column */
#define SHORTCUT_GAP 28
#define DROP_PAD 4    /* dropdown border to the first item */
#define SEP_H 9
#define SLIDER_W 160
#define SLIDER_H 4
#define KNOB_R 6
#define SHADOW 6
#define FONT_PX 13

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

typedef enum { ITEM_ACTION, ITEM_CHECK, ITEM_RADIO, ITEM_SLIDER, ITEM_SEPARATOR } ItemKind;
enum { ITEM_DISABLED = 1, ITEM_GROUP_BREAK = 2 };

enum {
    ACT_SAVE_STATE = 1, ACT_LOAD_STATE, ACT_EXIT, SLIDER_VOLUME, ACT_GIVE_CARDS,
    CHECK_MOD = 200    /* + mod */
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

enum { MENU_FILE, MENU_AUDIO, MENU_VIEW, MENU_MODS, MENU_DEBUG, MENU_COUNT };
static Menu menus[MENU_COUNT] = {
    {"File", {{"Save state", "F5", ITEM_ACTION, ACT_SAVE_STATE, -1},
              {"Load state", "F7", ITEM_ACTION, ACT_LOAD_STATE, -1},
              {0, 0, ITEM_SEPARATOR, 0, -1}, {"Exit", "Esc", ITEM_ACTION, ACT_EXIT, -1}}, 4},
    {"Audio", {{"Volume", 0, ITEM_SLIDER, SLIDER_VOLUME, SET_MASTER_VOLUME}}, 1},
    {"View", {{"Window scale: 1x", 0, ITEM_RADIO, MENU_ITEM_SCALE_1, SET_SCALE, 1},
              {"Window scale: 2x", 0, ITEM_RADIO, MENU_ITEM_SCALE_2, SET_SCALE, 2},
              {"Window scale: 3x", 0, ITEM_RADIO, MENU_ITEM_SCALE_3, SET_SCALE, 3},
              {"Window scale: 4x", 0, ITEM_RADIO, MENU_ITEM_SCALE_4, SET_SCALE, 4},
              {"Window scale: 5x", 0, ITEM_RADIO, MENU_ITEM_SCALE_5, SET_SCALE, 5},
              {"Window scale: 6x", 0, ITEM_RADIO, MENU_ITEM_SCALE_6, SET_SCALE, 6},
              {"Fullscreen", "F11", ITEM_CHECK, MENU_ITEM_FULLSCREEN, SET_FULLSCREEN, 0, ITEM_GROUP_BREAK},
              {"Borderless window", 0, ITEM_CHECK, MENU_ITEM_BORDERLESS, SET_BORDERLESS},
              {"Integer scaling", 0, ITEM_RADIO, MENU_ITEM_SCALING_INTEGER, SET_SCALING, 0, ITEM_GROUP_BREAK},
              {"Fit to window", 0, ITEM_RADIO, MENU_ITEM_SCALING_FIT, SET_SCALING, 1},
              {"Stretch", 0, ITEM_RADIO, MENU_ITEM_SCALING_STRETCH, SET_SCALING, 2},
              {"4:3 aspect", 0, ITEM_RADIO, MENU_ITEM_ASPECT_4_3, SET_ASPECT, 0, ITEM_GROUP_BREAK},
              {"Square pixels", 0, ITEM_RADIO, MENU_ITEM_ASPECT_SQUARE, SET_ASPECT, 1},
              {"Smooth filtering", 0, ITEM_CHECK, MENU_ITEM_FILTER, SET_FILTER, 0, ITEM_GROUP_BREAK},
              {"VSync", 0, ITEM_CHECK, MENU_ITEM_VSYNC, SET_VSYNC}}, 15},
    {"Mods", {{0}}, 0},
    {"Debug", {{"Give 3 of every card", 0, ITEM_ACTION, ACT_GIVE_CARDS, -1}}, 1},
};

static int open_menu = -1, hot_item = -1, hover_bar = -1, grabbed, ready, visible = 1;
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
    FcPattern *pattern, *match;
    FcResult result;
    FcChar8 *file = NULL;
    int c;
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
    FcPatternDestroy(pattern);
    FcPatternDestroy(match);
}

static MenuCanvas *canvas;

static int text_width(const char *text)
{
    int width = 0;
    if (!font_loaded) {
        return (int)strlen(text) * 12;
    }
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        width += c >= 32 && c < 127 ? glyphs[c - 32].advance : glyphs[0].advance;
    }
    return width;
}

/* Source over destination. On an opaque canvas the destination's alpha is
 * ignored; on an overlay both alphas take part (straight, not premultiplied)
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
    return r << 16 | g << 8 | b;
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
    for (; *text; text++, x += 12) {
        int i;
        for (i = 0; i < bitmap_font_count; i++) {
            int row, column;
            if (bitmap_font[i].code != *text) {
                continue;
            }
            for (row = 0; row < 7; row++) {
                for (column = 0; column < 5; column++) {
                    if (bitmap_font[i].rows[row] & (0x10 >> column)) {
                        fill(x + column * 2, middle - 7 + row * 2, 2, 2, colour, 255);
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

void Menu_LoadSettings(void)
{
    Settings_Load();
    Spu_SetOutputVolume(Settings_Get(SET_MASTER_VOLUME));
    Platform_SetScale(Settings_Get(SET_SCALE));
    Mods_SetEnabled(MODS_FIELD_MODELS, Settings_Get(SET_MOD_3D_MONSTERS));
    Mods_SetEnabled(MODS_HAND_CAMERA, Settings_Get(SET_MOD_HAND_CAMERA));
}

static void setting_changed(SettingId id, int value)
{
    switch (id) {
    case SET_MASTER_VOLUME: Spu_SetOutputVolume(value); break;
    case SET_SCALE: Platform_SetScale(value); break;
    case SET_MOD_3D_MONSTERS: Mods_SetEnabled(MODS_FIELD_MODELS, value); break;
    case SET_MOD_HAND_CAMERA: Mods_SetEnabled(MODS_HAND_CAMERA, value); break;
    default: break;
    }
}

/* --- layout ---------------------------------------------------------- */

int Menu_Height(void) { return MENU_H; }

void Menu_Init(void)
{
    int i, at = 0;
    Menu *mods = &menus[MENU_MODS];
    load_font();
    for (i = 0; i < MODS_COUNT && i < 8; i++) {
        mods->items[i].label = Mods_Name(i);
        mods->items[i].kind = ITEM_CHECK;
        mods->items[i].id = CHECK_MOD + i;
        mods->items[i].setting = i == MODS_FIELD_MODELS ? SET_MOD_3D_MONSTERS : SET_MOD_HAND_CAMERA;
    }
    mods->count = i;
    for (i = 0; i < MENU_COUNT; i++) {
        menus[i].w = text_width(menus[i].label) + BAR_PAD * 2;
        menus[i].x = at;
        at += menus[i].w;
    }
    Settings_Observe(setting_changed);
    ready = 1;
}

void Menu_SetItemEnabled(int id, int enabled)
{
    int menu, item;
    for (menu = 0; menu < MENU_COUNT; menu++) {
        for (item = 0; item < menus[menu].count; item++) {
            if (menus[menu].items[item].id == id) {
                if (enabled) menus[menu].items[item].flags &= ~ITEM_DISABLED;
                else menus[menu].items[item].flags |= ITEM_DISABLED;
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

/* The open menu's box, without its shadow. */
static void drop_geometry(int which, int *x, int *y, int *w, int *h)
{
    const Menu *menu = &menus[which];
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
            wide += 12 + SLIDER_W + 12 + text_width("100");
        }
        widest = wide > widest ? wide : widest;
        if (item->shortcut) {
            int s = text_width(item->shortcut);
            shortcuts = s > shortcuts ? s : shortcuts;
        }
    }
    *x = menu->x;
    *y = MENU_H;
    *w = ITEM_PAD + MARK_W + widest + (shortcuts ? SHORTCUT_GAP + shortcuts : 0) + ITEM_PAD;
    if (*w < menu->w) {
        *w = menu->w;
    }
    *h = height;
}

void Menu_Bounds(int *x, int *y, int *w, int *h)
{
    if (!visible) {
        *x = *y = *w = *h = 0;
        return;
    }
    *x = 0;
    *y = 0;
    *w = canvas ? canvas->width : 0;
    *h = MENU_H;
    if (open_menu >= 0) {
        int dx, dy, dw, dh;
        drop_geometry(open_menu, &dx, &dy, &dw, &dh);
        *h = dy + dh + SHADOW - *y;
        if (dx + dw + SHADOW > *w) {
            *w = dx + dw + SHADOW;
        }
    }
}

/* Row `index` of the open menu: its top, or -1 past the end. */
static int item_top(int which, int index)
{
    int x, y, w, h, i;
    drop_geometry(which, &x, &y, &w, &h);
    y += DROP_PAD;
    for (i = 0; i < index; i++) {
        y += item_height(&menus[which].items[i]);
    }
    if (menus[which].items[index].flags & ITEM_GROUP_BREAK) y += SEP_H;
    return y;
}

static int item_at(int which, int px, int py)
{
    int x, y, w, h, i;
    if (which < 0) {
        return -1;
    }
    drop_geometry(which, &x, &y, &w, &h);
    if (px < x || px >= x + w || py < y + DROP_PAD || py >= y + h - DROP_PAD) {
        return -1;
    }
    y += DROP_PAD;
    for (i = 0; i < menus[which].count; i++) {
        const Item *item = &menus[which].items[i];
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

static void slider_geometry(int which, int index, int *sx, int *middle)
{
    int x, y, w, h;
    drop_geometry(which, &x, &y, &w, &h);
    *sx = x + ITEM_PAD + MARK_W + text_width(menus[which].items[index].label) + 12;
    *middle = item_top(which, index) + ITEM_H / 2;
}

/* --- drawing --------------------------------------------------------- */

static void draw_check(int x, int middle, int checked)
{
    int size = 14, top = middle - size / 2;
    if (checked) {
        int i;
        fill(x, top, size, size, C_ACCENT, 255);
        /* A tick: a short stroke down-right, a long one up-right. */
        for (i = 0; i < 3; i++) {
            fill(x + 3 + i, top + 7 + i, 2, 2, C_TEXT_ON_ACCENT, 255);
        }
        for (i = 0; i < 6; i++) {
            fill(x + 5 + i, top + 9 - i, 2, 2, C_TEXT_ON_ACCENT, 255);
        }
    } else {
        outline(x, top, size, size, C_MARK);
    }
}

static void draw_radio(int x, int middle, int on)
{
    if (on) {
        disc(x + 7, middle, 7, C_ACCENT);
        disc(x + 7, middle, 3, C_TEXT_ON_ACCENT);
    } else {
        disc(x + 7, middle, 7, C_MARK);
        disc(x + 7, middle, 6, C_DROP);
    }
}

static int item_state(const Item *item)
{
    if (item->id >= CHECK_MOD) return Mods_Enabled(item->id - CHECK_MOD);
    if (item->setting >= 0 && item->kind == ITEM_CHECK) return Settings_Get(item->setting) != 0;
    if (item->setting >= 0 && item->kind == ITEM_RADIO) return Settings_Get(item->setting) == item->value;
    return 0;
}

void Menu_Draw(MenuCanvas *into)
{
    int i;
    canvas = into;
    if (!ready || !visible) {
        return;
    }
    if (canvas->alpha) {
        int x, y, w, h;
        Menu_Bounds(&x, &y, &w, &h);
        for (i = y; i < y + h && i < canvas->height; i++) {
            memset(canvas->pixels + (size_t)i * (size_t)canvas->stride, 0, (size_t)canvas->width * 4);
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
    if (open_menu >= 0) {
        const Menu *menu = &menus[open_menu];
        int x, y, w, h, top;
        drop_geometry(open_menu, &x, &y, &w, &h);
        for (i = SHADOW; i > 0; i--) {
            fill(x + i, y + i, w, h, 0x000000u, (unsigned)(10 + (SHADOW - i) * 8));
        }
        fill(x, y, w, h, C_DROP, 255);
        outline(x, y, w, h, C_DROP_EDGE);
        top = y + DROP_PAD;
        for (i = 0; i < menu->count; i++) {
            const Item *item = &menu->items[i];
            int middle, hot = i == hot_item;
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
                fill(x + 3, top, w - 6, ITEM_H, C_ACCENT, 255);
            }
            if (item->kind == ITEM_CHECK) {
                draw_check(x + ITEM_PAD, middle, item_state(item));
            } else if (item->kind == ITEM_RADIO) {
                draw_radio(x + ITEM_PAD, middle, item_state(item));
            }
            draw_text(x + ITEM_PAD + MARK_W, middle, item->label, ink);
            if (item->shortcut) {
                draw_text(x + w - ITEM_PAD - text_width(item->shortcut), middle, item->shortcut, dim);
            }
            if (item->kind == ITEM_SLIDER) {
                int sx, sm, knob, filled;
                char value[8];
                int minimum = Settings_Min(item->setting), maximum = Settings_Max(item->setting);
                int setting = Settings_Get(item->setting);
                slider_geometry(open_menu, i, &sx, &sm);
                knob = sx + KNOB_R + (setting - minimum) * (SLIDER_W - KNOB_R * 2) / (maximum - minimum);
                filled = knob - sx;
                fill(sx, sm - SLIDER_H / 2, SLIDER_W, SLIDER_H, C_TRACK, 255);
                fill(sx, sm - SLIDER_H / 2, filled, SLIDER_H, hot ? C_TEXT_ON_ACCENT : C_ACCENT, 255);
                disc(knob, sm, KNOB_R, C_KNOB_EDGE);
                disc(knob, sm, KNOB_R - 1, C_KNOB);
                snprintf(value, sizeof(value), "%d", setting);
                draw_text(sx + SLIDER_W + 12, middle, value, setting ? ink : dim);
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

static void slider_from_pointer(int which, int index, int px)
{
    const Item *item = &menus[which].items[index];
    int sx, middle, span = SLIDER_W - KNOB_R * 2;
    int minimum = Settings_Min(item->setting), maximum = Settings_Max(item->setting);
    slider_geometry(which, index, &sx, &middle);
    set_slider(item, minimum + ((px - sx - KNOB_R) * (maximum - minimum) + span / 2) /
               (span > 0 ? span : 1));
}

static void close_menu(void)
{
    open_menu = -1;
    hot_item = -1;
    grabbed = 0;
}

static void activate(const Item *item, int *quit)
{
    if (item->flags & ITEM_DISABLED) return;
    switch (item->id) {
    case ACT_SAVE_STATE: Memories_StateRequest(1, 0); break;
    case ACT_LOAD_STATE: Memories_StateRequest(2, 0); break;
    case ACT_EXIT: *quit = 1; break;
    case ACT_GIVE_CARDS: Cheats_GiveAllCards(3); break;
    default:
        if (item->id >= CHECK_MOD) {
            Settings_Set(item->setting, !Mods_Enabled(item->id - CHECK_MOD));
        } else if (item->setting >= 0 && item->kind == ITEM_CHECK) {
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
static int step_item(int which, int from, int direction)
{
    const Menu *menu = &menus[which];
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
        if (getenv("MEMORIES_TRACE_MENU")) {
            fprintf(stderr, "menu: press b%d at %d,%d open=%d\n", event->button, px, py, open_menu);
        }
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
                    open_menu = bar;
                    hot_item = -1;
                } else {
                    close_menu();
                }
            }
        } else {
            int index = item_at(open_menu, px, py);
            int x, y, w, h;
            drop_geometry(open_menu, &x, &y, &w, &h);
            if (px < x || px >= x + w || py < y || py >= y + h) {
                close_menu(); /* a click outside an open menu only closes it */
            } else if (index >= 0 && event->button == 1) {
                const Item *item = &menus[open_menu].items[index];
                if (item->kind == ITEM_SLIDER && !(item->flags & ITEM_DISABLED)) {
                    hot_item = index;
                    slider_from_pointer(open_menu, index, px);
                    grabbed = 1;
                } else {
                    activate(item, quit);
                }
            }
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
        if (open_menu >= 0 && hot_item >= 0 &&
            menus[open_menu].items[hot_item].kind == ITEM_SLIDER &&
            !(menus[open_menu].items[hot_item].flags & ITEM_DISABLED)) {
            const Item *item = &menus[open_menu].items[hot_item];
            set_slider(item, Settings_Get(item->setting) + 5 * event->wheel);
            Settings_Save();
            return 1;
        }
        return open_menu >= 0 || event->y < MENU_H;
    case MENU_EVENT_MOTION: {
        int px = event->x, py = event->y;
        int bar = bar_item_at(px, py), was_hot = hot_item, was_bar = hover_bar;
        if (grabbed) {
            slider_from_pointer(open_menu, hot_item, px);
            return 1;
        }
        hover_bar = bar;
        if (open_menu >= 0) {
            if (bar >= 0 && bar != open_menu) {
                open_menu = bar; /* dragging along the bar opens the next menu */
                hot_item = -1;
                return 1;
            }
            hot_item = item_at(open_menu, px, py);
            return was_hot != hot_item || was_bar != hover_bar;
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
        if (open_menu < 0) {
            if (event->key == MENU_KEY_F10) {
                open_menu = 0;
                hot_item = step_item(0, -1, 1);
                return 1;
            }
            return 0;
        }
        switch (event->key) {
        case MENU_KEY_ESCAPE: close_menu(); return 1;
        case MENU_KEY_LEFT: case MENU_KEY_RIGHT:
            if (hot_item >= 0 && menus[open_menu].items[hot_item].kind == ITEM_SLIDER) {
                const Item *item = &menus[open_menu].items[hot_item];
                set_slider(item, Settings_Get(item->setting) + (event->key == MENU_KEY_RIGHT ? 5 : -5));
                Settings_Save();
            } else {
                open_menu = (open_menu + (event->key == MENU_KEY_RIGHT ? 1 : MENU_COUNT - 1)) % MENU_COUNT;
                hot_item = step_item(open_menu, -1, 1);
            }
            return 1;
        case MENU_KEY_UP: hot_item = step_item(open_menu, hot_item < 0 ? 0 : hot_item, -1); return 1;
        case MENU_KEY_DOWN: hot_item = step_item(open_menu, hot_item, 1); return 1;
        case MENU_KEY_ENTER:
            if (hot_item >= 0 && menus[open_menu].items[hot_item].kind != ITEM_SLIDER) {
                activate(&menus[open_menu].items[hot_item], quit);
            }
            return 1;
        default: return 1; /* the open menu swallows other keys */
        }
    case MENU_EVENT_KEY_UP:
        return open_menu >= 0;
    default:
        return 0;
    }
}
