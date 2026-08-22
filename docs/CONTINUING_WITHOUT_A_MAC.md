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

### 3. Touch input: SDL's window IS the presentation surface

Taps always reached the guest. What was wrong was the coordinate space, and
build 64 got it wrong in a new way that is worth writing down, because the
mistake is easy to make twice.

`SDL_uikitviewcontroller.viewDidLayoutSubviews` reports the SDL window size as
`self.view.bounds.size`, and `SDL_uikitview` delivers touches as
`-[UITouch locationInView:self]`. So **whatever is done to SDL's Metal view,
SDL's whole coordinate space follows it.** When build 64 shrank that view to the
letterbox rectangle, SDL's window became 536x402 and its touches became
content-relative — and the pointer transform then subtracted the letterbox
offset a *second* time:

```
iOS Vulkan presentation owns input mapping: window 536x402, guest 800x600,
    aspect-fit, content 536x402 at 169,0, scale 67%x67%
iOS SDL mouse down: button 0 at logical 65,121      -> guest x = -155
iOS SDL mouse down: button 0 at logical 395,256     -> guest x = 337, not 589
```

The `window 536x402` in that line is the tell: SDL's window had become the
content rect. A tap on the left third of the picture mapped to a negative guest
x and did nothing at all.

**The offset in this transform is always zero.** A letterbox offset lives in
where the view sits on screen, and UIKit has already removed it by the time SDL
sees a touch. Since build 65 the presenter also sets the view's *bounds* to the
guest resolution (see below), so the transform is normally a straight 1:1.

`BVNGuestPresentationContentRect` is still consulted, but only as a cross-check
that gets logged — never as a source of truth for input.

### 4. The swapchain recreation storm: FIXED, confirmed on device in build 65

Build 64's rate report settled it in one line:

```
Vulkan presentation swapchain rebuilt 113 time(s) in 1001 ms (8244 total);
layer natural drawable 834x625.
[mvk-info] Created 3 swapchain images with size (800, 600)
```

Natural drawable 834x625, swapchain 800x600. They disagree, permanently, so
every acquire and present returns `VK_SUBOPTIMAL_KHR` and DXVK rebuilds. Two
things are now established rather than assumed:

- MoltenVK's suboptimal test is `bounds x contentsScale` versus the extent the
  swapchain was created at.
- **DXVK does not adopt `currentExtent`.** It kept asking for 800x600 while the
  natural size was 1608x1206 and then 834x625. Sizing the view in points was
  therefore never going to fix this on its own.

The fix is to make `bounds x contentsScale` equal the guest resolution exactly,
and the way to do that without shrinking the picture is to stop scaling with the
frame:

```objc
view.bounds = CGRectMake(0, 0, guestWidth, guestHeight);  // 800x600
view.layer.contentsScale = 1.0;                           // natural == 800x600
view.transform = CGAffineTransformMakeScale(k, k);        // display size
view.center = <centre of the letterbox rectangle>;
```

`bounds x contentsScale` is now an integer with no rounding to get wrong, and
Core Animation scales the 800x600 drawable up to the display rect — which is
what was already happening, since the swapchain was never larger than the guest
resolution anyway.

It also fixes touch for free, and that is the point worth keeping: because SDL
reports its window as the view's bounds, **SDL's window is now the guest
resolution**, and `-[UITouch locationInView:]` is transform-aware, so SDL hands
Boxedwine coordinates that are already guest pixels. Presentation and input stop
being two things that have to be kept in agreement and become one thing.

**Confirmed.** The build-65 device log contains zero
`Vulkan presentation swapchain rebuilt` lines and two swapchain creations for
the whole session, against 8,244 in a minute on build 64. The natural drawable
reads 800x600 and MoltenVK creates 800x600. The storm is gone.

### 5. Rotation: the geometry signal cannot be a UIKit layout callback

Build 65 re-fitted the picture from the overlay's `-layoutSubviews`, and on
device that callback fired for landscape -> portrait and then never again:
rotating back produced no `Guest presentation` line at all, the Metal view kept
its portrait geometry, and the picture ended up off-centre and half off-screen.
Touch died in portrait at the same time, with the pointer transform reading a
correct 1:1 - the view had been left outside its ancestor's stale bounds, so
UIKit never delivered the touches for SDL to transform.

The cause was self-inflicted: the fit called `-layoutIfNeeded` on the Metal
view *from inside the overlay's own `-layoutSubviews`*. The Metal view lives
under a `UIDropShadowView` — UIKit's own hosting wrapper, which the build-65
log named — so that call walked up to the window and re-entered the layout of a
sibling subtree mid-pass, which UIKit does not support.

Two changes, and the second is the one to keep:

- The fit no longer forces layout. It assigns `CAMetalLayer.drawableSize`
  directly, which is exactly what SDL's `-updateDrawableSize` would compute
  once `contentsScale` is pinned to 1.
- **Geometry is polled from Boxedwine's own main loop**, in
  `KNativeInputSDL::processEvents`, roughly every 200 ms:
  `BVNSyncGuestPresentationGeometry` compares the window's bounds and safe-area
  insets against the ones the current fit was computed for and re-fits when
  they differ. That loop runs for as long as the guest does, so unlike a UIKit
  callback it cannot quietly stop arriving.

The fit also measures against the **window**, not the Metal view's superview,
for the same reason the drop-shadow view should not be trusted: it is UIKit's,
not ours.

### 6. The Fruit of Grisaia: DXVK cannot create a device for it

Its build-65 log shows DXVK failing `vkCreateDevice` six times, every attempt
rejected for the same four features:

```
VK_ERROR_FEATURE_NOT_PRESENT: ... the 5th flag in VkPhysicalDeviceFeatures      (geometryShader)
VK_ERROR_FEATURE_NOT_PRESENT: ... the 39th flag in VkPhysicalDeviceFeatures     (shaderCullDistance)
VK_ERROR_FEATURE_NOT_PRESENT: ... the 1st flag in ...Robustness2Features        (robustBufferAccess2)
VK_ERROR_FEATURE_NOT_PRESENT: ... the 3rd flag in ...Robustness2Features        (nullDescriptor)
err:   DxvkAdapter: Failed to create device
```

Those are exactly the four `dxvk-2.5.2-moltenvk.patch` relaxes, and **Saya's
DXVK device in the same build requests none of them** — so this title reaches a
DXVK code path the patch does not cover. The patch touches
`src/d3d11/d3d11_device.cpp` and `src/dxvk/dxvk_adapter.cpp`; whatever Grisaia
calls builds its enabled-feature set somewhere else. Finding it needs the DXVK
source tree, and fixing it needs a rebuild, which needs mingw on Linux or WSL —
neither available on the Mac this was diagnosed from.

The game does not handle the failure: it shows a mojibake "DirectX" dialog and
then faults reading `0x0000000F` in `grisaia+0x12a2bc` (`mov (%eax),%eax` on an
interface pointer it never received).

Build 66 ships a workaround rather than a fix. In the same session wined3d's
Vulkan renderer created a device without complaint, and this engine is
Direct3D 9 — the shader-model-4 wall that forces Saya through DXVK does not
apply — so `BVNApplyKnownCompatibilityProfile` now launches it **without**
`-dxvk`. That has a clean falsifier: if the DirectX dialog still appears, DXVK
was not the cause and the profile should be deleted rather than elaborated.

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
  guest aspect fit and then re-derives the pointer transform. Presentation and
  input are never computed independently - that is what broke tapping in builds
  62 and 64. `-safeAreaInsetsDidChange` re-fits too: UIKit reports the new safe
  area separately from, and sometimes later than, the bounds change during a
  rotation, and build 64 fitted a portrait window using the landscape insets,
  producing a 278pt-wide picture in a 402pt-wide window. The fit also discards
  a horizontal inset when the window is taller than it is wide, because an
  iPhone in portrait never has one and a non-zero value is stale by definition.
- **The overlay is pinned to the window with constraints, and its panel rows
  are `UIButtonTypeCustom`.** Both are build-64 bug fixes rather than taste. A
  direct window subview has no view controller laying it out, so an
  autoresized frame is not guaranteed to survive a rotation - and a stale frame
  means controls that are drawn in one place and hit-tested in another, which
  looks exactly like "the menu does not respond". And on iOS 15+ a
  `UIButtonTypeSystem` button carries a `UIButtonConfiguration`, after which a
  later `-setTitle:forState:` is not reflected; that is why the two rows whose
  titles change at runtime rendered blank while "Quit to library" did not.

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
- `geometryShader`, `shaderCullDistance`, and robustness2's
`robustBufferAccess2` / `nullDescriptor` are disabled, while
`textureCompressionBC` and `VK_EXT_transform_feedback` are requested only when
reported. The explicit false mask is intentional because the device log showed
the earlier query values could not safely be reused for MoltenVK device
creation. Upstream fails device creation without this mask; nothing in
`GetMaxFeatureLevel()` consults these fields and its floor is
`D3D_FEATURE_LEVEL_10_1`, so the resulting device is still shader-model-4
capable.

The prebuilt 32-bit modules are committed at `ios/app/Dxvk/` so CI needs no
mingw toolchain. To rebuild them (Linux or Windows/WSL both work):

```bash
git clone --branch v2.5.2 --recurse-submodules https://github.com/doitsujin/dxvk
cd dxvk && git apply --unidiff-zero /path/to/dxvk-2.5.2-moltenvk.patch
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
d3d10core.dll  19d682f82b59f33e8fe581b2bb2d283cce7c262b62e5ff4cf891c4b01dce9f86
d3d11.dll      fd46bf3c2860b2baa039c82f88b37925ae5c6870d962ce058be2a215eafe7a96
dxgi.dll       37edf9541f50c217e175c2d55a18408a0203efaccc8e0e88f4f568696eccf4ef
```
Any prior module RVA must be rediscovered against these exact files. Do not
reuse an address recorded for an older module hash.

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
