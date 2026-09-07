# Wine 11 scheduling, helper faults and launcher handoff

## Device evidence

The September 7 10:45, 10:47, 17:15, 17:16, 17:20 and 17:21 captures
identify `bc0f99a8+dirty`, app build 137, Wine 11 and increased-memory signing.

- The 64-bit managed loading stall has an earlier causal boundary than its
  later read loop: two threads fault at host PC `0x11b6d546c`, executing
  `0xf8ea806a` (SWPAL) at unaligned guest addresses. This is executable code
  owned by FEX, outside its translated block ranges. Both faults are contained
  rather than recovered. The subsequent read(fd=91) loop repeatedly returns
  EINTR, with roughly ten million calls in five seconds. It is downstream
  evidence, not a reason to turn interrupted reads into successful reads.
- The D3D9 gameplay capture spends millions of calls in `getrusage` and
  `clock_gettime`. The x64 path reports zero rusage fields and its yield fast
  path bypasses the bounded scheduling detector used by the older kernel.
  Wine 11's `NtYieldExecution` compares per-thread switch counters before and
  after yielding. A zero-filled response never reports a performed yield.
  A busy-host sample also lands in `ReconstructXMMRegisters`, called by the
  full syscall synchronization path while this polling thread occupies a core.
- A 32-bit launcher starts child pid 50, then exits cleanly. The runtime
  unconditionally requests session shutdown and kills the remaining processes
  while the child is still loading its WoW64 image.
- Floating-point stereo audio at 44.1 kHz receives about 189–193 KB/sec in
  several intervals, versus 352,800 bytes/sec required for continuous output.
  Converter-retained input hides empty playback queues from the underrun
  counter. The user clarified that the later failure is a freeze with the app
  still open; the capture has no terminal guest fault or process-group exit.
- The desktop capture still has blank file panes and shell controls, with
  repeated Wine cross-process paint-message warnings. These warnings do not
  establish why the destination window is considered cross-process. The
  filesystem enumeration worked in the preceding capture. This remains open.

## Shared runtime changes

1. Route unaligned SIGBUS faults in owned FEX helpers to FEX's existing
   non-JIT atomic emulation. Keep `IsJIT=false` for helpers: they do not have
   the inline translated-block header used for JIT backpatching. Unrelated
   native PCs, other signal causes and unsupported instructions still chain.
2. Use the existing bounded yield detector for both x64 syscall dispatch and
   its FEX fast path. It activates on sustained polling and sleeps 100 us at
   most once per 4 ms, not once per syscall. Record actual elapsed sleep time.
   Return native per-thread CPU time and observed scheduling counts through
   the Linux x64 rusage layout. Process/children accounting remains unchanged.
3. After the root launcher exits, check for surviving user processes outside
   retirement locks. Preserve the running session while a child remains;
   helper processes alone do not retain it. Deduplicate exit observers and
   bind delayed polls and shutdown retries to the launch generation.
4. Count playback starvation independently from retained resampler input.
   OSS position/space accounting continues to include that input.
5. Keep the FEX frame authoritative for clock and thread-usage queries in the
   Unix guest lane. Call the same kernel implementations, accepting only
   committed writable result pages. Invalid buffers, other queries, PE callers,
   pending signals and terminating threads retain the complete syscall path.
   This removes the measured SIMD/x87 reconstruction cost from these polls.

No game files, title-specific rules, Wine runtime layers or graphics routing
are changed. Direct3D 9 continues through DXVK/Vulkan/MoltenVK; DXMT serves
Direct3D 10/11. Fast x87 remains disabled. Reuse the current Wine ZIPs and
container; this iteration updates only the IPA.

## Validation and acceptance

Compiled production-function fixtures exercise Linux rusage on the host,
Apple rusage layout with Mach stubs, bounded yield timing, helper-fault routing,
launcher/child/helper lifetimes, duplicate and stale asynchronous callbacks,
and playable versus retained audio queues. Existing dispatch, signal, affinity,
surface and Vulkan tests are also required before publication.

These checks do not execute ARM64 atomics on the iPhone or establish a frame
rate. The managed loading path and launcher handoff have targeted fixes;
audio, low frame rate, the later 32-bit freeze and desktop painting require
device acceptance and further evidence. No minimum FPS is claimed.

References:
- [Linux getrusage ABI and thread accounting](https://www.man7.org/linux/man-pages/man2/getrusage.2.html)
- Wine 11 source: `dlls/ntdll/unix/sync.c`, `NtYieldExecution`.
- Pinned FEX source: `FEXCore/Source/Utils/ArchHelpers/Arm64.cpp`,
  `HandleUnalignedAccess`, including its non-JIT atomic path (read only).
