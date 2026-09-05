# Startup retest: build 137

All five supplied device captures identify revision `8b0e503c`. They therefore
exercise the previous signal-state and watchdog changes. They do not establish
successful rendering or game startup.

## Evidence

| Capture | Last useful boundary |
| --- | --- |
| First non-verbose 32-bit cube | Vulkan submission and acquire succeed; worker `002C` requests Windows thread termination with status `1`; no present follows. |
| Second non-verbose 32-bit cube | Repeats the same worker exit and missing first frame. |
| Verbose 32-bit cube | `RtlExitUserThread(1)` is visible, followed by repeated `NtTerminateThread` relay calls. The verbose trace changes the termination behavior. |
| 32-bit visual novel | Wine selects Vulkan, creates windows and assigns the main caption. Guest execution continues during the capture, but there is no evidence of a visible game window or first rendered frame. |
| 64-bit game | The application displays the same internal error before graphics initialization. Its relevant startup API calls are absent despite verbose tracing being enabled. |

The first non-verbose cube also eventually reports a Wine server partial wakeup
write and repeated interrupted futex waits. These occur after the rendering
worker has exited; they do not explain that initial exit. The allocation census
counts reserved pages as mapped and is not proof of an allocation failure.

## Confirmed diagnostic defect

[Wine 9's relay filter](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/ntdll/relay.c)
matches a bare list entry against a function name. An entire module needs
`module.*`. The previous six bare module entries silently excluded the APIs the
trace was intended to show. The two explicit ntdll function entries did match,
which explains why thread-exit calls appeared while other startup calls did not.

The include list now uses module wildcards, keeps the existing hot-call
exclusions, and includes only `RtlExitUserThread` from ntdll. The direct
`NtTerminateThread` relay entry is removed because it loops in this capture;
the existing emulator-side Wine server request witness still records its
handle and exit status. The underlying relay interaction is not claimed fixed.
Tests now exercise representative API names against Wine's named-export filter
semantics, including a regression proving that the old bare names fail.

## Missing renderer exception evidence

[DXVK 2.5.2's Windows thread wrapper](https://github.com/doitsujin/dxvk/blob/v2.5.2/src/util/thread.cpp)
catches every exception escaping the worker and returns status `1` without
logging it. This is consistent with the device evidence, but is not yet proof
of which exception, or even which exit path, the device took. The CS thread's
inner handler only catches `DxvkError`.

The same pinned DXVK source (`b4faf0bb3e3f6eb62920d745870422b50f8f33ab`) now
builds in CI with the existing MoltenVK compatibility patch and a worker
diagnostic patch. `BOXEDWINE_DXVK_WORKER` records the thread ID and reports
`DxvkError`, `std::system_error` including its numeric code, `std::bad_alloc`,
other standard exceptions, and unknown exceptions. A bounded stack buffer and
direct stderr write avoid using the allocating DXVK logger on this failure path.
The original exit status is preserved. A startup marker proves that the patched
worker wrapper was loaded even when it does not catch an exception.

Both the Wine64 PE overlay and the app's DXVK resource folder receive those
same rebuilt DLLs. CI verifies the PE32 machine and diagnostic payload, records
source/patch provenance, and keys its output cache on the pin, build script,
validator and patches. There is no renderer-version change or game-specific
override. The existing compatibility feature mask is retained, not newly
validated as a complete substitute for unsupported Vulkan features.

## Acceptance boundary

Local validation: 32 relay tests passed. The relevant packaging/probe suite
also passed (51 Linux-dependent tests skipped on Windows). A native Windows
harness compiled the actual patched DXVK thread wrapper and exercised normal
return plus five exception categories, verifying stderr reports and exit
statuses: success remains 0 and exceptions remain 1. This tests the diagnostic
path, not FEX translation, Vulkan rendering, or iPhone execution.

This is a diagnostic repair, not a demonstrated game or rendering fix. Host
tests and compilation cannot determine the cause of the worker exit or the
64-bit application's startup check. The next device capture must show the
worker marker and exception detail, or the last successful API call before the
error. The visual novel needs a verbose capture long enough to distinguish
slow window initialization from a stable wait.

Test the 32-bit cube with verbose tracing off first. Test the 64-bit game and
32-bit visual novel with verbose tracing on. A 64-bit DXMT cube run checks for
regression. Preserve each session log and the approximate wait duration.

Delivery remains one increased-memory entitlement template, `BoxedVN.ipa`.
The user performs artifact downloading, checksum verification, signing and
device testing.
