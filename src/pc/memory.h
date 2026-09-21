#ifndef MEMORIES_PC_MEMORY_H
#define MEMORIES_PC_MEMORY_H
#include <stddef.h>
#include <stdint.h>

#define MEMORIES_RAM_SIZE 0x200000u
#define MEMORIES_SCRATCHPAD_SIZE 0x400u

/* Guest-layout storage for adapters/fixtures, not a host address-space mapping.
 * MMIO, BIOS, overlay execution and RAM mirrors above 2 MiB are not supported. */
typedef struct MemoriesMemory {
    _Alignas(16) uint8_t ram[MEMORIES_RAM_SIZE];
    _Alignas(16) uint8_t scratchpad[MEMORIES_SCRATCHPAD_SIZE];
} MemoriesMemory;

/* Return NULL for an invalid span/alignment, even if only its final byte is bad.
 * Zero-length spans are allowed only at addresses inside a supported region. */
void *Memories_Resolve(MemoriesMemory *memory, uint32_t address,
                       size_t length, size_t alignment);
uint32_t Memories_ReadLE32(const uint8_t *bytes);
void Memories_WriteLE32(uint8_t *bytes, uint32_t value);
#endif
