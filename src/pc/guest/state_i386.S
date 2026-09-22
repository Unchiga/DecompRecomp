/* VSync is the save-state boundary (see state.h). The entry records what the
 * game caller expects back -- the callee-saved registers and where its return
 * address is -- and continues in the C implementation on the same frame. */
#ifdef _WIN32
#define SYMBOL(name) _##name
#define FUNCTION(name)
#define END(name)
#else
#define SYMBOL(name) name
#define FUNCTION(name) .type name, @function
#define END(name) .size name, . - name
#endif
    .text
    .globl SYMBOL(VSync)
    FUNCTION(VSync)
SYMBOL(VSync):
    movl %ebx, SYMBOL(Memories_StateEntry)
    movl %esi, SYMBOL(Memories_StateEntry) + 4
    movl %edi, SYMBOL(Memories_StateEntry) + 8
    movl %ebp, SYMBOL(Memories_StateEntry) + 12
    movl %esp, SYMBOL(Memories_StateEntry) + 16
    jmp SYMBOL(Memories_VSync)
    END(VSync)

/* void Memories_StateReturn(const MemoriesStateEntry *entry, int value):
 * return `value` from the VSync call that `entry` describes. */
    .globl SYMBOL(Memories_StateReturn)
    FUNCTION(Memories_StateReturn)
SYMBOL(Memories_StateReturn):
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    movl 0(%ecx), %ebx
    movl 4(%ecx), %esi
    movl 8(%ecx), %edi
    movl 12(%ecx), %ebp
    movl 16(%ecx), %esp
    ret
    END(Memories_StateReturn)

#ifdef _WIN32
/* void Memories_ContextSwitch(StackContext *from, const StackContext *to):
 * the ucontext stand-in (state.c). Keeps the callee-saved registers on the
 * stack being left and resumes the one `to` describes. */
    .globl SYMBOL(Memories_ContextSwitch)
SYMBOL(Memories_ContextSwitch):
    movl 4(%esp), %eax
    movl 8(%esp), %edx
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl %esp, (%eax)
    movl (%edx), %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
#else
    .section .note.GNU-stack, "", @progbits
#endif
