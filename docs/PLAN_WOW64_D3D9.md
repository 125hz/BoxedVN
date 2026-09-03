# Plan: Direct3D 9 for 32-bit programs on the WoW64 lane

Status: steps 1, 2 and 4 are implemented -- the 64-bit guest Vulkan ICD, the
host dispatcher with the bootstrap command set, and the packaging of both the
ICD and the 32-bit DXVK override. Steps 3, 5 and 6 are written down below with
what each one still needs. This document is the successor to phase 4 of
`docs/PLAN_WOW64_UNDER_FEX64.md` for Direct3D 9, which that plan did not cover
(it plans D3D11 through DXMT).

## The blocker, stated exactly

The 32-bit Direct3D 9 probe now runs its own code to the end of startup and
puts up a Win32 message box: `Direct3DCreate9 failed (HRESULT 0x80004005)`.
Everything before that works. In the 02:10 device run the WoW64 lane resolved
the whole 32-bit import chain out of `c:/windows/syswow64` -- kernel32,
kernelbase, advapi32, sechost, msvcrt, ucrtbase, gdi32, user32, win32u,
opengl32, wined3d, d3d9, zlib1 and libgcc -- entered 32-bit mode
(`FAR_TRANSFER_POST ... cs=0x23 ... decode=32`), created its window, and only
then failed at device creation. `BOXEDWINE_X64_PE32_GAP tree=i386-windows
required=14 missing=0` in the 02:12 run says the packaging gap that killed the
previous attempt is closed.

**Update.** The `libgcc` in that chain is `libgcc_s_dw2-1.dll`, and a later
device run no longer resolved it: it missed in all four search directories,
immediately after `opengl32.dll` was mapped and again after `uxtheme.dll`.
Wine builds neither tree's copy -- it is the i686 mingw unwinder runtime that
Ubuntu's mingw-built i386 PE modules import -- so it is now a required name
alongside `zlib1.dll`, which raises `K_X64_WOW64_LANE_PE32_MODULE_COUNT` to 15
and makes later logs read `required=15`. The workflow supplies it from
`gcc-mingw-w64-i686` when the i386 package lacks it. Note the failure mode is
softer than zlib1's: the importing builtin fails to load and its caller runs
on without it, so the process reaches its own code and only Direct3D is gone.

Three facts from that run decide the design.

**1. Wine's own d3d9 is wined3d, and wined3d needs OpenGL or Vulkan.** There is
no OpenGL on iOS, and the lane has no Vulkan either. Both dlopens are in the
log, back to back, immediately before the message box is built:

```
open('/usr/lib/boxedwine64-x11/libvulkan.so.1') -> -2
open('/etc/ld.so.cache')                        -> 18
open('/lib/x86_64-linux-gnu/libvulkan.so.1')    -> -2
open('/usr/lib/x86_64-linux-gnu/libvulkan.so.1')-> -2
open('/lib/libvulkan.so.1')                     -> 18      <-- opened
open('/usr/lib/libvulkan.so.1')                 -> -2
... then the identical sequence for libGL.so.1, with /lib/libGL.so.1 -> 18
```

`/lib/libvulkan.so.1` and `/lib/libGL.so.1` **open and the search continues
past them**, which is what a dynamic loader does when a file it opened is not
a usable object. Those two files are the IA-32 lane's guest shims, shipped in
the Boxedwine root filesystem: i386 ELFs that trap to the host with
`int 0x9A` and `int 0x99`. A 64-bit guest's `ld-linux` rejects them for their
ELF class and keeps looking, finds nothing, and the driver has no
implementation. The 64-bit lane has never had a Vulkan or a GL client library
of its own.

**2. wined3d did not even try Vulkan.** Wine 9.0's
`wined3d_get_renderer()` maps `WINED3D_RENDERER_AUTO` -- the default -- to
`WINED3D_RENDERER_OPENGL` (`dlls/wined3d/wined3d_main.c`), and
`adapter_vk.c:173` is the only place that would load `winevulkan.dll` or
`vulkan-1.dll`. Neither name appears anywhere in the run's 437 `DLL_SEARCH`
lines for that process, and the trace budget (1024 per process) was not spent,
so the absence is real rather than truncated. The `libvulkan.so.1` dlopen
above is the X11 driver's own initialisation, not wined3d's. So even a working
64-bit Vulkan would not change this run's outcome without
`HKCU\Software\Wine\Direct3D\renderer = "vulkan"` in the prefix.

**3. No unix-side Vulkan library was ever loaded.** The `x86_64-unix`
libraries the run opened are `ntdll.so`, `win32u.so`, `winex11.so`,
`opengl32.so`, `ws2_32.so` and `mountmgr.so`. `winevulkan.so` is not among
them.

## What exists

### The IA-32 lane's Vulkan bridge (the model, and why it is not reusable)

| Piece | Where |
|---|---|
| Guest library | `/lib/libvulkan.so.1` in the Boxedwine root filesystem (upstream's build; nothing in this repo builds it). i386 ELF. |
| Guest trap | `int 0x9A`, decoded by every IA-32 CPU backend (`source/emulation/cpu/common/common_other.cpp:43`, `normal/normal_other.h:210`, `jit/jitOther.h:144`, table entry in `decoder.cpp:455`). |
| Kernel entry | `callVulkan(CPU*, U32 index)` -- `source/vulkan/vulkancommon.cpp:718`. |
| Dispatch table | `int9ACallback[]`, 645 entries, filled by `vulkan_init()` (`vulkancommon.cpp:699`), which `startupArgs.cpp:1228` calls at startup. |
| Marshal | `source/vulkan/vk_host.cpp`, `vk_host_marshal.cpp` (generated, ~2.7 MB together), plus `vkdef.h`/`vkfuncs.h`. Reads and writes guest memory through the 32-bit page table and keeps a per-process pointer map (`KProcess::vulkanPtrMap`). |
| Host | MoltenVK 1.4.2, static arm64, pinned in `scripts/dependencies.lock.sh`. Reached through `KNativeSystem::getVulkan()` / `platform/sdl/kvulkanSDL.cpp`; surfaces are created with `SDL_Vulkan_CreateSurface`. |
| Build gate | `BOXEDWINE_VULKAN`, defined for `BOXEDVN_TARGET_IOS` and for the macOS host build (`CMakeLists.txt:324,330`). |

The guest half is 32-bit only: an i386 ELF, an opcode CPU64 does not decode,
and a marshal whose pointer handling is written against 32-bit guest
addresses. The *host* half -- everything from `int9ACallback[]` inward -- is
architecture-neutral and is the part worth keeping.

### The 64-bit lane

| Question | Answer |
|---|---|
| Does `source/vulkan` compile into the 64-bit kernel? | Yes. `BOXEDWINE_VULKAN` is a `PUBLIC` definition on `boxedwine_core`, which is one library for both lanes, and `vulkan_init()` runs on every iOS launch. It is simply unreachable from a 64-bit guest. |
| Is there a 64-bit guest Vulkan shim? | No. `scripts/build-boxedwine-x64-x11.sh` builds `libX11.so.6`/`libXext.so.6` from `tools/x11-64`; there is no equivalent for Vulkan or GL. |
| Does the 64-bit kernel have a Vulkan hostcall? | It did not. `syscall64.cpp` reserved two host bridges, `BOXEDWINE_X64_HOSTCALL_DXMT_UNIX_CALL` (0x7fff0001) and `BOXEDWINE_X64_HOSTCALL_X11_BRIDGE` (0x7fff0002). A third is added by this change. |
| Is there a 64-bit OpenGL bridge? | No. `gl64Bridge` / `GL64_SYSCALL_NR` are referenced in `syscall64.cpp` under `#ifdef BOXEDWINE_OPENGL`, but nothing defines that macro and `source/opengl/gl64bridge*` does not exist. Dead code. |
| Does Wine give 32-bit code a route to the 64-bit unix Vulkan? | Yes, and it is free. Wine 9.0's `dlls/winevulkan/vulkan_thunks.c:45435` defines `__wine_unix_call_wow64_funcs[]`, and `dlls/winevulkan/vulkan.c` carries the `is_wow64()` test and the `*32` entry points that take `UINT32` fields and `UlongToPtr` them. A 32-bit `winevulkan.dll` reaches the 64-bit `winevulkan.so` through the standard WoW64 unixlib dispatch; nothing on our side has to thunk anything. |
| Which DXVK does the repo produce? | It does not build one; it ships prebuilt binaries. `ios/app/Dxvk/{d3d9,d3d11,d3d10core,dxgi}.dll` are DXVK 2.5.2 (upstream b4faf0b) cross-built `i686-w64-mingw32` and patched for MoltenVK (`third_party/patches/dxvk-2.5.2-moltenvk.patch`). All four are PE32, machine 0x14c -- 32-bit, which is exactly the class `syswow64` wants. The patch disables `geometryShader`, `shaderCullDistance`, `robustBufferAccess2` and `nullDescriptor` for the MoltenVK device and makes `textureCompressionBC` and `VK_EXT_transform_feedback` conditional. |

## The design

```
32-bit program (i386 PE)
  └─ d3d9.dll                DXVK 2.5.2 i386, the file already in ios/app/Dxvk,
     │                       projected over syswow64 with WINEDLLOVERRIDES d3d9=n
     └─ vulkan-1.dll (i386)   Wine builtin forwarder
        └─ winevulkan.dll (i386)
           └─ __wine_unix_call, WoW64 arm       Wine's own thunks, nothing of ours
              └─ x86_64-unix/winevulkan.so       Wine's 64-bit unix library
                 └─ dlopen("libvulkan.so.1")     the piece that does not exist
                    └─ 64-bit guest shim         NEW: tools/vulkan-64, staged at
                       │                         /usr/lib/boxedwine64-x11
                       └─ syscall 0x7fff0003     NEW: the bridge in syscall64.cpp
                          └─ int9ACallback[]     existing host marshal
                             └─ MoltenVK → Metal
```

Why DXVK rather than wined3d's Vulkan renderer: wined3d over Vulkan is a
second translation of the same problem and has to be switched on by a registry
key, and the repo already ships a DXVK `d3d9.dll` of the right architecture,
already patched for MoltenVK's feature gaps and already the renderer the IA-32
lane uses for the same programs. wined3d-over-Vulkan stays available as a
one-registry-key comparison once Vulkan reaches the guest, and is the fallback
if DXVK's D3D9 path turns out to need something Metal cannot do.

Why a guest shim rather than teaching CPU64 to decode `int 0x9A`: the 64-bit
lane's guest code is translated by FEX, not by a BoxedWine decoder, so a new
trap opcode would be a translator patch. A reserved syscall number is the
mechanism the DXMT unix call and the X11 bridge already use, FEX passes
`SYSCALL` through untouched, and the shim is an ordinary ELF the guest loader
finds first on `LD_LIBRARY_PATH`.

### What has to be built

1. **The 64-bit guest `libvulkan.so.1`.** An x86-64 ELF with the Vulkan loader
   soname. **Correction to an earlier draft of this plan:** it is not
   `winevulkan.so` that dlopens the library and it is not six symbols. Wine
   9.0's `dlls/winex11.drv/vulkan.c` is what calls
   `dlopen(SONAME_LIBVULKAN, RTLD_NOW)`, and its `LOAD_FUNCPTR` list is
   **sixteen required symbols** -- `vkCreateInstance`, `vkCreateSwapchainKHR`,
   `vkCreateXlibSurfaceKHR`, `vkDestroyInstance`, `vkDestroySurfaceKHR`,
   `vkDestroySwapchainKHR`, `vkEnumerateInstanceExtensionProperties`,
   `vkGetDeviceProcAddr`, `vkGetInstanceProcAddr`,
   `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`,
   `vkGetPhysicalDeviceSurfaceFormatsKHR`,
   `vkGetPhysicalDeviceSurfacePresentModesKHR`,
   `vkGetPhysicalDeviceSurfaceSupportKHR`,
   `vkGetPhysicalDeviceXlibPresentationSupportKHR`, `vkGetSwapchainImagesKHR`
   and `vkQueuePresentKHR` -- plus four `LOAD_OPTIONAL_FUNCPTR` names. Missing
   one of the sixteen makes the driver close the handle and give up, which in
   a log is indistinguishable from the library not existing. winevulkan then
   takes `vk_funcs` from `__wine_get_vulkan_driver` and reaches everything
   else through `vkGetInstanceProcAddr`. The list lives in
   `tools/vulkan-64/winex11-vulkan-imports.txt` and the validator re-measures
   it against the packaged `winex11.so`'s own string table.

   **What that re-measurement can and cannot decide.** A `dlsym` argument is
   an ordinary string literal, so a scan of the driver sees it -- but it sees
   every other literal too, and the first CI run failed on five of them. So
   the check is split. What is decidable, and is fatal, is the reverse
   direction: a name the contract records as dlsym'd that the driver no longer
   spells out, which means Wine renamed or dropped it. What is not decidable
   -- a name the driver mentions that the contract does not list -- is printed
   as a note telling the reader to go and read `vulkan.c`, and the answer is
   written back into a `[not-imported]` block with the reason. That block
   holds five names today: `vkCreateWin32SurfaceKHR` and
   `vkGetPhysicalDeviceWin32PresentationSupportKHR` (literals in
   `wine_vk_host_fn_name()`, which rewrites the Win32 spelling to the Xlib one
   before anything is looked up, and also the driver's own `X11DRV_*`
   exports), `vkGetPhysicalDeviceProperties2KHR` and
   `vkGetPhysicalDeviceMemoryProperties2KHR` (requested through
   `vkGetInstanceProcAddr`, never dlsym'd), and `vkGetRandROutputDisplayEXT`
   (display enumeration, also absent from the `LOAD_FUNCPTR` list). The scan
   itself is now anchored at a string boundary and requires `vk` followed by
   an upper-case letter, which is what stops the driver's `X11DRV_vk*` exports
   and its internal `wine_vk_init` /
   `wine_vk_instance_convert_create_info` from matching at all.

   **KHR aliases.** Un-dlsym'd does not mean unwanted. DXVK asks for
   `vkGetPhysicalDeviceProperties2KHR` and
   `vkGetPhysicalDeviceMemoryProperties2KHR` through `vkGetInstanceProcAddr`
   on a Vulkan 1.0 instance, and a NULL answer is how it concludes the driver
   has no `VK_KHR_get_physical_device_properties2` and cannot be used at all.
   `BOXEDWINE_X64_VK_ALIASES` in the ABI header lists the six commands
   promoted from KHR into Vulkan 1.1 that this bridge carries; the guest ICD
   exports each alias under the core command's own entry point (the trap
   carries the operation number, not the name), and the host tries the core
   spelling against MoltenVK first and the KHR spelling second, because a
   driver may expose only one of the two.
   *Done:* `tools/vulkan-64/vulkan.c`,
   `scripts/build-boxedwine-x64-vulkan.sh`,
   `scripts/validate-x64-vulkan-shim.py`.
2. **The operation table.** `include/boxedwine_x64_vulkan_bridge.h` holds it.
   **The IA-32 marshal is not reused, and does not need to be.** It exists
   because a 32-bit guest pointer is not a host address and a 32-bit guest's
   Vulkan structures are not the host's; on this lane neither is true. The
   bridge is served only to the identity-mapped process, where a guest address
   *is* the host address, and every Vulkan structure has an identical layout
   on x86-64 System V and arm64 AAPCS64 (both LP64, natural alignment
   throughout, no bitfields, no packed members). So the host casts the
   argument words to the real Vulkan types and calls MoltenVK directly. That
   removes the 2.7 MB of generated marshal from this lane entirely and
   replaces it with one typed call per command in
   `source/vulkan/vulkanbridge64.cpp`. The two things it must never do are
   also the two the IA-32 marshal avoids: `pAllocator` is forced to NULL on
   every command that takes one, and no command whose parameters include a
   guest callback is in the table (a guest function pointer is x86-64 code the
   arm64 host cannot call).
   Operation numbers are the IA-32 lane's own ordinals from
   `source/vulkan/vkdef.h` plus `0x1000`, so the two lanes can never drift and
   a diagnostic printing one number means the same command on both;
   `scripts/test_x64_vulkan_shim.py` asserts every ordinal against `vkdef.h`.
   `vkCreateXlibSurfaceKHR` is the single exception to pass-through: MoltenVK
   has no Xlib surface extension, so the bridge reads the window id out of the
   guest's `VkXlibSurfaceCreateInfoKHR` (offset 32 in the x86-64 layout) and
   builds the surface through `KNativeSystem::getVulkan()`, exactly as
   `BOXED_vkCreateXlibSurfaceKHR` does for the IA-32 lane.
   *Done:* the bootstrap set -- instance, physical device, device, queue,
   memory, buffers, images, image views, fences, semaphores, surface and
   swapchain: 66 commands. *Not done:* the recording half (command pools and
   buffers, `vkCmd*`, pipelines, descriptors, render passes, samplers, query
   pools). Those are deliberately absent rather than guessed: every name a
   caller asks for and the table does not carry is printed as
   `BOXEDWINE_X64_VULKAN_BRIDGE call=vkGetProcAddr missing=<name>`, so one
   device run produces DXVK's real requirement list instead of a guess.
   Two things the next batch has to respect that the current one did not have
   to: a command with a `float` parameter (`vkCmdSetLineWidth`,
   `vkCmdSetDepthBias`, `vkCmdSetBlendConstants`, `vkCmdSetDepthBounds`)
   passes it in a vector register, and a command with more than eight
   integer/pointer parameters (`vkCmdPipelineBarrier`, `vkCmdWaitEvents`)
   passes the rest on the stack, where Apple's arm64 ABI packs sub-8-byte
   arguments to their natural size. Both are handled correctly by the typed
   call this file already uses and would be handled incorrectly by any
   generic forwarder, which is why there is not one.
3. **Packaging.** The shim goes to `K_X64_GUEST_VULKAN_LIB_PATH`
   (`/usr/lib/boxedwine64-x11/libvulkan.so.1`), which the 02:10 log proves is
   the first path the guest loader tries. That directory is already staged by
   `scripts/build-wine64-runtime-ci.sh --x11-shim-dir`; the new artifact rides
   in the same directory rather than inventing a second one.
   *Done:* `--vulkan-shim` in the builder, a required entry in
   `scripts/validate-wine64-runtime.sh`, and the build step in
   `.github/workflows/build-ios.yml`.
4. **The DXVK override.** Stage `ios/app/Dxvk/d3d9.dll` into the 32-bit PE
   layer under a distinct directory (`dxvk-i386/`) rather than over
   `i386-windows/d3d9.dll`, project it into `syswow64` only when the launch
   opts in, and pass `WINEDLLOVERRIDES=d3d9=n` for that launch. Opt-in by
   environment variable `BOXEDVN_WOW64_D3D9=dxvk` (default: unset, meaning
   Wine's own d3d9), so a broken Vulkan path cannot regress the lane's current
   behaviour.
   *Done:* `--dxvk-i386-dir` in the builder,
   `K_X64_WINE_DXVK_PE32_DIR` and `K_X64_WOW64_D3D9_ENV` in
   `include/guest_wine64_layout.h`, `projectX64WineDxvkD3d9()` in
   `source/sdl/startupArgs.cpp`, and `X64Runtime.wow64Environment` in
   `ios/app/Sources/AppModel.swift`, which sets the variable for the two
   launches that enter 32-bit code (the bundled D3D9 probe, and a program
   whose PE header says i386).
5. **The renderer key, for comparison only.** `renderer = "vulkan"` under
   `HKCU\Software\Wine\Direct3D` makes wined3d take `adapter_vk.c` and load
   `winevulkan.dll`. Worth one run now that step 1 has landed, because it
   exercises the same chain with none of DXVK's requirements and its failure
   mode is easier to read. Nothing writes that key yet.

### Risks

- **MoltenVK feature gaps.** The DXVK build already carries the patch for the
  four features Metal has no answer for, and the same binaries drive Direct3D 9
  on the IA-32 lane today, so the feature question is answered for D3D9
  specifically. What is not answered is whether MoltenVK behaves the same when
  the Vulkan calls arrive through a 64-bit guest shim rather than a 32-bit one;
  the marshal is shared, the pointer widths are not.
- **Memory model.** This is the deepest risk and it is the same one the DXMT
  work hit. Host Vulkan handles are host pointers, so only the one process per
  session that FEX translates -- the one holding the identity map -- can use
  them; a `CreateProcess` child gets a sparse `KMemory64` and cannot
  (`docs/KNOWN_LIMITATIONS_IOS.md` section 4). The bridge enforces that
  directly: every Vulkan command from a process without the identity map is
  refused as `BOXEDWINE_X64_VULKAN_BRIDGE call=<name> status=-7
  reason=native-memory`, and the guest ICD declines at load time when the
  capability probe says the same, so a forked child gets
  `VK_ERROR_INITIALIZATION_FAILED` rather than a fault inside Metal.

  The second half -- `vkMapMemory` returning an address a *32-bit* caller can
  hold -- turns out to be Wine's problem and Wine already solves it, which
  changes what step 3 is. Wine 9.0's `wine_vkAllocateMemory` checks
  `device->phys_dev->external_memory_align`, allocates the mapping itself with
  `NtAllocateVirtualMemory` and `zero_bits` set (which on WoW64 forces the
  allocation below 4 GiB), and imports it with
  `VkImportMemoryHostPointerInfoEXT`; `wine_vkMapMemory2KHR` then checks
  `(UINT_PTR)*data >> 32` and, if the driver handed back a pointer above
  4 GiB for a WoW64 caller, unmaps it and returns
  `VK_ERROR_OUT_OF_HOST_MEMORY` with `FIXME("returned mapping %p does not fit
  32-bit pointer")`. So there is nothing for this bridge to place: the memory
  Wine imports is guest memory in the low alias, which under the identity map
  is a real host address below 4 GiB, page-aligned, and passes straight
  through. What step 3 actually requires is that
  `VK_EXT_external_memory_host` reaches the guest --
  `vkEnumerateDeviceExtensionProperties` and `vkGetPhysicalDeviceProperties2`
  are pass-through so it does if MoltenVK has it, and
  `vkGetMemoryHostPointerPropertiesEXT` is already in the command table for
  exactly this reason. **Unverified:** whether the pinned MoltenVK 1.4.2
  advertises `VK_EXT_external_memory_host`. If it does not, `vkMapMemory` will
  return a >4 GiB pointer, Wine's own check will refuse it, and the
  `BOXEDWINE_X64_VULKAN_MAP ... low4g=0` witness this change adds will say so
  in one line. The fallback in that case is the DXMT `CpuPlaced` discipline
  from `scripts/dxmt-patches`: have the bridge itself place mappable memory in
  the guest's low alias.
- **Low-alias pressure.** Every 32-bit allocation, including whatever DXVK maps
  for staging, lives below 4 GiB through `include/guest_low_alias.h`. A real
  program with large textures can exhaust that faster than the legacy lane.
- **The shim is a large surface.** The IA-32 lane's table has 645 entries. The
  64-bit shim does not need all of them, but it needs whatever DXVK calls, and
  the honest way to find that out is to implement the bootstrap set and read
  the `BOXEDWINE_X64_VULKAN_BRIDGE ... name=?` lines a run produces.
- **Two shims, one soname.** Once a 64-bit `libvulkan.so.1` exists, both lanes
  have a file with that name and the wrong one is merely skipped rather than
  reported. The startup witness below is what makes that visible.

### Steps and the marker that proves each one

| # | Step | Marker the next device run has to show |
|---|---|---|
| 0 | Bridge reserved and reachable | `BOXEDWINE_X64_VULKAN_SHIM present=... hostcall=0x7fff0003 abi=2 soname=libvulkan.so.1 path=/usr/lib/boxedwine64-x11/libvulkan.so.1 bridge_ops=66 lane32_ops=645` at startup, on every launch |
| 1 | Guest ICD built and staged | the same line with `present=1`, and `open('/usr/lib/boxedwine64-x11/libvulkan.so.1') -> ` a file descriptor with **no** further `libvulkan.so.1` search after it |
| 2 | ICD reaches the host | `BOXEDWINE_X64_VULKAN_BRIDGE ordinal=0 pid=10 op=0 name=abi ... status=2` then `op=1 name=probe ... caps=0x7` -- all three capability bits: the code is compiled in, the driver loaded, and the caller holds the identity map |
| 3 | Wine binds the loader | `x86_64-unix/winevulkan.so` in the `DLL_SEARCH` opens, and a `winevulkan.dll` open from `syswow64`. If `i386-windows/vulkan-1.dll` or `winevulkan.dll` is missing, the CI runtime build now warns by name before the archive is made |
| 4 | Instance and device | `BOXEDWINE_X64_VULKAN_BRIDGE call=vkCreateInstance ... status=0` then `call=vkEnumeratePhysicalDevices ... status=0`, then `call=vkCreateDevice ... status=0`; and `missing=<name>` lines naming every command DXVK asked for that the table does not carry |
| 5 | DXVK selected | `BOXEDWINE_X64_MODULE_OVERLAY name=d3d9.dll source=.../dxvk-i386/d3d9.dll ... status=projected tree=dxvk-i386` and `status=override-applied WINEDLLOVERRIDES=d3d9=n`, and no `Direct3DCreate9 failed` message box |
| 5a | Mapped memory fits a 32-bit caller | `BOXEDWINE_X64_VULKAN_MAP ... low4g=1`. `low4g=0` means MoltenVK has no `VK_EXT_external_memory_host`, Wine will refuse the mapping, and step 3 of the risk section becomes real work |
| 6 | First frame | `BOXEDWINE_X64_VULKAN_PRESENT frame=0 status=0`, and `BOXEDWINE_X11_PRESENT` for the program's own window rather than the desktop's |

## What is implemented

- `include/boxedwine_x64_vulkan_bridge.h`. The ABI, in the shape of
  `include/boxedwine_x64_x11_bridge.h`: hostcall number `0x7fff0003`, an
  IN/OUT `uint64_t` argument array of at most 16 slots, the error codes, an
  ABI version the shim refuses to run against a mismatch of, the capability
  bits, the guest-side inline `syscall` helper compiled only for `__x86_64__`,
  and `BOXEDWINE_X64_VK_COMMANDS` -- the 66-command table that the guest ICD,
  the host dispatcher and the validator are all generated or checked from.
- `tools/vulkan-64/vulkan.c`. The guest ICD. One entry point per command, each
  packing its arguments and trapping; a `vkGetInstanceProcAddr` /
  `vkGetDeviceProcAddr` pair that asks the host about every name and returns
  NULL for one the host cannot serve; and a load-time capability check that
  turns "this build has no Vulkan" or "this process is not the identity-mapped
  one" into `VK_ERROR_INITIALIZATION_FAILED` rather than a syscall errno cast
  to a `VkResult`. It needs no Vulkan headers to build: it passes every
  structure through by pointer and never looks inside one.
- `tools/vulkan-64/winex11-vulkan-imports.txt`,
  `scripts/build-boxedwine-x64-vulkan.sh`,
  `scripts/validate-x64-vulkan-shim.py`. The build and its contract. The
  builder refuses a compiler that does not target x86-64, by name, rather than
  producing nothing.
- `source/vulkan/vulkanbridge64.cpp` (new) and `vulkanbridge64.h`. The host
  dispatcher: argument-array validation against the guest page table for read
  *and* write, the identity-map refusal, one typed call per command, the
  `vkCreateXlibSurfaceKHR` special case, and the witnesses.
- `source/kernel/syscall64.cpp`. Reduced to the syscall number, the three
  `static_assert`s that keep it from colliding with the DXMT unix call or the
  X11 bridge, and one forwarding case.
- `source/vulkan/vulkancommon.cpp`. The `BOXEDWINE_X64_VULKAN_SHIM` startup
  line, now also carrying `bridge_ops`. `present` and `icd` are answered from
  the mounted guest filesystem rather than from a compile-time define: the
  define (`BOXEDWINE_X64_VULKAN_GUEST_SHIM`) was never set by any build, so
  the line read `present=0 icd=none` even on runs whose `wine64.zip` carried
  the file. `present=1` now means the path resolves to an ELF64 object;
  `icd=elf32-lane32-shim` names the IA-32 shim the 64-bit loader walks past,
  and `not-elf` / `unreadable` name the other two ways the file can be wrong.
- `include/guest_wine64_layout.h`. `K_X64_WINE_DXVK_PE32_DIR`,
  `K_X64_DXVK_PE32_MODULE_NAMES`, `K_X64_WOW64_D3D9_ENV`.
- `source/sdl/startupArgs.cpp`. `projectX64WineDxvkD3d9()` and the opt-in.
- `scripts/build-wine64-runtime-ci.sh`, `scripts/validate-wine64-runtime.sh`,
  `.github/workflows/build-ios.yml`. The packaging of both artifacts.
- `scripts/test_x64_vulkan_shim.py`. Twenty-six tests that hold the ABI
  header, `vkdef.h`, the guest ICD, the host dispatcher and the import
  contract to the same command list, without a compiler or a Wine install.

Nothing is enabled by default for a 64-bit launch: the DXVK projection runs
only when the launch sets `BOXEDVN_WOW64_D3D9=dxvk`, which only a 32-bit
launch does.

## Observed programs

Two device runs on 2026-09-03 are the evidence behind everything above. Only
the second is a real program; neither is named here.

**02:10 -- the 32-bit Direct3D 9 probe.** Reaches the message box described at
the top. Process tree: pid 10 is the launched loader, execs `wine-preloader`
then `wine` in place and keeps FEX (`BOXEDWINE_X64_EXEC pid=10 ... fex=1
native=1`); it forks the wineserver (12 -> 14, 12 exits) and `wineboot --init`
(16 -> 18), which starts `services.exe` (20 -> 22) and `winedevice.exe`
(25 -> 27). pid 10 enters 32-bit mode at 02:10:45.840
(`FAR_TRANSFER_POST kind=jmpf from=0x7bce1277 rip=0x78e64516 cs=0x23 decode=32`),
loads the whole 32-bit chain from `syswow64`, and fails at
`Direct3DCreate9`. The two failed dlopens quoted above are at the end of the
log; the 330x298 window created straight afterwards is the message box. The
run then sits in `__psynch_cvwait`, which is the message box waiting for a
click, not a hang.

**02:12 -- a 32-bit program launched from the app's Run program row**
(`wine D:\<folder>\<name>_en.exe`). Reconstructed tree, with line numbers:

- 18: `container: Launching ... through BoxedWine FEX and DXMT`.
- 137-138: the 32-bit PE layer is mounted and projected -- 785 modules,
  `BOXEDWINE_X64_PE32_GAP tree=i386-windows required=14 missing=0`. The
  packaging gap that killed the earlier attempt is closed.
- 164, 430-479: pid 10 is the launched loader; it execs `wine-preloader` then
  `wine` and keeps FEX (`fex=1 native=1`).
- 597-669: 10 forks 12, which execs `wineserver -p`, double-forks to 14 and
  exits 0. **pid 14 is the persistent wineserver.** Every later
  `DLL_SEARCH pid=14 op=open` is the wineserver opening a file on a client's
  behalf, not a process of its own loading modules.
- 718-724: 10 forks 16, which forks 18 and exits 0. pid 18 is
  `wineboot.exe --init`.
- 1592-1599, 2268, 2512: 18 -> 20 -> 22 is `services.exe`, and 22 -> 25 -> 27
  is `winedevice.exe`, which exits 0 for want of a drivers directory. The same
  shape as the probe run.
- 1565-1573: pid 10 stalls in `__psynch_cvwait` at 02:12:54 -- it is waiting
  for `wineboot`, which is the normal cold-prefix wait, not a defect.
- 2899: `wineboot` exits 0.
- 2900-2914: pid 10 immediately maps the program's own image -- base
  `0x400000`, `len=0x281000`, and the low WoW64 pages at `0x10000` and
  `0x20000` -- then asks the wineserver for `ntdll.dll`, which opens
  `x86_64-windows/ntdll.dll` and then `i386-windows/ntdll.dll`, and for
  `apisetschema.dll`.
- 2915: the log ends there.

**What stops it: nothing, in this log.** The program does not spawn a second
executable -- every child in the run is Wine infrastructure, and the
`CreateProcess` double-fork signature (`FORK parent=N child=M` immediately
followed by `FORK parent=M child=P`) appears only for the wineserver,
wineboot, services and winedevice. The program does not exit: there is no
`exit_group` for pid 10 and no `PROC_EXIT pid=10`. The session is not stuck:
the last `SAMPLE` at 02:13:01 is `state=waiting` on the cold-boot wait, and
the untimestamped tail after it is pid 10 making progress through the loader.
The i386 `ntdll.dll` open at the end is the same step the probe run reached at
02:10:45 just before its far transfer into 32-bit code.

The log simply stops mid-line at about 02:13:05, roughly fourteen seconds
after launch, with no shutdown record; the next log begins a different launch
at 02:14:02. So this run was cut off (the app was left or killed) before the
lane reached 32-bit code. It is not evidence of a defect and it is not
evidence of success -- it is a truncated capture, and the run needs repeating
with the session left alone until either a window appears or a marker names a
stop. What it does prove is that the cold-prefix boot costs about thirteen
seconds before a program's first instruction, which is long enough that a
run judged by watching the screen will be abandoned before it starts.

## Tracking

| Step | Gate met | Note |
|---|---|---|
| 0 bridge reserved | earlier change | witness only |
| 1 guest ICD | this change | built by `scripts/build-boxedwine-x64-vulkan.sh`, staged by the runtime builder, required by the runtime validator |
| 2 ICD reaches host | this change | 66 commands dispatched; every other name is reported as `missing=` |
| 3 Wine binds the loader | | `i386-windows` must carry `vulkan-1.dll` and `winevulkan.dll`; still unverified, but the CI runtime build now warns by name if the i386 package lacks either |
| 4 instance and device | | the code is there; no device run yet |
| 5 DXVK selected | this change | projection and `WINEDLLOVERRIDES=d3d9=n` behind `BOXEDVN_WOW64_D3D9=dxvk` |
| 5a mapped memory below 4 GiB | | depends on MoltenVK carrying `VK_EXT_external_memory_host`; `BOXEDWINE_X64_VULKAN_MAP ... low4g=` answers it |
| 6 first frame | | needs the recording half of the command table (command pools and buffers, `vkCmd*`, pipelines, descriptors, render passes, samplers, query pools) |

## What the next session does

1. Read the run's `missing=<name>` lines and add exactly those commands to
   `BOXEDWINE_X64_VK_COMMANDS`, `tools/vulkan-64/vulkan.c` and
   `source/vulkan/vulkanbridge64.cpp`. `scripts/test_x64_vulkan_shim.py` fails
   until all three agree, and the ordinal has to come from
   `source/vulkan/vkdef.h`. Mind the float and >8-argument commands noted in
   step 2 above.
2. Read `BOXEDWINE_X64_VULKAN_MAP ... low4g=`. If it is 0, MoltenVK has no
   `VK_EXT_external_memory_host` and the bridge has to place mappable memory
   in the guest low alias itself, the way `scripts/dxmt-patches` does for
   Metal buffers.
3. Confirm `i386-windows/vulkan-1.dll` and `winevulkan.dll` from the CI log's
   new warnings. If the i386 package does not carry them, DXVK's d3d9 has no
   route to the bridge at all and they have to come from the same Wine version
   by another means -- the same problem, and the same shape of fix, as
   `zlib1.dll`.
4. Once a frame presents, try `renderer = "vulkan"` in the prefix as the
   comparison run described in step 5 of "What has to be built".
