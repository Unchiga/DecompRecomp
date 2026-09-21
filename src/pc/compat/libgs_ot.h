#ifndef MEMORIES_PC_LIBGS_OT_H
#define MEMORIES_PC_LIBGS_OT_H
#include "pc/render/packets.h"

/* Guest GsOT: length, org, offset, point, tag; five little-endian u32 words. */
#define MEMORIES_GSOT_SIZE 20u
#define MEMORIES_GSOT_MAX_LENGTH 14u
/* Resident LIBGPU tail installed by ClearOTagR: first -> second -> 0xFFFFFF. */
#define MEMORIES_OT_TAIL_FIRST UINT32_C(0x80094728)
#define MEMORIES_OT_TAIL_SECOND UINT32_C(0x80094714)

typedef enum MemoriesGsResult {
    MEMORIES_GS_OK,
    MEMORIES_GS_ARGUMENT,
    MEMORIES_GS_ADDRESS,
    MEMORIES_GS_CHAIN_LIMIT
} MemoriesGsResult;

/* Write the retail initial words of both tail nodes. Only for fixtures and
 * hosts that have not loaded the executable's data segment. */
MemoriesGsResult Memories_OtInstallTail(MemoriesMemory *memory);
/* Reverse-link count entries like the OTC DMA (top byte zero, physical links),
 * then point entry 0 at the tail and relink the first tail node's tag. */
MemoriesGsResult Memories_ClearOTagR(MemoriesMemory *memory, uint32_t table,
                                     uint32_t count);
/* 0x80085DB0. Nothing is written when the descriptor or table is invalid. */
MemoriesGsResult Memories_GsClearOt(MemoriesMemory *memory, uint16_t offset,
                                    uint16_t point, uint32_t descriptor);
/* 0x80085E10. The node linking to the node before the terminator takes over
 * the destination entry's link, which drops the source's two tail nodes.
 * hop_limit bounds the source walk; nothing is written on failure. */
MemoriesGsResult Memories_GsSortOt(MemoriesMemory *memory, uint32_t source,
                                   uint32_t destination, size_t hop_limit);
/* 0x80085D80 up to the DrawOTag boundary: snapshot the chain at descriptor+16. */
MemoriesGpuResult Memories_GsCollectOt(MemoriesMemory *memory, uint32_t descriptor,
    uint32_t *words, size_t capacity, size_t hop_limit, size_t *count);
#endif
