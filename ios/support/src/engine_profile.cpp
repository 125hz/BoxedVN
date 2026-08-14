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
    // This exact list is the one device run 3 reached a rendering engine
    // with; it is shipped as a set because that is how it was proven.
    return {
        "--no-sandbox",
        "--disable-gpu",
        "--disable-software-rasterizer",
        "--disable-direct-composition",
        "--disable-features=CalculateNativeWinOcclusion",
        "--disable-background-timer-throttling",
        "--disable-renderer-backgrounding",
        "--disable-backgrounding-occluded-windows",
        // NW.js otherwise preserves the package's desktop-sized initial
        // window inside Boxedwine's larger virtual monitor. Maximise the
        // engine window generically so RPG Maker and other browser games use
        // the presentation area instead of being a small window in its
        // centre. A user-provided --start-maximized still wins in the merge.
        "--start-maximized",
        // Old NW.js packages can honour maximize while retaining their
        // decorated, package-sized client. Chromium's fullscreen startup path
        // removes that frame and uses the complete virtual monitor. Kiosk is
        // intentional here: BoxedVN runs one imported game per session and
        // owns the app-level exit/menu controls outside the guest.
        "--kiosk",
        "--start-fullscreen",
    };
}

const char kChromiumDefaultsOptOut[] = "--bvn-no-default-switches";
const char kBootDiagnosticsOptIn[] = "--bvn-boot-diagnostics";
const char kFontGateShimOptOut[] = "--bvn-no-font-fix";

namespace {

// "--disable-features=a,b" -> "--disable-features"; "--no-sandbox" -> itself.
std::string switchName(const std::string& argument) {
    const std::size_t equals = argument.find('=');
    return equals == std::string::npos ? argument : argument.substr(0, equals);
}

std::string switchValue(const std::string& argument) {
    const std::size_t equals = argument.find('=');
    return equals == std::string::npos ? std::string()
                                       : argument.substr(equals + 1);
}

// Appends the entries of `addition` that `value` does not already list.
// Comma separated, order preserving, so the user's own entries stay first.
std::string mergeCommaList(const std::string& value,
                           const std::string& addition) {
    std::string merged = value;
    std::size_t start = 0;
    while (start <= addition.size()) {
        const std::size_t comma = addition.find(',', start);
        const std::size_t end =
            comma == std::string::npos ? addition.size() : comma;
        const std::string entry = addition.substr(start, end - start);
        if (!entry.empty()) {
            bool present = false;
            std::size_t scan = 0;
            while (scan <= merged.size()) {
                const std::size_t next = merged.find(',', scan);
                const std::size_t stop =
                    next == std::string::npos ? merged.size() : next;
                if (merged.compare(scan, stop - scan, entry) == 0) {
                    present = true;
                    break;
                }
                if (next == std::string::npos) {
                    break;
                }
                scan = next + 1;
            }
            if (!present) {
                if (!merged.empty()) {
                    merged += ',';
                }
                merged += entry;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return merged;
}

}  // namespace

ChromiumSwitchMerge mergeChromiumSwitches(
    const std::vector<std::string>& userArguments) {
    ChromiumSwitchMerge result;
    bool declinedFontFix = false;

    for (const std::string& argument : userArguments) {
        if (argument == kChromiumDefaultsOptOut) {
            result.optedOut = true;
            continue;  // Stripped: it is BoxedVN's word, not Chromium's.
        }
        if (argument == kBootDiagnosticsOptIn) {
            result.bootDiagnostics = true;
            continue;  // Also BoxedVN's word.
        }
        if (argument == kFontGateShimOptOut) {
            declinedFontFix = true;
            continue;
        }
        result.arguments.push_back(argument);
    }

    result.fontGateShim = !declinedFontFix;

    // The probe reports through console.log, which reaches the session log
    // only when Chromium is logging to stderr. Asking for diagnostics and
    // silently getting none would be worse than not offering them.
    if (result.bootDiagnostics) {
        const bool alreadyLogging = std::any_of(
            result.arguments.begin(), result.arguments.end(),
            [](const std::string& argument) {
                return argument.rfind("--enable-logging", 0) == 0;
            });
        if (!alreadyLogging) {
            result.arguments.push_back("--enable-logging=stderr");
            result.added++;
        }
    }

    if (result.optedOut) {
        return result;
    }

    for (const std::string& option : chromiumCompatibilitySwitches()) {
        const std::string name = switchName(option);

        auto existing = std::find_if(
            result.arguments.begin(), result.arguments.end(),
            [&name](const std::string& argument) {
                return switchName(argument) == name;
            });

        if (existing == result.arguments.end()) {
            result.arguments.push_back(option);
            result.added++;
            continue;
        }

        // --disable-features is a list, so two of them are not a conflict -
        // and letting one replace the other is exactly the silent loss this
        // function exists to avoid, because Chromium keeps only the last.
        if (name == "--disable-features") {
            const std::string merged =
                mergeCommaList(switchValue(*existing), switchValue(option));
            if (merged != switchValue(*existing)) {
                *existing = name + "=" + merged;
                result.mergedFeatures = true;
            }
            continue;
        }
        result.deferredToUser++;
    }
    return result;
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
