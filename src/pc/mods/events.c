/* Managed callbacks are ordered by priority, then registration order. A
 * snapshot makes subscribing/unsubscribing during dispatch well-defined. */
#include "events.h"
#include "../../types.h"
#include "mods.h"
#include <string.h>

typedef struct {
    int owner, token, priority;
    unsigned event;
    MemoriesModCallback callback;
} Hook;
typedef struct {
    void *data;
    size_t size;
    unsigned version;
} State;
/* Input can arrive from VBlank: dispatch must never allocate. Distinct event
 * kinds have separate snapshots; same-kind recursion is explicitly bypassed. */
#define HOOK_LIMIT 4096
static Hook hooks[HOOK_LIMIT], snapshots[MEMORIES_EVENT_COUNT][HOOK_LIMIT];
static int count, serial;
static volatile int mutating;
static State states[MODS_MAX];
static unsigned dispatching;

int Mods_Subscribe(int owner, unsigned event, int priority, MemoriesModCallback callback)
{
    int i;
    Hook hook;
    if (owner < 0 || owner >= Mods_Count() || !callback || event >= MEMORIES_EVENT_COUNT || serial == 0x7fffffff)
        return 0;
    if (count == HOOK_LIMIT)
        return 0;
    mutating = 1;
    hook.owner = owner;
    hook.event = event;
    hook.priority = priority;
    hook.callback = callback;
    hook.token = ++serial;
    for (i = count; i > 0 && hooks[i - 1].priority < priority; i--)
        hooks[i] = hooks[i - 1];
    hooks[i] = hook;
    count++;
    __asm__ volatile("" ::: "memory");
    mutating = 0;
    return hook.token;
}
void Mods_Unsubscribe(int owner, int token)
{
    int i;
    for (i = 0; i < count; i++)
        if (hooks[i].owner == owner && hooks[i].token == token) {
            mutating = 1;
            memmove(hooks + i, hooks + i + 1, (size_t)(--count - i) * sizeof(*hooks));
            __asm__ volatile("" ::: "memory");
            mutating = 0;
            return;
        }
}
void Mods_ClearHooks(int owner)
{
    int i;
    for (i = count - 1; i >= 0; i--)
        if (hooks[i].owner == owner)
            Mods_Unsubscribe(owner, hooks[i].token);
    memset(&states[owner], 0, sizeof(states[owner]));
}
int Mods_RegisterState(int owner, void *data, size_t size, unsigned version)
{
    if (owner < 0 || owner >= Mods_Count() || !data || !size || size > (1u << 20))
        return 0;
    states[owner].data = data;
    states[owner].size = size;
    states[owner].version = version;
    return 1;
}
void Mods_VisitState(void (*visit)(int, void *, size_t, unsigned, void *), void *context)
{
    int i;
    for (i = 0; i < Mods_Count(); i++)
        if (Mods_Active(i) && states[i].data)
            visit(i, states[i].data, states[i].size, states[i].version, context);
}
void Mods_Dispatch(MemoriesModEvent *event)
{
    Hook *snapshot;
    int i, n = count;
    unsigned bit;
    if (!event || event->type >= MEMORIES_EVENT_COUNT || !n || mutating)
        return;
    bit = 1u << event->type;
    if (dispatching & bit)
        return; /* Calling the original from a replacement cannot recurse. */
    snapshot = snapshots[event->type];
    memcpy(snapshot, hooks, (size_t)n * sizeof(*snapshot));
    dispatching |= bit;
    for (i = 0; i < n; i++)
        if (snapshot[i].event == event->type && Mods_Active(snapshot[i].owner)) {
            int j;
            MemoriesModEvent before = *event;
            for (j = 0; j < count; j++)
                if (hooks[j].token == snapshot[i].token)
                    break;
            if (j == count)
                continue;
            snapshot[i].callback(event);
            event->type = before.type;
            event->phase = before.phase;
            if (before.phase == MEMORIES_AFTER)
                *event = before; /* after hooks observe */
            if (event->handled && event->phase == MEMORIES_BEFORE)
                break; /* first replacement wins */
        }
    dispatching &= ~bit;
}
int Mods_Notify(unsigned type, int a, int b, int c)
{
    MemoriesModEvent event = {type, MEMORIES_BEFORE, a, b, c, 0, 0};
    Mods_Dispatch(&event);
    return event.handled;
}

int Mods_DamageLife(int side, int life, int damage, int kind)
{
    MemoriesModEvent event = {MEMORIES_EVENT_DAMAGE, MEMORIES_BEFORE, side, damage, kind, life, 0};
    Mods_Dispatch(&event);
    if (!event.handled) {
        int amount = event.b < 0 ? 0 : event.b;
        event.result = amount > life ? 0 : life - amount;
    }
    if (event.result < 0)
        event.result = 0;
    if (event.result > 32767)
        event.result = 32767;
    event.phase = MEMORIES_AFTER;
    Mods_Dispatch(&event);
    return event.result;
}
