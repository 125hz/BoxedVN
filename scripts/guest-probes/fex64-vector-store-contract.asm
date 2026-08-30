; FEX translator conformance fixture for unaligned 128-bit vector stores.
;
; The device reported SIGILL on a host word of 0xffff0177 while executing the
; bundled libc's `movups xmmword ptr [rax + 0x30], xmm0`. The valid ARM64
; lowering of that displacement is STUR Q23, [X11, #-16] = 0x3c9f0177, so this
; fixture drives the exact operation through the real ARM64 JIT and checks the
; stored bytes. It is deliberately generic: no Wine, no DXMT, no guest library.
;
; The instruction under test is pinned at absolute guest address 0x10100 so the
; probe can arm FEX's targeted IR capture on it without disassembling the built
; fixture. Consumed by FEX's TestHarnessRunner with the VIXL simulator; this is
; not a standalone Linux ELF.

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
ORG 0x10000

%define DATA  0x100000000
%define STACK 0xe0001000
%define PASS  0x4645585f50415353 ; "FEX_PASS"
%define FAIL  0x4645585f4641494c ; "FEX_FAIL"

%define LOW   0x8877665544332211
%define HIGH  0xffeeddccbbaa9988

start:
    mov     rsp, STACK

    ; A 16-byte pattern that is asymmetric in every lane, so a store landing
    ; at the wrong address cannot be mistaken for a correct one.
    mov     rdi, DATA
    mov     rax, LOW
    mov     [rdi], rax
    mov     rax, HIGH
    mov     [rdi + 8], rax
    movups  xmm0, [rdi]

    ; Poison the destination window, including the bytes a doubled +0x30
    ; displacement would reach.
    mov     rcx, DATA + 0x100
    xor     edx, edx
    xor     esi, esi
.poison:
    mov     [rcx + rsi], rdx
    add     rsi, 8
    cmp     rsi, 0x100
    jb      .poison

    mov     rax, DATA + 0x100
    jmp     vector_store_target

; Pin the operation under test at a fixed absolute address.
times 0x100 - ($ - $$) db 0x90

vector_store_target:
    movups  [rax + 0x30], xmm0

    ; The pattern must land exactly at +0x30.
    mov     rdx, [rax + 0x30]
    mov     rcx, LOW
    cmp     rdx, rcx
    jne     fail
    mov     rdx, [rax + 0x38]
    mov     rcx, HIGH
    cmp     rdx, rcx
    jne     fail

    ; A doubled displacement would have written at +0x60 instead.
    mov     rdx, [rax + 0x60]
    test    rdx, rdx
    jnz     fail
    mov     rdx, [rax + 0x68]
    test    rdx, rdx
    jnz     fail

    ; Nothing may have landed just below the destination either.
    mov     rdx, [rax + 0x28]
    test    rdx, rdx
    jnz     fail

    ; Repeat through a negative displacement from a higher base: that is the
    ; form which lowers to a negative unscaled ARM64 immediate.
    mov     rbx, DATA + 0x180
    movups  [rbx - 0x10], xmm0
    mov     rdx, [rbx - 0x10]
    mov     rcx, LOW
    cmp     rdx, rcx
    jne     fail
    mov     rdx, [rbx - 8]
    mov     rcx, HIGH
    cmp     rdx, rcx
    jne     fail

    ; And an unaligned base, so the lowering cannot take the aligned LDR/STR
    ; shortcut for either operand.
    mov     rbx, DATA + 0x201
    movups  [rbx + 0x30], xmm0
    mov     rdx, [rbx + 0x30]
    mov     rcx, LOW
    cmp     rdx, rcx
    jne     fail
    mov     rdx, [rbx + 0x38]
    mov     rcx, HIGH
    cmp     rdx, rcx
    jne     fail

    mov     rax, PASS
    hlt

fail:
    mov     rax, FAIL
    hlt
