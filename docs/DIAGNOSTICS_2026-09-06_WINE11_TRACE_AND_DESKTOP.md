# Wine 11: trace overhead and nested desktop windows

## Evidence

Six device captures (`223955`, `224047`, `224138`, `224312`, `224551`,
`224844`) identify revision `22ee91bb+dirty`. Both graphics probes now render,
the desktop starts, and the 64-bit application proceeds beyond its former
startup failure. These are device confirmations of progress, not just CI
results. All six launches explicitly enable Wine relay and DXMT trace output.

- The 32-bit visual novel capture is 452.7 MB / 5,026,172 lines. It ends while
  comparing changing audio asset filenames, with the main thread still active.
  There are roughly 625,000 calls to each of the kernel32 and kernelbase
  `lstrcmpiA` entry points. This capture does not establish a deadlock.
- The 32-bit 3D application capture is 314.1 MB / 4,317,593 lines. It still
  reads configuration/data and executes application code, with repeated
  `Sleep(0)` calls on another thread. DirectDraw capability probing reports
  swapchain failures; it is not yet established that those failures block the
  eventual D3D9 path. A non-verbose retest is needed before attributing the
  remaining startup delay to Wine 11 itself.
- The 64-bit application capture is 457.4 MB / 5,732,775 lines. Input polling,
  clocks, and file reads dominate relay traffic. DXMT's detailed allocation
  census is also enabled by the verbose setting. No FPS improvement can be
  quantified from this instrumented run alone.
- The desktop log creates and paints an 800x20 taskbar at y=580. A missing
  on-screen taskbar therefore does not mean Wine omitted its creation.

## Changes

1. Extend the existing Wine relay exclusion list for bulk string comparisons,
   file reads, zero-duration sleeps, condition waits, input polling, and clock
   queries. Registry, module loading, exceptions, window creation, and message
   boxes remain traced. Shared syscall witnesses retain file-error/progress
   evidence. On an offline pass over the supplied API Call/Ret records, this
   filter excludes 99.7%, 94.6%, and 83.5% respectively in the three application
   captures. Those percentages describe trace records, not speed gains.
2. Remove the syscall-level stdout/stderr tee. `DevTTY::writeNative` already
   emits the same bytes to the captured host console, so every guest diagnostic
   was logged twice. The descriptor still receives every write, and existing
   exception/lock diagnostics remain. Redirected output stays with its intended
   file or pipe instead of being unconditionally copied into the host log.
3. Translate X window origins to screen coordinates before native blitting.
   X children store parent-relative positions; `XWindow::draw` previously used
   those positions directly, so a client nested inside a moved desktop/window
   was drawn at the wrong location. Reuse the same ancestor transform already
   used for input and the Metal patch compositor. This fixes a concrete shared
   compositor defect, but the complete reported desktop appearance still needs
   a device retest.

No application-specific patches or data modifications. Wine remains 11.0,
hosted by BoxedWine with FEX and the existing graphics backends. The current
container and Wine ZIPs can be retained; these changes ship with the IPA.

## Validation and sources

All 33 verbose-trace contract tests and all eight host CTest suites pass.
The trace tests preserve startup diagnostics while checking the new hot-call
exclusions and the single descriptor-owned output path. Native rendering is
compiled by the iOS CI job and still requires device confirmation.

- [Wine 11 relay filter implementation](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/relay.c): exclusion matching and dynamically sized registry-value loading.
- [Xlib window coordinates](https://xorg.freedesktop.org/archive/current/doc/libX11/libX11/libX11.html#Window_Attributes): child geometry is relative to its parent.
- [Wine 11 taskbar initialization](https://github.com/wine-mirror/wine/blob/wine-11.0/programs/explorer/systray.c): creates and shows the shell taskbar for a shell-enabled desktop.
