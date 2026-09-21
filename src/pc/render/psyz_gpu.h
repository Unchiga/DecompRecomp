#ifndef MEMORIES_PC_PSYZ_GPU_H
#define MEMORIES_PC_PSYZ_GPU_H
#include "packets.h"

/* Caller initializes ResetGraph/draw environment and owns the GPU on this
 * thread. Snapshot must not change during submission. Entire stream validates
 * before any word is submitted; runtime backend failures can leave partial work. */
MemoriesGpuResult Memories_PsyzSubmit(const uint32_t *words, size_t count);
#endif
