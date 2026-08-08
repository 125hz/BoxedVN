# BoxedVN iOS architecture

How the pieces fit together, and why each seam is where it is. If you are
about to change the app lifecycle, the threading, or the JIT path, read the
relevant section first — those three are the ones with non-obvious constraints.

---

## 1. Layers

```
┌──────────────────────────────────────────────────────────┐
│ SwiftUI          ios/app/Sources/*.swift                 │
│                  library, import, game detail, status,   │
│                  settings, log viewer                    │
└───────────────────────────┬──────────────────────────────┘
                            │  plain C, via the bridging header
┌───────────────────────────▼──────────────────────────────┐
│ C ABI            ios/runtime/include/BVNRuntime.h        │
│                  ios/runtime/include/BVNImport.h         │
└───────────────────────────┬──────────────────────────────┘
┌───────────────────────────▼──────────────────────────────┐
│ Objective-C++    ios/runtime/src/*.mm                    │
│  bridge          lifecycle, state machine, JIT probe,    │
│                  logging, paths, platform shim           │
└──────────┬───────────────────────────────┬───────────────┘
           │                               │
┌──────────▼──────────────┐   ┌────────────▼───────────────┐
│ boxedvn_support         │   │ Boxedwine core (upstream)  │
│ ios/support/            │   │ source/, platform/, lib/   │
│ PE detection, safe ZIP  │   │ Linux kernel emulation,    │
│ import, manifests, JSON │   │ x86 CPU, ARM64 JIT, Wine   │
│ no SDL/UIKit/Boxedwine  │   │ SDL2 2.32.10               │
└─────────────────────────┘   └────────────────────────────┘
```

**Swift never includes a Boxedwine or SDL header.** The bridging header pulls
in exactly two files, both plain C. That is what keeps the frontend
independent of the emulator's C++ and what will let a second backend slot in
later without a frontend rewrite.

---

## 2. Application lifecycle

This is the part that is easiest to get wrong.

SDL's iOS backend expects to own `UIApplicationMain`. `SDL_UIKitRunApp`
installs `SDLUIKitDelegate`, and that delegate calls the supplied main function
from `-postFinishLaunch` **on the main thread**. `SDL_PumpEvents` then services
the run loop from inside it by calling `CFRunLoopRunInMode` in a loop
(`SDL_uikitevents.m`). An SDL application on iOS therefore never returns from
`main` in the ordinary sense.

BoxedVN adopts that arrangement rather than fighting it:

```
main()                          ios/runtime/src/BVNMain.mm
  └─ SDL_UIKitRunApp(argc, argv, BVNGuestMain)
       └─ UIApplicationMain(..., [SDLUIKitDelegate getAppDelegateClassName])
            └─ BVNAppDelegate                 ios/runtime/src/BVNAppDelegate.mm
                 ├─ [super application:didFinishLaunchingWithOptions:]
                 │    schedules -postFinishLaunch
                 ├─ createLibraryWindow
                 │    NSClassFromString(@"BoxedVNFrontend")
                 │      -> makeRootViewController -> UIHostingController
                 │    UIWindow at UIWindowLevelNormal - 1
                 └─ -postFinishLaunch
                      └─ BVNGuestMain          ios/runtime/src/BVNRuntime.mm
```

`BVNAppDelegate` subclasses `SDLUIKitDelegate` and overrides
`+getAppDelegateClassName`, which is SDL's own documented subclassing hook, so
no SDL source is patched.

`SDLUIKitDelegate` is not installed by SDL's build, so the interface BoxedVN
subclasses is redeclared in `BVNAppDelegate.mm`. It is pinned to the SDL2
version in `scripts/dependencies.lock.sh`; if that pin moves, re-check the
declaration against `src/video/uikit/SDL_uikitappdelegate.h`.

### There is no `@main` in the Swift code

`BoxedVNFrontend` is reached by name, through `NSClassFromString`, because the
static library is linked *by* the Swift target and cannot reference it. The
contract is:

```swift
@objc(BoxedVNFrontend)
final class BoxedVNFrontend: NSObject {
    @MainActor
    @objc static func makeRootViewController() -> UIViewController
}
```

If that class or selector is renamed, the app launches to a black screen and
`BVNAppDelegate` logs exactly which half of the contract is missing.

`-ObjC` is in `OTHER_LDFLAGS` because nothing references `BVNAppDelegate` by
symbol — SDL looks it up by name at runtime — so the linker would otherwise
drop it from the static archive.

### Two windows

| Window | Level | Owner |
|--------|-------|-------|
| library (SwiftUI) | `UIWindowLevelNormal - 1` | `BVNAppDelegate` |
| guest (SDL) | `UIWindowLevelNormal` | SDL, created on `SDL_CreateWindow` |

`BVNRuntime` hides the library window before calling `boxedmain()` and shows it
again when that call returns, through `BVNFrontendHideLibrary` /
`BVNFrontendShowLibrary`. The SwiftUI hierarchy is never torn down, so
navigation state survives a session.

---

## 3. Threading model

| Concern | Thread | Notes |
|---------|--------|-------|
| UIKit and SwiftUI | main | as UIKit requires |
| SDL event handling | main | inside `boxedmain()`; SDL's UIKit backend requires it |
| Idle run-loop servicing | main | `BVNGuestMain` calls `CFRunLoopRunInMode(…, 0.02, true)` while no guest is running — this is what keeps SwiftUI alive |
| Guest CPU and JIT | pthreads | created by Boxedwine's `KNativeThread`; `BOXEDWINE_MULTI_THREADED` is on |
| Audio | SDL audio thread | SDL2's coreaudio backend |
| Frame presentation | main | inside Boxedwine's loop, via `KNativeScreenSDL` |
| Launch requests | any | `BVNRuntimeRequestLaunch` only *records*; the guest is always started from main |
| Shutdown | any | `BVNRuntimeRequestShutdown` posts `SDL_QUIT`, which SDL documents as thread safe |

State shared between threads (`gState`, the pending launch, the last error) is
guarded by one mutex in `BVNRuntime.mm`. The frontend polls
`BVNRuntimeGetState` every 500 ms rather than being called back, which keeps
the C ABI free of callbacks into Swift.

**One session at a time.** `acceptLaunchLocked` refuses a launch while the
state is `Starting`, `Running` or `Stopping`, and says which.

### The state machine

```
        ┌────────────────────────────────────────────┐
        │                                            │
     Idle ──launch accepted──► Starting ──► Running ─┼─► Stopped
        ▲                          │            │    │
        │                          │            └────┼─► (shutdown) Stopping ─► Stopped
        │                          └──JIT missing───►└─► Failed
        └───────────────── next launch accepted from Stopped or Failed
```

`Stopped` and `Failed` are deliberately *not* reset to `Idle`, so the frontend
can show how the last session ended.

---

## 4. JIT

Boxedwine's own ARM64 code generator is used — `JitArmV8CodeGen`, reached
through `BOXEDWINE_MAC_JIT` + `TARGET_CPU_ARM64` in `include/boxedwine.h`,
which selects `BOXEDWINE_JIT_ARMV8`, `BOXEDWINE_MULTI_THREADED`,
`BOXEDWINE_HOST_EXCEPTIONS` and `BOXEDWINE_MEM_CACHE`. There is no separate
BoxedVN JIT and there is no demonstration JIT.

### iOS is not macOS here — and it is not just a different toggle

macOS and iOS gate JIT memory through **two different mechanisms**, not two
configurations of the same one. Getting this distinction wrong is a real trap:
an earlier version of this port assumed the iOS mechanism was "MAP_JIT,
unlocked by CS_DEBUGGED" — plausible, matches a technique that genuinely
existed pre-iOS 14, and wrong. Apple patched that trick; `MAP_JIT` on iOS
stays gated behind the separate `dynamic-codesigning` entitlement, which is
approved only for browser engines and is never obtainable by a sideloaded
app, debugger attached or not. Passing `MAP_JIT` on iOS fails with `EPERM`
unconditionally — this is exactly the bug that made the physical-device JIT
probe report failure even with a genuine StikDebug attach confirmed via
`CS_DEBUGGED`.

| | macOS arm64 | iOS arm64 |
|---|---|---|
| Allocation | `mmap(..., MAP_JIT)` — `MAP_BOXEDWINE = MAP_JIT` | plain `mmap(..., PROT_EXEC)`, no `MAP_JIT` — `MAP_BOXEDWINE = 0` |
| Region behaviour | per-thread W xor X, toggled | plain RWX for as long as `CS_DEBUGGED` holds |
| `pthread_jit_write_protect_np` | required around writes | **unavailable; does not compile** |
| Gate | `com.apple.security.cs.allow-jit` at signing time (app-controlled) | kernel-set `CS_DEBUGGED`, present only while a genuine third-party debugger (StikDebug or equivalent, over the real Developer Disk Image / debugserver channel) is attached |
| Instruction cache flush | `__builtin___clear_cache` works | must use `sys_icache_invalidate` |

The last row is a separate link-time trap: `__builtin___clear_cache` lowers to
a call to compiler-rt's `__clear_cache` on AArch64, and compiler-rt
deliberately does not build that file for Darwin. An iOS link fails with an
undefined `___clear_cache`. `platform/linux/platform.cpp` routes both
`writeCodeToMemory` overloads and `clearInstructionCache` through one helper
that calls `sys_icache_invalidate` on iOS.

### The probe — split into a safe check and an unsafe one

`ios/runtime/src/BVNJIT.mm` exposes **two** entry points, not one, because
actually testing execution can crash the process with no recovery possible,
and that risk must never be taken automatically.

**Why:** a real device crash confirmed that on iOS, `mmap(PROT_EXEC)`
succeeding does **not** mean the page can safely be executed. Apple's
code-signing enforcement for anonymous executable memory is lazy — checked at
first *instruction fetch*, not at `mmap()` time. When that lazy check fails,
the kernel does not return an error to the process; it delivers an
**uncatchable `SIGKILL`** (`CODESIGNING` / "Invalid Page") the instant
execution is attempted. There is no signal handler, no exception, no way to
guard against it from inside the process. An earlier version of this probe
called through the mapped page unconditionally at every app launch (inside
`AppModel.init()`), and crashed the app on boot as a result — every time,
regardless of whether a JIT enabler was even attached.

`BVNJITProbeStatus()` — **safe, call freely:**
- reads whether `BOXEDWINE_JIT` is compiled in (compile-time flag)
- reads `csops(getpid(), CS_OPS_STATUS, ...)` for the `CS_DEBUGGED` flag
- **never maps or executes anything**
- reports at most `BVNJITStatusLikelyAvailable` — necessary evidence, not
  proof

This is the only probe automatic code may call: `AppModel.init()`'s initial
value and the 0.5s poll timer's `refreshJIT()` (`ios/app/Sources/AppModel.swift`)
both use it exclusively, so the JIT badge stays live without ever risking a
crash on a launch or a timer tick the user didn't ask for.

`BVNJITProbeExecute()` — **unsafe, gated to specific call sites:**
1. `mmap(PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS)` — no `MAP_JIT`; this step alone is safe
2. write three ARM64 instructions that return `0x4258`
3. `sys_icache_invalidate`
4. **call it** — the unsafe step; a real `SIGKILL` can happen exactly here

Only called from `BVNRuntime.mm`'s `runSession()`, immediately before
`boxedmain()` actually starts a guest. That is a deliberate, user-initiated
moment (the user pressed Launch or Run Wine Notepad), and Boxedwine's own JIT
would hit the identical risk moments later regardless — so nothing is made
safer by skipping the check there, but tying a possible crash to a specific
action the user just took is far better than a silent crash on every cold
launch.

Whichever probe ran, a debugger-attached-but-`mmap`-still-fails result (only
observable in the unsafe probe, since the safe one never attempts the mmap)
points at the app's own signature — most likely `get-task-allow` not
surviving whatever tool signed the IPA (see `docs/BUILD_IOS.md`'s entitlements
table) — rather than at StikDebug.

---

## 5. Rendering

The first iOS build has **no OpenGL**. None of `BOXEDWINE_OPENGL_SDL`,
`BOXEDWINE_OPENGL_ES` or `BOXEDWINE_OPENGL_OSMESA` is defined.

Upstream's macOS arm64 target gets software GL from a prebuilt
`libOSMesa.8.dylib` backed by a 124 MB `libLLVM.dylib`, both macOS arm64
binaries. Porting that to iOS means cross-building Mesa and LLVM for iOS, and
llvmpipe would itself need JIT to be fast. That is a project in its own right
and is not on the path to running a visual novel.

So guest output takes Boxedwine's CPU framebuffer path: Wine renders into
guest memory with GDI/DirectDraw, `KNativeScreenSDL` blits it into an
`SDL_Texture`, and SDL2 presents it — Metal-backed on iOS.

**Consequence:** any guest program that requires OpenGL will fail, and
`KNativeSystem::getOpenGL()` logs "Failed to load OpenGL, will probably crash".
That is a deliberate, identified limitation, not a silent degradation.

The next step for accelerated content is `BOXEDWINE_ES` plus
`source/opengl/es`, which translates GL to GLES and which SDL2 supports on iOS
— not OSMesa.

---

## 6. Audio

All of `platform/mac/` is macOS-only in practice: `knativecoreaudio.cpp` is
Wine's CoreAudio driver built on the CoreAudio HAL (`<CoreAudio/CoreAudio.h>`,
absent on iOS), `audiounit.cpp` uses `kHALOutputParam_Volume` and the DLS synth
AudioUnit, `pixelformat.cpp` is CGL, `macOpenGL.mm` is AppKit.

The iOS build omits `BOXEDWINE_CORE_AUDIO`, which makes
`KNativeAudio::init()` register `KNativeAudioSDL` — SDL2's coreaudio backend,
i.e. AVAudioSession and RemoteIO. MIDI is off, and requesting it on iOS is a
configure-time error with an explanation rather than a link failure later.

---

## 7. Filesystem layout

| Location | Contents | Backed up | Visible in Files |
|----------|----------|-----------|------------------|
| Application Support `/rootfs` | root filesystem archive and writable overlay | no | no |
| Application Support `/prefixes/<id>` | one Wine prefix per game | no | no |
| Documents `/Games/<id>` | imported content and `manifest.json` | yes | yes |
| Documents `/Logs` | session logs | yes | yes |
| Caches `/BoxedVN` | regenerable data | no | no |

Wine prefixes are in Application Support because they are large, contain
thousands of small files, and are rebuildable — but saves live inside them, so
`docs/KNOWN_LIMITATIONS_IOS.md` records exporting saves into Documents as an
open gap.

`UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` are both set, so
a user can drop a root filesystem archive or a game into Documents without the
app, and copy saves and logs out.

---

## 8. How a guest is started

`BVNRuntime` synthesises an argv from real Boxedwine options
(`commandLine.txt` documents all of them) and calls `boxedmain()`:

```
boxedvn
  -root   <Application Support>/prefixes/<id>      writable overlay
  -zip    <root filesystem>.zip                    read-only, mounted at /
  -nozip  1                                        do not scan for a *Wine*.zip
  -log    <Documents>/Logs/boxedvn-<stamp>.log
  -mount_drive <Documents>/Games/<id>/content d    the game becomes D:
  -resolution 800x600
  -bpp 32
  -w      /home/username/.wine/dosdevices/d:/<workdir>
  /bin/wine  d:\game.exe  [arguments...]
```

`/bin/wine` is a Boxedwine symlink (`bin/wine.link`) into `/opt/wine/bin/wine`
inside the root filesystem.

The full command line is written to the log before `boxedmain` is called, with
spaces quoted, so a failing launch can be reproduced exactly.

---

## 9. Logging

`internal_log` writes to stdout and, when `-log` is passed, to
`KSystem::logFile`. On iOS neither stdout nor stderr reaches the user.

`BVNLog` redirects both through a pipe and reads it on a dedicated thread,
appending to the session log file **and** to a 512 KB in-memory tail that the
log viewer renders. So the viewer shows real emulator and Wine output, not a
BoxedVN summary of it. `BVNLogWrite` adds BoxedVN's own structured entries to
the same two sinks, interleaved in order.

Every write to the file is unbuffered, so a `kpanic` — which calls `exit(1)` —
loses at most one `read()` worth of in-flight bytes.

---

## 10. The backend seam

```
GuestArchitecture   Unknown | X86_16 | X86_32 | X86_64
ExecutableFormat    Unknown | NotAnExecutable | DosMz | NeWin16 | LeVxd
                    | Pe32 | Pe32Plus
RuntimeBackendID    None | BoxedwineX86 | FutureX64
```

`RuntimeBackendCapabilities` records, per backend, whether it is `implemented`,
which architectures it can execute, whether it `requiresJIT`, whether it has an
interpreter fallback, and which renderers it can present through.

- `BoxedwineX86` is implemented and executes `X86_16` and `X86_32`.
- `FutureX64` is a **reserved identifier**. `implemented == false`,
  `executableArchitectures` is empty, and `canExecute` returns false for
  everything. It exists so manifests can record which backend wrote them and
  so the library and importer are written against an abstraction — not because
  any x64 support exists.

`selectBackend(X86_64)` returns `None`, and the UI shows the message from
`unsupportedArchitectureMessage`: *"x64 is not supported by the current
runtime…"*.

Manifests are versioned (`schemaVersion`, currently 1). A manifest from a newer
BoxedVN is refused with an explanation instead of being partially read.

When an x64 backend is eventually added, what changes is `allBackends()` and a
new implementation. The game library, importer, manifest format and frontend
do not.

---

## 11. Build graph

```
boxedwine_zlib      lib/zlib + minizip
boxedwine_softfloat lib/softfloat            (80-bit x87)
boxedwine_asmjit    lib/asmjit               (ARM64 assembler)
boxedwine_pugixml   lib/pugixml
        │
boxedwine_core      source/** + platform/{linux,sdl}/**  + SDL2
        │
boxedvn_support     ios/support/**           (no SDL, no Boxedwine)
        │
boxedvn_runtime     ios/runtime/**
        │
libboxedvn.a        one archive, merged with libtool
        │
BoxedVN.app         Swift, linked by XcodeGen + xcodebuild
```

The merge exists so the application project links exactly one input and never
needs editing when the CMake target graph changes.

`cmake/BoxedwineSources.cmake` records where the source list came from:
`project/linux/makefile` and the macOS Xcode target, which uses
`PBXFileSystemSynchronizedRootGroup` for `source/`, `include/` and `asmjit/`
with no exception sets — every `.cpp` underneath them is compiled.
