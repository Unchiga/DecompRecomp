/* VSync is the save-state boundary (see state.h). The entry records what the
 * game caller expects back -- the callee-saved registers and where its return
 * address is -- and continues in the C implementation on the same frame. */
    .text
    .globl VSync
    .type VSync, @function
VSync:
    movl %ebx, Memories_StateEntry
    movl %esi, Memories_StateEntry + 4
    movl %edi, Memories_StateEntry + 8
    movl %ebp, Memories_StateEntry + 12
    movl %esp, Memories_StateEntry + 16
    jmp Memories_VSync
    .size VSync, . - VSync

/* void Memories_StateReturn(const MemoriesStateEntry *entry, int value):
 * return `value` from the VSync call that `entry` describes. */
    .globl Memories_StateReturn
    .type Memories_StateReturn, @function
Memories_StateReturn:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movl 0(%ecx), %ebx
    movl 4(%ecx), %esi
    movl 8(%ecx), %edi
    movl 12(%ecx), %ebp
    movl 16(%ecx), %esp
    ret
    .size Memories_StateReturn, . - Memories_StateReturn
    .section .note.GNU-stack, "", @progbits
