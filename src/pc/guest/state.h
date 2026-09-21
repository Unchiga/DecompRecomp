#ifndef MEMORIES_PC_GUEST_STATE_H
#define MEMORIES_PC_GUEST_STATE_H
/* Save states. A state is taken and resumed at one kind of place only: a
 * VSync(0) call made from game code. What is stored there is
 *
 * - guest RAM and the scratchpad,
 * - the game objects' host variables (game_data/game_bss and each module's
 *   ovl_* sections),
 * - the game stack above the VSync call and the callee-saved registers the
 *   caller expects back,
 * - each native subsystem's state, as a tagged chunk it describes itself.
 *
 * States are meant to outlive a rebuild of the native side, which is the
 * common debugging loop (reach a stub, implement it, come back). For that the
 * build links every game object's code and variables at fixed addresses and
 * the game runs on a stack mapped at a fixed address, so return addresses and
 * pointers held in a state still mean the same thing. Nothing native is
 * stored by address. A state does not survive a change to game sources; the
 * header carries a fingerprint of the game code and a mismatch is reported.
 *
 * Words of game data that the linker relocated (pointers to native
 * functions, for instance) differ between builds. The file keeps the
 * startup image of that data beside the saved one, and a word still at its
 * startup value is taken from the running build instead. */
#include <stddef.h>
#include <stdint.h>

typedef struct MemoriesState MemoriesState;
typedef struct MemoriesStateField {
    void *data;
    size_t size;
} MemoriesStateField;

/* Subsystems: save or load one chunk made of these fields, in order. When
 * loading, a missing chunk or one of another size (a layout that changed
 * since) is reported and left alone. Returns 1 if the fields were loaded. */
int Memories_StateChunk(MemoriesState *state, const char *tag, const MemoriesStateField *fields, size_t count);
int Memories_StateLoading(const MemoriesState *state);

/* Registers on entry to VSync, written by the assembly entry (state_i386.S). */
typedef struct MemoriesStateEntry {
    uint32_t ebx, esi, edi, ebp, esp; /* esp points at the return address */
} MemoriesStateEntry;
extern MemoriesStateEntry Memories_StateEntry;

/* main(): run `entry` on the fixed game stack. Does not return. */
int Memories_StateRunGame(int (*entry)(void));
/* VSync(0), after presenting: act on a pending save or load request. */
void Memories_StatePoint(unsigned presented_frames);
/* 1 save, 2 load; taken up at the next state point. Async-signal-safe. */
void Memories_StateRequest(int what, int slot);
/* Locate the running build's symbol table beside the executable. */
int Memories_SymbolTablePath(char *out, size_t size);
int Memories_LastStateSlot(void);

/* Each subsystem's chunk. */
void Spu_State(MemoriesState *state);
void LibSpu_State(MemoriesState *state);
void LibDs_State(MemoriesState *state);
void LibEtc_State(MemoriesState *state);
void LibGpu_State(MemoriesState *state);
void LibGte_State(MemoriesState *state);
void LibPress_State(MemoriesState *state);
void LibMcrd_State(MemoriesState *state);
void Platform_State(MemoriesState *state);
#endif
