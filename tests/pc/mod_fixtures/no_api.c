/* A mod whose MemoriesModInit says yes but never sets mod->api: the loader
 * refuses it rather than guess which table it filled in
 * (tests/pc/mods_lifecycle_test.c). */
#include "types.h"
#include "pc/mods/modapi.h"
static const MemoriesModHost *host;
static void callback(void) { host->set_setting(host, "unexpected_callback", 1); }
int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    mod->frame = mod->reset = callback;
    return 1;
}
