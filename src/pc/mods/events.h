#ifndef MEMORIES_MOD_EVENTS_H
#define MEMORIES_MOD_EVENTS_H
#include "modapi.h"
int Mods_Subscribe(int owner, unsigned event, int priority, MemoriesModCallback callback);
void Mods_Unsubscribe(int owner, int token);
void Mods_ClearHooks(int owner);
int Mods_RegisterState(int owner, void *data, size_t size, unsigned version);
void Mods_VisitState(void (*visit)(int owner, void *data, size_t size, unsigned version, void *context), void *context);
#endif
