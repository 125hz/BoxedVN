# Wine 11 unified loader and shared window-session views

Device revision `0aaf403a+dirty`, build 137. Captures `203334`, `203458`,
`203655`, and `203751` pass the previous server fork failure. The daemon
survives and services start. Two shared-runtime incompatibilities remain.

## Unified WoW64 loader

The 32-bit launches end at CPU64 RIP `0x7bf647bf`, bytes `c2 04 00`, with
status `0xc000001d`. This is a fallback interpreter execution of 32-bit
ntdll, not a FEX graphics failure. PID 10 loaded `start.exe /exec`, held the
translator while waiting, and denied PID 39 the role at exec.

Wine 11 `get_alternate_wineloader()` under `WINEARCH=win64` returns an
`i386-unix/wine` loader path for an i386 image even though that Unix loader
is not packaged. `build_initial_params()` consequently falls back to
`start.exe`. Setting the supported `WINEARCH=wow64` mode makes the loader
keep both PE architectures in the 64-bit Unix process, which is the CPU and
memory ownership model of this runtime. The default now selects that mode.
Caller overrides remain supported. Existing win64 prefixes remain valid;
this changes the execution mode, not the prefix directory or saved data.

## Read-only window-session data

Both the desktop and the 64-bit graphics probe repeatedly report
`NtUserRegisterClassExWOW Failed to get shared session object for window class`.
Repeated desktop creation launches more helpers. Their logs end without an
app shutdown or native crash report, so memory-pressure termination is a
plausible consequence, not a confirmed iOS crash cause.

Wine 11 win32u maps `\KernelObjects\__wine_session` with PAGE_READONLY.
Its Unix `map_file_into_view()` chooses writable MAP_PRIVATE on Linux for
read-only sections, relying on Linux private mappings seeing shared page
cache changes before copy-on-write. BoxedWine's file mapper copies private
file bytes eagerly, while wineserver's MAP_SHARED writes live in the shared
registry rather than on disk. Thus these views miss both existing and later
server object updates. The path is incompatible even with a free translator.

A general Wine runtime patch selects the existing portable non-Linux path:
MAP_SHARED with PROT_READ for read-only sections. Writable sections remain
shared and VPROT_WRITECOPY remains private, preserving image relocation
isolation. No program names, checks, or binary modifications are involved.
The patch and its test participate in the Wine compilation cache key.
Additional bounded Wine winstation warnings are enabled for device diagnosis.

## Validation and remaining acceptance

The regression compiles the actual Wine mapping policy and alternate-loader
function. Unmodified Wine fails the read-only shared mapping check; the
patched source passes read-only/shared, writable/shared, copy-on-write/private,
and both PE architectures in unified WoW64 mode. The production mmap syscall
fixture additionally checks a late read-only session view at a nonzero file
offset and visibility of subsequent object updates. All eight MSVC host test
suites pass after updating the launch default expectations.

This requires one Wine runtime recompilation. Only the Unix runtime and app
launch environment change; the Wine 11 PE32 archive and base filesystem can
be retained. Device acceptance is still required for desktop creation and
first frames in both architecture probes, followed by regular programs.

References:
- [Wine 11 mapping policy](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/unix/virtual.c).
- [Wine 11 window-session views](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/win32u/winstation.c).
- [Wine 11 alternate loader](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/unix/loader.c).
- [Wine 11 startup fallback](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/ntdll/unix/env.c).
