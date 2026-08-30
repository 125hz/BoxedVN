; FEX translator conformance fixture for unencodable ALU constants.
;
; The ARM64 add/sub immediate form encodes an unsigned twelve-bit value,
; optionally shifted left by twelve. A negative guest displacement reaches the
; backend as a sign-extended 64-bit inline constant, which does not fit. The
; device executed 0xffff0177 where a valid add belonged, because the oversized
; immediate was shifted into the opcode field with assertions compiled out.
;
; This fixture drives the arithmetic itself, not the encoding: every result and
; every observable flag is checked, so a truncated or sign-flipped immediate
; fails here regardless of what instruction it encodes to. The negative
; non-flag-setting address calculation is pinned at absolute guest address
; 0x10100 so the probe can arm FEX's targeted IR capture on it without
; disassembling the built fixture.
;
; Consumed by FEX's TestHarnessRunner with the VIXL simulator; deliberately not
; a standalone Linux ELF, and independent of Wine and DXMT.

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

start:
    mov     rsp, STACK

    ; A base whose low bits make a truncated displacement obvious: a masked
    ; 0xffffffc0 would land far above the base instead of 0x40 below it.
    mov     rdi, DATA + 0x1000
    jmp     negative_add_entry

; Pin the operation under test at a fixed absolute address, and enter the block
; a few instructions earlier so the target is not at block offset zero. The
; device fault was at offset 0x4 within its block; matching that keeps the
; capture's host-word window bounded by the following guest opcode rather than
; collapsing onto the block entry.
times 0x0fc - ($ - $$) db 0x90

negative_add_entry:
    nop
    nop
    nop
    nop

negative_add_target:
    ; The exact operation the device miscompiled: a non-flag-setting add of a
    ; negative constant, reaching the backend as 0xffffffffffffffc0.
    lea     rbx, [rdi - 0x40]

    mov     rcx, DATA + 0xfc0
    cmp     rbx, rcx
    jne     fail

    ; The same shape at 32-bit width, where the constant is 0xffffffc0.
    mov     esi, 0x2000
    lea     edx, [esi - 0x40]
    cmp     edx, 0x1fc0
    jne     fail

    ; A displacement that needs more than twelve bits but is positive and
    ; shift-encodable, and one that is neither.
    lea     rbx, [rdi + 0x1000]
    mov     rcx, DATA + 0x2000
    cmp     rbx, rcx
    jne     fail
    lea     rbx, [rdi + 0x12345]
    mov     rcx, DATA + 0x13345
    cmp     rbx, rcx
    jne     fail
    lea     rbx, [rdi - 0x12345]
    mov     rcx, DATA - 0x11345
    cmp     rbx, rcx
    jne     fail

    ; Flag-setting add of an unencodable negative constant. 0x1000 + -0x40
    ; carries out of the 64-bit add, so CF must be set and the result exact.
    mov     r8, 0x1000
    add     r8, -0x40
    jnc     fail
    cmp     r8, 0xfc0
    jne     fail

    ; The same add taken to exactly zero must set ZF and CF and clear SF.
    mov     r9, 0x40
    add     r9, -0x40
    jnz     fail
    jnc     fail
    js      fail

    ; And one that stays negative: no carry, sign set.
    mov     r10, 0x10
    add     r10, -0x40
    jc      fail
    jns     fail
    mov     r11, -0x30
    cmp     r10, r11
    jne     fail

    ; Flag-setting subtraction of a constant that is not add/sub encodable at
    ; all. 0x12345 borrows nothing from 0x20000 and leaves a positive result.
    mov     r12, 0x20000
    sub     r12, 0x12345
    jc      fail
    jz      fail
    js      fail
    cmp     r12, 0xdcbb
    jne     fail

    ; Subtraction that borrows: CF set, result wraps negative.
    mov     r13, 0x1000
    sub     r13, 0x12345
    jnc     fail
    jns     fail
    mov     r14, 0x1000 - 0x12345
    cmp     r13, r14
    jne     fail

    ; A comparison against an unencodable constant must still order correctly;
    ; this is the cmp/cmn immediate path rather than the add/sub one.
    mov     r15, 0x12345
    cmp     r15, 0x12345
    jne     fail
    cmp     r15, 0x12346
    jae     fail

    ; The encodable fast path must remain an immediate operation and stay
    ; correct: small, and twelve-bit-shifted.
    mov     rax, 0x100
    add     rax, 0x40
    cmp     rax, 0x140
    jne     fail
    add     rax, 0x1000
    cmp     rax, 0x1140
    jne     fail
    sub     rax, 0x140
    cmp     rax, 0x1000
    jne     fail

    ; Finally, store through a negative displacement so a wrong address is
    ; observable in memory rather than only in a register.
    mov     rdi, DATA + 0x2000
    mov     rax, 0x1122334455667788
    mov     [rdi - 0x40], rax
    mov     rsi, DATA + 0x1fc0
    mov     rdx, [rsi]
    cmp     rdx, rax
    jne     fail

    mov     rax, PASS
    hlt

fail:
    mov     rax, FAIL
    hlt
