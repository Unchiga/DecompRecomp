#include <psyz.h>
#include "psyz_gpu.h"

/* queue_buf[0x4000] in the pinned PSY-Z libgpu.c. GP0 writes silently drop
 * words when full, so flush only at complete command boundaries below. */
#define PSYZ_GP0_CAPACITY 0x4000u

static int flush(void)
{
    /* Pinned backend is synchronous. A future asynchronous backend needs a
     * bounded pump/wait contract instead of an unbounded polling loop here. */
    return Psyz_GpuExeque() == 0;
}

MemoriesGpuResult Memories_PsyzSubmit(const uint32_t *words, size_t count)
{
    size_t offset = 0, queued = 0;
    MemoriesGpuResult result = Memories_GpuValidate(words, count);
    if (result != MEMORIES_GPU_OK) return result;
    if (!count) return MEMORIES_GPU_OK;
    /* Drain SDK commands queued before this submission. */
    if (!flush()) return MEMORIES_GPU_BACKEND_ERROR;
    while (offset < count) {
        size_t i, length = Memories_GpuCommandWords(words[offset]);
        if (length > PSYZ_GP0_CAPACITY - queued) {
            if (!flush()) return MEMORIES_GPU_BACKEND_ERROR;
            queued = 0;
        }
        for (i = 0; i < length; ++i) Psyz_GpuWriteGP0(words[offset++]);
        queued += length;
    }
    return flush() ? MEMORIES_GPU_OK : MEMORIES_GPU_BACKEND_ERROR;
}
