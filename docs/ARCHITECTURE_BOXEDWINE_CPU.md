# BoxedWine CPU architecture and Phase 0 plan

Status: Phase 0 investigation only. This document changes no runtime behaviour.

Audit baseline:

- `ios`: `d6a5f706ecb151fea2be8434818e411a306e156c`
- `fex64`: `1a8f1df472530ba7e856a61898e92ab1c49781df`
- The CPU, decoder, soft-MMU, kernel, loader, and launch-policy files cited
  below are identical at those two revisions. The `fex64` branch remains a
  reference for executable memory and FEX integration; it is not the product
  direction.

## Executive conclusion

The premise that BoxedWine on iOS is using only the interpreter is false.
`BOXEDWINE_MAC_JIT` expands to the ARMv8 JIT configuration on an ARM64 Apple
target, and `NormalCPU` is the hybrid interpreter/JIT dispatcher. Hot decoded
blocks are handed to `JitArmV8CodeGen` after 50 executions. Generated ARM64 is
allocated from the StikDebug-prepared dual-mapped executable arena already used
by both app targets.

The important performance problem is real, but it is more specific: a
browser-engine compatibility profile deliberately forces anonymous executable
guest memory through the interpreter because device measurements found a
forward-progress failure when that code was ARM64-JIT compiled. The current
source does not establish the defect inside the translator. It establishes a
workaround and the observations that motivated it.

The best next investment is therefore:

1. measure JIT coverage and invalidation on device;
2. capture the first divergent generated-code block and state transition;
3. repair that defect in the existing ARMv8 backend; and
4. remove or narrow the broad anonymous-code interpreter policy after the fix
   is proven on device.

FEX behind BoxedWine's `CPU` factory is not a cheap CPU swap. FEX's direct
guest-memory model conflicts with BoxedWine's paged soft MMU, and the apparent
`CPU::run()` seam does not cover translator lifecycle, memory, signals, or code
invalidation. It should be deferred. A 64-bit guest is a separate architecture
programme and is explicitly out of scope for Phase 1.

## Verification of the four starting claims

| Claim | Verdict | Evidence |
|---|---|---|
| iOS runs only the interpreter | **Refuted** | `CMakeLists.txt:244-251` defines `BOXEDWINE_MAC_JIT`; `include/boxedwine.h:64-72` derives `BOXEDWINE_JIT` and `BOXEDWINE_JIT_ARMV8` on ARM64; `source/emulation/cpu/normal/normalCPU.cpp:393-400,595-665` selects and dispatches the JIT; `source/emulation/cpu/armv8/jitArmV8CodeGen.cpp:7016-7023` constructs it. |
| The CPU backend seam is clean | **Refuted as stated** | `CPU::run()` is a useful execution seam (`source/emulation/cpu/common/cpu.h:201-203,359-374`), but the actual translator seam is the large `JitCodeGen` contract (`source/emulation/cpu/jit/jitCodeGen.h:24-88,98-145,176-208`) plus decoded-op and lifecycle integration. |
| The memory model is the main FEX obstacle | **Confirmed** | The whole guest address space and API are 32-bit and page mediated (`include/kmemory.h:28-32,65-70,84-178`; `source/emulation/softmmu/kmemory_soft.h:37-85`). `mapNativeMemory` still assigns an unrelated 32-bit guest address and installs pages in that MMU (`source/kernel/kmemory.cpp:1896-1922`). |
| 64-bit guests are separate work | **Confirmed** | CPU state is IA-32-shaped (`source/emulation/cpu/common/cpu.h:208-258`), the loader accepts ELFCLASS32 only (`source/kernel/loader/kelf.h:24-78`; `source/kernel/loader/loader.cpp:40-50,233-249`), and syscall dispatch uses the i386 register ABI and numbering (`source/kernel/syscall.cpp:36-41,2294-2307,2740-2769`). |

## What actually runs today

### Build selection

The iOS target gives `boxedwine_core` the `BOXEDWINE_MAC_JIT` definition in
`CMakeLists.txt:242-252`. On an ARM64 target, `include/boxedwine.h:64-72` turns
that into all of the following:

- `BOXEDWINE_HOST_EXCEPTIONS`
- `BOXEDWINE_MEM_CACHE`
- `BOXEDWINE_JIT`
- `BOXEDWINE_JIT_ARMV8`
- `BOXEDWINE_MULTI_THREADED`

`cmake/BoxedwineSources.cmake:32-42` includes every non-test C++ file under
`source/` except the WASM backend, so the ARMv8 JIT translation unit is built.
There are two similarly named ARMv8 implementations in the tree:

- `source/emulation/cpu/armv8/armv8CPU.cpp:19-24` is an older dynamic backend
  gated by the undefined `BOXEDWINE_DYNAMIC_ARMV8`, so its body is absent.
- `source/emulation/cpu/armv8/jitArmV8CodeGen.cpp:19-23` is gated by the defined
  `BOXEDWINE_JIT_ARMV8`, so this is the active backend.

That distinction explains how a source census can incorrectly conclude that
the ARMv8 path compiles to nothing.

### Factory and dispatch

`CPU::allocCPU` does unconditionally return `NormalCPU`
(`source/emulation/cpu/common/cpu.cpp:15-17`), and production threads obtain a
CPU there (`source/kernel/kthread.cpp:141-142`). This does not imply
interpreter-only execution.

`NormalCPU` is the shared decoded interpreter and JIT dispatcher:

- Its constructor selects `firstDynamicOp` whenever `BOXEDWINE_JIT` is defined
  (`source/emulation/cpu/normal/normalCPU.cpp:393-400`).
- Native builds use a hotness threshold of 50 (`source/emulation/cpu/common/cpu.h:190-194`).
- `firstDynamicOp` starts compilation at that threshold
  (`source/emulation/cpu/jit/jitCodeGen.cpp:883-892`).
- `JitCodeGen::doJIT` creates the process-level register-sync, block-exit,
  single-op, entry, signal, and flag helpers before compiling and committing a
  block (`source/emulation/cpu/jit/jitCodeGen.cpp:799-855`).
- `NormalCPU::run` invokes the interpreted warm-up path through the threshold,
  then invokes the installed block entry (`source/emulation/cpu/normal/normalCPU.cpp:653-664`).
- The ARM64 implementation is selected through `startNewJIT`
  (`source/emulation/cpu/armv8/jitArmV8CodeGen.cpp:7016-7023`). Its entry thunk
  loads the installed block, the MMU/cache base, and CPU state before entering
  translated code (`source/emulation/cpu/armv8/jitArmV8CodeGen.cpp:6943-6963`).

### Executable memory

The hard iOS-specific part is already wired to BoxedWine, not merely available
on the reference app:

- JIT blocks allocate through `KMemory::allocCodeMemory` and are copied through
  `Platform::writeCodeToMemory`
  (`source/emulation/cpu/jit/jitCodeGen.cpp:1032-1044`).
- `BNativeHeap` obtains executable 64 KiB blocks through
  `Platform::alloc64kBlock` (`source/util/bnativeheap.cpp:84-120`).
- On iOS, that path calls `BVNExecMemAlloc`
  (`platform/linux/platform.cpp:350-388`).
- Writes are redirected to the RW alias while calls retain the RX address
  (`platform/linux/platform.cpp:50-75`; `ios/runtime/include/BVNExecMemory.h:120-146`).

Route (a), if defined as "wire up the existing ARMv8 JIT and its executable
arena," has already been completed. Phase 1 must verify and improve its coverage
rather than add a second selection at `CPU::allocCPU`.

## The real CPU/backend contract

There is a clean object-level boundary for the execution loop, but not a clean
plug-in boundary for an unrelated translator.

### What `CPU` owns

`CPU` owns the guest integer registers, segment state, EIP, flags and lazy-flag
operands, x87/SSE state, exception state, signal notification, decoded-op cache
pointer, and thread/memory associations
(`source/emulation/cpu/common/cpu.h:201-288,323-396`). It also implements the
decoder callback used to fetch bytes and locate decoded operations
(`source/emulation/cpu/common/cpu.h:369-375`). `NormalCPU` adds the run loop,
decoded-block lookup, executable-address validation, and normal instruction
handlers (`source/emulation/cpu/normal/normalCPU.h:24-38`).

A new `CPU` subclass would minimally have to preserve:

- register, lazy-flag, x87, SSE, segment, and privilege semantics;
- synchronous x86 exceptions and host-fault recovery;
- Linux signal delivery and safe exits from translated loops;
- the syscall transition into BoxedWine's kernel;
- clone, fork, exec, and per-thread state;
- self-modifying-code detection and all decoded/translated-code invalidation;
- debug traps, instruction counting, scheduling, and termination; and
- the existing interpreter fallback so unsupported or selected blocks remain
  executable.

### What the current native backend implements outside `CPU::run`

The active backend is a `JitCodeGen` implementation, not a `CPU` subclass.
`JitCodeGen` requires host instruction emission for CPU/MMU accesses, memory
reads and writes, flags, branches, calls, block entry/exit, state sync, and code
commit (`source/emulation/cpu/jit/jitCodeGen.h:24-145,176-208`). Compiled blocks
are installed in `DecodedOp::pfnJitCode` and share metadata with the interpreter.

Code-page writes retire decoded and compiled blocks
(`source/kernel/kmemory.cpp:1601-1694,1718-1725`). The selected JIT can also
register address-space, invalidation, and thread-start callbacks
(`source/emulation/cpu/jit/jitCodeLifecycle.h:42-78`). Signals set an atomic
pending flag (`source/kernel/kthread.cpp:190-197`), and generated branches poll
it and exit back to the run loop (`source/emulation/cpu/jit/jitCodeGen.cpp:943-958`).

These are required semantics, not optional optimisations. A FEX-backed `run()`
would still need adapters for all of them.

## Memory seam and why `mapNativeMemory` is not a FEX foundation

### BoxedWine model

BoxedWine defines 1,048,576 4 KiB pages, exactly covering a 4 GiB guest address
space (`include/kmemory.h:28-32`). Guest addresses are `U32` throughout mmap,
mprotect, copy, scalar access, page iteration, decoded-code lookup, and code
invalidation (`include/kmemory.h:84-178`). `KMemoryData` stores a per-page MMU,
decoded-op cache, and native code heap; native JIT builds also keep per-page read
and write translation caches (`source/emulation/softmmu/kmemory_soft.h:37-85`).

The ARMv8 JIT is already optimised for this model. `onPageChanged` records a
host-base-minus-guest-page-base value per page
(`source/emulation/softmmu/kmemory_soft.cpp:85-96`), and the generated entry
loads that cache adjacent to the MMU (`source/emulation/cpu/armv8/jitArmV8CodeGen.cpp:6949-6954`).
This permits fast host loads after a page lookup while preserving BoxedWine page
permissions, copy-on-write, mapped files, and invalidation.

### `mapNativeMemory`

`KMemory::mapNativeMemory` does not make the host pointer become the guest
address. It reserves a new range in the 32-bit guest space, wraps each host page
as a native RAM page, installs those pages in the MMU, refreshes the translation
cache, and returns the allocated guest address
(`source/kernel/kmemory.cpp:1896-1922`). Its real call sites expose individual
host graphics buffers to 32-bit guest code, for example
`source/vulkan/vulkancommon.cpp:332-352` and
`source/opengl/glMarshal.cpp:431-439`.

That is useful zero-copy marshalling, but it is not a flat guest address space.
It cannot provide the direct addressing contract demonstrated by the FEX
reference bridge.

### FEX model demonstrated in this tree

The reference bridge maps guest code and stack with host `mmap`, passes those
actual host pointer values as the guest entry and stack, and lets FEX read guest
bytes directly (`ios/fex/app/BVNFexBridge.mm:567-590`). Only FEX's generated
ARM64 output lives in the dual-mapped executable arena; the bridge gives FEX the
RW-minus-RX alias offset (`ios/fex/app/BVNFexBridge.mm:404-429`).

Passing every translated load and store back through `KMemory` would preserve
BoxedWine semantics but erase the direct-memory advantage that motivates FEX.
Avoiding that cost requires a new direct/flat memory implementation plus a way
for BoxedWine's kernel, file mappings, permissions, copy-on-write, futexes, and
code invalidation to share ownership of it. `mapNativeMemory` is therefore a
dead end for a whole-process FEX backend, though it remains appropriate for
bounded host buffers.

## Anonymous and runtime-generated code: actual mechanism

Two separate mechanisms are easy to conflate.

### General self-modifying-code policy

Writes to decoded code retire the affected decoded/JIT blocks and increment a
per-byte saturating write counter
(`source/emulation/softmmu/codePageData.cpp:80-120`). Only a byte that reaches
`MAX_DYNAMIC_COUNT` (255) is considered dynamic
(`source/emulation/softmmu/codePageData.h:22`; `source/emulation/softmmu/codePageData.cpp:466-487`).
A writable-to-executable protection transition records the page as having been
written (`source/emulation/softmmu/kmemory_soft.cpp:219-232`). When a block is
decoded, any instruction covering a saturated byte is marked `OP_FLAG_NO_JIT`
(`source/emulation/cpu/normal/normalCPU.cpp:566-579`).

This is a churn heuristic for repeatedly modified code. It does not prohibit
new anonymous code from entering the ARM64 JIT.

### iOS browser compatibility policy

The iOS launch profile explicitly enables `interpretAnonymousExecutable`
(`ios/runtime/src/BVNLaunchArguments.cpp:238-252`), which becomes the
`-interpreterAnonymousExecutable` core switch
(`ios/runtime/src/BVNLaunchArguments.cpp:495-501`). At decode time, BoxedWine:

1. resolves mapped ELF or PE ownership;
2. treats unresolved executable memory as anonymous;
3. exempts the known Wine relay and BoxedWine callback/stub ranges
   (`source/emulation/cpu/common/anonymousCodePolicy.h:11-22`);
4. marks every instruction in a selected block `OP_FLAG_NO_JIT`; and
5. sets its run count above the compilation threshold
   (`source/emulation/cpu/normal/normalCPU.cpp:456-473,545-579`).

`JitCodeGen::compileOps` refuses a block containing `OP_FLAG_NO_JIT`
(`source/emulation/cpu/jit/jitCodeGen.cpp:742-750`). This explicit launch policy,
not an inability to recognise runtime-generated x86, is what produced the large
interpreted-block count.

The evidence recorded in `PROGRESS.md:303-333` is still important: one device
snapshot put a browser main thread almost entirely in an anonymous generated
block; broadly interpreting those blocks restored forward progress. Later
attempts to keep healthy anonymous pages compiled and demote only a repeated EIP
did not trigger because the failure did not remain at one dispatcher entry
(`ios/runtime/src/BVNLaunchArguments.cpp:242-248`).

That proves a correctness or progress defect on the compiled path and proves
the interpreter is a viable reference. It does **not** prove that all
runtime-generated x86 is intrinsically unsafe to translate. The policy selects
code by mapping provenance, not by an unsupported instruction or a demonstrated
memory hazard. The likely fault domains are ARMv8 instruction lowering, block
chaining/exit state, or code-write invalidation. Phase 0 cannot choose among
them without a captured first divergence.

This defect is therefore a credible, high-value target inside the existing
backend. Removing the workaround before that evidence exists would be guessing.

## Costed route comparison

These estimates are engineering effort, not calendar promises. Device feedback
adds at least one manual CI/build/sideload/log cycle to each evidence step.

| Route | Work actually required | Estimate | Risk | Expected value |
|---|---|---:|---|---|
| (a) Wire existing ARMv8 JIT | Core wiring and executable memory are already complete. Add generic coverage/invalidation/arena counters and prove on-device entry for representative mapped and anonymous blocks. | **2-4 engineer-days**, 1-2 device cycles | Low | Essential measurement foundation; no large speed-up by itself because the JIT is already active. |
| (b) Fix anonymous generated-code fallback | Capture guest bytes, decoded ops, entry/exit CPU state, compiled host span, and invalidation history at the first no-progress boundary; reduce to a replay/differential case; fix the first interpreter/JIT divergence; narrow or remove the broad policy; add regression coverage. | **2-5 engineer-weeks**, roughly 3-6 device cycles | Medium-high until the first divergence is captured | Best near-term chance of a large gain for CPU-heavy 32-bit browser-engine workloads while preserving all BoxedWine services and portability. |
| (c) FEX behind `CPU::allocCPU` | Add a BoxedWine-aware FEX runtime, reconcile direct guest memory with `KMemory`, bridge complete IA-32 state/syscalls/signals/threads/fork/exec, implement permissions and invalidation, retain interpreter fallback, and build an optional portable selection layer. | **3-6 engineer-months** for a credible 32-bit prototype; longer for production hardening | Very high | Duplicates much of the current translator integration while fighting the memory model. Poor value for the present 32-bit product. |

Route (c) should not begin during the 32-bit performance programme. It becomes
worth re-evaluating only if a future 64-bit memory/kernel design deliberately
provides a direct-address backend that FEX can use without routing every access
through the soft MMU.

## Recommended Phase 1

Phase 1 should be **(a) measurement followed immediately by (b) defect
isolation**, treated as one bounded programme.

### Step 1: prove the active paths

Add low-overhead, title-independent session totals for:

- decoded blocks and interpreted instructions;
- JIT compile attempts, commits, entries, and exits;
- `NO_JIT` reasons: explicit range, module, anonymous policy, and write churn;
- code invalidations split by guest write, protection transition, unmap/exec,
  and cache reset; and
- executable-arena committed, peak-live, retired, and available bytes.

Acceptance is a device log proving which path executed. A green CI build proves
only compilation and packaging.

### Step 2: capture the first divergence

Make a diagnostic mode capture, at a bounded no-progress trigger:

- guest EIP range and bytes;
- decoded operations and block boundaries;
- CPU integer, segment, flags/lazy-flags, x87, and SSE state at entry and exit;
- generated ARM64 address/length and symbol-resolvable app addresses;
- direct successors and block-chain targets; and
- every write/protection/invalidation event touching the guest pages.

The capture must be generic and opt-in. It must not contain title-specific names
or policy.

### Step 3: reproduce and fix

Turn the captured block into a deterministic interpreter-versus-ARMv8 replay.
Compare architectural state at instruction or short-block boundaries and patch
the first divergence, rather than the final stall. If the state agrees, move
the comparison boundary across block chaining and invalidation until the first
incorrect transition is isolated.

The fix belongs in the shared decoder/JIT/invalidation layer where the evidence
points. iOS-specific code should remain limited to executable-memory allocation
and device diagnostics.

### Step 4: retire the workaround by evidence

After the replay passes, stop enabling the broad anonymous interpreter policy
for the affected engine class. Keep the interpreter, explicit module/range
diagnostics, and high-churn fallback available. Device acceptance requires a log
showing anonymous blocks compile and enter, forward progress continues, no
compiled-code fault occurs, and executable-arena usage stays within the prepared
budget.

### Portability boundary

The existing design already preserves the right boundary:

- native ARM64 uses `JitArmV8CodeGen`;
- other native targets select their existing code generator at compile time;
- WebAssembly keeps its separate backend; and
- `NormalCPU` remains the shared interpreter and fallback.

Phase 1 must not make BoxedWine depend on UIKit, Metal, the iOS arena, or FEX.
Only the `Platform` executable-memory hook and optional diagnostics may remain
iOS-specific.

## 64-bit guest scope: do not start in Phase 1

Supporting a 64-bit guest is not a compiler flag or just another translator.
It requires coordinated replacements or parallel implementations for:

1. **CPU architecture:** 16 64-bit general registers, RIP, long-mode decoding,
   REX prefixes, operand/address-size rules, FS/GS bases, 16 XMM registers, and
   x86-64 exception/interrupt state. The current core exposes nine `Reg`
   entries, eight XMM registers, segmented IA-32 state, and a 32-bit EIP
   (`source/emulation/cpu/common/cpu.h:208-258`).
2. **Memory:** a 64-bit guest-address type, sparse multi-level address-space
   metadata, mmap/mprotect/unmap and mapping records, page-cache and code-cache
   keys, copy helpers, futexes, signals, and every subsystem API currently
   taking `U32`. The fixed array is exactly 4 GiB
   (`include/kmemory.h:28-32,65-178`), and mmap explicitly rejects 32-bit
   overflow (`source/kernel/kmemory.cpp:591-613`).
3. **Loader and process startup:** ELF64 headers/program headers, x86-64 ELF
   validation, auxiliary vector, stack layout, relocations, TLS, vDSO/callback
   conventions, and mixed 32/64 process metadata. The current loader defines
   only `k_Elf32_*` and accepts only ELF class 1
   (`source/kernel/loader/kelf.h:24-78`; `source/kernel/loader/loader.cpp:40-50`).
4. **Kernel ABI:** the x86-64 syscall numbers and register calling convention,
   64-bit pointers and structure layouts, signal frames, clone/TLS rules,
   ioctl marshalling, and compat separation from the existing i386 ABI. The
   current dispatcher takes arguments from EBX/ECX/EDX/ESI/EDI/EBP and indexes
   the i386 table with EAX (`source/kernel/syscall.cpp:36-41,2294-2307,2740-2769`).
5. **Userland:** a 64-bit Linux filesystem and 64-bit Wine plus corresponding
   graphics, audio, input, and system libraries. The current build tooling and
   test assets explicitly target i386/PE32, for example
   `tools/buildWine/wine_builds.json:44-52` and
   `tools/wineTests/README.md:1-4,184-192`.
6. **Host bridges:** audit every OpenGL, Vulkan, X11, audio, socket, and file
   marshaller that encodes guest pointers or structure layouts as `U32`.
7. **Compatibility:** preserve the 32-bit interpreter, ARMv8 JIT, and WASM
   paths rather than widening shared types until they silently break 32-bit
   layout and performance.

A production-quality 64-bit programme is plausibly **6-12 engineer-months or
more**, depending on how much kernel and marshalling code can be cleanly shared.
That estimate is intentionally separate from route (c): choosing FEX supplies
instruction translation, not the missing 64-bit BoxedWine kernel and ABI.

## Decision

- Keep BoxedWine as the product and `fex64` as a non-product reference.
- Treat the existing ARMv8 JIT and iOS executable arena as working foundations.
- Start Phase 1 with generic JIT coverage/invalidation measurements.
- Use those measurements to isolate and fix the compiled anonymous-code
  forward-progress defect.
- Do not integrate FEX behind `CPU::allocCPU` for the current 32-bit product.
- Do not start 64-bit guest work in Phase 1.
