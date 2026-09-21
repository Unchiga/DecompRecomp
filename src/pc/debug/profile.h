#ifndef MEMORIES_PC_DEBUG_PROFILE_H
#define MEMORIES_PC_DEBUG_PROFILE_H
#include <stdint.h>

void Profile_Init(void);
void Profile_Sample(uintptr_t address);

#endif
