/*
 * BoxedWine - the guest-visible Wine prefix a launch actually uses.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * The root filesystem ships one prefix, /home/username/.wine, and it is a
 * 32-bit Wine installation. Wine64 refuses to run in it -- "'...' is a 32-bit
 * installation, it cannot support 64-bit applications" -- so an x86-64 launch
 * needs a prefix of its own. Giving that launch a separate writable host root
 * is not enough on its own, because the guest path is what Wine looks at and
 * that path still resolved to the bundled 32-bit installation.
 *
 * So an x86-64 launch is given WINEPREFIX=/home/username/.wine64 and
 * WINEARCH=win64, and everything in the emulator that used to assume the one
 * fixed prefix -- the dosdevices drive links, the T: mount, the ddraw and
 * DXVK overlays -- resolves it from the guest environment instead.
 *
 * /home/username/.wine is untouched and remains the default for 32-bit
 * programs: with no WINEPREFIX in the environment the resolution below
 * returns exactly that, so existing IA-32 behaviour is unchanged.
 */

#ifndef __GUEST_WINE_PREFIX_H__
#define __GUEST_WINE_PREFIX_H__

// The prefix a 32-bit launch uses, and the fallback for anything unusable.
#define K_DEFAULT_GUEST_WINE_PREFIX "/home/username/.wine"

// The prefix and architecture an x86-64 launch defaults to. Separate from the
// 32-bit prefix on purpose: Wine records the architecture in the prefix, and
// the two cannot share one.
#define K_X64_GUEST_WINE_PREFIX "/home/username/.wine64"
#define K_X64_GUEST_WINE_ARCH "win64"

// The two directories every Wine prefix has, and the DOS drive link that makes
// C: reachable. wineboot exits 0 without creating the link in a prefix it did
// not initialise itself, and the guest then reopens the missing path forever:
// one device run logged 468,768 failed opens of dosdevices/c:.
#define K_GUEST_WINE_DRIVE_C "drive_c"
#define K_GUEST_WINE_DOSDEVICES "dosdevices"
#define K_GUEST_WINE_C_LINK "c:"
// Relative on purpose: it is what Wine writes, and it keeps the prefix
// relocatable. The guest filesystem resolves a relative link target against
// the link's own directory.
#define K_GUEST_WINE_C_LINK_TARGET "../drive_c"

#if defined(__cplusplus)
#include <cstddef>
#include <string>

namespace boxedvn {

// True when this WINEPREFIX value can be used as a guest prefix at all.
//
// Wine's own rule is that WINEPREFIX must be an absolute path, and everything
// downstream here concatenates onto it -- "<prefix>/dosdevices/d:" and so on
// -- so a relative or empty value would silently produce a path in whatever
// the current directory happens to be. A lone "/" is rejected for the same
// reason: it would put dosdevices at the filesystem root. A ".." component is
// rejected because the result would depend on symlink resolution rather than
// on the text, and a prefix that moves is worse than one that is refused.
inline bool isUsableGuestWinePrefix(const char* value) noexcept {
    if (value == nullptr || value[0] != '/') {
        return false;
    }
    std::size_t end = 0;
    while (value[end] != 0) {
        ++end;
    }
    // Trailing slashes are cosmetic; "/" alone is not a prefix.
    while (end > 1 && value[end - 1] == '/') {
        --end;
    }
    if (end < 2) {
        return false;
    }
    // Reject a ".." path component, wherever it appears.
    std::size_t start = 0;
    while (start < end) {
        std::size_t stop = start;
        while (stop < end && value[stop] != '/') {
            ++stop;
        }
        if (stop - start == 2 && value[start] == '.' && value[start + 1] == '.') {
            return false;
        }
        start = stop + 1;
    }
    return true;
}

// The prefix a launch should actually use: the environment's own value when
// it is usable, and the 32-bit default otherwise. Trailing slashes are
// removed so callers can append "/dosdevices" without producing "//".
inline std::string resolveGuestWinePrefix(const char* winePrefixValue) {
    if (!isUsableGuestWinePrefix(winePrefixValue)) {
        return std::string(K_DEFAULT_GUEST_WINE_PREFIX);
    }
    std::string resolved(winePrefixValue);
    while (resolved.size() > 1 && resolved.back() == '/') {
        resolved.pop_back();
    }
    return resolved;
}

// The value after "WINEPREFIX=" in one environment entry, or nullptr when the
// entry is something else. The last matching entry wins, which is what the
// guest would see, so callers scan the whole environment rather than stopping
// at the first hit.
inline const char* guestWinePrefixAssignment(const char* entry) noexcept {
    if (entry == nullptr) {
        return nullptr;
    }
    const char* name = "WINEPREFIX=";
    std::size_t index = 0;
    while (name[index] != 0) {
        if (entry[index] != name[index]) {
            return nullptr;
        }
        ++index;
    }
    return entry + index;
}

// What a prefix is still missing. Kept as a decision so it can be tested
// without a filesystem: the rule that matters is that nothing valid is ever
// replaced, and a prefix Wine64 has yet to initialise is completed rather
// than rebuilt.
struct GuestWinePrefixSetup {
    bool createDriveC = false;
    bool createDosDevices = false;
    bool createDriveCLink = false;

    bool anyWorkToDo() const {
        return createDriveC || createDosDevices || createDriveCLink;
    }
};

inline GuestWinePrefixSetup planGuestWinePrefixSetup(bool driveCExists,
                                                     bool dosDevicesExists,
                                                     bool driveCLinkExists) {
    GuestWinePrefixSetup setup;
    setup.createDriveC = !driveCExists;
    setup.createDosDevices = !dosDevicesExists;
    // An existing c: is left exactly as it is, wherever it points. It may be a
    // link the user or a previous wineboot made deliberately, and replacing it
    // would silently move the guest's C: drive.
    setup.createDriveCLink = !driveCLinkExists;
    return setup;
}

inline std::string guestWineDriveCPath(const std::string& prefix) {
    return prefix + "/" + K_GUEST_WINE_DRIVE_C;
}

inline std::string guestWineDosDevicesPath(const std::string& prefix) {
    return prefix + "/" + K_GUEST_WINE_DOSDEVICES;
}

inline std::string guestWineDriveCLinkPath(const std::string& prefix) {
    return guestWineDosDevicesPath(prefix) + "/" + K_GUEST_WINE_C_LINK;
}

} // namespace boxedvn
#endif

#endif
