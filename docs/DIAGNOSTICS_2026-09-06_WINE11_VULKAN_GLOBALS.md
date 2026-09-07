# Wine 11 startup: global Vulkan procedure lookup

## Device evidence

All four supplied captures (`220849`, `221001`, `221103`, `221212`) identify
revision `21714ce3+dirty`, build 137. They cover both cube architectures, the
desktop, and a 32-bit application. The previous illegal-instruction and shared
session-object failures are absent. X11 display initialization now succeeds.

The desktop capture shows `CreateDesktopW` opening an X11 display and creating
an 800x600 window, then loading the guest `libvulkan.so.1`. Its bridge reports
ABI 7 and capabilities `0x5`: host marshal and driver, without native identity
memory. The next instruction fetch is at zero. Wine's exception report returns
`c0000005` from desktop creation, followed by another window-creation call that
does not complete. The other three captures show the same zero-address fault
in interpreter-based desktop helpers immediately after that same Vulkan probe.
The main FEX thread remains waiting for a Wine server reply.

## Cause and correction

Wine 11 moved Vulkan initialization into `win32u`. Its `vulkan_init_once`
resolves `vkCreateInstance` and `vkEnumerateInstanceExtensionProperties` through
`vkGetInstanceProcAddr(NULL, name)`, then calls the extension enumerator. Our
shim routed both lookups through `bw_call`, which rejects processes without
the native map. A rejected lookup became NULL. Wine's syscall exception
recovery then escaped initialization before the enclosing `pthread_once`
finished; later window initialization waits on that unfinished initialization.

The Vulkan loader contract requires callable global entry points before an
instance exists. The shim now resolves all four global commands locally in
`vkGetInstanceProcAddr`. Calls still pass through the existing readiness and
error-conversion checks. An interpreter helper receives
`VK_ERROR_INITIALIZATION_FAILED` instead of calling NULL. Wine's adapter probe
can return normally and use its non-Vulkan display fallback. The native FEX
owner retains its existing graphics dispatch. No memory-access restriction,
game executable, prefix, or Wine version changes.

This corrects a loader API contract; it does not add accelerated rendering to
interpreter-only helper processes. Device testing is still required to confirm
startup completes and to identify any subsequent Wine 11 compatibility gaps.

## Validation

- Built the actual guest shim with GCC, warnings treated as errors.
- Added an executable fixture that includes the production shim and replaces
  only its syscall transport. It checks sparse-helper capabilities `0x5`, no
  backend, mismatched ABI, working native dispatch, host-error conversion,
  repeated initialization calls, and unknown procedure names.
- Removing the fix from a scratch copy makes the fixture fail on the first
  non-NULL global-entry-point assertion.
- All 146 Vulkan contract tests and all eight host CTest suites passed.
- The fixture runs during guest Vulkan compilation in GitHub Actions. Existing
  cache inputs cover both the shim and fixture. The cached Wine 11 compiler
  outputs can be reused; the guest bridge is rebuilt and repackaged.

## Primary sources

- [Khronos: vkGetInstanceProcAddr global-command contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html)
- [Wine 11 win32u Vulkan initialization](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/win32u/vulkan.c)
- [Wine 11 adapter discovery and its once-initialization](https://github.com/wine-mirror/wine/blob/wine-11.0/dlls/win32u/d3dkmt.c)

Wine source was checked against the locally extracted, pinned 11.0 release.
