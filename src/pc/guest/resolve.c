/* Memories_Resolve for the mapped ILP32 image: a guest address is already a
 * host address, so this only keeps the range and alignment checks. */
#include "image.h"
#include "pc/memory.h"

void *Memories_Resolve(MemoriesMemory *memory, uint32_t address,
                       size_t length, size_t alignment)
{
    uint32_t physical = address & UINT32_C(0x1fffffff);
    (void)memory;
    if (!alignment || (alignment & (alignment - 1)) || (address & (alignment - 1)) ||
        (address >= UINT32_C(0x20000000) && (address < MEMORIES_GUEST_RAM ||
                                             address >= UINT32_C(0xc0000000)))) {
        return NULL;
    }
    if (physical >= UINT32_C(0x10000) && physical < MEMORIES_GUEST_RAM_SIZE) {
#ifdef _WIN32
        /* Windows holds most of the low mirror (image.c): use guest RAM. */
        if (address < UINT32_C(0x20000000)) address |= MEMORIES_GUEST_RAM;
#endif
        return length <= MEMORIES_GUEST_RAM_SIZE - physical ? (void *)(uintptr_t)address : NULL;
    }
    if (physical >= UINT32_C(0x1f800000) && physical < UINT32_C(0x1f800400) &&
        address < UINT32_C(0xa0000000)) {
        return length <= UINT32_C(0x1f800400) - physical ? (void *)(uintptr_t)address : NULL;
    }
    return NULL;
}

uint32_t Memories_ReadLE32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

void Memories_WriteLE32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}
