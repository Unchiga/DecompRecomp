#ifndef MEMORIES_PC_DEBUG_SYMBOLS_H
#define MEMORIES_PC_DEBUG_SYMBOLS_H
#include <stdint.h>

int Symbols_Load(void);
const char *Symbols_Lookup(uintptr_t address, uintptr_t *offset);

#endif
