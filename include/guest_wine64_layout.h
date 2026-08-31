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
#define K_X64_WINE_UNIX_DIR K_X64_WINE_MODULE_ROOT "/x86_64-unix"
#define K_X64_WINE_LOADER K_X64_WINE_MODULE_ROOT "/wine64"
#define K_X64_WINE_SERVER K_X64_WINE_MODULE_ROOT "/wineserver"

// Kept reachable for anything that still names the relocated layout. Nothing
// in a launch resolves through these any more.
#define K_X64_WINE_COMPAT_ROOT "/usr/lib/wine"

// WINEDLLPATH names module ROOTS, not the PE directory: Wine appends
// "/x86_64-windows" to each entry itself. Pointing it straight at the PE
// directory would make it search for x86_64-windows/x86_64-windows.
#define K_X64_WINE_DLL_PATH_ASSIGNMENT "WINEDLLPATH=" K_X64_WINE_MODULE_ROOT

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
