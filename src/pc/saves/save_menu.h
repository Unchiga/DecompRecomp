#ifndef MEMORIES_PC_SAVE_MENU_H
#define MEMORIES_PC_SAVE_MENU_H
/* The save slot menu, which replaces the memory card dialog.
 *
 * The game asks for a load or a save through MemCardDialog_Request and then
 * calls MemCardDialog_Poll every frame until it returns the outcome
 * (src/game/mem_card_dialog_runtime.c). On the port those two hand the
 * request to this menu instead: the player picks a slot with the pad, is
 * asked before an existing save is overwritten, and the poll returns what
 * the card dialog would have: 1 done, 2 failed, 3 cancelled.
 *
 * The menu is drawn in the port's overlay (hud.c calls SaveMenu_Draw), so
 * it needs no game text or textures and reads the same on every backend.
 * Everything runs on the game thread, which is also the one that draws. */
#include "save_slots.h"
#include "pc/platform/menu.h"

/* The dialog steps of the retail table D_80090F9C. */
enum {
    SAVE_MENU_LOAD = 0,            /* load the player's save */
    SAVE_MENU_LOAD_PAIR = 1,       /* load one side of a two-player trade or duel */
    SAVE_MENU_SAVE = 2,            /* save the running game */
    SAVE_MENU_WRITE_PAIR = 4       /* write both traded saves back */
};

/* Pad bits as the game reads them (src/game/input.h). */
enum {
    SAVE_MENU_PAD_CANCEL = 0x20,   /* circle */
    SAVE_MENU_PAD_CONFIRM = 0x40,  /* cross */
    SAVE_MENU_PAD_START = 0x800,
    SAVE_MENU_PAD_UP = 0x1000,
    SAVE_MENU_PAD_RIGHT = 0x2000,
    SAVE_MENU_PAD_DOWN = 0x4000,
    SAVE_MENU_PAD_LEFT = 0x8000
};

/* Sounds the caller plays for the menu, as SD_SEPlayFull ids. */
enum { SAVE_MENU_SOUND_NONE = 0, SAVE_MENU_SOUND_MOVE = 6, SAVE_MENU_SOUND_CONFIRM = 7,
       SAVE_MENU_SOUND_CANCEL = 8, SAVE_MENU_SOUND_BUZZER = 9 };

/* Take a dialog request. `buffer` is the dialog's transfer buffer (for a
 * save, the header sits in the 0x200 bytes before it), `second` the second
 * trade record, `size` the bytes the dialog would move and `name` the
 * memory card file name. Returns 1 when the menu took the request, 0 when
 * the step is not one it handles. */
int SaveMenu_Begin(int step, unsigned char *buffer, unsigned char *second, int size, const char *name,
                   SaveSlotCheck check);
int SaveMenu_Active(void);
/* The slot loaded or saved last, and the slot each side of the last pair
 * load came from; -1 for none. */
int SaveMenu_CurrentSlot(void);
int SaveMenu_PairSlot(int side);
/* One frame. `pressed` holds the buttons pressed this frame (with the
 * directions' auto-repeat), `channel` the dialog's card channel byte, which
 * tells the two sides of a pair load apart (0x10 is the second). Returns 0
 * while the menu is open and then the outcome. *sound is set to the sound
 * to play this frame. */
/* Supply the running build's callback on every poll: an open menu can be
 * restored in a new process without passing through SaveMenu_Begin. */
int SaveMenu_Poll(unsigned pressed, int channel, int *sound, SaveSlotCheck validity);

/* The overlay: draw over `canvas` and report the rectangle drawn. */
void SaveMenu_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h);
/* Changes whenever what SaveMenu_Draw would draw changes. */
unsigned SaveMenu_Signature(void);

#endif
