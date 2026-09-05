# Startup and input retest at revision 62153970

All four captures identify `62153970+dirty` with the increased-memory
entitlement active. They cover the cube twice, a 64-bit application, and a
32-bit text application.

## Device evidence

* 23:05:09: the 64-bit application acquires executable segment 1 and records
  3,064 `MTLCommandBuffer_presentDrawable` calls. No guest access fault or
  executable-pool exhaustion appears. The user reports visible animation,
  with the entire frontend unresponsive from the launch onward, before any
  input into the game. The capture contains roughly 148 MB of verbose output.
* 23:11:30: the cube's shader hash table has 27 elements but 305,431,229
  buckets; rehash requests 620,239,453 buckets and hits the allocator's size
  check. Earlier empty tables return 2 buckets rather than the native
  policy's 13, and the recorded resize threshold stays zero. Ordinary RAM
  exhaustion does not explain those arithmetic results. The verbose cube
  capture at 23:02:41 ends without a first frame or that final rejection.
* 23:07:14: the text application produces implausible layout coordinates and
  later exhausts its stack in `msvcrt!frexp`, at guest RIP `0x7aae3a19`.
  The fault report completes. Its control word is `0x027f`.

## Changes

The final launch callback now queues the blocking guest session with
`CFRunLoopPerformBlock`, on the main run loop's common modes. This lets the
GCD presentation completion return before entering SDL's nested event loop.
CoreFoundation intentionally suppresses reentrant main-queue service when
already executing a main-dispatch callback. Keeping the entire guest lifetime
inside that callback starves SwiftUI work despite continuing Metal frames.
UIKit and SDL stay on the main thread. A native macOS regression test uses
the production scheduling helper and requires queued main-thread work to run
before a simulated session exits.

The session emits a main-queue sentinel and the independent log watcher
reports event-loop phase/age and main-queue age. A two-second stall captures
the main host PC/LR, with four stack reports per session. This distinguishes
queue starvation from a callback, drawing operation, or lock that still
blocks after the launch change.

FEX's saved x87 register image uses logical ST(i) order, but its restore
copied values straight into physical R(i). The maintained patch restores
each value to `(saved TOP + i) & 7`, retaining physical tag bits. It also
converts the external 80-bit format when reduced-precision mode is selected.
The ARM64 fixture tests nonempty save/restore at all eight TOP positions and
the Windows `0x027f` control word. This is a concrete state-restoration defect;
it is not yet proven to explain all observed arithmetic corruption.

The 32-bit cube now reports raw-bit integer conversion, division/rounding,
subnormal scaling, and a nonempty save/restore before touching D3D9. This
separates a basic native JIT failure from later WoW64 or graphics state.
The same test passes on native i386 Windows; the cube cross-compiles with
warnings treated as errors. Host support tests, relay tests, and the
exit-dispatch source contracts pass. CI additionally executes the translator
fixture and real CoreFoundation scheduling test before publishing the IPA.

References: [Apple CoreFoundation main-queue reentrancy guard](https://github.com/apple-oss-distributions/CF/blob/main/CFRunLoop.c),
[SDL 2.32.10 UIKit event pump](https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/video/uikit/SDL_uikitevents.m),
and pinned FEX `04cbb90c715519136da771af3cc8f1dac9b821a6`,
`OpcodeDispatcher/Vector.cpp` and `OpcodeDispatcher/X87.cpp`.

## Device acceptance still required

Retest the 64-bit application with verbose off first, including frontend
buttons, guest cursor/taps, and stopping the session. Then test a desktop
launch and the 64-bit graphics cube. Test the 32-bit cube with verbose off
and the text application with verbose on. Look for a prompt
`BOXEDWINE_SESSION_MAIN_SENTINEL`, low queue ages, and
`BOXEDWINE_PE32_X87_PROBE ... pass=1`. Rendering or text startup remains
unverified until new device captures. The guest PE32 runtime DLLs are unchanged
in this revision; no external ZIP replacement is needed for these changes.
