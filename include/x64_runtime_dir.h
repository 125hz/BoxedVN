/*
 * BoxedWine - the modelled x86-64 guest user's private runtime directory.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * The 64-bit syscall layer reports exactly one user: UID and GID 1000, and its
 * stat implementation reports every filesystem object as owned by that user.
 * Wine's server places its socket directory in that user's XDG runtime
 * directory, which the standard defines as private -- 0700, owner only.
 *
 * The rootfs ships the directory, but FsZip::init builds plain FsFileNode
 * objects and drops the archive entry's Unix mode and ownership, so it arrived
 * read-only and wineserver died immediately on
 * `mkdir /run/user/1000/wine: Permission denied`. The permission policy in
 * FsFileNode::getMode therefore has to present it, which is why the path lives
 * here rather than being spelled out at each use.
 *
 * This is deliberately one exact subtree. /run and /run/user stay read-only to
 * the guest, and no other /run/user/<uid> is affected.
 */

#ifndef __X64_RUNTIME_DIR_H__
#define __X64_RUNTIME_DIR_H__

// The single user the 64-bit syscall layer models. Kept beside the path so a
// future change to either is made in one place.
#define K_X64_MODELLED_UID 1000
#define K_X64_MODELLED_GID 1000

// A string literal, so callers may concatenate it, e.g. K_X64_USER_RUNTIME_DIR "/".
#define K_X64_USER_RUNTIME_DIR "/run/user/1000"

// The mode the runtime directory presents: owner read, write and execute, and
// nothing for group or other.
#define K_X64_USER_RUNTIME_DIR_MODE 0700

#if defined(__cplusplus)
#include <cstddef>

namespace boxedvn {

// True only for the modelled user's runtime directory and the tree beneath it.
// Everything else under /run -- /run itself, /run/user, and any other
// /run/user/<uid> -- is deliberately excluded, so the write exception cannot
// widen by accident. The decision is pure string matching and the cost of
// getting it wrong is a writable /run, so it lives here and is unit tested.
inline bool isX64UserRuntimePath(const char* path) noexcept {
    if (path == nullptr) {
        return false;
    }
    const char* prefix = K_X64_USER_RUNTIME_DIR;
    std::size_t index = 0;
    while (prefix[index] != 0) {
        if (path[index] != prefix[index]) {
            return false;
        }
        ++index;
    }
    // The directory itself, or something genuinely inside it. A path that
    // merely shares the same leading characters -- /run/user/10001, say -- is
    // not inside it.
    return path[index] == 0 || path[index] == '/';
}

} // namespace boxedvn
#endif

#endif
