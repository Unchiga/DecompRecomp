/* Runtime-loaded modules. Several modules are built for the same load
 * address (0x80168000) and the game swaps them by reading a package over that
 * range, so two things that the hardware got for free need doing here:
 *
 * - A call through a guest address inside a shared bank is ambiguous. Every
 *   module image starts with its identifier word, so the function map is
 *   filtered by the identifier currently in guest memory.
 * - Loading a module gave it fresh initialized data. The native module's
 *   variables live in host sections of their own (the build renames them), a
 *   copy of which is taken at startup and put back whenever the module's
 *   first sector arrives. */
#include "pc/render/texture_dump.h"
#include "image.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char **snapshots;

int Memories_ModulesInit(void)
{
    unsigned i;
    snapshots = calloc(Memories_ModuleCount ? Memories_ModuleCount : 1, sizeof(*snapshots));
    if (!snapshots) {
        return -1;
    }
    for (i = 0; i < Memories_ModuleCount; i++) {
        const MemoriesModule *module = &Memories_Modules[i];
        size_t size = (size_t)(module->data_end - module->data);
        snapshots[i] = malloc(size ? size : 1);
        if (!snapshots[i]) {
            return -1;
        }
        memcpy(snapshots[i], module->data, size);
    }
    return 0;
}

int Memories_ModuleIsResident(unsigned bank, unsigned identifier)
{
    return !bank || *(const uint32_t *)(uintptr_t)bank == identifier;
}

/* Async-signal-safe: sectors are delivered from the disc interrupt. */
void Memories_GuestWritten(void *destination, size_t length)
{
    TextureDump_Written(destination, (unsigned)length); /* a delivery no longer describes these bytes */
    uintptr_t first = (uintptr_t)destination;
    unsigned i;
    for (i = 0; snapshots && i < Memories_ModuleCount; i++) {
        const MemoriesModule *module = &Memories_Modules[i];
        if (first <= module->bank && module->bank + 4 <= first + length &&
            *(const uint32_t *)(uintptr_t)module->bank == module->identifier) {
            memcpy(module->data, snapshots[i], (size_t)(module->data_end - module->data));
            memset(module->bss, 0, (size_t)(module->bss_end - module->bss));
        }
    }
}
