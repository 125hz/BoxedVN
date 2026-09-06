# Runtime and control follow-up

Input: device revision 2b37ad0d, build 137. Captures 152047 (32-bit 3D workload), 155043 (32-bit visual novel), 155540 (legacy Direct3D startup), 160124 (64-bit managed runtime), and 160416 (browser runtime).

## Evidence and changes

- The 3D workload has a 96% busy worker in `boxedvn_fex_extF80_mul`; acquire/present wait totals are negligible at the sampled slow interval. Add an opt-in FEX F64 x87 mode, off by default. The adapter converts between FEX's double stack and architectural 80-bit state, preserving software rounding/exception state. Full precision remains the compatibility baseline. This can change numerical results and is experimental; restart the app after changing it. The existing FXSAVE restore patch already handles F64 conversion and physical stack ordering.
- Cache the host syscall trace switch, avoiding libc getenv on every syscall; reduce ordinary device/mount tracing to warnings. No timing or scheduling semantics change.
- The first later 3D fault is a translated 32-bit RET at 0x439db52, host fault address 0x70c17aaf80. The existing capture does not identify the AArch64 memory operand. Add the faulting host instruction and registers/predictor base to the already bounded fault report. Do not infer a recovery from a RET alone or suppress a guest exception. The crash remains unresolved.
- Legacy startup reports missing COM class 62be5d10-60eb-11d0-bd3b-00a0c911ce86. Supply missing SystemDeviceEnum and DeviceMoniker registrations only for packaged devenum DLLs, in the matching registry views, preserving overrides. This is a multimedia enumeration repair, not evidence that cutscene audio or every startup failure is fixed.
- The managed runtime opens its actual Managed/mscorlib.dll before rejecting it. Local read-only inspection confirms a valid PE/CLI header, IL-only flags, and BSJB metadata signature. The final mono/2.0 diagnostic is a fallback search, not proof that files should be moved. Add managed assembly data/map diagnostics and Mono assembly tracing with verbose mode. No content modification or per-program environment workaround.
- The browser runtime still spawns 32-bit helper processes into the 64-bit fallback interpreter. Its RET-imm instruction failure is part of the missing WoW64 interpreter execution mode, not a missing DLL. A single opcode stub or disabling Chromium process isolation would not implement that mode. This remains unresolved.

## Frontend

Rename Games to Shortcuts and use “No shortcuts created yet”. Move the old directory intact; if the destination exists, preserve the old tree under Previous shortcuts and enumerate it as well. Fall back to the original directory if the move fails. Existing relative content resolution preserves manifests after relocation.

Add WASD/arrow joystick mapping, release held keys when it changes, and add center-lock relative mouse motion for Wine cursor mode. Deltas accumulate through the existing 60 Hz motion coalescer; grabbed XInput clients receive raw deltas and legacy motion clients receive points around the center. Menus generally need center lock off.

Wrap three bounded recent log records instead of truncating each to one line. Errors red, warnings orange, fixmes yellow, traces cyan. Each record is bounded to 900 characters so a malformed giant guest message cannot create unlimited layout work.

Retain the embedded presentation host during an active session when SwiftUI recycles a List row. Rebind replacement rows without exposing SDL's old foreground window. Avoid reattaching an unchanged host on every SwiftUI update.

Remove user-facing stop/quit controls pending safe FEX cancellation and teardown. No promise of restarting another container within the same app process. Restart the app to switch sessions. Natural process exit and internal shutdown handling remain.

## Validation and acceptance

Seven Windows host targets pass, including actual SoftFloat round-trip/rounding/state-preservation tests and architecture-scoped COM repair tests. FEX dispatch contract, 61 futex checks, and Wine packaging tests pass (platform-specific packaging checks deferred to CI). CI compilation and packaging must succeed before publication. UI gestures, faster x87 execution, startup progress, audio behavior, and crash stability require device testing; host tests cannot establish those results.

## Primary sources

- [Pinned FEX implementation](https://github.com/FEX-Emu/FEX): X87F64 decoder, stack optimization and FXSAVE conversion in the checked-out source; no upstream vendor modifications.
- [Wine 9 multimedia COM definitions](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/devenum/devenum_classes.idl).
- [Unity Mono image loader](https://github.com/Unity-Technologies/mono/blob/unity-5.6/mono/metadata/image.c) and [file mapping](https://github.com/Unity-Technologies/mono/blob/unity-5.6/mono/utils/mono-mmap.c).
