/* LIBMCRD over memory card image files.
 *
 * A card is a 128 KiB raw image in the standard layout (the ".mcd"/".mcr"
 * format emulators use), so saves move freely between this port, emulators
 * and real cards. Slot 1 is memcard1.mcd in the user directory (paths.h),
 * created formatted when it is missing; slot 2 is memcard2.mcd beside it and
 * only exists if the file does.
 * MEMORIES_MEMCARD1 / MEMORIES_MEMCARD2 name other files.
 *
 * Layout: fifteen 8 KiB data blocks after a directory block of 128-byte
 * frames. Frame 0 is "MC"; frames 1-15 describe blocks 1-15 (state, size,
 * next block in the file, name); each ends in an XOR checksum.
 *
 * The library's card commands are asynchronous. The file is read or written
 * when a command is issued, on the game thread, and the result is reported
 * after the time the hardware would have taken (about one 128-byte frame per
 * VBlank), so the game's "now saving" messages stay up as they did. */
#include "types.h"
#include "pc/guest/state.h"
#include "pc/platform/platform.h"
#include "pc/platform/paths.h"
#include "pc/debug/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* From libmcrd.h and kernel.h, which pull in MIPS-only headers. */
enum { McFuncExist = 1, McFuncAccept, McFuncReadFile, McFuncWriteFile };
enum { McErrNone, McErrCardNotExist, McErrCardInvalid, McErrNewCard, McErrNotFormat, McErrFileNotExist,
       McErrAlreadyExist, McErrBlockFull };
struct DIRENTRY {
    char name[20];
    long attr, size;
    struct DIRENTRY *next;
    long head;
    char system[4];
};

#define CARD_SIZE 0x20000
#define FRAME 128
#define BLOCK 0x2000
#define BLOCKS 15
#define STATE_FIRST 0x51u
#define STATE_MIDDLE 0x52u
#define STATE_LAST 0x53u
#define STATE_FREE 0xa0u

typedef struct Card {
    char path[512];
    u8 image[CARD_SIZE];
    int present, announced; /* announced: the "new card" result has been given */
} Card;

static Card cards[2];
static int active;
/* The one command in flight. */
static long pending_command, pending_result;
static unsigned pending_until;
static int pending;

static u8 *entry(Card *card, int block) { return card->image + FRAME * (block + 1); }

static void seal(u8 *frame)
{
    u8 sum = 0;
    int i;
    for (i = 0; i < FRAME - 1; i++) {
        sum ^= frame[i];
    }
    frame[FRAME - 1] = sum;
}

static u32 word(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static void set_word(u8 *p, u32 value)
{
    p[0] = (u8)value;
    p[1] = (u8)(value >> 8);
    p[2] = (u8)(value >> 16);
    p[3] = (u8)(value >> 24);
}

static void format_image(Card *card)
{
    int i;
    memset(card->image, 0, CARD_SIZE);
    card->image[0] = 'M';
    card->image[1] = 'C';
    seal(card->image);
    for (i = 0; i < BLOCKS; i++) {
        u8 *frame = entry(card, i);
        set_word(frame, STATE_FREE);
        frame[8] = frame[9] = 0xff;
        seal(frame);
    }
    for (i = 16; i < 36; i++) { /* no broken sectors */
        u8 *frame = card->image + FRAME * i;
        set_word(frame, 0xffffffffu);
        frame[8] = frame[9] = 0xff;
        seal(frame);
    }
    memcpy(card->image + FRAME * 63, card->image, FRAME); /* write-test frame */
}

static int store(Card *card)
{
    char partial[600];
    FILE *file;
    snprintf(partial, sizeof(partial), "%s.partial", card->path);
    file = fopen(partial, "wb");
    if (!file || fwrite(card->image, 1, CARD_SIZE, file) != CARD_SIZE || fclose(file) != 0 ||
        rename(partial, card->path) != 0) {
        fprintf(stderr, "memories-pc: cannot write memory card %s\n", card->path);
        return -1;
    }
    return 0;
}

/* Re-read the image on every access, so a card swapped or edited outside the
 * game is seen the way a changed card would be. */
static Card *open_card(long channel)
{
    int slot = channel >> 4 ? 1 : 0;
    Card *card = &cards[slot];
    const char *named = getenv(slot ? "MEMORIES_MEMCARD2" : "MEMORIES_MEMCARD1");
    FILE *file;
    if ((channel & 0xf) != 0) {
        return NULL; /* no multitap */
    }
    if (named) snprintf(card->path, sizeof(card->path), "%s", named);
    else if (Paths_User(card->path, sizeof(card->path), slot ? "memcard2.mcd" : "memcard1.mcd")) return NULL;
    file = fopen(card->path, "rb");
    if (file) {
        size_t got = fread(card->image, 1, CARD_SIZE, file);
        fclose(file);
        if (got != CARD_SIZE) {
            memset(card->image + got, 0, CARD_SIZE - got);
        }
        card->present = 1;
    } else if (slot == 0) {
        format_image(card);
        card->present = store(card) == 0;
        if (card->present) {
            fprintf(stderr, "memories-pc: created memory card %s\n", card->path);
        }
    } else {
        card->present = 0;
    }
    if (!card->present) {
        card->announced = 0;
        return NULL;
    }
    return card;
}

static int formatted(const Card *card)
{
    return card->image[0] == 'M' && card->image[1] == 'C';
}

static int find(Card *card, const char *name)
{
    int i;
    for (i = 0; i < BLOCKS; i++) {
        const u8 *frame = entry(card, i);
        if (word(frame) == STATE_FIRST && !strncmp((const char *)frame + 10, name, 20)) {
            return i;
        }
    }
    return -1;
}

/* BIOS-style pattern: '?' matches one character, '*' the rest. */
static int matches(const char *pattern, const char *name)
{
    for (; *pattern; pattern++, name++) {
        if (*pattern == '*') {
            return 1;
        }
        if (*pattern != '?' && *pattern != *name) {
            return 0;
        }
        if (!*name) {
            return 0;
        }
    }
    return !*name;
}

static void begin(long command, long result, unsigned vblanks)
{
    pending_command = command;
    pending_result = result;
    pending_until = Platform_VBlankCount() + vblanks;
    pending = 1;
}

void MemCardInit(long shared_with_pad) { (void)shared_with_pad; }
void MemCardEnd(void) {}
void MemCardStart(void) { active = 1; }
void MemCardStop(void) { active = 0; }

static long check(long channel, long command)
{
    Card *card;
    long result;
    if (pending) {
        return 0;
    }
    card = open_card(channel);
    if (!card) {
        result = McErrCardNotExist;
    } else if (!card->announced) {
        card->announced = command == McFuncAccept; /* Exist keeps reporting it; Accept acknowledges */
        result = command == McFuncAccept && !formatted(card) ? McErrNotFormat : McErrNewCard;
    } else {
        result = formatted(card) ? McErrNone : McErrNotFormat;
    }
    begin(command, result, 20);
    LOG(LOG_MEMCARD, "check channel=%ld command=%ld result=%ld announced=%d",
        channel, command, result, card ? card->announced : 0);
    return 1;
}

long MemCardExist(long channel) { return check(channel, McFuncExist); }
long MemCardAccept(long channel) { return check(channel, McFuncAccept); }

static long transfer(long channel, const char *name, u8 *memory, long offset, long bytes, int writing)
{
    Card *card;
    long command = writing ? McFuncWriteFile : McFuncReadFile, result = McErrNone;
    int block;
    if (pending) {
        return 0;
    }
    card = open_card(channel);
    if (!card) {
        result = McErrCardNotExist;
    } else if (!formatted(card)) {
        result = McErrNotFormat;
    } else if ((block = find(card, name)) < 0) {
        result = McErrFileNotExist;
    } else {
        long at = offset, left = bytes;
        while (left > 0 && block >= 0 && block < BLOCKS) {
            if (at < BLOCK) {
                long count = BLOCK - at < left ? BLOCK - at : left;
                u8 *data = card->image + BLOCK * (block + 1) + at;
                if (writing) {
                    memcpy(data, memory, (size_t)count);
                } else {
                    memcpy(memory, data, (size_t)count);
                }
                memory += count;
                left -= count;
                at = 0;
            } else {
                at -= BLOCK;
            }
            block = entry(card, block)[8] | (entry(card, block)[9] << 8);
            block = block == 0xffff ? -1 : block;
        }
        if (left > 0) {
            result = McErrFileNotExist; /* past the end of the file */
        } else if (writing && store(card) != 0) {
            result = McErrCardInvalid;
        }
    }
    begin(command, result, (unsigned)((bytes + FRAME - 1) / FRAME) + 4);
    return 1;
}

long MemCardReadFile(long channel, char *file, unsigned long *address, long offset, long bytes)
{
    return transfer(channel, file, (u8 *)address, offset, bytes, 0);
}

long MemCardWriteFile(long channel, char *file, unsigned long *address, long offset, long bytes)
{
    return transfer(channel, file, (u8 *)address, offset, bytes, 1);
}

/* mode 0 waits for the command; mode 1 reports: 1 done, 0 running, -1 none. */
long MemCardSync(long mode, long *command, long *result)
{
    if (!pending) {
        return -1;
    }
    while ((int)(Platform_VBlankCount() - pending_until) < 0) {
        if (mode != 0) {
            return 0;
        }
        Platform_WaitVBlank(Platform_VBlankCount());
    }
    pending = 0;
    if (command) {
        *command = pending_command;
    }
    if (result) {
        *result = pending_result;
    }
    LOG(LOG_MEMCARD, "sync command=%ld result=%ld", pending_command, pending_result);
    return 1;
}

long MemCardCreateFile(long channel, char *file, long blocks)
{
    Card *card = open_card(channel);
    int chain[BLOCKS], found = 0, i;
    if (!card) {
        return McErrCardNotExist;
    }
    if (!formatted(card)) {
        return McErrNotFormat;
    }
    if (find(card, file) >= 0) {
        return McErrAlreadyExist;
    }
    for (i = 0; i < BLOCKS && found < blocks; i++) {
        if ((word(entry(card, i)) & 0xf0) == STATE_FREE) {
            chain[found++] = i;
        }
    }
    if (blocks < 1 || found < blocks) {
        return McErrBlockFull;
    }
    for (i = 0; i < blocks; i++) {
        u8 *frame = entry(card, chain[i]);
        int next = i + 1 < blocks ? chain[i + 1] : 0xffff;
        memset(frame, 0, FRAME);
        set_word(frame, i == 0 ? STATE_FIRST : i + 1 < blocks ? STATE_MIDDLE : STATE_LAST);
        if (i == 0) {
            set_word(frame + 4, (u32)blocks * BLOCK);
            strncpy((char *)frame + 10, file, 20);
        }
        frame[8] = (u8)next;
        frame[9] = (u8)(next >> 8);
        seal(frame);
        memset(card->image + BLOCK * (chain[i] + 1), 0, BLOCK);
    }
    return store(card) == 0 ? McErrNone : McErrCardInvalid;
}

long MemCardDeleteFile(long channel, char *file)
{
    Card *card = open_card(channel);
    int block;
    if (!card) {
        return McErrCardNotExist;
    }
    if (!formatted(card)) {
        return McErrNotFormat;
    }
    block = find(card, file);
    if (block < 0) {
        return McErrFileNotExist;
    }
    while (block >= 0 && block < BLOCKS) {
        u8 *frame = entry(card, block);
        int next = frame[8] | (frame[9] << 8);
        memset(frame, 0, FRAME);
        set_word(frame, STATE_FREE);
        frame[8] = frame[9] = 0xff;
        seal(frame);
        block = next == 0xffff ? -1 : next;
    }
    return store(card) == 0 ? McErrNone : McErrCardInvalid;
}

long MemCardFormat(long channel)
{
    Card *card = open_card(channel);
    if (!card) {
        return McErrCardNotExist;
    }
    format_image(card);
    return store(card) == 0 ? McErrNone : McErrCardInvalid;
}

long MemCardGetDirentry(long channel, char *name, struct DIRENTRY *directory, long *files, long offset, long max)
{
    Card *card = open_card(channel);
    long seen = 0, stored = 0;
    int i;
    *files = 0;
    if (!card) {
        return McErrCardNotExist;
    }
    if (!formatted(card)) {
        return McErrNotFormat;
    }
    for (i = 0; i < BLOCKS && stored < max; i++) {
        const u8 *frame = entry(card, i);
        char found[21];
        if (word(frame) != STATE_FIRST) {
            continue;
        }
        memcpy(found, frame + 10, 20);
        found[20] = 0;
        if (!matches(name, found) || seen++ < offset) {
            continue;
        }
        memset(&directory[stored], 0, sizeof(directory[stored]));
        memcpy(directory[stored].name, found, 20);
        directory[stored].attr = (long)STATE_FIRST;
        directory[stored].size = (long)word(frame + 4);
        directory[stored].head = i;
        stored++;
    }
    *files = stored;
    if (Log_Enabled(LOG_MEMCARD)) {
        LOG(LOG_MEMCARD, "directory channel=%ld pattern='%s' files=%ld", channel, name, stored);
        for (i = 0; i < stored; i++) {
            LOG(LOG_MEMCARD, "directory entry=%d name='%.20s' size=%ld head=%ld",
                i, directory[i].name, directory[i].size, directory[i].head);
        }
    }
    return McErrNone;
}

/* The card itself is a file; only the command in flight belongs to a state. */
void LibMcrd_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {{&active, sizeof(active)}, {&pending, sizeof(pending)},
                                         {&pending_command, sizeof(pending_command)},
                                         {&pending_result, sizeof(pending_result)},
                                         {&pending_until, sizeof(pending_until)}};
    Memories_StateChunk(state, "libmcrd", fields, sizeof(fields) / sizeof(fields[0]));
}
