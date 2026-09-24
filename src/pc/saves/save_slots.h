#ifndef MEMORIES_PC_SAVE_SLOTS_H
#define MEMORIES_PC_SAVE_SLOTS_H
/* Save slots: the port's saves, one file per slot, instead of memory cards.
 *
 * Slot n is saves/slotNN.sav in the user directory (paths.h). A slot file is
 * byte for byte the 8 KiB block the game writes to a memory card: the
 * 0x200-byte title/icon header, the 0x680-byte save state and its 0x680-byte
 * duplicate, then zeros. So a slot can go back onto a card image with any
 * memory card manager, and a card's save can become a slot.
 *
 * The first time the saves directory is created, the save on each memory
 * card image the older builds used (memcard1.mcd, memcard2.mcd) is copied
 * into slots 1 and 2. The card images are only read.
 *
 * Nothing here touches game memory; the caller passes the game's integrity
 * check in, so the storage can be tested on its own. */
#include <stddef.h>

#define SAVE_SLOT_COUNT 10
#define SAVE_SLOT_FILE_SIZE 0x2000
#define SAVE_SLOT_HEADER_SIZE 0x200
#define SAVE_SLOT_STATE_SIZE 0x680
#define SAVE_SLOT_DUPLICATE_OFFSET (SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE)

typedef enum { SAVE_SLOT_EMPTY, SAVE_SLOT_USED, SAVE_SLOT_DAMAGED } SaveSlotStatus;

/* What a slot shows in the menu, read from its save state. */
typedef struct SaveSlotInfo {
    SaveSlotStatus status;
    int from_duplicate; /* the first copy failed its check; the duplicate is used */
    char name[16];      /* player name in ASCII */
    int duelist_code;
    unsigned sequence;
    unsigned starchips;
    int wins, losses, cards;
    /* The file's modification time, seconds since 1970; aligned so i386
     * Linux lays it out as Windows does. */
    long long saved_at __attribute__((aligned(8)));
} SaveSlotInfo;

/* SaveData_ValidateIntegrity: nonzero when a 0x680-byte state is sound. */
typedef int (*SaveSlotCheck)(unsigned char *state);

/* Slots are numbered from 0 here; the menu shows them from 1. */
int SaveSlots_Path(int slot, char *out, size_t size);
void SaveSlots_Scan(SaveSlotInfo out[SAVE_SLOT_COUNT], SaveSlotCheck check);
/* Read a slot's sound state (the duplicate when the first copy is damaged)
 * into `state`. 0 on success, -1 when the slot is empty or both copies fail. */
int SaveSlots_ReadState(int slot, unsigned char state[SAVE_SLOT_STATE_SIZE], SaveSlotCheck check);
/* Replace a slot with `bytes` of file image (header first), padded with
 * zeros to a whole block. Written beside the slot and renamed over it, so a
 * failed write leaves the old save. 0 on success. */
int SaveSlots_WriteFile(int slot, const unsigned char *image, size_t bytes);
/* Patch `bytes` at `offset` of an existing slot, the same way. */
int SaveSlots_WriteAt(int slot, long offset, const unsigned char *data, size_t bytes);
/* Replace both state copies together, preserving the existing header and
 * padding. Used after a trade so backup recovery retains the traded cards. */
int SaveSlots_WriteState(int slot, const unsigned char state[SAVE_SLOT_STATE_SIZE]);
/* Copy the save named `name` off the memory card images into slots 1 and
 * 2, once: only when the saves directory does not exist yet. */
void SaveSlots_ImportMemoryCards(const char *name);
/* The player name of a state, in ASCII (full-width letters, digits and the
 * usual punctuation; anything else becomes '?'). */
void SaveSlots_StateName(const unsigned char *state, char *out, size_t size);

#endif
