#ifndef MEMORIES_PC_PACKETS_H
#define MEMORIES_PC_PACKETS_H
#include "pc/memory.h"

typedef enum MemoriesGpuResult {
    MEMORIES_GPU_OK,
    MEMORIES_GPU_ARGUMENT,
    MEMORIES_GPU_ADDRESS,
    MEMORIES_GPU_CHAIN_LIMIT,
    MEMORIES_GPU_CAPACITY,
    MEMORIES_GPU_UNSUPPORTED_COMMAND,
    MEMORIES_GPU_TRUNCATED_COMMAND,
    MEMORIES_GPU_BACKEND_ERROR
} MemoriesGpuResult;

/* Flatten a Psy-Q DMA list (24-bit links, 8-bit lengths) from guest RAM.
 * Empty OT entries are traversed, terminal packet payloads are included.
 * count is zero on failure; output may contain a partial snapshot but must not
 * be submitted. No live guest pointers escape. RAM must not change during copy.
 * hop_limit bounds empty/cyclic lists as well as lists containing payloads. */
MemoriesGpuResult Memories_GpuCollect(MemoriesMemory *memory, uint32_t head,
    uint32_t *words, size_t capacity, size_t hop_limit, size_t *count);

/* Fixed-size GP0 drawing subset supported by the pinned backend. Commands may
 * span DMA packets. Transfer commands and polylines fail explicitly for now. */
size_t Memories_GpuCommandWords(uint32_t command);
MemoriesGpuResult Memories_GpuValidate(const uint32_t *words, size_t count);
const char *Memories_GpuResultName(MemoriesGpuResult result);
#endif
