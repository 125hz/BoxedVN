# Startup retest: four captures at revision 34b366af

The 19:50:11, 19:52:42, 19:54:34 and 19:55:57 device logs all report
`34b366af+dirty`. CI replaces the tracked DXVK resources with its rebuilt DLLs,
which accounts for that suffix. The device reports the increased-memory
entitlement and approximately 6 GB available; this does not establish that a
particular allocation can fit in the guest virtual address space.

## Findings and changes

* **32-bit cube, 19:50:11:** the new worker report identifies `std::bad_alloc`
  on Windows thread 120, followed by thread termination with status 1. Swapchain
  acquisition and one queue submission succeed, but no present follows. The
  allocation size and caller are absent. Add bounded reports around the i386
  C++ `new` and `new[]` failure paths, retaining the original allocators,
  handlers and exceptions. Both D3D9 and D3D11 must contain this diagnostic.
  This is diagnostic work, not a rendering fix.
* **32-bit visual novel, 19:52:42:** the longer trace reaches an access violation
  while preparing dialog text. Matching the PE32 GDI DLL's symbols and fault
  bytes identifies `gdi32!ScriptPlace+0x80` (RVA `0x4b0b0`). The attribute-copy
  loop reads inaccessible guest address `0x330000`. EDI, which carries the glyph
  count in this binary, is `0x12df90`, an implausible count resembling a stack
  address. The subsequent 64-bit exception dispatcher rejects a 32-bit stack
  frame. The origin of the bad argument is still unknown. Add one bounded
  nearby EBP frame report with the caller and nine argument words on readable
  guest pages. Do not make the faulting PROT_NONE page readable or clamp the
  glyph count without understanding its source.
* **64-bit game, 19:54:34:** the startup trace checks
  `C:\users\Public\Documents`, receives false from `PathFileExistsW`, then
  opens the internal-error dialog. Wine's fallback resolves Common Documents
  to that directory. Create the six standard public profile directories during
  FEX64 prefix preparation, after the directory mount is attached. Preserve
  existing nodes. This addresses the observed missing-folder condition;
  subsequent application startup and rendering remain device acceptance work.
* **64-bit DXMT cube, 19:55:57:** DXMT drawable hostcalls continue through the
  end of the capture. This is evidence of continued graphics activity, not
  visual confirmation of the phone's display. No DXMT rendering changes are
  included. Exclude repetitive heap-free, fiber-local-storage and character-type
  relay calls to reduce verbose tracing overhead and log size.

## Source checks

[Wine 9 shell folder resolution](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/shell32/shellpath.c)
checks folder existence when creation was not requested.
[Wine 9 ScriptPlace](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/gdi32/uniscribe/usp10.c)
copies one visual attribute per glyph into its temporary glyph-property array.
[GNU ld wrapping](https://sourceware.org/binutils/docs/ld/Options.html)
redirects undefined symbol references; the diagnostic uses the i386 MinGW
operator names and forwards to their original definitions.

## Validation and delivery boundary

The native Windows i386 GCC 15.2 allocation probe passed normal scalar/array
allocation, zero-size allocation, failing scalar/array allocation, a throwing
new-handler, and propagation of a different exception. The expected failure
reports contain allocation size, image base and caller. The 32 relay tests and
the Windows support CTest pass. iPhone compilation and packaged DLL marker
validation run in GitHub Actions; device success requires another capture.

Publish only the increased-memory entitlement template `BoxedVN.ipa`.
The rebuilt DXVK DLLs must also reach the device through the matching
`wine64-pe32.zip`: replace the existing external ZIP in the selected container
(`main` in these captures). Updating only the IPA leaves the FEX64 WoW64 lane's
old DXVK DLLs in use. Do not download the IPA or verify its hash on the user's
behalf.
