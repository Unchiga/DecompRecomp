#ifndef MEMORIES_PC_RNG_H
#define MEMORIES_PC_RNG_H
#include <stdint.h>

/* Exact game-visible Psy-Q RNG; independent of host libc and SDK RNGs. */
extern uint32_t gRand_dwSeed;
int Memories_Rand(void);
void Memories_Srand(unsigned int seed);
/* Called with the caller's address on every draw when set (ai_trace.c). */
extern void (*Memories_RandHook)(void *caller);
#endif
