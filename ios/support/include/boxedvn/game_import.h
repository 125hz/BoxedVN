/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#ifndef BOXEDVN_GAME_IMPORT_H
#define BOXEDVN_GAME_IMPORT_H

#include <string>
#include <vector>

#include "boxedvn/manifest.h"
#include "boxedvn/pe_inspector.h"

namespace boxedvn {

struct DiscoveredExecutable {
    std::string relativePath;  // '/'-separated, relative to the scan root
    ExecutableInfo info;
};

// Scans `contentDirectory` for files with an executable extension and inspects
// each one.  Results are ordered so the most plausible launch target comes
// first: runnable before non-runnable, then shallower paths, then by a
// heuristic that demotes obvious installers and helpers.
std::vector<DiscoveredExecutable> discoverExecutables(
    const std::string& contentDirectory);

// True for names that are almost never the game itself: uninstallers, crash
// handlers, redistributable bundles.  Exposed for testing.
bool looksLikeSupportExecutable(const std::string& relativePath);

// Builds a manifest for a freshly imported game.  `selectedExecutable` may be
// empty, in which case the first runnable discovery is used; if there is none,
// selectedExecutable is left empty and the caller must surface the diagnostics.
GameManifest buildManifest(const std::string& id,
                           const std::string& title,
                           const std::string& contentDirectory,
                           const std::vector<DiscoveredExecutable>& discovered,
                           const std::string& selectedExecutable,
                           int64_t importedAtUnixSeconds);

// Derives a filesystem-safe identifier from a user-visible title.  The result
// is lowercase ASCII, digits and '-', never empty, and never longer than 64
// characters.
std::string makeIdentifier(const std::string& title);

}  // namespace boxedvn

#endif  // BOXEDVN_GAME_IMPORT_H
