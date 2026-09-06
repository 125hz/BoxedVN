# Vulkan pacing, WoW64 graphics, and application data

Device baseline: `acc5788a`, build 137. Captures `212507`, `212709`, and
`212847` on 2026-09-05. All changes below are shared runtime behavior;
there are no executable-name checks, game-data edits, or save resets.

## Evidence and changes

- The 32-bit D3D9 cube now animates at roughly 107–120 presents/s. The x64
  Vulkan hostcall bridge never called the app's frame limiter. Both Vulkan
  bridges now honor 30/60/120 Hz with their own pacing deadline. DXMT keeps
  its existing native presentation policy. Sleeping no longer holds the
  mutex used by the UI to change the cap.
- The 32-bit launcher logs class-not-registered for DxDiagProvider
  `{a65b8071-3bfe-4213-9a5b-491da4461ca7}`. Projected builtin modules do not
  automatically register themselves in an existing prefix. Missing server
  registrations are supplied only for packaged architectures, with Wine's
  `Apartment` threading model; existing registrations are preserved.
- After Start, WineD3D submits graphics pipelines with zero stages, then
  threads `00a8` and `00d4` report waiting for critical sections owned by
  each other. The already-packaged DXVK PE32 D3D11/DXGI/D3D10Core group is
  now projected together into syswow64 alongside DXVK D3D9. System32 keeps
  the PE32+ DXMT modules. Existing explicit DLL load orders are preserved.
  This is a shared WoW64 renderer change, not proof that the title now runs.
- The 64-bit Play crash remains a null read at image offset `0xdb6c5`.
  The current logs do not prove corrupted or missing saves. Inspection found
  a separate Linux ABI defect: newfstatat returned ENOENT for all relative
  paths using a directory FD. It now resolves against that directory and
  propagates lookup errors. Ordinary open now uses AT_FDCWD, and mkdirat
  delegates to the existing directory-aware implementation.
- `BOXEDWINE_X64_DATA_FILE` records bounded guest paths, operation results,
  file offsets, sizes/counts, and directory entry counts. Open's offset and
  requested fields carry flags and creation mode; stat's carry dirfd and
  file size; mkdir's requested field carries mode. No file contents are
  logged. Each process has separate 256-line limits for path operations,
  reads, and failures so normal reads cannot consume the failure budget.
- GDI+ and Windows Imaging Component warnings are enabled to investigate
  missing launcher artwork separately from DxDiag and game rendering.

## Validation and next device checks

Host tests exercise production stat and frame-pacing functions with isolated
backends. Stat tests include different files with the same name in cwd and
dirfd, absolute paths with invalid dirfd, EBADF/ENOTDIR/ENOENT, symlinks,
AT_EMPTY_PATH, and rejected flags. Pacing tests measure 30/60/120 Hz and check
that the settings mutex remains available during sleep. Registration,
override-preservation, and diagnostic-budget tests also pass.

Physical-device acceptance is still required. With verbose off: change the
running cube between 30 and 60 Hz; inspect launcher specs/art and press Start;
then reproduce the 64-bit Play action with existing saves intact. Look for
the DXVK PE32 route witness and data-file errors/short reads before the crash.
The previously supplied wine64-pe32.zip already contains all four DXVK DLLs;
this change does not require a new guest ZIP.

## Primary references

- [Linux stat/fstatat contract](https://man7.org/linux/man-pages/man2/stat.2.html)
- [Wine 9 DxDiag class declaration](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/dxdiagn/dxdiagn.idl)
- [Wine 9 GDI+ image loading](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/gdiplus/image.c)
- [Wine 9 WIC component registration](https://github.com/wine-mirror/wine/blob/wine-9.0/dlls/windowscodecs/regsvr.c)
- [DXVK 2.5.2 setup and DLL dependencies](https://github.com/doitsujin/dxvk/blob/v2.5.2/README.md#dll-dependencies)
