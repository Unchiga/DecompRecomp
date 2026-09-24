#define _POSIX_C_SOURCE 200809L
#include "pc/saves/save_slots.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include "pc/compat/posix.h"
#include "scratch.h"
#include <unistd.h>

#define NAME "BASLUS-01411-YUGIOH"

/* Stands in for SaveData_ValidateIntegrity: a state is sound when its last
 * byte is 0x5A. */
static int sound(unsigned char *state) { return state[SAVE_SLOT_STATE_SIZE - 1] == 0x5A; }

static void make_state(unsigned char *state, int code, unsigned starchips)
{
    memset(state, 0, SAVE_SLOT_STATE_SIZE);
    state[0] = 1;                      /* one deck card */
    state[0x50] = 3;                   /* three of card 1 */
    state[0x51] = 2;
    memcpy(state + 0x334, &code, 4);
    state[0x404] = 7;                  /* sequence */
    state[0x40C] = 0x60; state[0x40D] = 0x82; /* full-width A */
    state[0x40E] = 0x4F; state[0x40F] = 0x82; /* full-width 0 */
    state[0x518] = 5;                  /* wins */
    state[0x51A] = 2;                  /* losses */
    memcpy(state + 0x5E0, &starchips, 4);
    state[SAVE_SLOT_STATE_SIZE - 1] = 0x5A;
}

static void make_image(unsigned char *image, int code, unsigned starchips)
{
    memset(image, 0, SAVE_SLOT_FILE_SIZE);
    image[0] = 'S';
    image[1] = 'C';
    make_state(image + SAVE_SLOT_HEADER_SIZE, code, starchips);
    make_state(image + SAVE_SLOT_DUPLICATE_OFFSET, code, starchips);
}

/* A formatted card image holding the save in block 3. */
static void write_card(const char *path, const unsigned char *save)
{
    static unsigned char card[0x20000];
    unsigned char *frame = card + 128 * (3 + 1);
    FILE *file;
    card[0] = 'M';
    card[1] = 'C';
    frame[0] = 0x51;
    frame[8] = frame[9] = 0xff;
    strcpy((char *)frame + 10, NAME);
    memcpy(card + 0x2000 * (3 + 1), save, SAVE_SLOT_FILE_SIZE);
    file = fopen(path, "wb");
    assert(file);
    assert(fwrite(card, 1, sizeof(card), file) == sizeof(card));
    assert(!fclose(file));
}

int main(void)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE], state[SAVE_SLOT_STATE_SIZE];
    SaveSlotInfo slots[SAVE_SLOT_COUNT];
    char directory[SCRATCH_MAX], card[SCRATCH_MAX + 32], path[1024];
    int i;
    scratch_template(directory, sizeof(directory), "memories-save-slots");
    assert(mkdtemp(directory));
    assert(!setenv("MEMORIES_USER_DIR", directory, 1));
    assert(!unsetenv("MEMORIES_MEMCARD1"));
    assert(!unsetenv("MEMORIES_MEMCARD2"));

    /* First run: the save on memcard1.mcd becomes slot 1, and the card stays. */
    make_image(image, 0x1234, 4);
    snprintf(card, sizeof(card), "%s/memcard1.mcd", directory);
    write_card(card, image);
    SaveSlots_ImportMemoryCards(NAME);
    SaveSlots_Scan(slots, sound);
    assert(slots[0].status == SAVE_SLOT_USED);
    assert(slots[0].duelist_code == 0x1234);
    assert(slots[0].starchips == 4);
    assert(slots[0].wins == 5 && slots[0].losses == 2);
    assert(slots[0].cards == 6);
    assert(slots[0].sequence == 7);
    assert(!strcmp(slots[0].name, "A0"));
    assert(!slots[0].from_duplicate);
    for (i = 1; i < SAVE_SLOT_COUNT; i++) assert(slots[i].status == SAVE_SLOT_EMPTY);
    assert(!access(card, F_OK));

    /* Once only: a slot the player cleared is not filled again. */
    assert(!SaveSlots_Path(0, path, sizeof(path)));
    assert(!remove(path));
    SaveSlots_ImportMemoryCards(NAME);
    SaveSlots_Scan(slots, sound);
    assert(slots[0].status == SAVE_SLOT_EMPTY);

    /* A save written as header + both copies reads back whole, padded. */
    make_image(image, 0x5678, 900);
    assert(!SaveSlots_WriteFile(4, image, SAVE_SLOT_HEADER_SIZE + 2 * SAVE_SLOT_STATE_SIZE));
    assert(!SaveSlots_ReadState(4, state, sound));
    assert(!memcmp(state, image + SAVE_SLOT_HEADER_SIZE, SAVE_SLOT_STATE_SIZE));
    SaveSlots_Scan(slots, sound);
    assert(slots[4].status == SAVE_SLOT_USED && slots[4].starchips == 900);

    /* A damaged first copy falls back to the duplicate. */
    image[SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE - 1] = 0;
    memset(image + SAVE_SLOT_DUPLICATE_OFFSET + 0x5E0, 0, 4);
    image[SAVE_SLOT_DUPLICATE_OFFSET + 0x5E0] = 77;
    assert(!SaveSlots_WriteFile(5, image, SAVE_SLOT_FILE_SIZE));
    SaveSlots_Scan(slots, sound);
    assert(slots[5].status == SAVE_SLOT_USED && slots[5].from_duplicate && slots[5].starchips == 77);

    /* Both damaged: shown as damaged, not loadable. */
    image[SAVE_SLOT_DUPLICATE_OFFSET + SAVE_SLOT_STATE_SIZE - 1] = 0;
    assert(!SaveSlots_WriteFile(6, image, SAVE_SLOT_FILE_SIZE));
    SaveSlots_Scan(slots, sound);
    assert(slots[6].status == SAVE_SLOT_DAMAGED);
    assert(SaveSlots_ReadState(6, state, sound) == -1);
    assert(SaveSlots_ReadState(7, state, sound) == -1);

    /* A patch changes only the bytes it names. */
    memset(state, 0xEE, 0x10);
    assert(!SaveSlots_WriteAt(4, SAVE_SLOT_HEADER_SIZE + 0x10, state, 0x10));
    assert(!SaveSlots_ReadState(4, state, sound));
    assert(state[0x10] == 0xEE && state[0x1F] == 0xEE && state[0x20] == 0 && state[0x50] == 3);
    assert(SaveSlots_WriteAt(8, SAVE_SLOT_HEADER_SIZE, state, 0x10) == -1); /* no file to patch */
    assert(SaveSlots_WriteAt(4, SAVE_SLOT_FILE_SIZE - 4, state, 8) == -1);
    assert(SaveSlots_WriteAt(4, 1, state, SIZE_MAX) == -1);
    assert(SaveSlots_WriteAt(4, -1, state, 1) == -1);

    /* An unreadable existing path is damaged, never an empty slot that can
     * be overwritten without confirmation. Directories fail on both OSes. */
    assert(!SaveSlots_Path(9, path, sizeof(path)));
    assert(!mkdir(path, 0700));
    SaveSlots_Scan(slots, sound);
    assert(slots[9].status == SAVE_SLOT_DAMAGED);
    assert(!rmdir(path));

#ifndef _WIN32
    /* Force a short write without filling a disk. The old save must survive
     * and the partial stream must close, so retrying cannot leak handles. */
    {
        char partial[1100];
        int fd, next;
        assert(!SaveSlots_Path(4, path, sizeof(path)));
        snprintf(partial, sizeof(partial), "%s.partial", path);
        fd = open("/dev/null", O_RDONLY);
        assert(fd >= 0 && !close(fd));
        assert(!symlink("/dev/full", partial));
        assert(SaveSlots_WriteFile(4, image, sizeof(image)) == -1);
        next = open("/dev/null", O_RDONLY);
        assert(next == fd);
        assert(!close(next));
        assert(access(partial, F_OK) == -1);
        assert(!SaveSlots_ReadState(4, state, sound));
        assert(state[0x10] == 0xEE && state[0x50] == 3);
    }
#endif

    for (i = 0; i < SAVE_SLOT_COUNT; i++) {
        assert(!SaveSlots_Path(i, path, sizeof(path)));
        remove(path);
    }
    remove(card);
    snprintf(path, sizeof(path), "%s/saves/.cards-imported", directory);
    remove(path);
    snprintf(path, sizeof(path), "%s/saves/.cards-importing", directory);
    remove(path);
    snprintf(path, sizeof(path), "%s/saves", directory);
    assert(!rmdir(path));
    assert(!rmdir(directory));
    puts("save slots: ok");
    return 0;
}
