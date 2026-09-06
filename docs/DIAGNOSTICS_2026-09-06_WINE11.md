# Wine 11 migration and shared runtime corrections

## Runtime and delivery

The migration branch is `codex/boxedwine-wine11`, starting from `7b6e0357`.
Wine 11.0 is built from the official source archive for both PE architectures
and the ELF64 Unix runtime. Source SHA-256:
`c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701`.
The loader, ntdll, wineserver, builtins, OSS and XAudio modules share that source
version. The packaging script accommodates Wine 11's loader beside Unix ntdll.
Developer import archives and DWARF sections are excluded from staged runtime
modules; the original build cache retains them. The process-start diagnostic
also handles Wine 11's reply layout, which no longer carries an entry address.
The source install's optional preloader is not staged: ntdll retains its
existing reservation fallback inside the BoxedWine-owned guest address space.

Wine still runs as a Linux guest inside BoxedWine. FEX supplies CPU translation;
DXMT supplies the existing 64-bit graphics path. The 32-bit D3D9 path remains
DXVK through the guest Vulkan bridge to MoltenVK. Wine's NTSYNC improvements do
not automatically apply without corresponding emulated kernel support. No FPS
improvement is claimed from the version change alone.

The IPA requires its matching `wine64-pe32.zip`. The app checks a Wine 11 stamp
before mounting an existing PE32 layer, including desktop and 64-bit launches.
Existing prefix registry files are copied before their first Wine 11 update.
This is a registry backup, not a full rollback snapshot of the prefix.

## Evidence from build 137

The four supplied logs identify revision `7b6e0357+dirty`.

* The managed 64-bit application opens and maps its real `mscorlib.dll`, but
  `FileTimeToSystemTime` fails while its C runtime obtains file information.
  Read-only inspection of the bundled runtime confirms the failure precedes
  writing the file size. Mono's file-map size helper returns zero on fstat
  failure; mapping length zero still maps the file, but its assembly reader
  rejects a zero-length image. `sys_fstat64` was passing filesystem milliseconds
  to a Linux stat writer expecting seconds. Path-based stat already converted
  these units. Descriptor stat now does the same.
* Relative mouse motion previously had no functioning LP64 XInput2 path. The
  guest libXi now describes a relative master pointer, registers event masks,
  and provides allocated LP64 raw-motion cookies. Root subscribers receive
  signed deltas independently of the visible cursor's bounded position.
  Touchpad clicks no longer inject an extra absolute move in center-lock mode.
  This is the pointer subset of XI2, not full touch/device support.
* A legacy launcher creates an unpresented Vulkan capability-check surface
  under a hidden parent, then maps an unrelated visible GDI launcher. The hidden
  surface had retained display selection. Mapping a visible window now selects
  it when the previous presentation window and its ancestors are not mapped.
  A subsequent first Vulkan frame restores selection to its rendering window.
* Both full-precision and reduced-precision runs contain guest faults. Reduced
  x87 precision is disabled for migration testing; this does not establish that
  it caused every fault. Strict x86 memory ordering defaults on when no user
  preference exists, because hardware TSO is not enabled by this iOS runtime.

The live log is fixed at five 12-point rows with a compact monospaced font,
horizontal scrolling, stable row identities, and explicit severity colors.

## Validation and device acceptance

The compiled Linux X11 libraries are exercised for pointer metadata, signed
raw-motion deltas, timestamps and cookie allocation/free. A compiled regression
uses the actual descriptor-stat implementation and checks timestamp units,
file size and invalid descriptors. Existing host, graphics, packaging and FEX
checks remain in CI. CI compilation and packaging do not prove device behavior.

On the matching IPA and PE32 ZIP, first test both cubes, then managed startup,
the legacy launcher, and relative camera movement. Begin with verbose logging
off and retain full x87 precision. Compare equivalent scenes and resolution
before attributing performance differences to Wine 11. Existing prefixes may
spend extra time updating on their first launch.

## Primary references

* [Wine 11.0 announcement](https://www.winehq.org/news/2026011301)
* [Wine 11.0 source](https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz):
  `loader/main.c`, `dlls/ntdll/unix/loader.c`, `dlls/winex11.drv/mouse.c`,
  `dlls/winex11.drv/bitblt.c`, and `include/wine/asm.h`.
* [Unity Mono file mapping](https://github.com/Unity-Technologies/mono/blob/unity-5.6/mono/utils/mono-filemap.c)
  and [assembly image loader](https://github.com/Unity-Technologies/mono/blob/unity-5.6/mono/metadata/image.c).
* [FileTimeToSystemTime](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-filetimetosystemtime):
  failure and input-range contract.
* [XInput2 protocol](https://xorg.freedesktop.org/archive/current/doc/inputproto/XI2proto.txt).
