/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#ifndef BOXEDVN_MANIFEST_H
#define BOXEDVN_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

#include "boxedvn/architecture.h"

namespace boxedvn {

// Bump when a change cannot be read by an older build.  Readers refuse a
// manifest whose schemaVersion is above kManifestSchemaVersion, and upgrade
// older ones in place.
inline constexpr int kManifestSchemaVersion = 1;

// One candidate executable discovered inside an imported game.
struct ManifestExecutable {
    std::string relativePath;   // relative to the game's content directory
    std::string formatName;     // ExecutableFormat, as a stable string
    std::string architecture;   // GuestArchitecture, as a stable string
    bool runnable = false;
    std::string diagnostic;
    uint16_t subsystem = 0;
};

// The persisted description of one imported game.
//
// Written next to the game's content so that a game directory is
// self-describing and can be copied out through Files.
struct GameManifest {
    int schemaVersion = kManifestSchemaVersion;

    // Stable identifier, used for the on-disk directory and prefix names.
    std::string id;

    std::string title;

    // Directory holding the imported game files, relative to the games root.
    std::string contentDirectory;

    // The executable the user chose, relative to contentDirectory.
    std::string selectedExecutable;

    // Working directory for the guest process, relative to contentDirectory.
    // Empty means the directory containing the selected executable.
    std::string workingDirectory;

    // Extra command line arguments passed to the guest executable.
    std::vector<std::string> arguments;

    // Extra guest environment variables, "NAME=VALUE".
    std::vector<std::string> environment;

    // Which backend this manifest was created for.  Recorded so a future x64
    // backend can tell which entries it owns without re-inspecting every file.
    std::string backend = toString(RuntimeBackendID::BoxedwineX86);

    // Name of the Wine prefix directory this game uses.
    std::string winePrefix;

    // "automatic", "wined3d", or "dxvk". Automatic reads the game's own
    // binaries (see boxedvn/direct3d_profile.h) and selects DXVK when
    // anything in it links Direct3D 10 or newer, which WineD3D cannot reach
    // on Metal; everything else stays on WineD3D. Stored per game so users
    // can override imperfect detection.
    std::string renderer = "automatic";

    // Every executable found during import, so the user can change their mind
    // without re-importing.
    std::vector<ManifestExecutable> discoveredExecutables;

    // Seconds since the Unix epoch.
    int64_t importedAtUnixSeconds = 0;

    // Guest screen size requested at launch.  Zero means "use the default".
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
};

// Serialises to JSON.  Deterministic key order, so manifests diff cleanly.
std::string serialiseManifest(const GameManifest& manifest);

struct ManifestParseResult {
    bool ok = false;
    std::string error;
    GameManifest manifest;
};

// Parses JSON produced by serialiseManifest.  Rejects, with a specific reason:
// malformed JSON, a missing schemaVersion, a schemaVersion newer than this
// build understands, or a missing required field.
ManifestParseResult parseManifest(const std::string& json);

}  // namespace boxedvn

#endif  // BOXEDVN_MANIFEST_H
