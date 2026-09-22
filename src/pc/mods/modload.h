#ifndef MEMORIES_PC_MODS_MODLOAD_H
#define MEMORIES_PC_MODS_MODLOAD_H
/* The game's own loader for a mod's library (modload.c), the same on every
 * platform: one `.mod` file, an i386 ELF shared object, runs on Linux and on
 * Windows alike (notes/modding.md, "Native mods").
 *
 * A mod is linked to nothing the operating system offers. Every name it
 * leaves undefined is looked up in two tables, and a name in neither refuses
 * the load with that name as the reason:
 *
 *  - the C library a mod is given (modlibc.c, headers in src/pc/mods/libc):
 *    memory, strings, numbers, maths, time and formatted text, and reading
 *    and writing the files the host hands it. There is no fopen, no
 *    remove or rename, no socket, no process and no other library;
 *  - the game and the port's own code and data (Memories_ModSymbols, which
 *    the build generates), leaving out the platform layer that reaches the
 *    operating system. */
#include <stddef.h>

typedef struct {
    const char *name;
    void *address;
} ModSymbol;

/* Sorted by name. The generated table is the build's (tools/pc/build_game32.py);
 * a test links its own. */
extern const ModSymbol Memories_ModSymbols[];
extern const unsigned Memories_ModSymbolCount;
/* modlibc.c's table, sorted by name. */
extern const ModSymbol Memories_ModLibc[];
extern const unsigned Memories_ModLibcCount;

/* Points the table's stdout and stderr at the host's; ModLoad_Open calls it. */
void ModLibc_Start(void);

/* What a name a mod leaves undefined resolves to, or NULL. */
void *ModLoad_Resolve(const char *name);

/* Load, relocate and initialise a library. On failure NULL, and `error`
 * says why. A loaded library stays for as long as the game runs. */
void *ModLoad_Open(const char *path, char *error, size_t error_size);
/* A symbol the library defines, or NULL. */
void *ModLoad_Symbol(void *library, const char *name);

#endif
