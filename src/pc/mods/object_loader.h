#ifndef MEMORIES_PC_MODS_OBJECT_LOADER_H
#define MEMORIES_PC_MODS_OBJECT_LOADER_H
/* The loader for code mods: one 32-bit x86 ELF relocatable object (`.o`),
 * the same file on Linux and on Windows (notes/modding.md). Both builds are
 * 32-bit x86 with the same calling convention, so the machine code in a mod
 * runs on either; only the container differs, and this reads the one
 * container on both.
 *
 * Loading lays the object's allocated sections out in fresh memory, binds
 * each name it leaves undefined through `resolve` (the game's export table
 * and the host's C library, exports.h), applies its relocations, and then
 * makes the code executable and nothing else. The file is untrusted input:
 * anything malformed, and anything this loader does not do, fails with a
 * message and leaves nothing behind. What it does not do, on purpose:
 * position-independent code (a GOT), thread-local storage, COMMON symbols,
 * and constructors. tools/pc/build_mod.py builds objects without any of
 * them. */
#include <stddef.h>
#include <stdint.h>

typedef void *(*ObjectResolver)(const char *name, void *context);

typedef struct {
    unsigned char *image;   /* code, then data, in one block */
    size_t size, code_size;
    /* The object's own global and local functions and objects, for finding
     * its entry point and for crash reports. Names point into `strings`. */
    struct ObjectSymbol { const char *name; uintptr_t address, size; int function, global; } *symbols;
    size_t symbol_count;
    char *strings;
} LoadedObject;

/* Returns 0 and fills `object`, or -1 with the reason in `error`. */
int ObjectLoader_Load(const void *data, size_t size, ObjectResolver resolve, void *context,
                      LoadedObject *object, char *error, size_t error_size);
/* A global symbol the object defines, or NULL. */
void *ObjectLoader_Symbol(const LoadedObject *object, const char *name);
/* Gives the memory back. The game never does this for a mod (code it may
 * still hold pointers into); the tests do. */
void ObjectLoader_Free(LoadedObject *object);

#endif
