/* The game's jmp_buf is the Psy-Q int[12] at a fixed guest address, too small
 * for the host libc's. Six words are enough for the i386 callee-saved state. */
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
    .globl SYMBOL(Psx_setjmp)
    FUNCTION(Psx_setjmp)
SYMBOL(Psx_setjmp):
    movl 4(%esp), %eax
    movl %ebx, 0(%eax)
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 16(%eax)
    movl (%esp), %ecx
    movl %ecx, 20(%eax)
    xorl %eax, %eax
    ret
    END(Psx_setjmp)

    .globl SYMBOL(Psx_longjmp)
    FUNCTION(Psx_longjmp)
SYMBOL(Psx_longjmp):
    movl 4(%esp), %edx
    movl 8(%esp), %eax
    testl %eax, %eax
    jnz 1f
    incl %eax
1:  movl 0(%edx), %ebx
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp
    jmp *20(%edx)
    END(Psx_longjmp)
#ifndef _WIN32
    .section .note.GNU-stack,"",@progbits
#endif
