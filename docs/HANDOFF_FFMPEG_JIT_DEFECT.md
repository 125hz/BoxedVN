# Boxedwine ARM64 JIT: wrong register value before `DIV ECX` in ffmpeg.dll

I'm debugging an x86-on-ARM64 emulator and have isolated a JIT
mistranslation to one module. I need help narrowing it further. Please
reason about the JIT, not about the game.

## The stack

- **Boxedwine** — a 32-bit x86 emulator that runs a real 32-bit Wine inside an
  emulated Linux kernel. It has an ARM64 JIT (`source/emulation/cpu/armv8/`,
  with the architecture-independent translation in
  `source/emulation/cpu/jit/`) and a fallback interpreter
  (`source/emulation/cpu/normal/`).
- Running on iOS (Apple A19), 32-bit guest, ARM64 host.
- **Guest**: an NW.js application (Chromium ~70–80) — an RPG Maker MV game.
  The failing module is Chromium's bundled `ffmpeg.dll`, loaded by Wine's
  loader as a PE section at base `0x76B60000`.

## The bug

The guest takes `EXCEPTION_INT_DIVIDE_BY_ZERO` about two seconds into
gameplay, deterministically, killing the Chromium child process and with it
the whole app. Emulator-side snapshot at the trap:

```
Guest divide exception: pid 9f thread a6 divide by zero;
  EIP 76B8D515 ffmpeg.dll+0002D515 (image at 76B60000);
  EAX 00000001 ECX 00000000 EDX 00000000 EBX 00000000
  ESP 174FF498 EBP 174FF4B4 ESI 00000001 EDI 158B9D00;
  bytes F7 F1 50 FF 37 68 4C 60 D0 76 6A 30 6A 00 E8 4D
```

Decoding the bytes:

```
F7 F1                   div  ecx              ; EDX:EAX / ECX
50                      push eax
FF 37                   push dword [edi]
68 4C 60 D0 76          push 0x76D0604C
6A 30                   push 0x30
6A 00                   push 0
E8 4D ...               call ...
```

So: `EDX:EAX = 0x0000000000000001` divided by `ECX = 0`, then the quotient
is passed as an argument alongside `0x30` (= 48, which is `AV_LOG_DEBUG`) and
a pointer into the same image — the shape of
`av_log(NULL, AV_LOG_DEBUG, fmt, [edi], 1/ecx)`. FFmpeg computes a ratio for
a log line whose arguments are evaluated even when the message is discarded.

Two different threads of the same process reach the same EIP with **identical
register values**, so it is deterministic rather than a race.

## The isolating experiment

Boxedwine can force one module through the interpreter instead of the JIT
(`-interpreterModule ffmpeg`, matched against the module name containing the
faulting EIP).

- **JIT**: the divide-by-zero fires on every run — 5+ consecutive runs, same
  EIP, same registers.
- **Interpreter for `ffmpeg.dll` only, everything else still JIT**: zero
  divide exceptions, the game runs on and reaches its title screen.

So the JIT is producing a different `ECX` at that instruction than the
interpreter does. Since the divisor really is zero *as the JIT sees it*, and
the interpreter never trips, the fault is almost certainly **an earlier
instruction in `ffmpeg.dll` translated incorrectly**, leaving a wrong value in
`ECX` (or in whatever it was loaded from) by the time `div ecx` executes.

Caveat I want to keep honest: interpreting the module also changes timing, so
a timing-dependent bug is not formally excluded. The identical registers
across two threads argue against that.

## How Boxedwine translates `div`

`Jit::div32` in `source/emulation/cpu/jit/jitArith.h`:

```cpp
IfNot(JitWidth::b32, src); {          // src == 0 ->
    emulateSingleOp();                //   fall back to the interpreter for
} EndIf();                            //   this one op, which raises #DE

RegPtr edx = getReg(2, 2);
// unsigned path:
IfGreaterThanOrEqual(JitWidth::b32, ComparisonType::Unsigned, edx, src); {
    emulateSingleOp();                // quotient overflow -> same fallback
} EndIf();
RegPtr eax = getReg(0, 0);
(this->*callback)(JitWidth::b32, eax, edx, src);
```

So the `#DE` is raised by the interpreter re-executing the single
instruction after the JIT observes `src == 0`. The registers in my snapshot
are therefore the JIT's register state at that moment — which is exactly the
state I believe is wrong.

## What I'm asking

1. **Where would you look first** for an ARM64 JIT mistranslation that
   produces a wrong general-purpose register value, given the fault surfaces
   at a `div` but the `div` translation itself looks sound? My working
   assumption is that `div` is only the *detector*.
2. Are there **known-difficult x86 sequences** on an ARM64 backend that
   commonly produce a wrong register — lazy/deferred flag evaluation, partial
   register writes (8/16-bit subregisters aliasing the 32-bit register),
   `rep`-prefixed string ops, `cmpxchg`, `xchg`, shift/rotate flag semantics,
   `imul`/`mul` upper halves, or memory operand address calculation?
3. This codebase has previous form: a `REP MOVS` overlapped-copy defect in
   the JIT that corrupted memory. **A second, possibly related symptom**: when
   Chromium's crash handler runs after the `#DE`, it faults in `ntdll` at
   `mov [ecx+4], eax` with registers holding **ASCII text** where pointers
   belong (`ECX = 0x6D65523F` = `"?Rem"`, `EAX = 0x4A65766F` = `"oveJ"`); on
   another run the same register held `0x0000000A`. That looks like memory
   corruption. Could a single JIT defect plausibly explain both a wrong
   register value and heap corruption — e.g. a mistranslated string/block
   operation writing to the wrong address?
4. **How would you narrow this from a module to an instruction** without a
   guest debugger? Options I can implement: bisect by forcing interpretation
   of address ranges within the module (`-interpreterRange` exists), log every
   JIT block decoded in that module, or diff JIT vs interpreter register state
   at block boundaries. Which is most likely to converge quickly?

## Secondary issues (lower priority, separate problems)

- The game's title screen renders but **buttons and some images are missing**.
  A probe injected into the guest reports `ImageManager.isReady() === false`
  persistently, and two of the game's `@font-face` declarations report
  `status: "error"` (`azuki`, `kaku`) while the main one loads fine.
- The guest window is 816×624 inside a 1280×720 virtual X11 desktop, so it
  does not fill the screen the way other Wine games do.

These are almost certainly unrelated to the JIT defect; I'd rather solve that
one first.


---

## Offline disassembly (the binary is now available)

`ffmpeg.dll` was copied off the device. It verifies against the device
report exactly: `SizeOfImage = 0x314000`, and RVA `0x2D515` is
`F7 F1 50 FF 37 68 4C 60 1A 10 ...`, matching the device dump once the
absolute operand is rebased (`0x101A604C` at file base `0x10000000` ->
`0x76D0604C` at device base `0x76B60000`).

The callee at `+0x5C528` is confirmed `av_reduce`: sign extraction by
`sar eax,0x1f`, absolute value via `add`/`adc`/`xor`, a GCD call, a
Stern-Brocot refinement loop, and finally

```
0005C81A  mov    eax, edx
0005C81C  neg    eax
0005C81E  test   edi, edi
0005C820  cmovns eax, edx
0005C826  mov    dword ptr [ecx], eax   ; *dst_num
0005C82A  mov    dword ptr [ebx], esi   ; *dst_den
```

**It calls out of the tested range.** The interval `76BBC528-76BBC800`
covered only the entry region, and only four blocks executed inside it:

| helper | RVA | device address | called |
|---|---|---|---|
| `av_gcd` | `0x58840` | `0x76BB8840` | 1x |
| 64-bit divide | `0x165480` | `0x76CC5480` | 5x |
| (from `av_gcd`) | `0x16CFE0` | `0x76CC8FE0` | 2x |

The divide helper is MSVC's `__alldiv`, and its normalisation loop is the
strongest suspect in the whole chain:

```
001654E7  shr ebx, 1
001654E9  rcr ecx, 1      ; rotate through carry - CF is an INPUT
001654EB  shr edx, 1
001654ED  rcr eax, 1
001654EF  or  ebx, ebx
001654F1  jne 0x101654E7
```

Each `rcr` consumes the carry produced by the `shr` immediately before it,
and its own carry output is dead - overwritten by the next `shr`. So
`DecodedOp::needsToSetFlags` reports no CF/OF requirement and
`Jit::dynamic_rcr32_reg_op` takes its `else` branch, which reaches `rcr`
through `dynamic_RI(..., &Jit::rcrValue)`. That path does fetch the incoming
carry (`dynamic_RI` calls `getCF()` when `callbackWithCF` is set), so the
plumbing is present; what is not yet verified is whether `getCF()`
materialises the correct value from the lazily-recorded `shr` that produced
it.

That is a hypothesis, not a finding. The decisive test is one run
interpreting `__alldiv` alone:

```
--bvn-interpret-range=76cc5480-76cc5530
```

`av_gcd` is the fallback if that comes back clean; it carries its own
awkward semantics - `shrd`, `sar` by `cl`, `cmova`/`cmovne`/`cmove`, and a
`mov ch, 0x20` followed by `test byte ptr [ebp-0x10], ch`, which is x86
high-byte register aliasing.

## Build 119 correctness fallback

The next supplied device log still used `-interpreterModule ffmpeg`. It no
longer crashed, but Chromium's boot probe stopped at four animation frames,
which confirmed that interpreting the whole media DLL is not shippable.

Build 119 therefore moves the workaround into the emulator core at the
narrowest statically-supported boundary: immediate 32-bit `RCR` on ARM64 is
executed by the reference interpreter for that single instruction, after
which dispatch resumes through the JIT. A CPU regression encodes the exact
`SHR/RCR/SHR/RCR` sequence above. The saved exact
`--bvn-interpret=ffmpeg` diagnostic is retired so existing manifests do not
keep selecting the slow module-wide path.

This is a correctness-first core fallback, not proof that the native ARM64
`RCR` emitter has been repaired. CI can prove the source compiles and the
host-independent suite remains clean; only a fresh physical-device run can
show that this sequence was the bad producer inside `av_reduce` and that the
title advances without the old divide exception.

## Build 119 device result and Build 120 correction

The fresh Build 119 log proved both halves of the earlier diagnosis more
precisely. Chromium advanced from four animation frames to 37 and entered
`Scene_Map`, so retiring module-wide FFmpeg interpretation fixed the stalled
event loop. It then reached the same `ffmpeg.dll+0x2D515` divide with
`ECX=0`, so the first RCR fallback had not corrected the quotient.

The fallback boundary was incomplete: ARM64 JIT `SHR` keeps its result flags
lazy, while `emulateSingleOp()` transfers control to the reference core, whose
`RCR` reads architectural EFLAGS. Branching without first calling
`fillFlags()` therefore made the reference RCR consume stale CF. Build 120
materializes the lazy producer before the one-instruction fallback. This is
still a core CPU-semantics fix and still needs a physical-device run to prove
that `ffmpeg.dll+0x2D515` no longer receives a zero denominator.
