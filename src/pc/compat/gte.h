#ifndef MEMORIES_PC_GTE_H
#define MEMORIES_PC_GTE_H
#include <stdint.h>

/* Software COP2. One instance, like the hardware; access from one thread.
 * Register numbering and read/write side effects follow the coprocessor:
 * data 0..31 (mfc2/mtc2/lwc2/swc2), control 0..31 (cfc2/ctc2). */
uint32_t Memories_GteReadData(unsigned index);
void Memories_GteWriteData(unsigned index, uint32_t value);
uint32_t Memories_GteReadControl(unsigned index);
void Memories_GteWriteControl(unsigned index, uint32_t value);
/* Execute a COP2 command word; only bits 0..24 are interpreted. Returns 0 for
 * an opcode with no implementation (FLAG is still cleared, as on hardware). */
int Memories_GteCommand(uint32_t command);
void Memories_GteReset(void);

/* lwc2/swc2 on host memory: little-endian words, no alignment requirement. */
void Memories_GteLoad(unsigned index, const void *address);
void Memories_GteStore(unsigned index, void *address);
void Memories_GteStoreWord(uint32_t value, void *address);
/* Save states: the register file. */
void *Gte_StateData(unsigned *size);
#endif
