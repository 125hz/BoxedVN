# Continuing BoxedVN without a Mac

Everything needed to build a testable unsigned IPA is committed. GitHub Actions
(`.github/workflows/build-ios.yml`) runs on `macos-15` and produces the same
slim IPA that reached Saya no Uta's main menu, so no local macOS is required.

## Building

Push to the `ios` branch, or run the workflow manually from the Actions tab.
It fetches the pinned dependencies, builds `BoxedVN.app`, packages
`BoxedVN-unsigned.ipa` and uploads it as a workflow artifact. Download it,
sign it (SideStore / Sideloadly / AltStore), and install.

The CI IPA is **slim**: it does not embed the ~160 MB root filesystem. Import
the runtime ZIP on the device once — it survives reinstalls. See
`docs/BUILD_IOS.md`.

JIT still requires StikDebug with `universal.js` assigned to the installed app,
and the assignment must be redone after every reinstall.

## Where the current state is

`PROGRESS.md` is the authoritative record. As of the last Mac session:

- The game **renders its main menu**. This required routing Direct3D 11 through
  a patched DXVK rather than wined3d, because Metal has no geometry shader
  stage and wined3d therefore refuses shader model 4 outright.
- It only works with `-interpreterModule d3d11`, i.e. **DXVK runs under the
  interpreter, not the ARM64 JIT**. That is a diagnostic workaround, not a fix,
  and it is the dominant cause of the low frame rate.

## The open problems, in priority order

### 1. The ARM64 JIT miscompiles DXVK (biggest win)

Measured with `THREAD_BASIC_INFO`: the guest thread consumed 981,821 us of CPU
in a 1000 ms window (98% of a core) while making zero guest progress, spinning
in JIT-compiled code. Interpreting `d3d11` removes it entirely.

The likely shape, from the eventual crash in `Mware.dll`:
```
mov  (%ecx),%eax     ; EAX = [ECX]      load vtable   -- executed correctly
call *0x6c(%eax)     ; CALL [EAX+slot]                -- branched to ECX instead
```
Register state at fault had `EIP == ECX == EDI` (the object) while `EAX` held
the correct vtable pointer, i.e. the indirect call used the addressing base
register instead of the loaded target. Audit the ARMv8 backend's indirect-call
lowering (`source/emulation/cpu/armv8/jitArmV8CodeGen.cpp`) and Boxedwine's
generic `CALL E32` path, which computes `newEip = read32(calculateEaa(op))`
before pushing the return address.

Fixing this lets `-interpreterModule d3d11` be removed, which should transform
performance. The temporary flag lives in `BVNApplyKnownCompatibilityProfile`
(`ios/runtime/src/BVNLaunchArguments.cpp`) and is covered by a test that
documents it must not ship.

Precedent: build 35 fixed a real ARMv8 `REP MOVS` miscompilation in this same
backend, so a second codegen defect is plausible.

### 2. Aspect ratio and safe area: FIXED in build 64

The picture was stretched because the aspect fit **never ran**, not because it
ran and got the wrong answer.

`BVNApplyGuestPresentationAspect` was called from the guest thread and forwarded
itself with `dispatch_async(dispatch_get_main_queue())`. The build-63 device log
proves those blocks never executed during the session: both of them (one per
presentation surface) were still queued when Wine exited 2.5 minutes later, and
ran only after `Boxedwine shutdown`, by which point their surfaces had been
unregistered:

```
18:42:29.831  Registered Vulkan surface 0x12235bc80 ... at 874x402
              (no "Guest presentation aspect-fitted" line, the whole session)
18:45:08.836  guest exited with code 1
18:45:08.856  Presentation aspect skipped: no registered view for this surface.
18:45:08.867  Presentation aspect skipped: no registered view for this surface.
```

**While `boxedmain` owns the main thread the main dispatch queue is not
drained.** SDL's `UIKit_PumpEvents` runs the main run loop from inside
`SDL_PollEvent`, which is why UIKit touches, layout and the native spinner all
keep working - but GCD blocks submitted to the main queue do not run until the
guest gives the thread back. Every other UIKit call on this path already used
`DISPATCH_MAIN_THREAD_BLOCK` (an SDL user event, serviced by Boxedwine's own
event loop); this one did not.

The fix: the aspect call moved inside the existing `DISPATCH_MAIN_THREAD_BLOCK`
in `KVulkdanSDLImpl::createVulkanSurface`, immediately after
`BVNRegisterGuestVulkanSurface`, and `BVNApplyGuestPresentationAspect` now
refuses an off-main call with a warning instead of quietly deferring it forever.

**Do not reintroduce `dispatch_async(dispatch_get_main_queue())` anywhere that
can run during a guest session.** It is not slow, it is silent.

Two details of the fit worth keeping:

- Only the **horizontal** safe-area insets are applied. In landscape the
  Dynamic Island is an opaque cut-out at one end and content must clear it; the
  bottom inset is the translucent home indicator, and surrendering 5% of the
  picture height to a line drawn over it anyway is the worse trade.
- The rectangle is snapped so `bounds x contentsScale` is a whole number of
  pixels. See section 4 for why one pixel matters.

### 3. Touch input: FIXED, and it must stay derived from the measured rect

Taps always reached the guest - the log showed all 32 of them. They were raw
**window** coordinates, because the Vulkan path has no SDL renderer and
therefore no `SDL_RenderSetLogicalSize` translation.

`KNativeScreenSDL::refreshIOSGuestPointerTransform` now inverts the rectangle
the presenter reports it applied (`BVNGuestPresentationContentRect`), never a
rectangle predicted from the window size. Build 62 predicted one, the layer
resize silently failed, and every tap was wrong by the letterbox offset.

It is called from three places, and all three matter: after the surface is
created and fitted, from `setScreenSize`, and from the overlay's
`layoutSubviews` after a rotation. For 874x402 with an 800x600 guest it yields
scale 67%, content 536x402 at x=169.

The percentage is whole-number (`KNativeInput`'s existing API) and is rounded to
nearest rather than truncated; truncation biased the scale down and the error
accumulated towards the far edge of the picture, which is where a visual novel
puts its menu.

### 4. UNANSWERED: the swapchain recreation storm

Counted in the build-63 log: **17,879 presentation swapchains created in a
150-second session**, about 119 per second, every one of them 800x600. Acquire
and present both return `1000001003` = `VK_SUBOPTIMAL_KHR`.

MoltenVK raises `VK_SUBOPTIMAL_KHR` when the presenting layer's *natural*
drawable size (`bounds x contentsScale`, which is what
`SDL_uikitmetalview.layoutSubviews` assigns) differs from the extent the
swapchain was actually created at. With the Metal view filling the window
(2622x1206 pixels) and DXVK creating an 800x600 swapchain, those could never
agree, so DXVK rebuilt the swapchain on essentially every frame.

Build 64 makes the view exactly the letterbox rectangle in whole pixels, which
*may* be enough - but only if DXVK adopts `VkSurfaceCapabilitiesKHR::currentExtent`
rather than re-requesting the guest resolution. **That is not verified.** The
evidence cuts against it: the swapchain was created at 794x568 and then 800x600,
i.e. at the X11 window sizes, never at the device size, which is what a
presenter that ignores `currentExtent` would do. MoltenVK ships here as a binary
and DXVK as prebuilt DLLs, so neither could be read to settle it offline.

Build 64 therefore replaces the per-creation log line (which was most of the
4.5 MB log) with a once-per-second rate report naming the layer's natural
drawable size:

```
Vulkan presentation swapchain created N time(s) in M ms; the presenting
layer's natural drawable size is WxH - compare it with the extent MoltenVK
logs on the next "Created N swapchain images with size" line
```

**One device run answers this.** If the two sizes now match and N drops to
roughly 1, the storm is gone. If they still differ, the remaining fix is to set
`view.layer.contentsScale = guestWidth / boundsWidth` so that
`bounds x contentsScale` equals the guest resolution exactly - the aspect-fit
rectangle has the guest's aspect ratio by construction, so one scale satisfies
both axes. That renders at 800x600 and lets Core Animation upscale, which is
what the app effectively does today anyway, minus the 119 rebuilds a second.

This is worth chasing regardless of the JIT work: it is pure waste on the
render path.

## The in-game overlay

`ios/runtime/src/BVNGuestOverlay.mm` is a UIKit view added directly to SDL's
guest `UIWindow`: a floating menu button with an on-screen keyboard, a rotation
lock and a quit control behind it.

It replaced the on-canvas keyboard that `KNativeScreenSDL` used to draw, which
was deleted in build 64. That keyboard was drawn with the SDL renderer, and a
guest presenting through Vulkan has **no SDL renderer at all** - so it existed
only during the Wine loading screen and was invisible for the entire session it
mattered in. A UIKit view sits above SDL's Metal view no matter how the guest
draws.

Four things about it are load-bearing:

- **Touch passthrough.** `-hitTest:withEvent:` returns nil for any point that
  does not land on one of the overlay's own controls, so the game still gets
  every tap. This is also why the menu is a button and not a gesture: a visual
  novel is nothing but taps, and a three-finger tap or edge swipe would fight
  the game for input.
- **Modifiers latch and really hold.** Ctrl, Alt and Shift send key-down when
  latched and key-up when unlatched, rather than applying to just the next
  keystroke. Visual novels skip read text while Ctrl is *held*; a one-shot
  prefix cannot express that. They are force-released whenever the keyboard is
  hidden or the overlay is removed, so a latched Ctrl cannot outlive its button.
- **Keys are named, not numbered.** The overlay resolves SDL's own scancode
  names ("Escape", "Left Ctrl", "PageUp") through
  `BVNGuestControlsScancodeForName`, so no scancode constant is duplicated on
  the UIKit side where it could drift. Unresolved names are logged once at
  install time instead of shipping as buttons that do nothing.
- **`-layoutSubviews` is where geometry changes are handled.** It re-applies the
  guest aspect fit and then re-derives the pointer transform from the rectangle
  that was actually applied. Presentation and input are never computed
  independently - that is what broke tapping in build 62.

Quit is confirmed inside the overlay's own panel rather than with a
`UIAlertController`, because presenting a view controller hands UIKit a modal
transition to drive while the emulator owns the main thread and the run loop is
pumped in microsecond slices.

Rotation defaults to landscape-locked and is opt-in per session. Unlocking has
to change **two** masks: the app delegate's
`application:supportedInterfaceOrientationsForWindow:` and SDL's, which answers
from `SDL_HINT_ORIENTATIONS` and otherwise derives landscape-only from the
guest window being wider than it is tall. UIKit intersects the two.

## The DXVK fork

`third_party/patches/dxvk-2.5.2-moltenvk.patch` applies to upstream DXVK at tag
`v2.5.2` (commit `b4faf0b`). It relaxes device features MoltenVK cannot provide
- `geometryShader`, `shaderCullDistance`, `textureCompressionBC`,
`VK_EXT_transform_feedback`, and robustness2's `robustBufferAccess2` /
`nullDescriptor` - from required to optional. Upstream fails device creation
without them; nothing in `GetMaxFeatureLevel()` consults them and its floor is
`D3D_FEATURE_LEVEL_10_1`, so the resulting device is still shader-model-4
capable.

The prebuilt 32-bit modules are committed at `ios/app/Dxvk/` so CI needs no
mingw toolchain. To rebuild them (Linux or Windows/WSL both work):

```bash
git clone --branch v2.5.2 --recurse-submodules https://github.com/doitsujin/dxvk
cd dxvk && git apply /path/to/dxvk-2.5.2-moltenvk.patch
meson setup --cross-file build-win32.txt --buildtype release \
      -Denable_d3d9=false -Denable_d3d8=false build.w32
ninja -C build.w32
i686-w64-mingw32-strip build.w32/src/{d3d11/d3d11.dll,dxgi/dxgi.dll,d3d10/d3d10core.dll}
```
D3D9/D3D8 are disabled because current mingw headers already define
`_D3DDEVINFO_RESOURCEMANAGER`, colliding with DXVK's own copy. The engine
imports only `d3d11.dll`, so this costs nothing here.

Copy the three stripped DLLs over `ios/app/Dxvk/` and commit.

### Do not rebuild DXVK while the JIT defect is being narrowed

Every rebuild shifts every offset in the binary. Build 59 removed 22 log calls
and moved `BindFramebuffer` from RVA `0x259B8F` to `0x2109A0`, invalidating any
recorded address. While hunting the ARM64 defect, treat the committed modules as
frozen and express positions as module + RVA, which is ASLR-independent.

Frozen module hashes (SHA-256):
```
d3d10core.dll  ad924849f3f9ca7c3405b6946565c22b7af5196e0943c8604b4e815693a2ccec
d3d11.dll      03ec08cb2131ec40a36f8b149dca474895835b9d69994f7f3ca9f75ba61d9a35
dxgi.dll       cb589a90b807499f995cf0a39d5a6d9fdc6c2ede8e2d211b87551ede4d379484
```
In this build, `D3D11CommonContext<D3D11ImmediateContext>::BindFramebuffer` is
at RVA `0x2109A0`. The guest load base observed on device is `0x7BB70000`.

### What the disassembly already rules out

The render-target loop is **fully unrolled** - eight straight-line blocks, one
per slot, and there is no backward branch anywhere in the region. Straight-line
code cannot spin, so the 98%-CPU spin is either a native back-edge the JIT
invented, a spinning helper/retry sequence emitted for a single x86 instruction,
or execution that has chained elsewhere entirely while the guest EIP bookkeeping
stayed parked. Build 60 maps the native PC back to a guest EIP
(`findOpFromJitAddress`) to decide which.

## Things already ruled out, with evidence

Recorded so they are not re-investigated:

- **wined3d for this title** - architectural, not a bug. Metal has no geometry
  shader stage, D3D10 mandates it, so wined3d never advertises feature level 10
  and skips every SM4 chunk. Saya's rendering is 331 SM4 shaders.
- **A software or DirectDraw fallback** - `Mware.dll` imports `d3d11.dll` in its
  PE import table; removing it fails the loader with `c0000135`.
- **Scheduler starvation and host-mutex blocking** for the main stall - the
  kernel reports the thread RUNNABLE and burning a full core.
- **A guest page-fault loop** - the per-thread fault counter stays at 2.
- **`IID_ID3D12CommandQueue` QueryInterface** - DXVK's own swapchain
  constructor queries it and nulls the out pointer; the warning is normal.
- **The D32_SFLOAT_S8_UINT render-pass diagnostic** - `BindFramebuffer` never
  has a depth-stencil view in these runs, so it is unrelated to the stall.
