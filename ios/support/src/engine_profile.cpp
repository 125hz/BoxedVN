/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/engine_profile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace boxedvn {
namespace {

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

// RPG Maker's own files. MV names its runtime rpg_*.js and MZ names it
// rmmz_*.js; both keep it beside the game's index.html, normally under www/
// for MV and at the top level for MZ. Only the core file is matched: a game
// that renames or concatenates its scripts still runs, it just reports an
// unknown framework.
GuestFramework frameworkForFileName(const std::string& lowerName) {
    if (lowerName == "rpg_core.js") {
        return GuestFramework::RpgMakerMv;
    }
    if (lowerName == "rmmz_core.js") {
        return GuestFramework::RpgMakerMz;
    }
    return GuestFramework::Unknown;
}

}  // namespace

const char* toString(GuestEngine engine) {
    switch (engine) {
        case GuestEngine::NwJs:     return "NW.js";
        case GuestEngine::Electron: return "Electron";
        case GuestEngine::Unknown:  break;
    }
    return "unknown";
}

const char* toString(GuestFramework framework) {
    switch (framework) {
        case GuestFramework::RpgMakerMv: return "RPG Maker MV";
        case GuestFramework::RpgMakerMz: return "RPG Maker MZ";
        case GuestFramework::Unknown:    break;
    }
    return "unknown";
}

GuestEngine engineForFileName(const std::string& fileName) {
    const std::string name = toLower(fileName);

    // NW.js. nw.dll is the engine itself; nw.pak is its resource bundle;
    // package.nw is the packed application. nw_elf.dll appears in the split
    // builds NW.js has shipped since 0.24, where nw.dll may be absent.
    if (name == "nw.dll" || name == "nw.pak" || name == "package.nw" ||
        name == "nw_elf.dll" || name == "nw_100_percent.pak") {
        return GuestEngine::NwJs;
    }

    // Electron. The asar archives are the reliable marker: the executable is
    // renamed to the game's title in every shipped app, and electron.asar is
    // present even when the application itself is unpacked.
    if (name == "electron.asar" || name == "app.asar") {
        return GuestEngine::Electron;
    }

    return GuestEngine::Unknown;
}

GuestEngineProfile detectGuestEngine(const std::string& gameDirectory,
                                     std::size_t maxFiles,
                                     std::size_t maxDepth) {
    GuestEngineProfile profile;
    if (gameDirectory.empty()) {
        return profile;
    }

    std::error_code ec;
    const fs::path root(gameDirectory);
    if (!fs::is_directory(root, ec)) {
        return profile;
    }

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return profile;
    }

    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (static_cast<std::size_t>(it.depth()) >= maxDepth) {
            it.disable_recursion_pending();
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        if (profile.filesInspected >= maxFiles) {
            profile.reachedLimit = true;
            break;
        }
        profile.filesInspected++;

        const std::string fileName = it->path().filename().string();
        const std::string lowerName = toLower(fileName);

        if (profile.engine == GuestEngine::Unknown) {
            const GuestEngine found = engineForFileName(lowerName);
            if (found != GuestEngine::Unknown) {
                profile.engine = found;
                profile.evidence = fileName + " ships with the game";
            }
        }
        if (profile.framework == GuestFramework::Unknown) {
            profile.framework = frameworkForFileName(lowerName);
        }

        // Both questions answered; nothing further can change the outcome.
        if (profile.engine != GuestEngine::Unknown &&
            profile.framework != GuestFramework::Unknown) {
            break;
        }
    }
    return profile;
}

std::vector<std::string> chromiumCompatibilitySwitches() {
    // See the header for why each of these is here and what falsifies it.
    return {
        "--no-sandbox",
        "--in-process-gpu",
        "--disable-direct-composition",
        "--disable-features=CalculateNativeWinOcclusion",
    };
}

bool argumentsCarryChromiumSwitch(const std::vector<std::string>& arguments) {
    for (const std::string& argument : arguments) {
        if (argument.rfind("--", 0) == 0 && argument.size() > 2) {
            return true;
        }
    }
    return false;
}

}  // namespace boxedvn
