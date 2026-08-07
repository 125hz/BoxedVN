/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#ifndef BOXEDVN_PATH_SAFETY_H
#define BOXEDVN_PATH_SAFETY_H

#include <string>
#include <vector>

namespace boxedvn {

enum class PathRejectionReason {
    None = 0,
    Empty,              // "" or a name that normalises away to nothing
    Absolute,           // "/etc/passwd" or "C:\..." or "\\server\share"
    Traversal,          // any ".." component that escapes the destination
    ReservedComponent,  // "." or "" components that cannot be represented
    NullByte,           // embedded NUL - truncation attack
    TooLong,            // component or whole path beyond the filesystem limit
    ControlCharacter,   // C0 control characters in a name
};

const char* toString(PathRejectionReason reason);

struct SanitisedPath {
    bool accepted = false;
    PathRejectionReason reason = PathRejectionReason::None;

    // Forward-slash separated, relative, with "." components removed and every
    // ".." resolved.  Only meaningful when accepted is true.
    std::string normalised;

    // True when the entry named a directory (trailing separator in the
    // archive entry).
    bool isDirectory = false;

    // Human readable explanation; always populated when accepted is false.
    std::string diagnostic;
};

// Normalises an archive entry name into a safe relative path.
//
// Rejects, rather than silently repairing:
//   * absolute POSIX paths ("/x"), DOS paths ("C:\x") and UNC paths ("\\\\s\\x")
//   * any path whose ".." components would resolve above the destination root
//   * embedded NUL bytes
//   * component or total lengths beyond kMaxComponentLength / kMaxPathLength
//
// Backslashes are treated as separators because ZIP archives produced on
// Windows frequently use them despite the specification requiring '/'.
SanitisedPath sanitiseArchiveEntryName(const std::string& rawName);

inline constexpr size_t kMaxComponentLength = 255;
inline constexpr size_t kMaxPathLength = 4096;

// Splits a normalised relative path on '/'.
std::vector<std::string> splitPath(const std::string& path);

// If every entry in `entries` sits underneath a single shared top-level
// directory, returns that directory's name; otherwise returns an empty string.
//
// This is what lets BoxedVN flatten the common "game.zip contains one folder
// named Game/" layout without guessing.  Entries must already be normalised.
std::string redundantTopLevelDirectory(const std::vector<std::string>& entries);

// Removes `prefix` and its separator from the front of `path`.  Returns an
// empty string when `path` *is* the prefix.
std::string stripTopLevelDirectory(const std::string& path,
                                   const std::string& prefix);

}  // namespace boxedvn

#endif  // BOXEDVN_PATH_SAFETY_H
