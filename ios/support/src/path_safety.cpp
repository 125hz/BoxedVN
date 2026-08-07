/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/path_safety.h"

#include <cctype>

namespace boxedvn {
namespace {

bool isSeparator(char c) {
    return c == '/' || c == '\\';
}

bool looksLikeDriveLetter(const std::string& s) {
    return s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) &&
           s[1] == ':';
}

SanitisedPath reject(PathRejectionReason reason, std::string diagnostic) {
    SanitisedPath result;
    result.accepted = false;
    result.reason = reason;
    result.diagnostic = std::move(diagnostic);
    return result;
}

}  // namespace

const char* toString(PathRejectionReason reason) {
    switch (reason) {
        case PathRejectionReason::None:              return "none";
        case PathRejectionReason::Empty:             return "empty";
        case PathRejectionReason::Absolute:          return "absolute";
        case PathRejectionReason::Traversal:         return "traversal";
        case PathRejectionReason::ReservedComponent: return "reserved-component";
        case PathRejectionReason::NullByte:          return "null-byte";
        case PathRejectionReason::TooLong:           return "too-long";
        case PathRejectionReason::ControlCharacter:  return "control-character";
    }
    return "none";
}

SanitisedPath sanitiseArchiveEntryName(const std::string& rawName) {
    if (rawName.empty()) {
        return reject(PathRejectionReason::Empty,
                      "The archive contains an entry with an empty name.");
    }

    if (rawName.find('\0') != std::string::npos) {
        return reject(PathRejectionReason::NullByte,
                      "The archive entry name contains a NUL byte, which is a "
                      "path-truncation attack.");
    }

    if (rawName.size() > kMaxPathLength) {
        return reject(PathRejectionReason::TooLong,
                      "The archive entry name is " +
                      std::to_string(rawName.size()) +
                      " bytes long, beyond the " +
                      std::to_string(kMaxPathLength) + " byte limit.");
    }

    for (unsigned char c : rawName) {
        if (c < 0x20 || c == 0x7F) {
            return reject(PathRejectionReason::ControlCharacter,
                          "The archive entry name contains a control character "
                          "(byte 0x" +
                          std::string(1, "0123456789ABCDEF"[(c >> 4) & 0xF]) +
                          std::string(1, "0123456789ABCDEF"[c & 0xF]) + ").");
        }
    }

    // Absolute forms must be refused outright rather than have their leading
    // separator stripped: an archive that names "/etc/passwd" is not something
    // to reinterpret, it is something to reject.
    if (isSeparator(rawName[0])) {
        return reject(PathRejectionReason::Absolute,
                      "The archive entry '" + rawName +
                      "' is an absolute path.");
    }
    if (looksLikeDriveLetter(rawName)) {
        return reject(PathRejectionReason::Absolute,
                      "The archive entry '" + rawName +
                      "' names a drive-absolute Windows path.");
    }

    const bool trailingSeparator = isSeparator(rawName.back());

    std::vector<std::string> stack;
    std::string component;
    component.reserve(64);

    auto flush = [&]() -> PathRejectionReason {
        if (component.empty() || component == ".") {
            component.clear();
            return PathRejectionReason::None;
        }
        if (component == "..") {
            if (stack.empty()) {
                return PathRejectionReason::Traversal;
            }
            stack.pop_back();
            component.clear();
            return PathRejectionReason::None;
        }
        if (component.size() > kMaxComponentLength) {
            return PathRejectionReason::TooLong;
        }
        stack.push_back(component);
        component.clear();
        return PathRejectionReason::None;
    };

    for (char c : rawName) {
        if (isSeparator(c)) {
            const PathRejectionReason reason = flush();
            if (reason != PathRejectionReason::None) {
                if (reason == PathRejectionReason::Traversal) {
                    return reject(reason,
                                  "The archive entry '" + rawName +
                                  "' uses '..' to escape the destination "
                                  "directory.");
                }
                return reject(reason,
                              "The archive entry '" + rawName +
                              "' contains a path component longer than " +
                              std::to_string(kMaxComponentLength) + " bytes.");
            }
            continue;
        }
        component.push_back(c);
    }

    const PathRejectionReason reason = flush();
    if (reason != PathRejectionReason::None) {
        if (reason == PathRejectionReason::Traversal) {
            return reject(reason,
                          "The archive entry '" + rawName +
                          "' uses '..' to escape the destination directory.");
        }
        return reject(reason,
                      "The archive entry '" + rawName +
                      "' contains a path component longer than " +
                      std::to_string(kMaxComponentLength) + " bytes.");
    }

    if (stack.empty()) {
        return reject(PathRejectionReason::Empty,
                      "The archive entry '" + rawName +
                      "' normalises to an empty path.");
    }

    SanitisedPath result;
    result.accepted = true;
    result.reason = PathRejectionReason::None;
    result.isDirectory = trailingSeparator;
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i != 0) {
            result.normalised.push_back('/');
        }
        result.normalised += stack[i];
    }
    if (result.normalised.size() > kMaxPathLength) {
        return reject(PathRejectionReason::TooLong,
                      "The normalised archive entry path exceeds " +
                      std::to_string(kMaxPathLength) + " bytes.");
    }
    result.diagnostic = "ok";
    return result;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string component;
    for (char c : path) {
        if (c == '/') {
            if (!component.empty()) {
                parts.push_back(component);
                component.clear();
            }
            continue;
        }
        component.push_back(c);
    }
    if (!component.empty()) {
        parts.push_back(component);
    }
    return parts;
}

std::string redundantTopLevelDirectory(const std::vector<std::string>& entries) {
    std::string candidate;
    bool sawNestedEntry = false;

    for (const std::string& entry : entries) {
        const std::vector<std::string> parts = splitPath(entry);
        if (parts.empty()) {
            continue;
        }
        if (candidate.empty()) {
            candidate = parts[0];
        } else if (candidate != parts[0]) {
            return std::string();
        }
        if (parts.size() > 1) {
            sawNestedEntry = true;
        }
    }

    // A single top-level *file* is not a redundant directory.
    if (!sawNestedEntry) {
        return std::string();
    }
    return candidate;
}

std::string stripTopLevelDirectory(const std::string& path,
                                   const std::string& prefix) {
    if (prefix.empty()) {
        return path;
    }
    if (path.size() == prefix.size() && path == prefix) {
        return std::string();
    }
    if (path.size() > prefix.size() &&
        path.compare(0, prefix.size(), prefix) == 0 &&
        path[prefix.size()] == '/') {
        return path.substr(prefix.size() + 1);
    }
    return path;
}

}  // namespace boxedvn
