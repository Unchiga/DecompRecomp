#include "pc/compat/libgs_ot.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)
#define MAIN_OT 0x80100000u
#define MAIN_TABLE 0x80101000u
#define SUB_OT 0x80100020u
#define SUB_TABLE 0x80102000u
static MemoriesMemory memory;

static uint32_t get(uint32_t address)
{
    return Memories_ReadLE32(memory.ram + (address & 0x1fffffu));
}
static void put(uint32_t address, uint32_t value)
{
    Memories_WriteLE32(memory.ram + (address & 0x1fffffu), value);
}
/* One-word AddPrim: a NOP carrying an identifying low byte pattern. */
static void add(uint32_t entry, uint32_t prim, uint32_t marker)
{
    put(prim, 0x01000000u | (get(entry) & 0xffffffu));
    put(prim + 4, marker);
    put(entry, (get(entry) & 0xff000000u) | (prim & 0xffffffu));
}

int main(void)
{
    /* The spliced chain precedes primitives already at the destination entry.
     * Markers are GP0 NOPs (top byte zero), so the stream also validates. */
    const uint32_t expected[] = {0x10, 0x20, 0x21, 0x11, 0x12, 0x13,
        0x80000000u, 0, 0, 0x00010002u, 0, 0, 0, 0};
    uint32_t out[32], saved[64];
    size_t count;
    CHECK(Memories_OtInstallTail(&memory) == MEMORIES_GS_OK);
    put(MAIN_OT, 6); put(MAIN_OT + 4, MAIN_TABLE);
    put(SUB_OT, 2); put(SUB_OT + 4, SUB_TABLE);
    put(MEMORIES_OT_TAIL_FIRST, 0); /* ClearOTagR must restore this tag */
    CHECK(Memories_GsClearOt(&memory, 0, 0, MAIN_OT) == MEMORIES_GS_OK);
    CHECK(Memories_GsClearOt(&memory, 0xfff0, 5, SUB_OT) == MEMORIES_GS_OK);
    CHECK(get(SUB_OT + 8) == 0xfff0u && get(SUB_OT + 12) == 5);
    CHECK(get(MAIN_OT + 16) == MAIN_TABLE + 4 * 63 && get(SUB_OT + 16) == SUB_TABLE + 12);
    CHECK(get(MAIN_TABLE + 4 * 63) == ((MAIN_TABLE + 4 * 62) & 0xffffffu));
    CHECK(get(MAIN_TABLE) == 0x094728u && get(MEMORIES_OT_TAIL_FIRST) == 0x04094714u);
    CHECK(Memories_GsCollectOt(&memory, MAIN_OT, out, 32, 80, &count) == MEMORIES_GPU_OK);
    CHECK(count == 8 && memcmp(out, expected + 6, 8 * sizeof(out[0])) == 0);

    add(MAIN_TABLE + 4 * 40, 0x80110000u, 0x10);
    add(MAIN_TABLE + 4 * 5, 0x80110010u, 0x12);
    add(MAIN_TABLE + 4 * 5, 0x80110020u, 0x11); /* later AddPrim draws first */
    add(MAIN_TABLE, 0x80110030u, 0x13);
    add(SUB_TABLE + 4 * 3, 0x80110040u, 0x20);
    add(SUB_TABLE, 0x80110050u, 0x21); /* splice node is this primitive */
    CHECK(Memories_GsSortOt(&memory, SUB_OT, MAIN_OT, 16) == MEMORIES_GS_OK);
    CHECK(get(0x80110050u) == 0x01110020u);
    CHECK(get(MAIN_TABLE + 4 * 5) == ((SUB_TABLE + 12) & 0xffffffu));
    CHECK(Memories_GsCollectOt(&memory, MAIN_OT, out, 32, 80, &count) == MEMORIES_GPU_OK);
    CHECK(count == 14 && memcmp(out, expected, sizeof(expected)) == 0);
    CHECK(Memories_GpuValidate(out, count) == MEMORIES_GPU_OK);

    /* An empty source splices through its entry 0, still dropping its tail. */
    CHECK(Memories_GsClearOt(&memory, 0, 0, MAIN_OT) == MEMORIES_GS_OK);
    CHECK(Memories_GsClearOt(&memory, 0, 0, SUB_OT) == MEMORIES_GS_OK);
    CHECK(Memories_GsSortOt(&memory, SUB_OT, MAIN_OT, 16) == MEMORIES_GS_OK);
    CHECK(get(SUB_TABLE) == 0x094728u && get(MAIN_TABLE) == ((SUB_TABLE + 12) & 0xffffffu));
    CHECK(Memories_GsCollectOt(&memory, MAIN_OT, out, 32, 80, &count) == MEMORIES_GPU_OK);
    CHECK(count == 8);

    /* Failures leave both tables untouched. */
    CHECK(Memories_GsClearOt(&memory, 0, 0, SUB_OT) == MEMORIES_GS_OK);
    memcpy(saved, memory.ram + (MAIN_TABLE & 0x1fffffu), sizeof(saved));
    /* The walk starts at entry 0, so an empty source needs exactly two hops. */
    CHECK(Memories_GsSortOt(&memory, SUB_OT, MAIN_OT, 1) == MEMORIES_GS_CHAIN_LIMIT);
    put(SUB_OT + 12, 0x100000u); /* index wraps like MIPS; this one leaves RAM */
    CHECK(Memories_GsSortOt(&memory, SUB_OT, MAIN_OT, 16) == MEMORIES_GS_ADDRESS);
    put(SUB_OT + 12, 0);
    put(SUB_TABLE, (SUB_TABLE + 4) & 0xffffffu); /* cycle */
    put(SUB_TABLE + 4, SUB_TABLE & 0xffffffu);
    CHECK(Memories_GsSortOt(&memory, SUB_OT, MAIN_OT, 1000) == MEMORIES_GS_CHAIN_LIMIT);
    CHECK(memcmp(saved, memory.ram + (MAIN_TABLE & 0x1fffffu), sizeof(saved)) == 0);
    put(SUB_OT, 15);
    CHECK(Memories_GsClearOt(&memory, 0, 0, SUB_OT) == MEMORIES_GS_ARGUMENT);
    put(SUB_OT, 2); put(SUB_OT + 4, 0x801ffff8u); /* table crosses RAM end */
    CHECK(Memories_GsClearOt(&memory, 1, 2, SUB_OT) == MEMORIES_GS_ADDRESS);
    CHECK(get(SUB_OT + 8) == 0 && get(SUB_OT + 12) == 0);
    CHECK(Memories_GsClearOt(&memory, 0, 0, 0x1f800000u + 0x3f0) == MEMORIES_GS_ADDRESS);
    CHECK(Memories_GsClearOt(NULL, 0, 0, SUB_OT) == MEMORIES_GS_ARGUMENT);
    puts("LIBGS ordering-table clear, splice and draw-collection checks passed");
    return 0;
}
