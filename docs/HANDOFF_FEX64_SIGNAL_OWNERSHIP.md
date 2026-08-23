# The x64 "translator stall" is a host signal-ownership defect

## Symptom

Both supplied x64 traces (`boxedvn-20260823-001716.log`,
`boxedvn-20260823-001822.log`) end the same way, within about a second of
`wine64` starting:

```
BOXEDWINE_FEX64_STALL pid=10 tid=11 stable_samples=3 state=running
  host_pc=0x1988271e8 guest_rip=0x71480c32c0 faults=0 handled=0 last_signal=0
BOXEDWINE_FEX64_STALL_HOST_IMAGE image=/usr/lib/system/libsystem_platform.dylib
  symbol_base=0x1988271e4 symbol_offset=0x4
```

The guest RIP, every guest GPR, the host PC and the host SP are then frozen for
the rest of the session while the thread reports `state=running`.

## What the sample actually shows

`libsystem_platform.dylib` + 4 with

```
x0=0x100805a14  x1=0x1e  x2=0x4  x3=0x16fdb6900  x4=0x16fdb6968  x5=<random>
```

is Darwin's `_sigtramp(catcher, sigstyle, sig, sinfo, uctx, token)`:

* `x1 = 0x1e` is `UC_FLAVOR`,
* `x2 = 4` is **SIGILL**,
* `x4 - x3 = 0x68` is `sizeof(siginfo_t)`, so those are the siginfo/ucontext
  pair,
* `x0 = 0x100805a14` is the installed handler, inside the app binary,
* `x5` changes on every sample because it is the trampoline token.

`host_lr = <jit pool>+0x1004174` disassembles to
`mov x0,x28 / mov x1,x30 / ldr x2,[x28,#1592] / blr x2`, which is FEXCore's
`ExitFunctionLinker` stub (`Pointers.ExitFunctionLink`). Darwin does not clear
LR when it delivers a signal, and FEX reaches its fault stubs with `br`, not
`bl`, so LR still names the last real call.

The guest RIP resolves exactly: libc is mapped at `0x7148008000`, so
`0x71480c32c0` is libc + `0xBB2C0`, whose bytes in the pinned rootfs are

```
f3 0f 1e fa   endbr64
66 0f 6e c6   movd  xmm0, esi
48 89 f8      mov   rax, rdi
66 0f 60 c0   punpcklbw xmm0, xmm0
66 0f 61 c0   punpcklwd xmm0, xmm0
66 0f 70 c0 00 pshufd xmm0, xmm0, 0
```

— `__memset_sse2_unaligned_erms`. The frozen GPRs (`rax = dst`,
`rdx = 0x518 = len`, `rdi = rax + rdx`, `rsi = 0`) place the guest partway
through that routine's `movups` prologue, and they are trustworthy because
FEX's `GuestSignal_*` stubs run `SpillStaticRegs` before they trap.

## Root cause

1. `BVNFEXBackendProbe()` calls `installFEXHostSignalHandlers()`, which installs
   `fexHostSignalHandler` for SIGSEGV/SIGBUS/SIGILL/SIGFPE and remembers the
   previous actions. This happens during the pre-launch probe.
2. Every guest thread then starts in `platformThread()`, which calls
   `platformInitExceptionHandling()`. That function installed `platformHandler`
   for the same four signals and **discarded `oldsa`**, so FEX's handler was
   unhooked. `std::call_once` means it was never reinstalled. This is why the
   traces report `faults=0 handled=0 last_signal=0`: FEX's handler never ran
   once.
3. FEX reports an invalid or unimplemented guest instruction by writing
   `CpuStateFrame::SynchronousFaultData` and branching to
   `Pointers.GuestSignal_SIGILL`, which is `SpillStaticRegs; hlt(0)`. `hlt` from
   EL0 is undefined, so it raises a host SIGILL.
4. `platformHandler` could not attribute that fault (`x19` holds a FEX static
   register, not a Boxedwine `CPU*`) and simply `return`ed. Returning from a
   synchronous fault handler without changing the context re-executes the
   faulting instruction, so the kernel re-delivered SIGILL forever.

That loop is the "stall". It is also the real explanation for the earlier
"the pinned glibc loader's SSE2 memset does not retire" finding that
`normalizeFEX64InterpreterMemset()` in `source/kernel/loader/loader64.cpp`
works around: the routine did not hang, it hit this signal loop. libc's copy of
the same routine is not covered by that patch, so the loop reappeared as soon
as libc was mapped.

## Fixes applied

* `platform/linux/platformThreads.cpp` records the previously installed action
  for each of the four signals and exposes `platformChainHostSignal()`.
  `sa_mask` is now initialised (it was an uninitialised stack `struct
  sigaction`).
* `platform/linux/platformThreads-armv8.cpp` and `-x64.cpp` forward every fault
  they cannot attribute to a Boxedwine CPU instead of returning.
* `ios/runtime/src/BVNFEXCPU64Adapter.mm` accepts faults whose PC lies in
  FEX's dispatcher when `SynchronousFaultData.FaultToTopAndGeneratedException`
  is set, takes the signal/trap/si_code from FEX rather than from the host
  siginfo, and does **not** re-read the static registers (the stub already
  spilled them). If the guest has no handler for that signal it unwinds FEX the
  way FEX's own `ExitOnHLT` path does — restore `ReturningStackLocation` into
  SP, jump to `ThreadStopHandlerAddress` — and retires the guest process,
  instead of letting Darwin's default action kill the whole app.
* Both the accepted and the declined case are now logged
  (`BOXEDWINE_FEX64_GUEST_FAULT`, `BOXEDWINE_FEX64_FAULT_DECLINED`), including
  the guest RIP and the 16 guest bytes at it.

## What the next device run has to answer

The signal plumbing is now correct, but it does not by itself say **which**
x86-64 instruction FEX refused. The next run should print, once:

```
BOXEDWINE_FEX64_GUEST_FAULT pid=.. tid=.. signal=4 trapno=6 si_code=2
  err=0 rip=0x71480c3??? bytes=[..]
```

`trapno=6 / si_code=2` is `#UD` from `UnimplementedOp`/`InvalidOp`;
`signal=11 / trapno=13` would instead be `NoExecOp`, i.e. the decoder walking
off the range returned by `BVNFEXCPU64AdapterQueryExecutableRange`. Those two
lead to completely different fixes, and the `bytes=[..]` field names the
instruction outright. `scripts/guest-probes/fex64-loader-stall.asm` already
encodes this exact memset shape instruction-for-instruction, so once the byte
sequence is known it can be reproduced under the VIXL harness on a Linux x86-64
host without a device.
