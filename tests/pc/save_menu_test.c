#define _POSIX_C_SOURCE 200809L
#include "pc/saves/save_menu.h"
#include "pc/guest/state.h"
#include "pc/compat/posix.h"
#include "scratch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME "BASLUS-01411-YUGIOH"

/* Exercise the menu's real file I/O and input state machine without a window. */
int Menu_Scale(void) { return 1; }
int Menu_Height(void) { return 24; }
int Menu_TextWidthScaled(const char *text, int scale) { return (int)strlen(text) * 6 * scale; }
void Menu_DrawTextScaled(MenuCanvas *canvas, int x, int y, const char *text, uint32_t colour, int scale)
{
    (void)canvas; (void)x; (void)y; (void)text; (void)colour; (void)scale;
}

struct MemoriesState { int loading; unsigned char chunk[4096]; size_t size; };
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count)
{
    assert(!strcmp(tag, "save-menu") && count == 1 && fields[0].size <= sizeof(state->chunk));
    if (state->loading) {
        assert(state->size == fields[0].size);
        memcpy(fields[0].data, state->chunk, state->size);
    } else {
        state->size = fields[0].size;
        memcpy(state->chunk, fields[0].data, state->size);
    }
    return state->loading;
}

static int sound(unsigned char *state) { return state[SAVE_SLOT_STATE_SIZE - 1] == 0x5A; }
static int poll(unsigned pressed, int channel)
{
    int effect;
    return SaveMenu_Poll(pressed, channel, &effect, sound);
}
static void begin(int step, unsigned char *buffer, unsigned char *second, int size, int channel)
{
    assert(SaveMenu_Begin(step, buffer, second, size, NAME, sound));
    assert(SaveMenu_Active());
    assert(!poll(0, channel));
}
static void make_image(unsigned char *image, int code)
{
    unsigned char *state = image + SAVE_SLOT_HEADER_SIZE;
    memset(image, 0, SAVE_SLOT_FILE_SIZE);
    image[0] = 'S'; image[1] = 'C';
    state[0x334] = (unsigned char)code;
    state[0x50] = 4;
    state[0x5E0] = 99;
    state[SAVE_SLOT_STATE_SIZE - 1] = 0x5A;
    memcpy(image + SAVE_SLOT_DUPLICATE_OFFSET, state, SAVE_SLOT_STATE_SIZE);
}
static void read_image(int slot, unsigned char *image)
{
    char path[1024];
    FILE *file;
    assert(!SaveSlots_Path(slot, path, sizeof(path)));
    file = fopen(path, "rb");
    assert(file);
    assert(fread(image, 1, SAVE_SLOT_FILE_SIZE, file) == SAVE_SLOT_FILE_SIZE);
    assert(!fclose(file));
}

int main(void)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE], before[SAVE_SLOT_FILE_SIZE];
    static unsigned char left[SAVE_SLOT_STATE_SIZE], right[SAVE_SLOT_STATE_SIZE];
    MemoriesState snapshot = {0};
    SaveSlotInfo slots[SAVE_SLOT_COUNT];
    char directory[SCRATCH_MAX], path[1024];
    int i;
    scratch_template(directory, sizeof(directory), "memories-save-menu");
    assert(mkdtemp(directory));
    assert(!setenv("MEMORIES_USER_DIR", directory, 1));
    /* Keep a developer's configured legacy cards out of this test. */
    snprintf(path, sizeof(path), "%s/missing.mcd", directory);
    assert(!setenv("MEMORIES_MEMCARD1", path, 1));
    assert(!setenv("MEMORIES_MEMCARD2", path, 1));

    begin(SAVE_MENU_LOAD, left, NULL, sizeof(left), 0);
    assert(poll(SAVE_MENU_PAD_CONFIRM, 0) == 2); /* no saves */
    assert(!SaveMenu_Active());

    /* Save an empty slot, then cancel an existing-slot overwrite. */
    make_image(image, 1);
    begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
    assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));
    assert(poll(SAVE_MENU_PAD_CONFIRM, 0) == 1);
    read_image(0, before);
    /* The game's bytes, then the port's token in the padding. */
    assert(!memcmp(image, before, SAVE_SLOT_TAG_OFFSET) && SaveSlots_Token(0));
    begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
    assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));
    assert(!poll(SAVE_MENU_PAD_CANCEL, 0));
    assert(poll(SAVE_MENU_PAD_CANCEL, 0) == 3);
    read_image(0, image);
    assert(!memcmp(image, before, sizeof(image)));

    /* Another duelist's save defaults to Cancel. */
    make_image(image, 2);
    begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
    assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));
    assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));
    read_image(0, image);
    assert(!memcmp(image, before, sizeof(image)));
    assert(poll(SAVE_MENU_PAD_CANCEL, 0) == 3);

    /* A restored menu must get its callback from the current caller. Begin
     * with NULL simulates a new process's uninitialized native callback;
     * restoring the menu chunk, as the loader does, does not restore it. */
    begin(SAVE_MENU_LOAD, left, NULL, sizeof(left), 0);
    SaveMenu_State(&snapshot);
    assert(SaveMenu_Begin(SAVE_MENU_LOAD, left, NULL, sizeof(left), NAME, NULL));
    snapshot.loading = 1;
    SaveMenu_State(&snapshot);
    assert(poll(SAVE_MENU_PAD_CONFIRM, 0) == 1);
    assert(left[0x334] == 1);

    /* Player 2 cannot load Player 1's slot. */
    make_image(image, 2);
    assert(!SaveSlots_WriteFile(1, image, sizeof(image)));
    begin(SAVE_MENU_LOAD_PAIR, left, NULL, sizeof(left), 0);
    assert(poll(SAVE_MENU_PAD_CONFIRM, 0) == 1);
    begin(SAVE_MENU_LOAD_PAIR, right, NULL, sizeof(right), 0x10);
    assert(!poll(SAVE_MENU_PAD_UP, 0x10));
    assert(!poll(SAVE_MENU_PAD_CONFIRM, 0x10));
    assert(SaveMenu_Active());
    assert(!poll(SAVE_MENU_PAD_DOWN, 0x10));
    assert(poll(SAVE_MENU_PAD_CONFIRM, 0x10) == 1);
    assert(left[0x334] == 1 && right[0x334] == 2);
    left[0x50] = 3; right[0x50] = 5;

    /* A changed second destination must reject the trade before writing
     * Player 1's save, even though Player 1 still matches. */
    make_image(image, 3);
    assert(!SaveSlots_WriteFile(1, image, sizeof(image)));
    assert(SaveMenu_Begin(SAVE_MENU_WRITE_PAIR, left, right, 0x400, NAME, sound));
    assert(poll(0, 0) == 2);
    read_image(0, image);
    assert(!memcmp(image, before, sizeof(image)));

    /* Trade into a recovered slot: preserve the valid copy's progress and
     * replace both copies, so later backup recovery cannot undo the trade. */
    make_image(image, 2);
    image[SAVE_SLOT_HEADER_SIZE + 0x5E0] = 0;
    image[SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE - 1] = 0;
    assert(!SaveSlots_WriteFile(1, image, sizeof(image)));
    assert(SaveMenu_Begin(SAVE_MENU_WRITE_PAIR, left, right, 0x400, NAME, sound));
    assert(poll(0, 0) == 1);
    for (i = 0; i < 2; i++) {
        read_image(i, image);
        assert(image[0] == 'S' && image[1] == 'C');
        assert(image[SAVE_SLOT_HEADER_SIZE + 0x50] == (i ? 5 : 3));
        assert(image[SAVE_SLOT_HEADER_SIZE + 0x5E0] == 99);
        assert(!memcmp(image + SAVE_SLOT_HEADER_SIZE, image + SAVE_SLOT_DUPLICATE_OFFSET, SAVE_SLOT_STATE_SIZE));
        image[SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE - 1] = 0;
        assert(!SaveSlots_WriteFile(i, image, sizeof(image)));
    }
    SaveSlots_Scan(slots, sound);
    assert(slots[0].from_duplicate && slots[1].from_duplicate);
    assert(!SaveSlots_ReadState(0, left, sound) && left[0x50] == 3);
    assert(!SaveSlots_ReadState(1, right, sound) && right[0x50] == 5);

    /* Saving the game in play over its own slot defaults to Overwrite and
     * draws a new token; over an earlier point of it, to Cancel. */
    {
        unsigned token = SaveSlots_Token(0);
        make_image(image, 1);
        image[SAVE_SLOT_HEADER_SIZE + 0x404] = 7;
        assert(!SaveSlots_WriteFile(0, image, sizeof(image)));
        image[SAVE_SLOT_HEADER_SIZE + 0x404] = 8;   /* the next save of that game */
        token = SaveSlots_Token(0);
        /* The cursor starts on the slot in use, slot 1. */
        begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
        assert(SaveMenu_CurrentSlot() == 0);
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* the question */
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* Overwrite: saved, message shown */
        read_image(0, image);
        assert(image[SAVE_SLOT_HEADER_SIZE + 0x404] == 8 && SaveSlots_Token(0) != token);
        image[SAVE_SLOT_HEADER_SIZE + 0x404] = 6;   /* an older point than slot 1 holds */
        begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* Cancel is chosen */
        read_image(0, before);
        assert(before[SAVE_SLOT_HEADER_SIZE + 0x404] == 8);
        assert(poll(SAVE_MENU_PAD_CANCEL, 0) == 3);
    }

    /* A save state from before a slot was written must not save over it
     * without asking. */
    {
        int empty = 2;
        make_image(image, 1);
        begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
        assert(!poll(SAVE_MENU_PAD_DOWN, 0) && !poll(SAVE_MENU_PAD_DOWN, 0));
        snapshot.loading = 0;
        SaveMenu_State(&snapshot);                 /* slot 3 shows Empty */
        make_image(before, 4);
        assert(!SaveSlots_WriteFile(empty, before, sizeof(before)));
        snapshot.loading = 1;
        SaveMenu_State(&snapshot);
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* asks */
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* another duelist's: Cancel */
        read_image(empty, image);
        assert(image[SAVE_SLOT_HEADER_SIZE + 0x334] == 4);
        assert(poll(SAVE_MENU_PAD_CANCEL, 0) == 3);
        snapshot.loading = 0;
    }

    /* Restoring an Overwrite prompt cannot authorize replacing a save
     * written after that prompt was captured, even for the same duelist. */
    {
        make_image(image, 1);
        image[SAVE_SLOT_HEADER_SIZE + 0x404] = 7;
        assert(!SaveSlots_WriteFile(0, image, sizeof(image)));
        image[SAVE_SLOT_HEADER_SIZE + 0x404] = 8;
        begin(SAVE_MENU_SAVE, image + SAVE_SLOT_HEADER_SIZE, NULL, 2 * SAVE_SLOT_STATE_SIZE, 0);
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* Overwrite is selected */
        SaveMenu_State(&snapshot);
        make_image(before, 1);
        before[SAVE_SLOT_HEADER_SIZE + 0x404] = 9;
        assert(!SaveSlots_WriteFile(0, before, sizeof(before)));
        read_image(0, before);
        snapshot.loading = 1;
        SaveMenu_State(&snapshot);
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* ask again using disk */
        assert(!poll(SAVE_MENU_PAD_CONFIRM, 0));   /* newer save: Cancel */
        read_image(0, image);
        assert(!memcmp(image, before, sizeof(image)));
        assert(poll(SAVE_MENU_PAD_CANCEL, 0) == 3);
        snapshot.loading = 0;
    }

#ifndef _WIN32
    /* A trade whose second write fails puts player 1's save back. */
    {
        char partial[1100];
        assert(!SaveSlots_Path(2, path, sizeof(path)));
        assert(!remove(path));
        make_image(image, 1);
        assert(!SaveSlots_WriteFile(0, image, sizeof(image)));
        make_image(image, 2);
        assert(!SaveSlots_WriteFile(1, image, sizeof(image)));
        begin(SAVE_MENU_LOAD_PAIR, left, NULL, sizeof(left), 0);
        while (SaveMenu_Active()) {
            if (poll(SAVE_MENU_PAD_CONFIRM, 0)) break;
        }
        begin(SAVE_MENU_LOAD_PAIR, right, NULL, sizeof(right), 0x10);
        assert(poll(SAVE_MENU_PAD_CONFIRM, 0x10) == 1);   /* the only other save */
        assert(left[0x334] == 1 && right[0x334] == 2);
        left[0x50] = 9; right[0x50] = 9;
        assert(!SaveSlots_Path(1, path, sizeof(path)));
        snprintf(partial, sizeof(partial), "%s.partial", path);
        assert(!symlink("/dev/full", partial));
        assert(SaveMenu_Begin(SAVE_MENU_WRITE_PAIR, left, right, 0x400, NAME, sound));
        assert(poll(0, 0) == 2);
        remove(partial);
        assert(!SaveSlots_ReadState(0, left, sound) && left[0x50] == 4);
        assert(!SaveSlots_ReadState(1, right, sound) && right[0x50] == 4);
    }
#endif

    for (i = 0; i < SAVE_SLOT_COUNT; i++) {
        assert(!SaveSlots_Path(i, path, sizeof(path)));
        remove(path);
    }
    snprintf(path, sizeof(path), "%s/saves/.cards-imported", directory);
    remove(path);
    snprintf(path, sizeof(path), "%s/saves", directory);
    assert(!rmdir(path));
    assert(!rmdir(directory));
    puts("save menu: ok");
    return 0;
}
