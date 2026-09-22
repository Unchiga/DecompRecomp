#ifndef MEMORIES_CONTROLS_WINDOW_H
#define MEMORIES_CONTROLS_WINDOW_H
#include "controls_runtime.h"
#include "menu.h"
/* Widget identifiers. The window uses them internally for hit-testing and
 * keyboard focus; tests and scripted input use them to address a control
 * without knowing the layout. Binding cells are
 * CONTROLS_UI_BINDING + destination * 2 + slot, pad picture buttons are
 * CONTROLS_UI_PAD + destination, and device-list entries are
 * CONTROLS_UI_CHOICE + entry (0 automatic, 1 none, 2 + device index). */
enum {
    CONTROLS_UI_PLAYER_1 = 1,
    CONTROLS_UI_PLAYER_2,
    CONTROLS_UI_DEVICE,
    CONTROLS_UI_KEYBOARD,
    CONTROLS_UI_CONTROLLER,
    CONTROLS_UI_REBIND,
    CONTROLS_UI_CLEAR,
    CONTROLS_UI_DEFAULTS,
    CONTROLS_UI_CANCEL,
    CONTROLS_UI_APPLY,
    CONTROLS_UI_OK,
    CONTROLS_UI_YES,
    CONTROLS_UI_NO,
    CONTROLS_UI_KEEP,
    CONTROLS_UI_BINDING = 100,
    CONTROLS_UI_PAD = 200,
    CONTROLS_UI_CHOICE = 500
};
void ControlsWindow_Init(void);
void ControlsWindow_Size(int *w, int *h);
/* The smallest size the layout still works at, in physical pixels. */
void ControlsWindow_MinSize(int *w, int *h);
void ControlsWindow_Draw(MenuCanvas *canvas);
/* Centre of a widget in canvas pixels, as the last draw placed it. Returns 0
 * when that widget is not on screen. */
int ControlsWindow_Locate(int id, int *x, int *y);
void ControlsWindow_Event(const MenuEvent *event);
void ControlsWindow_Key(int key, int down, int repeat, int modifiers);
void ControlsWindow_Tick(void);
void ControlsWindow_RequestClose(void);
void ControlsWindow_FocusLost(void);
int ControlsWindow_ShouldClose(void);
#endif
