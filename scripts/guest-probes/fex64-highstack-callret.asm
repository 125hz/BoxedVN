; FEX translator conformance fixture for nested CALL/LEAVE/RET on a relocated
; guest stack.
;
; Runs with BoxedWine's address translation ENABLED, via the test-only
; FEX_BOXEDWINE_ALIAS path in TestHarnessRunner, because that is the
; configuration the device runs and the one this defect lives in.
;
; WHAT THIS IS FOR
;
; A device run stopped in the epilogue of glibc's __libc_sigaction, at guest
; 0x7a4004d4b9:
;
;     0x454aa  mov  rax, [rbp-8]        ; stack-protector canary
;     0x454ae  sub  rax, fs:[0x28]
;     0x454b7  jne  __stack_chk_fail
;     0x454b9  leave
;     0x454ba  mov  eax, edx
;     0x454bc  ret
;
; on a thread whose stack was in Wine's top-down arena (rbp = 0x7ffffe1fe938),
; and the host crashed with PC 0 and fault address 0. The canary compare had
; already succeeded, which proves the arena stack was mapped and read correctly;
; the RET then loaded its target from the correctly translated host address
; (x3 held 0x7ffe1fe940, the alias of rbp+8) and branched to host address zero.
;
; The cause was in the emitted L1 lookup in DEF_OP(ExitFunction): it compared
; only the cached GUEST key. An empty L1 slot is {HostCode = 0, GuestCode = 0},
; so a dynamic guest target of zero matched slot zero and the emitted `ret TMP2`
; branched to host address zero. Every call whose return block is unknown also
; pushes a {0, 0} pair onto the call-return prediction stack, which is the same
; hazard on the prediction path.
;
; WHAT THIS FIXTURE PROVES
;
; Everything on the guest side of that sequence, on both stacks BoxedWine
; relocates -- the canonical low stack served through the OR alias, and the
; top-down arena served through the cleared-field alias:
;
;   - the return address a CALL pushes survives, and is readable at [rbp+8]
;     through an ordinary translated load. The push goes through DEF_OP(Push)
;     and the read through GenerateMemOperand, so the two paths have to agree
;     on where the guest stack lives, not merely agree with each other. The
;     expected value is carried in a register by each call site, so a callee
;     reached from several sites checks the address that site actually pushed.
;   - LEAVE restores RSP and RBP to the values the caller had.
;   - RET's dynamic target is that same qword, and execution actually arrives
;     at the instruction after the call.
;   - a first execution of every return site is an L1 miss, so arriving proves
;     the miss path produced a real, non-null host target through the
;     dispatcher rather than branching to whatever the empty slot held. The
;     nest is then run a second time on each stack, which takes the hits.
;   - an indirect CALL through a register, whose return block is not known at
;     compile time, is the case that pushes the zeroed prediction entry. Its
;     RET has to be as correct as any other.
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

; The canonical low stack, served through `guest | alias_base`.
%define LOW_STACK 0x7ffc8000
; Wine's top-down arena, served through `(guest | alias_base) & ~clear_mask`.
; This is the lane the device thread was on.
%define TOP_STACK 0x7ffffe008000

%define PASS 0x4645585f50415353 ; "FEX_PASS"
%define FAIL 0x4645585f4641494c ; "FEX_FAIL"

; A recognisable local, standing in for the stack-protector canary. The point
; is not the value but that a local at [rbp-8] survives the nested calls and
; the helper boundary, exactly as the device's canary did.
%define CANARY 0xc0ffee5533aa1177

; RDI carries the return address the current call site is about to push, so a
; callee reached from several sites can check the one that applies.

    mov rax, FAIL

    ; ---------------------------------------------------------------
    ; Phase 1: the canonical low stack, cold. Every return site below
    ; is compiled for the first time here, so every RET is an L1 miss.
    ; ---------------------------------------------------------------
    mov rsp, LOW_STACK
    mov r15, 0                     ; lane marker: 0 = low alias
    lea rdi, [rel low_cold_return]
    call run_nest
low_cold_return:
    mov r11, PASS
    cmp r14, r11
    jne fail_now
    mov r11, LOW_STACK
    cmp rsp, r11
    jne fail_now

    ; ---------------------------------------------------------------
    ; Phase 2: the top-down arena, which is the lane the device failed
    ; on. Same code, different stack: only the translation changes.
    ; ---------------------------------------------------------------
    mov rsp, TOP_STACK
    mov r15, 1                     ; lane marker: 1 = top arena
    lea rdi, [rel top_cold_return]
    call run_nest
top_cold_return:
    mov r11, PASS
    cmp r14, r11
    jne fail_now
    mov r11, TOP_STACK
    cmp rsp, r11
    jne fail_now

    ; ---------------------------------------------------------------
    ; Phase 3: run both lanes again. The return sites are warm now, so
    ; these RETs take the L1 hit path instead of the dispatcher path.
    ; Both have to arrive at the same place.
    ; ---------------------------------------------------------------
    mov rsp, LOW_STACK
    mov r15, 0
    lea rdi, [rel low_warm_return]
    call run_nest
low_warm_return:
    mov r11, PASS
    cmp r14, r11
    jne fail_now
    mov r11, LOW_STACK
    cmp rsp, r11
    jne fail_now

    mov rsp, TOP_STACK
    mov r15, 1
    lea rdi, [rel top_warm_return]
    call run_nest
top_warm_return:
    mov r11, PASS
    cmp r14, r11
    jne fail_now
    mov r11, TOP_STACK
    cmp rsp, r11
    jne fail_now

    mov rax, PASS
    hlt

; -------------------------------------------------------------------
; run_nest: three levels deep, each with the __libc_sigaction frame
; shape -- push rbp / mov rbp, rsp / sub rsp, N / local at [rbp-8] /
; leave / ret. The middle level crosses a helper boundary by calling
; through a register.
;
; In:  RDI = the return address this call site pushed
;      R15 = lane marker
; Out: R14 = PASS when every check held
; -------------------------------------------------------------------
run_nest:
    push rbp
    mov rbp, rsp
    sub rsp, 0x20
    mov r14, FAIL

    ; The qword the CALL pushed, read back through an ordinary translated
    ; load, has to be the address this call site named.
    mov r8, qword [rbp + 8]
    cmp r8, rdi
    jne run_nest_out

    ; The saved frame pointer LEAVE will restore sits at [rbp].
    mov rsi, qword [rbp]

    ; A local below the frame pointer, where the canary lives.
    mov r10, CANARY
    mov qword [rbp - 8], r10

    lea rdi, [rel run_nest_after_call]
    call level_one
run_nest_after_call:
    mov r11, PASS
    cmp r13, r11
    jne run_nest_out

    ; The local survived three nested frames and a helper boundary.
    mov r10, qword [rbp - 8]
    mov r11, CANARY
    cmp r10, r11
    jne run_nest_out

    ; The saved frame pointer is still the one the prologue stored.
    mov r8, qword [rbp]
    cmp r8, rsi
    jne run_nest_out

    mov r14, PASS

run_nest_out:
    leave
    ret

; -------------------------------------------------------------------
level_one:
    push rbp
    mov rbp, rsp
    sub rsp, 0x10
    mov r13, FAIL

    mov r8, qword [rbp + 8]
    cmp r8, rdi
    jne level_one_out

    ; A helper boundary: an indirect call whose return block the
    ; translator cannot know at compile time. This is the case that
    ; pushes a zeroed call-return prediction entry, and therefore the
    ; case whose RET could match an empty cache slot.
    lea rax, [rel level_two]
    lea rdi, [rel level_one_after_indirect]
    call rax
level_one_after_indirect:
    mov r11, PASS
    cmp r12, r11
    jne level_one_out

    mov r13, PASS

level_one_out:
    leave
    ret

; -------------------------------------------------------------------
level_two:
    push rbp
    mov rbp, rsp
    sub rsp, 0x10
    mov r12, FAIL

    ; Reached through `call rax`, so the pushed return address is the
    ; instruction after that indirect call.
    mov r8, qword [rbp + 8]
    cmp r8, rdi
    jne level_two_out

    lea rdi, [rel level_two_after_call]
    call level_three
level_two_after_call:
    mov r11, PASS
    cmp r10, r11
    jne level_two_out

    mov r12, PASS

level_two_out:
    leave
    ret

; -------------------------------------------------------------------
; The deepest frame. No further calls, so its RET is the one most
; likely to meet a cold L1 entry on the first pass.
level_three:
    push rbp
    mov rbp, rsp
    mov r10, FAIL

    mov r8, qword [rbp + 8]
    cmp r8, rdi
    jne level_three_out

    ; A qword one frame further down the same stack: memory below the
    ; current RSP is usable on both lanes.
    mov r8, rsp
    sub r8, 0x40
    mov r11, CANARY
    mov qword [r8], r11
    mov r9, qword [r8]
    mov r11, CANARY
    cmp r9, r11
    jne level_three_out

    ; The lane marker survived every frame, so neither lane clobbered a
    ; caller-visible register through a mistranslated stack write.
    cmp r15, 1
    ja level_three_out

    mov r10, PASS

level_three_out:
    leave
    ret

fail_now:
    mov rax, FAIL
    hlt
