#include "pc/memory.h"
#include "pc/rng.h"
#include "game/rand_get_interval.h"
#include "game/util_compare_s16.h"
#include <limits.h>
#include <stdio.h>

/* Unlike assert(), checks remain enabled in Release builds. */
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

static MemoriesMemory memory;

int main(void)
{
    static const int sequence[] = {16838, 5758, 10113, 17515, 31051};
    const short low = SHRT_MIN, high = SHRT_MAX;
    uint8_t *p;
    size_t i;
    Memories_Srand(1);
    for (i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        CHECK(Memories_Rand() == sequence[i]);
    }
    /* Real decompiled game code must consume our RNG, not host libc rand(). */
    Memories_Srand(1);
    CHECK(Rand_GetInterval(100) == 38);
    CHECK(Rand_GetInterval(100) == 58);
    Memories_Srand(UINT32_MAX);
    CHECK(Memories_Rand() == 15929);
    CHECK(gRand_dwSeed == UINT32_C(0xbe39e1cc));
    CHECK(Util_CompareS16(&low, &high) == -1);
    CHECK(Util_CompareS16(&high, &low) == 1);
    CHECK(Util_CompareS16(&low, &low) == 0);

    p = Memories_Resolve(&memory, 0x80000100u, 4, 4);
    CHECK(p != NULL);
    CHECK(p == Memories_Resolve(&memory, 0xa0000100u, 4, 4));
    CHECK(p == Memories_Resolve(&memory, 0x00000100u, 4, 4));
    Memories_WriteLE32(p, UINT32_C(0xfedcba98));
    CHECK(p[0] == 0x98 && p[3] == 0xfe);
    CHECK(Memories_ReadLE32(p) == UINT32_C(0xfedcba98));
    CHECK(Memories_Resolve(&memory, 0x801fffffu, 1, 1) != NULL);
    CHECK(Memories_Resolve(&memory, 0x801fffffu, 2, 1) == NULL);
    CHECK(Memories_Resolve(&memory, 0x80000100u, SIZE_MAX, 1) == NULL);
    CHECK(Memories_Resolve(&memory, 0x80000101u, 4, 4) == NULL);
    CHECK(Memories_Resolve(&memory, 0x80000100u, 4, 3) == NULL);
    CHECK(Memories_Resolve(&memory, 0x80000100u, 4, 0) == NULL);
    CHECK(Memories_Resolve(&memory, 0x1f801810u, 4, 4) == NULL);
    CHECK(Memories_Resolve(&memory, 0xc0000100u, 4, 4) == NULL);
    CHECK(Memories_Resolve(&memory, 0xffffffffu, 4, 4) == NULL);
    CHECK(Memories_Resolve(&memory, 0x00200000u, 0, 1) == NULL);
    CHECK(Memories_Resolve(NULL, 0, 1, 1) == NULL);
    p = Memories_Resolve(&memory, 0x1f800320u, 16, 16);
    CHECK(p == memory.scratchpad + 0x320);
    CHECK(p == Memories_Resolve(&memory, 0x9f800320u, 16, 16));
    CHECK(Memories_Resolve(&memory, 0xbf800320u, 16, 16) == NULL);
    CHECK(Memories_Resolve(&memory, 0x1f8003ffu, 1, 1) != NULL);
    CHECK(Memories_Resolve(&memory, 0x1f8003ffu, 2, 1) == NULL);
    puts("PC core: game RNG/comparator, guest aliases and bounds passed");
    return 0;
}
