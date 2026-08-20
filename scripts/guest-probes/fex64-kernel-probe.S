/*
 * Minimal x86-64 Linux process for BoxedWine's FEX kernel bring-up.
 * GPLv2; see license.txt.
 */

    .section .text
    .global _start
    .type _start, @function
_start:
    mov $1, %rax
    mov $1, %rdi
    lea message(%rip), %rsi
    mov $(message_end-message), %rdx
    syscall

    mov $60, %rax
    mov $42, %rdi
    syscall
    ud2
    .size _start, .-_start

    .section .rodata
message:
    .ascii "BoxedWine ELF64 process reached the x86-64 kernel\n"
message_end:
