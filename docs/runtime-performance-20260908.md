# CPU performance and startup follow-up

## Evidence

The six device logs ending 235508, 235741, 235939, 000155, 000354 and
001036 identify build 137, revision 24da88bc. They do not establish one shared
cause for every low frame rate.

- The 64-bit managed workload reaches gameplay but accumulates 34,502,990
  handled host faults. A repeated `SWPAL x10,x10,[x3]` site continues faulting
  over 100,000 times/second. FEX emulates each unaligned exchange in its signal
  handler; unlike unaligned acquire/release loads and stores, it does not
  backpatch this operation. Display acquisition/presentation takes microseconds.
- The 32-bit startup regression includes an aligned STR to a read-only guest
  page. The adapter passed it to Wine as SIGBUS/#AC because it checked only
  read permission. Wine needs SIGSEGV/#PF with the write bit for this access.
  Another attempt fails differently; restoring the pre-regression single-block
  mode is a separate conservative rollback, not proof that every fault has
  the same cause.
- The two 2D workloads present roughly 56 and 39 frames/second in the final
  cadence intervals. They do not have the managed workload's atomic fault
  storm. Removing the per-CALL diagnostic applies to all translated programs,
  but the atomic fast path alone cannot explain or fix their frame rates.
- The managed workload cannot instantiate XAudio2 2.7 or DirectSound 8. Both
  COM registrations are absent. It enumerates OSS devices but never starts
  submitting audio. This differs from an active stream suffering underruns.

## Changes

- A BoxedVN-owned AArch64 leaf thunk implements an unaligned 64-bit exchange
  contained within an aligned 16-byte region using an acquire/release CASPAL
  loop. It preserves adjacent bytes, all other registers, NZCV, and SP. SIMD
  and floating-point state are untouched. Naturally aligned operations use
  the original SWPAL. Exchanges spanning two 16-byte regions retain FEX's
  existing handler; this change does not weaken them into plain stores.
- Executable thunk banks are reserved before guest execution, one per 64 MB
  code segment. Signal-time publication uses fixed slots, writable aliases,
  instruction-cache synchronization, and a single atomic branch replacement.
  Slots remain immutable. Racing faults recognize an already published branch.
  Faults in the thunk's guest-memory instructions reconstruct the original
  registers and FEX site before delivering the guest exception.
- Darwin's saved ARM data-abort syndrome determines write access and real
  alignment faults. Repair requires the actual guest permission. Guest Linux
  signal frames carry the x86 page-fault error bits rather than always zero.
- Per-CALL stack witnesses are disabled by default. Set BW64_CALL_WITNESS=1
  to request them; BW64_NO_CALL_WITNESS still overrides. Return prediction,
  full x87 precision, and strict memory ordering remain enabled as before.
- FEX multiblock compilation is restored to the preceding working setting.
- Missing DirectSound classes and XAudio2 2.7 registration are supplied for
  each packaged architecture, preserving existing paths and native overrides.
  This is prefix/runtime repair, with no executable-name matching.

## Validation and device acceptance

Host tests execute the actual fault classifier and atomic publisher, including
dual writable/executable aliases, concurrent publication, and exhausted banks.
QEMU executes the generated ARM code over 507,904 register/alignment combinations,
222,208 split fallbacks, six protection-fault recoveries, and 40,000 concurrent
exchanges while another thread modifies an adjacent byte. Existing host suites
and FEX dispatch contracts also pass. These are correctness checks, not iPhone
performance measurements.

Device acceptance requires: the 32-bit workload boots again; the managed
workload logs ATOMIC_FASTPATH and substantially fewer new faults; its audio
classes instantiate and OSS submits audio; identical 2D scenes are timed with
verbose off. Remaining split atomics, CPU translation, synchronization, shader
work, and guest frame pacing can still limit FPS. No 30/60 FPS guarantee follows
from CI or from the QEMU checks.

## Primary references

- FEX signal/backpatch design:
  https://wiki.fex-emu.com/index.php/Development:Debugging_FEX_with_Signals
- FEX unaligned TSO backpatching:
  https://fex-emu.com/FEX-2405/
- ARM compare-and-swap pair acquire/release semantics:
  https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab
- Darwin data-abort syndrome handling:
  https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/sleh.c
- Wine 11 DirectSound COM classes:
  https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/dsound/dsound_classes.idl
- Wine 11 XAudio2 versioned COM identifiers:
  https://github.com/wine-mirror/wine/blob/wine-11.0/include/xaudio2.idl
