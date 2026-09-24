/* The save slot menu. See save_menu.h. */
#include "save_menu.h"
#include "pc/guest/state.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { VIEW_CLOSED, VIEW_LIST, VIEW_CONFIRM, VIEW_MESSAGE };
enum { CHOICE_OVERWRITE, CHOICE_KEEP };
#define BACK_TO_LIST -1
#define MESSAGE_FRAMES 75 /* how long a success message stays without a button */

/* Everything a save state needs to carry an open menu across. The buffers
 * are guest addresses, the same in every build. */
static struct {
    int view, step, started, side;
    int cursor, top, choice;
    int after_message;  /* the outcome once the message closes, or BACK_TO_LIST */
    int message_waits;  /* 1: only a button closes it */
    unsigned message_frames;
    char message[160];
    unsigned char *buffer, *second;
    int size;
    int current_slot;   /* the slot loaded or saved last, -1 for none */
    int pair_slot[2];   /* the slots the two sides of a pair load came from */
    unsigned changes;
    SaveSlotInfo slots[SAVE_SLOT_COUNT];
} menu = {VIEW_CLOSED, 0, 0, 0, 0, 0, 0, 0, 0, 0, "", NULL, NULL, 0, -1, {-1, -1}, 0, {{0}}};

static SaveSlotCheck check;
static int shown_rows = SAVE_SLOT_COUNT; /* how many rows the last draw had room for */

static void changed(void) { menu.changes++; }

static int state_code(const unsigned char *state)
{
    return (int)(state[0x334] | state[0x335] << 8 | state[0x336] << 16 | (unsigned)state[0x337] << 24);
}

static unsigned state_sequence(const unsigned char *state)
{
    return state[0x404] | state[0x405] << 8 | state[0x406] << 16 | (unsigned)state[0x407] << 24;
}

/* Whether a used slot holds the game being saved as it was last loaded or
 * saved: the save being written carries the next sequence number. Anything
 * else of the same duelist is another point in their game, older or newer. */
static int same_game(const SaveSlotInfo *info)
{
    return info->status == SAVE_SLOT_USED && info->duelist_code == state_code(menu.buffer) &&
           info->sequence + 1 == state_sequence(menu.buffer);
}

static int selectable(int slot)
{
    const SaveSlotInfo *info = &menu.slots[slot];
    if (menu.step == SAVE_MENU_SAVE) return 1;
    if (info->status != SAVE_SLOT_USED) return 0;
    return !(menu.step == SAVE_MENU_LOAD_PAIR && menu.side == 1 && slot == menu.pair_slot[0]);
}

static void show_message(int after, int waits, const char *format, int slot)
{
    snprintf(menu.message, sizeof(menu.message), format, slot + 1);
    menu.view = VIEW_MESSAGE;
    menu.after_message = after;
    menu.message_waits = waits;
    menu.message_frames = 0;
    changed();
}

static int close_with(int outcome)
{
    menu.view = VIEW_CLOSED;
    changed();
    return outcome;
}

static void keep_cursor_shown(void)
{
    int rows = shown_rows < 1 ? 1 : shown_rows;
    if (menu.cursor < menu.top) menu.top = menu.cursor;
    if (menu.cursor >= menu.top + rows) menu.top = menu.cursor - rows + 1;
}

/* Where the cursor starts: the slot in use, else the newest save for a
 * load and the first empty slot for a save. */
static int first_cursor(void)
{
    int slot, best = -1;
    if (menu.current_slot >= 0 && selectable(menu.current_slot)) return menu.current_slot;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) {
        const SaveSlotInfo *info = &menu.slots[slot];
        if (!selectable(slot)) continue;
        if (menu.step == SAVE_MENU_SAVE) {
            if (info->status == SAVE_SLOT_EMPTY) return slot;
            continue;
        }
        if (best < 0 || info->saved_at > menu.slots[best].saved_at) best = slot;
    }
    return best < 0 ? 0 : best;
}

static int write_pair(void)
{
    static unsigned char states[2][SAVE_SLOT_STATE_SIZE], first[SAVE_SLOT_STATE_SIZE];
    int side;
    if (menu.size != 0x400 || menu.pair_slot[0] == menu.pair_slot[1]) return 2;
    /* Validate both destinations before changing either save. Build from
     * the sound copy so a recovered slot keeps its untouched progress. */
    for (side = 0; side < 2; side++) {
        int slot = menu.pair_slot[side];
        const unsigned char *record = side ? menu.second : menu.buffer;
        unsigned char *state = states[side];
        /* As the card dialog did: only write back over the same duelist. */
        if (slot < 0 || !record || SaveSlots_ReadState(slot, state, check) || state_code(state) != state_code(record)) {
            fprintf(stderr, "memories-pc: could not write the trade back to save slot %d\n", slot + 1);
            return 2;
        }
        if (side == 0) memcpy(first, state, sizeof(first));
        memcpy(state, record, (size_t)menu.size);
    }
    if (SaveSlots_WriteState(menu.pair_slot[0], states[0])) return 2;
    if (SaveSlots_WriteState(menu.pair_slot[1], states[1])) {
        /* Neither or both: the game drops the trade on a failure, so player
         * 1's save goes back to what it was. */
        if (SaveSlots_WriteState(menu.pair_slot[0], first))
            fprintf(stderr, "memories-pc: could not undo the trade in save slot %d\n", menu.pair_slot[0] + 1);
        return 2;
    }
    return 1;
}

static void start(int channel)
{
    int slot, any = 0;
    menu.started = 1;
    menu.side = channel >> 4 ? 1 : 0;
    if (menu.step == SAVE_MENU_LOAD_PAIR && menu.side == 0) menu.pair_slot[0] = menu.pair_slot[1] = -1;
    SaveSlots_Scan(menu.slots, check);
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) any |= selectable(slot);
    menu.cursor = first_cursor();
    menu.top = 0;
    keep_cursor_shown();
    menu.view = VIEW_LIST;
    changed();
    if (!any) show_message(2, 1, "There are no saved games to load.", 0);
}

static int load(int slot)
{
    if (menu.size != SAVE_SLOT_STATE_SIZE || SaveSlots_ReadState(slot, menu.buffer, check)) {
        show_message(BACK_TO_LIST, 1, "Slot %d could not be read.", slot);
        return 0;
    }
    if (menu.step == SAVE_MENU_LOAD_PAIR) menu.pair_slot[menu.side] = slot;
    else menu.current_slot = slot;
    return close_with(1);
}

static void save(int slot)
{
    /* The dialog's buffer is the state; the header the game built is the
     * 0x200 bytes before it (SaveData_RequestWrite). */
    if (SaveSlots_WriteFile(slot, menu.buffer - SAVE_SLOT_HEADER_SIZE, SAVE_SLOT_HEADER_SIZE + (size_t)menu.size)) {
        show_message(BACK_TO_LIST, 1, "Could not save to slot %d. Try another slot.", slot);
        return;
    }
    menu.current_slot = slot;
    SaveSlots_Scan(menu.slots, check);
    show_message(1, 0, "Saved to slot %d.", slot);
}

int SaveMenu_Begin(int step, unsigned char *buffer, unsigned char *second, int size, const char *name,
                   SaveSlotCheck validity)
{
    if (step != SAVE_MENU_LOAD && step != SAVE_MENU_LOAD_PAIR && step != SAVE_MENU_SAVE &&
        step != SAVE_MENU_WRITE_PAIR) {
        return 0;
    }
    SaveSlots_ImportMemoryCards(name);
    check = validity;
    menu.step = step;
    menu.buffer = buffer;
    menu.second = second;
    menu.size = size;
    menu.started = 0;
    menu.choice = CHOICE_KEEP;
    menu.view = VIEW_LIST; /* open; the first poll reads the slots */
    changed();
    return 1;
}

int SaveMenu_Active(void) { return menu.view != VIEW_CLOSED; }
int SaveMenu_CurrentSlot(void) { return menu.current_slot; }
int SaveMenu_PairSlot(int side) { return side == 0 || side == 1 ? menu.pair_slot[side] : -1; }

int SaveMenu_Poll(unsigned pressed, int channel, int *sound, SaveSlotCheck validity)
{
    int slot;
    check = validity;
    *sound = SAVE_MENU_SOUND_NONE;
    if (menu.view == VIEW_CLOSED) return 2;
    if (!menu.started) {
        if (menu.step == SAVE_MENU_WRITE_PAIR) {
            menu.started = 1;
            return close_with(write_pair());
        }
        start(channel);
        return 0;
    }
    switch (menu.view) {
    case VIEW_MESSAGE:
        menu.message_frames++;
        if (!(pressed & (SAVE_MENU_PAD_CONFIRM | SAVE_MENU_PAD_CANCEL | SAVE_MENU_PAD_START)) &&
            (menu.message_waits || menu.message_frames < MESSAGE_FRAMES)) {
            return 0;
        }
        if (pressed & (SAVE_MENU_PAD_CONFIRM | SAVE_MENU_PAD_CANCEL | SAVE_MENU_PAD_START)) {
            *sound = SAVE_MENU_SOUND_CONFIRM;
        }
        if (menu.after_message != BACK_TO_LIST) return close_with(menu.after_message);
        menu.view = VIEW_LIST;
        changed();
        return 0;
    case VIEW_CONFIRM:
        if (pressed & (SAVE_MENU_PAD_LEFT | SAVE_MENU_PAD_RIGHT)) {
            menu.choice ^= 1;
            *sound = SAVE_MENU_SOUND_MOVE;
            changed();
        } else if (pressed & SAVE_MENU_PAD_CANCEL) {
            *sound = SAVE_MENU_SOUND_CANCEL;
            menu.view = VIEW_LIST;
            changed();
        } else if (pressed & (SAVE_MENU_PAD_CONFIRM | SAVE_MENU_PAD_START)) {
            if (menu.choice == CHOICE_OVERWRITE) {
                *sound = SAVE_MENU_SOUND_CONFIRM;
                save(menu.cursor);
            } else {
                *sound = SAVE_MENU_SOUND_CANCEL;
                menu.view = VIEW_LIST;
                changed();
            }
        }
        return 0;
    default:
        break;
    }
    if (pressed & (SAVE_MENU_PAD_UP | SAVE_MENU_PAD_DOWN)) {
        menu.cursor = (menu.cursor + (pressed & SAVE_MENU_PAD_UP ? SAVE_SLOT_COUNT - 1 : 1)) % SAVE_SLOT_COUNT;
        keep_cursor_shown();
        *sound = SAVE_MENU_SOUND_MOVE;
        changed();
        return 0;
    }
    if (pressed & SAVE_MENU_PAD_CANCEL) {
        *sound = SAVE_MENU_SOUND_CANCEL;
        return close_with(3);
    }
    if (!(pressed & (SAVE_MENU_PAD_CONFIRM | SAVE_MENU_PAD_START))) return 0;
    slot = menu.cursor;
    /* The files, not the list as it was drawn: a save state may have brought
     * the list back from before a slot was written. */
    SaveSlots_Scan(menu.slots, check);
    changed();
    if (!selectable(slot)) {
        *sound = SAVE_MENU_SOUND_BUZZER;
        return 0;
    }
    *sound = SAVE_MENU_SOUND_CONFIRM;
    if (menu.step != SAVE_MENU_SAVE) return load(slot);
    if (menu.slots[slot].status == SAVE_SLOT_EMPTY) {
        save(slot);
        return 0;
    }
    /* Overwriting the game you are playing is the usual case; another
     * duelist's save, an older or newer point of this one, or a damaged
     * save is kept unless the player moves to Overwrite. */
    menu.choice = same_game(&menu.slots[slot]) ? CHOICE_OVERWRITE : CHOICE_KEEP;
    menu.view = VIEW_CONFIRM;
    changed();
    return 0;
}

unsigned SaveMenu_Signature(void)
{
    return menu.view == VIEW_CLOSED ? 0 : menu.changes * 8u + (unsigned)menu.view * 2u + 1u;
}

/* Drawing. */

#define COLOUR_TEXT 0xf2f2f4u
#define COLOUR_DIM 0x8a8a92u
#define COLOUR_WARN 0xf0a070u
#define COLOUR_TITLE 0xffd870u

static uint32_t blend(uint32_t under, uint32_t over, unsigned alpha)
{
    unsigned inverse = 255 - alpha;
    unsigned r = ((over >> 16 & 255) * alpha + (under >> 16 & 255) * inverse) / 255;
    unsigned g = ((over >> 8 & 255) * alpha + (under >> 8 & 255) * inverse) / 255;
    unsigned b = ((over & 255) * alpha + (under & 255) * inverse) / 255;
    unsigned a = alpha + (under >> 24) * inverse / 255;
    return a << 24 | r << 16 | g << 8 | b;
}

static void fill(MenuCanvas *canvas, int x, int y, int w, int h, uint32_t colour, unsigned alpha)
{
    int row, column;
    for (row = y < 0 ? 0 : y; row < y + h && row < canvas->height; row++) {
        for (column = x < 0 ? 0 : x; column < x + w && column < canvas->width; column++) {
            uint32_t *pixel = canvas->pixels + (size_t)row * (size_t)canvas->stride + (size_t)column;
            *pixel = blend(*pixel, colour, alpha);
        }
    }
}

static void frame(MenuCanvas *canvas, int x, int y, int w, int h, int s)
{
    fill(canvas, x, y, w, h, 0x0c0e18u, 232);
    fill(canvas, x, y, w, s, 0x6078c0u, 255);
    fill(canvas, x, y + h - s, w, s, 0x6078c0u, 255);
    fill(canvas, x, y, s, h, 0x6078c0u, 255);
    fill(canvas, x + w - s, y, s, h, 0x6078c0u, 255);
}

/* The menu's own scale: at least the menu bar's, and big enough to read over
 * the game picture in a large window. */
static int ui_scale;

static void draw_text(MenuCanvas *canvas, int x, int y, const char *text, uint32_t colour)
{
    Menu_DrawTextScaled(canvas, x, y, text, colour, ui_scale);
}

static int text_width(const char *text) { return Menu_TextWidthScaled(text, ui_scale); }

static void centred(MenuCanvas *canvas, int x, int w, int y, const char *text, uint32_t colour)
{
    draw_text(canvas, x + (w - text_width(text)) / 2, y, text, colour);
}

static void describe(const SaveSlotInfo *info, char *out, size_t size)
{
    char when[32] = "";
    time_t seconds = (time_t)info->saved_at;
    struct tm *local = info->saved_at ? localtime(&seconds) : NULL;
    if (local) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", local);
    snprintf(out, size, "%u starchips   %d cards   W%d L%d   %s", info->starchips, info->cards, info->wins,
             info->losses, when);
}

static const char *title(void)
{
    if (menu.step == SAVE_MENU_SAVE) return "Save game";
    if (menu.step == SAVE_MENU_LOAD_PAIR) return menu.side ? "Player 2: choose a save" : "Player 1: choose a save";
    return "Load game";
}

static void draw_confirm(MenuCanvas *canvas, int px, int pw, int cy, int s)
{
    const SaveSlotInfo *info = &menu.slots[menu.cursor];
    char line[160], details[128];
    int w = pw - 48 * s, h = 132 * s, x = px + 24 * s, y = cy - h / 2, bx, bw = 120 * s;
    const char *labels[2] = {"Overwrite", "Cancel"};
    int i;
    frame(canvas, x, y, w, h, s);
    snprintf(line, sizeof(line), "Slot %d already has a save.", menu.cursor + 1);
    centred(canvas, x, w, y + 18 * s, line, COLOUR_TEXT);
    if (info->status == SAVE_SLOT_USED) {
        describe(info, details, sizeof(details));
        snprintf(line, sizeof(line), "%s   %s", info->name[0] ? info->name : "(no name)", details);
        centred(canvas, x, w, y + 38 * s, line, COLOUR_DIM);
        if (info->duelist_code != state_code(menu.buffer)) {
            centred(canvas, x, w, y + 58 * s, "It belongs to a different duelist.", COLOUR_WARN);
        } else if (!same_game(info)) {
            centred(canvas, x, w, y + 58 * s,
                    info->sequence + 1 > state_sequence(menu.buffer) ? "It is further along than the game you are saving."
                                                                     : "It is an earlier save of this game.",
                    COLOUR_WARN);
        }
    } else {
        centred(canvas, x, w, y + 38 * s, "The save in it is damaged.", COLOUR_WARN);
    }
    centred(canvas, x, w, y + 80 * s, "Overwrite it?", COLOUR_TEXT);
    bx = x + (w - 2 * bw - 16 * s) / 2;
    for (i = 0; i < 2; i++) {
        int chosen = i == menu.choice;
        int left = bx + i * (bw + 16 * s);
        fill(canvas, left, y + 96 * s, bw, 24 * s, chosen ? 0x3a5aa8u : 0x22263au, 255);
        centred(canvas, left, bw, y + 108 * s, labels[i], chosen ? COLOUR_TEXT : COLOUR_DIM);
    }
}

void SaveMenu_Draw(MenuCanvas *canvas, int *x, int *y, int *w, int *h)
{
    int s, row_h, rows, pw, ph, px, py, top, i, list_y;
    char line[160];
    *x = *y = *w = *h = 0;
    if (menu.view == VIEW_CLOSED || !menu.started || !canvas || !canvas->pixels) return;
    s = canvas->height / 420 > Menu_Scale() ? canvas->height / 420 : Menu_Scale();
    ui_scale = s;
    row_h = 22 * s;
    rows = (canvas->height - Menu_Height() - 96 * s) / row_h;
    rows = rows < 3 ? 3 : rows > SAVE_SLOT_COUNT ? SAVE_SLOT_COUNT : rows;
    if (rows != shown_rows) {
        shown_rows = rows;
        keep_cursor_shown();
    }
    pw = canvas->width - 16 * s < 600 * s ? canvas->width - 16 * s : 600 * s;
    ph = 44 * s + rows * row_h + 34 * s;
    px = (canvas->width - pw) / 2;
    py = Menu_Height() + (canvas->height - Menu_Height() - ph) / 2;
    frame(canvas, px, py, pw, ph, s);
    draw_text(canvas, px + 14 * s, py + 20 * s, title(), COLOUR_TITLE);
    list_y = py + 38 * s;
    top = menu.top;
    for (i = 0; i < rows && top + i < SAVE_SLOT_COUNT; i++) {
        int slot = top + i, ry = list_y + i * row_h, cy = ry + row_h / 2;
        const SaveSlotInfo *info = &menu.slots[slot];
        uint32_t colour = selectable(slot) ? COLOUR_TEXT : COLOUR_DIM;
        const char *right;
        char details[128];
        if (slot == menu.cursor) fill(canvas, px + 6 * s, ry, pw - 12 * s, row_h - 2 * s, 0x3a5aa8u, 200);
        if (info->status == SAVE_SLOT_USED) {
            snprintf(line, sizeof(line), "%2d   %s%s%s", slot + 1, info->name[0] ? info->name : "(no name)",
                     slot == menu.current_slot ? "  (current)" : "",
                     menu.step == SAVE_MENU_LOAD_PAIR && menu.side == 1 && slot == menu.pair_slot[0]
                         ? "  (player 1)" : "");
            describe(info, details, sizeof(details));
            right = details;
        } else {
            snprintf(line, sizeof(line), "%2d", slot + 1);
            right = info->status == SAVE_SLOT_EMPTY ? "Empty" : "Damaged save";
        }
        draw_text(canvas, px + 16 * s, cy, line, colour);
        draw_text(canvas, px + pw - 16 * s - text_width(right), cy, right,
                      info->status == SAVE_SLOT_DAMAGED ? COLOUR_WARN : colour);
    }
    if (menu.top > 0) draw_text(canvas, px + pw - 30 * s, py + 20 * s, "^", COLOUR_DIM);
    if (menu.top + rows < SAVE_SLOT_COUNT) draw_text(canvas, px + pw - 18 * s, py + 20 * s, "v", COLOUR_DIM);
    draw_text(canvas, px + 14 * s, py + ph - 16 * s,
                  menu.step == SAVE_MENU_SAVE ? "Cross: save here    Circle: back" : "Cross: load    Circle: back",
                  COLOUR_DIM);
    if (menu.view == VIEW_CONFIRM) {
        draw_confirm(canvas, px, pw, py + ph / 2, s);
    } else if (menu.view == VIEW_MESSAGE) {
        int mw = pw - 96 * s, mh = 64 * s, mx = px + 48 * s, my = py + (ph - mh) / 2;
        frame(canvas, mx, my, mw, mh, s);
        centred(canvas, mx, mw, my + 24 * s, menu.message, COLOUR_TEXT);
        if (menu.message_waits) centred(canvas, mx, mw, my + 46 * s, "Press Cross", COLOUR_DIM);
    }
    *x = px;
    *y = py;
    *w = pw;
    *h = ph;
}

void SaveMenu_State(MemoriesState *state)
{
    MemoriesStateField fields[] = {{&menu, sizeof(menu)}};
    if (Memories_StateChunk(state, "save-menu", fields, 1) && menu.view == VIEW_CONFIRM) {
        /* The slot may have changed since this prompt was saved. Return
         * to the list so the next pick reads disk and asks afresh, using
         * the validity callback supplied by the current caller. */
        menu.view = VIEW_LIST;
        menu.choice = CHOICE_KEEP;
        changed();
    }
}
