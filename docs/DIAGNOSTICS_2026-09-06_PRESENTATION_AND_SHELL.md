# Vulkan presentation and Wine shell registration

Device evidence: application revision `3b1752f8`, build 137. Logs ending
021501, 022139, 023122, 024649, 024949, 025043, 025649, 025732 and 030222
were inspected. The 64-bit game remains deferred at the user's request.

## Presentation performance

The 32-bit D3D9 game reaches gameplay. The first gameplay session later reports
an 800x600 guest window with Vulkan currentExtent 869x1890. The visual novel
reports the same 869x1890 extent for its 800x587 window, rebuilding approximately
58 swapchains per second. The snapshot of the last successful fit still says
800x587: it was not a live measurement of the damaged view. The menu's normal
and corrupt screenshots coincide with this session; the extent repair is a
candidate for that corruption, not proof that every texture problem is solved.

SDL 2.32.10's SDL_uikitviewcontroller.updateKeyboard assigns the full window
frame to self.view, even when keyboard height is zero. For an embedded view
scaled to 370/800, a 402x874 point frame changes its bounds to roughly 869x1890.
The existing poll compared only the parent container, which did not change, so
the damaged view could remain at that extent until a fullscreen transition.

The maintained SDL patch asks a weak application hook whether a Vulkan fit owns
the view. For that view, keyboard layout leaves presentation placement and
pixel geometry to BoxedVN. Ordinary SDL views retain their existing keyboard
behavior. The geometry poll now checks the actual view bounds, contents scale
and drawable size as well as its parent, recovering from other layout changes.
The patch participates in both dependency cache keys and is restored/applied
against the pinned source on every dependency preparation.

References:
- https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/video/uikit/SDL_uikitviewcontroller.m
- https://github.com/libsdl-org/SDL/blob/release-2.32.10/src/video/uikit/SDL_uikitmetalview.m

Acceptance: compare the same gameplay area at the same resolution, with verbose
off. Keyboard show/hide, toolbar input, pointer dragging and fullscreen changes
must not introduce a phone-sized Vulkan extent or sustained swapchain rebuilds.
No numerical FPS improvement is asserted before device testing.

## Saves and system information

The visual novel's save calls use a Documents CLSID namespace string as a disk
path. Repeated stat and mkdir calls return ENOENT under dosdevices/::/{CLSID}.
Its executable imports SHGetSpecialFolderLocation. Wine 9's desktop
GetDisplayNameOf explicitly returns ::{CLSID} unless ShellFolder/WantsForParsing
exists; with that value it delegates to the Documents folder for a real path.

Complete missing Documents COM registration and WantsForParsing in both registry
views when the matching shell32 DLL is packaged. Create missing standard user
profile directories alongside the existing public directories. Existing values,
COM overrides, directories and symlinks are preserved. There is no save-path
redirection for an individual application and no game data modification.

The DirectX startup-error session loads DXVK and enumerates the Apple GPU, then
exits graphics initialization before creating a device or surface. It also
reports WbemLocator class-not-registered. Complete the three Wine wbemprox class
registrations, for packaged architectures only, with Wine's Both threading model.
This repairs an observed system-information failure; the game's DirectX error
may have another cause. Its executable is not available for local inspection.

References:
- https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/shell32/shfldr_desktop.c
- https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/shell32/shfldr_fs.c
- https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/shell32/shell32_classes.idl
- https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/wbemprox/wbemprox.idl

Acceptance: save to a slot, load it, end/relaunch the guest, and load again.
The BOXEDWINE_X64_SHELL_COM and WBEM_COM witnesses identify prefix repair.
Host tests check architecture gating, idempotence and override preservation.

## Browser-engine startup boundary

The browser-engine parent owns FEX. Its crash handler, GPU subprocess and
renderer subprocess run on CPU64 and fault at the same PE32 ntdll C2 04 00
instruction. Retrying GPU children repeats this fault. CPU64 currently decodes
64-bit instructions and does not implement the full WoW64 mode transition and
32-bit execution needed here. Merely adding a 64-bit RET-immediate opcode would
not establish correct 32-bit stack/address semantics. No single-process or
sandbox-disabling application workaround is applied. This startup issue remains
open and requires shared CPU compatibility work.

## Validation boundary

Host support tests cover registry repair and the existing runtime contracts.
The iOS CI build validates the SDL patch against pinned sources, compiles UIKit
integration, and publishes one unsigned increased-memory IPA. Physical-device
FPS, save persistence and menu rendering remain acceptance checks.
