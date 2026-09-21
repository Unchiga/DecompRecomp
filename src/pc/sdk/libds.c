/* LIBDS/LIBCD over a raw MODE2/2352 disc image (MEMORIES_DISC, default
 * game/rpg-yfm.bin). Commands complete, and sectors arrive, on the VBlank
 * tick, which is interrupt context in the original too: a callback never runs
 * inside the call that queued it. Only async-signal-safe calls are used once
 * the image is open. XA audio and STR streaming are not decoded yet: an XA
 * "play" just advances the head at 1x so position polling still finishes. */
#include "types.h"
#include "psyq/libds.h"
#include "pc/audio/spu.h"
#include "pc/sdk/disc.h"
#include "pc/guest/image.h"
#include "pc/debug/log.h"
#include "pc/guest/state.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RAW_SECTOR 2352
#define USER_DATA 24
#define QUEUE 8
#define DATA_SECTORS_PER_TICK 8

typedef struct Pending {
    DslCB callback;
    u8 command;
    u8 result[8];
} Pending;

static int disc = -1;
static Pending queue[QUEUE];
static volatile unsigned queue_head, queue_tail;
static int next_id = 1;
static u8 mode, filter_file, filter_channel;
static int reading, streaming, head_lba, target_lba;
static DslRCB ready_callback;
static void (*cd_ready_callback)(u8, u8 *);
static u8 sector[RAW_SECTOR];
static unsigned sector_cursor;
static uint64_t stream_next_us;
static int xa_history[2][2];

static u8 to_bcd(int value) { return (u8)(((value / 10) << 4) | (value % 10)); }
static int from_bcd(u8 value) { return (value >> 4) * 10 + (value & 15); }

static int loc_to_lba(const DslLOC *loc)
{
    return (from_bcd(loc->minute) * 60 + from_bcd(loc->second)) * 75 + from_bcd(loc->sector) - 150;
}

static void lba_to_loc(int lba, DslLOC *loc)
{
    lba += 150;
    loc->minute = to_bcd(lba / (60 * 75));
    loc->second = to_bcd(lba / 75 % 60);
    loc->sector = to_bcd(lba % 75);
}

DslLOC *CdIntToPos_8007E600(int lba, DslLOC *loc)
{
    lba_to_loc(lba, loc);
    return loc;
}

int CdPosToInt_8007E710(const DslLOC *loc) { return loc_to_lba(loc); }
int CdPosToInt(DslLOC *loc) { return loc_to_lba(loc); }

static int read_raw(int lba, u8 *out)
{
    return disc >= 0 && pread(disc, out, RAW_SECTOR, (off_t)lba * RAW_SECTOR) == RAW_SECTOR;
}

int DsInit(void)
{
    const char *path = getenv("MEMORIES_DISC");
    if (disc < 0) {
        disc = open(path ? path : "game/rpg-yfm.bin", O_RDONLY);
        if (disc < 0) {
            fprintf(stderr, "memories-pc: cannot open the disc image %s (set MEMORIES_DISC)\n",
                    path ? path : "game/rpg-yfm.bin");
            _exit(1);
        }
    }
    queue_head = queue_tail = 0;
    reading = 0;
    return 1;
}

void CdFlush(void)
{
    queue_head = queue_tail;
    reading = 0;
}

static const u8 *find_entry(const u8 *directory, size_t size, const char *name, size_t length)
{
    size_t at = 0;
    while (at < size) {
        const u8 *record = directory + at;
        if (record[0] == 0) { /* records do not span sectors */
            at = (at / 2048 + 1) * 2048;
            continue;
        }
        if (record[32] >= length && memcmp(record + 33, name, length) == 0 &&
            (record[32] == length || record[33 + length] == ';')) {
            return record;
        }
        at += record[0];
    }
    return NULL;
}

static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

DslFILE *DsSearchFile(DslFILE *file, char *name)
{
    static u8 directory[16 * 2048];
    u8 raw[RAW_SECTOR];
    u32 extent, size, i;
    const char *part = name;
    if (disc < 0) {
        DsInit();
    }
    if (!read_raw(16, raw)) {
        return NULL;
    }
    extent = le32(raw + USER_DATA + 156 + 2);
    size = le32(raw + USER_DATA + 156 + 10);
    for (;;) {
        const u8 *record;
        const char *end;
        size_t length;
        while (*part == '\\') {
            part++;
        }
        end = strchr(part, '\\');
        length = end ? (size_t)(end - part) : strcspn(part, ";");
        if (size > sizeof(directory)) {
            size = sizeof(directory);
        }
        for (i = 0; i * 2048 < size; i++) {
            if (!read_raw((int)(extent + i), raw)) {
                return NULL;
            }
            memcpy(directory + i * 2048, raw + USER_DATA, 2048);
        }
        record = find_entry(directory, size, part, length);
        if (!record) {
            return NULL;
        }
        extent = le32(record + 2);
        size = le32(record + 10);
        if (!end) {
            memset(file, 0, sizeof(*file));
            lba_to_loc((int)extent, &file->pos);
            file->size = size;
            strncpy(file->name, part, sizeof(file->name) - 1);
            return file;
        }
        part = end;
    }
}

static int enqueue(u8 command, DslCB callback, const u8 *result)
{
    unsigned next = (queue_tail + 1) % QUEUE;
    if (next == queue_head) {
        return 0;
    }
    queue[queue_tail].command = command;
    queue[queue_tail].callback = callback;
    memset(queue[queue_tail].result, 0, 8);
    if (result) {
        memcpy(queue[queue_tail].result, result, 8);
    }
    queue_tail = next;
    if (++next_id <= 0) {
        next_id = 1;
    }
    return next_id;
}

static void execute(u8 command, u8 *param, u8 *result)
{
    switch (command) {
    case 0x02: target_lba = loc_to_lba((DslLOC *)param); break;
    case 0x06: /* ReadN */
    case 0x1b: /* ReadS */
        head_lba = target_lba;
        reading = 1;
        streaming = command == 0x1b;
        break;
    case 0x08: /* Stop */
    case 0x09: /* Pause */
        reading = 0;
        Spu_CdFlush();
        break;
    case 0x0d: filter_file = param[0]; filter_channel = param[1]; break;
    case 0x0e: mode = param[0]; break;
    case 0x10: /* GetlocL: header and subheader of the last sector read */
        memcpy(result, sector + 12, 8);
        break;
    case 0x15: /* SeekL */
    case 0x16:
        head_lba = target_lba;
        reading = 0;
        break;
    default: break;
    }
}

int DsCommand(u8 command, u8 *param, DslCB callback, int count)
{
    u8 result[8] = {0};
    (void)count;
    execute(command, param, result);
    return enqueue(command, callback, result);
}

int DsPacket(u8 new_mode, DslLOC *position, u8 command, DslCB callback, int count)
{
    u8 result[8] = {0};
    (void)count;
    mode = new_mode;
    target_lba = loc_to_lba(position);
    execute(command, NULL, result);
    return enqueue(command, callback, result);
}

int DsReadySystemMode(int value) { (void)value; return 0; }

int DsStartReadySystem(DslRCB callback, int count)
{
    (void)count;
    ready_callback = callback;
    return 1;
}

void DsEndReadySystem(void) { ready_callback = NULL; }

void *CdReadyCallback(void (*callback)(u8, u8 *))
{
    void *previous = (void *)cd_ready_callback;
    cd_ready_callback = callback;
    return previous;
}

int CdGetSector(void *destination, int words)
{
    unsigned bytes = (unsigned)words * 4;
    if (sector_cursor + bytes > RAW_SECTOR) {
        bytes = RAW_SECTOR - sector_cursor;
    }
    memcpy(destination, sector + sector_cursor, bytes);
    sector_cursor += bytes;
    Memories_GuestWritten(destination, bytes);
    Log_Signal(LOG_DISC, "lba %ld -> 0x%lx, %ld bytes", head_lba - 1,
               (long)(uintptr_t)destination, bytes, 0, 0, 0);
    return 1;
}

int CdMix(void *volume)
{
    (void)volume;
    return 1;
}

/* LIBCD streaming (St*): video sectors of a running stream are assembled into
 * whole frames here instead of going to the ready callback. Frames live in
 * host slots, not in the ring the game offers; a full queue drops the frame,
 * like a ring overrun. */
#define ST_SLOTS 8
#define ST_FRAME_BYTES (2016 * 40)
typedef struct StSlot {
    StHEADER header;
    u8 data[ST_FRAME_BYTES + 8];
    volatile int state; /* 0 free, 1 filling, 2 ready, 3 handed to the game */
} StSlot;
static StSlot st_slots[ST_SLOTS];
static volatile int st_active;
static int st_filling = -1;
static unsigned st_next_out, st_next_in, st_last_frame;
static DslLOC st_last_loc;

void StSetRing(u32 *ring, u32 sectors) { (void)ring; (void)sectors; }

void StClearRing(void)
{
    int i;
    for (i = 0; i < ST_SLOTS; i++) {
        st_slots[i].state = 0;
    }
    st_filling = -1;
    st_next_out = st_next_in = 0;
}

void StUnSetRing(void) { st_active = 0; StClearRing(); }

void StSetStream(u32 stream_mode, u32 start_frame, u32 end_frame, void (*first)(void), void (*second)(void))
{
    (void)stream_mode; (void)start_frame; (void)end_frame; (void)first; (void)second;
    st_active = 1;
}

u32 StGetNext(u32 **address, u32 **header)
{
    StSlot *slot = &st_slots[st_next_out % ST_SLOTS];
    if (slot->state != 2) {
        return 1;
    }
    slot->state = 3;
    *address = (u32 *)slot->data;
    *header = (u32 *)&slot->header;
    return 0;
}

u32 StFreeRing(u32 *base)
{
    StSlot *slot = &st_slots[st_next_out % ST_SLOTS];
    if (slot->state == 3 && (u32 *)slot->data == base) {
        slot->state = 0;
        st_next_out++;
        return 0;
    }
    return 1;
}

int StGetBackloc(DslLOC *loc)
{
    *loc = st_last_loc;
    return (int)st_last_frame;
}

void StCdInterrupt(void) {}

static void stream_video(const u8 *raw)
{
    StHEADER header;
    StSlot *slot;
    memcpy(&header, raw + USER_DATA, 24);
    if (header.id != 0x0160 || header.nSectors == 0 || header.nSectors > 40) {
        return;
    }
    if (header.secCount == 0) {
        slot = &st_slots[st_next_in % ST_SLOTS];
        if (slot->state != 0) {
            st_filling = -1; /* queue full: drop this frame */
            return;
        }
        st_filling = (int)(st_next_in % ST_SLOTS);
        slot->state = 1;
        slot->header = header;
        memcpy(&slot->header.loc, raw + 12, 4);
    }
    if (st_filling < 0) {
        return;
    }
    slot = &st_slots[st_filling];
    if (header.frameCount != slot->header.frameCount) {
        slot->state = 0;
        st_filling = -1;
        return;
    }
    memcpy(slot->data + header.secCount * 2016, raw + USER_DATA + 32, 2016);
    if (header.secCount + 1 == header.nSectors) {
        st_last_frame = header.frameCount;
        st_last_loc = slot->header.loc;
        slot->state = 2;
        st_next_in++;
        st_filling = -1;
    }
}

/* Synchronous drive control used by the movie player. */
int CdControlB(u8 command, u8 *param, u8 *result)
{
    u8 scratch[8];
    execute(command, param, result ? result : scratch);
    return 1;
}

int DsRead2(DslLOC *position, int read_mode)
{
    mode = (u8)read_mode;
    target_lba = head_lba = loc_to_lba(position);
    reading = 1;
    streaming = 1;
    return 1;
}

/* One XA ADPCM sector: 18 sound groups of 8 units x 28 samples (4-bit). */
static void play_xa(const u8 *raw)
{
    static const int k0[4] = {0, 60, 115, 98}, k1[4] = {0, 0, -52, -55};
    static int16_t frames[18 * 4 * 28 * 2 * 2];
    int stereo = raw[19] & 1, half_rate = raw[19] & 4, group, unit, i;
    size_t count[2] = {0, 0};
    if (raw[19] & 0x10) {
        return; /* 8-bit XA is not used by this disc */
    }
    for (group = 0; group < 18; group++) {
        const u8 *data = raw + 24 + group * 128;
        for (unit = 0; unit < 8; unit++) {
            int channel = stereo ? unit & 1 : 0, header = data[4 + unit];
            int shift = (header & 15) > 12 ? 9 : header & 15, filter = (header >> 4) & 3;
            for (i = 0; i < 28; i++) {
                int nibble = (data[16 + i * 4 + unit / 2] >> ((unit & 1) * 4)) & 15;
                int sample = ((int16_t)(nibble << 12) >> shift) +
                             ((xa_history[channel][0] * k0[filter] + xa_history[channel][1] * k1[filter] + 32) >> 6);
                sample = sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample;
                xa_history[channel][1] = xa_history[channel][0];
                xa_history[channel][0] = sample;
                if (stereo) {
                    frames[count[channel]++ * 2 + (size_t)channel] = (int16_t)sample;
                } else {
                    frames[count[0] * 2] = frames[count[0] * 2 + 1] = (int16_t)sample;
                    count[0]++;
                }
            }
        }
    }
    Spu_CdWrite(frames, count[0], half_rate ? 18900 : 37800);
}

/* Streaming reads (ReadS, or any read with the XA bit) run at the drive's
 * real rate because audio and video are paced by it: 75 sectors per second,
 * 150 with the speed bit. Plain data reads are not rate-limited to hardware
 * speed (2 sectors per millisecond). */
void Memories_DiscService(uint64_t now_us)
{
    int budget = 2;
    while (queue_head != queue_tail) {
        Pending done = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE;
        if (done.callback) {
            done.callback(2 /* DslComplete */, done.result);
        }
    }
    if (!reading) {
        stream_next_us = 0;
        return;
    }
    if (streaming || (mode & 0x40)) {
        uint64_t period = mode & 0x80 ? 1000000u / 150 : 1000000u / 75;
        if (!stream_next_us || now_us > stream_next_us + 200000) {
            stream_next_us = now_us;
        }
        budget = 0;
        while (now_us >= stream_next_us && budget < 4) {
            stream_next_us += period;
            budget++;
        }
    } else if (!ready_callback) {
        return;
    }
    for (; budget > 0 && reading; budget--) {
        int audio;
        if (!read_raw(head_lba, sector)) {
            reading = 0;
            break;
        }
        head_lba++;
        audio = (sector[18] & 0x64) == 0x64 || (sector[18] & 0x04);
        if (audio && (mode & 0x40)) {
            if (!(mode & 0x08) || (sector[16] == filter_file && sector[17] == filter_channel)) {
                play_xa(sector);
            }
            continue;
        }
        if (!audio && st_active) {
            stream_video(sector);
            continue;
        }
        if (audio || !ready_callback) {
            continue;
        }
        sector_cursor = USER_DATA;
        ready_callback(1 /* DslDataReady */, sector + 12, (u32 *)(sector + 12));
    }
}

/* The disc file itself belongs to the process. Callbacks are game functions.
 * Buffered movie frames are kept so a state taken during a stream resumes. */
void LibDs_State(MemoriesState *state)
{
    const MemoriesStateField fields[] = {
        {queue, sizeof(queue)}, {(void *)&queue_head, sizeof(queue_head)}, {(void *)&queue_tail, sizeof(queue_tail)},
        {&next_id, sizeof(next_id)}, {&mode, sizeof(mode)}, {&filter_file, sizeof(filter_file)},
        {&filter_channel, sizeof(filter_channel)}, {&reading, sizeof(reading)}, {&streaming, sizeof(streaming)},
        {&head_lba, sizeof(head_lba)}, {&target_lba, sizeof(target_lba)}, {&ready_callback, sizeof(ready_callback)},
        {&cd_ready_callback, sizeof(cd_ready_callback)}, {sector, sizeof(sector)},
        {&sector_cursor, sizeof(sector_cursor)}, {xa_history, sizeof(xa_history)}, {st_slots, sizeof(st_slots)},
        {(void *)&st_active, sizeof(st_active)}, {&st_filling, sizeof(st_filling)}, {&st_next_out, sizeof(st_next_out)},
        {&st_next_in, sizeof(st_next_in)}, {&st_last_frame, sizeof(st_last_frame)},
        {&st_last_loc, sizeof(st_last_loc)}};
    if (Memories_StateChunk(state, "libds", fields, sizeof(fields) / sizeof(fields[0]))) {
        stream_next_us = 0;
    }
}

/* Bulk reads for the native extras (src/pc/mods), beside the drive model
 * rather than through it: the game's own streaming keeps its queue, its head
 * and its callbacks while a whole MODEL.MRG record is fetched in one call. */
int Memories_DiscReadSectors(int lba, int sectors, void *out)
{
    u8 raw[RAW_SECTOR];
    int i;
    if (disc < 0) {
        DsInit();
    }
    for (i = 0; i < sectors; i++) {
        if (!read_raw(lba + i, raw)) {
            break;
        }
        memcpy((u8 *)out + (size_t)i * 2048, raw + USER_DATA, 2048);
    }
    return i;
}

int Memories_DiscFileStart(const char *path)
{
    DslFILE file;
    char name[64];
    strncpy(name, path, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    if (!DsSearchFile(&file, name)) {
        return -1;
    }
    return loc_to_lba(&file.pos);
}
