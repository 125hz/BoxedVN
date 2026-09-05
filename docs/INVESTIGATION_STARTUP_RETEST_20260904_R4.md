# Retest at revision 4bff8e3f

All six captures identify `4bff8e3f+dirty`. The user confirmed replacing the
external PE32 runtime ZIP alongside the IPA. Commit and investigation text use
generic application descriptions because the changes belong to the runtime.

## Device evidence

* **20:35:32, original 64-bit executable:** public profile directories are
  created successfully. The observed dialog now says the Steam client must be
  running, rather than the previous internal error. This is an application
  dependency check; this change does not bypass it.
* **20:41:02:** startup only, with JIT not enabled. No guest was launched.
* **20:41:07, replacement 64-bit executable:** an actual access violation at
  `0x7ffffaaa5239`, instruction bytes `c4e26d8c01` (`vpmaskmovd ymm0,ymm2,[rcx]`).
  RCX and the host fault address are both `0x7ffffab27198`. That is a canonical
  Wine top-arena address; its host image should be `0x7ffab27198`.
* **20:42:15, first visual-novel capture:** stops during startup without the
  later capture's fault. Do not treat this shorter log as successful startup.
* **20:44:42, 32-bit cube:** the same worker throws `std::bad_alloc`, but there
  is no allocation-size report. The built D3D9 DLL contains aligned operator
  new, which the previous wrappers did not cover.
* **20:46:42, longer visual-novel capture:** guest RIP `0x7aae3a19` resolves to
  `ucrtbase!frexp+0x89`, at the recursive subnormal-scaling path. Its stack store
  faults on guest `0x2fff4`, an inaccessible reservation. The runtime then
  consumes approximately one CPU core inside its own host-fault handler.
  The precise floating-point input and origin of the recursion remain unknown.

## Changes

FEX's non-SVE masked load/store lowering issues scalar lane accesses directly.
It bypasses the ordinary memory-operand helper that performs BoxedWine address
translation. Apply the existing alias helper after assembling the canonical
address in both masked paths, preserving guest registers, flags and mask-based
access suppression. The SVE path already uses the common translated helper.
The maintenance patch is in this repository; the vendored checkout is untouched.

Extend the alias-enabled conformance fixture with full and partial masked
loads/stores, both halves of YMM registers, unaligned operands, low and top
addresses, and a zero mask on an inaccessible address. Force the non-SVE path
to match iOS. Extend the IA-32 fixture with x87 zero/subnormal comparison and
subnormal scaling across a block boundary. Include the existing x87 context
patch in the test harness's patch set as well as the iOS build.

Fault-report reads now use checked `vm_read_overwrite` into stack buffers. The
old range-membership check did not guarantee readability and could itself
fault on an exhausted guest stack. Report read success separately from zero
data and retain the original guest fault. Add FP control/mask state and publish
the handler's disassembly with the host symbol artifact for future spin reports.

Add aligned `new`/`new[]` wrappers and a report for allocator rejections before
operator new. Preserve the original allocation implementations and exceptions.
Native Windows i386 tests pass ordinary, zero-sized, failed and aligned
allocations, new handlers and propagation of unrelated exceptions. The 32
relay tests pass; ARM64 translation and iOS compilation run in GitHub Actions.

## Primary references and acceptance

[Intel's instruction reference](https://cdrdv2-public.intel.com/782156/325383-sdm-vol-2abcd.pdf)
defines mask-selected accesses and fault suppression for inactive lanes.
[Apple's API](https://developer.apple.com/documentation/kernel/1585371-vm_read_overwrite)
and [XNU implementation](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_user.c)
provide checked copying into an existing buffer.

The masked-address fix targets the replacement 64-bit executable's observed
fault. The cube allocation failure and visual novel's floating-point behavior
remain device acceptance work. Publish one increased-memory `BoxedVN.ipa` plus
the matching external `wine64-pe32.zip`; do not download or hash-check the IPA
on the user's behalf.
