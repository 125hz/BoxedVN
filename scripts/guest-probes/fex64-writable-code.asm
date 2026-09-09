; Generic self-modifying code through BoxedWine's low and top guest aliases.
; No syscall, protection transition or explicit cache flush accompanies writes.
; A cached immediate and a cached indirect call target must both be refreshed.
%ifdef CONFIG
{
  "Match": "None",
  "RegData": {"RAX": "0x4645585f50415353"},
  "MemoryRegions": {
    "0x100000": "0x10000",
    "0x7ffc0000": "0x10000",
    "0x7ffffe000000": "0x10000"
  }
}
%endif
BITS 64
ORG 0x10000
    mov rsp,0x7ffcf000
    mov r12,0x100000
    call exercise
    mov r12,0x7ffffe000000
    call exercise
    mov rax,0x4645585f50415353
    hlt

exercise:
    ; mov eax,1 ; ret
    mov rax,0xc300000001b8
    mov [r12],rax
    call r12
    cmp eax,1
    jne fail
    mov dword [r12+1],2
    call r12
    cmp eax,2
    jne fail

    ; A second callee returning 3.
    mov rax,0xc300000003b8
    mov [r12+64],rax
    ; mov r11,imm64 ; call r11 ; ret
    lea r13,[r12+128]
    mov word [r13],0xbb49
    mov [r13+2],r12
    mov dword [r13+10],0xc3d3ff41
    call r13
    cmp eax,2
    jne fail
    lea rax,[r12+64]
    mov [r13+2],rax
    call r13
    cmp eax,3
    jne fail
    ret
fail:
    mov rax,0x4645585f4641494c
    hlt
