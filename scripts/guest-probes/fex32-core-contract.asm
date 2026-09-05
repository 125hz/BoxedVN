; BoxedVN IA-32 translator conformance fixture.
; TestHarnessRunner executes this through FEX's ARM64 JIT and VIXL simulator.

%ifdef CONFIG
{
  "Match": "None",
  "Mode": "32BIT",
  "RegData": {
    "RAX": "0x46333250"
  },
  "MemoryRegions": {
    "0x20000000": "0x10000"
  }
}
%endif

BITS 32

%define DATA  0x20000000
%define STACK 0xe0001000
%define PASS  0x46333250 ; "F32P"
%define FAIL  0x46333246 ; "F32F"

start:
    mov     esp, STACK

    ; IA-32 arithmetic must discard all state above bit 31.
    mov     eax, 0xffffffff
    add     eax, 2
    cmp     eax, 1
    jne     fail

    ; A nested call must push and consume a 32-bit return address and restore
    ; the architectural stack pointer exactly.
    mov     ebx, esp
    push    dword 0x10203040
    call    call_probe
    add     esp, 4
    cmp     esp, ebx
    jne     fail
    cmp     eax, 0x50607080
    jne     fail

    ; Exercise the SSE2 memory path Wine32 and the graphics stack rely on.
    mov     dword [DATA + 0x100], 1
    mov     dword [DATA + 0x104], 2
    mov     dword [DATA + 0x108], 3
    mov     dword [DATA + 0x10c], 4
    movdqu  xmm0, [DATA + 0x100]
    pshufd  xmm1, xmm0, 0x1b
    paddd   xmm0, xmm1
    movdqu  [DATA + 0x120], xmm0
    cmp     dword [DATA + 0x120], 5
    jne     fail
    cmp     dword [DATA + 0x124], 5
    jne     fail
    cmp     dword [DATA + 0x128], 5
    jne     fail
    cmp     dword [DATA + 0x12c], 5
    jne     fail

    ; REP STOS must update the 32-bit destination/count and touch the complete
    ; range without overwriting either guard byte.
    mov     byte [DATA + 0x0fff], 0x5a
    mov     byte [DATA + 0x1100], 0x5a
    mov     edi, DATA + 0x1000
    mov     ecx, 0x100
    mov     al, 0xa5
    rep stosb
    cmp     edi, DATA + 0x1100
    jne     fail
    cmp     ecx, 0
    jne     fail
    cmp     byte [DATA + 0x0fff], 0x5a
    jne     fail
    cmp     byte [DATA + 0x1000], 0xa5
    jne     fail
    cmp     byte [DATA + 0x10ff], 0xa5
    jne     fail
    cmp     byte [DATA + 0x1100], 0x5a
    jne     fail

    ; The effective address is 0xfffffff0 + 0x20000010. IA-32 wraps it to
    ; 0x20000000 before the memory access; leaking a 64-bit intermediate would
    ; address 0x120000000 instead.
    mov     eax, 0xfffffff0
    mov     dword [eax + 0x20000010], 0xdeadbeef
    cmp     dword [DATA], 0xdeadbeef
    jne     fail

    ; The C runtime scales a nonzero subnormal by 2^64 before recurring
    ; in frexp. It must become normal in one step, and zero compares equal.
    fninit
    mov dword [DATA + 0x200], 0
    mov dword [DATA + 0x204], 0
    fldz
    fld qword [DATA + 0x200]
    fucomip st0, st1
    fstp st0
    jp fail
    jne fail
    mov dword [DATA + 0x200], 1
    fldz
    fld qword [DATA + 0x200]
    fucomip st0, st1
    fstp st0
    jp fail
    je fail
    mov dword [DATA + 0x208], 0x5f800000 ; float 2^64
    fld qword [DATA + 0x200]
    ; A call forces the x87 value to survive a block boundary.
    call x87_scale
    fstp qword [DATA + 0x210]
    cmp dword [DATA + 0x210], 0
    jne fail
    cmp dword [DATA + 0x214], 0x00d00000 ; double 2^-1010
    jne fail
    mov     eax, PASS
    hlt

x87_scale:
    fmul dword [DATA + 0x208]
    ret

call_probe:
    push    ebp
    mov     ebp, esp
    cmp     dword [ebp + 8], 0x10203040
    jne     .bad
    mov     eax, 0x50607080
    pop     ebp
    ret
.bad:
    pop     ebp
    jmp     fail

fail:
    mov     eax, FAIL
    hlt
