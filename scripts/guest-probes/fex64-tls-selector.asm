; Ordinary Wine TLS reloads must not trap or translate host descriptor pointers.
%ifdef CONFIG
{
  "Match": "None",
  "RegData": { "RAX": "0x4645585f50415353", "RDX": "0x210000", "R10": "7" },
  "MemoryRegions": { "0x200000": "0x20000" }
}
%endif
BITS 64
ORG 0x10000
mov rsp, 0x218000
mov rbx, 0x200000
mov dword [rbx], 0x12345678
mov word [rbx+0x338], 0x63
mov dword [rbx+0x10000], 0x76543210
wrgsbase rbx
mov rax, 0x100200000
wrfsbase rax
push 0x246
popfq
; Exact Wine syscall/unix-call epilogue instruction.
mov fs, word [gs:0x338]
pushfq
pop rdx
mov r10d, 1
cmp edx, 0x246
jne fail
rdfsbase rdx
mov r10d, 2
cmp rdx, 0x200000
jne fail
mov r10d, 3
cmp dword [fs:0], 0x12345678
jne fail
; A second descriptor proves this is not a hardcoded TEB base.
mov eax, 0x6b
push 0xa93
popfq
mov fs, ax
pushfq
pop rdx
mov r10d, 8
cmp edx, 0xa93
jne fail
rdfsbase rdx
mov r10d, 4
cmp rdx, 0x210000
jne fail
mov r10d, 5
cmp dword [fs:0], 0x76543210
jne fail
; GS follows the same descriptor semantics, including clearing high bits.
mov rdx, 0x100200000
wrgsbase rdx
mov gs, ax
rdgsbase rdx
mov r10d, 6
cmp rdx, 0x210000
jne fail
mov r10d, 7
cmp dword [gs:0], 0x76543210
jne fail
mov rax, 0x4645585f50415353
hlt
fail:
mov rax, 0x4645585f4641494c
hlt
