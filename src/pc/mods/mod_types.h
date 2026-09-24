#ifndef MEMORIES_MOD_TYPES_H
#define MEMORIES_MOD_TYPES_H
#define MEMORIES_MOD_API 4
/* API 3: before hooks may alter arguments/result, or set handled to replace
 * the operation (including cancellation). Highest priority runs first;
 * equal priorities follow registration/load order. After hooks observe the
 * operation result. Input runs from VBlank (possibly interrupt context):
 * no allocation, file I/O or registration there. Keep all callbacks short. */
enum { MEMORIES_BEFORE, MEMORIES_AFTER };
enum {
    MEMORIES_EVENT_INPUT, MEMORIES_EVENT_DAMAGE, MEMORIES_EVENT_REWARD,
    MEMORIES_EVENT_FUSION, MEMORIES_EVENT_EFFECT, MEMORIES_EVENT_AI,
    MEMORIES_EVENT_SCENE, MEMORIES_EVENT_SAVE, MEMORIES_EVENT_LOAD,
    MEMORIES_EVENT_SETTINGS, MEMORIES_EVENT_EQUIP,
    /* API 4, after only: the save slot menu saved the running game (a slot,
     * from 0; b the slot's token, save_slots.h; c the save's sequence), or
     * loaded it (the same). What a mod keeps per save goes in its own file
     * named after the token (open_data), so each slot has its own. */
    MEMORIES_EVENT_SLOT_SAVE, MEMORIES_EVENT_SLOT_LOAD,
    MEMORIES_EVENT_COUNT
};
typedef struct {
    unsigned type, phase;
    int a, b, c, result, handled;
} MemoriesModEvent;
typedef void (*MemoriesModCallback)(MemoriesModEvent *event);

#endif
