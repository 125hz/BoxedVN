# BoxedVN — Progress and Handoff

**Purpose of this file:** a single place that tells any future session (Claude,
Codex or a human) exactly where the project stands, what is proven to work,
what has not been tried yet, and what to do next. Update it at the end of every
working session. Keep it honest — an inflated status here costs more time than
it saves.

**Last updated:** 2026-08-11 (build 81 is device-confirmed to fix Grisaia's
stretched/cut-off resolution, but builds 81 and 82 did not fix its right-side
input. The build-82 log proves Boxedwine now delivers taps as far right as
guest x=1227 to the correct 1280x720 X11 client, while Wine still initialized
the session at 800x600. The observed in-game x position is compressed by the
same 800/1280 ratio, so build 83 starts every imported Wine game whose launch
resolution is `default` on a generic 1280x720 virtual monitor. Explicit
per-game resolutions still take precedence. This prevents Wine from caching
an 800-pixel monitor before a game creates a 1280-pixel client. The Windows
support suite passes 93/93, and GitHub Actions run 31553074737 passed the full
iPhoneOS build and published the direct IPA. Fresh-device input validation is
pending. Song
of Saya remains
device-proven playable with the
interpreter workaround.
The newest
detail is at the end of the session log; the open-problem list lives in
`docs/CONTINUING_WITHOUT_A_MAC.md`.)
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
| 1 | Host-independent support library builds and passes tests | **done** | 92/92 tests pass in `build/tests-only/ios/tests/boxedvn_tests` |
| 2 | Boxedwine emulator core compiles for iOS arm64 | **done** | `libboxedwine_core.a`, arm64, 577 `JitArmV8CodeGen` symbols |
| 3 | Objective-C++ runtime bridge compiles and links | **done** | `libboxedvn.a`, 12 MB, arm64, exports `_main` |
| 4 | SwiftUI app shell links against the core | **done** | `BoxedVN.app`, arm64, `LC_BUILD_VERSION` platform 2 (iOS), minos 17.0 |
| 5 | Unsigned IPA produced and smoke tested | **done** | `BoxedVN-unsigned.ipa`, 1.9 MB, unzips to `Payload/BoxedVN.app` |
| 6 | Reproducible scripts for every step | **done** | `scripts/*.sh`, all run clean from a fresh clone |
| 7 | GitHub Actions workflow | **written, not yet run** | `.github/workflows/build-ios.yml` |
| 8 | **App launches and renders real UI on-device** | **done** | build 5 device session logs show frontend window creation and user-initiated Notepad launch |
| 9 | Wine 10 root filesystem imported on device | **done** | device container shows `Application Support/rootfs/boxedwine.zip`, 149.3 MB |
| 10 | StikDebug/TXM executable page preparation and RX/RW execution | **done** | build 6 returned from generated ARM64 code on the physical iPhone; see section 1l |
| 11 | Wine 10 boots and renders Notepad on hardware | **done** | build 9 first rendered; build 15 is high-resolution and interactive; see sections 1o and 1u |

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

## 2. What has NOT been proven yet

Be explicit about this when reporting status. No claim should be made about
the following until it has been observed:

- Build 23's 128 MiB JIT arena and full WineDbg attachment are device-proven.
  Build 24 proves removing winebus does not improve cold startup and that a
  second guest in the same Boxedwine process starts quickly. Build 25 proves
  MoltenVK and the Apple A19 GPU initialize, but DXVK 2.5.2 rejects the
  available Vulkan features. Build 26's WineD3D Vulkan replacement is locally
  built and tested but not yet device-proven.
- A game launcher runs on-device, but **no game title screen, audio, save, or
  load is proven yet**.
- Build 19's software keyboard is device-proven in Notepad. Hardware keyboard,
  native IME/Japanese composition, GameController and audio remain untested.
- The guest landscape lock needs an explicit physical rotate-both-directions
  acceptance test after the later keyboard/prefix changes.
- Guest exit, return to the library, and a second session in one app process
  remain untested.

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
Build 22 selects Wine 10's `renderer=gdi` no-3D backend for imported 2D games;
this is locally verified but not yet device-proven. The repository's
`BOXEDWINE_ES` + `source/opengl/es` experiment is old
and incomplete; accelerated graphics needs a deliberate translator plan, not
just a compile definition. The macOS OSMesa binaries remain unusable on iOS.

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
ios/tests/                  74 tests, dependency-free harness
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

**Round 3 — device evidence disproved round 2.** The normal window level and
AppDelegate `window` fallback shipped, but builds through 17 still present a
picker whose row highlights and never completes. The problem is deeper:
BoxedVN had no `UIApplicationSceneManifest`, created the library window by
manually borrowing a connected scene, and let pinned SDL 2.32.10 create its
guest window with deprecated `initWithFrame:`. Current iOS can render those
windows while refusing scene-owned services. This same malformed ownership
also explains why build 17's attached field reached `firstResponder=yes` but
never produced a keyboard scene.

Build 18 declares `BVNSceneDelegate`, creates the library with
`initWithWindowScene:`, and attaches SDL's guest window to the exact same
scene immediately after `SDL_CreateWindow`, before renderer creation or show.
Picker callbacks are now logged. Because physical-device remote-view behavior
still needs validation, build 18 also removes the picker as a single point of
failure: game ZIPs copied into **On My iPhone → BoxedVN** are listed on the
Library screen, matching the direct rootfs route in Settings.

**Round 4 — build 18 disproved scene ownership as the final picker fix.** The
device logged both scene connection markers and later attached SDL to that
scene, yet the SwiftUI `.fileImporter` still highlighted files without
completing. Build 19 replaces it for ZIPs and folders with an explicit
`UIDocumentPickerViewController(forOpeningContentTypes:asCopy: true)`, a real
delegate and forced file-extension display. Unlike `.fileImporter`, this asks
Files to import a sandbox-local copy rather than handing BoxedVN an
open-in-place security-scoped URL. The direct Documents lists remain the
picker-independent guarantee.

## 1g. W^X: the two-part JIT memory bug (2026-08-07)

Two separate mistakes, found one after the other because the first fix's
diagnostic exposed the second. Both were only visible on device.

**Symptom:** the app launched fine and reported `Status: JIT ready`, but
**Run Wine Notepad** crashed. The `.ips` showed `pc` equal to a freshly mapped
address and `esr` `(Instruction Abort) Permission fault`, with the region
listed as `rw-/rwx`.

**Part 1 — `mmap(PROT_EXEC)` does not fail, it clamps.** The region's *current*
protection came back `rw-` while `rwx` stayed as the *maximum*, and `mmap`
returned a perfectly valid pointer. Nothing reported an error; the JIT wrote
code and jumped, and the process died on a fault that cannot be caught. Fixed
by adding a `mprotect` after the mapping exists (the call the kernel does
honour for a `CS_DEBUGGED` process) and — importantly — a `vm_region_64`
verification that the execute bit is *genuinely* set before trusting it.

That verification is what made the rest of this tractable. It turned an
uncatchable fault into a printed sentence.

**Part 2 — iOS enforces strict W^X, silently.** With the verification in place
the device reported: *"mprotect(PROT_EXEC) reported success, but the region's
actual protection is rw- (max rwx)."* `mprotect` returns 0 and withholds the
execute bit whenever `PROT_WRITE` is requested alongside it. Execution is
allowed for this process — max is `rwx` — just never at the same time as write.
Fixed by keeping JIT pages `r-x` and flipping them to `rw-` only around actual
writes, via a new `boxedwineSetCodeMemoryWritable()` bracketing both
`Platform::writeCodeToMemory` overloads. Every code write in Boxedwine already
funnels through those two overloads (verified by grep: `bnativeheap.cpp` plus
both code generators), because macOS needs the same thing via
`pthread_jit_write_protect_np`.

**Part 3 — the W^X fix introduced a third failure, from dropping `PROT_EXEC`
from the `mmap`.** Next device report: *"actual protection is r-- (max rw-)."*
Note the **maximum** — it had lost its execute bit entirely, so no `mprotect`
could ever have granted execute, and the one that ran clamped to `r--` instead.
The mmap `prot` argument sets the region's **maximum** protection, and
`mprotect` can never raise a region above its maximum. `PROT_EXEC` must
therefore be passed to `mmap` *even though `mmap` will not honour it*, purely
to set the ceiling. Fixed in `Platform::alloc64kBlock` and mirrored in
`BVNJITProbeExecute`.

The final sequence, in both places:

1. `mmap` with `PROT_READ|PROT_WRITE|PROT_EXEC` — comes back `rw-`, max `rwx`.
2. Write the code while the page is still writable.
3. `mprotect` to `PROT_READ|PROT_EXEC` — this is what actually grants execute.
4. `vm_region_64` to **verify** the execute bit is set. Never skip this.
5. `sys_icache_invalidate`, then execute.

The probe's failure branches now distinguish *max lacks execute* (a bug in how
the page was mapped) from *current lacks execute despite max allowing it* (the
kernel refusing this process), because the two have completely different causes
and the earlier message conflated them.

**Verified:** builds clean for arm64; 26 pin checks pass. **Not yet confirmed
on device** — steps 1–2 are each confirmed by a device report, but the
combination has not been run.

**Open risk, stated up front:** `pthread_jit_write_protect_np` on macOS is
**per-thread**; `mprotect` is **process-wide**. A guest thread executing from a
page while another thread flips it writable will fault. Wine is multithreaded,
so this is live, not theoretical. The durable fix is a dual mapping
(`mach_vm_remap` the same physical pages at an `rw-` address for writing and an
`r-x` address for executing), which needs Boxedwine's write callbacks to target
the writable alias instead of the executable address. If crashes appear
*during* execution rather than at startup, and are timing-dependent, this is
the first suspect.

## 1i. Stop guessing: probe every allocation strategy at once (2026-08-07)

Attempt 3 (section 1g) failed too, and the message said why: *"the region's
MAXIMUM protection is rw- … this page was mapped without PROT_EXEC."* But the
probe **did** pass `PROT_EXEC` to `mmap` in that build — and the two device
crash reports from earlier builds show `alloc64kBlock`'s regions as
`rw-/rwx`, i.e. **max did include execute** at some point on this same device.

Those two facts contradict each other, and no amount of reading XNU source
resolves it from here. **Three guesses, three round trips, three wrong
answers.** Guessing a fourth time is not a strategy.

So `ios/runtime/src/BVNExecMemory.cpp` now tries **six** allocation strategies
in one pass, reads back what the kernel actually did for each with
`vm_region_64`, and uses whichever genuinely produced an executable page:

| # | strategy | needs write flip |
|---|---|---|
| 1 | `mmap(rwx)` used as-is | no |
| 2 | `mmap(rwx)` + `mprotect(rwx)` | no |
| 3 | `mmap(rwx, MAP_JIT)` | no |
| 4 | `mmap(rwx, MAP_JIT)` + `mprotect(r-x)` | yes |
| 5 | `mmap(rwx)` + `mprotect(r-x)` | yes |
| 6 | `vm_allocate` + `vm_protect(set_maximum)` + `vm_protect(r-x)` | yes |

Order is deliberate: strategies that leave pages writable **and** executable
come first, because those avoid the process-wide W^X window entirely.

Strategy 5 is the one already known to fail. The genuinely new candidates are
**3/4** — MAP_JIT was eliminated earlier on the assumption that `CS_DEBUGGED`
doesn't unlock it, which is worth re-testing rather than trusting — and **6**,
the Mach interface, which can set a region's *maximum* protection explicitly.
That is something the BSD layer cannot do after the fact, so if iOS really is
capping the maximum, 6 is the call that gets past it.

**The structural fix matters more than the matrix.** `Platform::alloc64kBlock`
now calls `BVNExecMemAlloc` instead of doing its own `mmap`, and
`boxedwineSetCodeMemoryWritable` forwards to `BVNExecMemSetWritable`. The probe
and the live allocator were two hand-synchronised copies of the same sequence,
and failure 3 happened precisely because they drifted. There is now one
implementation, so **a probe that passes is evidence about the code that
actually runs the guest.**

Every line of the matrix is written to the session log *as it is produced*,
before the execution step. If the call into freshly written memory kills the
process, the log still contains the full table plus "About to execute the probe
page" — so even a crash returns a complete answer.

**Verified:** builds clean for arm64 with no warnings; 26 pin checks pass.
**Not confirmed on device.** This build may still not run Notepad — but unlike
the last three, it cannot come back without telling us which strategy works.

## 1h. The app now reports its own version (2026-08-07)

Requested by the user: a sideloaded app has no App Store version history and no
update prompt, so there was no way to tell whether the installed IPA was the
newest build. `AppVersion` (`ios/app/Sources/Runtime.swift`) surfaces
`0.1.0 (3) · 769e6334` — marketing version, build number, git commit — on the
**first screen** under Runtime, in Runtime status, and in the first line of
every session log.

- Build number lives in the checked-in XcodeGen specs and is raised by
  `scripts/bump-build.sh` (`--set`/`--marketing` also available). It is not
  generated at build time from a timestamp, because a build from a given commit
  must stay reproducible on the CI runner.
- Git revision is stamped by `scripts/build-ios.sh` via `BVN_BUILD_REVISION`
  into the `BVNBuildRevision` Info.plist key. A working tree with uncommitted
  changes is marked `+dirty`. Anything not built through that script reports
  `unknown` rather than claiming a revision it doesn't have.

**Run `scripts/bump-build.sh` before packaging any IPA that goes to a device.**
Two IPAs reporting the same version is exactly the confusion this exists to
prevent.

## 1j. "Run Wine Notepad" froze the whole app - the probe could hang, not just crash (2026-08-07)

After section 1i's rebuild (build 4, revision `026811d4`), the user reported:
tapping **Run Wine Notepad** produced no alert at all and froze the entire
app - no popup, completely unresponsive, had to be force-quit. This is a
**different failure mode** from every previous report on this project: the
first three JIT bugs all produced a `.ips` crash report (SIGKILL, then
SIGBUS) - the process died and the crash reporter had something to say about
it. A silent freeze with no crash report is not the same bug wearing a new
face; it means the process did not die, it stopped making progress.

**Root cause, architectural, independent of which strategy hangs:**
`runSession()` (`ios/runtime/src/BVNRuntime.mm`) runs on the **main thread**
(SDL's UIKit backend requires this - see the file's own header comment) and,
until this fix, called `BVNJITProbeExecute()` **synchronously, directly on
that thread**, with no timeout. The main thread is the only thread servicing
UIKit's run loop. Anything that blocks it indefinitely presents to the user
exactly as reported: a totally dead app, no alert, because the alert itself
needs the main run loop to display.

Something inside the six-strategy probe (section 1i) not returning is
plausible on iOS specifically: a hardware permission fault delivered to a
process that has `CS_DEBUGGED` set **without an actively listening debugger**
on the other end of its Mach exception ports can leave the faulting thread
parked forever instead of terminating the process. Some external JIT
enablers (StikDebug included, potentially) set `CS_DEBUGGED` and then detach
rather than staying attached as a live debug session. This is a real,
previously-undocumented-in-this-project failure mode, distinct from the
faults that produced the two earlier `.ips` reports.

**The fix does not require knowing which strategy hangs, and deliberately
does not guess a specific cause:**

- `probeJitWithTimeout()` (`ios/runtime/src/BVNRuntime.mm`) runs
  `BVNJITProbeExecute()` on a background queue and waits on a
  `dispatch_semaphore_t` with a **6-second timeout**. If the probe does not
  signal in time, the wrapper gives up and returns `BVNJITStatusUnavailable`
  with a detail message saying plainly that this looks like a hang, not a
  crash, and pointing at the log.
- The stuck worker thread is **abandoned on purpose**, not joined or
  cancelled - a thread parked in a kernel trap servicing a hardware exception
  generally cannot be cancelled from user space, and forcing it risks
  corrupting kernel-side state for that fault. One leaked thread is a small
  price for the main thread never blocking.
- A `std::atomic<bool> gJitProbeInFlight` guards against a **second**
  concurrent probe if the user retries after a timeout: `BVNExecMemory`'s
  strategy-selection state (section 1i) is plain global bookkeeping with no
  locking of its own, so two probes racing on it would be a new bug layered
  on top of the first. A retry while one is still stuck gets an immediate,
  clear "still stuck, restart the app" message instead.
- Every `BVNExecMemory` strategy line is still written to the log file
  **unbuffered, as it happens** (section 1i). That did not change. It means
  that even though this exact freeze gave no popup, **the session log from
  that freeze already contains the last strategy line that ran before it
  stalled** - that is the single most useful piece of evidence for whoever
  picks this up next. See "Immediate next step" below.

**What this fix does and does NOT establish:** it guarantees the app can
never freeze completely from this call again - a hang now surfaces as a
clear "JIT unavailable, probe timed out" message after 6 seconds instead of
requiring a force-quit. It does **not** identify which of the six strategies
actually hung, or whether the true problem is a hang at all rather than
something else. That determination needs the log from the freeze that already
happened, or a fresh attempt with this build.

**Verified:** builds clean for arm64, no warnings, no errors. 26 pin checks
pass. **Not confirmed on device** - neither the timeout behavior itself, nor
(obviously) which strategy was actually responsible for the freeze the user
saw.

### Immediate next step for whoever picks this up (Claude or Codex)

1. **Recover the log from the freeze that already happened**, if the device
   still has it: BoxedVN's Documents folder is visible over USB/Finder
   (`UIFileSharingEnabled` is on) or in the Files app under "On My iPhone" →
   BoxedVN → `Logs/`. Find the newest `boxedvn-*.log` file with a
   timestamp matching the freeze and read its last few lines. Because
   `BVNLogWrite` writes to the log file unbuffered (`writeToSinks` in
   `ios/runtime/src/BVNLog.mm`), the last line present is the last thing that
   completed - almost certainly one of the six strategy lines from
   `BVNExecMemProbe` (`ios/runtime/src/BVNExecMemory.cpp`), naming the exact
   strategy that hung. **This is the fastest possible path to the real root
   cause** and should be checked before writing any more code.
2. **Ship build 5** (`ios/project.yml` / `ios/project-simulator.yml` already
   bumped to `CURRENT_PROJECT_VERSION "5"` as of this session; run
   `scripts/build-ios.sh` then `scripts/package-ipa.sh` to produce the IPA)
   and have the user retry **Run Wine Notepad**. Two outcomes:
   - **It times out again after ~6 seconds** with a popup this time (the
     actual fix working) - progress, and the log will again show which
     strategy is stuck; consider removing that specific strategy from
     `kStrategyOrder` in `BVNExecMemory.cpp` next, since it is worse than
     useless (a strategy that reports itself as viable and then hangs is more
     dangerous than one that fails outright).
   - **It behaves completely differently** (crashes cleanly, or actually
     works) - also progress; read whatever the log/`.ips` says next.
3. Only after JIT is confirmed non-hanging and either working or cleanly
   failing: continue with the milestones below.

## 1k. Device crash reports reveal the iOS 26/27 JIT contract (2026-08-07)

The paired iPhone's system crash-log domain contained five BoxedVN `.ips`
reports from the earlier launch/probe attempts. They resolve section 1j's open
question without guessing:

- three reports are `SIGBUS` instruction-abort permission faults with `pc`
  equal to the first byte of a newly mapped anonymous region;
- two are `SIGKILL` / `CODESIGNING` / `Invalid Page` at the same first
  instruction fetch;
- `vm_region_64` and the crash reporter showed mappings such as `rw-/rwx` or
  `r-x/rwx`, but that apparent execute permission did **not** mean TXM had
  authorized instruction fetch.

The missing operation is external to BoxedVN. Current StikDebug's
`universal.js` defines a prepare-region call as `x16 = 1`, `brk #0xf00d`.
StikDebug catches that stop and writes through debugserver to every 16 KiB page
so iOS 26/27 TXM will accept it as executable. `CS_DEBUGGED` is a prerequisite
for this interaction, not a substitute for it. Current emulator ports then
create separate RX and RW mappings of the prepared physical pages rather than
flipping one mapping between protections.

**Implemented:**

- `BVNExecMemory` now performs exactly that universal handshake, `vm_remap`s a
  writable alias, validates permanent `r-x`/`rw-` protections, writes the
  probe through RW, and executes only through RX. All six speculative
  mmap/mprotect/MAP_JIT strategies are retired.
- The live Boxedwine allocator uses the same implementation. Both
  `Platform::writeCodeToMemory` callback forms now receive the writable alias;
  generated code pointers, calls, and cache invalidation retain the RX address.
  This also removes section 1g's process-wide `mprotect` race.
- Dual mappings are tracked and both aliases are released together.
- The six-second wrapper remains. Its timeout now diagnoses a missing or
  inactive StikDebug universal script—the only expected reason to remain
  stopped at this deliberate breakpoint—instead of talking about the retired
  strategy matrix.
- Runtime and setup text now say explicitly: enable StikDebug Advanced
  Options, long-press BoxedVN, assign `universal.js`, launch through StikDebug,
  and leave the script active.

**Verified locally:** 59 support tests and 26 pin checks pass; the complete
arm64 Debug device app builds and validates; the x86_64 simulator compile-only
target builds. Build 6 Release also builds, validates, and packages into a
smoke-tested unsigned IPA.

**Additional device evidence recovered after implementation:** build 5's
session log shows the retired matrix completed, selected
`mmap+mprotect(r-x)`, logged `About to execute the probe page`, then timed out
exactly six seconds later and returned the runtime to `failed`. That proves the
reported freeze was the first instruction fetch and proves section 1j's
timeout works on the phone. The same container confirms the 149.3 MB Wine 10
rootfs is already installed. **Build 6 itself is not verified on the phone
yet**; section 1l records that subsequent result.

## 1l. Build 6 proves JIT execution; probe immediate was encoded incorrectly (2026-08-07)

The build 6 device log proves every previously uncertain JIT step works:

- StikDebug serviced `brk #0xf00d` and prepared the requested page;
- the mappings read back as RX `r-x/rwx` and RW `rw-/rwx`;
- BoxedVN wrote through RW, invalidated the RX instruction cache, called RX,
  and returned normally—no crash and no timeout.

The probe returned `0x00000458` while BoxedVN expected `0x00004258`. This was
not a JIT failure. The hand-written first instruction was `0xD2808B08`, which
is `movz x8, #0x0458`; the intended `movz x8, #0x4258` is `0xD2884B08`.

**Fix for build 7:** the instruction is no longer a copied magic number.
`encodeMovzX()` derives it from the same `kProbeExpectedValue` used by the
comparison, and a compile-time assertion pins the resulting opcode to
`0xD2884B08`. This prevents the test code and expected value from drifting
apart again. The next device run should pass the probe and enter
`boxedmain()` for the first time.

## 1m. Build 7 reaches Boxedwine; `-nozip` was given a stray value (2026-08-07)

Build 7 passed the corrected JIT probe, entered `boxedmain()`, and loaded the
149.3 MB Wine 10 rootfs in 178 ms. That proves the complete path through JIT,
rootfs lookup, ZIP integrity, and Boxedwine startup on the physical iPhone.

The next failure was entirely in BoxedVN's synthesized argv. It passed
`-nozip 1`, but upstream `StartUpArgs::parseStartupArgs` treats `-nozip` as a
valueless switch. The unconsumed `1` therefore became the first positional
argument, producing:

```
Launching "1" ...
Could not find 1
```

**Fix for build 8:** remove the `1`. Command construction now lives in the
pure C++ `BVNLaunchArguments.cpp`, shared by the iOS runtime and host test
target. Two new tests pin the complete Wine Notepad argv and a game launch
with mount, environment, working directory, sound, and guest arguments. The
suite now has 61 C++ tests, all passing.

## 1n. Build 8 reaches Wine, then parks on its first live JIT allocation (2026-08-08)

The complete paired-device logs confirm build 8 fixed argv and crossed every
earlier boundary:

```
Loaded .../boxedwine.zip in 132 ms
Launching "/bin/wine" "notepad"
```

Two attempts remained alive on a black SDL screen with no later output. The
current live allocator explained that exact boundary: every Boxedwine 64 KiB
code block called `allocatePrepared()`, which issued another StikDebug
`brk #0xf00d`. The probe only proved one breakpoint. Wine startup immediately
introduced a potentially unbounded series of debugger stops, vulnerable to
StikDebug being background-suspended and expensive even while it remained
active.

**Fix for build 9:** the deliberate startup probe now prepares one bounded
128 MiB RX arena, creates one RW alias, executes the probe inside it, and
retains the proven mapping. `BVNExecMemAlloc` suballocates live blocks from the
arena without another debugger breakpoint. Frees are validated and coalesced;
writable-address translation only accepts active allocations; exhaustion is
logged explicitly rather than silently starting a mid-guest debugger stop.
The dependency-free `BVNRangeAllocator` has regression coverage for alignment,
reuse, coalescing, containment, exhaustion, and invalid/duplicate release.

The device logs also exposed why all JIT diagnostics disappeared before the
quoted lines: BoxedVN passed its already-open session path to Boxedwine's
`-log`, whose parser calls `createNew()` and truncates the file. Build 9 omits
that duplicate logger and relies on the existing stdout/stderr capture, so one
continuous file now retains frontend, probe, allocator, Boxedwine, and Wine
output.

**Verified locally:** 65 C++ tests and dependency pin checks pass; the complete
arm64 Release app builds and validates as build 9. Packaged unsigned IPA:
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `a02a5932485b57b0068f49df457335d500b6e2631d97b428893a973b62ff636f`.
**Not yet device-verified.**

## 1o. Build 9 renders Notepad; build 10 preserves its aspect ratio (2026-08-08)

The user left build 9 on its black startup screen and Wine Notepad eventually
appeared. This is the first complete on-device proof of the retained JIT arena,
Boxedwine ARM64 execution, the Wine 10 prefix, Wine GUI startup and SDL frame
presentation. Build 9 logs show continuing JIT allocations rather than a
deadlock: the arena was ready in roughly 0.24 seconds, while Wine continued
allocating translated blocks for more than 40 seconds before the first useful
window appeared.

The resulting 800x600 desktop was stretched independently across the iPhone's
portrait width and height, making Notepad extremely tall and narrow. SDL's
UIKit backend intentionally ignores the requested SDL window size and creates
a full-screen native window. Boxedwine had not set a renderer logical size, so
the native surface scaled the guest non-uniformly.

**Fix for build 10:** on iOS, `KNativeScreenSDL` now sets the SDL renderer's
logical size to the guest resolution after renderer creation and after every
guest mode change. SDL letterboxes that logical image into the actual window,
updates the mapping when the device geometry changes, and applies the same
transform to mouse/touch events. Other platforms and Boxedwine's existing
fullscreen scaling are unchanged. The runtime log records the logical and
output sizes when the mapping is installed.

**Verified locally:** 65 C++ tests and 26 dependency pin checks pass; the iOS
Simulator compile-check passes; the complete arm64 Release app builds and
validates as build 10. The unsigned IPA passes its unpack/revalidation smoke
test at
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `d0489357d5a9fb9e8b3c81f8cd05351669a46a3e53d77e0f2512c4919288c918`.
The aspect-ratio and input behavior still require the physical-device check.

## 1p. Build 10 exposed double-scaling, rotation and keyboard gaps (2026-08-08)

Build 10 reduced the severe portrait distortion but remained visibly
stretched, would not rotate even with device orientation lock off, provided no
feedback over the black Wine-startup surface, and offered no way to summon the
iOS software keyboard.

The remaining stretch had a concrete second cause. On iOS, SDL ignores the
requested 800x600 native window size and fills the UIKit window. Before the
new renderer logical-size mapping ran, Boxedwine's desktop code compared that
request with the portrait display-mode dimensions and independently set X and
Y scales to fit. Build 10 then correctly letterboxed an image whose draw
coordinates had already been distorted. Build 11 makes SDL's logical-size
mapping the sole iOS presentation transform: the legacy desktop scaling and
mode-change resize path are skipped, and input scale/offset remain 100%/zero.

Build 11 also sets SDL's iOS orientation hint to portrait plus both landscape
orientations, asks the new guest controller to refresh its supported
orientations, and uses the iOS 16+ UIWindowScene geometry preference to start
the guest in landscape when a scene is available. The guest continues to
adapt to the actual window geometry rather than using device-orientation
values for layout.

A passthrough UIKit overlay was attached to SDL's final Metal view. It was
intended to show a small **Starting Wine** card whose text updated with live
JIT allocation counts
(`Translated 64 code blocks`, etc.), making forward progress visible without
intercepting guest taps outside the card. The card was dismissible. A persistent
**Keyboard** button was intended to toggle SDL's real iOS text-input path;
SDL's UIKit backend
turns software-keyboard Unicode input into text events that Boxedwine's
existing X11 input path was assumed to consume. On hardware this design failed:
UIKit could not service it while `boxedmain()` owned the main thread, and the
software-keyboard event type was not actually handled. Section 1q records the
build 12 replacement.

**Verified locally:** 65 C++ tests and 26 pin checks pass; the x86_64 iOS
Simulator core compile-check passes; build 11's full simulator app builds,
installs, launches and renders the library; the complete arm64 Release app
builds and validates. The unsigned IPA passes its unpack/revalidation smoke
test at
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `0b0ea9e462202906db5dadbc0c878fa3987f61555471669d5a658cfb3a316faf`.
Guest-only behavior still requires the hardware run.

## 1q. Build 11's UIKit overlay arrived late and blocked the guest (2026-08-08)

The physical-device build 11 run narrowed the problem precisely: Wine still
took roughly 1.5–2 minutes and Notepad rendered successfully, but the UIKit
**Starting Wine** overlay did not appear until *after* Notepad was already on
screen. It then spun forever and accepted neither its close action nor other
touches, leaving the guest unusable.

That behavior follows the runtime contract. `boxedmain()` owns the main thread
for the guest session and pumps SDL events itself; UIKit's normal run loop is
not available to commit a newly attached view hierarchy or deliver `UIButton`
actions. Queueing progress changes back to the main dispatch queue made the
problem worse because those blocks could not run until SDL yielded. A UIKit
overlay can therefore be visually late, logically stale, and input-blocking
even when its view code is otherwise valid.

Build 12 removes UIKit from guest controls. `KNativeScreenSDL` now:

- draws and presents a full **STARTING WINE** screen before SDL's window is
  shown, including an activity glyph, explicit Wine-loading status,
  and an honest first-start timing message;
- ends loading automatically on the first `putBitsOnWnd` call, which XServer
  makes only for a mapped visible `InputOutput` descendant of its root;
- draws a persistent **KEYBOARD** button after loading and hit-tests it in
  SDL's 800x600 logical coordinate space, so the visual and touch mappings are
  identical and no guest touch-blocking view exists;
- starts/stops SDL's UIKit software keyboard from the SDL mouse event itself;
  and
- consumes iOS `SDL_TEXTINPUT` commits and synthesizes the corresponding ASCII
  X11 key transitions. The previous code handled physical key down/up events
  but ignored the event type used for ordinary software-keyboard characters.

UIKit retains only its scene-orientation request; it creates no guest overlay
or button. Build 12 also preserves build 11's single logical-size presentation
mapping and rotation hints.

**Verified locally:** 65 C++ tests and 26 dependency pin checks pass; the
x86_64 iOS Simulator core compile-check passes; build 12's full simulator app
builds, installs, launches and renders the library; the complete arm64 Release
app builds and validates with `CFBundleVersion` 12. The final executable
exports the presentation, SDL loading-progress and control-hit-test hooks. The
unsigned IPA passes unpack/revalidation at
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `06b81703c501f5093d5586685b9e3cd80faaaa0be27c35f88d1a5bde9dcc2cc5`.
Guest behavior still requires the hardware run.

## 1r. Build 12 loading timer exposed an SDL callback lifetime race (2026-08-08)

Build 12 reached the physical device with the correct JIT handshake and SDL
logical surface, then aborted on the loading screen just as Wine began its
first native translations. The exported log ends after JIT allocations 1–8 at
01:14:22; no `putBitsOnWnd` completion line exists, so no Wine window had been
mapped and loading was correct to remain visible.

The `.ips` report is conclusive and matches the locally retained build exactly:

- app UUID `B1DF1C33-DC9C-345A-AAE7-E53D1830E318` matches the build 12 binary
  and dSYM;
- `EXC_CRASH / SIGABRT`, not a code-signing, JIT, watchdog or memory kill;
- main-thread stack: `std::__throw_bad_function_call` →
  `KNativeInputSDL::processEvents()+236` → `doMainLoop()`; and
- the disassembly at the crash site is the unchecked `callback->pfn()` call.

Two conditions combined. Build 12's 250 ms SDL timer queued roughly 360 custom
events during Wine's 90-second pre-main-loop startup, because the main thread
could not service them yet. Separately, Boxedwine's `sdlDispatch` waited on a
`std::condition_variable` without a completion predicate. A permitted spurious
wake could clear and recycle the callback while its SDL event remained queued;
when `processEvents` eventually reached that event it invoked an empty
`std::function` and terminated the app.

Build 13 fixes both sides:

- removes the loading timer and all of its queued custom events;
- makes the loading screen explicitly static during the pre-main-loop phase;
- shows SDL's UIWindow before presenting the loading frame, because iOS Metal
  may discard a drawable committed while the window is still hidden;
- adds a per-callback `completed` predicate and loops across spurious wakes;
- validates callback events before invocation, so a malformed or stale event
  is logged rather than becoming an uncaught C++ exception; and
- preserves automatic loading dismissal on the first mapped Wine window,
  aspect-correct presentation, SDL keyboard control and ASCII text input.

**Verified locally:** 65 C++ tests and 26 dependency checks pass; the x86_64
iOS Simulator core compile-check passes; build 13's full simulator app builds,
installs, launches and renders the library; the complete arm64 Release app
builds and validates as `CFBundleVersion` 13. The unsigned IPA passes unpack
and revalidation at
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `16661a649fe1e2757c1ca475687961f90d9067016b26ffd4984a5b64233ce79c`.
The crash fix still requires the hardware run.

## 1s. Build 13 exposed a pre-event-loop SDL dispatch deadlock (2026-08-08)

Build 13 remained on a black screen for more than three minutes and never
launched Wine. Its exported log ends immediately after
`iOS guest presentation: logical 800x600 in 874x402 output`; it contains
neither `iOS guest loading screen presented` nor the `/bin/wine notepad`
launch line. The matching `.ips` is a foreground `0x8BADF00D` process-exit
watchdog report: the app did not terminate within five seconds after the hung
session was closed. It is not a new JIT crash.

The symbolicated main-thread stack is conclusive:
`BoxedWineCondition::wait` → `sdlDispatch` →
`KNativeScreenSDL::showWindow` → `recreateMainWindow`. Build 13 showed the SDL
window before presenting the loading frame, but `showWindow()` judged this
pre-loop call to be off SDL's recorded main thread and synchronously dispatched
an event. The SDL event loop had not started yet, so no code could service the
event and complete the wait. Build 12's unsafe condition-variable wait could
escape spuriously; build 13's correct completion predicate exposed the latent
startup ordering bug instead of corrupting the callback lifetime.

Build 14 keeps the callback safety fix and changes only the known startup
phase: `recreateMainWindow()` is already running on UIKit's main queue, so it
directly shows and raises SDL's native window, records it as visible, and then
presents the static loading frame. Later window operations continue to use
normal SDL dispatch. A new
`iOS guest window shown directly for pre-loop startup` log marker makes this
boundary explicit.

**Verified locally:** 65 C++ tests and 26 dependency checks pass; the x86_64
iOS Simulator core compile-check passes; the complete arm64 Release app builds
and validates as `CFBundleVersion` 14. The unsigned IPA passes unpack and
revalidation at
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `2deacd59c752ea03b49d2f1ea9aa1aecd31dbab001e8c9c4f2406a62140fd734`.
The fix requires a hardware run.

## 1t. Build 14 boots Notepad; drawable scale and rotation were stale (2026-08-08)

Build 14 proves the pre-loop deadlock is fixed: its loading screen appeared,
Wine completed startup, and the device log reached
`iOS guest startup complete: first mapped X11 window 0x1005e`. Notepad rendered,
but it looked blurry, the loading screen had been stretched horizontally after
rotation, and neither guest taps nor the SDL **KEYBOARD** button responded.

The log and screenshot identify one shared presentation/input fault rather than
a new Wine failure. SDL created its Metal renderer in portrait and reported an
output of only `402x874` on a 3x device whose landscape screenshot is
`2622x1206` pixels (`874x402` points). The SDL window lacked
`SDL_WINDOW_ALLOW_HIGHDPI`, so its drawable was one pixel per point. The phone
then changed from portrait to landscape after renderer creation, leaving both
the logical viewport and SDL's corresponding mouse-coordinate transform at
risk of using the old geometry. The log contains no crash or guest-exit line.

Build 15:

- creates the UIKit SDL window with `SDL_WINDOW_ALLOW_HIGHDPI` and
  `SDL_WINDOW_RESIZABLE`, producing a native-scale Metal drawable;
- checks the actual renderer output before each presentation and reapplies the
  800x600 logical size whenever the drawable changes, keeping pixels and input
  on the same aspect-preserving transform; and
- logs resize/size-change events plus mouse down/up coordinates at the SDL
  boundary. If input still fails, the next device log now distinguishes an
  absent UIKit touch from a wrong guest coordinate.

**Verified locally:** 65 C++ tests and 26 dependency checks pass; the x86_64
iOS Simulator core compile-check passes; the complete arm64 Release app builds
and validates as `CFBundleVersion` 15. The unsigned IPA passes unpack and
revalidation at
`build/ios-Release/DerivedData/Build/Products/Release-iphoneos/BoxedVN-unsigned.ipa`,
SHA-256 `a528e3714dcad6e4c21956eae7d68894554a47b9c1109653a13087030f5538dd`.
The fix requires a hardware run.

## 1u. Build 15 device result and build 16 lifecycle fix (2026-08-08)

The user confirmed that build 15 restores a clear, usable Notepad. Its
native-scale renderer is visibly sharper and Notepad can be interacted with in
the initial landscape session. The device report narrowed the remaining faults
to three related presentation/focus transitions:

- launching **Run Wine Notepad** while the library was portrait created SDL's
  Metal drawable first and asked UIKit to rotate afterwards, so the loading
  screen was temporarily stretched;
- turning the device while Wine was running replaced/resized that drawable and
  first-responder hierarchy during Boxedwine's blocking event loop, freezing
  the session; and
- the SDL-rendered **KEYBOARD** button reached `SDL_StartTextInput`, but that
  API depends on SDL still having a focus window. The disrupted presentation
  could leave it without one, so no UIKit keyboard appeared.

Build 16 treats this as one lifecycle problem:

- `BVNRuntimeRequestLaunch` now asks `BVNAppDelegate` to request landscape and
  waits asynchronously for both scene orientation and window bounds to settle
  before enabling SDL's event pump or entering `boxedmain()`;
- the delegate and SDL orientation hint advertise landscape only for the guest
  lifetime, preventing live portrait/landscape drawable replacement; the
  library remains rotatable and its previous orientation is requested again
  after guest exit or a failed startup;
- the **KEYBOARD** button still calls `SDL_StartTextInput`, then explicitly
  invokes `showKeyboard` on SDL's own UIKit view controller. It does not add a
  second overlay or text field, so SDL remains the sole producer of key and
  text events; and
- startup/focus boundaries now log `Landscape guest geometry settled before
  SDL startup`, `SDL UIKit software keyboard shown`, and `iOS guest software
  keyboard requested through UIKit bridge` for a decisive device diagnosis.

The reported unused blue width is not a crop introduced by the Retina fix. The
guest desktop is fixed at 800x600 (4:3) and aspect-fit on a much wider phone.
That is acceptable for the Notepad milestone, but per-game resolution/display
profiles are now explicitly tracked for visual novels rather than solving it
by distorting the framebuffer.

**Verified locally:** 65 C++ tests and 26 dependency checks pass; the x86_64
iOS Simulator core compile-check passes; the complete arm64 Release app builds
and validates as `CFBundleVersion` 16. The packaged unsigned IPA is
`build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`1597dd91d5224177b5b02fc64a306aff7f61fb4453da1ed41e7d09908dff6497`.
Hardware validation is still required.

## 1v. Build 16 proves presentation; build 17 fixes the real keyboard responder (2026-08-08)

The build 16 device log `boxedvn-20260808-022637.log` resolves the remaining
ambiguity:

- `Landscape guest geometry settled before SDL startup` precedes `boxedmain`;
- SDL is created directly at landscape `874x402` points / `2622x1206` pixels,
  with an aspect-fit 800x600 viewport, so the portrait drawable race is fixed;
- Notepad maps, receives many correctly transformed mouse events and remains
  interactive;
- each **KEYBOARD** tap reaches the control and logs both the Objective-C bridge
  and C++ success path; but no keyboard appears; and
- `Boxedwine shutdown`, exit code 1 and state `stopped` prove that this
  `boxedmain()` invocation returned instead of killing the host process.

The build 16 message `SDL UIKit software keyboard shown` was therefore a
false-positive: it was emitted after calling SDL's void `showKeyboard` method,
not after observing a first responder or UIKit keyboard notification. Reading
pinned SDL 2.32.10 shows its private keyboard `UITextField` is both hidden and
created with `CGRectZero`; `showKeyboard` silently ignores the result of
`becomeFirstResponder`.

Build 17 keeps SDL's own field and event pipeline, but makes the field usable
as a responder on the current device OS:

- retrieves SDL 2.32.10's pinned `textField` ivar with the Objective-C runtime;
- attaches it to the active SDL controller, sets a 1x1 frame, `hidden = NO`
  and `alpha = 0.01`, and makes the SDL guest window key;
- invokes SDL's normal controller bookkeeping, then directly retries and
  verifies `textField.isFirstResponder`;
- treats a failed responder transition as a real bridge failure instead of
  logging success; and
- separately logs `UIKeyboardDidShowNotification` / did-hide, distinguishing
  focus acceptance from the asynchronous keyboard actually appearing.

No proxy text field was added. SDL still owns composition, Backspace, Return
and `SDL_TEXTINPUT`, avoiding a temporary ASCII-only path that would have to be
discarded for Japanese visual novels.

**Verified locally:** device Release and simulator compile-check pass; 65 C++
tests and 26 dependency checks pass; the validated `CFBundleVersion` 17 IPA is
`build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`3c6b2e820f89879fb8c7ff2de6aea3a22566d1b5add03debd3bf63a4d1227e6f`.
Hardware validation is required.

## 1w. Build 17 proves focus was not enough; build 18 repairs scene ownership (2026-08-08)

The build 17 device log `boxedvn-20260808-024410.log` is decisive. Notepad
runs, and a keyboard tap reports
`SDL keyboard show: keyWindow=yes attached=yes hidden=no firstResponder=yes`,
but never reports `UIKit confirmed software keyboard did show`. Later taps can
resign the responder and receive did-hide notifications. The field, button,
SDL bridge, key window and synchronous focus transition therefore all work;
the absent asynchronous presentation belongs to UIKit's scene layer.

Source inspection against the iOS 26 SDK found the common cause shared with
the still-broken physical-device document picker:

- `Info.plist` explicitly omitted `UIApplicationSceneManifest`;
- the app delegate manually created a SwiftUI window rather than using
  `UIWindowSceneDelegate`; and
- SDL 2.32.10 creates its guest `UIWindow` using `initWithFrame:`, deprecated
  on iOS 26, leaving it outside the library window's scene.

Build 18 makes one coherent lifecycle:

- `BVNSceneDelegate` owns the library window and creates it with
  `initWithWindowScene:`;
- after `SDL_CreateWindow`, `BVNAttachGuestWindowToScene` assigns SDL's native
  window to that same active scene before Metal renderer creation;
- the existing focusable SDL text field remains the sole text-input pipeline;
- picker completion/failure callbacks are logged; and
- a Library section imports game ZIPs already copied to BoxedVN Documents,
  bypassing the remote document picker entirely.

**Verified locally:** the arm64 Release app builds and validates as build 18;
the scene delegate and bridge symbols survive static-link dead stripping; the
built plist contains the single-scene configuration; 65 C++ tests and 26
dependency checks pass. An iOS 26.3 Simulator launch rendered the Library and
logged both scene markers (`scene-owned library window created` and
`UIWindowScene connected to BoxedVN library`); the new direct game-ZIP section
is visible.
The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`82fc503fc6cf56c6007adba7838f6efdd5347809d6cc9fcabd05753b26a67cc3`.
Hardware validation is still required for keyboard appearance and
system-picker completion.

## 1x. Build 18 proves UIKit still fails; build 19 guarantees input and import (2026-08-08)

The build 18 device log `boxedvn-20260808-030652.log` proves all intended scene
work happened: the library scene connected, SDL attached its window to the
same scene before renderer creation, Wine started, and Notepad remained
interactive. Nevertheless, every keyboard request reached
`keyWindow=yes attached=yes hidden=no firstResponder=yes` without a single
keyboard did-show notification. The system picker also still would not
complete any selection. Scene ownership was real and necessary, but not the
last cause of either device-only UIKit failure.

Build 19 stops making progress depend on those services:

- **Guest typing:** the SDL-rendered **KEYBOARD** button now opens a five-row
  touch QWERTY overlay rendered and hit-tested in the same 800x600 coordinate
  space as Wine. It sends X11 key transitions directly for letters, digits,
  Shift, Space, Backspace, Enter and arrows. The native keyboard is still
  requested using a normal-sized, alpha-1 SDL text field for future IME input,
  but it is no longer required to type.
- **File import:** SwiftUI's open-in-place `.fileImporter` is replaced with a
  `UIDocumentPickerViewController` configured with `asCopy: true` and an
  explicit delegate. Files now performs an import copy and returns a local URL
  rather than attempting the security-scoped hand-off that never completed.
- **Guaranteed fallback:** rootfs and game ZIPs copied to **On My iPhone →
  BoxedVN** remain selectable directly inside the app without any picker.

**Verified locally:** build 19 compiles and validates for arm64; the full iOS
26.3 Simulator app builds, installs, launches and renders the Library; 65 C++
tests and 26 pin checks pass. Wine and the built-in guest keyboard still
require the physical ARM64/JIT test. The validated and smoke-tested unsigned
IPA is `build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`e851315c47126363d7fe60e41fb453f0a9429c3965b905ee5947cbb563a99979`.

## 1y. Build 19 proves typing and reaches Saya; build 20 fixes launch context and attempts the startup timeout (2026-08-08)

The user confirmed that build 19's SDL QWERTY overlay types into Notepad. This
closes basic software input as a blocker, although the compact overlay still
needs polish and does not provide Japanese IME composition.

The first imported-game test launched the English `Saya_en.exe` and reached a
Wine error dialog. This is meaningful progress: the game ZIP, PE32 discovery,
D: mount, x86 translation and Windows application entry point all worked. The
build 19 log also exposed two concrete problems rather than a generic graphics
failure:

- the command launched `d:\Saya_en.exe` with no `-w`, so the process inherited
  Boxedwine's Linux working directory instead of the folder containing its
  data; and
- after JIT allocation #320, startup stopped for 94.6 seconds immediately
  before Wine reported that `Services\\winebth` could not create its optional
  Bluetooth driver. Rootfs ZIP loading took only 172 ms and the StikDebug
  handshake about 200 ms, so neither was responsible for the two-minute wait.

Build 20 addresses both across all games:

- an empty manifest working-directory setting now means the selected
  executable's parent. Root-level executables use the mounted D: root, while
  nested executables infer their subdirectory. An explicit setting still wins;
- Wine launches default to
  `WINEDLLOVERRIDES=winebth=;ddraw=n,b`. This was intended to disable the
  unsupported driver while preserving native-then-builtin DirectDraw fallback;
  section 1z records that the device disproved this part; and
- opening the working SDL keyboard no longer also starts SDL/UIKit text input.
  The direct X11 overlay does not need it, and the redundant hidden responder
  caused the glitchy double show/hide transitions seen on the device.

**Verified locally:** 66 C++ tests and 26 dependency/pin checks pass. The
complete arm64 Release app builds and validates as `CFBundleVersion` 20. Its
packaged unsigned IPA is `build/artifacts/BoxedVN-unsigned.ipa`, SHA-256
`281d73152a3bec95b60f31754e0ca3eda4132894ca35244fcec7375a30553ca9`, and
passes unpack/revalidation. The physical startup/Saya result is still pending;
do not claim the 95-second reduction or game fix until build 20's log proves
them. The garbled dialog also records a separate compatibility gap: the Wine
10 root defaults to English ACP 1252 and contains no CJK font, so a future
Japanese locale/font profile will be required even when an English translation
can otherwise run.

## 1z. Build 20 identifies Saya's GLX boundary; build 21 adds a software 2D prefix profile (2026-08-08)

The build 20 device log proves the working-directory fix: Saya launched as
`d:\\Saya_en.exe` with `-w /home/username/.wine/dosdevices/d:/`. It translated
past JIT block 768 and entered WineD3D graphics initialisation. The final line,
`Uknown int 99 call: 2897`, is not a JIT allocation failure. In the generated
Boxedwine GL table, `LAST_EXT` is 2896 and index 2897 is `kXChooseVisual`, so
Wine made its first `glXChooseVisual` request against an iOS core compiled with
no GL callback table. The static loading renderer remained visible, making the
fatal graphics boundary look like a translation hang.

The same log disproves build 20's Bluetooth workaround. Despite
`WINEDLLOVERRIDES=winebth=;ddraw=n,b`, Wine still attempted the kernel service,
paused about 58 seconds between JIT blocks 320 and 384, and logged the same
`Services\\winebth` failure. A DLL load-order environment variable is not the
right control for a service-manager timeout.

Build 21 prepares the writable Wine prefix before the JIT probe and guest
launch. On first use it extracts only `user.reg` and `system.reg` from the
read-only rootfs ZIP into the game's existing overlay. It atomically and
idempotently applies:

- `Software\\Wine\\Direct3D / DirectDrawRenderer = "gdi"` for imported games,
  routing classic DirectDraw output through Wine's CPU/GDI framebuffer instead
  of WineD3D's unavailable GLX path; and
- `System\\ControlSet001\\Services\\winebth / Start = 4` for every Wine
  prefix, disabling the unsupported Bluetooth service at its real startup
  policy rather than relying on a DLL override.

This is a compatibility profile for 2D games, not an accelerated renderer.
Direct3D/OpenGL titles can still reach the missing GL boundary. The repository's
old `source/opengl/es` translator is explicitly experimental and incomplete;
the next accelerated-graphics step needs a deliberate GLES/Metal translation
plan rather than a build flag alone.

**Verified locally:** 70 C++ tests and 26 dependency/pin checks pass, including
an end-to-end synthetic rootfs ZIP extraction/registry test and idempotent
second preparation. The complete arm64 Release app builds and validates as
`CFBundleVersion` 21. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build21-unsigned.ipa`, SHA-256
`5a7446474fb08c645ea0d7d3c48235577150a501736ee36c242087c41a9a177c`.
The GDI result and actual startup-time reduction still require the physical
device; do not claim Saya runs until that evidence exists.

## 1aa. Build 21 device evidence; build 22 fixes the actual Wine 10 policies (2026-08-08)

The build 21 device log (`boxedvn-20260808-041129.log`) is decisive. Prefix
preparation completed before launch, but Saya again stopped after JIT block
768 with the GLX function table and `Uknown int 99 call: 2897`. Wine also
attempted `winebth` and paused 55.5 seconds between JIT blocks 384 and 448.
This was not a registry-overlay precedence failure: the log proved the policy
ran, and source inspection of the exact upstream Wine 10.0 tag explains why
each value was ineffective.

- Wine 10 no longer reads `DirectDrawRenderer`. Its `wined3d` configuration
  reads `Software\\Wine\\Direct3D / renderer`; values `gdi` and `no3d` both
  select `WINED3D_RENDERER_NO3D`. Build 22 writes `renderer="gdi"` for
  imported games, so classic DirectDraw can use the CPU-backed path without
  asking Boxedwine for GLX.
- Wine 10's service manager auto-starts a service when any enumerated root PnP
  device names it, regardless of the service `Start` value. `wineboot`
  creates `ROOT\\WINE\\WINEBTH`, and its device `Service="winebth"` made
  build 21's `Start=4` irrelevant. Build 22 retains `Start=4` and clears that
  root device's `Service` association before wineserver starts. Wineboot sees
  the existing device and does not reinstall it.

Build 22 also updates Boxedwine's own `BoxedContainer::isGDI/setGDI` helpers to
use the current `renderer` key, preventing later settings UI work from writing
the obsolete value again. The prefix changes remain atomic and idempotent and
apply to an existing Saya import; no re-import is required.

**Verified locally:** the full `tests-only` suite passes, including extraction,
both registry mutations, and an idempotent second preparation. The complete
arm64 Release app builds and validates as `CFBundleVersion` 22. The
smoke-tested IPA is `build/artifacts/BoxedVN-build22-unsigned.ipa`, SHA-256
`4c16d9fbfc15cf8804e752b177052911462548e13c9ba10ae6092d8d5422241c`.
This was the pre-device evidence boundary; section 1ab records the subsequent
Build 22 device result and supersedes it.

## 1ab. Build 22 reaches Saya's launcher; build 23 removes the next JIT ceiling (2026-08-08)

The two build 22 device logs (`boxedvn-20260808-134757.log` and
`boxedvn-20260808-135059.log`) prove the corrected prefix policy works:
`winebth` no longer errors, Wine selects its no-3D renderer, and Song of
Saya's launcher renders and accepts the Start Game click. They also correct an
earlier diagnosis: the generic startup pause remains even without winebth.
The measured gaps between JIT allocations 384 and 448 are 55.089 and 56.752
seconds, so that time cannot honestly be credited to the removed Bluetooth
failure.

Start Game reaches the engine's D3D9 initialization. Wine reports that none of
the requested feature levels is supported by the current no-3D backend, the
engine then reads through null at `0x7F3D1CD6`, and Wine opens its debugger.
While starting that debugger BoxedVN reaches JIT allocation 1088 and exhausts
the original 64 MiB arena. The apparent debugger freeze therefore combines a
real guest fault with a separate host-code-capacity failure.

Build 23 raises the single StikDebug-prepared dual-mapped arena to 128 MiB.
Preparing another region after the guest begins would require an unsafe live
debugger stop, so one larger up-front arena is the correct bounded design. It
also adds `WINEDEBUG=+timestamp,+service` by default (without overriding a
per-game value) so the next exported logs can identify the remaining fixed
startup delay. This build does **not** claim to make Saya render: the D3D9
engine still needs an accelerated graphics translation backend.

A compile experiment also tested the dormant 2016 `source/opengl/es` shim.
It is not compatible with Boxedwine 26R2: the current GL marshaler requires a
desktop extension/constant surface the old shim does not define, and its
`GL_FUNC` rewrite generates nonexistent ES extension symbols. Those trial
changes were removed. Source inspection found a substantially better maintained
route already in Boxedwine 26R2: generated guest Vulkan forwarding,
`KVulkanSDL`, built-in DXVK DLL selection, and an upstream macOS target that
uses MoltenVK. The next accelerated proof should port that path to a statically
linked iOS MoltenVK and SDL's UIKit Vulkan surface, not revive the dead GL shim.

**Verified locally:** 71 C++ tests and both CTest entries pass. The complete
arm64 Release app builds and validates as `CFBundleVersion` 23. The
smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build23-unsigned.ipa`, SHA-256
`9bcb54e3ed300fb4af5f108677924fd0fb85a3094c6ff771667d3892cb368389`.
This was the pre-device evidence boundary; section 1ac records the subsequent
build 23 device result and supersedes it. No Saya gameplay claim is made.

## 1ac. Build 23 proves the larger arena and isolates the startup stall; build 24 removes it (2026-08-08)

The build 23 device logs (`boxedvn-20260808-141343.log` and
`boxedvn-20260808-141627.log`) prove the 128 MiB arena fixes the second,
host-side failure. Saya continues beyond allocation 1088, WineDbg fully
attaches, and Program Error Details displays the guest backtrace. The fault is
a null read in `mvware` reached from `saya_en` after WineD3D reports that the
no-3D adapter supports none of the requested D3D feature levels. This is now a
clean accelerated-graphics boundary, not a frozen debugger or exhausted JIT
arena.

The same logs precisely identify the generic launch delay. Both Notepad and
Saya finish NDIS startup at Wine timestamp ~42, then remain idle until
`Winedevice2` starts at ~98 and queries the service whose display name is
`Wine HID bus`: a repeatable 56–57 second root PnP/HID enumeration gap.
Boxedwine supplies iOS touch and the overlay keyboard independently through
SDL and X11, so build 24 disables `Services\\winebus` and clears the
`Enum\\ROOT\\WINE\\WINEBUS` service association. Wine 10 requires both
changes because its root-device scan ignores `Start=SERVICE_DISABLED`.
Windows raw-HID/game-controller support is intentionally unavailable under
this policy; it can become a per-game compatibility option when controllers
are implemented. Standard pointer and keyboard input should be unchanged.

Build 24 also removes the temporary `WINEDEBUG=+timestamp,+service` default.
An explicitly configured per-game `WINEDEBUG` value is still preserved. The
prefix patch runs before every Wine launch and is idempotent, so existing
rootfs and game imports require no reimport.

**Verified locally:** 71 C++ tests and both CTest entries pass. The arm64
Release app validates as `CFBundleVersion` 24. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build24-unsigned.ipa`, SHA-256
`3901e651e95973b959ac7165617422d52bed9dbbb146085a919eb2df413712b0`.
The startup-time reduction still requires physical-device confirmation; it is
not recorded as proven until that result arrives.

## 1ad. Build 24 falsifies the HID diagnosis; build 25 implements DXVK over Metal (2026-08-08)

Build 24 device logs (`boxedvn-20260808-144238.log` and
`boxedvn-20260808-144533.log`) show the same cold-start gap after winebus was
disconnected: Notepad pauses 57.508 seconds between JIT allocations 384 and
448, and cold Saya pauses 61.283 seconds. The first log then proves an
important distinction: after Notepad exits, launching Saya in the same app and
Boxedwine process reaches allocation 1024 in 0.321 seconds and its launcher in
seconds. The expensive work is reusable process-wide Wine/Boxedwine cold
initialization, not the removed HID root association. Build 25 repairs existing
prefixes to `winebus Start=3` and `Service=winebus`, preserving future
raw-HID/controller support.

Build 25 implements the first accelerated game path end to end:

- pins Khronos MoltenVK 1.4.2's official iOS arm64 static package by URL and
  SHA-256 and records its Apache-2.0 notice;
- enables Boxedwine 26R2's generated Vulkan marshaler and `KVulkanSDL` with
  `BOXEDWINE_VULKAN`;
- links MoltenVK into the app archive, force-loads `vkGetInstanceProcAddr` for
  SDL's loader, and links IOSurface;
- maps iOS surfaces to `VK_EXT_metal_surface`, not macOS's surface extension;
- starts imported games with `-dxvk 1`, using rootfs DXVK 2.5.2.

The intended route is D3D9 → DXVK → guest winevulkan → Boxedwine's Vulkan ABI
→ SDL UIKit `CAMetalLayer` → MoltenVK → Metal. The device binary exports
`vkGetInstanceProcAddr` and contains MoltenVK and `KVulkanSDL`; device behavior
is the remaining evidence boundary.

**Verified locally:** 71 C++ tests and both CTest entries pass. The arm64
Release app validates as `CFBundleVersion` 25. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build25-unsigned.ipa`, SHA-256
`42d0057f1875992dd9d16bfff302779c64850ce334f082780f1c5fc817c25d6e`.

## 1ae. Build 25 reaches Metal but DXVK rejects it; build 26 uses WineD3D Vulkan (2026-08-08)

The build 25 device log `boxedvn-20260808-150904.log` turns the previous
graphics hypothesis into concrete evidence. MoltenVK 1.4.2 initializes,
enumerates the Apple A19 GPU and reports 5461 MiB of GPU memory. Boxedwine's
guest Vulkan bridge is therefore live through Metal. DXVK 2.5.2 then reports
`Minimum required feature level D3D_FEATURE_LEVEL_9_1 not supported`.
Upstream DXVK's device-feature gate requires geometry shaders and
`VK_EXT_transform_feedback`; MoltenVK exposes neither on this device. This is
an API capability mismatch, not a RAM shortage and not a JIT failure.

The same log then exposes a second, independent blocker:
`wined3d_dll_init Disabling 3D support.` Builds 22–24 wrote
`Software\\Wine\\Direct3D / renderer="gdi"` into the persistent Saya prefix.
Build 25 enabled DXVK but did not remove that saved Wine policy, so when DXVK
failed, WineD3D deliberately selected its no-3D adapter and the game repeated
the null feature-level fault.

Build 26 changes the accelerated game route rather than repeatedly tuning an
incompatible DXVK build:

- imported games no longer receive `-dxvk 1`;
- prefix preparation idempotently repairs existing games to Wine 10's
  `renderer="vulkan"`, so no reimport is required;
- D3D9 uses WineD3D → guest winevulkan → Boxedwine's Vulkan marshaler →
  MoltenVK → Metal;
- the pinned Wine 10 rootfs was inspected directly and contains builtin
  `d3d9.dll`, `wined3d.dll`, `winevulkan.dll` and their Unix-side modules;
  its WineD3D binary contains the `Using the Vulkan renderer.` path;
- Boxedwine's Vulkan resolver retries promoted `...KHR` instance procedures
  under their Vulkan core names, which WineD3D needs when it requests Vulkan
  1.0 plus the KHR physical-device property/feature extensions;
- the home screen now reads the entitlement from the running signed process,
  not from the source plist, and shows both its signed state and the current
  `os_proc_available_memory()` headroom. It queries Security.framework first
  and falls back to the signed entitlement blob returned by `csops`.

**Verified locally:** 72 C++ tests and both CTest entries pass. The arm64
Release app validates as `CFBundleVersion` 26. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build26-unsigned.ipa`, SHA-256
`f1eab7d49846b85de2180f64b3ac80c2ab8eeb194872acac52e3ab2a0710eb37`.
The WineD3D Vulkan frame still requires physical-device evidence. Build 26's
first post-GetMoreRam signature result is recorded in section 1af.

## 1af. Build 26 proves the signing and reinstall boundaries; build 27 guards the JIT trap (2026-08-08)

The build 26 device logs and crashes are conclusive:

- all three sessions report `Standard limit; 3.29 GB available before process
  limit` and `The installed app's signed entitlement is not enabled` after the
  user applied GetMoreRam;
- the installed identifier is the signer-generated
  `org.boxedwine.boxedvn.FMHU3Z2423`, so reinstalling/re-signing produced a new
  StikDebug target;
- both `.ips` files are `EXC_BREAKPOINT / SIGTRAP` at
  `stikDebugPrepareRegion`, specifically the deliberate `brk #0xf00d` with
  immediate 61453. Saya and Notepad both stop immediately after the log says
  the 128 MiB arena is being requested. Neither Wine nor Vulkan starts.

This is not jetsam, memory exhaustion, WineD3D or a game crash. `CS_DEBUGGED`
survived, but StikDebug's `universal.js` was not servicing the newly installed
app's handshake. The increased-memory entitlement also did not survive the
final provisioning/signing step; changing the source entitlement alone cannot
override the profile authorized by iOS.

Build 27 wraps only the synchronous universal handshake with a temporary
SIGTRAP recovery point. When StikDebug handles the Mach exception, execution
continues exactly as before. When an unassigned/stopped script lets the trap
fall through to the app, BoxedVN now unmaps the unused arena and presents a JIT
setup error instructing the user to reassign the newly installed app instead
of allowing iOS to terminate the process. Unrelated traps retain fatal default
behavior.

**Verified locally:** 72 C++ tests, both CTest entries and 30 dependency-pin
checks pass. The arm64 Release app validates as `CFBundleVersion` 27. The
smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build27-unsigned.ipa`, SHA-256
`a571f856253ed72a61ac7c8b327f630ad24aecf454362854f182d2f0360037c5`.
The recovery branch itself needs the physical-device negative test described
below because the Intel host and simulator cannot execute ARM64 `brk`.

## 1ag. Build 27 proves WineD3D/MoltenVK; build 28 repairs destroyed iOS surface views (2026-08-08)

After `universal.js` was assigned to the reinstalled app, device logs
`boxedvn-20260808-154940.log` and `boxedvn-20260808-155231.log` prove the build
27 JIT path is healthy again. Notepad maps its first X11 window, renders and
shuts down normally. Saya is no longer stuck at a JIT allocation count:

- Wine logs `Using the Vulkan renderer.`;
- Boxedwine resolves the promoted Vulkan KHR aliases;
- MoltenVK enumerates the Apple A19 GPU and creates a `VkDevice` with eight
  extensions;
- WineD3D creates swapchains sized 113x2 and 794x568;
- the latter device, physical device and instance are then destroyed without
  a Wine exception or process crash, while the phone remains black.

Wine 10's `validate_state_table` messages are diagnostics only:
`compile_state_table()` returns success after validation. They do not explain
the presentation loss. Inspection of the pinned SDL2 2.32.10 UIKit source
does. Every `SDL_Vulkan_CreateSurface()` allocates an
`SDL_uikitmetalview`, pushes it into the SDL window's private `views` stack and
replaces the controller's visible view. SDL explicitly documents that it has
no public Vulkan surface-destroy hook, so destroying the host
`VkSurfaceKHR` never removes that UIKit view. Wine's dead probe layer therefore
continues covering Boxedwine's live X11 renderer.

Build 28 adds the missing lifecycle at Boxedwine's Vulkan boundary:

- `KVulkan` receives a destroy callback paired with surface creation;
- `KVulkanSDL` keeps surfaces in creation order with their X11 windows;
- the Objective-C++ bridge associates each surface with the exact UIKit view
  SDL just installed and detaches it through SDL's pinned
  `setSDLWindow:nil` bookkeeping when Wine calls `vkDestroySurfaceKHR`;
- the preceding live Vulkan view/fake-fullscreen window is restored when one
  remains; when none remain, Boxedwine recreates its normal SDL renderer and
  X11 compositor;
- tiny Wine helper surfaces below 320x200 no longer replace the configured
  800x600 global display and input coordinate space;
- surface IDs, X11 sizes, UIKit view registration/detachment and compositor
  restoration are logged for the next device run.

**Verified locally:** the arm64 Release app validates as `CFBundleVersion` 28;
72 C++ tests, both CTest entries and 30 dependency-pin checks pass. The
smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build28-unsigned.ipa`, SHA-256
`5871674259ba76531ee60cb684aea909a008fa6b719be768e191506e3df7e811`.
UIKit/Vulkan surface destruction still requires physical-device validation
because the simulator cannot run Boxedwine's ARM64 JIT or MoltenVK guest path.

## 1ah. Build 28 proves teardown but restores a live 113x2 probe; build 29 makes probes offscreen (2026-08-08)

The build 28 log `boxedvn-20260808-162043.log` removes the remaining
ambiguity. WineD3D successfully creates the Apple A19 Vulkan device. At first
X11 mapping it creates a 113x2 surface, and BoxedVN registers that surface as
the first visible `SDL_uikitmetalview`. Wine then creates a 794x568 surface,
destroys it immediately, and build 28 correctly detaches its exact UIKit view.
The log says one surface remains, however: the 113x2 view. Restoring that live
but non-display probe over X11 is the black screen. No Wine exception, JIT
stall, memory failure or MoltenVK failure appears.

Build 29 separates Vulkan existence from Vulkan presentation:

- X11 windows below 320x200 receive a retained, unattached `CAMetalLayer` and
  a real `VK_EXT_metal_surface`, so Wine can create its probe swapchain without
  changing SDL's visible controller view;
- only display-sized surfaces enter the UIKit view stack and become
  Boxedwine's fake-fullscreen window/input target;
- destroying the last display-sized surface restores the SDL X11 compositor
  even when an offscreen helper surface remains alive;
- helper destruction releases its retained Metal layer without touching SDL
  or UIKit presentation;
- native shutdown now drops `KVulkanSDL` before the session's SDL screen, so a
  later accelerated guest cannot target the prior guest window; abandoned
  helper layers and UIKit diagnostic associations are released;
- new logs explicitly say `offscreen helper` and report whether X11 was
  restored while helpers remain.

This is locally compiled and packaged, not device-proven. The design avoids a
second hidden SDL window because SDL UIKit supports only one window and clears
`SDL_WINDOW_HIDDEN`; using one would risk replacing SDL's key-window and
keyboard bookkeeping.

**Verified locally:** the arm64 Release app validates as `CFBundleVersion` 29;
72 C++ tests, both CTest entries and 30 dependency-pin checks pass. Physical
device acceptance is the visible launcher/main menu and the Build 29 markers
in `docs/TESTING_IOS.md`. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build29-unsigned.ipa`, SHA-256
`87aa8e8eeec8d9119a0531458415fd469e6160c7188d853522b15ca763972462`.

## 1ai. Build 29 exposes the real first-frame fault; build 30 makes it actionable (2026-08-08)

The build 29 device log `boxedvn-20260808-164144.log` proves the offscreen
helper design. Saya's blue X11 desktop and launcher return after the 113x2
capability surface remains alive. Pressing Start Game gets substantially
farther: WineD3D performs D3D11 format checks and shader parsing, SDL opens
stereo audio (44.1 kHz requested, 48 kHz obtained), and MoltenVK creates three
successive 1280x960 swapchains. The first two presentation views are detached
and X11 is restored correctly. The third remains live when Wine reports an
unhandled write fault at `0x03BE0000`, EIP `0x7F444DDF`, thread `0024`.
Consequently the final Metal view is black and obscures WineDbg. This is no
longer the helper-surface regression or a JIT stall.

MoltenVK logs `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling
primitive restart` immediately beforehand. Its 1.4.2 source documents this as
a warning because Metal always enables primitive restart; it is not a failed
pipeline result and is not patched speculatively.

Build 30 adds two bounded diagnostics instead:

- when a mapped X11 window is titled `Wine Debugger` or `Program Error`,
  `KVulkanSDL` detaches every visible iOS presentation layer newest-first,
  restores the SDL X11 compositor, and leaves the Vulkan objects valid for
  Wine's normal teardown; both X11 title-property write paths retrigger the
  check when Wine sets its title after mapping;
- Vulkan mappings retain allocation size, offset, host pointer, owner process,
  guest address and mapped length. The final 32 unmapped ranges are retained,
  and each unique guest fault is classified against live data, guard pages and
  recent mappings. This determines whether `0x03BE0000` is an overrun, stale
  device-memory pointer, or unrelated Mware/game-engine address without
  changing memory semantics.

**Verified locally:** the new native sources compile for iOS arm64; all 72 C++
tests, both CTest entries and all 30 dependency-pin checks pass. The validated
arm64 app is `CFBundleVersion` 30. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build30-unsigned.ipa`, SHA-256
`8fad2260e5dd4bf887ba6c8fdd4896628404556e1e81ce305a0bdbec1353490e`.
Physical-device validation is required for WineDbg visibility and the address
classification.

## 1aj. Build 30 rules out resolution/Vulkan mappings; build 31 isolates mvware (2026-08-08)

The two build 30 device logs `boxedvn-20260808-170551.log` and
`boxedvn-20260808-170843.log` are deterministic despite different launcher
resolutions. Both reach the visible launcher, create WineD3D/MoltenVK devices,
parse D3D11 shaders, open audio and cycle their first two presentation
surfaces correctly. Both then fail before a game frame with a write to
`0x03BD0000` at guest EIP `0x7F444DDF` on thread `0024`. The new tracker says
that address is outside all live Vulkan mappings and all retained recently
unmapped ranges. Wine prints its unhandled-page-fault line, then WineDbg
immediately suffers a nested read fault (`0xE4860004`/`0xE48C0004`) before it
maps `Wine Debugger` or `Program Error`. That explains why build 30's
title-based reveal never fires. It also rules out game resolution and the
known Vulkan mapped-memory lifetime/guard-page classes.

An earlier complete WineDbg backtrace names `mvware` at the top of the same
Saya failure. Build 31 introduces a repeatable, narrow compatibility profile:

- `BVNLaunchArguments` recognises `Saya_en.exe` case-insensitively in either
  the requested executable or a wrapper argument and emits
  `-interpreterModule mvware` before the guest command;
- `StartUpArgs` carries repeated module fragments into `KSystem`;
- `NormalCPU` resolves a decoded block's mapped module before taking the code
  cache lock and marks only matching blocks `OP_FLAG_NO_JIT`. Wine startup,
  WineD3D and all other game modules keep the ARM64 JIT, avoiding a global
  interpreter's cold-start cost;
- the first matching block logs `Compatibility CPU activated` so the device
  run proves the profile actually matched;
- Wine's TTY `Unhandled page fault` line now reveals X11 even if WineDbg fails
  before creating a window; and
- Saya-profile faults record a bounded snapshot with PID/thread, access kind,
  EIP, mapped module/offset, general registers and 16 instruction bytes. The
  limit is 64 and the logger is inactive for Notepad/ordinary launches.

This is a compatibility experiment backed by the current evidence, not a
claim that Saya reaches its menu. If build 31 still faults but never logs
`Compatibility CPU activated`, the snapshot's module name supplies the exact
fragment to use. If it activates and the same failure remains, the interpreter
has ruled out ARM64 code generation and the snapshot becomes the next engine/
WineD3D debugging target.

**Verified locally:** the complete native core/runtime and Release app compile
for iOS arm64; all 74 C++ tests, both CTest entries and all 30 dependency-pin
checks pass. The validated app is `CFBundleVersion` 31. The smoke-tested
unsigned IPA is `build/artifacts/BoxedVN-build31-unsigned.ipa`, SHA-256
`c307b60ff3be4503912d221270869f2bbbb53e86c1c0b8aeb3b9b3edd50fb976`.
Physical-device execution remains required.

## 1ak. Build 31 restores an interactive X11 failure screen; build 32 targets the real PE range (2026-08-08)

The build 31 device log `boxedvn-20260808-173247.log` proves the recovery path
works. When Saya faults, BoxedVN detaches the live 800x600 Metal presentation
surface and restores the SDL X11 compositor. The screenshot consequently has
blue side areas and a black 4:3 guest viewport instead of a full-screen black
layer. The built-in keyboard remains interactive and its key events continue
to be logged. This is a failed game process on a functioning frontend/input
loop, not an app or renderer hang.

The CPU experiment did not actually run. The command correctly contained
`-interpreterModule mvware`, but there is no `Compatibility CPU activated`
marker. The fatal snapshot resolves EIP `0x7F444DDF` as `Unknown`, because
Wine's anonymously mapped PE image does not appear in Boxedwine's Linux
file-mapping names. The snapshot also gives the exact failing instruction and
state: `F3 A4` (`REP MOVSB`), destination `EDI=0x03BD0000`, source
`ESI=0x03BCFFFC`, and invalid count `ECX=0xFFE1D4F0`. The same write fault and
nested WineDbg fault then occur. Build 31 therefore tested recovery and
diagnostics, but did not rule the ARM64 translator in or out.

Build 32 adds repeated `-interpreterRange START-END` support through
`BVNLaunchArguments`, `StartUpArgs`, `KSystem` and `NormalCPU`. Saya selects
the inclusive-start/exclusive-end range `0x7F300000–0x7F500000`, which covers
both observed WineDbg/mvware EIPs (`0x7F3D1CD6` and `0x7F444DDF`). Range checks
run before module-name resolution, and Saya no longer supplies the ineffective
name selector, so ordinary Wine startup has no added mapped-file lookup. Only
blocks decoded inside those 2 MiB receive `OP_FLAG_NO_JIT`; Wine, WineD3D and
all code outside it keep ARM64 JIT. The first hit logs the exact range and EIP.

This remains an evidence-bounded compatibility test. If build 32 reaches a
frame/menu, the JIT translation of preceding mvware code caused the corrupt
copy state. If the identical `REP MOVSB` state survives after the activation
marker, the decoded interpreter has ruled that out and the investigation moves
to the guest engine/WineD3D call state rather than widening interpreter use.

**Verified locally:** the complete iOS arm64 native core and Release app
compile; all 74 C++ tests, both CTest entries and all 30 dependency-pin checks
pass. The validated app is `CFBundleVersion` 32. The smoke-tested unsigned IPA
is `build/artifacts/BoxedVN-build32-unsigned.ipa`, SHA-256
`5f3144495fa64aa72a8a09b07595c51bbc566390a55588bad5deb8885055a61e`.
Physical-device execution remains required.

## 1al. Build 32 passes the engine fault; build 33 exposes the real frame boundary (2026-08-08)

The two build 32 logs `boxedvn-20260808-174955.log` and
`boxedvn-20260808-175203.log` are deterministic and show forward progress, not
a return of the old black-screen bug:

- `-interpreterRange 7f300000-7f500000` is present and activates at
  `7F444018` in both runs;
- the former fatal `REP MOVSB` at `7F444DDF` does not recur;
- the 113x2 capability surface remains offscreen, the launcher surface is
  destroyed cleanly, and the X11 compositor is restored;
- Start Game initializes D3D11 shaders, opens 44.1/48 kHz stereo audio, creates
  two transient game swapchains and a final 1024x768 or 800x600 swapchain;
- the final log line is PNG loading after that final surface. There is no
  WineDbg window or unhandled engine exception.

The full black picture came from SDL installing the final opaque
`SDL_uikitmetalview` at surface creation. Build 32 had no observation at the
actual `vkQueuePresentKHR` boundary, so it could not distinguish a healthy
engine still preparing its first frame from a successful presentation whose
contents were black.

Build 33 adds that missing boundary and fixes the misleading UI state:

- `vkCreateSwapchainKHR` records each host swapchain's owning surface in
  `KVulkanSDL`; destroy removes the mapping;
- the first queue submission is logged once, and each surface's first present
  attempt (plus any failed attempt) records its Vulkan result;
- a presentation Metal view starts with the Wine blue background and a native
  animated `STARTING GAME / WAITING FOR THE FIRST VIDEO FRAME` overlay;
- the first successful or suboptimal queue present dispatches one main-thread
  callback that removes the overlay. Later frames have no UIKit callback or
  added steady-state work.

This is both a user-facing correction and the next high-value diagnostic. If
the indicator remains, the guest never presented. If it disappears and black
remains, UIKit/surface selection is exonerated and the next work is guest
rendered content, shader output or multimedia playback.

**Verified locally:** the full iOS arm64 native core and Release application
compile and validate as `CFBundleVersion` 33; all 74 C++ tests, both CTest
entries and all 30 dependency-pin checks pass. The smoke-tested unsigned IPA
is `build/artifacts/BoxedVN-build33-unsigned.ipa`, SHA-256
`c1b29ac654c1745512a99968d24ed1c123c41843cb19ba4e45d0a832d80755dd`.
Physical-device presentation evidence remains required.

## 1am. Build 33 isolates the final pre-render stop; build 34 restores engine JIT (2026-08-08)

The build 33 device log `boxedvn-20260808-180757.log` proves the first-frame
indicator worked and, crucially, separates successive game surfaces:

- surface `0x15afbbe80` creates swapchain `0x158f47000`, submits one workload,
  and presents successfully with `VK_SUBOPTIMAL_KHR`; the native indicator is
  removed, then Wine destroys that transient surface normally;
- Wine immediately creates final 800x600 surface `0x160dd8e00` and swapchain
  `0x158f44c00`;
- that final surface reaches one primitive-restart warning and PNG loading,
  but never calls queue present. Its indicator therefore correctly remains;
- there is no unhandled exception or recurrence of the old `REP MOVSB` fault.

The broad `0x7F300000–0x7F500000` profile had achieved its diagnostic purpose:
it proved interpreting the mvware engine removes the corrupt-copy fault. It
also left the entire 2 MiB engine span on the decoded interpreter, including
post-launch asset/render preparation. That is unsuitable for an interactive
game and is now the strongest actionable cause of the silent pre-render stop.

Build 34 keeps only two 64 KiB interpreter windows:
`0x7F3D0000–0x7F3E0000` and `0x7F440000–0x7F450000`. They contain the two
device-observed mvware failure sites while returning the other 1.875 MiB—93.75
percent of the former compatibility span—to ARM64 JIT. A lightweight
interpreter heartbeat logs every 25 million profiled instructions if either
window is actively consuming CPU.

Presentation diagnostics are now per surface rather than per Vulkan instance.
Both `vkAcquireNextImageKHR` variants report the first acquire and failures;
all three queue-submit entry points report the first submission for the newest
live presentation surface; queue present remains tied to its exact swapchain.
The next log can therefore distinguish engine/interpreter work, first-image
acquisition, GPU submission and display without confusing a destroyed setup
surface with the final game surface.

**Verified locally:** the iOS arm64 native core and Release application compile
and validate as `CFBundleVersion` 34; all 74 C++ tests, both CTest entries and
all 30 dependency-pin checks pass. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build34-unsigned.ipa`, SHA-256
`16af19b0fcbc0981352e668ffaa312e66b3d2d36bc169ffc7c4c4ec1c34c17f1`.
Physical-device execution remains required.

## 1an. Build 35 fixes the underlying REP MOVS JIT defect (2026-08-08)

The build 34 device log `boxedvn-20260808-182111.log` closes the diagnostic
gap. The transient surface acquires, submits and presents successfully. Wine
then destroys it, creates the final 800x600 surface and swapchain, and executes
25,000,000 compatibility-interpreter instructions at `0x7F444BDE` without
ever reaching that surface's first acquire. The first-frame indicator was
therefore accurate. The renderer and UIKit layer were waiting for guest CPU
work; they were not the stall.

Source inspection found the exact defect in `Jit::movsr`. Its optimized
eight-byte copy path has a scalar fallback when source and destination overlap
by fewer than eight bytes. Both forward and backward fallbacks branched while
the invariant address-distance register was nonzero. Saya's captured
instruction was `F3 A4` (`REP MOVSB`) with `EDI == ESI + 4`, so the condition
could never become false. The loop continued after the requested count,
underflowed `ECX`, and eventually produced build 31's
`ECX=0xFFE1D4F0`/unmapped-write fault. Interpreting a surrounding range avoided
that JIT loop, but build 34 proved the engine code around it is too hot to
interpret.

Build 35 makes both overlap fallbacks loop on `ECX`, preserving bytewise x86
overlap propagation and terminating exactly at count zero. The Saya launch
profile now installs no module or address-range interpreter selector: Wine,
WineD3D and the entire game engine remain on ARM64 JIT. The CPU overlap test
also runs twice so its second pass enters the optimized JIT path; previously it
only verified the decoded-interpreter pass and could not catch this bug.

**Local verification:** the 74-test BoxedVN support suite and both CTest
entries pass. The full emulator test target and corrected JIT source compile
for macOS x86_64. Its isolated `Test Movsb 0a4` and `Test Movsb 2a4` cases pass
with the overlap regression executing a second, optimized-JIT pass; the 32-bit
MOVSD overlap stage also returns before that test reaches its separate existing
page-boundary coverage. The complete iOS arm64 native core and Release app
compile and validate as `CFBundleVersion` 35. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build35-unsigned.ipa`, SHA-256
`ec52ffa9e2e44001cab33947427a7f19bad528c33186a57526377c850a69fb76`.
Physical-device main-menu evidence remains required; local macOS cannot
reproduce the iOS Wine/MoltenVK stack.

## 1ao. Build 35 reaches the final engine wait; build 36 upgrades Wine (2026-08-08)

The build 35 device log `boxedvn-20260808-194648.log` confirms the JIT repair:
there is no interpreter selector, compatibility heartbeat, underflowed `ECX`,
or WineDbg fault. WineD3D initializes against the Apple A19 GPU. A transient
800x600 game surface acquires, submits and presents successfully, then closes
normally. The game creates its final 800x600 surface and swapchain, reaches
shader setup and PNG asset loading, but never calls the first final-surface
acquire. The first-frame indicator is therefore truthful; UIKit and Metal are
waiting for guest-side engine or synchronization work.

The root filesystem pin explains why another renderer patch is the wrong next
move. BoxedVN was running Wine 10.0. WineHQ bug 50577 is specifically **Saya
no Uta: hangs on RtlpWaitForCriticalSection**, with one thread waiting on a
critical section held by thread `0024`; WineHQ lists it among the bugs fixed in
[Wine 10.11](https://list.winehq.org/hyperkitty/list/wine-announce%40list.winehq.org/2025/6/).
The device log's game/D3D work also runs on Wine thread `0024`. This is strong
upstream evidence, not yet physical-device proof that the same lock is the
only remaining stop.

Build 36 changes the pinned default from Wine 10.0 to Boxedwine's checksummed
Wine 11.0 root, which contains the 10.11 fix. A deliberately bundled runtime
now takes precedence over an older imported archive, preventing an existing
`Application Support/rootfs/boxedwine.zip` from silently keeping the test on
Wine 10. Normal unbundled builds still use a user-imported root. Wine 10.0
remains pinned as an explicit legacy fallback. Existing game content and the
writable prefix are preserved so Wine can perform its normal prefix upgrade.

The Wine 11 archive is the exact 162,748,254-byte upstream artifact and
matches pinned SHA-256
`41835c49ce0e582a1d7a610243a8e4a95bc59996fb780c09705dc476bb9b6493`.
Its `wine.inf` identifies Wine 11.0 and its registry contains the entries
BoxedVN's idempotent renderer/Bluetooth policy patches. All 74 support tests,
both CTest entries and all 30 dependency-pin checks pass. The complete arm64
iOS application builds and validates as `CFBundleVersion` 36 with the root
present. The smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build36-wine11-unsigned.ipa`, SHA-256
`27f01f41468b356b47d193dbd99f188b7c766e4327738e51fee780f8db03c415`.
The embedded 162,748,254-byte `boxedwine.zip` hashes to the exact Wine 11 pin.

## 1ap. Build 36 rules out Wine 10; build 37 fixes X11 raster ops and captures the wait (2026-08-08)

The build 36 device log `boxedvn-20260808-203901.log` proves the bundled Wine
11 runtime is active. Saya creates the launcher, destroys a transient 800x600
presentation surface after a successful frame, then creates the final 800x600
surface and swapchain. It reaches the same PNG-loading boundary as Wine 10,
allocating roughly 2,432 JIT blocks without the old `REP MOVS` fault,
interpreter range or WineDbg crash. The final surface still never attempts its
first acquire. Wine 11 therefore controls the Wine-version variable and rules
the Wine 10.0/10.11 critical-section bug out as the sole remaining cause.

Immediately before the final stop, Boxedwine reports that X11 image raster
functions 1 and 6 are unsupported. Those are `GXand` and `GXxor`; classic
Win32/X11 masked drawing commonly composes those two operations. The old
implementation silently skipped the AND operation and only had a special-case
XOR path. Build 37 implements the complete 16-function X11 boolean raster set
for `PutImage`, including correct plane-mask merging, and retains the direct
copy fast path. A regression test covers every function plus masked `GXand`.

Build 37 also makes a failed device run useful without another speculative
patch. Each presentation surface owns a cancellable first-frame watchdog. If
no successful present arrives, snapshots at 12 and 30 seconds enumerate every
guest process/thread using atomically published dispatch-boundary EIP, stack
pointers and dispatch counts. Active futex records include the guest address,
operation, expected value, PID/TID and wait age. Comparing the two samples
distinguishes active guest execution, a host-blocked emulation thread and a
guest lock wait, while avoiding unsafe cross-thread reads of the live CPU
register file.

The 74-test support suite, both CTest entries and all 30 dependency-pin checks
pass. The macOS emulator test target compiles, and its final 78-test fast slice
passes with the new all-raster-operations regression included. The complete
arm64 iOS core and Release application compile and validate as
`CFBundleVersion` 37 with the exact pinned Wine 11 root bundled. The
smoke-tested unsigned IPA is
`build/artifacts/BoxedVN-build37-x11-watchdog-unsigned.ipa`, SHA-256
`9a851016616a5aeead597064311a9b2d966b82e1801677a8f9bbeb522a46aae2`.
Physical-device proof remains required because local macOS cannot reproduce
the iOS Wine/MoltenVK presentation stack.

## 1aq. Build 37's watchdog names the stall: MoltenVK pipeline compilation, starved by a guest getrusage spin; build 38 bounds both (2026-08-08)

The build 37 device log `boxedvn-20260808-220230.log` is the first run that
answers the question instead of restating it. Both automatic snapshots fired,
at 12 and 30 seconds, and the comparison between them is decisive.

In `Saya_en.exe` (pid `000A`, 16 threads), thread `0060` sits at guest EIP
`12FDDE77` with dispatch count `67187` — **identical in both samples**. Zero
dispatch-boundary progress across 18 seconds means the emulation thread is
blocked inside a host call, not executing guest code. Resolving that address
against the bundled 32-bit `lib/libvulkan.so.1` places it four instructions
into `vkCreateGraphicsPipelines` (entry `0x9E6C`), on the block boundary
created by its `__x86.get_pc_thunk` return, immediately before the `int $0x9A`
host trap. Saya is blocked in graphics-pipeline creation.

Meanwhile seven threads (`0065`–`006B`, minus `0069` at the 12-second sample)
sit at `1051ACF5`, which resolves inside `__getrusage64` at the instruction
following `int $0x80` with `EAX=0x4D` — `__NR_getrusage`. Each advanced by
roughly 21.8 million dispatches over those 18 seconds, about 1.2 M/s per
thread. Seven guest threads were spinning on a timing syscall, saturating the
phone's cores, while one thread waited on a native Metal compile.

Two independent defects, and build 38 addresses both:

- **The wait was unbounded.** MoltenVK's `metalCompileTimeout` defaults to
  effectively infinite, so a Metal compile that never calls back hangs the
  caller forever rather than failing. `configureMoltenVKForWineD3D()` now sets
  `MVK_CONFIG_METAL_COMPILE_TIMEOUT` to 20 s and disables
  `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS`; WineD3D's D3D11 compatibility path
  does not need bindless descriptor capacity. Both names and value types were
  checked against the bundled MoltenVK 1.4.2 headers, where
  `useMetalArgumentBuffers` is a `VkBool32` and `metalCompileTimeout` is
  nanoseconds. The call runs before `boxedmain()`, which is required: MoltenVK
  latches its configuration during first Vulkan initialization.
- **The spin was uncontested.** On a real kernel every `getrusage` is also a
  scheduling boundary; Boxedwine services it entirely in-process, so nothing
  ever yields. `GetrusageFairness` (`include/getrusagefairness.h`) watches for
  32 consecutive calls each within 5 ms of the previous, and only then inserts
  a 1 ms host scheduling point per call. Normal `getrusage` use and Wine's
  cold start pay nothing. The detector is a pure state machine, deliberately
  separated from sleeping and logging so `ios/tests/test_getrusage_fairness.cpp`
  can cover its transitions on the host. Activation is logged with PID/TID.

`vk_CreateGraphicsPipelines` now logs an enter/exit pair per call with a
monotonic call id, create-info count, per-info stage mask, topology,
`primitiveRestart`, `rasterizerDiscard`, render pass and subpass, plus the
measured duration. That converts the next run's outcome into a fact: either a
call id enters and never exits, or it exits with a duration and a result. The
same run already surfaced `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not
support disabling primitive restart` from MoltenVK, so the logged
`primitiveRestart` field is the value to read first. The generated Vulkan
bridge's pipeline marshalling was also given proper ownership and cleanup
rather than leaking its nested create-info allocations per call.

The 77-test support suite, both CTest entries and all dependency-pin checks
pass. The complete arm64 iOS core and Release application compile and validate
as `CFBundleVersion` 38 with the pinned Wine 11 root bundled. The smoke-tested
unsigned IPA is `build/artifacts/BoxedVN-build38-unsigned.ipa`, SHA-256
`215b14304202dd223e423fd1c5f44d8ab05cf78d08474ddf9a932b5bc55d907d`.

Build 38 is a **diagnostic build as much as a fix**. Neither change is yet
proven to produce a frame: if Metal compilation was merely starved, freeing
seven cores may be sufficient; if it was genuinely wedged, the 20 s ceiling
converts an indefinite hang into a logged error with a duration. Both outcomes
are progress, and the enter/exit log distinguishes them. Physical-device proof
remains required because local macOS cannot reproduce the iOS Wine/MoltenVK
presentation stack.

## 1ar. Build 38 clears the starvation and names the real defect: MoltenVK faults on a stage-less pipeline; build 39 refuses it and adds slim IPAs (2026-08-08)

Build 38's device log `boxedvn-20260808-223549.log` separated the two variables
cleanly, and one of them was a dead end.

**The fairness throttle worked exactly as designed.** The seven spinning
threads fell from roughly 21.8 million dispatches per 18 seconds to about
88,000, and pipeline compilation immediately became healthy: call #1 took
190.684 ms and calls #2, #3 and #4 took 0.230, 0.124 and 0.133 ms. So CPU
starvation was real and is now fixed — but it was **not** the terminal hang.

**The terminal hang is unchanged and now precisely located.** Thread `0054`
sits in `vkCreateGraphicsPipelines` with dispatch count `66766`, identical in
both snapshots, at `esp=0179E7F0 ebp=0179E7F8` — byte-for-byte the same stack
as build 37's thread `0060`. Same guest location, both builds. The new
per-call log names it: `call #5 enter: … info[0]: stages=0 mask=0x0
topology=3 primitiveRestart=0 rasterDiscard=0 renderPass=0x152679E00`, with no
matching exit. **A graphics pipeline with zero shader stages.**

That was settled locally rather than with another device cycle. A 40-line host
program built against the project's own `lib/mac/vulkan/lib/libMoltenVK.dylib`,
submitting exactly that pipeline shape, dies with **`EXIT=139` — SIGSEGV**.
MoltenVK does not validate `stageCount`; it builds a
`MTLRenderPipelineDescriptor` with a nil vertex function and dereferences it.

That also explains the two things that had not fit. First, why iOS *hangs*
where macOS crashes: Boxedwine installs a `SIGSEGV` handler to service guest
page faults, so a **host** fault raised inside a Vulkan call is delivered to a
handler that cannot map it to guest memory, and the emulation thread wedges —
`state=RUNNING`, zero dispatch progress, forever. Second, why build 38's 20 s
`metalCompileTimeout` never fired: the thread never reached the shader
compiler. Both symptoms follow from one cause.

Build 39 therefore refuses the call rather than forwarding it.
`include/vkpipelineguard.h` holds the decision as a pure, testable function:
a non-library graphics pipeline with `stageCount == 0`, or a null `pStages`
with a non-zero count, is rejected with `VK_ERROR_INITIALIZATION_FAILED`,
every output handle is set to `VK_NULL_HANDLE`, and nothing is handed to
MoltenVK. `VK_PIPELINE_CREATE_LIBRARY_BIT_KHR` is excluded, since a pipeline
library may legitimately carry no stages. A rejected pipeline is a visible,
recoverable guest error; a forwarded one freezes the process.

The refusal also dumps the guest's 88-byte `VkGraphicsPipelineCreateInfo`
verbatim. The decoded fields cannot distinguish "WineD3D really asked for a
stage-less pipeline" from "the generated bridge mis-read the struct", and that
distinction decides where the next fix belongs. The 32-bit layout and the
`i * 88` stride were both re-derived by hand and match; calls #1–#4 decoding
correctly is further evidence the reader is sound, which tilts toward WineD3D
genuinely emitting it — most likely after an upstream shader-generation
failure. The dump settles it either way.

**Iteration cost.** `scripts/build-ios.sh --no-bundled-rootfs` omits the
160 MB root filesystem, since `ios/app/Bundled` is already an optional
resource directory and `Storage.activeRootFilesystem` already falls back to a
user-imported archive. The result is a **3.7 MB IPA instead of 161 MB**. The
stash is restored from an `EXIT` trap, so an interrupted build cannot lose the
archive. Both forms of build 39 are packaged:
`BoxedVN-build39-slim-unsigned.ipa` (3.7 MB) and
`BoxedVN-build39-unsigned.ipa` (161 MB, SHA-256
`2e3d0b3ff852ab35884c6a9d85a793866002d119ca5088ea258f770fa821717b`).

81 host tests and all dependency-pin checks pass, including four new cases
covering the guard's accept, both refusals and the pipeline-library exemption.

What build 39 does **not** claim: refusing the pipeline removes the freeze, it
does not by itself draw Saya's menu. If WineD3D needed that pipeline, the next
symptom will be a missing or wrong frame rather than a hang — which is a
better failure, and a legible one.

## 1as. Build 39 removes the freeze and proves the bridge is innocent; the defect is WineD3D's shader translation (2026-08-08)

Build 39's log `boxedvn-20260808-233036.log` did what it was built to do. The
guard fired, the emulation thread never wedged, and the app stayed alive.

**The byte dump settles the marshalling question.** The refused struct at guest
address `0179E884` decodes exactly as written: `sType=0x1C`
(`VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO`), `pNext=0`, `flags=0`,
`stageCount=0`, `pStages=NULL`, then valid non-null pointers for every
fixed-function state (`pVertexInputState=0x0179E8DC`,
`pInputAssemblyState=0x0179E8FC`, viewport, raster, multisample, depth-stencil,
colour blend, dynamic), a real `layout=0x1507A9700`, a real
`renderPass=0x1507C2C80`, `subpass=0` and `basePipelineIndex=-1`. Nothing is
garbage. **The generated Vulkan bridge reads the struct correctly; WineD3D
genuinely emits a graphics pipeline with no shader stages.**

The guest agrees, immediately after each refusal:
`err:d3d:wined3d_context_vk_apply_draw_state Failed to get graphics pipeline.`
and `err:d3d:adapter_vk_draw_primitive Failed to apply draw state.`

**The stage histogram localises the defect precisely.** Across the run, 5
pipelines carried `stages=2` and succeeded; 3,199 carried `stages=0` and were
refused. The five that work are WineD3D's own internal blit/clear pipelines,
whose SPIR-V ships pre-compiled inside wined3d. Every pipeline that needs a
*game* shader — DXBC translated to SPIR-V by vkd3d-shader — arrives with
nothing attached. Saya is a D3D11 title (`d3d11_device_CheckFormatSupport`
stubs confirm it), so this is the whole rendering path.

That also explains the reported flicker and eventual death: with every draw
failing, the guest tore down and rebuilt its swapchain **489 times**, which is
the loading-screen/black-screen oscillation, and the log carries 3,199 copies
of a 264-character hex dump — write volume that competes with the frame loop
it is measuring.

Build 40 is therefore an instrumentation build, narrowly aimed:

- `vkCreateShaderModule` now logs the first 32 calls and every failure with
  code size, result and handle. This separates two very different defects that
  look identical from outside: shader modules that never reach Vulkan at all
  (translation failed inside the guest) versus modules Vulkan itself rejected.
- The pipeline refusal, enter/info and exit logging are all rate-limited. The
  first 32 calls and the first 8 refusals are traced in full; after that only
  failures, compiles slower than 50 ms, and a refusal count every 512.
- Games on the accelerated path launch with
  `WINEDEBUG=warn+d3d_shader,err+d3d_shader,err+d3d`, because at Wine's default
  verbosity the translation failure is silent and only its downstream symptom
  is visible. A caller-supplied `WINEDEBUG` still wins, and plain Wine apps
  such as the Notepad regression check are deliberately left unchanged.

84 host tests and all 30 dependency-pin checks pass.

**Honest status: Saya does not render, and build 40 does not claim to fix
that.** It converts a silent failure into a named one. The freeze is gone and
the process survives, but the cause — why vkd3d-shader produces no SPIR-V for
this game's shaders — is not yet known, and is the one thing that stands
between here and a menu.

## 1at. Build 40 names the failure: WineD3D cannot parse the game's DXBC at all (2026-08-09)

Build 40's log `boxedvn-20260809003044.log` isolated the defect to a single
guest function, and the slim IPA path worked (the command line shows the
imported `Library/Application Support/rootfs/boxedwine.zip`).

The `d3d_shader` channel is unambiguous:

```
warn:d3d_shader:wined3d_shader_extract_from_dxbc Failed to parse DXBC, hr 0x80070057.   (335x)
warn:d3d_shader:wined3d_shader_create_ps Failed to initialize pixel shader,  hr 0x80070057. (328x)
warn:d3d_shader:wined3d_shader_create_vs Failed to initialize vertex shader, hr 0x80070057. (7x)
```

`0x80070057` is `E_INVALIDARG`. The failure is in **`extract_from_dxbc`** —
container parsing — which happens *before* any SPIR-V translation. Every one of
the game's 335 shaders is rejected. Only two shader modules ever reached
Vulkan at all (`codeSize=504` and `codeSize=3320`), and those are wined3d's own
pre-compiled internal blits, exactly matching the 5 successful `stages=2`
pipelines from build 39. **No game shader ever becomes a Vulkan object.**

Two hypotheses were tested locally and **both falsified**, which is worth
recording so they are not revisited:

- *Missing shader-compiler DLLs.* The rootfs's
  `windows/system32/d3dcompiler_47.dll` is 3072 bytes, which looks like a stub.
  It is not: modern Wine splits every module into a thin PE and a Unix
  counterpart, and `opt/wine/lib/wine/i386-unix/d3dcompiler_47.dll.so` is
  304,380 bytes with `wined3d.dll.so` at 3,574,668. The D3D stack is complete.
- *A malformed container header.* Wine's tag check emits its own warning before
  returning, and at `warn+d3d_shader` no such message appears. The blob is
  reaching a working parser that rejects it for some other reason.

So `E_INVALIDARG` is being produced inside vkd3d-shader, which reports on its
own channel that build 40 did not capture. That single HRESULT covers several
faults needing entirely different fixes — a blob that is not DXBC, a container
whose checksum does not match its contents (which would indicate the bytes are
corrupted in flight, and this port has already found one CPU defect of exactly
that kind in build 35's `REP MOVS`), or an unsupported shader version.

Build 41 therefore adds `warn+vkd3d,fixme+vkd3d,err+vkd3d` to the accelerated
path's `WINEDEBUG`. These fire per shader rather than per draw, so the volume
stays bounded. 84 host tests and all 30 pin checks pass.

**Status: Saya still does not render.** Builds 38–41 have removed a CPU
starvation stall, a MoltenVK fault that froze the process, and a false lead
about the Vulkan bridge, and have narrowed a blank screen down to one guest
function with a known HRESULT. The remaining question — why vkd3d-shader
rejects every DXBC blob — is the last one between here and a menu, and build 41
is built to answer it rather than guess at it.

## 1au. Root cause found: Metal has no geometry shaders, so wined3d can never reach SM4; build 43 claims feature level 9_3 honestly (2026-08-09)

Build 42's trace ends the search. Two adjacent shaders in
`boxedvn-20260809005857.log` show the whole mechanism:

```
shader_dxbc_process_section Feature level 9 shader version 0ffff0200, 0ffff0201.
shader_trace     ps_2_1
shader_sm1_init Version: 0xffff0201.
wined3d_shader_create_ps Created pixel shader 001EB9C0.        <- succeeds
...
shader_dxbc_process_section Skipping chunk 'RDEF'.
shader_dxbc_process_section Skipping SM4+ shader.               <- refused
shader_dxbc_process_section Skipping chunk 'STAT'.
wined3d_shader_extract_from_dxbc Failed to parse DXBC, hr 0x80070057.
```

`0xffff0200`/`0xfffe0201` are `ps_2_0`/`vs_2_1`: these are `Aon9` chunks, the
feature-level-9 fallback bytecode that `d3dcompiler` embeds in a
`ps_4_0_level_9_x` shader. **Shaders carrying an Aon9 chunk are read and
created successfully. Shaders with only an SM4 chunk have that chunk skipped,
leaving nothing to parse, and fail with E_INVALIDARG.** wined3d is refusing
shader model 4 outright, which the `float_const_count 256`/`224` values in the
same trace corroborate - those are D3D9-era constant limits.

The reason was confirmed locally, not inferred. A feature probe built against
the project's own `libMoltenVK.dylib` reports:

```
geometryShader                         no
tessellationShader                     YES
```

This is not a property of one GPU and not a MoltenVK defect: **Metal has no
geometry shader stage at all**, so MoltenVK reports `geometryShader = false`
on every device and always will. Direct3D 10 mandates geometry shaders, so
wined3d's Vulkan adapter can never advertise shader model 4, and its DXBC
reader therefore skips every SM4+ code chunk. This is the same wall that
stopped DXVK in build 25, reached from the other side.

So SM4 cannot be made to work here. What can be fixed is the **inconsistency**:
the adapter was letting the game believe it had a capability the backend then
refused. Build 43 writes `MaxShaderModelVS=3`, `MaxShaderModelPS=3` and
`MaxShaderModelGS=0` under `Software\Wine\Direct3D` whenever the Vulkan
renderer is selected, making the device honestly feature level 9_3. A title
that ships `_level_9_x` shaders - as this one demonstrably does, since those
are exactly the ones already succeeding - should then select the set that
works. These keys can only lower a claimed capability, never raise one, so
they cannot break a title that already renders. The cap is not applied to the
software/GDI prefix, which has no reason to carry it.

85 host tests and all 30 pin checks pass, including a check that the cap is
written for the Vulkan path and absent from the GDI path.

**This is the first change since build 38 that can plausibly produce a
picture rather than a better error message.** It is still a hypothesis about
the game's behaviour: it assumes Saya honours the reported feature level when
choosing shaders. If it does, the draws that have been failing 3,199 times per
run should start succeeding. If it ignores the feature level and asks for SM4
regardless, the same shaders will fail and the answer is that this engine
cannot run on Metal through wined3d at all - at which point the question
becomes which renderer can, not how to fix this one.

## 1av. Build 44 closes the fallback question: the engine links Direct3D 11 statically; build 45 reverts the probe (2026-08-09)

Build 43's shader-model cap changed nothing. Both logs show identical counts to
build 42 - 335 DXBC failures, 4 feature-level-9 shaders, 339 SM4 skips - and
across an entire run **exactly four shaders are ever created, all shader model
2** (2x `ps_2_1`, 2x `vs_2_1`). The cap was inert because wined3d had already
computed SM3 on its own, and the game does not adapt to the feature level it is
handed. That hypothesis is falsified; the four working shaders are incidental,
almost certainly a video or blit helper, not evidence of a 9_x path.

Build 44 disabled `d3d11`/`dxgi` to ask whether the engine has any other
renderer. The answer is final:

```
err:module:import_dll Library d3d11.dll (which is needed by L"D:\Mware.dll") not found
err:module:import_dll Library Mware.dll (which is needed by L"D:\Saya_en.exe") not found
err:module:loader_init Importing dlls for L"D:\Saya_en.exe" failed, status c0000135
```

`Mware.dll` - the engine - imports `d3d11.dll` in its **PE import table**, so
the loader resolves it before any game code runs. There is no software path and
no DirectDraw path to fall back to. Build 45 reverts the override, since it can
only prevent the game launching, and adds a regression test so it is not
reintroduced. The full `d3d_shader` trace is also dialled back to `warn`, its
job done.

### Where this leaves the title

The constraint is now fully characterised and none of it is a BoxedVN defect:

1. `Mware.dll` links `d3d11.dll` statically - some Direct3D 11 implementation
   must exist or the process will not start.
2. The engine's rendering is 331 shader-model-4 shaders and it will not use
   anything else.
3. wined3d gates SM4 on Direct3D feature level 10, which mandates geometry
   shaders.
4. Metal has no geometry shader stage, so MoltenVK reports
   `geometryShader = false` on every device, permanently. Verified locally
   against the project's own `libMoltenVK.dylib`.

So wined3d can never run this game on Metal, and no registry or prefix setting
changes that. The remaining route is a **Direct3D 11 implementation that
compiles SM4 without a geometry shader stage**, which is exactly what DXVK is:
it translates DXBC to SPIR-V itself, and a vertex or pixel shader needs no GS
stage to compile. The only blocker is that stock DXVK *requires* the
`geometryShader` feature at device creation - the wall build 25 hit - which is
precisely the requirement CodeWeavers relaxed to ship DXVK on macOS.

That makes the next step concrete rather than exploratory: a 32-bit DXVK built
with the geometry-shader requirement relaxed and its reported feature level
decoupled from it, dropped in as `d3d11.dll`/`dxgi.dll` for this prefix. It
needs a mingw-w64 + meson cross-build and a pinned dependency, and it is the
one path that can put this engine on screen.

## 1aw. DXVK patched for MoltenVK and cross-built; build 46 routes Direct3D 11 through it (2026-08-09)

wined3d is out of the picture for this title, so build 46 replaces it. DXVK is
architecturally different in the one way that matters: it translates DXBC to
SPIR-V itself, and a vertex or pixel shader needs no geometry stage to compile.
wined3d instead gates SM4 on Direct3D feature level 10, which mandates geometry
shaders - a rule Metal can never satisfy.

Reading DXVK 2.5.2's source made the patch far smaller than build 25 implied.
`D3D11DeviceFeatures::GetMaxFeatureLevel()` never consults `geometryShader` at
all; its lowest return is `D3D_FEATURE_LEVEL_10_1`, which already accepts
shader model 4. The only blocker is `D3D11Device::GetDeviceFeatures()`, where
DXVK's convention is that `= VK_TRUE` means *required* (and fails
`checkFeatureSupport`) while `= supported.X` means *use if present*. Four
requirements are unsatisfiable on Metal and all four are relaxed to optional:

- `geometryShader` - no Metal stage exists;
- `shaderCullDistance` - not exposed by MoltenVK;
- `textureCompressionBC` - varies by Apple GPU family, so let DXVK fall back
  per format rather than refuse a device;
- `extTransformFeedback.transformFeedback` / `.geometryStreams` - backs D3D11
  stream-output, which a 2D title never issues.

Nothing else in `GetDeviceFeatures` is a hard requirement: everything above
feature level 10_1 already uses `supported.`.

The result is cross-built with mingw-w64 for i686. D3D9 and D3D8 are disabled
because current mingw headers already define `_D3DDEVINFO_RESOURCEMANAGER`,
which collides with DXVK's own copy - and the engine imports only `d3d11.dll`.
Stripped, the modules land at 4.7 MB, 3.0 MB and 135 KB, essentially matching
the stock builds, and ship in the app as a `Dxvk/` folder reference.

Wiring reuses machinery Boxedwine already has. `installBundledDxvk()` copies
the patched modules into the writable overlay at
`/home/username/.wine/drive_c/dxvk`, skipping files whose size already matches
so a relaunch does not recopy eight megabytes. Accelerated launches then pass
`-dxvk 1`, and `StartUpArgs` maps that directory over system32 and sets
`WINEDLLOVERRIDES=d3d11,d3d10core,d3d9,d3d8,dxgi=n,b` itself. Because only
d3d11/dxgi/d3d10core are shadowed, d3d8 and d3d9 still resolve to the archive's
stock DXVK.

86 host tests and all 30 pin checks pass. The slim IPA is 6.5 MB.

**Unproven on device.** MoltenVK previously rejected DXVK at the feature gate;
these four relaxations are aimed exactly at that gate, but removing them can
only reveal whatever DXVK asks for next. Boxedwine sets `DXVK_LOG_LEVEL=warn`,
so if a requirement is still unmet the log will name it rather than fail
silently - and if the gate is passed, DXVK compiles Saya's 331 SM4 shaders
through a path that has never been reached before.

## 1ax. Build 46 clears the DXVK feature gate; the next blocker is robustness2 (2026-08-09)

Build 46's log is the first real forward step in the graphics stack since the
problem was understood. The old refusal - `Minimum required feature level
D3D_FEATURE_LEVEL_9_1 not supported`, which stopped build 25 - **is gone**.
The prefix installer worked (`Installed the MoltenVK-compatible DXVK modules
into drive_c/dxvk`), Boxedwine's `-dxvk 1` mapped them over system32, and DXVK
got as far as calling `vkCreateDevice`. The four relaxed requirements were the
right ones.

It then failed one step later, and MoltenVK named the cause exactly:

```
[mvk-error] vkCreateDevice(): Requested physical device feature specified by the
            1st flag in VkPhysicalDeviceRobustness2FeaturesKHR is not available
[mvk-error] ... the 3rd flag in VkPhysicalDeviceRobustness2FeaturesKHR ...
err:   D3D11InternalCreateDevice: Failed to create D3D11 device
```

The 1st and 3rd flags are `robustBufferAccess2` and `nullDescriptor`. MoltenVK
advertises `VK_EXT_robustness2` but implements only the 2nd,
`robustImageAccess2` - which is why the extension is present while two of its
three features are not. DXVK requires both unconditionally.

The downstream consequence is the crash in the screenshot. With no D3D11
device, Wine fell back to wined3d (`wined3d_dll_init Using the Vulkan
renderer`) and `mware+0x81cd6` dereferenced the null it was handed:
`movl (%eax,%edx,4), %eax` with `EAX = 0`. The engine does not check the
result of device creation.

Build 47 relaxes both to optional. This one is a genuinely riskier change than
the previous four, and upstream says so in the code: DXVK requires
`nullDescriptor` because *"we no longer have a fallback for those in the
backend"*, and `robustBufferAccess2` because it uses the robustness alignment
properties. Relaxing them trades a guaranteed hard failure for possible
misbehaviour if the title actually binds null resources - a bet worth taking
for a 2D visual novel, since a device that exists can be diagnosed further and
one that is never created cannot.

`DXVK_LOG_LEVEL` is also raised from `warn` to `info` in `StartUpArgs`, so the
next run prints DXVK's complete device-feature table. Discovering blockers one
per device cycle is the expensive way to do this; the table names all of them
at once.

86 host tests and all 30 pin checks pass. The slim IPA is 6.5 MB.

## 1ay. Build 47 reaches feature level 11_0; a stale-DLL bug in BoxedVN's own installer masked the fix (2026-08-09)

Build 47's log contains the single most important line the graphics work has
produced:

```
info:  D3D11InternalCreateDevice: Maximum supported feature level: D3D_FEATURE_LEVEL_11_0
info:  D3D11InternalCreateDevice: Using feature level D3D_FEATURE_LEVEL_11_0
```

DXVK computes **feature level 11_0** on MoltenVK. Shader model 4 and 5 are
accepted. This is precisely what wined3d could never do, and it confirms the
central premise of the DXVK route: the geometry-shader stage is required only
to *advertise* Direct3D 10, never to compile a vertex or pixel shader.

Device creation nevertheless failed with the identical robustness2 error, and
the cause was BoxedVN's own code, not DXVK or MoltenVK. `installBundledDxvk()`
skipped any file whose size already matched the destination, on the stated
reasoning that these modules only change when the app is replaced. That
reasoning was wrong in the one case that mattered: relaxing two feature flags
changed no code size at all, so the rebuilt `d3d11.dll` was byte-for-byte the
same length (4,698,112) as build 46's. The installer skipped it and the device
kept running the previous build, reporting the very error the rebuild fixed.

Build 48 always overwrites. Copying roughly eight megabytes per launch is
irrelevant beside Wine's cold start, and no "is it already there" shortcut is
worth pinning a stale module. Two regression tests cover it: a same-size source
must replace the destination, and a build shipping no DXVK must not be treated
as an error. 88 host tests and all 30 pin checks pass.

The robustness2 relaxation from build 47 is therefore still **untested**. It
was compiled, but never reached the device.

## 1az. A runnable macOS host, so the guest stack can be debugged locally (2026-08-09)

Device round trips cost roughly fifteen minutes each and give one snapshot pair
per run. `BOXEDVN_BUILD_MACOS_HOST=ON` now produces `boxedvn_macos`, a
development-only executable that runs the same emulator core, the same
generated Vulkan bridge and the same patched DXVK against the Mac's own
MoltenVK - where lldb can attach and a rebuild is seconds.

**The iOS Simulator cannot serve this purpose**, which is worth recording so it
is not proposed again. The pinned MoltenVK xcframework contains exactly one
slice, `ios-arm64`; there is no simulator slice, and this development Mac is
Intel, so its simulator is x86_64. No Vulkan implementation can link into a
simulator build here. That is why the project's simulator preset is named
`ios-simulator-compile-check`: SDL2 simulator libraries exist, MoltenVK does
not. The macOS MoltenVK at `lib/mac/vulkan/lib/libMoltenVK.dylib` is universal
and already proven on this machine by the zero-stage and feature probes.

Six things were needed, each a small gap rather than a design problem:

- `BOXEDWINE_VULKAN` was defined only for iOS; the host needs the same bridge.
- `kvulkanSDL.cpp` included `vulkan_core.h` under `BOXEDWINE_IOS` rather than
  `BOXEDWINE_VULKAN`, so the host saw no `VK_SUCCESS`.
- SDL2's Cocoa backends need the AppKit/CoreGraphics framework set, plus
  OpenGL for `platform/mac/pixelformat.cpp` and CoreMIDI for the audio backend.
- `platform/linux/platform.cpp` calls three Cocoa helpers under `__MACH__`.
  Upstream defines them in its Xcode project behind a Swift bridging header,
  and the device shim is written against UIKit, so the host got its own
  minimal `ios/support/src/macos_host_platform.mm`.
- `main()` already exists in `platform/sdl/knativesystem.cpp` for non-iOS
  builds, so the executable only needs `-force_load` to pull it from the
  archive.

Verified end to end: the host loads the pinned root filesystem in 297 ms and
launches `/bin/wine notepad`, proving the emulator core, filesystem, JIT and
SDL paths all work outside the app.

**Caveat that bounds what this can prove.** This Mac is Intel, so the host
exercises Boxedwine's **x64** JIT, not the ARM64 one the phone uses. This port
has already found one ARM64-specific codegen defect (build 35's `REP MOVS`).
If the DXVK wedge is another, it will not reproduce here - and that
non-reproduction would itself be evidence, pointing at the ARM64 JIT rather
than at DXVK.

Still missing: the game itself is not on this Mac, so Saya cannot yet be run
locally.

## 8. Next actions, in order

Executable memory, rootfs loading, Wine execution and 2D rendering are now
device-proven. Game compatibility is next.

1. **Install the bundled-runtime build 38, then reassign `universal.js` to the
   newly installed BoxedVN in StikDebug.** Reinstalling or re-signing can change the target
   identity; `JIT ready`/`CS_DEBUGGED` alone is insufficient.
2. Launch Notepad once as a short regression check; build 27 already proves
   this path after the script was reassigned.
3. **Launch Saya directly.** The command must not contain
   `-dxvk 1`. Prefix preparation must say `stale GDI/no-3D policy repaired`.
   Confirm the 113x2 probe is logged as an `offscreen helper` and the launcher
   replaces the old black screen; build 29 already proves this path.
4. Confirm Settings reports **Bundled runtime**, proving embedded Wine 11 won
   over any earlier imported Wine 10 archive. When the launcher appears,
   confirm the command contains **no**
   `-interpreterRange` or `-interpreterModule`, then press Start Game once.
   The log should say the corrected `REP MOVS` overlap path is active and the
   entire engine retains JIT. Success is the first frame or menu. The
   `STARTING GAME / WAITING FOR THE FIRST VIDEO FRAME` indicator must replace
   unexplained black until the first successful present. Export the complete
   If it remains, leave it visible for **at least 60 seconds** — long enough
   for both automatic thread/futex snapshots (12 s, 30 s) *and* build 38's 20 s
   Metal compile ceiling to expire — then export the log. Do not reintroduce
   an interpreter range or change resolution without that evidence.
5. **Read the build 38 log in this order.** It is designed to answer the
   pipeline question in one run:
   a. `MoltenVK WineD3D policy:` confirms the configuration was applied before
      Vulkan initialization. Its absence invalidates the rest of the run.
   b. `iOS getrusage fairness activated` names the spinning PID/TIDs. Its
      absence means the spin did not recur and CPU starvation is not the
      variable under test this run.
   c. `Vulkan graphics pipeline call #N enter` / `exit`. An `enter` with no
      matching `exit` after 20 s means the ceiling did not take effect —
      re-check (a). An `exit` with a large duration means compilation was
      merely starved and is now completing. An `exit` with a non-zero result
      means MoltenVK rejected the pipeline, and the `primitiveRestart` and
      `topology` fields logged with that call id name the reason.
   d. Only if a pipeline is rejected for primitive restart: the fix belongs in
      WineD3D's input-assembly state, not in another emulator-side guess.
6. Do not block graphics work on the remaining cold-start delay. The warm
   evidence supports a later persistent Wine prewarm/reuse design.
7. Type in Notepad once to confirm the simplified keyboard still handles
   Shift, Space, Backspace, Enter and **HIDE** without native-keyboard flicker.
8. While Notepad is running, turn the phone both ways. The guest must remain
   landscape and responsive; live guest rotation is deliberately disabled.
9. **Launch Notepad a second time in the same app process.** Build 16 proves
   one guest can return, but not that SDL/Boxedwine can initialize twice.
10. Test its title screen, text rendering, touch input, audio, save and load.
11. Only then: compatibility fixes required by the game and an accelerated
   renderer for Direct3D/OpenGL titles. The old `source/opengl/es` shim is an
   experimental 2018 path and must not be enabled blindly.

## 9. Known open questions

- **Was Saya's pipeline compile starved, or genuinely wedged?** Build 37's
  snapshots prove thread `0060` made zero dispatch progress inside
  `vkCreateGraphicsPipelines` for 18+ seconds while seven sibling threads
  spun on `getrusage`. That is consistent with both CPU starvation of the
  Metal compiler and an internal MoltenVK stall. Build 38 changes both
  variables at once — deliberately, because each alone costs a device cycle —
  so the enter/exit duration log, not the outcome, is what separates them.
- **Does WineD3D request a pipeline Metal cannot express?** The same run shows
  `VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling primitive
  restart`. MoltenVK reports that during pipeline construction and continues,
  so it is not proven to be the stall — but if build 38's logged
  `primitiveRestart` field is `0` on the failing call id, this is a real
  WineD3D/MoltenVK incompatibility and belongs in input-assembly state, not
  in the emulator.
- **Why do seven Wine threads poll `getrusage` at ~1.2 M dispatches/second?**
  The fairness throttle is a mitigation, not an explanation. Identical
  per-thread counts point at a worker pool spin-waiting on the loading thread.
  If build 38 produces a frame, this stops mattering; if it does not, the
  spin's origin is the next thing to name.
- **Which cold initialization stage owns the 57–61 second pause?** Build 24
  proves it is not removed by disconnecting winebus. A subsequent guest starts
  in seconds inside the same process. Optimize with process reuse/prewarm or a
  real profile after graphics works.
- **Will GetMoreRam change the running signature and usable headroom?** Build
  26 reports both the signed `increased-memory-limit` entitlement and
  `os_proc_available_memory()` on the home/status screens. The first attempt
  remained `Standard limit` with 3.29 GB headroom, proving that signing path
  did not authorize the entitlement. Do not infer success from GetMoreRam's
  UI; only the installed signature and changed headroom count.
- **What Japanese profile does Saya need?** Build 19 uses an English ACP 1252
  prefix and no CJK font, and its error dialog is mojibake. First retest the
  corrected working directory. If needed after that, add a real per-game ACP
  932/locale/font profile rather than globally changing every Wine prefix.
- **Does import-as-copy complete where open-in-place failed?** Build 18 proved
  scene ownership alone does not. The direct Documents route means the answer
  cannot block game import.
- **Does landscape remain locked through physical device turns?** Build 16
  proves SDL starts with correct landscape geometry, but the latest report did
  not explicitly confirm turning the phone during that session.
- **What resolution does Fruit of Grisaia actually request?** The 800x600
  Notepad profile is intentionally 4:3 and leaves blue side area. Choose a
  per-game default from the executable's real behavior rather than stretching.
- **Does `boxedmain()` survive being called twice?** The design returns to the
  library UI after a session and allows another launch. Boxedwine calls
  `SDL_Init`/`SDL_Quit` inside that span. If a second session breaks, the
  fallback is to require an app restart between sessions, which still meets
  the MVP requirement of one session at a time but not the "exit without
  force-closing" criterion. **Untested.**
- **Does SDL 2.32.10's UIKit backend cooperate with a second `UIWindow`?**
  The library window sits below SDL's. Untested on device.
- **Memory.** Boxedwine reserves a large guest address space. Build 26 detects
  whether `increased-memory-limit` survived signing and reports current
  process headroom. The tested GetMoreRam/signing route did not preserve it;
  another provisioning method would need to prove both a signed entitlement
  and meaningfully greater headroom before BoxedVN treats it as enabled.
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

### 2026-08-09 — build 64: presentation, the in-game overlay, and an app icon

Worked from the exported build-63 device log (`boxedvn-20260809-183633.log`,
38,221 lines) rather than a live pull.

**The aspect fit never ran.** Not "ran and got it wrong" — never ran.
`BVNApplyGuestPresentationAspect` was called from the guest thread and
forwarded itself with `dispatch_async(dispatch_get_main_queue())`. Both queued
blocks (one per presentation surface) executed only *after*
`Boxedwine shutdown`, 2.5 minutes later, by which point their surfaces had been
unregistered — the log's only trace is two `Presentation aspect skipped: no
registered view for this surface` warnings at 18:45:08. While `boxedmain` owns
the main thread the main dispatch queue is not drained; SDL's pump services
UIKit events, not GCD. The call moved inside the existing
`DISPATCH_MAIN_THREAD_BLOCK` in `createVulkanSurface`, and the function now
refuses an off-main call loudly instead of deferring it silently forever.

**A second finding from the same log: 17,879 swapchain creations in 150
seconds**, ~119/second, every one 800x600, with acquire and present both
returning `VK_SUBOPTIMAL_KHR`. MoltenVK raises that whenever the layer's
natural drawable size differs from the swapchain extent, and a full-window
Metal view (2622x1206) driving an 800x600 swapchain can never satisfy it. The
letterbox may fix this as a side effect; it is **not verified**, because it
depends on whether DXVK adopts `currentExtent`, and both DXVK and MoltenVK are
binaries here. The per-creation log line (most of the 4.5 MB log) is now a
once-per-second rate report that names the layer's natural drawable size, so
one device run settles it. See section 4 of
`docs/CONTINUING_WITHOUT_A_MAC.md`.

**The in-game overlay** (`ios/runtime/src/BVNGuestOverlay.mm`) is new: a
floating menu button over SDL's window with an on-screen keyboard (function
row, full QWERTY, and latching Ctrl/Alt/Shift that genuinely hold the key
down), a rotation lock, and quit-to-library with in-panel confirmation. It
replaces the SDL-drawn keyboard, which was deleted — that one was drawn by the
SDL renderer, and a Vulkan guest has no SDL renderer, so it was invisible for
the whole session it mattered in.

**Rotation** is now unlockable per session, defaulting to landscape as before.
Unlocking changes both the app delegate's orientation mask and
`SDL_HINT_ORIENTATIONS`; UIKit intersects them, and SDL derives landscape-only
from the guest window's shape without the hint. In portrait the guest is
aspect-fitted into a centred band rather than stretched.

**An app icon**, drawn by `scripts/make-app-icon.swift` (a visual-novel text
box over a 4:3 screen) so the artwork is a reviewable diff rather than an
opaque binary.

Verified locally: full `scripts/build-ios.sh` succeeds, `BoxedVN.app` validates,
88/88 unit tests pass. **Nothing in this entry has been tested on device.**

### 2026-08-09 — build 65: three build-64 regressions, all from one wrong assumption

Device test of build 64 (log `boxedvn20260809195114.log`): the landscape aspect
ratio was correct, and touch had stopped working entirely.

**The assumption:** that resizing SDL's Metal view changes only what is drawn.
It does not. `SDL_uikitviewcontroller.viewDidLayoutSubviews` reports the SDL
window size as `self.view.bounds.size`, and `SDL_uikitview` delivers touches as
`-[UITouch locationInView:]`. Shrinking that view to the letterbox rectangle
therefore moved SDL's entire coordinate space with it, and the pointer
transform subtracted the letterbox offset a second time:

```
iOS Vulkan presentation owns input mapping: window 536x402, ... at 169,0, 67%
iOS SDL mouse down: button 0 at logical 65,121      -> guest x = -155
```

The left third of the picture was dead. Fixed by making the offset
unconditionally zero — a letterbox offset lives in the view's position on
screen, which UIKit removes before SDL ever sees the touch.

**The swapchain storm is answered, and the answer changes the design.** Build
64's rate report: `rebuilt 113 time(s) in 1001 ms (8244 total); layer natural
drawable 834x625` against MoltenVK's `Created 3 swapchain images with size
(800, 600)`. So MoltenVK's suboptimal test really is `bounds x contentsScale`
versus the swapchain extent, and **DXVK does not adopt `currentExtent`** — it
kept asking for 800x600 regardless. Sizing the view in points could never have
fixed it.

Build 65 sets the view's `bounds` to the guest resolution with
`contentsScale = 1.0` and does the scaling with a `CGAffineTransform`. The
natural drawable becomes exactly 800x600, an integer with no rounding to get
wrong — and because SDL reports its window as the view's bounds, SDL's window
becomes the guest resolution and touches arrive as guest pixels. Presentation
and input stop being two things that must be kept in agreement.

**Two smaller build-64 defects, both visible in the user's screenshot:**

- The menu rows whose titles change at runtime rendered blank. On iOS 15+ a
  `UIButtonTypeSystem` button carries a `UIButtonConfiguration`, after which
  `-setTitle:forState:` is not reflected. Panel rows are now
  `UIButtonTypeCustom`. "Quit to library", whose title is only set at creation,
  was unaffected — which is what made the pattern obvious.
- Portrait fitted the picture 278pt wide in a 402pt window, using the
  *landscape* safe-area insets (the log's `at 62.0` is exactly `insets.left`).
  UIKit reports the new safe area separately from the bounds change. The
  overlay now re-fits on `-safeAreaInsetsDidChange`, and the fit discards a
  horizontal inset when the window is taller than it is wide, since an iPhone
  in portrait never has one.

The overlay is also pinned to the window with constraints instead of an
autoresizing mask: a direct window subview has no view controller laying it
out, and a stale frame draws controls in one place while hit-testing them in
another — the most likely explanation for "Quit to library" not responding
after rotating.

Verified locally: build succeeds, app validates. **Untested on device.**

### 2026-08-09 — build 66: rotation, and Grisaia's DirectX failure

Device test of build 65: landscape touch correct and 1:1, portrait picture
filling the width as asked, and **the swapchain storm gone** — zero
`swapchain rebuilt` lines and two creations for the whole session, against
8,244 in a minute on build 64. Confirmed: MoltenVK's suboptimal test is
`bounds x contentsScale` versus the swapchain extent, and DXVK does not adopt
`currentExtent`.

**Rotation was still broken, and the cause was self-inflicted.** The fit called
`-layoutIfNeeded` on the Metal view from inside the overlay's own
`-layoutSubviews`. That view lives under a `UIDropShadowView` — UIKit's hosting
wrapper, which the log named for the first time — so the call walked to the
window and re-entered the layout of a sibling subtree mid-pass. After the first
rotation the subtree stopped being laid out: turning back to landscape produced
no re-fit at all, and in portrait the view sat outside its ancestor's stale
bounds, so UIKit delivered no touches even though the pointer transform was a
correct 1:1.

Fixed by not forcing layout — `CAMetalLayer.drawableSize` is assigned directly,
which is what `-updateDrawableSize` would compute anyway — and, more
importantly, by **polling the window's geometry from
`KNativeInputSDL::processEvents`** every 200 ms. The emulator's own loop runs
for as long as the guest does; a UIKit layout callback demonstrably does not.
The fit also measures against the window rather than the drop-shadow view.

**The Fruit of Grisaia** shows a DirectX error and then faults reading
`0x0000000F` in `grisaia+0x12a2bc`. Cause: DXVK fails `vkCreateDevice` six
times, rejected for `geometryShader`, `shaderCullDistance`,
`robustBufferAccess2` and `nullDescriptor` — exactly the four the MoltenVK
patch relaxes, and exactly the four Saya's DXVK device does *not* request in
the same build. So this title reaches a DXVK path the patch does not cover;
finding and fixing it needs the DXVK source and a mingw rebuild, neither of
which is available on this Mac. wined3d created a Vulkan device fine in the
same session and the engine is Direct3D 9, so build 66 launches Grisaia
without `-dxvk` as a workaround with a clean falsifier.

Verified locally: build succeeds, app validates, 89/89 tests pass.
**Untested on device.**

### 2026-08-09 — build 67: touch again, the status bar, and a right click

Build 66 on device: rotation re-fits correctly in both directions now (the poll
works), and the swapchain storm stayed dead. But touch was gone entirely — and
the log narrows it sharply, because the overlay's own menu kept working while
SDL received **zero** mouse events after the fit.

Two defects, both traceable to removing `-layoutIfNeeded` from the fit:

- **The pointer transform went stale.** SDL reports its window size as the
  Metal view's `bounds`, and it does so from `-viewDidLayoutSubviews`. Without
  the forced layout, `refreshIOSGuestPointerTransform` ran *before* SDL had
  seen the resize and locked in `scale 109%x67%`; SDL then started delivering
  0..800 x 0..600 coordinates and nothing recomputed. The transform is now
  recomputed from the SDL resize event itself, which is the authoritative
  signal.
- The layout flush is restored, but only on the paths that are **not** inside a
  UIKit layout pass — surface creation and the `processEvents` poll. The
  overlay no longer re-fits from `-layoutSubviews` at all; that was what wedged
  UIKit in build 65 and the poll makes it unnecessary.

**The safe-area insets were read from the wrong view.** Build 66 switched to
`UIWindow.safeAreaInsets`; the log shows it reporting *portrait* insets
(l0 r0 t62 b34) while its bounds were landscape 874x402, and the
`UIDropShadowView` reporting the correct landscape ones (l62 r62 t0 b20) in the
same session. SDL's window is created with the legacy `-initWithFrame:` path
and attached to the scene afterwards, so its own safe area lags. Reverted to
the view hierarchy UIKit actually laid out — which is also why the overlay's
menu was sitting under the Dynamic Island.

**The status bar was never actually hidden.** `UIStatusBarHidden` with
`UIViewControllerBasedStatusBarAppearance=false` is a launch-time value modern
iOS stops honouring, and with controller-based appearance off SDL's view
controller could not answer `prefersStatusBarHidden` at all. Controller-based
appearance is now on and the guest window carries `SDL_WINDOW_BORDERLESS`,
which is the flag SDL answers from. That should take the clock and the
"< StikDebug" breadcrumb off the artwork.

**The Grisaia profile never ran.** `BVNRuntimeRequestLaunch` applied the
compatibility profile *before* assigning `enableWineD3DVulkan`, so the one
thing the profile did was overwritten two lines later — the device log still
shows `-dxvk 1` and the same six DXVK device failures. The profile is applied
last now, so the no-DXVK hypothesis gets its first real test in build 67.

Also added: **two-finger tap = right click**, and a throttled hit-test log in
the overlay that names whatever claims a touch, so if touch is still wrong the
next log identifies the swallower instead of another round of inference.

Verified locally: build succeeds, app validates, 89/89 tests pass.
**Untested on device.**

### 2026-08-09 — build 68: the overlay owns pointer input; first performance numbers

Build 67 on device: landscape touch correct, the status bar gone, Grisaia
running for the first time (so DXVK really was what its DirectX check tripped
over). Portrait touch still dead — the fourth build in a row to fail on it.

The hit-test diagnostic added in 67 finally made it unambiguous:

```
Overlay hit test at 284,536 in bounds 402x874 -> passed through to the game
```

That point is inside the picture (y 286..587), the overlay declined it, and
SDL received nothing. So the fault is entirely in UIKit's delivery to a
transformed view inside its own hosting hierarchy, and three builds of
reasoning about that hierarchy have now been wrong.

**The overlay therefore delivers guest pointer input itself.** It knows the
presenting view and `-[UITouch locationInView:]` resolves the scale transform,
so a touch becomes guest pixels with nothing in between to get wrong. When
there is no presenting view - the software-rendered Wine desktop, where SDL
owns a renderer and its own logical-size mapping - touches pass through as
before.

**First performance instrumentation.** `bvnReportPresentRate` logs presented
frames per second and host CPU cores busy every five seconds. Grisaia's
build-67 log has nothing to measure against: no swapchain storm, no interpreter
module, but 3,460 `getrusage fairness activated` transitions on a single Wine
thread over 258 seconds. Whether that thread is the ceiling is exactly what the
cores-busy figure will say - one core pinned means one guest thread to make
faster, several means throughput.

Also: the menu button is draggable and remembers its position as a fraction of
the safe area, so it survives rotation; the menu panel follows it and flips
above it near the bottom edge.

Right clicks: the build-67 logs contain no `overlay right click` lines at all,
so the two-finger tap either was not tried or did not fire. Untested.

Verified locally: build succeeds, app validates, 89/89 tests pass.
**Untested on device.**

### 2026-08-09 — build 69: the fairness throttle was the bottleneck

The frame-rate counter added in build 68 paid for itself on the first run:

```
Saya:    58.2 presented frames/sec; host CPU 0.50 cores busy
Grisaia:  0.4 presented frames/sec; host CPU 0.65 cores busy
```

Saya is fine - 58 fps even while interpreting DXVK, because a visual novel
asks almost nothing of the CPU. Grisaia at 0.4 fps with two thirds of one core
busy is not slow emulation; it is a guest that is **asleep**.

It was asleep in BoxedVN's own code. `GetrusageFairness` slept **1 ms on every
getrusage call** once it decided a thread was spinning, and that is
self-sustaining: a 1 ms sleep keeps the next call inside the 5 ms "rapid"
window, so the thread never leaves the throttled state and is capped at about
a thousand syscalls a second. The log carried 5,084 activation lines for one
thread - each one also a file write from a hot syscall path.

The mitigation was right that a spinning guest thread needs a real scheduling
point. It was wrong about the price: a scheduling point costs microseconds, and
it was spending a millisecond to buy one. The throttle is now **rate-limited in
wall time** - at most one 100 us scheduling point per millisecond, so at most a
tenth of the thread's time, while still delivering a thousand of them a second
where the original livelock had none. Activation is logged once per thread
instead of on every state flap, and the performance line now reports the
throttle's own cost so the next log answers directly whether any remains.

Portrait touch is still broken, and the new report is that in portrait even the
overlay's menu button stops responding - which nothing in the touch path can
explain and which suggests the overlay ends up on a window that is no longer on
screen. Build 69 re-asserts the overlay's attachment whenever the window
geometry changes, logs loudly if it ever finds it stale, and restores the
hit-test diagnostic (removed in 68) with the window and presenting-view state
included.

Verified locally: build succeeds, app validates, 91/91 tests pass.
**Untested on device.**

### 2026-08-09 — build 70: desktop mode, trackpad cursor, and a readable startup screen

Build 69's throttle change is measurably working and measurably *not* the whole
answer:

```
0.4 presented frames/sec; host CPU 0.47 cores busy;
fairness throttle 560 ms across 5,600 scheduling points
```

560 ms per 5,000 ms window is the designed 10% bound, so the mitigation is no
longer the bottleneck - and Grisaia is still at 0.4 fps with the machine 92%
idle. Something else is waiting. The one clue left is that 5,600 scheduling
points a second means a thread is polling continuously, so build 70 counts the
**calls** as well as the throttles: a thread polling a thousand times a second
and one polling a million look identical in the throttle count and need
completely different fixes. `getrusage(RUSAGE_SELF)` in particular takes the
process thread-table mutex and walks every thread, so its call rate matters.

Features this round:

- **Windows desktop mode**, above the games in the library. Launches
  `explorer /desktop=BoxedVN,1280x720` - a real desktop with a taskbar, Start
  menu and Explorer. 16:9 rather than the games' 4:3, so the letterbox is thin
  and the desktop's own edges stay clear of the Dynamic Island. The in-game
  overlay works there unchanged, which also makes it the place to test the
  keyboard against Notepad.
- **Trackpad pointer mode**, in the overlay menu. Direct tap remains the
  default and should: a visual novel is a full-screen tap target and a virtual
  cursor would be strictly worse for it. Trackpad exists for the small Windows
  controls a fingertip cannot hit - drag anywhere to move the cursor, tap to
  click where it is, two fingers for a right click.
- **The startup screen is UIKit text now**, not a bitmap font drawn one
  rectangle per pixel through SDL. It says Wine can take up to three minutes
  and that the app must stay open, which is the thing a player needs to know
  and could not previously read. The spinner is gone; the count of translated
  code blocks does the same job and also distinguishes progress from a hang.

Verified locally: build succeeds, app validates, 91/91 tests pass.
**Untested on device.**

### 2026-08-09 — build 71: the getrusage spin, and an overlay nobody could see

The poll counter added in build 70 named Grisaia's problem in one line:

```
0.4 presented frames/sec; host CPU 0.44 cores busy;
guest polled getrusage 185,630 and sched_yield 92,815 times
```

Per 5 seconds: **37,000 getrusage/sec and 18,600 sched_yield/sec**, an exact
2:1 ratio - a `getrusage; getrusage; sched_yield` spin loop. And
`KProcess::getrusuage(RUSAGE_SELF)` takes `threadsMutex` and walks every thread
in the process on each call, so that loop was ~37,000 lock acquisitions and
~370,000 per-thread time computations a second, all of it contending with
whatever else needed the thread table.

Build 71 caches the RUSAGE_SELF answer **per thread** for one millisecond. No
lock of its own, no shared state, and the result stays monotonic and accurate
to a millisecond - far finer than anything can read from cumulative CPU time.
The guest will still spin; the spin should now be nearly free.

**The overlay was invisible on the software-rendered path, and had been since
build 68.** `BVNAttachGuestWindowToScene` installs the overlay right after
`SDL_CreateWindow` - *before* SDL creates its renderer view, which is then
added to the same window and lands on top. A Vulkan game happened to survive
this because registering the surface re-fronted the overlay; the Wine desktop
and the blue boot screen never did. That is why the menu button was missing
there, and why build 70's new startup text never appeared. The emulator's
own event loop now re-asserts the overlay's place every 200 ms.

The startup notice also could not be hidden: the call that hides it lives in
`putBitsOnWnd`, which runs on the X server's thread, and the setter dropped
off-main requests. Both the visibility and the progress count are now recorded
atomically and applied on the main thread by the same 200 ms pass.

**Desktop mode showed a white rectangle** because `/desktop=BoxedVN,1280x720`
gives a bare desktop window: Wine's explorer only runs the shell - taskbar,
Start menu, icons - when the desktop is named `shell`. Corrected.

Also added: **session logging can be turned off in Settings**, persisted across
launches. On by default, because every diagnosis in this port has come from an
exported log, but the emulator writes from hot paths and a player who just
wants to read a novel should not have to pay for evidence nobody will read.

Verified locally: build succeeds, app validates, 91/91 tests pass.
**Untested on device.**

### 2026-08-09 — build 72: the overlay layout methods, deleted by my own edit

**The menu button was missing because build 70 deleted `-layoutSubviews`.** A
block replacement meant to rewrite the touch handlers spanned four layout
methods as well - `-layoutSubviews`, `-safeAreaInsetsDidChange`,
`-layoutMenuPanelWithSafeArea:` and `-layoutKeyboardPanelWithSafeArea:` - and
nothing caught it, because the overlay still receives a frame from its
constraints and still hit-tests. Only its *subviews* went unpositioned, so
every control kept the zero frame it was created with. The build-71 log says
so plainly in hindsight:

```
Overlay hit test at 656,146 in bounds 874x402 (window attached, ...) -> passed down to SDL
```

The overlay was alive the whole time. Restored, and the hit-test line now
prints the menu button's frame so a zero one is visible immediately.

**Grisaia is bimodal, and the getrusage theory is dead.** Build 71's cache
worked, but the numbers invert the story:

```
30.0 fps; 0.90 cores busy; getrusage 3,709,991 per 5 s   (740,000/sec)
 0.4 fps; 0.46 cores busy; getrusage   186,619 per 5 s   ( 37,000/sec)
```

The *fast* state polls twenty times harder. So the spin rate follows the frame
rate rather than causing it, and in the slow state the whole guest - spin loop
included - is running twenty times slower while using *less* CPU. It is
waiting for something, and which thing decides the fix: a futex, a timer, or a
runnable thread that is not being scheduled all look identical from here.

Rather than guess again, build 72 takes a **thread snapshot automatically when
two consecutive windows report under 2 fps**, at most once every 30 seconds.
Per-thread run state and CPU time at the moment it is happening is what
separates those three.

The 740,000 calls/sec in the *fast* state is worth noting on its own: at
roughly a microsecond of emulator dispatch each, that is most of the 0.90 cores
being spent on a spin loop, so there is headroom there even once the stall is
understood.

**Desktop mode ran nothing visible** because Wine's explorer could not start
its shell - `NtUserChangeDisplaySettings ... returned -2`, `Failed to set
primary display settings` - leaving a bare desktop window, the white rectangle.
It now launches `winefile` into that desktop, which does not depend on the
shell and is what "browse the files" actually needs.

Verified locally: build succeeds, app validates, 91/91 tests pass.
**Untested on device.**

### 2026-08-11 — build 75: Grisaia redraw heartbeat, portrait touch coordinates, and the NDIS timeout

The build-74 Grisaia log kills the build-73 display-refresh theory. In the
slow state the guest calls `vkQueuePresentKHR` only twice in five seconds, and
those calls spend effectively zero milliseconds in present/acquire. During
speed-line and fade animations it submits 19–30 frames/sec through the same
path. The guest is deciding not to redraw; the compositor is not withholding a
drawable. This matches the independently reported CatSystem2/Wine behavior
where Grisaia falls to 0.2 fps until mouse movement generates window activity.

Build 75 adds a title-scoped `-x11MotionHeartbeat` profile for `BootMenu.exe`
and `Grisaia.exe`. The emulator main loop sends an unchanged X11
`MotionNotify` every 33 ms. It does not move the pointer and no other title
receives the pulse. The next device log must contain `iOS X11 motion heartbeat
active at 30 Hz for this title`; only its measured FPS can establish success.

Portrait input had a separate deterministic transform bug. UIKit's
`locationInView:` already returns guest pixels because the presenting Metal
view has guest-sized bounds. `BVNGuestControlsSendPointer` then passed those
pixels to `KNativeInputSDL`, which treated them as host-window points and
applied the letterbox transform a second time. In portrait that maps ordinary
taps to an edge of the 800×600 guest. The bridge now applies the inverse before
using the existing input API, for left click, motion, and two-finger right
click. Overlay menu open/close is logged so the next report separates a UIKit
control failure from guest-coordinate delivery.

The rotation log does not show a rotation freeze: landscape settled in 84 ms
and SDL created an 874×402 window. It shows the same cold-Wine gap as every
other launch. In three logs JIT allocation progresses through block 320, Wine
reports `Auto-start service NDIS failed to start: 1053`, and allocation 384
arrives 57–59 seconds later. Boxedwine supplies Wine's interface data through
`nsiproxy`/netlink and has no NDIS kernel driver, so prefix preparation now
sets only `Services\\NDIS\\Start=4`; Wine services, `nsiproxy`, and `winebus`
remain enabled. Existing prefixes are repaired before every launch.

Also corrected the iOS plist key from the nonexistent
`CADisableMinimumFrameDuration` spelling to Apple's
`CADisableMinimumFrameDurationOnPhone`.

**Local evidence:** `git diff --check` passes. The focused MSVC launch-profile
test passes. The full Windows preset reaches the compiler but the repository's
vendored zlib configuration unconditionally declares `HAVE_UNISTD_H=1`, so
MSVC fails before compiling the tests; Ubuntu CI remains the portable suite and
macOS CI remains the iPhoneOS compiler/package gate. Build, unsigned IPA, and
all physical-device behavior are pending CI/device validation.

### 2026-08-11 — build 76: shared partial presents, rotation touch reset, and entitlement packaging

The build-75 Grisaia log contains the expected `iOS X11 motion heartbeat
active at 30 Hz` line while static dialogue remains at 0.2–0.6 fps. That
falsifies the event-heartbeat workaround, which is now removed end to end.
The same log switches immediately to 17–35 fps for animations while acquire
and present calls remain near zero milliseconds, so a generally slow or broken
JIT does not fit the observed boundary.

Wine's upstream fix for Grisaia deliberately routes partial `COPY` presents
through a GDI blit because a GL/Vulkan backbuffer is undefined after swap.
Boxedwine's `XServer::draw`, however, returned immediately whenever the SDL
window had `SDL_WINDOW_VULKAN`: it neither displayed nor consumed the X11
dirty window. The dirty bit then stayed latched, so later writes could not even
wake the compositor. Build 76 tracks the precise dirty rectangle for every
X11 image/copy/fill, consumes it while Vulkan owns the window, and places only
those opaque pixels in a transparent UIKit layer over the Metal frame. The
next successful full Vulkan present clears the patch layer. This is shared
Boxedwine presentation behavior; it contains no executable-name check or
synthetic frame timer. Device acceptance requires the new `Composited X11
partial present` log and visibly normal dialogue updates.

The portrait log proves the overlay remains attached, correctly sized, and
hit-tested after rotation, so build 75's coordinate change could not fix the
remaining failure. Build 76 treats it as stale UIKit tracking: after every
settled geometry change it clears the retained guest touch, cancels all
`UIControl` tracking, resets gesture recognizers, releases held keys, and
closes the menu. The initial portrait-to-landscape request is also deferred by
one main-queue turn so the SwiftUI launch touch completes before scene geometry
changes. Both live rotation and portrait launch remain device acceptance items.

MeloNX and BoxedVN both request
`com.apple.developer.kernel.increased-memory-limit`; the difference was the
artifact. BoxedVN's CI explicitly removed code signing, and Apple entitlements
live in the code signature, so GetMoreRAM had no request to preserve. CI now
keeps `BoxedVN-unsigned.ipa` genuinely unsigned and separately creates
`BoxedVN-entitlements-ready.ipa` with an ad-hoc signature containing
`get-task-allow` and the increased-memory request. The latter must still be
re-signed with the same App ID/Apple ID configured in GetMoreRAM, then verified
from BoxedVN's Memory row on device.

The host-independent preset is now genuinely Windows-buildable: zlib no longer
claims `<unistd.h>` on MSVC, test warning flags use MSVC spellings, and the two
PID-based tests use `_getpid`. The complete Windows test executable builds and
CTest passes 1/1. GitHub Actions run 31536075940 then passed the iPhoneOS
compile and validated both packages: unsigned SHA-256
`5a4e8f395ca8dfac914844daca9683ab999b179e88462f715f49d36484314dc7`
and entitlement-template SHA-256
`4b57b2edaea6fd80a324f497c44d2c2696e1a50661ff44e249a33386504bbf25`.
All physical-device behavior remains pending device evidence.

### 2026-08-11 — build 77: repair clean-prefix renderer selection

The normal and entitlement build-76 logs ended on the same fatal core path.
Both said `wined3d_dll_init Using the OpenGL renderer`, resolved the GLX entry
points, and then stopped at `Uknown int 99 call: 2897`. Index 2897 is
`kXChooseVisual`; the iOS build intentionally has no host OpenGL dispatcher, so
this was an emulator abort rather than an idle blue screen. Neither log reached
`Registered Vulkan surface`, which also excludes build 76's new
X11-over-Vulkan compositor as the cause of this startup failure.

The latent caller bug was that `BVNRuntime` used the `enableWineD3DVulkan`
field for two independent choices: whether to force WineD3D's Vulkan renderer
in the prefix, and whether to mount the patched DXVK DLLs. Grisaia's profile
correctly turned the latter off, but on a clean/recreated prefix that also left
Wine's default OpenGL renderer selected. The earlier build-75 prefix already
contained `renderer=vulkan`, which is why the same profile happened to launch
there.

Build 77 adds a separate `useWineD3DVulkanRenderer` policy. Imported Wine games
always set it before title profiles run; Grisaia then disables only DXVK. Prefix
preparation therefore writes `renderer=vulkan` on both clean and existing
prefixes while leaving Boxedwine's `-dxvk` switch absent for this Direct3D 9
engine. A host regression test locks that ordering and independence down.

The entitlement log independently closes the packaging question: its installed
signature reported `Increased limit signed; 5.99 GB available`, while the
normal IPA reported `Standard limit; 3.29 GB available`. The renderer abort is
identical in both logs and unrelated to the entitlement.

GitHub Actions run 31540593751 passed the host tests in 19 seconds and the
iPhoneOS build plus both packages in 2 minutes 37 seconds. The unsigned IPA is
SHA-256
`8b2375b4dcc595f64a16fee5762e42b22c3f9e0fb8ff2a015139f0ca0511ab38`;
the entitlement-ready IPA is
`a1cb5616ed4cb67b6e16d8499c9fac6f630727f624aa73af4a1d5fd2f2025117`.
This proves compilation and packaging only; launcher recovery still needs the
next physical-device log.

### 2026-08-11 â€” build 78: immediate partial presents and rotation-safe input

The corrected Grisaia logs are `boxedvn-20260811-173422.log` and
`boxedvn-20260811-174039.log`; the previously examined portrait log was Song
of Saya. Both build-77 Grisaia sessions reach the same 1024x768 WineD3D/Vulkan
surface. The first spends long periods at 0.2-0.8 Vulkan presents per second;
the second has the same static cadence but jumps to 8-10 fps during animation.
`vkQueuePresentKHR` normally consumes 0-1 ms, CPU use remains under one core,
and the principal guest threads sleep in `pselect6_time64`. This excludes a
slow ARM64 JIT translator and GPU back-pressure as the frame-time cause.

During those same static periods Wine logs repeated `swapchain_blit_gdi` work
and Boxedwine logs hundreds of `Composited X11 partial present` updates. Build
76 copied each update into a transparent UIImage layer, but Core Animation
could defer the transaction until the next UIKit/Vulkan-driven commit. Build
78 explicitly flushes that transaction after each partial update. This is a
shared mixed GDI/Vulkan presentation repair with no game-name check. A device
test must verify that dialogue now becomes visible immediately; the Vulkan FPS
counter may correctly remain low because these repaired updates are not Vulkan
presents.

The logs also show Grisaia itself creates a 1024x768 (4:3) surface, which is why
the aspect-preserving presenter uses only 536 of the phone's 874 landscape
points. The in-game menu now has a persistent `Display: fit aspect/fill screen`
switch, providing the requested 16:9 fill without silently distorting every
4:3 game. Pointer settings in the same menu persist opacity, size, outline and
shadow opacity, thickness, and outer/inner-circle visibility.

Trackpad/direct pointer routing no longer requires a Vulkan presentation view.
For Wine's software-rendered blue desktop it maps the overlay's normalized
coordinates to Boxedwine's current guest screen, so trackpad mode has a visible
cursor and can click Winefile. All games and the built-in tools now mount
`Documents/Shared` as E:, and Browse PC opens that drive. Per-game Wine prefixes
remain separate because the build-73 shared-prefix experiment hid existing
saves and would also share incompatible registry/DLL overrides between games.

For rotation, cancelling recognizers and briefly toggling interaction was not
enough to make UIKit abandon the pre-rotation delivery chain. Every settled
geometry change now removes and reconstructs the lightweight overlay on the
current SDL UIWindow, preserving the menu position, pointer preferences and
startup notice. The software-path input fallback also keeps the overlay able
to deliver touches while a Vulkan surface is temporarily absent. Both live
rotation and launching from portrait remain device acceptance items.

The JIT status screen now says `Confirmed` whenever CS_DEBUGGED reports a
debugger attachment and removes the separate guest-launch confirmation row.
The two Documents-folder import sections are removed. CI produces only
`BoxedVN.ipa`, containing the increased-memory entitlement request, and uploads
it to the rolling prerelease tag `ios-latest` so it can be downloaded directly
instead of extracting an Actions artifact ZIP. Build logs remain Actions
artifacts.

The two corrected logs show virtually identical approximately 102-second cold
startup gaps. The gap is dominated by Wine/.NET/process initialization around
sleeping guest threads, not sustained JIT compilation; a reusable translated
block cache would need relocation and invalidation machinery Boxedwine does not
currently have and is not safe to improvise in this batch. Selecting the final
game executable instead of its launcher remains the only low-risk per-library
way to skip launcher work when a title supports it.

**Build evidence:** `git diff --check` passes. After loading Visual Studio's
developer environment, the Windows preset rebuilt successfully: all 92 C++
tests pass and CTest passes 1/1. The pin script needs Bash and was not run on
this Windows host. GitHub Actions run 31544859678 passed the full Ubuntu test
suite and the macOS iPhoneOS build in 3 minutes 33 seconds. Its rolling
`ios-latest` Release contains `BoxedVN.ipa` (8,152,639 bytes, SHA-256
`dc05e179d063e60bc973ef1c83a2e716328ab8b52e856edfd3a7a0794be542f9`), and a
direct HTTP request returns that IPA as an attachment. Every new device
behavior is pending a fresh build-78 log and physical-device test.

### 2026-08-11 — build 79: coherent mixed presents and one X11 pointer space

The build-78 device log reaches Grisaia's requested 1280x720 surface and
confirms that presentation geometry itself is internally consistent: the Metal
view has 1280x720 bounds and drawable size, SDL reports 1280x720, and the
pointer transform is 1:1 in Fit, Fill, landscape, and portrait. Fit shows the
surface at 714.7x402 points; Fill uses the full 750x402 safe-area width. The
game also sustains about 58 Vulkan presents/sec once active. The earlier 0.4
fps samples remain guest-selected static cadence—present/acquire consume at
most 1 ms—and are not evidence of JIT translation or GPU back-pressure.

The screenshot's vertical cut is exactly at guest x approximately 720 on the
1280x720 surface. Build 78 made only each X11 dirty rectangle opaque above the
Vulkan frame. A full-screen WineD3D GDI COPY update that dirtied 720 pixels of
width therefore displayed one X11 snapshot to the left and a differently timed
Vulkan snapshot to the right. Build 79 detects a broad update from an X11
window that covers the presentation and publishes its complete backing store
as one coherent image; small updates and real child windows remain bounded to
their dirty rectangles. The first twelve
updates now log window, dirty, published, and guest rectangles so the next
device run can confirm the exact producer without flooding the log.

The right/bottom pointer error was a separate Boxedwine core ordering bug.
`XServer::mouseButton` converted fake-fullscreen game-local coordinates to X11
root coordinates before choosing the target window. `XServer::mouseMove`
chose an overlapping Wine window first and converted only afterward. Hover and
click could consequently address different windows, matching the screenshot
where the cursor is over Back while a save-data box highlights. Motion now
converts before hit-testing, the same as button input. Because the overlay gets
guest pixels directly from UIKit's transformed Metal view, this single X11
fix applies to Fit, Fill, landscape, and portrait without a second display
formula.

Trackpad mode now discards the first movement sample of every new gesture so a
lift/re-touch cannot nudge the cursor. A 350 ms stationary hold sends button
down; subsequent movement drags, and release/cancel/rotation always sends the
matching button up. The virtual cursor is hidden whenever the Wine startup
notice is visible and restored after that notice leaves.

**Build evidence:** `git diff --check` passes, the Windows support executable
runs all 92 tests with zero failures, and CTest passes 1/1. GitHub Actions run
31547397032 then passed the same 92 tests plus the full iPhoneOS compile, app
validation, increased-memory entitlement check, packaging, smoke test, and
rolling Release upload in 3 minutes 14 seconds. The downloaded `BoxedVN.ipa`
contains build 79, is 8,154,693 bytes, and matches both the runner and Release
digest: SHA-256
`e8f5ac822312507662bd9b8f8616b598c011890faa0035e1c0b64120f5c31679`.
Its direct URL returned HTTP 200 with `Content-Disposition: attachment`.
Physical Fit/Fill, portrait input, pointer targeting, trackpad dragging, and
mixed-present rendering remain pending a fresh device test.

### 2026-08-11 — build 80: client-aware mixed presents and direct fake-fullscreen input

The build-79 logs are `boxedvn-20260811-184649.log` and
`boxedvn-20260811-185325.log`. The Grisaia run registers the real Vulkan client
as X11 window `0x10134`, 1280x720 at root position `(3,29)`. Immediately around
it, the X11 fallback reports a decorated 1286x752 parent at `(0,0)`. Build 79's
broad-update heuristic promoted that parent's entire backing store as a
1280x720 overlay. This exactly explains the screenshot: source row zero is the
Wine title bar, and the backing pixels to the right of the game's 960x720 COPY
region are black. Opening the native menu caused a fresh Vulkan present to
clear the bad overlay; closing it allowed the next X11 repaint to expose it
again.

Build 80 removes full-parent promotion. `XWindow::draw` now sends mixed-present
updates only when the dirty window is the active Vulkan client, an ancestor,
or a descendant. The iOS compositor uses the active client's root rectangle as
the coordinate origin and rejects a decorated ancestor update if any part
escapes that client. A complete client-contained logical frame such as the
observed 960x720 COPY is scaled coherently to 1280x720, and its scale is reused
for later partial dialogue updates. This preserves the immediate GDI text path
without copying decorations, a black unused strip, or an unscaled vertical
slice over the Vulkan frame.

The pointer failure is below UIKit. In landscape the active client is already
wider than Boxedwine's 800x600 root; in the second log it is 1280x960 at
`(-3,23)`. After unlocking rotation, the overlay is rebuilt at 402x874 and
continues logging portrait hit tests, proving the responder chain is alive.
Boxedwine nevertheless converted those guest pixels to root space and asked
the smaller root to choose a window again. Points toward the right/bottom can
therefore select an overlapping Wine child or fall outside the root. Motion
and button dispatch now start at `fakeFullScreenWnd` and bubble to its parents;
ordinary desktop sessions still use root hit-testing. The same change covers
direct touch, trackpad click, Fit, Fill, and post-rotation input.

Pointer settings now expose a persistent trackpad sensitivity slider from
0.5x to 3.0x, defaulting to the previous fixed 1.4x movement. The existing
appearance controls and gesture semantics are unchanged.

**Build evidence:** `git diff --check` passes. The Windows host-independent
executable runs all 92 tests with zero failures, and CTest passes 1/1. GitHub
Actions run 31549007709 passed the same host suite plus the macOS iPhoneOS
compile, app validation, increased-memory entitlement check, packaging, smoke
test, and rolling Release upload in 4 minutes 45 seconds. The published build
80 `BoxedVN.ipa` is 8,155,666 bytes; a fresh Release download matches the
runner and published checksum, SHA-256
`4027e331c26f7c653c487e578942e111f97f0a60c0fd4de6985646170a01e67d`.
The Release targets `cbdce90013a6cb01bb3cc6c6b404c0d7095e9fad` and the
asset lists `Payload/BoxedVN.app`, including `_CodeSignature/CodeResources` for
the ad-hoc entitlement template. Physical rendering and input acceptance
remain pending build 80.

### 2026-08-11 - build 81: one client coordinate space for rendering and input

The build-80 reports are `boxedvn-20260811-191321.log` and
`boxedvn-20260811-191651.log`. Both independently register Grisaia's final
Vulkan presentation client as 1280x720 at X11 root position `(3,29)`, and both
show a decorated 1286x752 ancestor repeatedly presenting only the client
rectangle `(3,29) 960x720`. Build 80 labelled those dirty bounds a 960x720
"logical frame" and published them as 1280x720. That 4:3 horizontal expansion
is the direct source of the enlarged title screen and the missing right edge.
A dirty rectangle says which pixels changed; it cannot establish a second
framebuffer resolution.

The compositor no longer stores or applies a dirty-derived logical size. It
maps every legitimate update through the active client's live 1280x720 extent.
The specific incoherent ancestor case - a `(0,0)` client-relative update that
covers the full height but stops before the active right edge - is rejected
instead of stretched or left as a vertical seam over the Vulkan image. Bounded
diagnostics report those rejections so the next device log can prove whether
Wine produces a corrected full-width update afterward.

The pointer symptom has the same stale-width signature: in the screenshot the
native cursor is over QL near guest x=1090 while the game highlights 05 near
x=817, approximately `1090 * 960 / 1280`. `createVulkanSurface` resized SDL and
`KNativeInputSDL` to the active client but did not resize Boxedwine's X11 root.
Direct delivery therefore still gave Wine root coordinates whose declared
desktop size could be the preceding 960-wide mode. Build 81 synchronizes the
virtual X11 root to contain the fake-fullscreen client without resizing SDL a
second time. The original root size is restored when that client is destroyed.
Button diagnostics now include the X11 root extent, making this invariant
observable in the next log.

**Build evidence:** `git diff --check` passes. The Windows host-independent
executable runs all 92 tests with zero failures, and CTest passes 1/1. GitHub
Actions run 31550815893 passed the same host suite plus the full iPhoneOS
compile, app validation, increased-memory entitlement check, packaging, and
rolling Release upload in 2 minutes 49 seconds. The published build 81
`BoxedVN.ipa` is 8,156,671 bytes and its downloaded SHA-256 matches the Release
digest and sidecar:
`0f6b2866f393e46b89c506ec28af58976593b1829cc3a984b174812df9f2fb5a`.
The direct URL returns HTTP 200 and `Content-Disposition: attachment`, and the
rolling Release now targets the exact producing commit
`5b747a51c9641d78e19a9da07c66d6dea1966fd9`. Physical rendering/input
acceptance remains pending build 81.

### 2026-08-11 - build 82: synchronize Wine's live Xlib screen records

Build 81 fixed the resolution, which is now device-confirmed. The remaining
input reports are `boxedvn-20260811-194259.log` and
`boxedvn-20260811-194901.log`. They disprove another UIKit or presentation
transform error: the overlay and SDL are both 1:1 at 1280x720, the X11 root is
successfully expanded to 1283x749 around the client at `(3,29)`, and taps at
guest x=1021, 1117, 1202, 1241, and 1250 all reach the correct fake-fullscreen
X11 target unchanged. Nevertheless, the game still stops responding on the
right near x=800.

The remaining 800-pixel boundary comes from `XServer::createScreen`. Every
Wine X11 connection gets an Xlib `Screen` struct whose width/height are copied
only once, while the desktop is still 800x600. Build 81 resized the root
`XWindow`, but never updated those already-open guest-memory records. Wine can
therefore continue clipping or translating absolute cursor positions against
an 800-pixel screen while Boxedwine delivers positions across a 1280-pixel
client.

Build 82 adds one screen-size synchronization path. It updates width, height,
and physical dimensions in every live Xlib `Screen` whenever Boxedwine changes
the desktop, enters fake fullscreen, or restores the desktop. Fake fullscreen
publishes the active client size (1280x720), which is the same coordinate space
used by UIKit, SDL, pointer injection, and the Vulkan presentation.

`KNativeInputSDL::getMousePos` also had its fake-fullscreen conversion reversed:
it called `screenToWindow` on coordinates that were already client-local, then
`XQueryPointer` subtracted the client origin again. It now validates once in
client space and calls `windowToScreen` exactly once before X11 reports root
and window coordinates. The iOS `XWarpPointer` path preserves the same
client-local invariant. Bounded `XQueryPointer` diagnostics now report root,
local, Xlib-screen, and fake-client dimensions for the next device log.

**Build evidence:** `git diff --check` passes. The Windows host-independent
executable runs all 92 tests with zero failures, and CTest passes 1/1. GitHub
Actions run 31552046685 passed the same host suite plus the full iPhoneOS
compile, app validation, increased-memory entitlement check, packaging, and
rolling Release upload in 2 minutes 30 seconds. The published build 82
`BoxedVN.ipa` is 8,157,877 bytes and its downloaded SHA-256 matches the Release
digest and sidecar:
`289a416a643ac550548a1f794ddbb73508eac796b0f33976288025e1407e6542`.
The rolling Release targets the exact producing commit
`821854e1646d56fd242787675cc0d16b7f250573`. Physical right-side input
acceptance remains pending build 82.

### 2026-08-11 - build 83: start default game sessions on a coherent 1280x720 Wine monitor

The device report and `boxedvn-20260811-200729.log` show that build 82 did not
fix right-side input. Its new diagnostics isolate the remaining transform:
UIKit hit tests reach screen x=783, the native overlay maps trackpad positions
as far as guest x=1227, Boxedwine targets the active 1280x720 client directly,
and `XQueryPointer` reports a 1280x720 Xlib screen. The game nevertheless acts
near x=767, which is the same 800/1280 compression seen in the UI.

The launch command in that same log contains no `-resolution`, so Wine starts
and caches its virtual monitor at Boxedwine's 800x600 default before Grisaia
later creates its 1280x720 client. Changing Xlib's live screen record after the
game opens is too late for Wine's already-built Windows monitor geometry.

Build 83 fixes the ordering generically in `BVNBuildLaunchArguments`: every
imported Wine game left on the launch setting `default` now receives
`-resolution 1280x720` before Wine starts. This is not keyed to Grisaia or an
executable name. It gives all default game sessions one coherent 16:9 monitor
before Wine caches display metrics. A user-selected resolution such as
1024x576 remains authoritative and is emitted exactly once. Wine tools retain
their explicit sizes.

**Build evidence:** `git diff --check` passes. The host-independent Windows
suite now runs 93 tests with zero failures, including a new explicit-resolution
precedence regression test, and CTest passes 1/1. GitHub Actions run
31553074737 passed the same support suite plus the full iPhoneOS compile, app
validation, increased-memory entitlement check, packaging, and rolling Release
upload in 2 minutes 25 seconds. The published build 83 `BoxedVN.ipa` is
8,158,088 bytes; its downloaded SHA-256 matches the Release digest and
sidecar: `e1d1c70107c8c4142386782e74547dff202f74e114ed7b1b7ba1fcc46945b627`.
The rolling Release targets the exact producing commit
`e09a9ab2421530b9d9ab32807b3a49627ecf880f`. Physical right-side input
acceptance remains pending build 83.

### 2026-08-11 - build 84: coherent phone mode, mixed-present persistence, and runtime controls

The build-83 device logs separate the remaining high-resolution input problem
from UIKit. At 1280x720, the overlay, SDL, the X11 root, every live Xlib
`Screen`, and the active Wine client all receive the requested coordinates
unchanged as far as x=1196. Grisaia still exposes a narrower logical hit-test
range, while its own 1024x576 mode is device-confirmed to reach the final
controls. Generic game sessions left on `default` therefore now start Wine at
1024x576. This remains a title-independent 16:9 policy; explicit 1280x720 is
still available for DPI-aware games. Browse the PC uses the same mode, making
Wine's non-antialiased desktop text more legible on a phone.

The software Wine desktop no longer estimates cursor coordinates from the
whole UIWindow. The UIKit overlay converts both direct touches and the visual
trackpad cursor through SDL's actual logical renderer viewport with
`SDL_RenderWindowToLogical` / `SDL_RenderLogicalToWindow`, including its
letterbox. This removes the small visual-cursor versus click offset and stops
black-bar taps from being clamped onto a guest edge.

`fps-drop.log` rules out RAM exhaustion and a blocked GPU: the signed process
has 5.99 GB of headroom, the JIT arena still has over 60 MB free, host present
calls take only milliseconds, and Wine continues publishing X11/GDI partial
updates while Vulkan falls to 0.2-0.6 presents/sec. The mixed presenter was
clearing those newer patches on every sparse Vulkan present. It now preserves
the patch layer during sparse/static cadence and clears it only after two
nearby Vulkan presents prove a full-frame animation stream has resumed.

The 1 GB value shown inside both games was not an iOS limit: Boxedwine
hard-coded it in Linux `sysinfo()` and `/proc/meminfo`. On iOS these now report
a live host-backed budget capped at 3 GB, which is the safe ceiling for the
32-bit Wine address space. The optional in-game performance overlay is
draggable and displays FPS, process RAM used/available total, frame-time, and
battery; each metric has an independent persistent switch.

Live rotation was removed from the in-game menu. Settings now selects one
whole-app orientation (portrait, landscape, or landscape flipped), and UIKit
and SDL receive the same single-orientation policy before Wine starts. This
avoids replacing the active responder/Metal hierarchy during a session.

**Local evidence:** `git diff --check` passes. In the Visual Studio developer
environment the host-independent suite builds successfully, runs all 93 tests
with zero failures, and CTest passes 1/1. iPhoneOS compilation and physical
input/FPS acceptance remain pending build 84.
