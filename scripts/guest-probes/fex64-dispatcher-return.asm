; FEX translator conformance fixture for Wine's dispatcher return.
;
; Runs with BoxedWine's address translation ENABLED, via the test-only
; FEX_BOXEDWINE_ALIAS path in TestHarnessRunner, because that is the
; configuration the device runs and the one a translation defect hides in.
;
; Wine's server_init_process_done receives the PE entry point and calls
; signal_start_thread -> call_init_thunk, which sets GS to the Windows TEB,
; builds a five-qword iretq frame and enters pLdrInitializeThunk through
; __wine_syscall_dispatcher_return. The device gets as far as the ARCH_SET_GS
; and ARCH_GET_FS that call_init_thunk performs, and then exits 1 without ever
; reaching Windows code -- so the transition itself is what has never been
; proven under this backend.
;
; This reproduces that shape: general registers restored from a saved frame,
; then a far return through iretq onto a different, canonical low stack. It is
; deliberately not FEX's own Primary_CF.asm, which neither restores a register
; set nor crosses BoxedWine's aliased low-address stack.
;
; Every address reaches memory through a register: an absolute x86-64 memory
; operand is disp32 and would truncate a canonical address in the assembler.
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
    "0x7ffc0000": "0x20000"
  }
}
%endif

BITS 64
ORG 0x10000

; A canonical low Unix-side stack the frame is built on, and a separate
; canonical low Windows-side stack iretq switches to. Both are served through
; the host alias; the guest only ever sees these values.
%define UNIX_STACK 0x7ffc8000
%define WIN_STACK  0x7ffd0000
%define FRAME      0x7ffc4000

; The segment selectors Wine's x86-64 path uses: __USER_CS and __USER_DS.
%define WINE_CS 0x33
%define WINE_SS 0x2b
; IF | reserved bit 1, which is what a restored Windows context carries.
%define WINE_FLAGS 0x202

%define PASS 0x4645585f50415353 ; "FEX_PASS"
%define FAIL 0x4645585f4641494c ; "FEX_FAIL"

; The register values the dispatcher frame restores. RCX is the CONTEXT
; pointer and RDX the PEB in Wine's real frame; the values here only have to
; be recognisable.
%define VAL_RBX 0xb1b1b1b1b1b1b1b1
%define VAL_RCX 0xc1c1c1c1c1c1c1c1
%define VAL_RDX 0xd1d1d1d1d1d1d1d1
%define VAL_RSI 0x5151515151515151
%define VAL_RDI 0xd7d7d7d7d7d7d7d7
%define VAL_RBP 0xb9b9b9b9b9b9b9b9

    mov rsp, UNIX_STACK
    mov rax, FAIL
    cld

    ; ---------------------------------------------------------------
    ; Build the saved register frame the dispatcher restores from.
    ; Wine keeps this in its syscall frame; the shape that matters here
    ; is that the values are loaded from guest memory on the aliased
    ; low stack immediately before the far transfer.
    ; ---------------------------------------------------------------
    mov r10, FRAME
    mov rbx, VAL_RBX
    mov qword [r10 + 0x00], rbx
    mov rbx, VAL_RCX
    mov qword [r10 + 0x08], rbx
    mov rbx, VAL_RDX
    mov qword [r10 + 0x10], rbx
    mov rbx, VAL_RSI
    mov qword [r10 + 0x18], rbx
    mov rbx, VAL_RDI
    mov qword [r10 + 0x20], rbx
    mov rbx, VAL_RBP
    mov qword [r10 + 0x28], rbx

    ; ---------------------------------------------------------------
    ; Build the five-qword iretq frame on the Unix stack, in the order
    ; iretq consumes it: RIP, CS, RFLAGS, RSP, SS.
    ; ---------------------------------------------------------------
    mov rsp, UNIX_STACK
    mov rbx, WINE_SS
    push rbx                       ; SS
    mov rbx, WIN_STACK
    push rbx                       ; new RSP
    mov rbx, WINE_FLAGS
    push rbx                       ; RFLAGS
    mov rbx, WINE_CS
    push rbx                       ; CS
    lea rbx, [rel thunk_entry]
    push rbx                       ; target RIP

    ; ---------------------------------------------------------------
    ; Restore the general registers, exactly as CONTEXT_INTEGER would,
    ; and take the frame. Nothing below this may execute.
    ; ---------------------------------------------------------------
    mov r10, FRAME
    mov rbx, qword [r10 + 0x00]
    mov rcx, qword [r10 + 0x08]
    mov rdx, qword [r10 + 0x10]
    mov rsi, qword [r10 + 0x18]
    mov rdi, qword [r10 + 0x20]
    mov rbp, qword [r10 + 0x28]
    iretq

    ; Reached only if iretq fell through, which is itself a failure.
    mov rax, FAIL
    hlt

thunk_entry:
    ; ---------------------------------------------------------------
    ; RFLAGS first, before anything else here can change them. This
    ; also forces the flags to be materialised: the translator keeps
    ; guest arithmetic flags in the host condition register and
    ; computes them lazily, so pushfq is what makes the value the
    ; frame supplied observable at all.
    ;
    ; r13 is not part of the restored set, so it is free to hold this.
    ; ---------------------------------------------------------------
    pushfq
    pop r13

    ; ---------------------------------------------------------------
    ; The stack must be the one the frame supplied, not the one the
    ; iretq frame was built on. Getting this wrong is the difference
    ; between Windows code running on its own stack and running on
    ; Wine's Unix stack. pushfq/pop above left it where it was.
    ; ---------------------------------------------------------------
    mov r11, WIN_STACK
    cmp rsp, r11
    jne fail_now

    ; Every restored register survived the transfer.
    mov r11, VAL_RBX
    cmp rbx, r11
    jne fail_now
    mov r11, VAL_RCX
    cmp rcx, r11
    jne fail_now
    mov r11, VAL_RDX
    cmp rdx, r11
    jne fail_now
    mov r11, VAL_RSI
    cmp rsi, r11
    jne fail_now
    mov r11, VAL_RDI
    cmp rdi, r11
    jne fail_now
    mov r11, VAL_RBP
    cmp rbp, r11
    jne fail_now

    ; RFLAGS came from the frame: IF set, and the arithmetic flags clear
    ; because 0x202 carries none of them. This is the value captured on
    ; entry, not the flags the comparisons above just produced -- reading
    ; them here would only report the last cmp.
    test r13, 0x200                ; IF
    jz fail_now
    test r13, 0x1                  ; CF must be clear
    jnz fail_now
    test r13, 0x40                 ; ZF must be clear
    jnz fail_now

    ; The new stack is usable: a push and pop on it round-trips, and the
    ; pointer returns to where the frame put it.
    mov r11, 0x1234567890abcdef
    push r11
    pop r12
    cmp r11, r12
    jne fail_now
    mov r11, WIN_STACK
    cmp rsp, r11
    jne fail_now

    ; Still 64-bit: a 64-bit operand and a canonical low pointer both work
    ; after the CS reload. A drop out of long mode would fail here.
    mov r10, FRAME
    mov r11, 0x0123456789abcdef
    mov qword [r10 + 0x100], r11
    mov r12, qword [r10 + 0x100]
    cmp r11, r12
    jne fail_now

    mov rax, PASS
    hlt

fail_now:
    mov rax, FAIL
    hlt
