#ifndef MEMORIES_MOD_HOOKS_H
#define MEMORIES_MOD_HOOKS_H
/* Function hooks for code mods (hooks.c, mod API 4). */
/* Replace `function`, a game function, with `replacement` for `owner`'s mod
 * while it is applied; *original (when given) is kept pointing at what the
 * replacement should call to run the function it displaced. A token, or 0
 * when the function cannot be hooked (not a game function, or full). */
int Hooks_Add(int owner, void *function, void *replacement, void **original);
void Hooks_Remove(int owner, int token);
void Hooks_Clear(int owner);
/* Rebuild every chain from the mods now applied (Mods_Active). */
void Hooks_Relink(void);
#endif
