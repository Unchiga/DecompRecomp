#include "pc/render/packets.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)
static MemoriesMemory memory;
static void put(uint32_t address, uint32_t word)
{
    Memories_WriteLE32(memory.ram + address, word);
}

int main(void)
{
    uint32_t out[16];
    size_t count;
    const uint32_t expected[] = {0xe100010au, 0x65000000u, 0x00100010u, 0, 0x00100010u};
    /* Empty OT bucket -> draw state -> sprite split across two DMA packets.
     * The terminal node still contains data; the texture command's low bits
     * must survive without being confused with a pointer. */
    put(0x100, 0x200);
    put(0x200, 0x01000300); put(0x204, expected[0]);
    put(0x300, 0x02000400); put(0x304, expected[1]); put(0x308, expected[2]);
    put(0x400, 0x02ffffff); put(0x404, expected[3]); put(0x408, expected[4]);
    CHECK(Memories_GpuCollect(&memory, 0x80000100u, out, 16, 4, &count) == MEMORIES_GPU_OK);
    CHECK(count == 5 && memcmp(out, expected, sizeof(expected)) == 0);
    CHECK(Memories_GpuValidate(out, count) == MEMORIES_GPU_OK);
    CHECK(Memories_GpuCollect(&memory, 0xa0000100u, out, 16, 4, &count) == MEMORIES_GPU_OK);
    CHECK(Memories_GpuCollect(&memory, 0x100, out, 4, 4, &count) == MEMORIES_GPU_CAPACITY);
    CHECK(count == 0);
    CHECK(Memories_GpuCollect(&memory, 0x100, out, 16, 3, &count) == MEMORIES_GPU_CHAIN_LIMIT);
    CHECK(count == 0);
    put(0x400, 0x00000100); /* cycle */
    CHECK(Memories_GpuCollect(&memory, 0x100, out, 16, 8, &count) == MEMORIES_GPU_CHAIN_LIMIT);
    put(0x100, 0x101); /* unaligned next pointer */
    CHECK(Memories_GpuCollect(&memory, 0x100, out, 16, 8, &count) == MEMORIES_GPU_ADDRESS);
    put(0x100, 0x00200000);
    CHECK(Memories_GpuCollect(&memory, 0x100, out, 16, 8, &count) == MEMORIES_GPU_ADDRESS);
    put(0x1ffffc, 0x01ffffff); /* length crosses RAM end */
    CHECK(Memories_GpuCollect(&memory, 0x1ffffc, out, 16, 8, &count) == MEMORIES_GPU_ADDRESS);
    CHECK(Memories_GpuCollect(&memory, 0x1f800000, out, 16, 8, &count) == MEMORIES_GPU_ADDRESS);
    CHECK(Memories_GpuCollect(&memory, 0xffffff, NULL, 0, 1, &count) == MEMORIES_GPU_OK);
    CHECK(count == 0);
    CHECK(Memories_GpuCollect(&memory, 0, NULL, 1, 1, &count) == MEMORIES_GPU_ARGUMENT);
    CHECK(Memories_GpuCollect(&memory, 0, out, 16, 0, &count) == MEMORIES_GPU_ARGUMENT);
    CHECK(Memories_GpuValidate(NULL, 0) == MEMORIES_GPU_OK);
    CHECK(Memories_GpuValidate(NULL, 1) == MEMORIES_GPU_ARGUMENT);
    out[0] = 0x2c000000; /* FT4 requires nine words */
    CHECK(Memories_GpuValidate(out, 8) == MEMORIES_GPU_TRUNCATED_COMMAND);
    out[0] = 0xa0000000;
    CHECK(Memories_GpuValidate(out, 1) == MEMORIES_GPU_UNSUPPORTED_COMMAND);
    out[0] = 0x48000000; /* variable-length polyline */
    CHECK(Memories_GpuValidate(out, 1) == MEMORIES_GPU_UNSUPPORTED_COMMAND);
    CHECK(Memories_GpuCommandWords(0x20000000) == 4);
    CHECK(Memories_GpuCommandWords(0x24000000) == 7);
    CHECK(Memories_GpuCommandWords(0x28000000) == 5);
    CHECK(Memories_GpuCommandWords(0x2c000000) == 9);
    CHECK(Memories_GpuCommandWords(0x30000000) == 6);
    CHECK(Memories_GpuCommandWords(0x34000000) == 9);
    CHECK(Memories_GpuCommandWords(0x38000000) == 8);
    CHECK(Memories_GpuCommandWords(0x3f000000) == 12);
    CHECK(Memories_GpuCommandWords(0x60000000) == 3);
    CHECK(Memories_GpuCommandWords(0x64000000) == 4);
    CHECK(Memories_GpuCommandWords(0x68000000) == 2);
    CHECK(Memories_GpuCommandWords(0x7c000000) == 3);
    /* Model the resident SDK's two payload-bearing tail nodes. The copy
     * command must execute before the four NOPs in the terminating node. */
    put(0x94728, 0x04094714); put(0x9472c, 0x80000000);
    put(0x94730, 0); put(0x94734, 0); put(0x94738, 0x00010002);
    put(0x94714, 0x04ffffff);
    put(0x94718, 0); put(0x9471c, 0); put(0x94720, 0); put(0x94724, 0);
    CHECK(Memories_GpuCollect(&memory, 0x80094728u, out, 16, 2, &count) == MEMORIES_GPU_OK);
    CHECK(count == 8 && out[0] == 0x80000000u && out[3] == 0x00010002u && out[7] == 0);
    CHECK(Memories_GpuValidate(out, count) == MEMORIES_GPU_OK);
    puts("Retail packet traversal, validation and malformed-chain checks passed");
    return 0;
}
