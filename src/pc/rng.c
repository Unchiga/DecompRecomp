#include "rng.h"
#include <limits.h>

_Static_assert(UINT_MAX == UINT32_MAX, "Game ABI requires 32-bit unsigned int");
uint32_t gRand_dwSeed;
void (*Memories_RandHook)(void *caller);

int Memories_Rand(void)
{
    if (Memories_RandHook) Memories_RandHook(__builtin_return_address(0));
    gRand_dwSeed = gRand_dwSeed * UINT32_C(1103515245) + UINT32_C(12345);
    return (int)((gRand_dwSeed >> 16) & UINT32_C(0x7fff));
}

void Memories_Srand(unsigned int seed)
{
    gRand_dwSeed = seed;
}
