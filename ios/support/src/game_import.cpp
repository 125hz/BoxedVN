/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#include "boxedvn/game_import.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "boxedvn/path_safety.h"
#include "boxedvn/zip_import.h"

namespace fs = std::filesystem;

namespace boxedvn {
namespace {

std::string toLower(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool hasExecutableExtension(const std::string& lowerPath) {
    static const char* kExtensions[] = {".exe", ".com", ".bat", ".pif"};
    for (const char* extension : kExtensions) {
        const size_t length = std::char_traits<char>::length(extension);
        if (lowerPath.size() > length &&
            lowerPath.compare(lowerPath.size() - length, length, extension) == 0) {
            return true;
        }
    }
    return false;
}

size_t depthOf(const std::string& path) {
    return splitPath(path).size();
}

}  // namespace

bool looksLikeSupportExecutable(const std::string& relativePath) {
    const std::string lower = toLower(relativePath);
    static const char* kMarkers[] = {
        "unins",      "uninstall", "setup",     "install",
        "vcredist",   "dotnetfx",  "directx",   "dxsetup",
        "crashrep",   "crashsend", "reporter",  "updater",
        "redist",     "config.exe", "settei",   "readme",
    };
    for (const char* marker : kMarkers) {
        if (lower.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<DiscoveredExecutable> discoverExecutables(
    const std::string& contentDirectory) {
    std::vector<DiscoveredExecutable> discovered;

    const std::vector<std::string> files =
        listRegularFilesRecursively(contentDirectory);

    for (const std::string& relative : files) {
        const std::string lower = toLower(relative);
        if (!hasExecutableExtension(lower)) {
            continue;
        }
        DiscoveredExecutable entry;
        entry.relativePath = relative;
        entry.info = inspectExecutableFile(
            (fs::path(contentDirectory) / fs::path(relative)).string());
        discovered.push_back(std::move(entry));
    }

    std::stable_sort(discovered.begin(), discovered.end(),
                     [](const DiscoveredExecutable& a,
                        const DiscoveredExecutable& b) {
                         if (a.info.runnable != b.info.runnable) {
                             return a.info.runnable;
                         }
                         const bool aSupport = looksLikeSupportExecutable(a.relativePath);
                         const bool bSupport = looksLikeSupportExecutable(b.relativePath);
                         if (aSupport != bSupport) {
                             return !aSupport;
                         }
                         const size_t aDepth = depthOf(a.relativePath);
                         const size_t bDepth = depthOf(b.relativePath);
                         if (aDepth != bDepth) {
                             return aDepth < bDepth;
                         }
                         return a.relativePath < b.relativePath;
                     });

    return discovered;
}

std::string makeIdentifier(const std::string& title) {
    std::string out;
    bool lastWasDash = false;
    for (unsigned char c : title) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
            lastWasDash = false;
        } else if (!lastWasDash && !out.empty()) {
            out.push_back('-');
            lastWasDash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "game";
    }
    if (out.size() > 64) {
        out.resize(64);
        while (!out.empty() && out.back() == '-') {
            out.pop_back();
        }
    }
    return out;
}

GameManifest buildManifest(const std::string& id,
                           const std::string& title,
                           const std::string& contentDirectory,
                           const std::vector<DiscoveredExecutable>& discovered,
                           const std::string& selectedExecutable,
                           int64_t importedAtUnixSeconds) {
    GameManifest manifest;
    manifest.schemaVersion = kManifestSchemaVersion;
    manifest.id = id;
    manifest.title = title;
    manifest.contentDirectory = contentDirectory;
    manifest.backend = toString(RuntimeBackendID::BoxedwineX86);
    manifest.winePrefix = id;
    manifest.importedAtUnixSeconds = importedAtUnixSeconds;

    for (const DiscoveredExecutable& entry : discovered) {
        ManifestExecutable executable;
        executable.relativePath = entry.relativePath;
        executable.formatName = toString(entry.info.format);
        executable.architecture = toString(entry.info.architecture);
        executable.runnable = entry.info.runnable;
        executable.subsystem = entry.info.subsystem;
        executable.diagnostic = entry.info.diagnostic;
        manifest.discoveredExecutables.push_back(std::move(executable));
    }

    if (!selectedExecutable.empty()) {
        manifest.selectedExecutable = selectedExecutable;
    } else {
        for (const DiscoveredExecutable& entry : discovered) {
            if (entry.info.runnable) {
                manifest.selectedExecutable = entry.relativePath;
                break;
            }
        }
    }

    if (!manifest.selectedExecutable.empty()) {
        const std::vector<std::string> parts =
            splitPath(manifest.selectedExecutable);
        if (parts.size() > 1) {
            std::string directory;
            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                if (i != 0) {
                    directory.push_back('/');
                }
                directory += parts[i];
            }
            manifest.workingDirectory = directory;
        }
    }

    return manifest;
}

}  // namespace boxedvn
