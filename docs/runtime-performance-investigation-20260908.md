# CPU dispatch, exact arithmetic and Metal submission investigation

## Decision and evidence boundary

Keep BoxedWine, Wine 11, FEX and the native Metal renderer for this iteration. The new logs identify large, specific CPU costs that can be addressed without replacing the operating-system runtime or losing 32-bit support. This does not establish a maximum achievable frame rate. No device FPS improvement is claimed from host benchmarks or IPA compilation.

The two device captures are `boxedvn-20260908-171641.log` (32-bit D3D9 workload) and `boxedvn-20260908-172119.log` (64-bit managed workload). Both embed revision `525985d7+dirty`, build 137. Both reach the native DXMT Metal path. The old Vulkan route is not the explanation for these particular captures.

| Observation | 32-bit workload | 64-bit workload |
|---|---:|---:|
| CompileBlock requests | 9.76 million | 9.04 million |
| Requests resolved to existing compiled blocks | 9.45 million, about 96% | 8.63 million, about 95% |
| Handled host faults | About 581 | 8.57 million |
| Dominant late CPU samples | Exact x87 helpers and C++ exit dispatch | Managed runtime and unaligned atomic handling |
| Native present call duration in sampled reports | About 3–6 microseconds | About 4–5 microseconds |
| Severe frame submission gaps | 5.28–7.46 seconds | CPU-limited submission also observed |

The lookup-hit count is not a count of new translations. Repeatedly returning through C++ to find existing code is avoidable work. Exact-symbol samples in the 32-bit capture include `softfloat_shiftRightJam128`, `softfloat_roundPackToExtF80`, `softfloat_subMagsExtF80`, and the extended-precision add/sub entry points. Two workers can consume close to a complete CPU core while the main thread waits.

The GPU is executing Metal work. It cannot execute x86 game logic, Wine, or x87 physics arithmetic. Microsecond CPU-side presentation calls and multi-second gaps before the next present support investigating CPU production of frames first. They **do not measure GPU utilization or GPU execution time**; GPU saturation is not ruled out for other scenes. Audio underruns similarly show missed delivery deadlines, rather than proving that the audio renderer is itself the primary bottleneck.

## Implemented changes

1. **Preserve exact x87 arithmetic but compile its helpers together.** A CMake overlay enables a unity build for the pinned SoftFloat library. This exposes helper bodies to the optimizer while retaining full extended-precision results, rounding modes, exception flags and the existing FEX calling convention. Three unused entry points remain separate because their dependencies are intentionally absent from FEX's trimmed archive. There is no fast-math flag, precision reduction or vendor-source modification. SoftFloat's own performance guidance identifies separate translation units as a barrier to cross-function optimization. [SoftFloat documentation, section 9.6](https://www.jhauser.us/arithmetic/SoftFloat-3/doc/SoftFloat.html)

2. **Replace the conservative exit callback's C++ round trip with a four-instruction ARM64 leaf.** The leaf writes the next guest RIP and enters the ordinary dispatcher. It preserves the no-direct-linking policy and retains cache lookup/invalidation behavior. Guest static registers and NZCV are not spilled and refilled for a callback that only redirects execution. A single branch is published through the existing writable/executable aliases after the immutable leaf has been flushed.

3. **Enable the existing second-level lookup cache.** The native FEX allocator uses demand-paged anonymous mappings. Its large virtual reservation is not equivalent to immediately committing the entire allocation on the iPhone. Full guest-address key checks remain in the existing dispatcher. This is a measured-policy experiment: FEX has also changed lookup-cache policies upstream, so the result must be judged against device CPU and resident-memory data rather than assuming a bigger cache is universally faster. [FEX 2511](https://fex-emu.com/FEX-2511/)

4. **Restore the zero-fill contract required when FEX discards anonymous cache pages on Darwin.** Linux-style `MADV_DONTNEED` is used by the library as a reset operation; Darwin's ordinary advice is insufficient for that assumption. The FEX-only host shim checks mappings and uses `MADV_ZERO` for anonymous writable non-executable data, with a zero-before-`MADV_FREE` fallback. File-backed and executable mappings retain ordinary platform behavior. This prevents reusing stale entries when enabling the larger lookup cache. [Apple mmap definitions](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/mman.h), [Apple madvise implementation](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_mman.c)

5. **Avoid full CPU-state reconstruction for Metal command recording.** Only native Unix-call ordinals 36–38, the blit/compute/render command-stream interpreters, qualify. The existing native bridge still validates marshalled pointers. Submission, waits, allocation, presentation, pending signals and all other calls retain the complete path. The WoW64 tag is supported; unknown high bits are rejected. This reduces fixed overhead per recorded batch without moving guest callbacks into native code.

6. **Allow the existing unaligned atomic leaf in owned generated helper code.** Previously eligibility was restricted to translated block ranges. The CASPAL replacement remains limited to an unaligned 64-bit exchange contained in one aligned 16-byte region. It does not weaken atomic ordering or replace split accesses with ordinary loads and stores.

## Validation and remaining costs

Nine million differential add/sub/mul cases compare production unity arithmetic with a separately compiled pinned reference, across precisions 32/64/80, all five supported rounding modes, normal and exceptional operands, output bits, raised flags and already-raised flags. The Windows-hosted WSL benchmark measured:

| Operation | Separate translation units | Unity build |
|---|---:|---:|
| add | 8.44 ns/call | 7.62 ns/call |
| sub | 9.50 ns/call | 6.73 ns/call |
| mul | 7.30 ns/call | 5.56 ns/call |

These are x86 host arithmetic-call measurements, not ARM64 results or frame-rate predictions. CI repeats the differential/benchmark test natively on its Mac. The emitted dispatch leaf is executed under ARM64 QEMU against register and NZCV snapshots. Additional checks cover dual-alias atomic publication, contention and protection fallback, cache discard zeroing/exclusion, malformed Metal ordinals, and the pinned dispatcher ABI. CI adds an L2-enabled translated high-address call/return fixture.

The native ARM64 run subsequently passed the same nine million cases. Its reference-to-unity timings were 13.48 to 11.68 ns for add, 13.93 to 12.68 ns for subtract, and 9.90 to 8.91 ns for multiply: about 9–13% lower call time. This measures the Mac runner and the production calling convention, not iPhone gameplay. The Darwin memory test also passed. The iPhone build uses `vm_region_64` with extended mapping information, while macOS uses `mach_vm_region_recurse`: the two SDKs expose different region-query interfaces. CI additionally compiles the memory check against the iPhone SDK.

The final packaging run (`34293679463`, code revision `1ca92190`) again passed all nine million correctness cases, but its timings were mixed: add 16.15 to 19.11 ns, subtract 24.44 to 18.47 ns, multiply 21.57 to 12.49 ns. The short sequential benchmark on a shared runner is variable enough that a uniform ARM64 speedup is **not established**. The earlier 9–13% result must not be treated as a guaranteed improvement. Device measurements remain necessary for the arithmetic change as well as the dispatcher/cache changes.

**The 64-bit split-atomic storm is not fully solved.** The captured instruction is repeatedly `SWPAL x10,x10,[x3]`; many addresses cross a 16-byte boundary. Those cases still require the existing FEX handler. The current fast leaf cannot atomically cover them with one aligned CASPAL. Eliminating signal delivery for them requires a separate validated slow-path entry or a correctly integrated runtime backpatcher, including protection-fault recovery and register reconstruction. It must not be replaced by a racy memcpy. [FEX signal handling](https://wiki.fex-emu.com/index.php/Development:Debugging_FEX_with_Signals)

**The D3D9 color fault remains unlocalized.** Format definitions, packed vertex-color conversion, render-target channel masks and basic native bridge layouts were checked; no evidence justifies globally swapping red and blue or disabling gamma. D3D9 explicitly applies sRGB write conversion to clear operations as well as draws. The cube now performs one-shot offscreen GPU readbacks for red/green/blue clears, red vertex color, red texture sampling and middle-gray sRGB output. These bounded `BOXEDWINE_D3D9_COLOR` lines distinguish rendering from presentation and introduce no work into ordinary games. The sRGB sample allows normal one-unit conversion rounding differences. [Microsoft D3D9 gamma contract](https://learn.microsoft.com/en-us/windows/win32/direct3d9/gamma)

## Follow-up: writable generated code and stale translations

Further offline disassembly of the supplied managed-runtime module found a stronger explanation for the repeated exchanges. Instruction-boundary matching places all 41 captured addresses from that module at one candidate relocation, including the hottest fault-associated sample at an exchange in the callsite patcher. This is an inference from matching the local module to sampled addresses, not a loader-recorded image base.

The frontend inherits FEX's default `mtrack` self-modifying-code policy, but does not implement `MarkGuestExecutableRange` write protection. Guest writes to writable executable memory can therefore change a JIT call target without a syscall or a cache invalidation. Reusing the old translated immediate can keep entering the runtime patcher. This is a shared emulation correctness problem as well as a performance concern; it does not require changing an application's instructions or files.

For the supported single-block configuration, the follow-up change emits FEX's existing code-byte validation for blocks touching writable executable guest pages. Read-only executable image pages retain the ordinary path. Both the saved reference bytes and the generated validation loads use BoxedWine's guest aliases. The frontend invalidation callback also takes FEX's required lock and invalidates shared translated blocks plus every thread cache in the same context, including callbacks from a still-running retired execution epoch. Otherwise an L1/L2 miss could recover the stale translation again from the shared cache.

The regression fixture patches an immediate and an indirect call target through both low and top guest aliases, without any syscall or explicit flush. Its purpose is to verify that execution observes the new code. Device performance and color acceptance remain outstanding; extra validation on writable code has a cost, so the net performance result must still be measured.

## Follow-up: native playback removes translated per-sample work

The next device captures use revision `1ca92190`. The 32-bit workload records 638,976 CompileBlock entries (301,709 hits), substantially fewer redundant entries than the previous multi-million-call capture, but presentation still has multi-second gaps. The managed workload records 966,656 entries and roughly 6.37 million handled host faults. Those results rule out calling the earlier dispatcher change sufficient.

Static instruction-boundary matching of the provided audio module places four hot 32-bit addresses at a common candidate relocation: `0x13450000` for `wrap_oal.dll`. The locations include sample dispatch and ring-buffer effects loops with dense x87 arithmetic. This relocation is an inference from the executable and sampled addresses, not a loader-recorded base. It points to audio mixing as a concrete translated CPU cost, rather than assuming the arithmetic is physics.

The shared OpenAL replacement exposes 121 playback/EFX entry points in both guest architectures and runs OpenAL Soft 1.24.3 mixing natively through CoreAudio. It keeps Wine, FEX, BoxedWine process ownership and guest files intact. The native API bridge validates guest ranges, keeps device/context handles as 32-bit tokens, isolates process generations, preserves the Sound setting and retires native resources on address-space destruction. Only implemented extensions and guest function pointers are exposed. The source pin, generated facade, iOS build and license are in the repository. [OpenAL Soft source](https://github.com/kcat/openal-soft/tree/dc7d7054a5b4f3bec1dc23a42fd616a0847af948)

The real native loopback test creates 64 playing sources plus reverb, checks audible finite PCM, context isolation, bad guest pointers, muting and cleanup. On the local x86-64 host, 5.33 seconds of audio mixes in about 0.03�0.04 seconds. This establishes functional native mixing and host headroom, not an iPhone speedup. DirectSound/XAudio workloads continue through their existing paths; this change does not promise to accelerate every visual novel.

All six original device color readbacks pass, including sRGB gray. The expanded probe adds programmable constant/swizzle output, RGBA upload, and BC1/BC2/BC3 textures. No global red/blue swap is justified by the passing basic cases. The application-specific visual symptom remains unresolved pending those readbacks.

The full VIXL fixture suite passes with writable-code validation, including modified immediates and indirect call targets in low/top aliases. A negative-control build disabling only the writable-page trigger fails at the stale immediate (the fixture returns FAIL rather than PASS), establishing that the test detects the original defect. The automatic writable-page check is deliberately limited to the single-block configuration already used by the app; an experimental multi-block interaction needs separate investigation before that mode can be enabled.

## Architecture alternatives

The current Madeira project describes **ARM64EC Wine with FEX translating x64 application code**, a native Metal bridge, and an in-process server. Its architectural advantage includes executing more Wine/runtime code natively; it is not simply a faster implementation of Linux syscalls. Its current README and code are more reliable for this comparison than its older speculative architecture analysis. Its modified code also has licensing conditions that require review before reuse. [Madeira repository](https://github.com/willfaust/Madeira)

| Option | Expected scope | Decision |
|---|---|---|
| Optimize native FEX dispatch, exact arithmetic and command recording in BoxedWine | Targets measured costs while retaining both guest bitnesses | Implemented in this iteration |
| ARM64EC Wine plus x64 translation | Reduces translated Wine work; requires rebuilding/integrating a different Wine execution ABI | Promising longer-term work, not a drop-in switch |
| Adopt an ARM64EC launcher wholesale | Also requires a new validated 32-bit execution path and different runtime ownership | Not a replacement for current 32/64 support without substantial work |
| Relax x86 memory ordering globally | May improve some translated workloads, but introduces concurrency/correctness risk | Not enabled on these samples alone |
| Reduce x87 precision globally | Could accelerate arithmetic but previously caused compatibility failures | Rejected; exact arithmetic retained |
| Switch GPU translators again | Would not remove the measured software arithmetic and atomic signal work | Not the first intervention |

FEX documents the cost of strong x86 memory ordering on ARM and has introduced targeted vector-memory optimizations. That supports investigating precise hot paths; it does not justify disabling ordering globally. [FEX 2404](https://fex-emu.com/FEX-2404/), [FEX 2406](https://fex-emu.com/FEX-2406/)

## Device acceptance

Use verbose logging off and leave reduced-precision x87 off. First run both cubes, preserving the new color-oracle log. Then compare the same 32-bit level and the same 64-bit scene with the previous build, including CPU samples, actual presentation intervals, host-fault growth and audio underruns. Record memory use after several minutes. A successful package or a faster arithmetic benchmark does not establish playable performance, correct colors, or glitch-free sound.
