#include "types.h"
#include "pc/mods/modapi.h"
static const MemoriesModHost *host;
static int state;
static void callback(void) { host->set_setting(host, "unexpected_callback", 1); }
static void event(MemoriesModEvent *e) { (void)e; callback(); }
int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    host = from;
    if (host->api < 3) return 0;
    mod->api = 3;
    mod->frame = mod->reset = mod->shutdown = callback;
    host->subscribe(host, MEMORIES_EVENT_DAMAGE, 0, event);
    host->register_state(host, &state, sizeof(state), 1);
    return 0; /* Every callback/registration must be discarded. */
}
