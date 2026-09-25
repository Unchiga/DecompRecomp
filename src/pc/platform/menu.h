#ifndef MEMORIES_PC_MENU_H
#define MEMORIES_PC_MENU_H
#include <stdint.h>

/* The window's menu bar. It is composited in software into a buffer the
 * platform owns, so the port needs no toolkit and every backend shows the
 * same menu; text is FreeType through fontconfig's default sans-serif face,
 * with a built-in bitmap font when no face loads. Main thread only.
 *
 * File  > Save state, Load state, Exit
 * Audio > Volume slider over the whole mix (src/pc/audio/spu.c)
 * View  > Scale 1x-4x
 * Game  > Mods opens the searchable mod manager
 * Debug > development helpers (src/pc/debug) */

typedef struct MenuCanvas {
    uint32_t *pixels; /* 0xAARRGGBB (alpha ignored unless `alpha`), row-major */
    int stride;       /* pixels per row */
    int width, height;
    /* 0: the canvas already holds the picture and the menu paints over it.
     * 1: the canvas is a transparent overlay the platform blends over the
     * picture itself; Menu_Draw clears its bounds to transparent first. */
    int alpha;
} MenuCanvas;

typedef enum {
    MENU_EVENT_NONE,
    MENU_EVENT_BUTTON_DOWN, /* button: 1 left, 2 middle, 3 right; x, y */
    MENU_EVENT_BUTTON_UP,
    MENU_EVENT_MOTION,      /* x, y */
    MENU_EVENT_WHEEL,       /* wheel: +1 up, -1 down; x, y */
    MENU_EVENT_LEAVE,       /* the pointer left the window */
    MENU_EVENT_KEY_DOWN,    /* key */
    MENU_EVENT_KEY_UP,
    MENU_EVENT_TEXT
} MenuEventType;

typedef enum {
    MENU_KEY_OTHER, MENU_KEY_ESCAPE, MENU_KEY_F10, MENU_KEY_LEFT, MENU_KEY_RIGHT, MENU_KEY_UP, MENU_KEY_DOWN,
    MENU_KEY_ENTER, MENU_KEY_TAB, MENU_KEY_BACKSPACE
} MenuKey;

typedef struct MenuEvent {
    MenuEventType type;
    int x, y, button, wheel;
    MenuKey key;
    char text[32];
} MenuEvent;

typedef enum {
    MENU_ITEM_SCALE_1 = 100,
    MENU_ITEM_SCALE_2,
    MENU_ITEM_SCALE_3,
    MENU_ITEM_SCALE_4,
    MENU_ITEM_SCALE_5,
    MENU_ITEM_SCALE_6,
    MENU_ITEM_FULLSCREEN = 120,
    MENU_ITEM_BORDERLESS,
    MENU_ITEM_SCALING_INTEGER,
    MENU_ITEM_SCALING_FIT,
    MENU_ITEM_SCALING_STRETCH,
    MENU_ITEM_ASPECT_4_3,
    MENU_ITEM_ASPECT_SQUARE,
    MENU_ITEM_ASPECT_WIDESCREEN,
    MENU_ITEM_FILTER,
    MENU_ITEM_VSYNC,
    MENU_ITEM_TITLE, /* Debug > Jump to > Title Screen: enabled once Main_Loop runs */
    MENU_ITEM_DECKS  /* Game > Deck slots: enabled where the deck can change (deck_menu.c) */
} MenuItemId;

/* The stored settings (settings.txt in the user directory, see paths.h;
 * MEMORIES_SETTINGS elsewhere; MEMORIES_VOLUME and MEMORIES_SCALE override),
 * and the mods they say are applied. Applied whether or not a window opens,
 * so headless runs honour them too. */
void Menu_LoadSettings(void);
int Menu_Height(void);
/* Prepare fonts and layout; needs no display. */
void Menu_Init(void);
/* The menu's size multiple: bar, rows, font and HUD scale together. The
 * platform sets it from the setting, or from the window height when that is
 * 0 (Menu_AutoScale), before it sizes a window or lays one out. */
int Menu_Scale(void);
void Menu_SetScale(int scale);
int Menu_AutoScale(int window_h);
/* Draw the bar and, when open, its menu. */
void Menu_Draw(MenuCanvas *canvas);
void Menu_DrawText(MenuCanvas *canvas, int x, int y, const char *text, uint32_t colour);
int Menu_TextWidth(const char *text);
/* Auxiliary windows can fit their UI without changing the game menu scale. */
void Menu_DrawTextScaled(MenuCanvas *canvas, int x, int y, const char *text, uint32_t colour, int scale);
int Menu_TextWidthScaled(const char *text, int scale);
void Menu_SetVisible(int visible);
int Menu_IsOpen(void);
/* The rectangle the menu currently covers (the bar, plus an open menu). */
void Menu_Bounds(int *x, int *y, int *w, int *h);
/* Handle one event. Returns 1 when the menu took it (and must be redrawn)
 * and the game must not see it; sets *quit when File > Exit was chosen. */
int Menu_Event(const MenuEvent *event, int *quit);
/* Enable or disable an item by its backend-independent id. Disabled items
 * are dimmed, cannot be selected with the keyboard and ignore clicks. */
void Menu_SetItemEnabled(int id, int enabled);

/* Provided by the platform for the Video menu. */
int Platform_Scale(void);
void Platform_SetScale(int scale);
#endif
