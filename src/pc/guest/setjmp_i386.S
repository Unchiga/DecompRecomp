/* The game's jmp_buf is the Psy-Q int[12] at a fixed guest address, too small
 * for the host libc's. Six words are enough for the i386 callee-saved state. */
    .text
    .globl Psx_setjmp
    .type Psx_setjmp, @function
Psx_setjmp:
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
    .size Psx_setjmp, .-Psx_setjmp

    .globl Psx_longjmp
    .type Psx_longjmp, @function
Psx_longjmp:
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
    .size Psx_longjmp, .-Psx_longjmp
    .section .note.GNU-stack,"",@progbits
