; FEX translator conformance fixture for the stack ops through the alias.
;
; Runs with BoxedWine's address translation ENABLED, via the test-only
; FEX_BOXEDWINE_ALIAS path in TestHarnessRunner.
;
; Push, PushTwo, Pop and PopTwo used ARM64 pre/post-indexed forms on the
; guest-visible address register. That writeback cannot be translated -- it
; would put a host address into RSP -- so those paths kept dereferencing the
; canonical stack. Wine's ntdll spun on it: a `push rbp` at guest
; 0x7a402602e4 writing canonical 0x7ffcfc78, ninety-three million handled
; faults at about 98% of a core.
;
; TEST DESIGN, and the reason this fixture is written the way it is:
;
; The alias-enabled harness maps BOTH the canonical region and its host alias.
; A push followed by a pop therefore proves nothing -- two untranslated
; operations agree with each other perfectly, against the canonical copy. So
; every check here crosses the two worlds:
;
;   - after a push, the value is read back with an ordinary scalar load, which
;     goes through GenerateMemOperand and is known to translate. If the push
;     wrote the canonical page, this load reads the alias and sees the seed.
;   - before a pop, the alias is seeded through an ordinary scalar store with
;     a value the canonical page does not hold. If the pop reads the canonical
;     page, it returns the wrong value.
;
; The canonical and alias copies are given deliberately different contents so
; a missing translation fails on the value, deterministically, rather than
; happening to agree.
;
; Every address reaches memory through a register: an absolute x86-64 memory
; operand is disp32 and would truncate a 47-bit constant in the assembler.
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
    "0x7ffc0000": "0x10000",
    "0x7ffffe000000": "0x10000"
  }
}
%endif

BITS 64
ORG 0x10000

; The canonical low stack the device faulted on. Its host alias is
; 0x7ffcfc80 | 0x7800000000 = 0x787ffcfc80.
%define LOW_STACK_TOP 0x7ffcfc80
%define SCRATCH       0x7ffc0000

; A top-arena stack, to prove the second lane too. Host alias 0x7ffe008000.
%define TOP_STACK_TOP 0x7ffffe008000

%define PASS 0x4645585f50415353 ; "FEX_PASS"
%define FAIL 0x4645585f4641494c ; "FEX_FAIL"

%define MARK_A 0xa1a1a1a1a1a1a1a1
%define MARK_B 0xb2b2b2b2b2b2b2b2
%define MARK_C 0xc3c3c3c3c3c3c3c3
%define MARK_D 0xd4d4d4d4d4d4d4d4
%define POISON 0xdeadbeefdeadbeef

    mov rax, FAIL

    ; ===============================================================
    ; push rbp, at exactly the canonical stack the device faulted on.
    ; ===============================================================
    mov rsp, LOW_STACK_TOP
    mov rbp, MARK_A
    push rbp

    ; RSP must be canonical, and exactly one slot lower.
    mov rbx, LOW_STACK_TOP - 8
    cmp rsp, rbx
    jne done

    ; Read the pushed value back through an ordinary translated load. A push
    ; that skipped the translation wrote the canonical page and this reads
    ; the alias, so the compare fails.
    mov r10, LOW_STACK_TOP - 8
    mov rcx, qword [r10]
    mov rdx, MARK_A
    cmp rcx, rdx
    jne done

    ; ===============================================================
    ; pop, against an alias seeded with a value the canonical page cannot
    ; hold. A pop that skipped the translation returns the wrong value.
    ; ===============================================================
    mov r10, LOW_STACK_TOP - 8
    mov rdx, MARK_B
    mov qword [r10], rdx          ; ordinary translated store -> the alias
    mov rsp, LOW_STACK_TOP - 8
    pop rcx
    mov rdx, MARK_B
    cmp rcx, rdx
    jne done
    mov rbx, LOW_STACK_TOP
    cmp rsp, rbx
    jne done

    ; ===============================================================
    ; push rsp: the value and the address are the same register.
    ; ===============================================================
    mov rsp, LOW_STACK_TOP
    push rsp
    mov rbx, LOW_STACK_TOP - 8
    cmp rsp, rbx
    jne done
    ; x86-64 pushes the value of RSP *before* the decrement.
    mov r10, LOW_STACK_TOP - 8
    mov rcx, qword [r10]
    mov rdx, LOW_STACK_TOP
    cmp rcx, rdx
    jne done

    ; ===============================================================
    ; Adjacent pushes and pops, which the IR fuses into PushTwo/PopTwo.
    ; ===============================================================
    mov rsp, LOW_STACK_TOP
    mov r8, MARK_C
    mov r9, MARK_D
    push r8
    push r9

    mov rbx, LOW_STACK_TOP - 16
    cmp rsp, rbx
    jne done
    ; Both slots, read through translated loads.
    mov r10, LOW_STACK_TOP - 8
    mov rcx, qword [r10]
    mov rdx, MARK_C
    cmp rcx, rdx
    jne done
    mov r10, LOW_STACK_TOP - 16
    mov rcx, qword [r10]
    mov rdx, MARK_D
    cmp rcx, rdx
    jne done

    ; Seed both slots through translated stores, then pop the pair.
    mov r10, LOW_STACK_TOP - 16
    mov rdx, MARK_A
    mov qword [r10], rdx
    mov r10, LOW_STACK_TOP - 8
    mov rdx, MARK_B
    mov qword [r10], rdx
    mov rsp, LOW_STACK_TOP - 16
    pop r8
    pop r9
    mov rdx, MARK_A
    cmp r8, rdx
    jne done
    mov rdx, MARK_B
    cmp r9, rdx
    jne done
    mov rbx, LOW_STACK_TOP
    cmp rsp, rbx
    jne done

    ; ===============================================================
    ; call/ret on the canonical low stack. The return address is pushed and
    ; popped by the same machinery.
    ; ===============================================================
    mov rsp, LOW_STACK_TOP
    mov r11, 0
    call callee
    ; The callee ran and returned to the right place.
    cmp r11, 1
    jne done
    mov rbx, LOW_STACK_TOP
    cmp rsp, rbx
    jne done

    ; ===============================================================
    ; The same paths on a top-arena stack, so the second lane is covered.
    ; ===============================================================
    mov rsp, TOP_STACK_TOP
    mov rbp, MARK_C
    push rbp
    mov rbx, TOP_STACK_TOP - 8
    cmp rsp, rbx
    jne done
    mov r10, TOP_STACK_TOP - 8
    mov rcx, qword [r10]
    mov rdx, MARK_C
    cmp rcx, rdx
    jne done

    mov r10, TOP_STACK_TOP - 8
    mov rdx, MARK_D
    mov qword [r10], rdx
    mov rsp, TOP_STACK_TOP - 8
    pop rcx
    mov rdx, MARK_D
    cmp rcx, rdx
    jne done
    mov rbx, TOP_STACK_TOP
    cmp rsp, rbx
    jne done

    mov rsp, TOP_STACK_TOP
    mov r8, MARK_A
    mov r9, MARK_B
    push r8
    push r9
    mov rbx, TOP_STACK_TOP - 16
    cmp rsp, rbx
    jne done
    mov rsp, TOP_STACK_TOP - 16
    pop r8
    pop r9
    mov rdx, MARK_B
    cmp r8, rdx
    jne done
    mov rdx, MARK_A
    cmp r9, rdx
    jne done

    mov rsp, TOP_STACK_TOP
    mov r11, 0
    call callee
    cmp r11, 1
    jne done
    mov rbx, TOP_STACK_TOP
    cmp rsp, rbx
    jne done

    ; ===============================================================
    ; A byte-sized scratch check, so the sub-word store path is covered on
    ; a canonical address as well.
    ; ===============================================================
    mov r10, SCRATCH
    mov rdx, POISON
    mov qword [r10], rdx
    mov rcx, qword [r10]
    cmp rcx, rdx
    jne done

    mov rax, PASS

done:
    hlt

callee:
    mov r11, 1
    ret
