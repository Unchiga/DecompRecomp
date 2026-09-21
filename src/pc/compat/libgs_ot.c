#include "libgs_ot.h"

#define LINK_MASK UINT32_C(0x00ffffff)
#define LENGTH_MASK UINT32_C(0xff000000)

static uint8_t *word(MemoriesMemory *memory, uint32_t address)
{
    return Memories_Resolve(memory, address, 4, 4);
}

MemoriesGsResult Memories_OtInstallTail(MemoriesMemory *memory)
{
    static const uint32_t first[5] = {0, UINT32_C(0x80000000), 0, 0, UINT32_C(0x00010002)};
    static const uint32_t second[5] = {UINT32_C(0x04ffffff), 0, 0, 0, 0};
    uint8_t *a = Memories_Resolve(memory, MEMORIES_OT_TAIL_FIRST, 20, 4);
    uint8_t *b = Memories_Resolve(memory, MEMORIES_OT_TAIL_SECOND, 20, 4);
    size_t i;
    if (!a || !b) {
        return MEMORIES_GS_ARGUMENT;
    }
    for (i = 0; i < 5; i++) {
        Memories_WriteLE32(a + 4 * i, first[i]);
        Memories_WriteLE32(b + 4 * i, second[i]);
    }
    return MEMORIES_GS_OK;
}

MemoriesGsResult Memories_ClearOTagR(MemoriesMemory *memory, uint32_t table,
                                     uint32_t count)
{
    uint8_t *entries, *tail;
    uint32_t i;
    if (!memory || !count || count > (UINT32_C(1) << MEMORIES_GSOT_MAX_LENGTH)) {
        return MEMORIES_GS_ARGUMENT;
    }
    entries = Memories_Resolve(memory, table, (size_t)count * 4, 4);
    tail = word(memory, MEMORIES_OT_TAIL_FIRST);
    if (!entries || !tail) {
        return MEMORIES_GS_ADDRESS;
    }
    for (i = count - 1; i > 0; i--) {
        Memories_WriteLE32(entries + 4 * i, (table + 4 * (i - 1)) & LINK_MASK);
    }
    Memories_WriteLE32(tail, (MEMORIES_OT_TAIL_SECOND & LINK_MASK) | UINT32_C(0x04000000));
    Memories_WriteLE32(entries, MEMORIES_OT_TAIL_FIRST & LINK_MASK);
    return MEMORIES_GS_OK;
}

MemoriesGsResult Memories_GsClearOt(MemoriesMemory *memory, uint16_t offset,
                                    uint16_t point, uint32_t descriptor)
{
    uint8_t *ot = Memories_Resolve(memory, descriptor, MEMORIES_GSOT_SIZE, 4);
    uint32_t length, org;
    MemoriesGsResult result;
    if (!ot) {
        return memory ? MEMORIES_GS_ADDRESS : MEMORIES_GS_ARGUMENT;
    }
    length = Memories_ReadLE32(ot);
    org = Memories_ReadLE32(ot + 4);
    if (length > MEMORIES_GSOT_MAX_LENGTH) {
        return MEMORIES_GS_ARGUMENT;
    }
    result = Memories_ClearOTagR(memory, org, UINT32_C(1) << length);
    if (result != MEMORIES_GS_OK) {
        return result;
    }
    Memories_WriteLE32(ot + 8, offset);
    Memories_WriteLE32(ot + 12, point);
    Memories_WriteLE32(ot + 16, org + (UINT32_C(4) << length) - 4);
    return MEMORIES_GS_OK;
}

MemoriesGsResult Memories_GsSortOt(MemoriesMemory *memory, uint32_t source,
                                   uint32_t destination, size_t hop_limit)
{
    uint8_t *src = Memories_Resolve(memory, source, MEMORIES_GSOT_SIZE, 4);
    uint8_t *dst = Memories_Resolve(memory, destination, MEMORIES_GSOT_SIZE, 4);
    uint8_t *node, *splice, *entry;
    uint32_t current, previous, before, index, link;
    if (!src || !dst) {
        return memory ? MEMORIES_GS_ADDRESS : MEMORIES_GS_ARGUMENT;
    }
    current = previous = before = Memories_ReadLE32(src + 4);
    index = Memories_ReadLE32(src + 12) - Memories_ReadLE32(dst + 8);
    for (;;) {
        node = word(memory, current);
        if (!node) {
            return MEMORIES_GS_ADDRESS;
        }
        link = Memories_ReadLE32(node) & LINK_MASK;
        if (link == LINK_MASK) {
            break;
        }
        if (!hop_limit--) {
            return MEMORIES_GS_CHAIN_LIMIT;
        }
        before = previous;
        previous = current;
        current = link;
    }
    splice = word(memory, before);
    entry = word(memory, Memories_ReadLE32(dst + 4) + (index << 2));
    if (!splice || !entry) {
        return MEMORIES_GS_ADDRESS;
    }
    Memories_WriteLE32(splice, (Memories_ReadLE32(splice) & LENGTH_MASK) |
                                   (Memories_ReadLE32(entry) & LINK_MASK));
    Memories_WriteLE32(entry, (Memories_ReadLE32(entry) & LENGTH_MASK) |
                                  (Memories_ReadLE32(src + 16) & LINK_MASK));
    return MEMORIES_GS_OK;
}

MemoriesGpuResult Memories_GsCollectOt(MemoriesMemory *memory, uint32_t descriptor,
    uint32_t *words, size_t capacity, size_t hop_limit, size_t *count)
{
    uint8_t *ot = Memories_Resolve(memory, descriptor, MEMORIES_GSOT_SIZE, 4);
    if (!ot) {
        if (count) {
            *count = 0;
        }
        return memory ? MEMORIES_GPU_ADDRESS : MEMORIES_GPU_ARGUMENT;
    }
    return Memories_GpuCollect(memory, Memories_ReadLE32(ot + 16), words,
                               capacity, hop_limit, count);
}
