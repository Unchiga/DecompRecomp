/* Mods > 3D Monsters: the face-up monsters on the duel field stand on their
 * cards as the models the battle presentation uses, animating on the spot,
 * floating just above them, the player's turned to face the opponent and the
 * opponent's to face the player, whichever way the camera is round.
 *
 * The console could not do this: one monster's MODEL.MRG record is a quarter
 * of its RAM and a quarter of its VRAM, and the duel already has both. The
 * port has neither limit, so each monster gets a private arena (mapped
 * outside guest RAM, at a negative address so the pointer-sign idioms in the
 * model code still hold) and a private texture bank in the software GPU
 * (src/pc/render/soft_gpu.h), which is VRAM-shaped memory a primitive selects
 * with bits the hardware leaves unused in its texture-page word.
 *
 * The monsters are sorted into the game's own model ordering table, the one
 * the battle presentation draws its two duellists into, so they are layered
 * where the game would layer them: over the field, under the hand and the
 * rest of the interface, and against each other by depth.
 *
 * Everything the game owns is borrowed and returned inside the pass: model
 * slot 0 (the duel field itself only uses slot 2, the arena), the packet work
 * base and, while a monster is being loaded, the loader's staging buffer and
 * the model area of VRAM. With the mod off nothing here runs at all.
 *
 * How a monster is loaded, all of it synchronous and beside the game's
 * streaming (Memories_DiscReadSectors), so the duel's own transfers are not
 * disturbed:
 *
 * - `Model_LoadMonsterMerge`'s id arithmetic picks the MODEL.MRG record;
 * - the record's seventeen phases are what `func_80056D7C` programs, so its
 *   copies are replayed here against this monster's arena instead of the two
 *   fixed duel arenas. The phases that a slot flagged `0x80` skips (the
 *   sequence bank and the 50-sector block) are skipped here too;
 * - the phases that upload to VRAM upload for real, because the palette phase
 *   of the setup below reads them back through the GPU. The duel's pixels are
 *   put back afterwards and the monster's block is kept in its bank;
 * - `func_80056828` then runs the same eleven setup phases the game runs,
 *   including `func_8004CB0C`'s parse of the HMD data in the arena. With the
 *   three command words at -1 (which the `0x80` flag leaves behind) no
 *   per-monster control module is ever called, so none is loaded.
 *
 * Drawing is `func_800540B4` and `func_800556E8`, the pair the Library's card
 * model view already drives, with the slot placed by `func_8005A4C4` at the
 * card's own field coordinates (`D_800908A0`, the table the duel projects its
 * card sprites from).
 */
#include "types.h"
#include "psyq/libgte.h"
#include "psyq/libgpu.h"
#include "psyq/libgs.h"
#include "psyq/libhmd.h"
#include "ygo_types.h"
#include "game/model.h"
#include "game/duel_card.h"
#include "game/duel_card_layout.h"
#define DUEL_SCREEN_TABLES_TYPED_POSITIONS
#include "game/duel_screen_tables.h"
#include "game/view_state.h"
#include "game/graphics_frame.h"
#include "game/graphics_frame_buffer.h"
#define MODEL_SLOT_SETUP_EXPLICIT_TRANSFER_ARGS
#include "game/model_slot_setup.h"
#include "game/model_slot_updates.h"
#include "game/model_load_step.h"
#include "game/func_800540B4.h"
#include "game/func_800556E8.h"
#include "game/func_8005922C.h"
#include "game/func_80058DD8.h"
#include "game/file_transfer.h"
#include "game/file_transfer_steps.h"
#include "game/duel_display.h"
#include "game/duel_scene_state.h"
#include "game/high_memory_addresses.h"
#include "pc/compat/gte.h"
#include "pc/render/packets.h"
#include "pc/render/soft_gpu.h"
#include "pc/debug/log.h"
#include "pc/sdk/disc.h"
#include "pc/mods/mods.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/mman.h>

extern u8 D_8009B1D5;          /* the side the view belongs to */
extern void *D_800E9D98[];     /* D_800E9D90[2]: func_800540B4's table */
extern void (*D_800E9DB0[4])(void); /* the frame service callbacks */
extern u32 D_800FE240;         /* GsSetWorkBase */

/* One monster's private RAM. 96 sectors of model data, then the module
 * destinations the record's phases write and the two data words
 * func_8004CB0C stores in the slot, which is everything the two duel arenas
 * hold for a slot. Mapped where a guest pointer is negative, as PS1 code
 * expects of a real address. */
#define ARENA_VARIANT 0x30000u
#define ARENA_PRIMARY 0x35000u
#define ARENA_DATA_A 0x36000u
#define ARENA_DATA_B 0x37000u
#define ARENA_SIZE 0x40000u
#define ARENA_BASE 0x90000000u

/* The model area of VRAM for slot 0: the texture block below the two display
 * buffers, and the palette rows beside them. The record also puts one
 * 256-pixel row aside at (0x200, 0xF2). */
#define BLOCK_X 0
#define BLOCK_Y 0xF0
#define BLOCK_W 0x100
#define BLOCK_H (0x200 - 0xF0)
#define ROW_X 0x200
#define ROW_Y 0xF2
#define ROW_W 0x100

/* The size a monster is drawn at: the middle one stands TALL_PIXELS high in
 * the middle of the field, on a 240-line picture, and the rest spread around
 * that (fit() explains how). MEMORIES_MODS_SCALE scales all of them. */
#define TALL_PIXELS 32
/* How tall a middling monster is at 1:1 in the middle of the field, measured
 * across the models this way; only the spread around it depends on this. */
#define MIDDLING_PIXELS 110
/* How many ordering-table entries a monster is moved towards the camera so
 * that it is drawn over the card it stands on (sort_monster). */
#define DEPTH_STEPS 3
#define SCALE_SMALLEST 0x100
#define SCALE_LARGEST 0x1800

#define SECTOR 2048
#define RECORD_SECTORS MODEL_MRG_SECTOR_COUNT
#define CACHE 8

typedef struct {
    int card;      /* one-based card id; 0 when the entry is free */
    int position;  /* 0 face-up attack, 1 face-up defence */
    unsigned used; /* frame number of the last draw, for replacement */
    int bank;      /* its texture bank in the software GPU */
    u8 *arena;
    ModelSlot slot;
    int stepped;   /* the animation is advanced once a frame, not once a draw */
    /* Where the body sits around the model's own origin, and the scale it is
     * drawn at; both from measure(), once, when it is loaded. */
    int raw_x, raw_y, raw_z;    /* where its body sits around its origin */
    int body_x, body_y, body_z; /* the same, at the scale it is drawn */
    int scale;
} Monster;

static Monster cache[CACHE];
static int enabled[MODS_COUNT];
static u8 *record;             /* one MODEL.MRG record, read whole */
static int mrg_start = -2;
static unsigned frame;
static int inside;

const char *Mods_Name(int mod)
{
    return mod == MODS_FIELD_MODELS ? "3D Monsters" : mod == MODS_HAND_CAMERA ? "Hand camera (L1/R1 turn, L3/R3 zoom)" : "";
}
int Mods_Enabled(int mod) { return mod >= 0 && mod < MODS_COUNT && enabled[mod]; }

void Mods_SetEnabled(int mod, int on)
{
    if (mod >= 0 && mod < MODS_COUNT) {
        enabled[mod] = on != 0;
    }
}

void Mods_Reset(void)
{
    int i;
    for (i = 0; i < CACHE; i++) {
        cache[i].card = 0;
    }
}

static void say(const char *format, ...)
{
    char message[512];
    va_list arguments;
    if (!Log_Enabled(LOG_MODS)) return;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    LOG(LOG_MODS, "%s", message);
}

static int tunable(const char *name, int fallback)
{
    const char *text = getenv(name);
    return text && *text ? (int)strtol(text, NULL, 0) : fallback;
}

/* Model_LoadMonsterMerge's own arithmetic: the three id ranges with no record
 * are rejected and every id above one is biased down. */
static int mrg_record(int model)
{
    if (model < 0 || model >= MODEL_MRG_ID_END ||
        (model >= MODEL_MRG_FIRST_GAP_START && model < MODEL_MRG_FIRST_GAP_END) ||
        (model >= MODEL_MRG_SECOND_GAP_START && model < MODEL_MRG_SECOND_GAP_END) ||
        model == MODEL_MRG_SINGLE_GAP_ID) {
        return -1;
    }
    if (model >= MODEL_MRG_LAST_ID) {
        model--;
    }
    if (model >= MODEL_MRG_SECOND_GAP_END) {
        model -= MODEL_MRG_GAP_SIZE;
    }
    if (model >= MODEL_MRG_FIRST_GAP_END) {
        model -= MODEL_MRG_GAP_SIZE;
    }
    return model;
}

static u16 scratch_pixels[BLOCK_W * BLOCK_H];
static u16 saved_block[BLOCK_W * BLOCK_H], saved_row[ROW_W];

static void bank_put(u16 *bank, int x, int y, int w, int h, const u16 *pixels)
{
    int row;
    for (row = 0; row < h; row++) {
        memcpy(bank + (size_t)(y + row) * SOFT_GPU_WIDTH + x, pixels + (size_t)row * w, (size_t)w * 2);
    }
}

static void load_rect(int x, int y, int w, int h, const void *pixels)
{
    RECT rect;
    rect.x = (short)x;
    rect.y = (short)y;
    rect.w = (short)w;
    rect.h = (short)h;
    LoadImage(&rect, (u32 *)pixels);
}

static void store_rect(int x, int y, int w, int h, void *pixels)
{
    RECT rect;
    rect.x = (short)x;
    rect.y = (short)y;
    rect.w = (short)w;
    rect.h = (short)h;
    StoreImage(&rect, (u32 *)pixels);
}

/* The record's image phases: one 64x16 block a sector, filling the texture
 * band from the top and stepping a column to the right at its end. This is
 * the file-transfer runtime's phase-2 advance with the x-advance bit clear,
 * which is how the MODEL phases program it. */
static const u8 *upload_blocks(const u8 *at, int sectors, int x)
{
    int y = 0x100, i;
    for (i = 0; i < sectors; i++, at += SECTOR) {
        load_rect(x, y, 0x40, 0x10, at);
        y += 0x10;
        if ((y & 0xFF) == 0) {
            y = (y ^ 0x100) & 0x100;
            x += 0x40;
        }
    }
    return at;
}

/* Replay func_80056D7C's seventeen phases for slot 0 into this monster's
 * arena. `position` is the record's alternate selector, which chooses between
 * the two stance variants. */
/* `slot_index` chooses the VRAM column (slot 1's block sits 0x100 to the
 * right) and, when `full`, the sequence bank goes to its slot block too. */
static void run_record_slot(Monster *monster, const u8 *at, int position, int slot_index, int full)
{
    FileTransferDescriptor descriptor;
    u8 *arena = monster->arena;
    int xbase = slot_index << 8;

    memcpy(arena, at, 96 * SECTOR);                       /* 0: model data */
    at += 96 * SECTOR;
    at = upload_blocks(at, 48, xbase);                    /* 1: textures */
    memcpy(D_801DD000, at, 2 * SECTOR);                   /* 2: palette stage */
    at += 2 * SECTOR;
    load_rect(xbase, 0xF8, 0x100, 8, D_801DD000);         /* 3: palettes */
    memcpy(D_801DE000, at, SECTOR);
    at += SECTOR;
    if (position == 0) {                                  /* 4: stance 0 */
        load_rect(ROW_X, ROW_Y, ROW_W, 1, D_801DE000);
        at = upload_blocks(at, 16, xbase + 0xC0);
    } else {
        at += 16 * SECTOR;
    }
    memcpy(D_801DD000, at, SECTOR);                       /* 5 */
    at += SECTOR;
    if (position == 1) {                                  /* 6: stance 1 */
        load_rect(ROW_X, ROW_Y, ROW_W, 1, D_801DD000);
        at = upload_blocks(at, 16, xbase + 0xC0);
    } else {
        at += 16 * SECTOR;
    }
    if (position == 0) {                                  /* 7: stance-0 module */
        memcpy(arena + ARENA_VARIANT, at, 10 * SECTOR);
    }
    at += 10 * SECTOR;
    at += 10 * SECTOR;                                    /* 8: the other slot */
    if (position == 1) {                                  /* 9: stance-1 module */
        memcpy(arena + ARENA_VARIANT, at, 10 * SECTOR);
    }
    at += 10 * SECTOR;
    at += 10 * SECTOR;                                    /* 10 */
    memcpy(arena + ARENA_PRIMARY, at, 2 * SECTOR);        /* 11: primary module */
    at += 2 * SECTOR;
    at += 2 * SECTOR;                                     /* 12 */
    if (full) {                                           /* 13: sequence bank */
        memcpy((void *)(uintptr_t)(0x801A8000u + (unsigned)slot_index * 0x800u), at, SECTOR);
    }
    at += SECTOR;
    at += 50 * SECTOR;                                    /* 14: voices */
    memcpy(D_801DD000, at, SECTOR);                       /* 15: metadata */

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.position = (u32)position;
    descriptor.callback_data = (void *)(uintptr_t)slot_index;
    func_80056D7C(&descriptor, 16);                       /* 16: into the slot */
}

static void run_record(Monster *monster, const u8 *at, int position)
{
    run_record_slot(monster, at, position, 0, 0);
}

/* The model runtime's own setup phases, which end with the slot ready. */
static int run_setup(void)
{
    int guard;
    for (guard = 0; guard < 64; guard++) {
        if (func_80058DD8(0) == 1) {
            return 1;
        }
        func_80056828(0);
    }
    return 0;
}

/* Take the block of VRAM the record filled into this monster's bank, so that
 * every monster on the field keeps its textures at once. Their coordinates
 * are the ones the model's primitives already carry: a bank is VRAM-shaped. */
static void keep_textures(Monster *monster)
{
    u16 *bank = SoftGpu_Bank(monster->bank);
    if (!bank) {
        return;
    }
    store_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, scratch_pixels);
    bank_put(bank, BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, scratch_pixels);
    store_rect(ROW_X, ROW_Y, ROW_W, 1, scratch_pixels);
    bank_put(bank, ROW_X, ROW_Y, ROW_W, 1, scratch_pixels);
}

/* Put the duel's staging buffer and its pixels back. */
static void give_back(const u8 *staging, size_t length)
{
    memcpy(D_801DD000, staging, length);
    load_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, saved_block);
    load_rect(ROW_X, ROW_Y, ROW_W, 1, saved_row);
}

static int load_monster(Monster *monster, int card, int position)
{
    struct timespec started, finished;
    ModelSlot *slot = &D_800F2C40[0];
    static u8 staging[0x2000]; /* the loader's own scratch, put back after */
    u8 *payload_base;
    int model = mrg_record(card - 1), sectors;

    if (model < 0) {
        return 0;
    }
    if (mrg_start == -2) {
        mrg_start = Memories_DiscFileStart("\\DATA\\MODEL.MRG;1");
        say("MODEL.MRG starts at sector %d\n", mrg_start);
    }
    if (mrg_start < 0) {
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &started);
    if (!record && !(record = malloc(RECORD_SECTORS * SECTOR))) {
        return 0;
    }
    sectors = Memories_DiscReadSectors(mrg_start + model * RECORD_SECTORS, RECORD_SECTORS, record);
    if (sectors != RECORD_SECTORS) {
        say("card %d: read %d of %d sectors\n", card, sectors, RECORD_SECTORS);
        return 0;
    }

    /* The slot is reset exactly as Model_LoadMonsterMerge resets it, with the
     * flag that skips the sound bank and leaves the three control-module
     * command words at -1 so no module is ever called. */
    func_8004CB0C(0, 0, 0, 4);
    slot->field_DF8 = (u16)(card - 1);
    slot->field_E1D = 0x80;
    slot->field_DFA = 0;
    slot->field_DFC = 0;
    slot->field_DFE = (u8)position;
    slot->field_DFF = 0;
    slot->field_E14 = 0;

    /* The record's phases fill the loader's staging buffer and the model area
     * of VRAM, both of which belong to the duel. Keep what is in them. */
    memcpy(staging, D_801DD000, sizeof(staging));
    store_rect(BLOCK_X, BLOCK_Y, BLOCK_W, BLOCK_H, saved_block);
    store_rect(ROW_X, ROW_Y, ROW_W, 1, saved_row);
    run_record(monster, record, position);

    /* func_80056828's first phase takes the payload from the slot-0 arena
     * word; point it at this monster's. The call is synchronous, so nothing
     * else can see the substitution. */
    payload_base = D_80010000;
    D_80010000 = monster->arena;
    if (!run_setup()) {
        D_80010000 = payload_base;
        give_back(staging, sizeof(staging));
        say("card %d: setup did not finish (phase %d)\n", card, slot->field_E14);
        return 0;
    }
    D_80010000 = payload_base;

    slot->field_DE8 = (s32)(monster->arena + ARENA_DATA_A);
    slot->field_DEC = (s32)(monster->arena + ARENA_DATA_B);
    monster->slot = *slot;
    monster->card = card;
    monster->position = position;
    keep_textures(monster);
    give_back(staging, sizeof(staging));
    clock_gettime(CLOCK_MONOTONIC, &finished);
    say("card %d stance %d loaded in %d us: %d units, %d parts, animation %d\n", card, position,
        (int)((finished.tv_sec - started.tv_sec) * 1000000 +
              (finished.tv_nsec - started.tv_nsec) / 1000),
        slot->field_E1A, slot->field_E1B, slot->field_BF5);
    return 1;
}

static void fit(Monster *monster);

static Monster *acquire(int card, int position)
{
    Monster *monster = NULL;
    int i;
    for (i = 0; i < CACHE; i++) {
        if (cache[i].card == card && cache[i].position == position) {
            return &cache[i];
        }
    }
    for (i = 0; i < CACHE; i++) {
        if (!cache[i].card) {
            monster = &cache[i];
            break;
        }
        if (!monster || cache[i].used < monster->used) {
            monster = &cache[i];
        }
    }
    if (!monster->arena) {
        void *wanted = (void *)(uintptr_t)(ARENA_BASE + (unsigned)(monster - cache) * ARENA_SIZE);
        void *got = mmap(wanted, ARENA_SIZE, PROT_READ | PROT_WRITE,
                         MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got != wanted) {
            say("no arena at %p\n", wanted);
            return NULL;
        }
        monster->arena = got;
    }
    monster->bank = (int)(monster - cache) + 1;
    monster->card = 0;
    if (!load_monster(monster, card, position)) {
        return NULL;
    }
    fit(monster);
    return monster;
}

/* Every unit's world matrix is cached against the frame counter, so the same
 * monster standing on two cards would otherwise be drawn twice in one place. */
static void forget_coordinates(ModelSlot *slot)
{
    int i;
    for (i = 0; i < slot->field_E1A; i++) {
        GsCOORDUNIT *unit = (GsCOORDUNIT *)(uintptr_t)slot->field_000[i].field_00;
        while (unit && unit->flg) {
            unit->flg = 0;
            unit = unit->super;
        }
    }
}

static void place(ModelSlot *slot, int x, int y, int z, int yaw, int scale)
{
    VECTOR size;
    forget_coordinates(slot);
    func_8005A4C4(slot, x, y, z, yaw);
    size.vx = size.vy = size.vz = scale;
    size.pad = 0;
    func_8005922C(slot->field_D18, &size);
}

/* Point a run of freshly sorted packets at a monster's texture bank: bits
 * 11-14 of a primitive's texture-page word, which the hardware ignores and
 * retail always leaves zero. The page word is the second corner's texture
 * word; the palette word beside it needs nothing, because a bank holds the
 * palettes at their own coordinates too. */
static void stamp_bank(const u32 *from, const u32 *to, int bank)
{
    while (from < to) {
        unsigned words = *from >> 24, at = 1;
        while (at <= words) {
            u32 command = from[at];
            unsigned op = command >> 24, length = Memories_GpuCommandWords(command), corner;
            if (!length || at + length > words + 1) {
                break;
            }
            if (op >= 0x24 && op <= 0x3f && (op & 4)) {
                unsigned corners = (op & 8) ? 4 : 3, index = at + 1;
                for (corner = 0; corner < corners; corner++) {
                    if ((op & 0x10) && corner) {
                        index++;
                    }
                    index++;
                    if (corner == 1) {
                        ((u32 *)from)[index] |= (u32)bank << 27;
                    }
                    index++;
                }
            }
            at += length;
        }
        from += words + 1;
    }
}

/* Sorting a monster into the game's own model ordering table is all the
 * drawing there is: the frame it belongs to has not been sent to the GPU yet.
 * D_800E9D98[0] is the table func_800540B4 uses for slots 0 and 1, which the
 * battle presentation draws its duellists into. */
static void sort_monster(Monster *monster)
{
    const u32 *from = (const u32 *)(uintptr_t)D_800FE240;
    GsOT *table = (GsOT *)D_800E9D98[0];
    GsOT_TAG *org = table->org;
    uint32_t zsf3 = Memories_GteReadControl(29), zsf4 = Memories_GteReadControl(30);
    int nearer = tunable("MEMORIES_MODS_DEPTH", DEPTH_STEPS);

    /* The field's cards and a model measure depth differently in the same
     * table: a card is sorted at a sixteenth of its distance
     * (func_80015EF4), a model's primitives at a quarter of theirs, which is
     * the scale the GTE's Z factors give them. Quartering those factors puts
     * a monster on the same scale as the card it stands on, and the few
     * entries taken off `org` -- which the primitive drivers index from,
     * with no offset of their own -- are what draw it in front of that card
     * rather than under it. */
    Memories_GteWriteControl(29, zsf3 / 4);
    Memories_GteWriteControl(30, zsf4 / 4);
    table->org = org - nearer;
    func_800540B4(0);
    table->org = org;
    Memories_GteWriteControl(29, zsf3);
    Memories_GteWriteControl(30, zsf4);
    stamp_bank(from, (const u32 *)(uintptr_t)D_800FE240, monster->bank);
}

/* The measuring table: the packet area of the frame buffer that is about to
 * be cleared for the next frame, which is free until then. Its packets are
 * read for their screen coordinates and never drawn. */
#define TAGS_BYTES 0x10000
#define TABLE_AT TAGS_BYTES
#define PACKETS_AT (TAGS_BYTES + 0x20)
static u8 *scratch;

static int packet_height(const u32 *from, const u32 *to);

static int sort_aside(void)
{
    GsOT *table = (GsOT *)(scratch + TABLE_AT);
    void *live = D_800E9D98[0];
    u32 base = D_800FE240;
    int height;
    table->length = 14;
    table->org = (GsOT_TAG *)scratch;
    table->offset = 0;
    table->point = 0;
    GsClearOt(0, 0, table);
    GsSetWorkBase((PACKET *)(scratch + PACKETS_AT));
    D_800E9D98[0] = table;
    func_800540B4(0);
    height = packet_height((const u32 *)(scratch + PACKETS_AT), (const u32 *)(uintptr_t)D_800FE240);
    D_800E9D98[0] = live;
    D_800FE240 = base;
    return height;
}

/* How many scan lines the packets just sorted aside cover. This is the only
 * thing that answers "how big is this monster": a model's parts say where its
 * joints are, not how far its skin reaches, and Korogashi is one big ball
 * around a single joint. */
static int packet_height(const u32 *from, const u32 *to)
{
    int top = 0x7fff, bottom = -0x7fff;
    while (from < to) {
        unsigned words = *from >> 24, at = 1;
        while (at <= words) {
            u32 command = from[at];
            unsigned op = command >> 24, length = Memories_GpuCommandWords(command), corner;
            if (!length || at + length > words + 1) {
                break;
            }
            if (op >= 0x20 && op <= 0x3f) {
                unsigned corners = (op & 8) ? 4 : 3, index = at + 1;
                for (corner = 0; corner < corners; corner++) {
                    int y;
                    if ((op & 0x10) && corner) {
                        index++; /* every further corner of a shaded polygon */
                    }
                    y = (short)(from[index] >> 16);
                    top = y < top ? y : top;
                    bottom = y > bottom ? y : bottom;
                    index++;
                    if (op & 4) {
                        index++; /* texture coordinates */
                    }
                }
            }
            at += length;
        }
        from += words + 1;
    }
    return bottom > top ? bottom - top : 0;
}

/* Where the body sits around the model's own origin. A duel model is built
 * around the point between the two duellists, so slot 0's body stands some
 * 250 units up the field from the origin func_8005A4C4 places, and nothing in
 * the record says by how much -- the parts do, once their world matrices are
 * built. y is the feet, which is y at its largest because it grows downwards. */
static void measure_body(Monster *monster)
{
    ModelSlot *slot = &D_800F2C40[0];
    s32 feet = -0x7fffffff, sum_x = 0, sum_z = 0;
    int parts = 0, i;

    *slot = monster->slot;
    place(slot, 0, 0, 0, 0, MODEL_FIXED_ONE);
    for (i = 0; i < slot->field_E1A; i++) {
        GsCOORDUNIT *unit = (GsCOORDUNIT *)(uintptr_t)slot->field_000[i].field_00;
        MATRIX world;
        if (!unit) {
            continue;
        }
        GsGetLwUnit(unit, &world);
        feet = world.t[1] > feet ? world.t[1] : feet;
        sum_x += world.t[0];
        sum_z += world.t[2];
        parts++;
    }
    if (parts) {
        monster->raw_x = sum_x / parts;
        monster->raw_y = feet;
        monster->raw_z = sum_z / parts;
    }
}

static void scale_body(Monster *monster)
{
    monster->body_x = monster->raw_x * monster->scale / MODEL_FIXED_ONE;
    monster->body_y = monster->raw_y * monster->scale / MODEL_FIXED_ONE;
    monster->body_z = monster->raw_z * monster->scale / MODEL_FIXED_ONE;
}

/* Pick the scale this monster is drawn at, once, when it is loaded: one
 * monster is several times another end to end, and at a single scale either
 * the small ones are specks or the big ones cover the field. Each is sized
 * until it is about TALL_PIXELS high in the middle of the field, then the
 * square root of that answer is taken against the middle size, which keeps
 * the order -- a dragon still towers over Sangan -- while bringing the range
 * in. The monster is sorted but never drawn: the packets are read and thrown
 * away. */
static void fit(Monster *monster)
{
    ModelSlot *slot = &D_800F2C40[0];
    int target = tunable("MEMORIES_MODS_PIXELS", TALL_PIXELS), attempt, height = 0;

    monster->scale = MODEL_FIXED_ONE / 2;
    measure_body(monster);
    for (attempt = 0; attempt < 5; attempt++) {
        int wanted;
        scale_body(monster);
        *slot = monster->slot;
        place(slot, -monster->body_x, -monster->body_y, -monster->body_z, 0, monster->scale);
        height = sort_aside();
        if (height <= 0) {
            break;
        }
        wanted = monster->scale * target / height;
        if (wanted > monster->scale * 15 / 16 && wanted < monster->scale * 17 / 16) {
            break;
        }
        monster->scale = wanted < SCALE_SMALLEST ? SCALE_SMALLEST
                       : wanted > SCALE_LARGEST ? SCALE_LARGEST : wanted;
    }
    if (height > 0 && monster->scale > 0) {
        /* The size that would make it TALL_PIXELS high says how big the
         * monster is in itself; drawing it at the square root of that against
         * a middling monster keeps the order -- a dragon still towers over
         * Sangan -- while bringing a sevenfold range down to about two and a
         * half. */
        double natural = (double)target * MODEL_FIXED_ONE / monster->scale;
        monster->scale = (int)(MODEL_FIXED_ONE * target / sqrt(natural * MIDDLING_PIXELS));
        monster->scale = monster->scale < SCALE_SMALLEST ? SCALE_SMALLEST
                       : monster->scale > SCALE_LARGEST ? SCALE_LARGEST : monster->scale;
    }
    monster->scale = monster->scale * tunable("MEMORIES_MODS_SCALE", MODEL_FIXED_ONE) / MODEL_FIXED_ONE;
    scale_body(monster);
    say("card %d fits %d pixels at %d/4096, body at %d,%d,%d\n", monster->card, height, monster->scale,
        monster->body_x, monster->body_y, monster->body_z);
}

/* How far above its card a monster floats, in field units (y is down).
 * LIFT_PIXELS is what that comes to on the 240-line picture from the view the
 * duel is played from; MEMORIES_MODS_LIFT overrides the units directly. */
#define LIFT_PIXELS 8
#define LIFT_UNITS_PER_PIXEL 2
static int lift(void)
{
    return tunable("MEMORIES_MODS_LIFT", LIFT_PIXELS * LIFT_UNITS_PER_PIXEL);
}

static void draw_monster(Monster *monster, int x, int z, int yaw)
{
    ModelSlot *slot = &D_800F2C40[0];
    int turned = yaw == MODEL_ANGLE_HALF_TURN;

    *slot = monster->slot;
    /* The body offset was measured facing up the field, so turning the
     * monster turns it too. */
    place(slot, turned ? x + monster->body_x : x - monster->body_x, -monster->body_y - lift(),
          turned ? z + monster->body_z : z - monster->body_z, yaw, monster->scale);
    sort_monster(monster);
    if (!monster->stepped) {
        func_800556E8(0);
        monster->stepped = 1;
    }
    monster->slot = *slot;
    monster->used = frame;
}

/* The duel field seen from above is the one place this draws. Its own
 * card-drawing service is installed only while the field is up and the arena
 * model has to be loaded; the rest is the camera. Choosing a zone, using a
 * card and the fusion presentations all fly it down to eye level with the
 * mat (pitch 1022 of a 4096-unit turn, against 256 for the view the duel is
 * played from) and put their own panels on the screen, and a monster
 * standing on a card has nothing to stand on there. */
#define FIELD_PITCH 512

static int duel_field_up(void)
{
    static int phase = -1, distance = -1, pitch = -1, angle = -1;
    if (Log_Enabled(LOG_MODS) && (phase != (gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK) ||
                      distance != D_800F2848.field_00 || pitch != D_800F2848.field_04 ||
                      angle != D_800F2848.angle)) {
        phase = gDuel_wSceneStateFlags & DUEL_SCENE_PHASE_MASK;
        distance = D_800F2848.field_00;
        pitch = D_800F2848.field_04;
        angle = D_800F2848.angle;
        say("duel scene phase %d, camera %d away, pitch %d, angle %d, side %d, projection %d\n",
            phase, distance, pitch, angle, D_8009B1D5, D_800F2848.projection);
    }
    return D_800E9DB0[3] == Duel_DrawFieldCards && D_800F2C40[2].field_E1F != 0 &&
           D_800F2848.field_04 < tunable("MEMORIES_MODS_PITCH", FIELD_PITCH);
}

typedef struct {
    Monster *monster;
    int x, z, yaw;
} Standing;

/* The five monster zones of a side, in card-record order. */
#define MONSTER_ZONES 5
#define SIDE_ZONE(side, zone) ((side) ? 20 + (zone) : 5 + (zone))
/* Records 0-14 are the player's, whose view is the quarter-turn camera. */
#define DUEL_SIDE_PLAYER 0

void Mods_DrawFrame(void)
{
    static ModelSlot borrowed;
    Standing standing[DUEL_SIDE_COUNT * MONSTER_ZONES];
    u32 work_base;
    int count = 0, i, side, zone;

    HandCamera_Frame();
    if (inside || !enabled[MODS_FIELD_MODELS] || !duel_field_up()) {
        return;
    }
    frame++;
    inside = 1;
    borrowed = D_800F2C40[0];
    work_base = D_800FE240;
    scratch = &D_800A5768[GsGetActiveBuff() * GRAPHICS_PACKET_BUFFER_SIZE];

    /* The projection the duel draws its own field with. */
    GsSetRefView2(&D_800F2848.view);
    SetGeomScreen(D_800F2848.projection);
    SetGeomOffset(0xA0, 0x6C);
    SetFarColor(0, 0, 0);
    SetFogNearFar(0x28A, 0x320, D_800F2848.projection);

    for (side = 0; side < DUEL_SIDE_COUNT; side++) {
        for (zone = 0; zone < MONSTER_ZONES; zone++) {
            int index = SIDE_ZONE(side, zone);
            DuelCardRecord *card = &D_801A7AD8[index];
            Monster *monster;
            int id = card->card_id;

            /* A field full of monsters, for measuring: every zone stands a
             * different one, which exercises the cache, the arenas and the
             * texture banks at once. */
            if (tunable("MEMORIES_MODS_TEST", 0)) {
                id = tunable("MEMORIES_MODS_TEST", 0) + zone * 2 + side;
            } else if (!(card->flags & DUEL_CARD_FLAG_OCCUPIED) ||
                (card->flags & DUEL_CARD_FLAG_FACE_DOWN) || id <= 0 ||
                ((gDuel_adwCardStats[id - 1] >> 0x1A) & 0x1F) >= 0x14) {
                continue; /* empty, face down, or a magic or trap card */
            }
            /* The record carries a stance of its own for a monster in
             * defence, which is the one the battle presentation would use. */
            monster = acquire(id, (card->flags & DUEL_CARD_FLAG_DEFENSE_POSITION) ? 1 : 0);
            if (!monster) {
                continue;
            }
            standing[count].monster = monster;
            standing[count].x = D_800908A0[index].x;
            standing[count].z = D_800908A0[index].y;
            /* A duel model faces the player's edge of the mat at yaw 0 --
             * that is slot 0 of the battle presentation, which stands
             * up-field looking back -- so the player's own monsters turn
             * round to face the opponent. The facing is fixed to the side
             * that owns the zone, not to whose turn it is: the turn switch
             * swings the camera a half turn round the mat over 48 frames
             * and flips D_8009B1D5 a third of the way through, and choosing
             * by that snapped every monster round mid-swing and left the
             * player's showing their backs for the opponent's turn. */
            standing[count].yaw = side == DUEL_SIDE_PLAYER ? MODEL_ANGLE_HALF_TURN : 0;
            count++;
        }
    }

    for (i = 0; i < count; i++) {
        standing[i].monster->stepped = 0;
    }
    for (i = 0; i < count; i++) {
        draw_monster(standing[i].monster, standing[i].x, standing[i].z, standing[i].yaw);
    }

    D_800F2C40[0] = borrowed;
    if (!count) {
        D_800FE240 = work_base;
    }
    SetGeomOffset(0, 0);
    inside = 0;
}
