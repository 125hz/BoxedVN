/*
 * Minimal x86-64 Linux process for BoxedWine's FEX kernel bring-up.
 * GPLv2; see license.txt.
 */

    .section .text
    .global _start
    .type _start, @function
_start:
    lea equal_left(%rip), %rdi
    lea equal_right(%rip), %rsi
    call strcmp_vector_probe
    test %eax, %eax
    jne equal_failed

    lea unequal_left(%rip), %rdi
    lea equal_right(%rip), %rsi
    call strcmp_vector_probe
    cmp $-12, %eax
    jne unequal_failed

    mov $1, %rax
    mov $1, %rdi
    lea success_message(%rip), %rsi
    mov $(success_message_end-success_message), %rdx
    syscall

    mov $60, %rax
    // Versioned success code: the older scalar-only probe returned 42. A
    // stale packaged binary must not satisfy this stronger acceptance gate.
    mov $45, %rdi
    syscall
    ud2

equal_failed:
    mov $1, %rax
    mov $1, %rdi
    lea equal_failure(%rip), %rsi
    mov $(equal_failure_end-equal_failure), %rdx
    syscall
    mov $60, %rax
    mov $43, %rdi
    syscall
    ud2

unequal_failed:
    mov $1, %rax
    mov $1, %rdi
    lea unequal_failure(%rip), %rsi
    mov $(unequal_failure_end-unequal_failure), %rdx
    syscall
    mov $60, %rax
    mov $44, %rdi
    syscall
    ud2

/* Mirrors the loader's SSE2 string-comparison core.  The first call takes
 * both the all-equal loop edge and the terminating-NUL edge; the second takes
 * the differing-byte edge.  Together they cover the vector masks, scalar
 * flags, indirect data loads, and guest call/return path implicated during
 * loader bring-up. */
strcmp_vector_probe:
    endbr64
strcmp_vector_loop:
    movlpd (%rdi), %xmm1
    movlpd (%rsi), %xmm2
    movhpd 8(%rdi), %xmm1
    movhpd 8(%rsi), %xmm2
    pxor %xmm0, %xmm0
    pcmpeqb %xmm1, %xmm0
    pcmpeqb %xmm2, %xmm1
    psubb %xmm0, %xmm1
    pmovmskb %xmm1, %edx
    sub $0xffff, %edx
    jne strcmp_vector_found
    add $16, %rdi
    add $16, %rsi
    jmp strcmp_vector_loop

strcmp_vector_found:
    add $0xffff, %edx
    not %edx
    bsf %edx, %ecx
    movzbl (%rdi,%rcx), %eax
    movzbl (%rsi,%rcx), %edx
    sub %edx, %eax
    ret
    .size _start, .-_start

    .section .rodata
    .balign 64
equal_left:
    .ascii "0123456789abcdef"
    .byte 0
    .fill 15, 1, 0
    .balign 64
equal_right:
    .ascii "0123456789abcdef"
    .byte 0
    .fill 15, 1, 0
    .balign 64
unequal_left:
    .ascii "0123456789abcXef"
    .byte 0
    .fill 15, 1, 0
success_message:
    .ascii "BoxedWine FEX64 SSE2 strcmp/call-ret PASS\n"
success_message_end:
equal_failure:
    .ascii "BoxedWine FEX64 SSE2 equal comparison FAIL\n"
equal_failure_end:
unequal_failure:
    .ascii "BoxedWine FEX64 SSE2 unequal comparison FAIL\n"
unequal_failure_end:
