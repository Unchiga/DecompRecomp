#ifndef MEMORIES_PC_DEBUG_SYMBOLS_H
#define MEMORIES_PC_DEBUG_SYMBOLS_H
#include <stddef.h>
#include <stdint.h>

int Symbols_Load(void);
const char *Symbols_Lookup(uintptr_t address, uintptr_t *offset);
/* Code loaded after startup (a mod's object file, src/pc/mods): its
 * functions, so crash and hang reports can name them. One call per object. */
typedef struct {
    uintptr_t address, size;
    const char *name;
} SymbolsEntry;
int Symbols_Add(const SymbolsEntry *entries, size_t count);

#endif
