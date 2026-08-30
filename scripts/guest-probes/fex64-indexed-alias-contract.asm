; FEX translator conformance fixture for mixed-domain indexed addressing.
;
; A guest effective address may combine two registers that live in different
; halves of the address space. The packaged loader faults on exactly this
; shape:
;
;     mov qword ptr [r10 + r14], rax
;
; with a canonical LOW base and a HIGH index. When the host translation is
; applied to the base alone -- before the index has contributed its own high
; bits -- the result is an address in neither half. The address has to be
; completed in guest space and translated exactly once.
;
; This fixture drives the arithmetic itself: every store is read back and
; compared, so a wrongly ordered translation fails on the value rather than on
; an encoding scan. It also pins the store under test at absolute guest
; address 0x10100 so the probe can arm FEX's targeted IR capture on it and
; prove the emitted host words contain the canonical ADD before the alias ORR.
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

; The harness maps one region high enough that a base/index split puts the two
; components in different halves of the address space, which is the shape the
; loader used.
%define BASE_LOW  0x1000
%define INDEX_HIGH (DATA - BASE_LOW)

%define VALUE_A 0x5eed5eed5eed5eed
%define VALUE_B 0x0dd0dd0dd0dd0dd0
%define VALUE_C 0xbeefbeefbeefbeef

start:
    mov     rsp, STACK

    mov     r10, BASE_LOW
    mov     r14, INDEX_HIGH
    mov     rax, VALUE_A
    jmp     indexed_alias_entry

; Enter the block a few instructions before the store so the capture window is
; bounded by the following guest opcode rather than collapsing onto the block
; entry, and pin the store itself at a fixed absolute address.
times 0x0fc - ($ - $$) db 0x90

indexed_alias_entry:
    nop
    nop
    nop
    nop

indexed_alias_target:
    ; Low base, high index, scale 1: the exact operand shape that faulted.
    mov     [r10 + r14], rax
    mov     rcx, [r10 + r14]
    cmp     rcx, rax
    jne     fail

    ; The completed address is what matters, so a plain absolute reference to
    ; the same location must observe the same bytes.
    mov     rbx, DATA
    mov     rdx, [rbx]
    cmp     rdx, rax
    jne     fail

    ; The reversed encoding: high base, low index.
    mov     rsi, VALUE_B
    mov     [r14 + r10], rsi
    mov     rdi, [r14 + r10]
    cmp     rdi, rsi
    jne     fail
    mov     rdx, [rbx]
    cmp     rdx, rsi
    jne     fail

    ; A scaled index reaching the same address, so the shift is applied before
    ; the translation as well.
    mov     r11, BASE_LOW / 8
    mov     r8, VALUE_C
    mov     [r14 + r11*8], r8
    mov     r9, [r14 + r11*8]
    cmp     r9, r8
    jne     fail
    mov     rdx, [rbx]
    cmp     rdx, r8
    jne     fail

    ; A displacement on top of a mixed-domain index must not shift the address
    ; out of the completed range either.
    mov     rdx, VALUE_A
    mov     [r10 + r14 + 0x40], rdx
    mov     rcx, [r10 + r14 + 0x40]
    cmp     rcx, rdx
    jne     fail
    mov     rcx, [rbx + 0x40]
    cmp     rcx, rdx
    jne     fail

    ; And the original location must still hold its own value, proving the
    ; displaced store did not alias onto it.
    mov     rdx, [rbx]
    cmp     rdx, r8
    jne     fail

    mov     rax, PASS
    hlt

fail:
    mov     rax, FAIL
    hlt
