# FEX32 on iOS: address-space contract and delivery stages

Status: design boundary verified; implementation requires a translator build
that exposes a guest-address bias. The standard iOS executable layout remains
mandatory because reducing `__PAGEZERO` prevented the app from launching on
device.

## Product policy

FEX is intended to become the default CPU backend for native iOS guests once
both 32-bit and 64-bit acceptance suites pass. The interpreter and the existing
ARMv8 backend remain recovery paths, and non-iOS and WebAssembly builds retain
their current portable selections. "Default" therefore describes the shipped
iOS runtime policy, not removal of BoxedWine's portable backends.

## Verified blocker

The standard arm64 Mach-O reserves the complete low 4 GiB as `__PAGEZERO`.
XNU raises the process VM map's minimum address to the end of that segment;
fixed or overwrite allocation cannot lower it again. The launch-safe app can
therefore neither reserve nor populate an IA-32 identity-mapped address space.

The pinned FEXCore assumes identity mapping in several independent places:

1. `FEXCore/Source/Interface/Core/Core.cpp` passes `GuestRIP` to the decoder as
   `reinterpret_cast<const uint8_t*>(GuestRIP)`.
2. The ARM64 JIT's memory operations load and store through the guest effective
   address without an integration-supplied base.
3. The 32-bit entry path rejects entrypoints with nonzero upper 32 bits.
4. The x32 syscall wrappers reinterpret 32-bit guest pointer arguments as host
   pointers, including nested structures, clone/TLS state, and `argv`/`envp`.
5. Fault-safe memory access and signal-frame delivery dereference the supplied
   guest address directly.

Setting data-segment bases cannot repair this contract. It would not relocate
instruction fetch, and flat IA-32 applications expect ordinary DS/ES/SS bases
to remain zero. Routing every access through `KMemory` callbacks would preserve
semantics but discard the direct-memory performance that motivates FEX.

The vendored translator also explicitly forbids AI-generated contributions.
BoxedVN integration work must not modify `third_party/fex64/fex`; a guest-base
facility must arrive through a separately maintained, permitted, reviewed
translator source.

## Required translator contract

A usable FEX32 build for standard iOS must expose one process-wide 4 GiB-aligned
host base `B` and preserve IA-32 architectural addresses as 32-bit values:

- instruction bytes for guest EIP `x` are read at `B + x`;
- every generated guest load/store uses `B + zero_extend(effective_address)`;
- lookup-cache, invalidation, fault recovery and signal reports remain keyed by
  architectural address `x`, not host address `B + x`;
- indirect branches and return addresses keep 32-bit wraparound semantics;
- syscall arguments remain guest addresses and are translated exactly once at
  the BoxedWine memory boundary; and
- direct host pointers are never written into 32-bit guest registers.

The base must be an explicit context property. Inferring it from a code mapping,
overloading segment state or adding it only in the decoder would leave other
memory and control-flow paths inconsistent.

`GuestAddressContract32` and `Fex32BackendDescriptor` encode this boundary in
host-only tests. A paged descriptor cannot claim direct access, and a biased
direct descriptor must provide aligned code and data access across the complete
4 GiB window. A structurally valid descriptor remains explicitly unavailable
until an approved translator implements that contract.

`selectFex32Backend` is the admission boundary for that future translator. It
requires an explicit request and returns separate not-requested, invalid,
unavailable and selected states. The current descriptor therefore cannot be
promoted into a runnable CPU merely because a build flag or UI value exists;
selection becomes possible only after the complete address and ABI contract is
both valid and reported available.

## BoxedWine memory contract

After the translator exposes that base, BoxedWine needs a flat 32-bit memory
implementation whose page metadata remains authoritative while host storage is
reserved at `B + guestAddress`:

- `mmap`, `mprotect`, `munmap`, file mappings and anonymous mappings update both
  BoxedWine page metadata and the high host window;
- copy-on-write and shared mappings preserve current kernel behaviour;
- executable-page writes invalidate FEX blocks by architectural guest range;
- futexes and atomic operations use the same backing bytes as translated code;
- host bridges translate guest pointers through the memory object rather than
  assuming identity; and
- fork/exec owns or serialises the single flat window until per-process biased
  windows are proven.

This is a new `KMemory` mode, not `mapNativeMemory()`: that helper allocates an
unrelated guest address for a bounded host buffer and cannot represent a whole
address space.

`Fex32GuestWindow` now defines the first host-tested ownership boundary for
that mode. It requires a complete, high, 4 GiB-aligned reservation; translates
only checked 32-bit ranges; and mirrors commitment and permissions for every
4 KiB guest page under a lock. Platform VM operations are injected, so the
required reserve, commit, protect, decommit and release transitions are
deterministic in the host suite without consuming a real 4 GiB test mapping.
Operations are grouped at the platform page size (16 KiB on current iOS
devices) without treating adjacent guest pages as logically mapped. The MMU
must remain authoritative when the native VM and `KMemory` adapters are added.
Until then this is an always-tested support contract, not an enabled emulator
memory mode, and it cannot make the current identity-only translator usable by
itself.

## Delivery stages

1. **Biased translator probe.** With an approved translator build, reserve a
   high 4 GiB window, run a freestanding IA-32 probe, and verify instruction
   fetch, stack, wrapped addressing, scalar/SSE memory, syscall exit and code
   invalidation. No Wine or graphics are involved.
2. **Flat BoxedWine memory.** Add the biased `KMemory` mode and differential
   tests against the soft MMU for mapping, protection, copy and invalidation.
3. **CPU adapter.** Bridge BoxedWine IA-32 registers, flags, segments, signals,
   scheduling and syscall transitions into FEX while retaining interpreter
   escape paths for unsupported or diagnostic execution.
4. **Wine32 startup.** Boot the existing 32-bit Wine root filesystem, then
   validate process creation, TLS, callbacks, audio/input and the current D3D9
   graphics path.
5. **Default selection.** Make FEX the native iOS default only after repeated
   device runs pass startup, process, graphics and recovery acceptance. Keep a
   user-independent fallback so a translator regression cannot make the app
   unlaunchable.

Until stage 1 exists, claiming FEX32 support would be cosmetic. The shipping
32-bit path remains BoxedWine's ARMv8 JIT/interpreter while the 64-bit FEX path
continues to provide the integration and device-debugging foundation.

The x86-64 bring-up has a separate pre-device conformance fixture at
`scripts/guest-probes/fex64-loader-stall.asm`. It runs the sampled libc SIMD
memory path through FEX's ARM64 VIXL simulator in single-block and multiblock
modes. Passing that fixture proves translator instruction/control-flow
semantics only; it does not prove iOS executable memory, host callbacks, Wine,
DXMT or Metal presentation.
