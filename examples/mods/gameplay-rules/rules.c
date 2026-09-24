#include "types.h"
#include "pc/mods/modapi.h"
static const MemoriesModHost *host;
/* No pointers: this buffer can be copied into and out of a save state. */
static struct { u32 damage_events; } state;
static void damage(MemoriesModEvent *event)
{
    if (event->phase == MEMORIES_BEFORE) {
        int percent = host->setting(host, "damage_percent", 100);
        if (percent < 0) percent = 0;
        if (percent > 300) percent = 300;
        event->b = (int)((s64)event->b * percent / 100);
        state.damage_events++;
    }
}
static void fusion(MemoriesModEvent *event)
{
    if (event->phase != MEMORIES_BEFORE || !host->setting(host, "fusion", 0)) return;
    if ((event->a == 1 && event->b == 2) || (event->a == 2 && event->b == 1)) {
        event->result = 3;
        event->handled = 1;
    }
}
int MemoriesModInit(const MemoriesModHost *from, MemoriesMod *mod)
{
    if (from->api < 3) return 0;
    host = from;
    mod->api = 3;
    return host->register_state(host, &state, sizeof(state), 1) &&
           host->subscribe(host, MEMORIES_EVENT_DAMAGE, 0, damage) &&
           host->subscribe(host, MEMORIES_EVENT_FUSION, 0, fusion);
}
