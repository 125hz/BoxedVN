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

| | macOS arm64 | iOS 26/27 arm64 |
|---|---|---|
| Allocation | `mmap(..., MAP_JIT)` | a bounded set of `mmap(r-x)` segments, each followed by StikDebug's universal prepare-region request (`x16 = 1`, `brk #0xf00d`) |
| Region behaviour | one mapping, per-thread W xor X | two mappings of the same pages: permanent `r-x` execution and `rw-` generation aliases |
| `pthread_jit_write_protect_np` | required around writes | **unavailable; not used** |
| Gate | `com.apple.security.cs.allow-jit` at signing time | kernel-set `CS_DEBUGGED` **plus** an active StikDebug universal script that prepares pages through debugserver/TXM |
| Instruction cache flush | `__builtin___clear_cache` works | `sys_icache_invalidate` on the `r-x` address |

The last row is a separate link-time trap: `__builtin___clear_cache` lowers to
a call to compiler-rt's `__clear_cache` on AArch64, and compiler-rt
deliberately does not build that file for Darwin. An iOS link fails with an
undefined `___clear_cache`. `platform/linux/platform.cpp` routes both
`writeCodeToMemory` overloads and `clearInstructionCache` through one helper
that calls `sys_icache_invalidate` on iOS.

### The probe — split into a safe status check and an external handshake

`ios/runtime/src/BVNJIT.mm` exposes **two** entry points, not one, because the
universal prepare-region operation intentionally stops the target at an ARM64
breakpoint. It must never happen from a timer or cold-launch status refresh.

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

`BVNJITProbeExecute()` — **gated to the deliberate guest-launch path:**
1. refuse immediately unless `CS_DEBUGGED` is set
2. `mmap` the first arena segment `r-x`, with no `MAP_JIT`
3. issue StikDebug's universal prepare-region request (`x16 = 1`,
   `brk #0xf00d`); its active script services the stop and prepares every
   16 KiB page through debugserver/TXM
4. `vm_remap` the same pages to a second address and make only that alias
   `rw-`
5. write three ARM64 instructions through the `rw-` alias, invalidate the
   instruction cache for the `r-x` address, and call the `r-x` address
6. prepare the remaining segments the same way, still inside this one
   deliberate handshake, stopping early and keeping what succeeded if the
   kernel refuses one
7. retain those proven dual mappings and suballocate Boxedwine's 64 KiB code
   blocks from them; live guest execution does not issue another debugger stop

How many segments, and therefore how much total capacity, comes from
`BVNPlanJitArena` (`ios/runtime/src/BVNJitArenaPlan.h`): an eighth of the
smaller of `os_proc_available_memory()` and the device's RAM, with a 128 MiB
floor and a 512 MiB ceiling. The floor is the capacity every device result up
to build 89 was measured against. The ceiling exists because the guest needs
the rest: `BVNGuestReportedTotalMemory` advertises up to 3 GB to a 32-bit
Wine, and an arena that crowds that out trades one failure for another.

The old single fixed 128 MiB was sized for one Direct3D 9 title plus a fully
attached WineDbg. It is not enough for a guest that runs a browser engine:
those start six to ten guest processes, each translating its own copy of the
engine, and exhaust it mid-launch. Every allocation after that point fails and
the guest wedges.

Only called from `BVNRuntime.mm`'s `runSession()`, immediately before
`boxedmain()` actually starts a guest. That is a deliberate, user-initiated
moment (the user pressed Launch or Run Wine Notepad), and Boxedwine's own JIT
would hit the identical risk moments later regardless — so nothing is made
safer by skipping the check there, but tying a possible crash to a specific
action the user just took is far better than a silent crash on every cold
launch.

Build 27 temporarily installs a SIGTRAP recovery point around only step 3.
StikDebug sees the Mach exception first, so a functioning universal script
still prepares the region and resumes normally. If a reinstall has lost the
script assignment and the deliberate breakpoint falls through to POSIX signal
delivery, the probe recovers, unmaps the unused arena and returns a diagnostic
instead of letting iOS terminate the process. The previous handler is restored
immediately; unrelated traps retain fatal behavior.

`probeJitWithTimeout()` runs the handshake on a worker and stops waiting after
six seconds. That covers a debugger leaving the target stopped. The narrow
SIGTRAP recovery covers the distinct failure where the breakpoint is not
consumed at all. Together they handle both observed missing-script outcomes.

`BVNExecMemory` owns the retained RX/RW arena. `Platform::alloc64kBlock`
suballocates and returns an RX address to Boxedwine, while both
`writeCodeToMemory` callback forms receive the translated RW alias. Cache
invalidation and calls continue to use the original RX address. Returning a
block coalesces it into the arena rather than unmapping part of the shared
region. This removes both the old process-wide `mprotect` race and the need to
park Wine repeatedly at StikDebug breakpoints while it builds its code cache.

Builds 31–32 add a bounded compatibility seam without changing that JIT
contract. `BVNLaunchArguments` may emit repeated `-interpreterModule` fragments
or inclusive-start/exclusive-end `-interpreterRange` values for a known game
profile. During block decode, `NormalCPU` checks ranges first, then resolves a
mapped module before taking the memory-cache lock only when name selectors
exist. Matching blocks receive `OP_FLAG_NO_JIT`; every other Wine, WineD3D and
game block follows the normal ARM64 JIT threshold. Build 31 proved Wine's PE
`mvware` image is `Unknown` to Boxedwine's Linux mapped-file table, so build 32
selects Saya's evidence-bounded `0x7F300000–0x7F500000` range instead. That
covers both observed mvware EIPs without adding name-lookup overhead during
Wine startup. The first activation is logged. This is not a global interpreter
mode and cannot substitute for the StikDebug arena.

Build 34 proved why an address-range interpreter is diagnostic rather than a
shipping solution: the final Saya surface waited while 25 million decoded
instructions ran inside one 64 KiB window. That evidence led to the generic
JIT implementation in `Jit::movsr`. The optimized eight-byte `REP MOVS` path
falls back to scalar copies for overlaps shorter than eight bytes, because a
wide read/write would change x86 byte-propagation semantics. The old fallback
loop branched on the invariant source/destination delta; a nonzero four-byte
overlap therefore ran forever and underflowed `ECX`. Build 35 branches on
`ECX` in both directions. Saya no longer emits an interpreter module or range,
so its full engine uses the ordinary ARM64 JIT while the compatibility seam
remains available for future evidence-bounded diagnostics.

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

The SDL window requests `SDL_WINDOW_ALLOW_HIGHDPI`, so Metal's drawable uses
the screen's native scale rather than one blurry pixel per UIKit point. A
guest launch does not immediately call `boxedmain()`: while the ordinary UIKit
run loop is still active, `BVNPrepareGuestPresentation` requests landscape and
waits until both `UIWindowScene.interfaceOrientation` and the library window's
bounds agree. Only then is SDL allowed to create its `UIWindow` and
`CAMetalLayer`. The app delegate and SDL orientation hint remain
landscape-only until the session returns, avoiding live drawable replacement
while Boxedwine owns the main thread. As a resize safety net, every present
still compares renderer output dimensions and reapplies the logical guest
size. SDL uses that same viewport for mouse-event transforms.

Build 18 uses UIKit's scene lifecycle even though SDL still owns
`UIApplicationMain`: `BVNSceneDelegate` creates the SwiftUI library window
with `initWithWindowScene:`, and immediately after `SDL_CreateWindow` the app
attaches SDL 2.32.10's legacy `initWithFrame:` window to that same scene. The
attachment happens before renderer creation and before either window is
shown. This is required on current iOS for window-owned services such as the
software-keyboard scene and the document picker's remote view controller;
merely having a visible/key legacy window was not sufficient.

The same renderer owns guest-adjacent UI. Before Wine maps its first X11
window, `KNativeScreenSDL` draws a startup screen into the 800x600 logical
surface immediately after making SDL's window visible; presenting while the
window is hidden can discard the Metal drawable on iOS. This happens before
SDL's event loop, while startup is already executing on UIKit's main queue, so
the one initial show operation calls SDL directly. Routing it through
Boxedwine's synchronous `sdlDispatch` would wait for an event loop that cannot
yet service the callback, which was build 13's deadlock. The first
`putBitsOnWnd` call is a mapped `InputOutput` descendant of XServer's root, so
it deterministically ends loading. The screen is intentionally static while
Wine owns the pre-main-loop thread: SDL cannot safely render from a timer
thread, and queueing timer events while the main loop is blocked caused build
12's callback crash. Early allocation-only logs incorrectly mixed translation
with a fixed cold-initialization delay. Build 23's Wine timestamps place it
between NDIS completion and Winedevice2. Build 24 proves disconnecting winebus
does not change it, while a second guest in the same process starts in seconds.
Winebus is restored in build 25. The remaining cold work is not represented as
a fabricated progress bar.
After startup, SDL draws and hit-tests a **KEYBOARD** button in the same
logical coordinate space as guest input.
UIKit does not place views over SDL: `boxedmain()` owns the main thread, so a
UIKit overlay cannot reliably commit or deliver control actions during the
session. Builds 16-19 changed SDL text-input state and also called
`showKeyboard`/`hideKeyboard` on SDL's own UIKit view controller. Build 16
proved those calls happened but current iOS did not promote SDL 2.32.10's
hidden, zero-sized field to a visible software-keyboard session. Build 17
therefore retrieves that pinned SDL field, attaches it to the active guest
view, and makes it a 1x1 almost-transparent first responder before verifying
`isFirstResponder`. It does not create a competing field: characters are still
emitted by SDL's own `UITextField`, and `SDL_TEXTINPUT` events are translated
into the X11 key transitions Boxedwine consumes. UIKit did-show/did-hide
notifications are logged separately from the synchronous responder result.
The build 17 device log showed `firstResponder=yes` without did-show, which is
why build 18 repaired the containing UIWindowScene. Build 18 proved that scene
attachment but still received no did-show notification. Build 19 therefore
removes the native keyboard as a usability dependency: tapping **KEYBOARD**
opens a five-row QWERTY keyboard rendered and hit-tested by SDL. Its keys call
Boxedwine's X11 input path directly and include digits, Shift, Space,
Backspace, Enter and arrows. Build 20 no longer calls `SDL_StartTextInput` or
the UIKit bridge when opening this keyboard; asking both systems to present at
once caused duplicate hide/resize transitions even though UIKit never showed
a useful keyboard. Native IME is a later, separate input mode rather than a
side effect of opening the reliable guest overlay.

Build 20's first Saya launch reached `Uknown int 99 call: 2897`. The generated
GL table maps 2897 to `glXChooseVisual`, proving WineD3D reached the missing GL
backend rather than the ARM64 JIT stalling. Build 21 tried the obsolete
`DirectDrawRenderer=gdi` value; its device log proved Wine 10 ignored it and
entered GLX anyway. Build 22 writes Wine 10's current
`Software\\Wine\\Direct3D / renderer="gdi"`, which selects wined3d's no-3D
adapter for this software compatibility path. Direct3D/OpenGL still require an
accelerated translation backend.

Build 22 device logs prove that software path reaches and interacts with Song
of Saya's launcher, but Start Game requests a real D3D9 feature level. The
no-3D adapter supplies none, the engine faults through null, and Wine starts
its debugger. That debugger then exhausts the original 64 MiB JIT arena.
Build 23 raised the single pre-authorized arena to 128 MiB, and build 90
replaces the fixed size with the budget-derived segment plan above. Both
deliberately refuse to request another StikDebug stop during live guest
execution: by then StikDebug may be suspended in the background, and the stop
would never be serviced.

The dormant `source/opengl/es` backend was compile-tested against 26R2 and is
not viable: its 2016 fixed-function declarations lack the constants and
extension symbols required by the current generated GL marshaler. Build 25
instead implements Boxedwine's generated Vulkan bridge, `KVulkanSDL`, SDL
UIKit's `CAMetalLayer`, and statically linked Khronos MoltenVK 1.4.2. iOS maps
the guest request to `VK_EXT_metal_surface`; device logs prove the bridge
enumerates an Apple A19 GPU. Those same logs prove rootfs DXVK 2.5.2 rejects
MoltenVK because its baseline device gate requires geometry shaders and
`VK_EXT_transform_feedback`.

Build 26 therefore routes imported Direct3D games through Wine 10's built-in
WineD3D Vulkan renderer: D3D9 → WineD3D → guest winevulkan → Boxedwine's
`int 0x9a` Vulkan marshaler → `KVulkanSDL` → MoltenVK → Metal. It repairs
existing prefixes from `renderer=gdi` to `renderer=vulkan`, so a game does not
need reimporting. The host Vulkan procedure resolver also retries a missing
`...KHR` name under its promoted core spelling, which lets WineD3D's Vulkan
1.0-plus-KHR negotiation use MoltenVK's core Vulkan implementations.

One iOS lifecycle detail sits below that API path. SDL2 2.32.10 creates a new
`SDL_uikitmetalview` and makes it the window controller's view for every
`SDL_Vulkan_CreateSurface()` call, but exposes no matching surface-destroy
function. WineD3D creates short-lived capability/probe surfaces before its
real render surface. Without host bookkeeping, destroying a probe destroys
its Vulkan objects but leaves its now-dead `CAMetalLayer` above Boxedwine's
X11 renderer. Build 28 makes `KVulkanSDL` track the ordered set of host
surfaces and the Objective-C++ bridge track each surface's exact UIKit view.
`vkDestroySurfaceKHR` detaches that view through SDL's version-pinned
`setSDLWindow:nil` path, restores the preceding surface and fake-fullscreen
window, or recreates the normal SDL renderer when no Vulkan surface remains.
Its device log proved that lifecycle but exposed another distinction: the
preceding surface can itself be a still-live capability probe. Build 29 creates
tiny helper windows (below 320x200; Saya uses 113x2) against a retained,
unattached `CAMetalLayer` through `vkCreateMetalSurfaceEXT`. They remain valid
Vulkan surfaces for Wine but never replace the UIKit view, fake-fullscreen
window or global 800x600 guest/input coordinate system. Presentation teardown
counts presentation surfaces separately, so X11 returns while helpers live.
`KNativeSystem::shutdown()` destroys `KVulkanSDL` before releasing the session
screen. This prevents a second accelerated guest from inheriting a Vulkan
backend bound to the previous SDL window and releases probe layers abandoned
by a forced Wine exit.

Build 29's device run proves that surface distinction: the X11 launcher is
visible and responsive after the 113x2 helper and transient launcher surface.
Starting Saya then creates three successive 1280x960 presentation surfaces,
initializes WineD3D shaders and SDL audio, and faults on a guest write before a
first rendered frame. The final Vulkan surface remains valid, so its Metal
view would normally obscure WineDbg with black. Build 30 treats X11 windows
titled `Wine Debugger` or `Program Error` as diagnostics: title notification
works whether Wine sets the property before or after mapping, visible Metal
views are detached newest-first, and X11 is restored without destroying the
underlying Vulkan handles. Normal guest teardown can consequently destroy
those handles once, without a double UIKit detach.

The Vulkan bridge also records each mapped device-memory allocation's owning
guest process, offset, length and 32-bit address, plus the last 32 unmapped
ranges. Each unique guest page fault is classified as mapped data, a leading
or trailing guard page, a recently unmapped range, or outside the tracked
ranges. This diagnostic is intentionally passive: it does not pad an unknown
host allocation or hide a real engine overrun. The MoltenVK message about
disabling primitive restart is a warning from its always-enabled Metal
implementation and is not treated as the fatal event; the following Wine
access violation is.

Build 30's two-resolution device comparison classified the fatal
`0x03BD0000` write as outside both live and retained-unmapped Vulkan ranges,
then observed a nested WineDbg fault before any diagnostic X11 window mapped.
Build 31 therefore also watches Wine's TTY stream for its unhandled-page-fault
line and immediately detaches presentation layers/restores X11. For the Saya
profile, the first 64 guest faults include PID, thread, access kind, EIP,
mapped module/offset, registers and instruction bytes. Those snapshots are
diagnostic only and are disabled for ordinary Notepad launches.

The build 31 device run proves that fallback: it removed the final Metal view,
restored the 800x600 X11 viewport and left the built-in keyboard responsive.
The engine still faulted because its module selector never activated. Its
snapshot captured `F3 A4` (`REP MOVSB`) at `0x7F444DDF`, with destination
`0x03BD0000`, source `0x03BCFFFC` and an invalid `ECX=0xFFE1D4F0`. Build 32's
temporary range profile covered the surrounding engine code and established
that this was CPU/JIT behavior rather than genuine guest/WineD3D state. Build
35 removes that profile after correcting the generic overlap loop described
above.

Two build 32 device runs activate that range and proceed beyond the old fault.
They initialize D3D11 and audio, destroy two transient presentation surfaces,
and leave a final 800x600 or 1024x768 swapchain alive. At that point the old
bridge knew only that a `VkSurfaceKHR` existed. SDL had already installed its
opaque `CAMetalLayer`, so a surface which had never presented and a guest that
successfully presented a black frame were visually and diagnostically
identical.

Build 33 carries presentation identity through the generated bridge without
putting UIKit types into it. `vkCreateSwapchainKHR` records the host
`VkSwapchainKHR` → `VkSurfaceKHR` relationship in `KVulkanSDL`, destruction
removes it, and `vkQueuePresentKHR` reports the per-swapchain result. The SDL
backend records only the first successful present for each presentation
surface and dispatches one main-thread callback. Until that callback, the
UIKit Metal view uses the Wine blue background and a non-interactive native
first-frame indicator; the callback removes it. The steady-state frame path
does no UIKit work. The bridge also logs the first Vulkan queue submission,
so a device log can now distinguish no GPU work, submitted work without a
present, a failed present, and a successful-but-black frame.

Build 37 attaches a lifetime token to each presentation surface and starts a
native watchdog only for display-sized surfaces. If no successful present is
observed, it records snapshots at 12 and 30 seconds. Emulation threads publish
their last dispatch-boundary EIP/ESP/EBP and dispatch count through atomics;
the watchdog never races the live CPU register file. Kernel snapshots retain
shared process references while enumerating threads and include active futex
guest addresses, expected values and wait ages. Destroying the surface or
presenting its first frame cancels later samples. This is diagnostic-only work
off the rendering hot path.

The build 33 device log shows why presentation identity must be per surface:
the first game surface submits and successfully presents once, removes its
indicator, and is immediately destroyed. The following final 800x600 surface
stops after PNG loading without a present, so its own indicator remains.
Build 34 extends the association to both acquire entry points and attributes
each queue submission to the newest live presentation surface. It also reduces
the game-specific interpreter policy from one 2 MiB span to the two 64 KiB
windows containing the observed `0x7F3D1CD6` and `0x7F444DDF` sites. The rest
of mvware returns to the ARM64 JIT. A 25-million-instruction heartbeat inside
those windows detects active interpreter-bound work without logging ordinary
JIT execution or adding UIKit work to the render loop.

The current 800x600 desktop is aspect-fit, not cropped or non-uniformly
stretched. Its 4:3 viewport therefore leaves unused width on modern phones.
Per-game resolution profiles are intentionally deferred until real game
resolution requirements are known; the first Fruit of Grisaia run should
drive that work.

**Consequence:** any guest program that requires OpenGL will fail, and
`KNativeSystem::getOpenGL()` logs "Failed to load OpenGL, will probably crash".
That is a deliberate, identified limitation, not a silent degradation.

The repository's `BOXEDWINE_ES`/`source/opengl/es` translator is an incomplete
2018 experiment, not a production backend that can safely be enabled with one
flag. The accelerated-content path needs a deliberate fixed-function GL to
GLES/Metal design. The macOS OSMesa binaries are not usable on iOS.

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
app, and copy saves and logs out. Both Settings (rootfs) and Library (game ZIP)
enumerate ZIPs at the top of that folder, providing an import path with no
`UIDocumentPickerViewController` or security-scoped cross-process hand-off.
The system picker in build 19 is no longer SwiftUI's open-in-place
`.fileImporter`; it is an explicit `UIDocumentPickerViewController` configured
with `asCopy: true`, an import-mode delegate, and visible file extensions. The
returned file is local to BoxedVN, avoiding the security-scoped open contract
that never completed on the physical test device.

---

## 8. How a guest is started

`BVNRuntime` synthesises an argv from real Boxedwine options
(`commandLine.txt` documents all of them) and calls `boxedmain()`:

```
boxedvn
  -root   <Application Support>/prefixes/<id>      writable overlay
  -zip    <root filesystem>.zip                    read-only, mounted at /
  -nozip                                           do not scan for a *Wine*.zip
  -mount_drive <Documents>/Games/<id>/content d    the game becomes D:
  -resolution 800x600
  -bpp 32
  -w      /home/username/.wine/dosdevices/d:/<workdir>
  /bin/wine  d:\game.exe  [arguments...]
```

If a manifest has no explicit working directory, BoxedVN derives it from the
selected executable; a root-level `D:\game.exe` therefore gets `-w
/home/username/.wine/dosdevices/d:/`. This matters because older games often
open scripts, archives and configuration using relative paths. Before building
this command, the runtime extracts Wine's `user.reg` and `system.reg` from the
read-only rootfs ZIP when the writable overlay lacks them. It disables and
disconnects `winebth`, but keeps `winebus` at `Start=3` with its root service
association intact. Build 24 proved removing winebus did not improve cold
startup, so build 25 repairs prefixes rather than sacrificing future
controllers. For the software fallback, imported games can set
`Software\\Wine\\Direct3D / renderer=vulkan`, selecting the active Wine root's WineD3D
Vulkan backend. Build 26 overwrites the stale build 22 `gdi` value on every
imported-game launch. Repeated preparation is idempotent, and an existing game
import does not need to be recreated.

`/bin/wine` is a Boxedwine symlink (`bin/wine.link`) into `/opt/wine/bin/wine`
inside the root filesystem.

The full command line is written to the log before `boxedmain` is called, with
spaces quoted, so a failing launch can be reproduced exactly.

---

## 9. Logging

`internal_log` writes to stdout/stderr. BoxedVN deliberately does **not** pass
Boxedwine `-log`: upstream handles that option with `createNew()`, which would
truncate the frontend and JIT diagnostics already present in the session log.
On iOS neither stdout nor stderr reaches the user directly.

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
