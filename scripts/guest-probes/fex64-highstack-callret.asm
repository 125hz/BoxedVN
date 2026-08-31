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
;   - every register-overlap shape the Push lowering branches on, including
;     `push rsp`, where the value register is the address register, and a pair
;     of adjacent pushes, which the register allocator fuses into one paired
;     store. Each is read back through an ordinary translated load.
;   - the return address read straight off the stack on entry to a callee,
;     before anything else touches it -- the guest-side equivalent of the
;     read-back the CALL push itself now performs.
;   - a dispatcher boundary between a frame's CALL and its RET, so the frame
;     has to survive a round trip out of translated code and back.
;
; What this fixture deliberately does NOT cover: a guest RET to a null target.
; That has to become a guest fault, and a guest fault ends the harness rather
; than producing a pass or a fail, so the disposition is pinned in the host
; test suite instead (see fex_null_guest_target_takes_the_guest_fault_path).
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

    ; Every Push lowering shape, on this same lane.
    lea rdi, [rel run_nest_after_shapes]
    call push_shapes
run_nest_after_shapes:
    mov r11, PASS
    cmp rbx, r11
    jne run_nest_out

    mov r14, PASS

run_nest_out:
    leave
    ret

; -------------------------------------------------------------------
; push_shapes: the register-overlap shapes the Push lowering has to
; handle, plus the immediate read-back a CALL's own return-address push
; is now instrumented with.
;
; The lowering copies the pushed value into a temporary when it would
; otherwise be destroyed by the address write or the result write. Those
; branches are chosen by register allocation, which a guest fixture
; cannot address directly -- but the x86 forms below are what force
; them, and each one is read back through an ordinary translated load,
; so a shape that stores to the wrong place fails on the value.
;
; In:  RDI = the return address this call site pushed
; Out: RBX = PASS when every check held
; -------------------------------------------------------------------
push_shapes:
    ; The return address, read straight off the stack before anything
    ; else touches it. This is the guest-side equivalent of the
    ; read-back the CALL push now performs: if the push never landed,
    ; this is the first place it shows.
    mov r8, qword [rsp]
    mov rbx, FAIL
    cmp r8, rdi
    jne push_shapes_done_nostack

    push rbp
    mov rbp, rsp

    ; Shape 1: value, address and result all distinct.
    mov r8, CANARY
    push r8
    mov r9, qword [rsp]
    cmp r9, r8
    jne push_shapes_out
    pop r10
    cmp r10, r8
    jne push_shapes_out

    ; Shape 2: the value register IS the address register. x86 pushes
    ; the stack pointer's value from BEFORE the decrement, so a lowering
    ; that stores the already-adjusted pointer fails here.
    mov r11, rsp
    push rsp
    mov r9, qword [rsp]
    cmp r9, r11
    jne push_shapes_out
    add rsp, 8

    ; Shape 3: a live caller-visible register that the epilogue still
    ; needs afterwards.
    push rbp
    mov r9, qword [rsp]
    cmp r9, rbp
    jne push_shapes_out
    add rsp, 8

    ; Shape 4: two adjacent pushes, which the register allocator fuses
    ; into a single paired store. The second push lands at the lower
    ; address, so the order below is the one that has to hold.
    mov r8, 0x1234567890abcdef
    mov r9, 0x0fedcba987654321
    push r8
    push r9
    mov r10, qword [rsp]
    cmp r10, r9
    jne push_shapes_out
    mov r10, qword [rsp + 8]
    cmp r10, r8
    jne push_shapes_out
    add rsp, 16

    ; A dispatcher boundary between this frame's CALL and its RET: an
    ; indirect branch whose target the translator cannot know at compile
    ; time, so the block ends and control re-enters through the exit
    ; path. (A syscall would do the same, but the harness has no kernel
    ; behind it, and what matters here is only that the frame survives a
    ; round trip out of translated code and back.)
    lea rax, [rel push_shapes_resume]
    jmp rax
push_shapes_resume:

    ; The frame is intact across that boundary.
    mov r8, qword [rbp + 8]
    cmp r8, rdi
    jne push_shapes_out

    mov rbx, PASS

push_shapes_out:
    leave
    ret

push_shapes_done_nostack:
    ; The return address was already wrong on entry, so the frame this
    ; routine would build cannot be trusted either. Return without one.
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
