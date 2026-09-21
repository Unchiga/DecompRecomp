#ifndef MEMORIES_PC_GUEST_MIPS_H
#define MEMORIES_PC_GUEST_MIPS_H
#include <stdint.h>

/* Run a routine of a loaded overlay (see mips.c). `args` are the MIPS
 * arguments in order: the first four go in a0-a3, the rest on the stack.
 * Returns 0 and the routine's v0 in *result, or -1 after printing why the
 * routine could not be run, in which case the caller falls back. */
int Memories_MipsTry(uint32_t address, const uint32_t *args, unsigned count, uint32_t *result);
/* As above with four arguments; a failure ends the process. */
uint32_t Memories_MipsCall(uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
/* Whether an address lies in a slot whose code is interpreted. */
int Memories_MipsInOverlay(uint32_t address);
/* The guest-call fault handler's way into the interpreter: it stores the
 * called address here and resumes in the thunk. */
extern uint32_t Memories_MipsThunkTarget;
uint32_t Memories_MipsThunk(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                            uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11);
#endif
