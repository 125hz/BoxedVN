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
#define K_X64_WINE_LOADER K_X64_WINE_MODULE_ROOT "/wine64"
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

} // namespace boxedvn

#endif // __cplusplus

#endif
