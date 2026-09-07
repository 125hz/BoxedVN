# Wine 11 presentation resize and pointer ordering

## Device evidence

Nonverbose logs `boxedvn-20260906-230844.log` and `231130.log` confirm that the
32-bit visual novel and 32-bit 3D program boot on revision 22ee91bb. The earlier
multi-million-line relay runs were not representative startup/performance tests.

The 3D run creates a presentation surface at 394x275, then switches the guest
display and X11 client to 640x480. The UIKit Metal view keeps its creation size.
Acquisition and presentation repeatedly return VK_SUBOPTIMAL_KHR (1000001003),
and swapchain creation reaches at least n=8460. This is actual rebuild work,
not just an overactive frame counter. The window-size witness also retained
creation dimensions and concealed the later guest resize.

MoltenVK checks image extent against both surface and natural layer extent:
https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKSwapchain.mm

## Runtime changes

- Synchronize the native presentation view with its current X11 window before
  surface capability queries and swapchain creation. Unchanged sizes take a
  mutex-protected check without a main-thread dispatch. Recheck inside the
  dispatch, and never hold the surface mutex while waiting for the main thread.
  Detached, destroyed and offscreen probe surfaces do not resize the live view.
- Compare surface diagnostics with the current window dimensions.
- Apply iOS virtual cursor warps directly to X11 and the cached pointer. Sending
  them through SDL queued a delayed synthetic absolute event that could overwrite
  a newer swipe position. Warp events stay core motion, not raw physical deltas.
- Route XMoveResizeWindow through the same configure path as XConfigureWindow.
  The previous method resized storage but ignored the requested x/y and omitted
  the exposure event needed to repaint the new backing store.
- Include the blocked socket descriptor and last request in existing bounded FEX
  stall reports. This distinguishes a server reply wait from a Windows object
  wait without enabling full Wine relay tracing.

## Remaining boundaries

The Unity-based 64-bit program in `231651.log` reaches DXMT device setup and
MonoManager ReloadAssembly. Its main thread then remains in a 16-byte pipe read.
Missing optional I18N probing paths alone do not establish a fatal dependency;
the local installation also omits I18N.dll. There is no basis to alter its files
or claim its startup wait fixed. New IPC fields will identify the pending request.

The 3D audio stream eventually supplies about 176 kB/s at 44100 Hz, stereo S16,
with few underruns and roughly 48 ms queued in sampled steady playback. The
cause of earlier silence is not established by that steady-state evidence.

The desktop retest `232422.log` identifies b7502096, shows a taskbar strip and
incomplete file-manager content. The repeated Wine WM_NCPAINT/WM_ERASEBKGND
packing warnings concern cross-process painting; they remain visible because
silencing them would not repair the desktop. The move/resize correction fixes a
real X11 defect, but complete desktop rendering still needs device validation.

## Validation

The native fixture compiles the production surface synchronization, cursor-warp
and window-move methods with platform stubs. It checks resize transitions, 1000
unchanged queries, unknown/destroyed/probe/detached surfaces, immediate core warp
delivery without native warps, and move/no-op behavior. Vulkan contract checks
verify synchronization precedes both capability entry points and creation.
CI compiles the real iPhoneOS implementation; physical GPU, swipe feel, audio
start and a sustained 30 FPS target remain device acceptance checks.
