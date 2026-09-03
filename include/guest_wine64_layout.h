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
