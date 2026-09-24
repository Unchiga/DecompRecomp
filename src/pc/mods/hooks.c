/* Function hooks (mod API 4): a code mod replaces or wraps one of the game's
 * own functions, by address, and gets the function it displaced back to call.
 *
 * Every game unit is compiled with -fpatchable-function-entry=8,6
 * (tools/pc/build_game32.py): six bytes of nops before the function's entry
 * and two at it. A hooked function's six bytes become `jmp *slot`, an
 * indirect jump through a pointer kept here, and its two become `jmp -8`,
 * back into them. The two-byte store is the only one made to code a thread
 * may be running, and it is one aligned-enough write; everything after that
 * changes only `slot` and the mods' `original` pointers, which are words.
 *
 * Hooks on one function chain in the order they were made: the one made
 * last is called first, and its `original` leads to the one before, down to
 * the game's own code. Only applied mods are in a chain; Hooks_Relink
 * rebuilds every chain when a mod is applied or removed. A function no
 * applied mod hooks gets its two nops back. */
#include "hooks.h"
#include "mods.h"
#include "../compat/mman.h"
#include <stdint.h>
#include <string.h>

#define PRE 6     /* nops before the entry */
#define TARGETS_MAX 1024
#define HOOKS_MAX 2048

typedef struct {
    unsigned char *entry;
    unsigned char saved[2];  /* the entry's own two nops */
    void *volatile slot;     /* where `jmp *slot` goes */
    int patched;
} Target;

typedef struct {
    int owner, token, target;
    void *replacement;
    void **original;
} Hook;

static Target targets[TARGETS_MAX];
static int target_count;
static Hook hooks[HOOKS_MAX];
static int hook_count, serial;

static int writable(unsigned char *from, size_t size, int on)
{
    uintptr_t page = 4096, start = (uintptr_t)from & ~(page - 1), end = ((uintptr_t)from + size + page - 1) & ~(page - 1);
    return mprotect((void *)start, end - start, on ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_EXEC) == 0;
}

/* The six bytes before, and the two at, the entry a patchable function has.
 * GCC writes single-byte nops; clang writes 66 90 at the entry. */
static int patchable(const unsigned char *entry)
{
#ifndef __i386__
    if (entry) return 0;   /* `jmp *[abs32]` is the 32-bit game's; 64-bit host tests hook nothing */
#endif
    for (int i = -PRE; i < 0; i++) if (entry[i] != 0x90) return 0;
    return (entry[0] == 0x90 && entry[1] == 0x90) || (entry[0] == 0x66 && entry[1] == 0x90);
}

static int find_target(unsigned char *entry)
{
    int i;
    for (i = 0; i < target_count; i++) if (targets[i].entry == entry) return i;
    if (target_count == TARGETS_MAX || !patchable(entry)) return -1;
    targets[i].entry = entry;
    memcpy(targets[i].saved, entry, 2);
    targets[i].slot = entry + 2;
    targets[i].patched = 0;
    /* The jump before the entry is written once and never changes: until
     * the entry points at it, nothing runs it. */
    if (!writable(entry - PRE, PRE + 2, 1)) return -1;
    entry[-6] = 0xFF;
    entry[-5] = 0x25;   /* jmp *[slot] */
    {
        uint32_t slot = (uint32_t)(uintptr_t)&targets[i].slot;
        memcpy(entry - 4, &slot, 4);
    }
    writable(entry - PRE, PRE + 2, 0);
    return target_count++;
}

static void set_entry(Target *target, int hooked)
{
    uint16_t value;
    if (hooked == target->patched) return;
    if (hooked) value = (uint16_t)(0xEB | (0xF8 << 8));   /* jmp -8: back to the jmp *[slot] */
    else memcpy(&value, target->saved, 2);
    if (!writable(target->entry, 2, 1)) return;
    __atomic_store_n((uint16_t *)target->entry, value, __ATOMIC_SEQ_CST);
    writable(target->entry, 2, 0);
    target->patched = hooked;
}

void Hooks_Relink(void)
{
    for (int t = 0; t < target_count; t++) {
        void *next = targets[t].entry + 2;   /* the game's own code, after the nops */
        for (int h = 0; h < hook_count; h++) {
            if (hooks[h].target != t || !Mods_Active(hooks[h].owner)) continue;
            if (hooks[h].original) __atomic_store_n(hooks[h].original, next, __ATOMIC_SEQ_CST);
            next = hooks[h].replacement;
        }
        /* An unapplied mod's `original` still leads somewhere real. */
        for (int h = 0; h < hook_count; h++) {
            if (hooks[h].target == t && !Mods_Active(hooks[h].owner) && hooks[h].original)
                __atomic_store_n(hooks[h].original, (void *)(targets[t].entry + 2), __ATOMIC_SEQ_CST);
        }
        __atomic_store_n(&targets[t].slot, next, __ATOMIC_SEQ_CST);
        set_entry(&targets[t], next != (void *)(targets[t].entry + 2));
    }
}

int Hooks_Add(int owner, void *function, void *replacement, void **original)
{
    int target;
    if (!function || !replacement || function == replacement || hook_count == HOOKS_MAX || serial == 0x7fffffff) return 0;
    target = find_target(function);
    if (target < 0) return 0;
    if (original) *original = targets[target].entry + 2;
    hooks[hook_count++] = (Hook){owner, ++serial, target, replacement, original};
    Hooks_Relink();
    return serial;
}

void Hooks_Remove(int owner, int token)
{
    for (int i = 0; i < hook_count; i++) {
        if (hooks[i].owner != owner || hooks[i].token != token) continue;
        memmove(hooks + i, hooks + i + 1, (size_t)(--hook_count - i) * sizeof(*hooks));
        Hooks_Relink();
        return;
    }
}

void Hooks_Clear(int owner)
{
    int kept = 0;
    for (int i = 0; i < hook_count; i++) if (hooks[i].owner != owner) hooks[kept++] = hooks[i];
    if (kept == hook_count) return;
    hook_count = kept;
    Hooks_Relink();
}
