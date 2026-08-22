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

    /* The pinned dynamic loader clears its final data segment with an SSE2
     * loop. Device traces showed that loop repeatedly returning to its
     * pointer-update edge after mmap had already succeeded. Prove both a
     * bulk fill and the normalized positive-add form of that loop before
     * launching a real guest. */
    lea rep_stos_buffer(%rip), %rdi
    mov $0x5a, %esi
    mov $0x6000, %edx
    call rep_stos_probe
    lea rep_stos_buffer(%rip), %rcx
    cmp %rcx, %rax
    jne rep_stos_failed
    cmpb $0x5a, (%rcx)
    jne rep_stos_failed
    cmpb $0x5a, 0x5fff(%rcx)
    jne rep_stos_failed
    lea rep_stos_buffer(%rip), %rdx
    lea rep_stos_buffer+0x6000(%rip), %rdi
    call normalized_pointer_probe
    lea rep_stos_buffer(%rip), %rcx
    cmpb $0, (%rcx)
    jne rep_stos_failed
    cmpb $0, 0x5fff(%rcx)
    jne rep_stos_failed

    mov $1, %rax
    mov $1, %rdi
    lea success_message(%rip), %rsi
    mov $(success_message_end-success_message), %rdx
    syscall

    mov $60, %rax
    // Versioned success code: the older scalar-only probe returned 42. A
    // stale packaged binary must not satisfy this stronger acceptance gate.
    mov $47, %rdi
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

rep_stos_failed:
    mov $1, %rax
    mov $1, %rdi
    lea rep_stos_failure(%rip), %rsi
    mov $(rep_stos_failure_end-rep_stos_failure), %rdx
    syscall
    mov $60, %rax
    mov $46, %rdi
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

/* Semantically equivalent replacement for the loader's vector memset path.
 * R9 is caller-saved in the x86-64 System V ABI, so it can retain the return
 * value while REP STOSB consumes RDI/RCX/AL. */
rep_stos_probe:
    mov %rdi, %r9
    mov %rdx, %rcx
    mov %sil, %al
    rep stosb
    mov %r9, %rax
    ret

/* Mirrors the loader loop after its guarded `sub rdx, -64` to `add rdx, 64`
 * normalization. The compare immediately after the update owns the flags. */
normalized_pointer_probe:
    pxor %xmm0, %xmm0
normalized_pointer_loop:
    movaps %xmm0, (%rdx)
    movaps %xmm0, 0x10(%rdx)
    movaps %xmm0, 0x20(%rdx)
    movaps %xmm0, 0x30(%rdx)
    add $0x40, %rdx
    cmp %rdi, %rdx
    jb normalized_pointer_loop
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
    .ascii "BoxedWine FEX64 SSE2/REP STOS/call-ret PASS\n"
success_message_end:
equal_failure:
    .ascii "BoxedWine FEX64 SSE2 equal comparison FAIL\n"
equal_failure_end:
unequal_failure:
    .ascii "BoxedWine FEX64 SSE2 unequal comparison FAIL\n"
unequal_failure_end:
rep_stos_failure:
    .ascii "BoxedWine FEX64 REP STOS fill FAIL\n"
rep_stos_failure_end:

    .section .bss
    .balign 64
rep_stos_buffer:
    .skip 0x6000
