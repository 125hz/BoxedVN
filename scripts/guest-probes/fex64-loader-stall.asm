; FEX translator conformance fixture for the x86-64 loader startup paths.
; This file is consumed by FEX's TestHarnessRunner with the VIXL simulator;
; it is deliberately not a standalone Linux ELF.

%ifdef CONFIG
{
  "Match": "None",
  "RegData": {
    "RAX": "0x4645585f50415353"
  },
  "MemoryRegions": {
    "0x100000000": "0x10000"
  }
}
%endif

BITS 64

%define DATA  0x100000000
%define STACK 0xe0001000
%define PASS  0x4645585f50415353 ; "FEX_PASS"
%define FAIL  0x4645585f4641494c ; "FEX_FAIL"

start:
    ; TestHarnessRunner starts with no guest stack. The harness maps this
    ; region for address-mode tests, so use it for guest call/return traffic.
    mov     rsp, STACK

    ; Mirror the loader handoff repair: materialise an absolute target in R11
    ; and jump through it. Keep the encoding explicitly 64-bit even though the
    ; flat fixture's target happens to be nearby.
    db      0x49, 0xbb
    dq      handoff_target
    db      0x41, 0xff, 0xe3
    jmp     fail

handoff_target:

    ; Two equal strings with a terminating NUL in the first 16-byte vector.
    mov     rdi, DATA
    mov     rax, 0x6867666564636261 ; "abcdefgh"
    mov     [rdi], rax
    mov     qword [rdi + 8], 0
    mov     rdi, DATA + 0x40
    mov     [rdi], rax
    mov     qword [rdi + 8], 0

    mov     rdi, DATA
    mov     rsi, DATA + 0x40
    call    strcmp_vector_probe
    test    eax, eax
    jnz     fail

    ; One differing byte must take the scalar mismatch path and return -12.
    mov     rdi, DATA + 0x80
    mov     rax, 0x6867666564636261
    mov     [rdi], rax
    mov     qword [rdi + 8], 0
    mov     byte [rdi + 3], 'X'
    mov     rsi, DATA + 0x40
    call    strcmp_vector_probe
    cmp     eax, -12
    jnz     fail

    ; Reproduce the pinned libc path sampled on device: an 8-byte-unaligned
    ; destination, a 0x518-byte zero fill, unaligned head/tail stores and the
    ; aligned 0x40-byte loop whose pointer update is encoded as SUB -0x40.
    ; Fill guard bytes first so an early return or overrun is visible.
    mov     rdi, DATA + 0x2000
    mov     esi, 0x5a
    mov     edx, 0x600
    call    rep_stos_probe
    mov     rdi, DATA + 0x2038
    xor     esi, esi
    mov     edx, 0x518
    call    glibc_memset_probe
    mov     r8, DATA
    lea     r9, [r8 + 0x2038]
    cmp     rax, r9
    jnz     fail
    cmp     byte [r8 + 0x2037], 0x5a
    jnz     fail
    cmp     qword [r8 + 0x2038], 0
    jnz     fail
    cmp     qword [r8 + 0x2548], 0
    jnz     fail
    cmp     byte [r8 + 0x2550], 0x5a
    jnz     fail

    ; Exercise REP STOS and preserve the original destination as the return.
    mov     rdi, DATA + 0x8000
    mov     esi, 0x5a
    mov     edx, 0x1000
    call    rep_stos_probe
    lea     r9, [r8 + 0x8000]
    cmp     rax, r9
    jnz     fail
    cmp     byte [r8 + 0x8000], 0x5a
    jnz     fail
    cmp     byte [r8 + 0x8fff], 0x5a
    jnz     fail

    mov     rax, PASS
    hlt

fail:
    mov     rax, FAIL
    hlt

; Mirrors the loader's SSE2 string comparison: NUL/equal masks, byte-mask
; subtraction, branch to the first differing byte, and scalar return.
strcmp_vector_probe:
    movlpd  xmm1, [rdi]
    movhpd  xmm1, [rdi + 8]
    movlpd  xmm2, [rsi]
    movhpd  xmm2, [rsi + 8]
    pxor    xmm0, xmm0
    pcmpeqb xmm0, xmm1
    pcmpeqb xmm1, xmm2
    psubb   xmm1, xmm0
    pmovmskb edx, xmm1
    sub     edx, 0xffff
    jnz     .found
    add     rdi, 16
    add     rsi, 16
    jmp     strcmp_vector_probe

.found:
    add     edx, 0xffff
    not     edx
    bsf     ecx, edx
    movzx   eax, byte [rdi + rcx]
    movzx   edx, byte [rsi + rcx]
    sub     eax, edx
    ret

rep_stos_probe:
    mov     r9, rdi
    mov     rcx, rdx
    mov     al, sil
    rep stosb
    mov     rax, r9
    ret

; Instruction-for-instruction shape of the pinned libc large memset path used
; by the observed 0x518-byte zero fill. Short and very-large branches are kept
; only as explicit failures because this fixture targets the sampled contract.
glibc_memset_probe:
    endbr64
    movd    xmm0, esi
    mov     rax, rdi
    punpcklbw xmm0, xmm0
    punpcklwd xmm0, xmm0
    pshufd  xmm0, xmm0, 0
    cmp     rdx, 0x10
    jb      fail
    cmp     rdx, 0x20
    jbe     fail
    cmp     rdx, [rel memset_rep_threshold]
    ja      fail
    movups  [rdi], xmm0
    movups  [rdi + 0x10], xmm0
    add     rdi, rdx
    cmp     rdx, 0x40
    jbe     fail
    movups  [rax + 0x20], xmm0
    movups  [rax + 0x30], xmm0
    add     rdi, -0x40
    cmp     rdx, 0x80
    jbe     .tail
    lea     rdx, [rax + 0x40]
    and     rdx, -0x10
.loop:
    movaps  [rdx], xmm0
    movaps  [rdx + 0x10], xmm0
    movaps  [rdx + 0x20], xmm0
    movaps  [rdx + 0x30], xmm0
    sub     rdx, -0x40
    cmp     rdx, rdi
    jb      .loop
.tail:
    movups  [rdi], xmm0
    movups  [rdi + 0x10], xmm0
    movups  [rdi + 0x20], xmm0
    movups  [rdi + 0x30], xmm0
    ret

align 8
memset_rep_threshold:
    dq 0x1000
