; FEX translator conformance fixture for Wine's top-down arena.
;
; Wine reserves [0x7ffffe000000, 0x7fffffff0000) and then reads, writes and
; performs atomics through pointers into it. Those addresses need 47 bits,
; which is a different shape from every other guest address BoxedWine hosts:
; the canonical low lane fits in 33 and the identity lane in 39.
;
; Every store here is read back and compared, and the atomic's return value is
; checked as well as the memory it updated, so a truncated, sign-extended or
; wrongly relocated address fails on the value rather than on an encoding scan.
;
; Consumed by FEX's TestHarnessRunner with the VIXL simulator; deliberately not
; a standalone Linux ELF, and independent of Wine and DXMT.
;
; Scope, stated plainly: the harness does not publish BoxedWine's alias
; configuration, so this runs with the translation disabled and proves the
; translator handles 47-bit guest addresses correctly in its default mode. The
; translation itself is proven separately -- exhaustively over the arena in
; ios/tests/test_guest_top_alias.cpp, and at the encoding level against the
; real emitter in fex64-emitter-encoding-check.cpp.

%ifdef CONFIG
{
  "Match": "None",
  "RegData": {
    "RAX": "0x4645585f50415353"
  },
  "MemoryRegions": {
    "0x7ffffe000000": "0x10000",
    "0x7ffffffe0000": "0x10000"
  }
}
%endif

BITS 64
ORG 0x10000

%define ARENA 0x7ffffe000000
%define STACK 0xe0001000
%define PASS  0x4645585f50415353 ; "FEX_PASS"
%define FAIL  0x4645585f4641494c ; "FEX_FAIL"

%define VALUE_A 0x5eed5eed5eed5eed
%define VALUE_B 0x0dd0dd0dd0dd0dd0
%define ADDEND  0x0000000100000001

    mov rsp, STACK
    mov rax, FAIL

    ; --- plain store and load through a 47-bit pointer -------------------
    mov r10, ARENA
    mov rbx, VALUE_A
    mov qword [r10], rbx
    mov rcx, qword [r10]
    cmp rcx, rbx
    jne done

    ; --- the same pointer reached through base + displacement ------------
    mov rbx, VALUE_B
    mov qword [r10 + 0x1000], rbx
    mov rcx, qword [r10 + 0x1000]
    cmp rcx, rbx
    jne done
    ; the first slot must not have moved
    mov rcx, qword [r10]
    mov rdx, VALUE_A
    cmp rcx, rdx
    jne done

    ; --- base + index, where the index carries the arena's own bits ------
    ; The completed address is what has to be translated; translating the
    ; base alone would land in neither lane.
    xor r11, r11
    mov r11, 0x2000
    mov rbx, VALUE_A
    mov qword [r10 + r11], rbx
    mov rcx, qword [r10 + r11]
    cmp rcx, rbx
    jne done

    ; --- sub-qword accesses, so a byte-granular offset is exercised ------
    mov byte [r10 + 0x3000], 0x5a
    movzx rcx, byte [r10 + 0x3000]
    cmp rcx, 0x5a
    jne done
    mov dword [r10 + 0x3004], 0xdeadbeef
    mov ecx, dword [r10 + 0x3004]
    mov edx, 0xdeadbeef
    cmp rcx, rdx
    jne done

    ; --- a locked read-modify-write through the arena --------------------
    ; LOCK XADD returns the old value and stores the sum. Both are checked:
    ; a relocation applied to only one of the two accesses would pass a
    ; store-only test.
    mov rbx, VALUE_A
    mov qword [r10 + 0x4000], rbx
    mov rdx, ADDEND
    lock xadd qword [r10 + 0x4000], rdx
    mov rcx, VALUE_A
    cmp rdx, rcx                      ; XADD returned the old value
    jne done
    mov rcx, qword [r10 + 0x4000]
    mov rdx, VALUE_A
    add rdx, ADDEND
    cmp rcx, rdx                      ; memory holds the sum
    jne done

    ; --- a locked compare-and-swap through the arena ---------------------
    mov rax, qword [r10 + 0x4000]     ; expected
    mov rbx, VALUE_B
    lock cmpxchg qword [r10 + 0x4000], rbx
    jne done                          ; the compare must have succeeded
    mov rcx, qword [r10 + 0x4000]
    mov rdx, VALUE_B
    cmp rcx, rdx
    jne done

    ; --- the far end of the arena ----------------------------------------
    ; The last aligned qword the reservation covers, 47 bits with every low
    ; bit set: a truncated or sign-extended address cannot reach it.
    mov r10, 0x7ffffffefff8
    mov rbx, VALUE_A
    mov qword [r10], rbx
    mov rcx, qword [r10]
    cmp rcx, rbx
    jne done

    mov rax, PASS

done:
    hlt
