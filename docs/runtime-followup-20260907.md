# Runtime and Metal follow-up, 2026-09-07

Device baseline: 442dbbf5. Seven logs from 22:37 through 22:54 were reviewed.
All changes apply to shared runtime behavior; no executable-name routing or game-data changes.

## Findings and changes

- Native compute-pipeline creation used an invalid Metal device. In the new DXMT
  source, ArgumentEncodingContext declared device_ after ClearUAV even though
  ClearUAV constructs pipelines through that context. C++ initializes in member
  declaration order. Move device_ first. This directly matches the visual novel's
  objc_msgSend crash in newComputePipelineState. Missing compute pipelines may
  also affect rendering correctness; the reported color discrepancy still needs
  a device comparison after this repair. No guessed gamma override was added.
- Unlocked cube rendering spent about 7.85 ms in nextDrawable, about 600 calls
  per five seconds. Separate offscreen rendering from display acquisition in
  unlocked mode, acquiring at most 120 display images per second and retaining
  fence dependencies on intervening frames. The FPS counter counts submitted
  game frames, including offscreen ones; this does not increase physical refresh.
- Slow 32-bit gameplay instead spent most of its time on CPU work, with millions
  of cached CompileBlock visits. Enable FEX's connected-block compilation in the
  adapter. Keep the direct-link and dead-flag-pass safeguards and full x87 precision.
  This is a performance candidate, not a measured 30 FPS result.
- Audio starvation reached 59 underruns in one five-second interval. Allow two
  host callbacks per default fragment and room for three fragments. This adds
  buffering latency and tolerates short producer stalls; it cannot hide long CPU
  stalls or guarantee continuous audio under overload.
- The Unity loading exceptions occur in System.Guid.NewGuid, both during analytics
  initialization and savepoint creation. Inspection of the supplied mscorlib shows
  RandomNumberGenerator.Create returning null before GetBytes. Add missing-only
  registrations from Wine 11's rsaenh.rgs when the provider DLL is packaged.
  Existing provider paths/defaults are preserved. The device must confirm whether
  missing registration was the cause. Most earlier host faults were handled
  unaligned atomic operations, not repeated managed null dereferences.
- getpid incorrectly returned the calling thread ID. Return the owning process ID
  and keep gettid distinct. Wine compares process identity across threads, so this
  affects shared runtime checks and signal targeting.
- Desktop directory enumeration returned entries successfully. The drawing bridge
  ignored GC clipping, and line drawing used incorrect coordinates/color. Implement
  rectangle/bitmap clipping, correct line rasterization, safe negative bounds and
  overlap-safe copies. Gather only requested image rows rather than copying every
  preceding row for small dirty rectangles. This fixes concrete rendering defects;
  complete file-manager and taskbar recovery still requires the device test.
- Fullscreen uses one draggable side menu for keyboard, controls and presentation
  mode. Its normalized position persists across rotation. It fades after four
  idle seconds and restores opacity on touch.

## Validation

Host CMake/CTest suites, guest runtime boundary fixtures, sanitizer-backed production
X11 pixel fixtures, guest pointer-rewrite and ABI/shim tests pass. CI compiles the
native iPhoneOS and both guest PE graphics halves and publishes the increased-memory
IPA. Physical-device acceptance remains outstanding for all reported symptoms.

## Primary references

- Wine 11: dlls/rsaenh/rsaenh.rgs and dlls/advapi32/crypt.c
  https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/rsaenh/rsaenh.rgs
- Apple nextDrawable availability and blocking:
  https://developer.apple.com/documentation/quartzcore/cametallayer/1478172-nextdrawable
- Microsoft Direct3D 9 gamma behavior:
  https://learn.microsoft.com/en-us/windows/win32/direct3d9/gamma
- POSIX process and thread identity:
  https://www.man7.org/linux/man-pages/man2/gettid.2.html
