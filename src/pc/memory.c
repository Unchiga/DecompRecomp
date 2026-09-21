#include "memory.h"

void *Memories_Resolve(MemoriesMemory *memory, uint32_t address,
                       size_t length, size_t alignment)
{
    uint8_t *base;
    uint32_t physical;
    size_t offset, capacity;
    if (!memory || !alignment || alignment > 16 ||
        (alignment & (alignment - 1)) || (address & (alignment - 1))) {
        return NULL;
    }
    /* Accept physical, KSEG0 and KSEG1; reject mapped/privileged segments. */
    if (address < UINT32_C(0x20000000)) {
        physical = address;
    } else if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000)) {
        physical = address & UINT32_C(0x1fffffff);
    } else {
        return NULL;
    }
    if (physical < MEMORIES_RAM_SIZE) {
        base = memory->ram;
        capacity = MEMORIES_RAM_SIZE;
        offset = physical;
    } else if (physical >= UINT32_C(0x1f800000) && physical < UINT32_C(0x1f800400) &&
               address < UINT32_C(0xa0000000)) {
        base = memory->scratchpad;
        capacity = MEMORIES_SCRATCHPAD_SIZE;
        offset = physical - UINT32_C(0x1f800000);
    } else {
        return NULL;
    }
    if (length > capacity - offset) {
        return NULL;
    }
    return base + offset;
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
