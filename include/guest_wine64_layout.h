/*
 * BoxedWine - where the packaged 64-bit Wine actually lives in the guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * Wine does not take its module search path from a compiled-in constant. It
 * derives it at startup from where its own files are:
 *
 *   - the Unix half, ntdll.so, is found relative to the loader binary, and the
 *     module root is that directory's parent -- so `.../wine/x86_64-unix`
 *     yields a module root of `.../wine`;
 *   - the loader's own directory, read from /proc/self/exe, is where it looks
 *     for `wineserver`;
 *   - builtin PE modules are then looked up as `<module root>/x86_64-windows/`,
 *     and WINEDLLPATH adds further module roots, each of which gets the same
 *     `x86_64-windows` suffix appended.
 *
 * The runtime had the module trees under Ubuntu's canonical root but the
 * loader relocated to /usr/lib/wine, so those derivations disagreed with each
 * other: the loader's directory named one place and the module trees were in
 * another. A device run reached Windows code and then failed with
 * "could not load kernel32.dll, status c0000135" with kernel32.dll present in
 * the archive the whole time.
 *
 * Everything real now lives under the canonical root below, so every path Wine
 * derives lands in the same tree. /usr/lib/wine stays reachable as guest
 * symlinks for anything that still names it.
 *
 * Wine also derives its data directory by walking two parents up from this
 * module root and appending `share/wine`. With Ubuntu's multiarch module root,
 * `.../wine/../../share/wine` normalizes to `/usr/lib/share/wine`, while the
 * distro packages the NLS tables under `/usr/share/wine`. The resource link below
 * joins those two paths. Without it, ntdll initializes its locale tables from
 * a null pointer before the first Windows process can start.
 *
 * A note on those symlinks: BoxedWine's ZIP filesystem does not read POSIX
 * symlinks out of an archive. It recognises a link by a `.link` suffix on the
 * entry name, whose contents are the target path (see EXT_LINK in
 * source/io/fsnode.h). A real symlink stored in the ZIP becomes a small
 * regular file holding the target text, which is why the compatibility entries
 * are written that way and checked for by name.
 */

#ifndef __GUEST_WINE64_LAYOUT_H__
#define __GUEST_WINE64_LAYOUT_H__

// Ubuntu's own module root, and the one every derived path agrees on.
#define K_X64_WINE_MODULE_ROOT "/usr/lib/x86_64-linux-gnu/wine"
#define K_X64_WINE_PE_DIR K_X64_WINE_MODULE_ROOT "/x86_64-windows"
// New WoW64 keeps the 32-bit PE builtins in a third tree beside the two above,
// under the same module root, and Wine appends the architecture directory to
// each root it searches. The 32-bit builtins are PE32 images executed in the
// CPU's compatibility mode; there is no i386-unix tree, which is the whole
// point of new WoW64 and the reason the distro's old-WoW64 i386-unix tree must
// never be packaged.
#define K_X64_WINE_PE32_DIR K_X64_WINE_MODULE_ROOT "/i386-windows"
#define K_X64_WINE_UNIX_DIR K_X64_WINE_MODULE_ROOT "/x86_64-unix"
// The prebuilt 32-bit DXVK, staged in the same 32-bit PE layer but in a
// directory of its own rather than over i386-windows/d3d9.dll. Wine's own
// d3d9 is wined3d, which needs OpenGL or Vulkan; DXVK's d3d9 needs Vulkan
// only, and the binary the app already ships is the same one the IA-32 lane
// uses for the same programs. Keeping it apart is what makes the override
// opt-in: a launch that does not ask for it never sees these files, so a
// broken Vulkan path cannot regress the lane's current behaviour.
#define K_X64_WINE_DXVK_PE32_DIR K_X64_WINE_MODULE_ROOT "/dxvk-i386"
// The one module the WoW64 Direct3D 9 lane needs, and the ones that ride
// along when the layer carries them. d3d9 is projected on its own by default
// because Direct3D 11 already has a route (DXMT) and mixing the two renderers
// in one prefix has never been tried.
#define K_X64_DXVK_PE32_MODULE_NAMES {"d3d9.dll"}
// The launch environment variable that turns the projection on, read by
// source/sdl/startupArgs.cpp and set by the iOS launcher for a 32-bit
// program. "dxvk" is the only value that enables it; anything else, including
// the variable being unset, keeps Wine's own d3d9.
#define K_X64_WOW64_D3D9_ENV "BOXEDVN_WOW64_D3D9"
#define K_X64_WOW64_D3D9_DXVK "dxvk"
// The override entry the projected DXVK d3d9 needs: native only, because the
// projected file is a real PE32 DXVK image and Wine has to load it rather than
// fall back to its own builtin. Wine separates WINEDLLOVERRIDES entries with
// ';' and the modules of one entry with ','.
#define K_X64_WOW64_D3D9_NATIVE_OVERRIDE "d3d9=n"
#define K_WINE_DLL_OVERRIDES_NAME "WINEDLLOVERRIDES"
#define K_WINE_DLL_OVERRIDES_SEPARATOR ";"
// The lane launches through the name upstream's WoW64 layout uses. Wine
// derives the loader for a 32-bit image by stripping "64" from its own name
// and hands the image to start.exe whenever that yields a name
// (dlls/ntdll/unix/env.c); a loader called wine64 therefore never runs a
// 32-bit program in-process. The packaged binaries keep their wine64 names
// and are aliased under the wine names at startup.
#define K_X64_WINE_LOADER K_X64_WINE_MODULE_ROOT "/wine"
#define K_X64_WINE_LOADER64 K_X64_WINE_MODULE_ROOT "/wine64"
#define K_X64_WINE_PRELOADER K_X64_WINE_MODULE_ROOT "/wine-preloader"
#define K_X64_WINE_PRELOADER64_NAME "wine64-preloader"
#define K_X64_WINE_LOADER64_NAME "wine64"
#define K_X64_WINE_SERVER K_X64_WINE_MODULE_ROOT "/wineserver"

// Kept reachable for anything that still names the relocated layout. Nothing
// in a launch resolves through these any more.
#define K_X64_WINE_COMPAT_ROOT "/usr/lib/wine"
#define K_X64_WINE_DERIVED_DATA_ROOT "/usr/lib/share/wine"
#define K_X64_WINE_DATA_ROOT "/usr/share/wine"

// WINEDLLPATH names module ROOTS, not the PE directory: Wine appends
// "/x86_64-windows" to each entry itself. Pointing it straight at the PE
// directory would make it search for x86_64-windows/x86_64-windows.
#define K_X64_WINE_DLL_PATH_ASSIGNMENT "WINEDLLPATH=" K_X64_WINE_MODULE_ROOT

// The x86-64 X11 client libraries BoxedWine ships in place of the distro
// ones. winex11.so links libX11.so.6 and libXext.so.6 by DT_NEEDED and the
// guest's own ld-linux resolves them, so WINEDLLPATH (which names Wine
// module roots) has no say. LD_LIBRARY_PATH is what ld-linux consults first,
// and this directory goes at its head for a 64-bit launch. The distro
// libraries stay where they are; they are simply found second.
#define K_X64_GUEST_X11_LIB_DIR "/usr/lib/boxedwine64-x11"
#define K_X64_GUEST_LIBRARY_PATH_ASSIGNMENT "LD_LIBRARY_PATH=" K_X64_GUEST_X11_LIB_DIR

// The glibc CPU tunables every x86-64 guest process is launched with, and why
// a launch has to set them at all.
//
// glibc picks its string and memory routines by IFUNC, once, while ld.so
// relocates the process, from whatever CPUID reported. The process the session
// launches runs on FEX, which advertises AVX/AVX2/FMA, so its libc resolves to
// the AVX2 variants. Every process forked from it cannot run on FEX -- a
// second identity mapping of the guest address space is impossible -- and
// falls back to the x86-64 interpreter, which implements SSE2 only. Wine
// double-forks before every exec, so the grandchild is already inside an AVX2
// libc routine before it ever reaches execve; a device run stopped one at
// "unimpl opcode ... c5 f9 6e c6", which is `vmovd xmm0, esi`, the prologue of
// an AVX2 string routine.
//
// GLIBC_TUNABLES is read by ld.so before the first IFUNC resolves and is an
// ordinary environment variable, so it is inherited by fork and carried across
// execve -- the one switch that reaches the children as well as the process
// that was launched. CPUID is deliberately left advertising AVX: a Windows
// program does not go through glibc's IFUNC table and the 64-bit lane needs
// the feature bits.
//
// XSAVE and XSAVEC are disabled for a second, independent reason. ld.so picks
// its lazy-binding trampoline from them -- _dl_runtime_resolve_xsavec when
// they are usable, _dl_runtime_resolve_fxsave otherwise -- and the interpreter
// implements FXSAVE/FXRSTOR but neither XSAVE, XSAVEC nor XRSTOR. Without
// this a helper would die the same way on the first lazily bound PLT entry,
// whatever its string routines were.
//
// The spellings are the ones the packaged glibc accepts. glibc-rootfs64.zip is
// assembled from an Ubuntu 24.04 runner (scripts/build-wine64-runtime-ci.sh
// records source_image=ubuntu-24.04-apt and copies the runner's own
// libraries), which is glibc 2.39, and 2.39's sysdeps/x86/cpu-tunables.c
// matches exactly AVX, AVX2, FMA, FMA4, XSAVE, XSAVEC and
// AVX_Fast_Unaligned_Load among these. F16C is deliberately absent: glibc has
// no tunable of that name -- it would be ignored rather than honoured -- and
// disabling AVX is what keeps the VEX-encoded routines out.
#define K_X64_GUEST_GLIBC_HWCAPS \
    "-AVX,-AVX2,-AVX_Fast_Unaligned_Load,-FMA,-FMA4,-XSAVE,-XSAVEC"
#define K_X64_GUEST_GLIBC_TUNABLES_NAME "GLIBC_TUNABLES"
#define K_X64_GUEST_GLIBC_HWCAPS_TUNABLE \
    "glibc.cpu.hwcaps=" K_X64_GUEST_GLIBC_HWCAPS

// Where a 64-bit guest Vulkan shim has to be staged, and why it is this
// directory. Wine's winevulkan.so dlopens the bare soname, so the guest's own
// ld-linux decides; the directory above is already at the head of
// LD_LIBRARY_PATH for a 64-bit launch, and a device run of a 32-bit Direct3D 9
// program shows the loader trying exactly this path first:
//
//   open('/usr/lib/boxedwine64-x11/libvulkan.so.1') -> -2
//   ... then /etc/ld.so.cache and six more paths, of which /lib/libvulkan.so.1
//   opens and is walked past, because the file in the root filesystem is the
//   IA-32 shim (an i386 ELF; it traps through `int 0x9A`, which CPU64 does not
//   decode). The 64-bit loader rejects it for its ELF class and keeps
//   searching, so wined3d gets no Vulkan adapter and no GL adapter and
//   Direct3DCreate9 fails.
//
// scripts/build-boxedwine-x64-vulkan.sh builds the file, the runtime builder
// stages it here and scripts/validate-wine64-runtime.sh requires it in
// wine64.zip as an ELF64 x86-64 object. The name is fixed here so the builder,
// the packaging and the startup witness cannot disagree about it -- and the
// witness (BOXEDWINE_X64_VULKAN_SHIM in source/vulkan/vulkancommon.cpp) reads
// this path out of the mounted guest filesystem and checks its ELF class,
// because the IA-32 shim in the root filesystem answers to the same name.
#define K_X64_GUEST_VULKAN_SONAME "libvulkan.so.1"
#define K_X64_GUEST_VULKAN_LIB_PATH \
    K_X64_GUEST_X11_LIB_DIR "/" K_X64_GUEST_VULKAN_SONAME

// What the 64-bit lane has for OpenGL, which is nothing, and why nothing is
// the settled answer rather than a gap waiting to be filled.
//
// docs/KNOWN_LIMITATIONS_IOS.md section 3 states the build-wide position: this
// build defines no GL backend, because upstream's software GL is a macOS
// dylib pair (OSMesa over LLVM) that does not exist for iOS and would need a
// JIT of its own. Two further facts make it specific to this lane. The host is
// Metal-only, so even a perfect guest client library would have nothing to
// bind to; and the 64-bit X11 client libraries in the directory above are a
// bridge to BoxedWine's built-in X server (tools/x11-64), which serves no GLX
// opcodes -- so a Mesa libGL staged here would load and then fail at
// glXQueryExtension instead of at dlopen.
//
// The consequence a device log shows is not a missing file. The soname
// resolves: the loader walks the same search order it walks for
// libvulkan.so.1 and reaches /lib/libGL.so.1, which is the IA-32 lane's own
// guest shim (tools/opengl, an i386 ELF trapping through `int 0x99`). A
// 64-bit process is refused it for its ELF class, and Wine reports
//
//   err:wgl:init_opengl Failed to load libGL: libGL.so.1: wrong ELF class:
//                       ELFCLASS32
//   err:wgl:init_opengl OpenGL support is disabled.
//
// which is byte for byte the same outcome as the file not being there at all:
// init_opengl disables OpenGL on any dlopen failure, whatever the reason. So
// removing the IA-32 file from this lane's view would change the sentence and
// nothing else, and that file is the IA-32 lane's working GL client library --
// it is found second, not shipped for us. It stays.
//
// What must never happen instead is this lane acquiring a GL library of its
// own that cannot work. The shim directory heads LD_LIBRARY_PATH, so anything
// named libGL.so.1 there is found FIRST, ahead of the IA-32 file, and a
// wrong-class or non-ELF file there would fail identically while looking like
// ours. scripts/build-wine64-runtime-ci.sh and
// scripts/validate-wine64-runtime.sh therefore allow the entry to be absent --
// that is the documented state -- and require an ELF64 x86-64 object if it is
// ever present.
//
// The startup witness (BOXEDWINE_X64_OPENGL in source/sdl/startupArgs.cpp)
// resolves the soname the way the guest loader does and names the file that
// answers and its ELF class, so a future log states this without Wine's wgl
// channel being enabled.
#define K_X64_GUEST_OPENGL_SONAME "libGL.so.1"
#define K_X64_GUEST_OPENGL_LIB_PATH \
    K_X64_GUEST_X11_LIB_DIR "/" K_X64_GUEST_OPENGL_SONAME

// The directories the guest's ld-linux tries for a bare soname, in the order a
// device run was observed trying them: LD_LIBRARY_PATH first (which for a
// 64-bit launch is the shim directory above, see
// K_X64_GUEST_LIBRARY_PATH_ASSIGNMENT), then the multiarch pair, then the
// unsuffixed pair. The glibc-hwcaps subdirectories the same run tried are
// omitted deliberately: nothing packages into them, and a witness that lists
// paths nobody fills says less, not more.
#define K_X64_GUEST_LIBRARY_SEARCH_DIR_COUNT 5
#define K_X64_GUEST_LIBRARY_SEARCH_DIRS \
    {K_X64_GUEST_X11_LIB_DIR, "/lib/x86_64-linux-gnu", \
     "/usr/lib/x86_64-linux-gnu", "/lib", "/usr/lib"}

// wined3d does not choose its backend from what is installed. It reads
// HKCU\Software\Wine\Direct3D value "renderer" once, and Wine 9's
// wined3d_get_renderer() maps the default -- WINED3D_RENDERER_AUTO, which is
// what an unset value means -- to WINED3D_RENDERER_OPENGL
// (dlls/wined3d/wined3d_main.c). adapter_vk.c is the only place that would
// load winevulkan.dll, and it is reached only when the value says "vulkan".
//
// So on a lane with no OpenGL an unset value is not "pick what works": it is
// "pick the one thing that cannot". A device run of a 32-bit Direct3D 11
// program proves the shape end to end -- Wine's own 32-bit d3d11 loaded
// wined3d, wined3d loaded opengl32, opengl32 reached for libGL.so.1 above, and
// wined3d_adapter_gl_init failed for want of a pixel format. winevulkan.dll
// appears nowhere in that run's module searches, so it was never tried.
//
// The value is written into the 64-bit prefix by the launch
// (configureX64WineD3dRenderer in source/sdl/startupArgs.cpp) rather than by
// the iOS prefix policy, which prepares only the IA-32 lane's prefix. It is
// written ONLY when the 64-bit Vulkan client library is packaged and is an
// ELF64 object, for the same reason the audio driver's registry value is
// written only when the driver is packaged: naming a backend that is not there
// leaves wined3d with no adapter rather than with a fallback.
//
// The doubled backslashes are the registry file's own escaping. user.reg
// spells the section "[Software\\Wine\\Direct3D]" on disk.
#define K_X64_WINED3D_REGISTRY_SECTION "Software\\\\Wine\\\\Direct3D"
#define K_X64_WINED3D_RENDERER_NAME "renderer"
#define K_X64_WINED3D_RENDERER_VULKAN "vulkan"

// Relay tracing, for the one question a device log cannot otherwise answer:
// which Windows call a program's startup check made before it stopped and
// painted a message box.
//
// Nothing else here can see a guest's API calls. The syscall trace sees file
// opens, the X bridge sees windows and their captions, and a check that reads
// the registry, asks for a named object or looks at an environment variable
// does none of those -- a device capture of exactly that shape carried no
// guest activity at all between the program's imports finishing and its error
// dialog appearing. Wine can see them: +relay writes one line per call into a
// traced module, with the arguments, the return value and the caller's own
// return address.
//
// Unrestricted it answers nothing, because it buries the answer: relay over a
// whole process writes tens of thousands of lines a second. Wine restricts it
// from the registry and from nowhere else -- HKCU\Software\Wine\Debug values
// RelayInclude and RelayExclude, each a ';'-separated list of "module.*" or
// "module.function" entries, read once when the first traced call is made and
// with no environment variable of their own. So a launch that asks for the
// trace writes them into the 64-bit prefix, the way the audio driver and the
// wined3d renderer are written (configureX64WineRelayFilter in
// source/sdl/startupArgs.cpp).
//
// The include list is the modules whose calls name a decision rather than
// bookkeeping: advapi32 (the registry and service queries a licence or
// configuration check makes), kernel32 and kernelbase (named objects, the
// environment, module handles, GetProcAddress, system information), user32
// (the message box itself -- its relay line carries the caller's return
// address, which is what names the module that raised the dialog), version
// (file version probing) and ws2_32 (what a client library does when it
// cannot reach the service it expects). Only RtlExitUserThread from ntdll is
// included. NtTerminateThread relay loops in the WoW64 device capture; the
// server-request witness already records its handle and status without relay.
// Wine 9 relay.c:check_list treats a bare module name as a FUNCTION name, so
// omitting .* silently suppresses every API call in that module.
//
// The exclude list removes what those modules are called for per loop
// iteration rather than per decision, and the message loop a modal box runs
// once it is up -- which would otherwise fill the log after the answer had
// already been written, and keep filling it for as long as the dialog stands.
//
// The values stay in the prefix once written. That costs nothing: relay
// happens only when WINEDEBUG names the channel as well, which only a launch
// with the setting on does, so a prefix carrying the filter behaves exactly
// as it did before for every other launch.
#define K_X64_WINE_TRACE_ENV "BOXEDVN_X64_WINE_TRACE"
#define K_X64_WINE_TRACE_RELAY "relay"
// The channels the iOS launch appends to WINEDEBUG when the setting is on,
// kept here so the two sides can be compared by a test rather than by memory.
// loaddll names each module as it is attached and debugstr carries whatever
// the program passes to OutputDebugString -- which for a wrapper layer is
// frequently the reason itself. Both are one line per event, not per call.
#define K_X64_WINE_TRACE_CHANNELS "+relay,+loaddll,+debugstr,+seh"
// The doubled backslashes are the registry file's own escaping: user.reg
// spells this section "[Software\\Wine\\Debug]" on disk.
#define K_X64_WINE_DEBUG_REGISTRY_SECTION "Software\\\\Wine\\\\Debug"
#define K_X64_WINE_RELAY_INCLUDE_NAME "RelayInclude"
#define K_X64_WINE_RELAY_EXCLUDE_NAME "RelayExclude"
#define K_X64_WINE_RELAY_INCLUDE \
    "advapi32.*;kernel32.*;kernelbase.*;user32.*;version.*;ws2_32.*;" \
    "ntdll.RtlExitUserThread"
#define K_X64_WINE_RELAY_EXCLUDE \
    "kernel32.GetLastError;kernel32.SetLastError;" \
    "kernelbase.GetLastError;kernelbase.SetLastError;" \
    "kernel32.GetTickCount;kernel32.GetTickCount64;" \
    "kernelbase.GetTickCount;kernelbase.GetTickCount64;" \
    "kernel32.QueryPerformanceCounter;kernelbase.QueryPerformanceCounter;" \
    "kernel32.GetSystemTimeAsFileTime;kernelbase.GetSystemTimeAsFileTime;" \
    "kernel32.GetCurrentThreadId;kernel32.GetCurrentProcessId;" \
    "kernel32.TlsGetValue;kernel32.TlsSetValue;" \
    "kernelbase.TlsGetValue;kernelbase.TlsSetValue;" \
    "kernel32.MultiByteToWideChar;kernel32.WideCharToMultiByte;" \
    "kernelbase.MultiByteToWideChar;kernelbase.WideCharToMultiByte;" \
    "user32.PeekMessageA;user32.PeekMessageW;" \
    "user32.GetMessageA;user32.GetMessageW;" \
    "user32.TranslateMessage;user32.DispatchMessageA;user32.DispatchMessageW;" \
    "user32.DefWindowProcA;user32.DefWindowProcW;" \
    "user32.MsgWaitForMultipleObjects;user32.MsgWaitForMultipleObjectsEx;" \
    "user32.GetQueueStatus"

// The DXMT modules are built as Wine builtins (their DOS stub carries Wine's
// builtin marker so winemetal.dll can bind to winemetal.so through
// __wine_unix_call). Wine treats a builtin-marked PE found outside its module
// tree as a stale installed copy and loads its own builtin of that name
// instead: a device run found d3d11.dll and dxgi.dll beside the executable,
// then resolved the import chain of Wine's wined3d-backed d3d11 and never
// looked for winemetal.dll. The modules therefore have to be presented at the
// module root, over Wine's own d3d11, dxgi and d3d10core, with winemetal
// beside them. These are the names the overlay projects.
#define K_X64_DXMT_MODULE_COUNT 4
#define K_X64_DXMT_MODULE_NAMES {"d3d11.dll", "dxgi.dll", "d3d10core.dll", "winemetal.dll"}

// Wine's own WoW64 thunking layer, which lives in the 64-bit PE tree even
// though it exists to serve 32-bit code: ntdll loads wow64.dll to build the
// 32-bit process, wow64win.dll thunks the user/GDI syscalls the 32-bit side
// makes into the 64-bit win32u, and wow64cpu.dll performs the mode transfer
// itself. A packaged 64-bit tree missing any of the three has no 32-bit lane
// at all, and Wine reports that only as a failure to start the image.
#define K_X64_WOW64_MODULE_COUNT 3
#define K_X64_WOW64_MODULE_NAMES {"wow64.dll", "wow64win.dll", "wow64cpu.dll"}

// The 32-bit builtins the lane's own import chain reaches before a Windows
// program runs a single instruction of its own code. Every name here was
// observed being resolved by a device run of a 32-bit Direct3D 9 probe: the
// loader entered 32-bit mode at ntdll's WoW64 entry, mapped kernel32,
// kernelbase, advapi32, sechost, msvcrt, ucrtbase, gdi32, user32, win32u,
// opengl32, wined3d and d3d9 out of the projected i386-windows tree, then
// searched the whole module path for zlib1.dll -- which 32-bit wined3d
// imports -- found nothing, and exited STATUS_DLL_NOT_FOUND (0xc0000135)
// before reaching the program's entry point.
//
// The 64-bit tree carries zlib1.dll; the packaged 32-bit tree did not. Wine
// reports that difference only as a process that never started, which on
// screen is indistinguishable from a program that ran and drew nothing. The
// names are therefore checked where the tree is built and reported again at
// launch, so the gap is named rather than inferred.
//
// libgcc_s_dw2-1.dll is the second name of the same kind, and it is here for
// the same reason. Ubuntu builds Wine's PE modules with mingw-w64, and the
// i386 ones reach for the shared i686 libgcc: a later device run resolved the
// whole chain above and then missed libgcc_s_dw2-1.dll in all four search
// directories, immediately after mapping opengl32.dll and again after
// uxtheme.dll. That miss is quieter than zlib1's -- the importing builtin
// fails to load and its caller carries on, so the program reaches its own
// code and only Direct3D is gone -- which makes it exactly the kind of gap
// that has to be caught where the tree is packaged. `dw2` is the i686 mingw
// unwinder flavour, so the name is specific to the 32-bit tree; the 64-bit
// one would want libgcc_s_seh-1.dll and does not import it.
#define K_X64_WOW64_LANE_PE32_MODULE_COUNT 15
#define K_X64_WOW64_LANE_PE32_MODULE_NAMES \
    {"ntdll.dll", "kernel32.dll", "kernelbase.dll", "advapi32.dll", \
     "sechost.dll", "msvcrt.dll", "ucrtbase.dll", "gdi32.dll", "user32.dll", \
     "win32u.dll", "opengl32.dll", "wined3d.dll", "d3d9.dll", "zlib1.dll", \
     "libgcc_s_dw2-1.dll"}

// The side-by-side assemblies, staged apart from the module trees.
//
// Wine ships no winsxs tree and wine.inf never mentions one. A real prefix
// gets its winsxs from wineboot's fake-DLL install: install_fake_dll calls
// register_fake_dll for every builtin it copies into the prefix, and that
// enumerates the module's RT_MANIFEST resources and writes
// windows\winsxs\manifests\<arch>_<name>_<key>_<version>_<lang>_deadbeef.manifest
// plus windows\winsxs\<same stem>\<file> for each one whose resource NAME
// begins with WINE_MANIFEST (dlls/setupapi/fakedll.c).
//
// On this lane that pass produces nothing: the prefix's system32 is the
// in-memory projection below rather than a directory wineboot filled in, and a
// device run recorded system32 left empty by a wineboot that exited 0. The
// consequence is not a missing file anyone reports. ntdll's lookup_winsxs is
// the FIRST thing lookup_assembly tries, so an empty winsxs sends the loader
// on to the private-assembly probes -- <appdir>\<name>.dll,
// <appdir>\<name>.manifest and the two <name>\<name> forms -- and when those
// miss, parse_depend_manifests fails the whole activation context with
// STATUS_SXS_CANT_GEN_ACTCTX. A program whose manifest requires
// Microsoft.Windows.Common-Controls 6.0 then runs with no activation context
// and gets the version 5 common controls, whose structures and behaviours are
// not the ones it was built against.
//
// scripts/stage-wine64-sxs-assemblies.py performs the same derivation at
// packaging time, from the same source of truth -- the packaged modules' own
// WINE_MANIFEST resources -- and stages it here; the launch projects it into
// the prefix the way it projects system32. Staged in a directory of its own
// rather than under the module root because Wine appends an architecture
// directory to every module root it searches, and the 32-bit half ships in the
// separate PE32 archive at the same guest path, so the two merge in the guest.
#define K_X64_GUEST_WINSXS_DIR "/usr/lib/boxedwine64-winsxs"
#define K_X64_GUEST_WINSXS_MANIFEST_SUBDIR "manifests"
#define K_X64_GUEST_WINSXS_MANIFEST_DIR \
    K_X64_GUEST_WINSXS_DIR "/" K_X64_GUEST_WINSXS_MANIFEST_SUBDIR
// The suffix Wine gives every manifest it writes, and the trailer it appends
// to the stem. lookup_manifest_file reads the trailer back to tell a
// Wine-provided assembly from one an installer put in the same directory.
#define K_X64_SXS_MANIFEST_SUFFIX ".manifest"
#define K_X64_SXS_ASSEMBLY_TRAILER "deadbeef"
// The assembly whose absence is the failure this staging exists to prevent,
// and the two architecture tokens Wine substitutes for an empty
// processorArchitecture (current_arch in fakedll.c).
#define K_X64_SXS_REQUIRED_ASSEMBLY "Microsoft.Windows.Common-Controls"
#define K_X64_SXS_ARCH_64 "amd64"
#define K_X64_SXS_ARCH_32 "x86"

// The builtin whose absence is the failure this layout exists to prevent. It
// is the first PE module Wine loads after the server handshake, so a guest
// that cannot see this file gets as far as Windows code and no further.
#define K_X64_WINE_BUILTIN_PROBE K_X64_WINE_PE_DIR "/kernel32.dll"

// A PE image starts with these two bytes. Enough to tell a real module from a
// truncated entry or from a link target written as a plain file.
#define K_PE_IMAGE_SIGNATURE_0 'M'
#define K_PE_IMAGE_SIGNATURE_1 'Z'

#if defined(__cplusplus)
#include <string>
#include <vector>

namespace boxedvn {

// True when this environment entry sets WINEDLLPATH at all, whatever value it
// carries. A caller that supplied one keeps it: the point is a working
// default, not a policy the caller cannot escape.
inline bool isWineDllPathAssignment(const std::string& entry) {
    return entry.rfind("WINEDLLPATH=", 0) == 0;
}

inline bool environmentSetsWineDllPath(
    const std::vector<std::string>& environment) {
    for (const std::string& entry : environment) {
        if (isWineDllPathAssignment(entry)) {
            return true;
        }
    }
    return false;
}

// The value BoxedVN supplies when the caller did not.
inline std::string wineDllPathAssignment() {
    return K_X64_WINE_DLL_PATH_ASSIGNMENT;
}

// LD_LIBRARY_PATH follows the same rule: a caller that set one keeps it.
inline bool isLibraryPathAssignment(const std::string& entry) {
    return entry.rfind("LD_LIBRARY_PATH=", 0) == 0;
}

inline bool environmentSetsLibraryPath(
    const std::vector<std::string>& environment) {
    for (const std::string& entry : environment) {
        if (isLibraryPathAssignment(entry)) {
            return true;
        }
    }
    return false;
}

// The library path a 64-bit launch gets when the caller supplied none: the
// BoxedWine X11 client libraries first, so winex11.so binds to them rather
// than to the distro libX11 that tries to open an X socket.
inline std::string guestLibraryPathAssignment() {
    return K_X64_GUEST_LIBRARY_PATH_ASSIGNMENT;
}

// The DXMT module names, in the order they are projected over the module
// root. Every name is a Wine builtin the DXMT copy must replace, except
// winemetal.dll, which Wine does not ship and which binds to the packaged
// winemetal.so once it is a module-root builtin.
inline std::vector<std::string> x64DxmtModuleNames() {
    return K_X64_DXMT_MODULE_NAMES;
}

// The 32-bit builtins a Windows program binds to before its own entry point
// runs. Returned as a list rather than searched for on demand so the launch
// can report the whole gap at once: one missing name ends the process with a
// status no window ever shows.
inline std::vector<std::string> x64Wow64LanePe32ModuleNames() {
    return K_X64_WOW64_LANE_PE32_MODULE_NAMES;
}

// The 32-bit DXVK modules a launch projects over syswow64 when it opts in.
// Kept as a list for the same reason as the one above: the projection reports
// each name it could not find, because a missing override shows on screen
// only as a program that still cannot create a device.
inline std::vector<std::string> x64DxvkPe32ModuleNames() {
    return K_X64_DXVK_PE32_MODULE_NAMES;
}

// Whether a launch asked for DXVK's 32-bit d3d9 in place of Wine's own.
// Exact-match on purpose: an unset variable, an empty one, or any other
// spelling keeps Wine's d3d9, so a Vulkan path that does not work cannot
// regress a lane that at least reaches its own error today.
inline bool x64Wow64D3d9UsesDxvk(const char* value) {
    return value != nullptr &&
           std::string(value) == std::string(K_X64_WOW64_D3D9_DXVK);
}

// Only a real file is projected over a module-root builtin; a directory or a
// missing source leaves Wine's own module in place so the launch still has a
// (non-DXMT) Direct3D rather than none.
inline bool shouldOverlayX64WineModule(bool sourceExists, bool sourceIsDirectory) {
    return sourceExists && !sourceIsDirectory;
}

// The name of the ELF class in a guest file's identification bytes, given
// however many bytes were actually read.
//
// Named rather than reduced to a boolean because "nothing there", "there and
// not an object at all", "there and the wrong architecture" and "there and
// usable" are four different states that Wine's loader collapses into one
// message -- and on this lane the wrong-architecture one is the common case,
// because the IA-32 lane's guest shims answer to the same sonames.
inline const char* guestElfClassName(const unsigned char* bytes,
                                     unsigned long length) {
    static const unsigned char magic[4] = {0x7f, 'E', 'L', 'F'};
    if (bytes == nullptr || length == 0) {
        return "unreadable";
    }
    // Whatever was read is compared against as much of the magic as it
    // covers, so a short read of something that is not an ELF at all is still
    // named "not-elf" rather than blamed on the read.
    for (unsigned long i = 0; i < length && i < 4; ++i) {
        if (bytes[i] != magic[i]) {
            return "not-elf";
        }
    }
    if (length < 5) {
        return "unreadable";
    }
    // EI_CLASS: 2 is ELFCLASS64, 1 is ELFCLASS32 -- the IA-32 lane's shim.
    if (bytes[4] == 2) {
        return "elf64";
    }
    if (bytes[4] == 1) {
        return "elf32";
    }
    return "elf-unknown";
}

// True when a class name from the function above is the only one a 64-bit
// guest process can bind to.
inline bool guestElfClassIsUsable(const char* className) {
    return className != nullptr && std::string(className) == "elf64";
}

// The paths the guest's ld-linux would try for a bare soname, in the order it
// tries them. Returned whole so a witness can report which one answered
// rather than only whether something did: the file that answers here is
// routinely the IA-32 lane's, and its path is what says so.
inline std::vector<std::string> guestLibrarySearchPaths(
    const std::string& soname) {
    const char* dirs[] = K_X64_GUEST_LIBRARY_SEARCH_DIRS;
    std::vector<std::string> paths;
    for (const char* dir : dirs) {
        paths.push_back(std::string(dir) + "/" + soname);
    }
    return paths;
}

// The wined3d adapter a given HKCU\Software\Wine\Direct3D "renderer" value
// selects, with the empty string standing for the value being absent.
//
// The absent case is the one worth spelling out: Wine 9 maps
// WINED3D_RENDERER_AUTO to the OpenGL adapter, so a prefix that says nothing
// has asked for the one backend this lane cannot provide.
inline const char* wined3dAdapterForRenderer(const std::string& renderer) {
    if (renderer.empty()) {
        return "opengl";
    }
    if (renderer == K_X64_WINED3D_RENDERER_VULKAN) {
        return "vulkan";
    }
    if (renderer == "gl" || renderer == "opengl") {
        return "opengl";
    }
    if (renderer == "gdi" || renderer == "no3d") {
        return "no3d";
    }
    return "unknown";
}

// Whether a 64-bit launch writes renderer=vulkan into its prefix.
//
// Gated on the lane's own Vulkan client library being present and usable, not
// on the value being absent: pointing wined3d at a backend whose client
// library is missing or is the IA-32 shim leaves it with no adapter at all,
// which is worse than the OpenGL adapter it would otherwise fail to build.
// This is the same rule the audio driver's registry value follows.
inline bool shouldConfigureX64WineD3dVulkan(bool vulkanClientIsUsable) {
    return vulkanClientIsUsable;
}

// True when the first bytes of a guest file are a PE image signature. Passed
// however many bytes were actually read, so a short read is a failure rather
// than an out-of-bounds test.
inline bool looksLikePeImage(const unsigned char* bytes, unsigned long length) {
    return bytes != nullptr && length >= 2 &&
           bytes[0] == (unsigned char)K_PE_IMAGE_SIGNATURE_0 &&
           bytes[1] == (unsigned char)K_PE_IMAGE_SIGNATURE_1;
}

// True when this environment entry sets GLIBC_TUNABLES at all, whatever it
// carries.
inline bool isGlibcTunablesAssignment(const std::string& entry) {
    return entry.rfind(K_X64_GUEST_GLIBC_TUNABLES_NAME "=", 0) == 0;
}

// True when a GLIBC_TUNABLES value already carries a glibc.cpu.hwcaps list.
// glibc separates tunables with ':' and spells each one name=value, so the
// list is set when the name starts the value or follows a separator -- a
// substring test alone would also match a longer name that merely ends in it.
inline bool glibcTunablesSetHwcaps(const std::string& value) {
    const std::string name = "glibc.cpu.hwcaps=";
    for (std::string::size_type at = value.find(name);
         at != std::string::npos; at = value.find(name, at + 1)) {
        if (at == 0 || value[at - 1] == ':') {
            return true;
        }
    }
    return false;
}

// The GLIBC_TUNABLES value an x86-64 launch runs with, given whatever the
// caller already supplied (the empty string when the caller supplied nothing).
//
// A caller's tunables are kept and appended to, not replaced: a caller that
// set one is tuning something else -- malloc arenas, say -- and dropping that
// silently would trade one defect for another. A caller that already named
// glibc.cpu.hwcaps keeps its list untouched: there is one hwcaps list per
// process and merging two of them behind the caller's back is a guess.
inline std::string guestGlibcTunablesValue(const std::string& existing) {
    if (existing.empty()) {
        return K_X64_GUEST_GLIBC_HWCAPS_TUNABLE;
    }
    if (glibcTunablesSetHwcaps(existing)) {
        return existing;
    }
    return existing + ":" + K_X64_GUEST_GLIBC_HWCAPS_TUNABLE;
}

inline std::string guestGlibcTunablesAssignment(const std::string& existing) {
    return std::string(K_X64_GUEST_GLIBC_TUNABLES_NAME "=") +
           guestGlibcTunablesValue(existing);
}

// True when a WINEDLLOVERRIDES value already names this module, whatever load
// order it gives it.
//
// Parsed rather than substring-matched: the value is a ';'-separated list of
// entries, each "dll[,dll...]=order", so "d3d9" has to be matched as a whole
// module name on the left of an '=' and not as a fragment of a longer one.
// Wine compares module names case-insensitively, so this does too.
inline bool wineDllOverridesNameModule(const std::string& value,
                                       const std::string& module) {
    std::string::size_type entryAt = 0;
    while (entryAt <= value.size()) {
        const std::string::size_type entryEnd = value.find(';', entryAt);
        const std::string entry = value.substr(
            entryAt, entryEnd == std::string::npos ? std::string::npos
                                                   : entryEnd - entryAt);
        const std::string names = entry.substr(0, entry.find('='));
        std::string::size_type nameAt = 0;
        while (nameAt <= names.size()) {
            const std::string::size_type nameEnd = names.find(',', nameAt);
            std::string name = names.substr(
                nameAt, nameEnd == std::string::npos ? std::string::npos
                                                     : nameEnd - nameAt);
            // Trim the spaces a hand-written value may carry around a name.
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
                name.erase(name.begin());
            }
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
                name.pop_back();
            }
            if (name.size() == module.size()) {
                bool same = true;
                for (std::string::size_type i = 0; i < name.size(); ++i) {
                    const char a = name[i] >= 'A' && name[i] <= 'Z'
                                       ? (char)(name[i] - 'A' + 'a') : name[i];
                    const char b = module[i] >= 'A' && module[i] <= 'Z'
                                       ? (char)(module[i] - 'A' + 'a') : module[i];
                    if (a != b) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    return true;
                }
            }
            if (nameEnd == std::string::npos) {
                break;
            }
            nameAt = nameEnd + 1;
        }
        if (entryEnd == std::string::npos) {
            break;
        }
        entryAt = entryEnd + 1;
    }
    return false;
}

// The WINEDLLOVERRIDES a launch that projects DXVK's 32-bit d3d9 runs with,
// given whatever the caller already set.
//
// There is one WINEDLLOVERRIDES per process, so the entry is merged into the
// caller's value rather than added beside it -- two assignments would leave
// the guest with two and which one wins is not something to guess at. A caller
// that already named d3d9 keeps its own load order: it said what it wanted.
inline std::string wineDllOverridesWithDxvkD3d9(const std::string& existing) {
    if (existing.empty()) {
        return K_X64_WOW64_D3D9_NATIVE_OVERRIDE;
    }
    if (wineDllOverridesNameModule(existing, "d3d9")) {
        return existing;
    }
    return existing + K_WINE_DLL_OVERRIDES_SEPARATOR +
           K_X64_WOW64_D3D9_NATIVE_OVERRIDE;
}

} // namespace boxedvn

#endif // __cplusplus

#endif
