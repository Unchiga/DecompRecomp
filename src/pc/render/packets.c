#include "packets.h"

MemoriesGpuResult Memories_GpuCollect(MemoriesMemory *memory, uint32_t head,
    uint32_t *words, size_t capacity, size_t hop_limit, size_t *count)
{
    size_t used = 0, hops = 0;
    if (!count) return MEMORIES_GPU_ARGUMENT;
    *count = 0;
    if (!memory || (!words && capacity) || !hop_limit) return MEMORIES_GPU_ARGUMENT;
    /* The first pointer can be a CPU alias. Links inside tags are physical. */
    if ((head >= 0x80000000u && head < 0x80200000u) ||
        (head >= 0xa0000000u && head < 0xa0200000u)) head &= 0x1fffffffu;
    while (head != 0x00ffffffu) {
        uint8_t *packet;
        uint32_t tag;
        size_t length, i;
        if (hops++ >= hop_limit) return MEMORIES_GPU_CHAIN_LIMIT;
        if (head >= MEMORIES_RAM_SIZE) return MEMORIES_GPU_ADDRESS;
        packet = Memories_Resolve(memory, head, 4, 4);
        if (!packet) return MEMORIES_GPU_ADDRESS;
        tag = Memories_ReadLE32(packet);
        length = tag >> 24;
        if (!Memories_Resolve(memory, head, (length + 1) * 4, 4))
            return MEMORIES_GPU_ADDRESS;
        if (length > capacity - used) return MEMORIES_GPU_CAPACITY;
        for (i = 0; i < length; ++i)
            words[used++] = Memories_ReadLE32(packet + 4 + i * 4);
        head = tag & 0x00ffffffu;
    }
    *count = used;
    return MEMORIES_GPU_OK;
}

size_t Memories_GpuCommandWords(uint32_t command)
{
    unsigned op = command >> 24;
    if (op >= 0x20 && op <= 0x3f) {
        size_t vertices = (op & 8) ? 4 : 3;
        size_t textured = (op & 4) ? 1 : 0;
        size_t shaded = (op & 16) ? 1 : 0;
        return 1 + vertices * (1 + textured + shaded) - shaded;
    }
    if (op >= 0x40 && op <= 0x47) return 3;
    if (op >= 0x50 && op <= 0x57) return 4;
    if (op >= 0x60 && op <= 0x7f)
        return 2 + ((op & 4) ? 1 : 0) + ((op & 0x18) ? 0 : 1);
    if (op >= 0xe1 && op <= 0xe6) return 1;
    if (op == 0x00) return 1; /* Retail ClearOTagR's terminal packet has NOPs. */
    if (op == 0x02) return 3;
    if (op == 0x80) return 4;
    return 0;
}

MemoriesGpuResult Memories_GpuValidate(const uint32_t *words, size_t count)
{
    size_t offset = 0;
    if (!words && count) return MEMORIES_GPU_ARGUMENT;
    while (offset < count) {
        size_t length = Memories_GpuCommandWords(words[offset]);
        if (!length) return MEMORIES_GPU_UNSUPPORTED_COMMAND;
        if (length > count - offset) return MEMORIES_GPU_TRUNCATED_COMMAND;
        offset += length;
    }
    return MEMORIES_GPU_OK;
}

const char *Memories_GpuResultName(MemoriesGpuResult result)
{
    switch (result) {
    case MEMORIES_GPU_OK: return "ok";
    case MEMORIES_GPU_ARGUMENT: return "invalid argument";
    case MEMORIES_GPU_ADDRESS: return "invalid guest packet address/span";
    case MEMORIES_GPU_CHAIN_LIMIT: return "packet chain exceeds hop limit (possibly cyclic)";
    case MEMORIES_GPU_CAPACITY: return "packet snapshot capacity exceeded";
    case MEMORIES_GPU_UNSUPPORTED_COMMAND: return "unsupported GP0 command";
    case MEMORIES_GPU_TRUNCATED_COMMAND: return "truncated GP0 command";
    case MEMORIES_GPU_BACKEND_ERROR: return "GPU backend failed";
    }
    return "unknown GPU error";
}
