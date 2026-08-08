# BoxedVN — Progress and Handoff

**Purpose of this file:** a single place that tells any future session (Claude,
Codex or a human) exactly where the project stands, what is proven to work,
what has not been tried yet, and what to do next. Update it at the end of every
working session. Keep it honest — an inflated status here costs more time than
it saves.

**Last updated:** 2026-08-07
**Branch:** `ios`
**Upstream base:** `danoon2/Boxedwine` commit
`379bf2414a67fc6509d506a6eefdf6ffa7ebf82d` (2026-08-05, "build fix"),
Boxedwine version string `26R2`.

---

## 1. What is proven to work

Everything in this section was executed and its output observed. Nothing here
is inferred.

| # | Milestone | Status | Evidence |
|---|-----------|--------|----------|
| 1 | Host-independent support library builds and passes tests | **done** | 59/59 tests pass via `ctest --preset tests-only` |
| 2 | Boxedwine emulator core compiles for iOS arm64 | **done** | `libboxedwine_core.a`, arm64, 577 `JitArmV8CodeGen` symbols |
| 3 | Objective-C++ runtime bridge compiles and links | **done** | `libboxedvn.a`, 12 MB, arm64, exports `_main` |
| 4 | SwiftUI app shell links against the core | **done** | `BoxedVN.app`, arm64, `LC_BUILD_VERSION` platform 2 (iOS), minos 17.0 |
| 5 | Unsigned IPA produced and smoke tested | **done** | `BoxedVN-unsigned.ipa`, 1.9 MB, unzips to `Payload/BoxedVN.app` |
| 6 | Reproducible scripts for every step | **done** | `scripts/*.sh`, all run clean from a fresh clone |
| 7 | GitHub Actions workflow | **written, not yet run** | `.github/workflows/build-ios.yml` |
| 8 | **App launches and renders real UI on-device** | **done, on simulator; fix applied to device build** | see section 1a |

### 1a. The black-screen bug: found, fixed, verified on simulator

The user reported that on a **physical iPhone 17**, BoxedVN installed and
launched but stayed on a black screen with no UI. Rather than guess, this was
debugged using the iOS Simulator (see section 1b for why that's valid despite
BoxedVN targeting devices only) with a real build/install/launch/log cycle.

**Root cause:** `SDL_UIKitRunApp` resolves the app delegate class with
`[SDLUIKitDelegate getAppDelegateClassName]` — a message sent to the
`SDLUIKitDelegate` class **by name**, not through dynamic subclass dispatch.
Objective-C class-method overrides only take effect for a message sent to the
subclass (or an instance of it); a call site that names the base class
explicitly always resolves against the base class's own method table,
regardless of what subclasses exist. `BVNAppDelegate` overrode
`+getAppDelegateClassName` inside its own `@implementation` block, which is
exactly the pattern that does **not** work here — SDL's own source comment
above that method says as much ("make sure to add a **category**"). Without
the category, `UIApplicationMain` was instantiating SDL's own vanilla
`SDLUIKitDelegate` the entire time. `BVNAppDelegate` — and therefore
`createLibraryWindow`, the `BoxedVNFrontend` lookup, and the SwiftUI window —
never ran at all. `BVNGuestMain` still ran fine (SDL's real delegate calls
`forward_main` regardless), which is why the process didn't crash and why the
JIT-probe log lines appeared even with no UI visible.

**A second, smaller bug was found and fixed along the way:** `BVNLogWrite`
calls made from `createLibraryWindow` happened before `BVNLogStartSessionFile`
(which was only called from inside `BVNGuestMain`), so any diagnostic from
window creation — including the one that would have named this exact bug —
never reached the log file, only an in-memory ring nobody could read because
the window that would show the log viewer never appeared. Fixed by moving
`BVNLogStartSessionFile()` to the very first line of
`-application:didFinishLaunchingWithOptions:`, before anything else runs.

**Fix:** `ios/runtime/src/BVNAppDelegate.mm` now declares
`@interface SDLUIKitDelegate (BoxedVN)` / `@implementation SDLUIKitDelegate
(BoxedVN)` with `+getAppDelegateClassName` returning `@"BVNAppDelegate"`. A
category attaches directly to the real class's method table, so it affects
every call site including SDL's own.

**Verified on the iPhone 17 simulator** (booted locally, built for x86_64
since the dev Mac is Intel — see section 1b): before the fix, black screen,
reproduced exactly as reported. After the fix: the library UI renders —
title, Runtime status ("JIT unavailable", correctly, since the simulator has
no ARM64 JIT), Games list, Import actions, Settings, Logs. Confirmed
repeatable across a full terminate/relaunch cycle.

**Applied to the device build** and repackaged:
`build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`83b2dba27cd63f89d19a020b7b6758d804cbcc466073b3cbf1280b5b6243ff59`. **Not yet
installed or tested on the physical iPhone 17** — that is the immediate next
step, replacing the previous IPA (SHA-256
`9997e540531a06c78785008c96986d631c0a811ec76ca198360d834c80f88265`), which
still has the bug.

### 1b. Why the simulator was usable for this despite section 3's warning

`docs/KNOWN_LIMITATIONS_IOS.md` and `docs/BUILD_IOS.md` correctly say the
simulator can't run a guest (Boxedwine's ARM64 JIT doesn't exist on x86_64,
and the simulator preset was documented as compile-only for that reason). But
the black-screen bug was in app-lifecycle code that runs **before** any guest
session starts, so it was fully reproducible and fixable there. Getting this
working required:

- Fixing `scripts/fetch-dependencies.sh`'s `ios-simulator` case to pass
  `-DCMAKE_OSX_ARCHITECTURES="$(uname -m)"` — it previously built with no
  explicit arch, which is wrong for a cross-compiled simulator target.
- Building `boxedwine_core` for `iphonesimulator` x86_64. This **worked
  without any new upstream guards**: `TARGET_CPU_ARM64` is false on x86_64, so
  `include/boxedwine.h`'s existing `#else` branch selects
  `BOXEDWINE_JIT_X64` — a real, already-supported Boxedwine backend — and the
  ARM64-only translation units (`armv8CPU.cpp`, `jitArmV8CodeGen.cpp`, etc.)
  simply compile to empty objects, exactly as they do on other non-ARM64
  Boxedwine targets.
- A new XcodeGen spec, `ios/project-simulator.yml`, since the shipping
  `ios/project.yml` hard-codes `SUPPORTED_PLATFORMS: iphoneos` /
  `ARCHS: arm64` (deliberately, for the device-only ship target) and
  xcodebuild resolves destinations from the project file, not from
  command-line `-destination`/setting overrides.
- `mcp__Claude_Code_iOS_Simulator__control` (`attach`, `launch`, `screenshot`)
  to drive it, plus reading the session log directly out of the simulator's
  container directory on disk
  (`~/Library/Developer/CoreSimulator/Devices/<udid>/data/Containers/Data/Application/<container>/Documents/Logs/`).

This is a legitimate, reusable debugging path for any future UI/lifecycle bug
that doesn't require JIT — worth reaching for before assuming something is
device-specific.

## 2. What has NOT been tried

Be explicit about this when reporting status. None of the following has been
observed, and no claim should be made about them until it has:

- **The app has never been launched on a physical device.** It was launched
  once on a physical iPhone 17 by the user and showed a black screen — that
  bug is now understood, fixed, and verified on the simulator (section 1a),
  but the fixed build has not yet gone back onto the physical device.
- **JIT has never been probed on real hardware.** `BVNJITProbe` is written,
  compiles, and was confirmed to correctly report "unavailable, not ARM64" on
  the x86_64 simulator — but no ARM64 device has executed it yet.
- **The root filesystem has never been mounted.** No guest has started.
- **Wine has never booted.** Notepad has not run.
- **No game has been imported on a device.** The importer is unit tested
  against synthetic archives on the host only.
- **Audio, input and rendering are untested at runtime.** They are wired
  through SDL2 but nothing has ever produced a frame or a sample.

## 3. Environment note that shapes everything

The development Mac available for this work is an **Intel Core i7-9750H**, not
Apple Silicon:

```
machdep.cpu.brand_string: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz
hw.optional.arm64:        not present
```

Consequences, which are not optional to work around:

- Cross-compiling for iOS arm64 works normally and is what the whole build
  does. This is unaffected.
- The **ARM64 macOS** Boxedwine target cannot be *run* here, only compiled.
  Upstream's `project/mac-xcode` build was therefore not executed; the source
  list and flags were derived from reading `project/linux/makefile` and the
  Xcode project file instead, which is recorded in
  `cmake/BoxedwineSources.cmake`.
- A **macOS x86_64** build can run locally and exercises Boxedwine's x64 JIT,
  not the ARM64 one. Useful for validating rootfs mounting and Wine boot
  logic on the host, useless for validating the ARM64 code generator.
- The iOS Simulator on this machine is x86_64, so it cannot exercise the ARM64
  JIT either. `ios-simulator-compile-check` is a compile-only preset for that
  reason and is documented as such.

## 4. Architecture decisions already made

These were decided after reading the actual upstream source. Changing them is
allowed but should be a deliberate choice, not an accident.

### 4.1 SDL owns `UIApplicationMain`; SwiftUI is hosted inside it

`main()` in `ios/runtime/src/BVNMain.mm` calls `SDL_UIKitRunApp`. SDL's
`SDLUIKitDelegate` calls the supplied main function from `-postFinishLaunch`
**on the main thread**, and `SDL_PumpEvents` services the run loop from inside
it (`SDL_uikitappdelegate.m`, `SDL_uikitevents.m`). Fighting that is how SDL
ports on iOS break.

So `BVNAppDelegate` subclasses `SDLUIKitDelegate` and overrides
`+getAppDelegateClassName`, which is SDL's documented subclassing hook.
SwiftUI lives in a second `UIWindow` at `UIWindowLevelNormal - 1`, reached by
name via `@objc(BoxedVNFrontend)` because the static library cannot reference
the Swift target.

**There is no `@main` in the Swift code, and there must not be one.**

### 4.2 Threading model

| Concern | Thread |
|---------|--------|
| UIKit / SwiftUI | main |
| SDL event handling | main, inside `boxedmain()` |
| Idle run-loop servicing | main, inside `BVNGuestMain`'s loop |
| Guest CPU / JIT | pthreads created by Boxedwine (`BOXEDWINE_MULTI_THREADED`) |
| Audio | SDL's audio thread |
| Frame presentation | main, inside Boxedwine's loop |
| Launch requests | any thread; only recorded, always *started* from main |
| Shutdown | any thread; posts `SDL_QUIT` |

One guest session at a time, enforced in `acceptLaunchLocked`.

### 4.3 JIT on iOS differs from macOS in a way that matters

`pthread_jit_write_protect_np` is **marked unavailable on iOS** and does not
compile. On iOS a `MAP_JIT` region is simply RWX once the kernel has granted
`dynamic-codesigning`, which happens only while a debugger is attached. So:

- The W^X toggle in `platform/linux/platform.cpp` is now macOS-only.
- The instruction-cache flush still runs on both, but via
  `sys_icache_invalidate` on iOS: `__builtin___clear_cache` lowers to
  compiler-rt's `__clear_cache`, which compiler-rt deliberately does not build
  for Darwin, so an iOS link fails with an undefined `___clear_cache`.
- `BVNJITProbe` verifies the whole path end to end (mmap → write → icache
  flush → call) rather than reading an entitlement string.

### 4.4 No OpenGL in the first iOS build

Upstream's macOS arm64 target gets software OpenGL from a prebuilt
`libOSMesa.8.dylib` backed by a **124 MB `libLLVM.dylib`**, both arm64 macOS
binaries. Neither exists for iOS, and cross-building Mesa + LLVM for iOS is a
project in its own right (and llvmpipe would itself need JIT).

So the first iOS build defines **none** of `BOXEDWINE_OPENGL_SDL`,
`BOXEDWINE_OPENGL_ES` or `BOXEDWINE_OPENGL_OSMESA`. Guest output goes through
Boxedwine's CPU framebuffer path (`KNativeScreenSDL`) onto SDL2's renderer,
which is Metal-backed on iOS.

This is the right call for classic 2D visual novels, which use GDI/DirectDraw.
It means **any guest program that requires OpenGL will fail**, and
`KNativeSystem::getOpenGL()` logs "Failed to load OpenGL, will probably crash".
`BOXEDWINE_ES` + `source/opengl/es` is the natural next step, not OSMesa.

### 4.5 Audio goes through SDL, not Wine's CoreAudio driver

All of `platform/mac/` is macOS-only in practice — the CoreAudio HAL header,
the DLS synth AudioUnit, CGL and AppKit. On iOS the build omits
`BOXEDWINE_CORE_AUDIO`, which makes `KNativeAudio::init()` register
`KNativeAudioSDL`. MIDI is off and is refused at configure time on iOS with an
explanation.

### 4.6 Backend abstraction is real but honest

`boxedvn::RuntimeBackendID` has `BoxedwineX86` (implemented) and `FutureX64`
(reserved, `implemented == false`, executes nothing). `selectBackend` returns
`None` for `X86_64` and the UI surfaces
"x64 is not supported by the current runtime". Manifests record which backend
they were written for. **Nothing claims x64 works.**

## 5. Upstream changes made (keep this list short)

Five narrow edits, all `TARGET_OS_IPHONE`-scoped or build-flag-gated:

| File | Change |
|------|--------|
| `include/boxedwine.h` | Do not auto-enable `BOXEDWINE_OPENGL_OSMESA` on iOS |
| `platform/linux/platform.cpp` | W^X toggle macOS-only; `sys_icache_invalidate` on iOS; generic `getPixelFormats` on iOS |
| `platform/sdl/knativesystem.cpp` | `BOXEDWINE_EXTERNAL_MAIN` suppresses the built-in `main()` |

Everything else is additive: `cmake/`, `ios/`, `scripts/`, `docs/`,
`.github/`, `CMakeLists.txt`, `CMakePresets.json`.

Two build-system workarounds avoid patching vendored third-party code:

- `boxedwine_zlib` is compiled with `-Dfdopen=fdopen`. The vendored zlib
  predates the Apple SDK change that makes `<sys/cdefs.h>` pull in
  `<TargetConditionals.h>`; its `zutil.h` then sees `TARGET_OS_MAC`, assumes
  classic Mac OS, and `#define fdopen(fd,mode) NULL`, which mangles the real
  declaration in `<stdio.h>`.
- `boxedwine_asmjit` is compiled with `-include libkern/OSCacheControl.h` on
  iOS. asmjit calls `sys_icache_invalidate` on every `__APPLE__` target but
  only includes the header under `TARGET_OS_OSX`.

## 6. Repository map

```
CMakeLists.txt              one CMake graph for all native code
CMakePresets.json           ios-device-release, ios-device-debug, macos-dev,
                            tests-only, ios-simulator-compile-check
cmake/BoxedwineSources.cmake  the emulator source list, with provenance
ios/support/                host-independent logic, no Boxedwine/SDL/UIKit
  include/boxedvn/          architecture, pe_inspector, path_safety,
                            zip_import, manifest, json, game_import
ios/tests/                  59 tests, dependency-free harness
ios/runtime/                the C ABI + Objective-C++ bridge
  include/BVNRuntime.h      runtime, JIT, paths, logging  (Swift sees this)
  include/BVNImport.h       import, manifests, backends   (Swift sees this)
  src/BVNMain.mm            main() -> SDL_UIKitRunApp
  src/BVNAppDelegate.mm     SDLUIKitDelegate subclass + SwiftUI hosting
  src/BVNRuntime.mm         the state machine and boxedmain() argv
  src/BVNJIT.mm             the end-to-end JIT probe
  src/BVNLog.mm             stdout/stderr capture + session log
  src/BVNPaths.mm           storage layout
  src/BVNImport.mm          C wrapper over ios/support
  src/BVNMacPlatformShim.mm MacPlatform* symbols platform.cpp expects
ios/app/                    the SwiftUI shell
ios/project.yml             XcodeGen spec (the .xcodeproj is NOT committed)
scripts/                    every build step, all set -euo pipefail
docs/                       BUILD_IOS, ARCHITECTURE_IOS, TESTING_IOS,
                            KNOWN_LIMITATIONS_IOS
```

## 7. How to build, from a clean clone

```bash
git clone <this fork> && cd boxedvn && git checkout ios
./scripts/bootstrap-macos.sh
./scripts/fetch-dependencies.sh --platform ios
./scripts/fetch-rootfs.sh                 # optional; see docs/BUILD_IOS.md
./scripts/build-ios.sh --configuration Release
./scripts/package-ipa.sh --app build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN.app
```

Full detail, including what a signing tool does to the entitlements, is in
`docs/BUILD_IOS.md`.

## 1c. JIT still reporting "unavailable" after enabling it (2026-08-07, same day)

After the black-screen fix, the user reported: launched with JIT enabled
(StikDebug), and Runtime status still said "JIT unavailable."

**Found two real bugs**, both fixed without needing the physical device:

1. **The JIT badge never auto-refreshed.** `AppModel.jit` was set exactly
   once, at `AppModel.init()` — a `.probe()` call that runs at cold launch,
   before any JIT enabler has necessarily attached (StikDebug's attach is
   inherently asynchronous relative to app launch). The only way to get a
   fresh read was to navigate into **Runtime status** and tap **Re-check**.
   If the user just glanced at the home screen — which is exactly what
   "launched with JIT on... still says unavailable" describes — they'd see a
   permanently stale reading no matter what actually happened afterward.
   **Fix:** `AppModel.refreshJIT()` is now called on the same 0.5s timer that
   already polls runtime state (`ios/app/Sources/AppModel.swift`). The probe
   is one mmap+munmap of a single page — cheap enough to just keep live.

2. **No way to tell "StikDebug never attached" from "it attached but JIT is
   still broken."** The probe only ever reported the `mmap` failure with one
   generic message. Added a second, independent signal: `csops(getpid(),
   CS_OPS_STATUS, ...)` read directly from the kernel
   (`ios/runtime/src/BVNJIT.mm`), checking the `CS_DEBUGGED` flag. This is
   the same flag StikDebug's attach is trying to set, checked independently
   of whether the `mmap` probe below it succeeds. Surfaced as
   `BVNJITReport.debuggerAttached` through the C ABI, `JITReport` in Swift,
   and a new "Debugger attached (CS_DEBUGGED)" row in Runtime status, with
   the detail message now branching on it:
   - not attached → points at StikDebug/the attach itself
   - attached but `mmap` still fails → points at the app's own signature
     (specifically: whether `get-task-allow` survived signing — see below)

**Verified:** rebuilds cleanly, links, runs without crashing on the
simulator with the new fields wired through end to end (compile + runtime
smoke test only — the diagnostic's actual discriminating value only shows up
on a device where `CS_DEBUGGED` genuinely varies, which the simulator can't
exercise). **Not yet confirmed against a real StikDebug attach.**

**Also checked, not a bug but worth knowing:** the IPA is built and packaged
unsigned by design (`CODE_SIGNING_ALLOWED=NO`), so `ios/app/BoxedVN.entitlements`
— confirmed intact, `get-task-allow: true` — is not yet baked into any
signature at the point this repo produces the IPA. Whether it survives is
entirely up to the signing tool used afterward (SideStore/Sideloadly/AltStore/
etc.), and some tools strip entitlements they don't recognise. **If the new
"Debugger attached" row reads "yes" but "Executable memory" still reads
"no,"** that is the smoking gun for this — re-sign with a tool/flow that
preserves it, or check the signed IPA's entitlements directly with
`codesign -d --entitlements :- BoxedVN.app` after signing.

New IPA: `build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`958b07324a95a3efee2a6e51428b29e56970a334077410b1f099410dd9cd3b56`.

## 1d. The real JIT mechanism was wrong — MAP_JIT is the wrong tool on iOS (2026-08-07)

The physical-device screenshot from section 1c's fix showed something the
simulator could never have shown: **`CS_DEBUGGED` correctly `yes`** (StikDebug's
attach genuinely worked) **but `mmap(PROT_EXEC | MAP_JIT)` still `EPERM`.**
That is a specific, diagnosable combination, not a vague failure — researched
it rather than guessing again.

**Root cause:** `MAP_JIT` on Apple platforms is gated behind Apple's separate
`dynamic-codesigning` entitlement, approved only for browser engines. No
sideloaded third-party app can ever have it, debugger attached or not. The
mechanism this code was originally built on — "`CS_DEBUGGED` unlocks
`MAP_JIT`" — genuinely existed pre-iOS 14 and Apple patched it (confirmed via
research: the pre-14 self-`ptrace(PT_TRACE_ME)` trick some old emulator ports
used was *also* separately patched around the same time). Passing `MAP_JIT`
on iOS now fails with `EPERM` unconditionally.

What a **legitimate** third-party debugger attach (StikDebug/Jitterbug/
SideJITServer, over the real Developer Disk Image / debugserver channel —
the same one Xcode's own "Debug > Attach to Process" uses) actually grants:
the kernel flags the process `CS_DEBUGGED`, which relaxes code-signing
enforcement for **plain `mmap()`/`mprotect()` `PROT_EXEC` transitions** on
that process. No `MAP_JIT` flag involved anywhere in this path.

**Fix:** `MAP_BOXEDWINE` (`include/boxedwine.h`) is `0` on iOS now, not
`MAP_JIT` — `Platform::alloc64kBlock` (`platform/linux/platform.cpp`) is
otherwise unchanged, it just requests `PROT_EXEC` without `MAP_JIT` in the
flags. Verified via `source/util/bnativeheap.cpp` that Boxedwine allocates its
JIT code buffer exactly once per block with no follow-up `mprotect` step, so
this one call site is the only place that needed to change.
`ios/runtime/src/BVNJIT.mm`'s probe now performs the identical allocation, so
a passing probe means the real allocator will actually work, not just that
some other memory trick succeeded.

**A second, smaller bug found while fixing this:** `BVNJIT.mm` never
`#include`d `boxedwine.h`, so its own `#if defined(BOXEDWINE_JIT)` check was
always false — "ARM64 JIT compiled in" read "no" on *every* build regardless
of whether the real JIT was present (it was; 577 `JitArmV8CodeGen` symbols,
verified separately). Fixed by passing `BOXEDWINE_JIT` as an explicit compile
definition to `boxedvn_runtime` in `ios/runtime/CMakeLists.txt`, since this
target only ever builds where it's unconditionally true.

Corrected the same "`CS_DEBUGGED` grants `dynamic-codesigning`" conflation
everywhere it had been documented (it was in the original section 1a/1c write-up
above, too — treat those as superseded by this entry, not as still-accurate
background): `ios/app/BoxedVN.entitlements`, `docs/ARCHITECTURE_IOS.md`,
`docs/BUILD_IOS.md`, `docs/KNOWN_LIMITATIONS_IOS.md`, `docs/TESTING_IOS.md`.

**Verified:** rebuilds clean for the arm64 device target; confirmed via `nm`/
`strings` on the linked binary that the corrected `mmap` call and new error
strings are actually present in what gets shipped. **Not yet re-tested against
the physical device this diagnosis was built from** — that is the very next
step and the one that actually closes this out.

New IPA: `build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`d6ec5182a98a1051d06eb950dfe05c24a390f9822a790faaf6285fa85d5686e0`.

## 1e. The MAP_JIT fix caused a crash-on-boot regression — fixed by splitting the probe (2026-08-07)

Installing the section-1d IPA made the app **crash on every launch**, before
any user interaction. Diagnosed from the actual `.ips` crash report (not
guessed):

- Crashing thread: main thread, inside `AppModel.init()` → `BVNJITProbe()`.
- `exception`: `EXC_BAD_ACCESS` / `SIGKILL`, `KERN_PROTECTION_FAILURE`.
- `termination`: `{"namespace":"CODESIGNING","indicator":"Invalid Page"}`.
- `ktriageinfo`: *"Failed to fault in a page with execute permissions."*
- The faulting region showed `rw-/rwx SM=COW` — **current** protection was
  read/write only, despite `PROT_EXEC` having been requested at `mmap()` time
  and that call having returned successfully.

**What this reveals:** on iOS, `mmap(PROT_EXEC)` succeeding does not mean the
page can safely be executed. Code-signing enforcement for anonymous
executable memory is lazy — checked at first *instruction fetch*, not at
`mmap()` time. When that lazy check fails, the kernel does not return an
error to the calling code; it delivers an **uncatchable `SIGKILL`** the
instant execution is attempted. Section 1d's fix (drop `MAP_JIT`, request
`PROT_EXEC` directly) was directionally correct — `MAP_JIT` genuinely cannot
work here — but it silently traded a safe failure mode (`mmap` returning
`EPERM`, handled gracefully) for an unsafe one (a process-killing fault with
zero recovery path), and that code ran unconditionally on **every cold
launch** via `AppModel.init()`, not just when actually testing JIT.

**Fix — split into a genuinely safe probe and an explicitly unsafe one:**

- `BVNJITProbeStatus()` — reads `BOXEDWINE_JIT` (compile-time) and
  `csops(getpid(), CS_OPS_STATUS, ...)` for `CS_DEBUGGED` only. **Never maps
  or executes anything.** Reports at most `BVNJITStatusLikelyAvailable`.
  This is now the *only* probe automatic code may call — `AppModel.init()`'s
  initial value and the 0.5s poll timer's `refreshJIT()` both use it
  exclusively.
- `BVNJITProbeExecute()` — the full mmap+write+icache-flush+**call** sequence,
  unchanged from section 1d's logic, but now clearly documented as unsafe and
  called from exactly one place: `BVNRuntime.mm`'s `runSession()`, immediately
  before actually starting a guest. That is a deliberate, user-initiated
  moment (Launch / Run Wine Notepad was just pressed), and Boxedwine's own
  real JIT hits the identical risk moments later regardless — so this ties a
  possible crash to a specific action, not to opening the app.
- New `BVNJITStatusLikelyAvailable` enum value distinguishes "debugger
  attached, JIT expected to work, not yet proven" from `Available` ("actually
  ran generated code successfully") in both the C ABI and the Swift UI.

A related Swift compiler issue surfaced while rebuilding `StatusView`: the
combined JIT section closure became too complex for the type-checker
("unable to type-check this expression in reasonable time") once the new
three-state status text/color logic was added. Fixed by factoring each row
and the footer into their own small `View` structs
(`ios/app/Sources/Views.swift`) — a good general pattern to reach for early
rather than letting one `body` closure grow indefinitely.

**Verified:**
- Rebuilds and links clean for the arm64 device target.
- Source-reviewed directly: neither `AppModel.init()` nor the poll timer's
  `refreshJIT()` reach `BVNJITProbeExecute()` — only `runSession()` does.
- Smoke-tested on the iPhone 17 simulator: fresh install, launch, screenshot
  confirms the library UI renders with no crash, process stays alive
  (`launchctl list` shows it running), no fresh crash report generated. (The
  simulator can't exercise the actual unsafe-probe risk itself — x86_64 isn't
  ARM64, so `BVNJITProbeExecute()` takes its early "not ARM64" return there —
  but it does confirm the split compiles, links, and the automatic paths
  genuinely never call the risky function during a normal boot.)
- **Not yet re-tested on the physical iPhone 17** that produced the crash
  report — that is the real confirmation this is fixed.

New IPA: `build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`5381984f40179e2b5f6d1cce55b325abf91f0563f65336b14006519f673415fa`. This
supersedes the section-1d build (`d6ec5182…`), which is the one that crashed.

## 1f. Root filesystem import: file picker wouldn't let the file be selected (2026-08-07)

Two rounds on this, because the first fix addressed a real but different bug
than the one actually reported.

**Round 1 — wrong diagnosis.** Reported: "opens the picker, but I cannot
select [wine10.zip]." Assumed the classic `allowedContentTypes: [.zip]`
selectability bug — `UTType.zip` sometimes fails to match a real ZIP whose
UTI got lost or ambiguous in transit (very common for AirDropped files), and
the picker greys the row out so it cannot be tapped at all. Fixed by
broadening both ZIP-importing `fileImporter` calls
(`ios/app/Sources/Views.swift`, game import and root filesystem import) to
`[.zip, .data]` — `.data` is the generic type essentially everything
conforms to, so it makes the file selectable regardless of how its UTI was
tagged. Also added real ZIP validation to `AppModel.importRootFilesystem`
(`BVNZipInspect`, the same check `GameLibrary.importGame` already used for
games) so a non-ZIP selection is still caught and reported clearly rather
than silently copied, and moved the copy off the main thread
(`Task.detached`, matching `importGame`'s existing pattern) since a ~150 MB
synchronous copy on the main thread would freeze the UI.

**Then the user clarified the actual symptom**, which is a materially
different bug: *"tapping [the file] does nothing"* — not greyed out, a normal
row, but tapping it neither selects nor dismisses the picker. Confirmed via
`AskUserQuestion` rather than guessing again, since a wrong second guess
would cost another full build/sign/install cycle on the only physical device
available. Also confirmed: works on the Simulator, fails only on the
physical device — an AirDrop-from-Mac transfer either way.

**Round 2 — the real cause.** `ios/runtime/src/BVNAppDelegate.mm` sets the
SwiftUI library window's `windowLevel` to `UIWindowLevelNormal - 1` (a
sub-normal level), on the theory that it would make a later SDL guest window
(created at the default normal level) visually cover the library without an
explicit hide, as a backstop alongside the explicit `.hidden` toggle in
`BVNFrontendHideLibrary`/`BVNFrontendShowLibrary`. A sub-normal window level
is non-standard, and non-standard levels are a known source of UIKit
misbehaving on exactly this kind of case: whether a window can properly
become key, and whether it correctly receives/routes touches to view
controllers it presents modally — which is exactly how SwiftUI's
`.fileImporter` presents `UIDocumentPickerViewController`. This lines up with
the reported symptom (taps registering as "seen" by the row but not acted
on) and with it being reproducible only on a physical device, not the
Simulator, where UIKit's window-level/key-window internals are known to
sometimes diverge from real hardware — the user's device is additionally on
an iOS 27 **beta**, which raises the odds of this kind of edge case further.

**Fix:** removed the `windowLevel` override entirely; the library window now
uses the standard `UIWindowLevelNormal`. This costs nothing structurally —
`BVNFrontendHideLibrary` already explicitly hides the window before a guest
starts (`BVNRuntime.mm`'s `runSession()`), so the level trick was never
load-bearing for the actual show/hide behaviour, only a redundant backstop
that turned out to have a real cost.

**Verified:** both fixes rebuild and link clean for the arm64 device target.
The content-type broadening was smoke-tested on the Simulator (picker opens,
files are selectable) before the real cause was identified; the window-level
fix has not yet been tested at all — it's a source-level fix reasoned from a
plausible, well-supported mechanism, not confirmed by reproduction, since
the failure mode (physical device only) isn't something available to test
from here. **This is the least-verified fix shipped so far in this project;
say so plainly if it turns out not to be the answer** rather than reaching
for a third guess.

New IPA: `build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`54091becda5aaabaf10c2b42eff4d506ad7eb1871061ef3f73396cc22056a1b9`. Supersedes
`5381984f…` (crash-on-boot fix; still had the picker bug) and
`f1f60e76…` (an intermediate build with only the content-type broadening,
which does not fix the actually-reported symptom and was never sent to the
user).

## 8. Next actions, in order

1. **Sign and install the new IPA** (`build/artifacts/BoxedVN-unsigned.ipa`,
   SHA-256 `5381984f…3415fa` — supersedes `d6ec5182…`, which crashed on boot;
   see section 1e) **on the physical iPhone 17**. First confirm the app now
   opens to the library UI at all (that is the crash fix — should need no
   debugger attached to verify). Then attach StikDebug and open Runtime
   status: expect `Debugger attached: yes` and `Status: Likely available`
   automatically. Actual confirmed `Status: Available` only appears after
   pressing **Run Wine Notepad** or **Launch**, since that is now the only
   place `BVNJITProbeExecute()` runs — if JIT genuinely doesn't work, expect
   the crash to happen *there* instead (tied to that action, not to opening
   the app), and the exact `errno` from the log at that point is the real
   diagnosis. With MAP_JIT already eliminated as a cause, a failure there
   would point somewhere new.
3. **Import a root filesystem** through Settings and confirm the file lands in
   Application Support.
4. **Run Wine Notepad.** This is the first real end-to-end test: rootfs mount,
   Linux kernel emulation, Wine start, framebuffer presentation. Expect
   problems here, and expect the session log to explain them.
5. **Verify the session can exit** without force-quitting, and that the
   library window comes back (`BVNFrontendShowLibrary`).
6. **Import one DRM-free 32-bit visual novel** and run it.
7. Only then: input refinement, audio verification, and the GLES renderer.

## 9. Known open questions

- **Does `boxedmain()` survive being called twice?** The design returns to the
  library UI after a session and allows another launch. Boxedwine calls
  `SDL_Init`/`SDL_Quit` inside that span. If a second session breaks, the
  fallback is to require an app restart between sessions, which still meets
  the MVP requirement of one session at a time but not the "exit without
  force-closing" criterion. **Untested.**
- **Does SDL 2.32.10's UIKit backend cooperate with a second `UIWindow`?**
  The library window sits below SDL's. Untested on device.
- **Memory.** Boxedwine reserves a large guest address space. The
  `increased-memory-limit` entitlement is requested but free-Apple-ID signing
  strips it. Whether the app survives on a 4 GB device is unknown.
- **`kpanic` kills the process.** `internal_kpanic` calls `exit(1)`. The log
  file captures the message first because stdout/stderr are redirected
  unbuffered, but the app does die. Acceptable for the MVP; noted in
  `docs/KNOWN_LIMITATIONS_IOS.md`.

## 10. Session log

### 2026-08-07 — initial port
- Cloned upstream, recorded commit, created `ios` branch.
- Read `project/linux/makefile` and the macOS `.pbxproj` to derive the real
  source list and flag set; recorded provenance in
  `cmake/BoxedwineSources.cmake`.
- Discovered the dev Mac is Intel, adjusted plan (section 3).
- Built `ios/support` + 59 tests; all pass.
- Compiled the emulator core for iOS arm64; four upstream guards needed.
- Wrote the runtime bridge; two more link-level fixes needed
  (`___clear_cache`, CoreBluetooth).
- Produced a validated 1.9 MB unsigned IPA.
- Wrote the build scripts, presets, CI workflow and docs.
