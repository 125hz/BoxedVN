; FEX translator conformance fixture for REP MOVS/STOS through the alias.
;
; This one runs with BoxedWine's address translation ENABLED, via the
; test-only FEX_BOXEDWINE_ALIAS path in TestHarnessRunner. That distinction is
; the whole point: fex64-top-alias-contract.asm runs with the translation off,
; so it could not catch a memory operation that forgot to apply it.
;
; The device did. DEF_OP(MemCpy) ORed the low alias base into its working
; pointers and never applied the top relocation, so a canonical top-arena
; pointer -- where the OR is a no-op -- reached the vectorised ldp/stp loop
; unchanged and faulted on 0x7fffffa10188.
;
; Guest registers hold CANONICAL addresses throughout. The harness maps each
; region at its translated host address as well, so a pointer that reaches the
; host untranslated lands on unmapped memory and faults, while a correctly
; translated one reads and writes the bytes this fixture compares.
;
; Lengths are chosen to force the vectorised path the device used and to leave
; a tail the scalar path has to finish.
;
; Every address reaches memory through a register: x86-64 absolute memory
; operands are disp32, so a 47-bit constant written as [ADDRESS] would be
; silently truncated by the assembler rather than by the translator.
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
    "0x7ffffe000000": "0x20000",
    "0x100000": "0x10000"
  }
}
%endif

BITS 64
ORG 0x10000

; Canonical guest addresses. Their host aliases are
;   0x7ffffe000000 -> 0x7ffe000000   (top arena: clear bits 39..46)
;   0x7ffffe010000 -> 0x7ffe010000
;   0x00100000     -> 0x7800100000   (canonical low: OR the alias base)
%define ARENA_SRC 0x7ffffe000000
%define ARENA_DST 0x7ffffe010000
%define LOW_BUF   0x100000

%define STACK 0xe0001000
%define PASS  0x4645585f50415353 ; "FEX_PASS"
%define FAIL  0x4645585f4641494c ; "FEX_FAIL"

%define PATTERN_A 0x1122334455667788
%define PATTERN_B 0x99aabbccddeeff00
%define FILLBYTE  0x5a

; 0x210 bytes: 33 whole 16-byte pairs plus a remainder, so the vectorised loop
; runs and a tail is left over.
%define COPY_BYTES 0x210
%define COPY_QWORDS (COPY_BYTES / 8)
%define LAST_QWORD (COPY_BYTES - 8)

    mov rsp, STACK
    mov rax, FAIL
    cld

    ; ---------------------------------------------------------------
    ; Seed the arena source with a recognisable ascending pattern.
    ; ---------------------------------------------------------------
    mov rdi, ARENA_SRC
    mov rcx, COPY_QWORDS
    mov rdx, PATTERN_A
seed_arena:
    mov qword [rdi], rdx
    add rdi, 8
    add rdx, 1
    dec rcx
    jnz seed_arena

    ; ---------------------------------------------------------------
    ; REP MOVSQ: top arena -> top arena. Both pointers need relocating.
    ; ---------------------------------------------------------------
    mov rsi, ARENA_SRC
    mov rdi, ARENA_DST
    mov rcx, COPY_QWORDS
    rep movsq

    ; RSI/RDI/RCX must be guest-visible canonical values afterwards.
    mov rbx, ARENA_SRC + COPY_BYTES
    cmp rsi, rbx
    jne done
    mov rbx, ARENA_DST + COPY_BYTES
    cmp rdi, rbx
    jne done
    test rcx, rcx
    jnz done

    ; Verify the copy at both ends.
    mov r9, ARENA_DST
    mov rdx, PATTERN_A
    mov rbx, qword [r9]
    cmp rbx, rdx
    jne done
    add rdx, COPY_QWORDS - 1
    mov rbx, qword [r9 + LAST_QWORD]
    cmp rbx, rdx
    jne done

    ; ---------------------------------------------------------------
    ; REP MOVSB: canonical LOW source -> top arena destination. The two
    ; pointers take different lanes, so a translation applied to only one
    ; of them fails here.
    ; ---------------------------------------------------------------
    mov rdi, LOW_BUF
    mov rcx, COPY_QWORDS
    mov rdx, PATTERN_B
seed_low:
    mov qword [rdi], rdx
    add rdi, 8
    add rdx, 1
    dec rcx
    jnz seed_low

    mov rsi, LOW_BUF
    mov rdi, ARENA_DST
    mov rcx, COPY_BYTES
    rep movsb

    mov rbx, LOW_BUF + COPY_BYTES
    cmp rsi, rbx
    jne done
    mov rbx, ARENA_DST + COPY_BYTES
    cmp rdi, rbx
    jne done

    mov r9, ARENA_DST
    mov rdx, PATTERN_B
    mov rbx, qword [r9]
    cmp rbx, rdx
    jne done
    add rdx, COPY_QWORDS - 1
    mov rbx, qword [r9 + LAST_QWORD]
    cmp rbx, rdx
    jne done

    ; ---------------------------------------------------------------
    ; REP MOVSB the other way: top arena source -> canonical LOW dest.
    ; ---------------------------------------------------------------
    mov rsi, ARENA_DST
    mov rdi, LOW_BUF
    mov rcx, COPY_BYTES
    rep movsb
    mov r9, LOW_BUF
    mov rdx, PATTERN_B
    mov rbx, qword [r9]
    cmp rbx, rdx
    jne done

    ; ---------------------------------------------------------------
    ; REP STOSQ into the top arena. RAX is the store value from here on,
    ; so failures below report through fail_now rather than falling into
    ; done with a pattern in RAX.
    ; ---------------------------------------------------------------
    mov rdi, ARENA_SRC
    mov rcx, COPY_QWORDS
    mov rax, PATTERN_B
    rep stosq

    mov rbx, ARENA_SRC + COPY_BYTES
    cmp rdi, rbx
    jne fail_now
    test rcx, rcx
    jnz fail_now
    mov r9, ARENA_SRC
    mov rdx, PATTERN_B
    mov rbx, qword [r9]
    cmp rbx, rdx
    jne fail_now
    mov rbx, qword [r9 + LAST_QWORD]
    cmp rbx, rdx
    jne fail_now

    ; ---------------------------------------------------------------
    ; REP STOSB into the top arena, with an odd length so the vectorised
    ; store loop leaves a scalar tail.
    ; ---------------------------------------------------------------
    mov rdi, ARENA_DST
    mov rcx, COPY_BYTES + 5
    mov al, FILLBYTE
    rep stosb

    mov rbx, ARENA_DST + COPY_BYTES + 5
    cmp rdi, rbx
    jne fail_now
    mov r9, ARENA_DST
    movzx rbx, byte [r9]
    cmp rbx, FILLBYTE
    jne fail_now
    movzx rbx, byte [r9 + COPY_BYTES + 4]
    cmp rbx, FILLBYTE
    jne fail_now
    ; One byte past the fill was never written by anything above, so the
    ; freshly mapped page still reads zero there. A store that ran on would
    ; have left the fill byte instead.
    movzx rbx, byte [r9 + COPY_BYTES + 5]
    test rbx, rbx
    jnz fail_now

    ; ---------------------------------------------------------------
    ; Backward REP MOVSB (DF set) between two top-arena buffers.
    ; ---------------------------------------------------------------
    mov rdi, ARENA_SRC
    mov rcx, COPY_QWORDS
    mov rdx, PATTERN_A
seed_back:
    mov qword [rdi], rdx
    add rdi, 8
    add rdx, 1
    dec rcx
    jnz seed_back

    std
    mov rsi, ARENA_SRC + COPY_BYTES - 1
    mov rdi, ARENA_DST + COPY_BYTES - 1
    mov rcx, COPY_BYTES
    rep movsb
    cld

    mov rbx, ARENA_SRC - 1
    cmp rsi, rbx
    jne fail_now
    mov rbx, ARENA_DST - 1
    cmp rdi, rbx
    jne fail_now
    mov r9, ARENA_DST
    mov rdx, PATTERN_A
    mov rbx, qword [r9]
    cmp rbx, rdx
    jne fail_now
    add rdx, COPY_QWORDS - 1
    mov rbx, qword [r9 + LAST_QWORD]
    cmp rbx, rdx
    jne fail_now

    mov rax, PASS
    hlt

fail_now:
    mov rax, FAIL
done:
    hlt
