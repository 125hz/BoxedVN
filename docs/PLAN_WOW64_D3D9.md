# Plan: Direct3D 9 for 32-bit programs on the WoW64 lane

Status: design, plus the first plumbing. The bridge skeleton described in
"What was implemented" is in the tree; nothing renders yet. This document is
the successor to phase 4 of `docs/PLAN_WOW64_UNDER_FEX64.md` for Direct3D 9,
which that plan did not cover (it plans D3D11 through DXMT).

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
   soname, exporting `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`,
   `vkCreateInstance`, `vkEnumerateInstanceExtensionProperties`,
   `vkEnumerateInstanceLayerProperties` and `vkEnumerateInstanceVersion` --
   the six symbols Wine's `winevulkan.so` binds by name -- and returning every
   other entry point through the two `GetProcAddr` calls. Each one packs its
   arguments and traps through `boxedwine_x64_vulkan_call`. Modelled on
   `tools/x11-64` and built by a new `scripts/build-boxedwine-x64-vulkan.sh`,
   validated the way `scripts/validate-x64-x11-shim.py` validates the X11 shim
   (the imports Wine's `winevulkan.so` actually names, checked against the very
   `winevulkan.so` that gets packaged).
2. **The operation table.** `include/boxedwine_x64_vulkan_bridge.h` reserves
   the numbering. The host side of each operation is the existing
   `int9ACallback[]` entry; what is new per operation is only the 64-bit
   argument unpacking and pointer validation. The order that matters is the
   order `Direct3DCreate9` walks: instance creation and extension enumeration,
   physical-device enumeration and properties, surface creation
   (`VK_KHR_xlib_surface`, which the IA-32 lane already special-cases as
   `BOXED_vkCreateXlibSurfaceKHR`), device creation, then the swapchain and
   command-buffer calls DXVK needs to present a frame.
3. **Packaging.** The shim goes to `K_X64_GUEST_VULKAN_LIB_PATH`
   (`/usr/lib/boxedwine64-x11/libvulkan.so.1`), which the 02:10 log proves is
   the first path the guest loader tries. That directory is already staged by
   `scripts/build-wine64-runtime-ci.sh --x11-shim-dir`; the new artifact should
   ride in the same directory rather than inventing a second one.
4. **The DXVK override.** Stage `ios/app/Dxvk/d3d9.dll` into the 32-bit PE
   layer under a distinct directory (`dxvk-i386/`) rather than over
   `i386-windows/d3d9.dll`, project it into `syswow64` only when the launch
   opts in, and pass `WINEDLLOVERRIDES=d3d9=n` for that launch. Opt-in by
   environment variable `BOXEDVN_WOW64_D3D9=dxvk` (default: unset, meaning
   Wine's own d3d9), so a broken Vulkan path cannot regress the lane's current
   behaviour. This step needs the container setup in `ios/`, which is outside
   this change's scope; it is written down here as the next owner's first task.
5. **The renderer key, for comparison only.** `renderer = "vulkan"` under
   `HKCU\Software\Wine\Direct3D` makes wined3d take `adapter_vk.c` and load
   `winevulkan.dll`. Worth one run once step 1 lands, because it exercises the
   same chain with none of DXVK's requirements and its failure mode is easier
   to read.

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
  (`docs/KNOWN_LIMITATIONS_IOS.md` section 4). Worse, `vkMapMemory` has to
  return an address the *guest* can dereference, and under WoW64 a 32-bit
  caller needs it below 4 GiB. The DXMT `CpuPlaced` work in
  `scripts/dxmt-patches` solved the equivalent problem for Metal buffers by
  making the caller place the memory rather than letting Metal own it; the
  Vulkan bridge needs the same discipline -- allocate mappable memory in the
  guest's low alias and hand Vulkan a pointer into it -- and the IA-32 lane's
  marshal does not do that today because on that lane every guest address is
  translated anyway.
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
| 0 | Bridge reserved and reachable | `BOXEDWINE_X64_VULKAN_SHIM present=0 icd=none hostcall=0x7fff0003 abi=1 soname=libvulkan.so.1 path=/usr/lib/boxedwine64-x11/libvulkan.so.1 lane32_ops=645` at startup, on every launch |
| 1 | Guest shim built and staged | the same line with `present=1`, and `open('/usr/lib/boxedwine64-x11/libvulkan.so.1') -> ` a file descriptor with **no** further `libvulkan.so.1` search after it |
| 2 | Shim reaches the host | `BOXEDWINE_X64_VULKAN_BRIDGE ordinal=0 pid=10 op=1 name=probe ... caps=0x3` -- both capability bits, i.e. the host marshal is compiled in and the caller holds the identity map |
| 3 | Wine binds the loader | `x86_64-unix/winevulkan.so` in the `DLL_SEARCH` opens, and a `winevulkan.dll` open from `syswow64` |
| 4 | Instance and device | `BOXEDWINE_X64_VULKAN_BRIDGE ... name=create-instance` then `name=enumerate-physical-devices` with a non-zero count, and no `name=?` line before them |
| 5 | DXVK selected | `BOXEDWINE_X64_MODULE_OVERLAY name=d3d9.dll source=.../dxvk-i386/d3d9.dll ... status=projected` and no `Direct3DCreate9 failed` message box |
| 6 | First frame | a present through the bridge, and `BOXEDWINE_X11_PRESENT` for the program's own window rather than the desktop's |

## What was implemented in this change

Only what is safe to land while the 64-bit lane has no Vulkan at all, which is
the plumbing and the witnesses:

- `include/boxedwine_x64_vulkan_bridge.h` (new). The ABI, in the shape of
  `include/boxedwine_x64_x11_bridge.h`: hostcall number `0x7fff0003`, an
  IN/OUT `uint64_t` argument array of at most 16 slots, the error codes, an
  ABI version the shim can refuse on, the capability bits, and the guest-side
  inline `syscall` helper compiled only for `__x86_64__`.
- `source/kernel/syscall64.cpp`. `boxedwineVulkanBridge64()` beside
  `boxedwineDxmtUnixCall64()`, wired into `ksyscall64`'s switch and named in
  `x64SyscallName`. It validates the argument array against the guest page
  table for read *and* write before touching a slot, answers `abi`, `probe`
  and `echo`, returns `BOXEDWINE_X64_VK_E_UNIMPL` for everything else, and
  logs `BOXEDWINE_X64_VULKAN_BRIDGE` for the first sixteen calls and for every
  unknown operation. Three `static_assert`s keep the number from colliding
  with the DXMT unix call or the X11 bridge.
- `source/vulkan/vulkancommon.cpp`. The `BOXEDWINE_X64_VULKAN_SHIM` startup
  line, emitted from `vulkan_init()` so it appears on every launch of either
  lane. `present` is compiled from `BOXEDWINE_X64_VULKAN_GUEST_SHIM`, which
  the packaging step defines when it stages the shim.
- `include/guest_wine64_layout.h`. `K_X64_GUEST_VULKAN_SONAME` and
  `K_X64_GUEST_VULKAN_LIB_PATH`, with the search trace that proves the path,
  so the builder, the packaging and the witness cannot disagree.

Nothing renders and nothing is enabled by default. A run of the current tree
produces exactly one new line, the startup witness with `present=0`.

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
| 0 bridge reserved | this change | witness only; nothing enabled |
| 1 guest shim | | needs `scripts/build-boxedwine-x64-vulkan.sh` |
| 2 shim reaches host | | |
| 3 Wine binds the loader | | `i386-windows` must carry `vulkan-1.dll` and `winevulkan.dll`; unverified |
| 4 instance and device | | |
| 5 DXVK selected | | needs the container change in `ios/` |
| 6 first frame | | |
