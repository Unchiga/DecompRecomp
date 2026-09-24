/* Save slot files. See save_slots.h. */
#define _POSIX_C_SOURCE 200809L
#include "save_slots.h"
#include "pc/platform/paths.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "pc/compat/posix.h"

/* Offsets in the save state (src/game/save_data.h). */
#define STATE_DECK 0x0
#define STATE_DECK_SIZE 40
#define STATE_QUANTITIES 0x50
#define STATE_QUANTITIES_SIZE 722
#define STATE_DUELIST_CODE 0x334
#define STATE_SEQUENCE 0x404
#define STATE_NAME 0x40C
#define STATE_NAME_CHARS 6
#define STATE_WINS 0x518
#define STATE_LOSSES 0x51A
#define STATE_STARCHIPS 0x5E0

/* Memory card image layout (src/pc/sdk/libmcrd.c). */
#define CARD_SIZE 0x20000
#define CARD_FRAME 128
#define CARD_BLOCKS 15
#define CARD_STATE_FIRST 0x51u

/* The token's tag: these eight bytes, then the token, little-endian. */
static const unsigned char TAG[8] = {'Y', 'F', 'M', 'S', 'L', 'O', 'T', 1};

static unsigned read_u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned read_u32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int SaveSlots_Path(int slot, char *out, size_t size)
{
    char relative[32];
    if (slot < 0 || slot >= SAVE_SLOT_COUNT) return -1;
    snprintf(relative, sizeof(relative), "saves/slot%02d.sav", slot + 1);
    return Paths_User(out, size, relative);
}

/* Full-width Shift-JIS (stored as little-endian halfwords) to ASCII. */
static char ascii(unsigned code)
{
    static const struct { unsigned short sjis; char ascii; } marks[] = {
        {0x8140, ' '}, {0x8143, ','}, {0x8144, '.'}, {0x8145, '.'}, {0x8146, ':'}, {0x8147, ';'},
        {0x8148, '?'}, {0x8149, '!'}, {0x815B, '-'}, {0x815E, '/'}, {0x8166, '\''}, {0x8168, '"'},
        {0x8169, '('}, {0x816A, ')'}, {0x817B, '+'}, {0x817C, '-'}, {0x8181, '='}, {0x8193, '%'},
        {0x8194, '#'}, {0x8195, '&'}, {0x8196, '*'}, {0x8197, '@'}, {0x83BF, 'a'} /* alpha */};
    size_t i;
    if (code >= 0x20 && code < 0x7F) return (char)code;
    if (code >= 0x8260 && code <= 0x8279) return (char)('A' + code - 0x8260);
    if (code >= 0x8281 && code <= 0x829A) return (char)('a' + code - 0x8281);
    if (code >= 0x824F && code <= 0x8258) return (char)('0' + code - 0x824F);
    for (i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        if (marks[i].sjis == code) return marks[i].ascii;
    }
    return '?';
}

char SaveSlots_Ascii(unsigned sjis) { return ascii(sjis); }

void SaveSlots_StateName(const unsigned char *state, char *out, size_t size)
{
    size_t length = 0;
    int i;
    if (!size) return;
    for (i = 0; i < STATE_NAME_CHARS && length + 1 < size; i++) {
        unsigned code = read_u16(state + STATE_NAME + 2 * i);
        if (!code) break;
        out[length++] = ascii(code);
    }
    while (length && out[length - 1] == ' ') length--;
    out[length] = 0;
}

/* The slot's whole file, -1 when missing, -2 for other read failures. */
static long read_file(int slot, unsigned char image[SAVE_SLOT_FILE_SIZE], long long *saved_at)
{
    char path[1024];
    struct stat info;
    FILE *file;
    size_t got;
    if (SaveSlots_Path(slot, path, sizeof(path))) return -2;
    file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? -1 : -2;
    got = fread(image, 1, SAVE_SLOT_FILE_SIZE, file);
    if (ferror(file)) {
        fclose(file);
        return -2;
    }
    fclose(file);
    memset(image + got, 0, SAVE_SLOT_FILE_SIZE - got);
    if (saved_at) *saved_at = stat(path, &info) == 0 ? (long long)info.st_mtime : 0;
    return (long)got;
}

/* Which copy in `image` is sound: 0 the first, 1 the duplicate, -1 neither. */
static int sound_copy(unsigned char *image, long got, SaveSlotCheck check)
{
    if (got >= SAVE_SLOT_HEADER_SIZE + SAVE_SLOT_STATE_SIZE && check(image + SAVE_SLOT_HEADER_SIZE)) return 0;
    if (got >= SAVE_SLOT_DUPLICATE_OFFSET + SAVE_SLOT_STATE_SIZE && check(image + SAVE_SLOT_DUPLICATE_OFFSET)) return 1;
    return -1;
}

void SaveSlots_Scan(SaveSlotInfo out[SAVE_SLOT_COUNT], SaveSlotCheck check)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    int slot, i;
    for (slot = 0; slot < SAVE_SLOT_COUNT; slot++) {
        SaveSlotInfo *info = &out[slot];
        const unsigned char *state;
        long long saved_at = 0;
        long got = read_file(slot, image, &saved_at);
        int copy;
        memset(info, 0, sizeof(*info));
        info->saved_at = saved_at;
        if (got == -1) {
            info->status = SAVE_SLOT_EMPTY;
            continue;
        }
        copy = sound_copy(image, got, check);
        if (copy < 0) {
            info->status = SAVE_SLOT_DAMAGED;
            continue;
        }
        state = image + (copy ? SAVE_SLOT_DUPLICATE_OFFSET : SAVE_SLOT_HEADER_SIZE);
        info->status = SAVE_SLOT_USED;
        info->from_duplicate = copy;
        SaveSlots_StateName(state, info->name, sizeof(info->name));
        info->duelist_code = (int)read_u32(state + STATE_DUELIST_CODE);
        info->sequence = read_u32(state + STATE_SEQUENCE);
        info->starchips = read_u32(state + STATE_STARCHIPS);
        info->wins = (int)read_u16(state + STATE_WINS);
        info->losses = (int)read_u16(state + STATE_LOSSES);
        for (i = 0; i < STATE_QUANTITIES_SIZE; i++) info->cards += state[STATE_QUANTITIES + i];
        for (i = 0; i < STATE_DECK_SIZE; i++) info->cards += read_u16(state + STATE_DECK + 2 * i) != 0;
    }
}

int SaveSlots_ReadState(int slot, unsigned char state[SAVE_SLOT_STATE_SIZE], SaveSlotCheck check)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    long got = read_file(slot, image, NULL);
    int copy = got < 0 ? -1 : sound_copy(image, got, check);
    if (copy < 0) return -1;
    memcpy(state, image + (copy ? SAVE_SLOT_DUPLICATE_OFFSET : SAVE_SLOT_HEADER_SIZE), SAVE_SLOT_STATE_SIZE);
    return 0;
}

static int store(int slot, const unsigned char image[SAVE_SLOT_FILE_SIZE])
{
    char path[1024], partial[1100];
    FILE *file;
    int failed;
    if (SaveSlots_Path(slot, path, sizeof(path))) return -1;
    snprintf(partial, sizeof(partial), "%s.partial", path);
    file = fopen(partial, "wb");
    if (!file) {
        fprintf(stderr, "memories-pc: cannot write save slot %s\n", partial);
        return -1;
    }
    failed = fwrite(image, 1, SAVE_SLOT_FILE_SIZE, file) != SAVE_SLOT_FILE_SIZE;
    /* On the disk before it replaces the old save, so a power cut leaves
     * one or the other and never an empty slot. */
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) failed = 1;
    /* Always close, including after a short write (e.g. a full disk). */
    if (fclose(file) != 0) failed = 1;
    if (failed || rename(partial, path) != 0) {
        fprintf(stderr, "memories-pc: cannot write save slot %s\n", path);
        remove(partial);
        return -1;
    }
    return 0;
}

/* A token no other save is likely to have: the time, the clock and a count,
 * mixed. Never 0, which means none. */
static unsigned fresh_token(void)
{
    static unsigned count;
    unsigned value = (unsigned)time(NULL) * 2654435761u ^ (unsigned)clock() * 40503u ^ ++count * 97u;
    value ^= value >> 15;
    value *= 0x2c1b3c6du;
    value ^= value >> 12;
    return value ? value : 1;
}

int SaveSlots_WriteFile(int slot, const unsigned char *image, size_t bytes)
{
    static unsigned char block[SAVE_SLOT_FILE_SIZE];
    unsigned token = fresh_token();
    int i;
    if (bytes > SAVE_SLOT_FILE_SIZE) return -1;
    memset(block, 0, sizeof(block));
    memcpy(block, image, bytes);
    /* The game reads no further than the duplicate; the rest of a block is
     * its padding, a card's included. */
    memcpy(block + SAVE_SLOT_TAG_OFFSET, TAG, sizeof(TAG));
    for (i = 0; i < 4; i++) block[SAVE_SLOT_TAG_OFFSET + sizeof(TAG) + i] = (unsigned char)(token >> 8 * i);
    return store(slot, block);
}

unsigned SaveSlots_Token(int slot)
{
    static unsigned char image[SAVE_SLOT_FILE_SIZE];
    const unsigned char *tag = image + SAVE_SLOT_TAG_OFFSET;
    if (read_file(slot, image, NULL) < 0 || memcmp(tag, TAG, sizeof(TAG))) return 0;
    return read_u32(tag + sizeof(TAG));
}

int SaveSlots_WriteAt(int slot, long offset, const unsigned char *data, size_t bytes)
{
    static unsigned char block[SAVE_SLOT_FILE_SIZE];
    if (offset < 0 || offset > SAVE_SLOT_FILE_SIZE || bytes > SAVE_SLOT_FILE_SIZE - (size_t)offset) return -1;
    if (read_file(slot, block, NULL) < 0) return -1;
    memcpy(block + offset, data, bytes);
    return store(slot, block);
}

int SaveSlots_WriteState(int slot, const unsigned char state[SAVE_SLOT_STATE_SIZE])
{
    static unsigned char block[SAVE_SLOT_FILE_SIZE];
    if (read_file(slot, block, NULL) < 0) return -1;
    memcpy(block + SAVE_SLOT_HEADER_SIZE, state, SAVE_SLOT_STATE_SIZE);
    memcpy(block + SAVE_SLOT_DUPLICATE_OFFSET, state, SAVE_SLOT_STATE_SIZE);
    return store(slot, block);
}

/* The first block of the file `name` on a card image, or -1. */
static int card_file(const unsigned char *card, const char *name)
{
    int block;
    if (card[0] != 'M' || card[1] != 'C') return -1;
    for (block = 0; block < CARD_BLOCKS; block++) {
        const unsigned char *frame = card + CARD_FRAME * (block + 1);
        if (read_u32(frame) == CARD_STATE_FIRST && !strncmp((const char *)frame + 10, name, 20)) return block;
    }
    return -1;
}

/* 0 when the card is done with: imported, already in its slot, or not
 * there to import; -1 when it should be tried again next time. */
static int import_card(int index, const char *name)
{
    static unsigned char card[CARD_SIZE], existing[SAVE_SLOT_FILE_SIZE];
    char path[1024], target[1024];
    const char *named = getenv(index ? "MEMORIES_MEMCARD2" : "MEMORIES_MEMCARD1");
    FILE *file;
    int block;
    long have = read_file(index, existing, NULL);
    if (have != -1) return have >= 0 ? 0 : -1;   /* never over a slot already there */
    if (named) snprintf(path, sizeof(path), "%s", named);
    else if (Paths_User(path, sizeof(path), index ? "memcard2.mcd" : "memcard1.mcd")) return 0;
    file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? 0 : -1;
    if (fread(card, 1, CARD_SIZE, file) != CARD_SIZE) block = -1;
    else block = card_file(card, name);
    fclose(file);
    if (block < 0) return 0;
    /* The game's save is one block; the header and both copies are in it. */
    if (SaveSlots_WriteFile(index, card + SAVE_SLOT_FILE_SIZE * (block + 1), SAVE_SLOT_FILE_SIZE)) return -1;
    if (!SaveSlots_Path(index, target, sizeof(target)))
        fprintf(stderr, "memories-pc: copied the save on %s into %s\n", path, target);
    return 0;
}

static void touch(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (file) fclose(file);
}

void SaveSlots_ImportMemoryCards(const char *name)
{
    char directory[1024], done[1100], pending[1100];
    struct stat info;
    if (Paths_User(directory, sizeof(directory), "saves")) return;
    snprintf(done, sizeof(done), "%s/.cards-imported", directory);
    snprintf(pending, sizeof(pending), "%s/.cards-importing", directory);
    if (stat(done, &info) == 0) return;
    if (stat(directory, &info) == 0 && stat(pending, &info) != 0) {
        /* Made by a build without the markers, which imported as it made
         * the folder. */
        touch(done);
        return;
    }
    if (Paths_MakeDirs(directory)) return;
    touch(pending);
    /* A card that could not be read is tried again at the next save or
     * load; one already in its slot, or not there at all, is done. */
    if (import_card(0, name) | import_card(1, name)) return;
    touch(done);
    remove(pending);
}
