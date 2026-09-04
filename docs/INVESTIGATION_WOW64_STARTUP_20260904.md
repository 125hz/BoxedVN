# WoW64 startup and first-frame investigation — 4 September 2026

Implemented shared CPU/signal and watchdog corrections in the BoxedWine runtime. This is a **device-validation candidate**, not evidence that the cube, the 32-bit visual novel, or the 64-bit game now works. No new iOS binary has been compiled or run for this change set.

All five supplied device logs identify build 137, `8e5ed098`. The checkout started at `8e5ed09834ce861c46ff875de345a6467d4ea5a0` on `codex/boxedwine-fex64-backend`. The supplied architecture description was treated as background; the conclusions below use the current checkout and logs.

## What the logs establish

| Log in `C:/Users/hero/Downloads` | Observation | Remaining uncertainty |
| --- | --- | --- |
| `boxedvn-20260904-180057.log` | Direct D3D9 cube: `vkQueueSubmit2` succeeds at line 5806; swapchain image 0 is acquired successfully at 5856. PID 10/TID `002C` exits at 5948 after 18 Vulkan calls, last `vkBeginCommandBuffer`. No `vkQueuePresentKHR` occurs. | Why that worker exits. The early submission does not establish a rendered cube frame. |
| `boxedvn-20260904-180222.log` | the 32-bit visual novel initializes Wine's Vulkan renderer. The last line, 6107, maps the visual novel launcher, window `0x10074`, 627×353. Watchdog output stops too. | Whether the watchdog deadlock fixed here explains this particular freeze; whether the launcher can then start the actual game. |
| `boxedvn-20260904-180430.log` | the 64-bit game displays `Internal error 0x06: System error!` at lines 5000/5002, before graphics initialization. | The failed startup check. The old capture did not enable the optional relay trace, and the error text alone is not a diagnosis. |
| `boxedvn-20260904-180543.log` | Desktop-launched x64 cube reaches repeated DXMT `MTLCommandBuffer_presentDrawable` calls, including line 13460. | Clean shutdown and repeated launch/translator ownership still need device acceptance. |
| `boxedvn-20260904-180745.log` | Desktop-launched D3D9 cube: submission succeeds at 6611, image acquisition at 6663, then PID 45/TID `0031` exits at 6673 after the same 18 Vulkan calls. No queue-present call follows. | Same worker-exit boundary as direct launch; changing only the desktop launcher would not address both. |

Both D3D9 captures have matching 804×585 window/surface/swapchain sizes. They do not support another speculative swapchain-size fix. Linux `exit(0)` is not the Windows thread exit status: Wine tears down the Unix pthread after sending the Windows status to its server.

## Implemented corrections

### Linux signal frames and floating-point state

The old frame put `siginfo` below the handler's stack pointer, where ordinary handler locals could overwrite it. Handler entry RSP was also 0 modulo 16 instead of 8. Nested `SA_ONSTACK` delivery restarted at the alternate stack top, overwriting the outer frame. The old `fpregs` pointer was null, so Wine could not obtain interrupted floating-point/SIMD state through it.

The new layout keeps the restorer, ucontext, siginfo and FP image above the handler's downward-growing stack, aligns entry and FP storage, preserves the red zone, and allocates nested frames below the interrupted alternate-stack pointer. Both asynchronous delivery and synchronous faults use it. Sigreturn restores the guest-visible FP image, including changes Wine made to it. This follows the [Linux signal-context ABI](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/uapi/asm/sigcontext.h) and the context accesses in [Wine 9's signal implementation](https://raw.githubusercontent.com/wine-mirror/wine/wine-9.0/dlls/ntdll/unix/signal_x86_64.c).

The frame preserves x87 registers/tags/control/status, MXCSR and all sixteen XMM registers. FEX advertises AVX, so translated threads additionally receive the standard XSAVE header, YMM upper halves and Linux extension markers. The interpreter, which does not advertise AVX, uses the legacy FP format. FXSAVE/FXRSTOR and MXCSR storage in the interpreter use the same architectural conversion. This does not make all interpreter SIMD arithmetic implement MXCSR rounding or exception behavior.

FEX's pinned x87 TOP is one three-bit byte, not three adjacent one-bit flags. The adapter previously discarded TOP bits during every exchange. The conversion now preserves the full status word, physical register/tag indexing and cached software-FPU values. ABI assertions bind the conversion to the pinned FEX flag locations. Source: pinned FEX `04cbb90c715519136da771af3cc8f1dac9b821a6`, `X86Enums.h`, `X87.cpp`, and `Core.cpp` register reconstruction APIs. Vendor sources were not modified.

### Watchdog deadlock

The watchdog suspended a guest thread, then called FEX `IsAddressInCodeBuffer`. In the pinned backend that query takes `IosMigrateLock`, which the suspended thread can own. The watchdog then cannot reach `thread_resume`. This is a concrete lock-order defect, although the supplied the 32-bit visual novel capture cannot prove which lock was held when logging stopped.

Sampling now uses the fixed executable pool bounds and copies saved state without querying mutable JIT metadata. Log fields label RIP as `rip_source=frame`; it can be stale during translated execution. Diagnostic low/top guest memory reads now use the guest-to-host alias. The source contract test rejects reintroducing the known lock-taking queries inside the suspended interval.

### Diagnostic coverage

Expected WoW64 segment-selector traps no longer spend the generated-fault log budget. The supplied cube capture had exhausted that budget on Wine's repeated `mov fs` transition by line 3275. Opcode reads now apply the guest alias for 32-bit code too.

`BOXEDWINE_X64_WINE_TERMINATE_THREAD_REQUEST` records the requested Windows exit code before Linux thread teardown, bounded to 64 reports. It decodes the fixed Wine 9 request on Unix sockets; it reports a **request**, not successful server acceptance. The decoder validates opcode, size and variable-length fields against [Wine 9's server protocol](https://raw.githubusercontent.com/wine-mirror/wine/wine-9.0/include/wine/server_protocol.h).

The existing **Verbose Wine trace** setting now applies to both cube launchers and the 64-bit desktop as well as Run program. It includes SEH and two targeted ntdll thread-exit exports. The default remains off. DXVK 2.5.2's outer [thread entry catches exceptions and returns 1](https://raw.githubusercontent.com/doitsujin/dxvk/v2.5.2/src/util/thread.cpp), so a worker's disappearance must not be treated as a successful frame or repaired by inventing semaphore wakes.

## Graphics choice

Keep BoxedWine in charge of processes, syscalls, memory, Wine and X11, with FEX translating the selected guest process. Keep the working x64 DXMT route. Upstream [DXMT implements D3D10/11](https://github.com/3Shain/dxmt), so the existing D3D9 route remains DXVK → Wine WoW64 Vulkan → ELF64 bridge → MoltenVK.

Upstream [DXMT 0.50 added WoW64 support](https://github.com/3Shain/dxmt/releases/tag/v0.50). That is the relevant direction for future 32-bit D3D11 acceleration, but this fork currently builds only x86-64 DXMT PE modules. Enabling it here requires matching PE32 modules and the Wine WoW64 Unix-call/pointer-conversion path, plus agreement with the native hostcall ABI. Copying the PE64 DLL into the 32-bit directory is not that implementation. This change set does not add PE32 DXMT.

The the 32-bit visual novel `XShmCreateImage` unimplemented message is not independently fatal: [Wine 9's bitmap path](https://raw.githubusercontent.com/wine-mirror/wine/wine-9.0/dlls/winex11.drv/bitblt.c) falls back to `XCreateImage` when it returns null. the 64-bit game's early error also provides no evidence for changing its graphics backend.

## Verification completed

- MSVC tests-only build and CTest: **513 support tests passed**. New cases cover stack alignment/red zone, nested alternate stacks, all eight TOP positions, all 65,536 x87 status words, tag classification, saved context edits, AVX components and Windows exit-code decoding.
- Existing Python suites: verbose trace 29, alias backing 86, translator role transfer 55, graphics probe 31 — **201 passed**.
- Pinned FEX exit-dispatch/loader contract script passed, including the watchdog guard.
- Clang frontend parsing of `cpu64.cpp` and `syscall64.cpp` through the installed clang-tidy succeeded with Windows compatibility defines. This is not an Apple Objective-C++/Swift build.
- `git diff --check` passed.

Reproduce host checks with the Visual Studio x64 environment initialized:

```text
cmake --preset tests-only
cmake --build --preset tests-only
ctest --preset tests-only --output-on-failure
python scripts/test_x64_verbose_wine_trace.py
python scripts/test_x64_alias_page_backing.py
python scripts/test-fex-exit-dispatch-contract.py --fex-root third_party/fex64/fex
python scripts/test_x64_translator_role_transfer.py
python scripts/test_x64_graphics_probe.py
```

## Device acceptance still required

Compile/package this change set using `.github/workflows/build-ios.yml` with the FEX64 runtime and verify the resulting installed revision in each fresh log. The current changes are local; no new IPA or device results accompany this report.

1. Direct D3D9 cube twice: require continued worker activity, successful queue presentation and a visibly rotating cube. If it stops, collect the new termination-request status and the first real fault.
2. the 32-bit visual novel with tracing initially off: require a responsive launcher, then launch the actual game and check rendering/input. A blue desktop or mapped launcher alone is insufficient.
3. the 64-bit game with Verbose Wine trace on: capture through the first error dialog. Use the caller/module, API return values, SEH and loaded modules to identify its startup check before implementing a title-specific correction.
4. 64-bit desktop: launch x64 cube, close it, launch D3D9 cube, close it, then x64 cube again. Require translator release/reacquisition and responsive desktop/input throughout. Repeat with one 32-bit and one 64-bit program after their direct routes pass.

No claim of first-frame success, fixed the 64-bit game startup, or clean repeated desktop launches is made by the host tests.
