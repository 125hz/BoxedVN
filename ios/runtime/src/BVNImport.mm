/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  Thin, allocation-free C wrapper around ios/support.  All of the logic is
 *  in the support library, which is unit tested; this file only marshals.
 */

#include "BVNImport.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "BVNRuntime.h"
#include "boxedvn/architecture.h"
#include "boxedvn/game_import.h"
#include "boxedvn/manifest.h"
#include "boxedvn/pe_inspector.h"
#include "boxedvn/zip_import.h"

namespace {

void copyInto(char* destination, size_t capacity, const std::string& text) {
    if (destination == nullptr || capacity == 0) {
        return;
    }
    const size_t copied = text.size() < capacity - 1 ? text.size() : capacity - 1;
    memcpy(destination, text.data(), copied);
    destination[copied] = '\0';
}

BVNGuestArchitecture toC(boxedvn::GuestArchitecture architecture) {
    switch (architecture) {
        case boxedvn::GuestArchitecture::X86_16: return BVNGuestArchitectureX86_16;
        case boxedvn::GuestArchitecture::X86_32: return BVNGuestArchitectureX86_32;
        case boxedvn::GuestArchitecture::X86_64: return BVNGuestArchitectureX86_64;
        case boxedvn::GuestArchitecture::Unknown: break;
    }
    return BVNGuestArchitectureUnknown;
}

boxedvn::GuestArchitecture fromC(BVNGuestArchitecture architecture) {
    switch (architecture) {
        case BVNGuestArchitectureX86_16: return boxedvn::GuestArchitecture::X86_16;
        case BVNGuestArchitectureX86_32: return boxedvn::GuestArchitecture::X86_32;
        case BVNGuestArchitectureX86_64: return boxedvn::GuestArchitecture::X86_64;
        case BVNGuestArchitectureUnknown: break;
    }
    return boxedvn::GuestArchitecture::Unknown;
}

void fillExecutableInfo(const boxedvn::ExecutableInfo& info,
                        BVNExecutableInfo* out) {
    out->architecture = toC(info.architecture);
    copyInto(out->format, sizeof(out->format), boxedvn::toString(info.format));
    copyInto(out->backend, sizeof(out->backend), boxedvn::toString(info.backend));
    out->coffMachine = info.coffMachine;
    out->subsystem = info.subsystem;
    out->managedDotNet = info.managedDotNet;
    out->runnable = info.runnable;
    copyInto(out->diagnostic, sizeof(out->diagnostic), info.diagnostic);
}

bool readTextFile(const std::string& path, std::string& contents,
                  std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "Could not open '" + path + "'.";
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    contents = buffer.str();
    return true;
}

bool writeTextFile(const std::string& path, const std::string& contents,
                   std::string& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "Could not write '" + path + "'.";
        return false;
    }
    stream << contents;
    if (!stream) {
        error = "Could not write '" + path + "'. The device may be full.";
        return false;
    }
    return true;
}

}  // namespace

extern "C" void BVNInspectExecutable(const char* path, BVNExecutableInfo* out) {
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (path == nullptr) {
        copyInto(out->diagnostic, sizeof(out->diagnostic),
                 "No path was given.");
        return;
    }
    fillExecutableInfo(boxedvn::inspectExecutableFile(path), out);
}

extern "C" void BVNZipInspect(const char* archivePath, BVNZipListing* out) {
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (archivePath == nullptr) {
        copyInto(out->error, sizeof(out->error), "No archive path was given.");
        return;
    }

    const boxedvn::ZipListing listing = boxedvn::listZipArchive(archivePath);
    out->ok = listing.ok;
    out->totalUncompressedBytes = listing.totalUncompressedBytes;
    out->entryCount = listing.entries.size();
    out->rejectedEntryCount = listing.rejectedEntryCount;
    copyInto(out->redundantTopLevel, sizeof(out->redundantTopLevel),
             listing.redundantTopLevel);
    copyInto(out->error, sizeof(out->error), listing.error);

    for (const boxedvn::ZipEntrySummary& entry : listing.entries) {
        if (!entry.accepted) {
            copyInto(out->firstRejection, sizeof(out->firstRejection),
                     entry.rejectionReason);
            break;
        }
    }
}

extern "C" void BVNZipExtract(const char* archivePath,
                              const char* destinationDirectory,
                              bool flattenRedundantTopLevel,
                              BVNZipExtractionResult* out) {
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (archivePath == nullptr || destinationDirectory == nullptr) {
        copyInto(out->error, sizeof(out->error),
                 "The archive path and destination are both required.");
        return;
    }

    boxedvn::ZipExtractionOptions options;
    options.flattenRedundantTopLevel = flattenRedundantTopLevel;
    options.failOnRejectedEntry = true;

    BVNLogWrite(BVNLogLevelInfo, "import",
                (std::string("extracting ") + archivePath + " -> " +
                 destinationDirectory).c_str());

    const boxedvn::ZipExtractionResult result =
        boxedvn::extractZipArchive(archivePath, destinationDirectory, options);

    out->ok = result.ok;
    out->filesWritten = result.filesWritten;
    out->directoriesCreated = result.directoriesCreated;
    out->entriesSkipped = result.entriesSkipped;
    out->bytesWritten = result.bytesWritten;
    copyInto(out->error, sizeof(out->error), result.error);

    if (result.ok) {
        BVNLogWrite(BVNLogLevelInfo, "import",
                    (std::string("extracted ") +
                     std::to_string(result.filesWritten) + " file(s), " +
                     std::to_string(result.bytesWritten) + " bytes").c_str());
    } else {
        BVNLogWrite(BVNLogLevelError, "import", result.error.c_str());
    }
}

extern "C" size_t BVNDiscoverExecutables(const char* contentDirectory,
                                         BVNDiscoveredExecutable* out,
                                         size_t capacity) {
    if (contentDirectory == nullptr) {
        return 0;
    }
    const std::vector<boxedvn::DiscoveredExecutable> discovered =
        boxedvn::discoverExecutables(contentDirectory);

    const size_t written = discovered.size() < capacity ? discovered.size() : capacity;
    for (size_t i = 0; i < written && out != nullptr; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        copyInto(out[i].relativePath, sizeof(out[i].relativePath),
                 discovered[i].relativePath);
        fillExecutableInfo(discovered[i].info, &out[i].info);
    }
    return discovered.size();
}

extern "C" bool BVNLooksLikeInstalledGameExecutable(
    const char* relativePath) {
    return relativePath != nullptr &&
        boxedvn::looksLikeInstalledGameExecutable(relativePath);
}

extern "C" void BVNMakeIdentifier(const char* title, char* out, size_t capacity) {
    copyInto(out, capacity, boxedvn::makeIdentifier(title != nullptr ? title : ""));
}

extern "C" bool BVNManifestWriteForImport(
    const char* manifestPath, const char* identifier, const char* title,
    const char* contentDirectory, const char* selectedExecutable,
    const BVNDiscoveredExecutable* discovered, size_t discoveredCount,
    int64_t importedAtUnixSeconds, char* error, size_t errorCapacity) {
    if (manifestPath == nullptr || identifier == nullptr ||
        contentDirectory == nullptr) {
        copyInto(error, errorCapacity,
                 "The manifest path, identifier and content directory are all "
                 "required.");
        return false;
    }

    std::vector<boxedvn::DiscoveredExecutable> entries;
    entries.reserve(discoveredCount);
    for (size_t i = 0; i < discoveredCount && discovered != nullptr; ++i) {
        boxedvn::DiscoveredExecutable entry;
        entry.relativePath = discovered[i].relativePath;
        entry.info.architecture = fromC(discovered[i].info.architecture);
        entry.info.runnable = discovered[i].info.runnable;
        entry.info.subsystem = discovered[i].info.subsystem;
        entry.info.diagnostic = discovered[i].info.diagnostic;
        // format is carried as a string through the C boundary; buildManifest
        // only needs it for the record it writes.
        entries.push_back(std::move(entry));
    }

    boxedvn::GameManifest manifest = boxedvn::buildManifest(
        identifier, title != nullptr ? title : identifier, contentDirectory,
        entries, selectedExecutable != nullptr ? selectedExecutable : "",
        importedAtUnixSeconds);

    // Restore the exact format strings the C side reported, so a round trip
    // through the bridge does not lose them.
    for (size_t i = 0; i < manifest.discoveredExecutables.size() &&
                       i < discoveredCount && discovered != nullptr;
         ++i) {
        manifest.discoveredExecutables[i].formatName = discovered[i].info.format;
    }

    std::string writeError;
    if (!writeTextFile(manifestPath, boxedvn::serialiseManifest(manifest),
                       writeError)) {
        copyInto(error, errorCapacity, writeError);
        BVNLogWrite(BVNLogLevelError, "import", writeError.c_str());
        return false;
    }
    copyInto(error, errorCapacity, "");
    return true;
}

extern "C" void BVNManifestRead(const char* manifestPath,
                                BVNManifestSummary* out) {
    if (out == nullptr) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (manifestPath == nullptr) {
        copyInto(out->error, sizeof(out->error), "No manifest path was given.");
        return;
    }

    std::string text;
    std::string error;
    if (!readTextFile(manifestPath, text, error)) {
        copyInto(out->error, sizeof(out->error), error);
        return;
    }

    const boxedvn::ManifestParseResult parsed = boxedvn::parseManifest(text);
    if (!parsed.ok) {
        copyInto(out->error, sizeof(out->error), parsed.error);
        return;
    }

    const boxedvn::GameManifest& manifest = parsed.manifest;
    out->ok = true;
    out->schemaVersion = manifest.schemaVersion;
    copyInto(out->identifier, sizeof(out->identifier), manifest.id);
    copyInto(out->title, sizeof(out->title), manifest.title);
    copyInto(out->backend, sizeof(out->backend), manifest.backend);
    copyInto(out->contentDirectory, sizeof(out->contentDirectory),
             manifest.contentDirectory);
    copyInto(out->selectedExecutable, sizeof(out->selectedExecutable),
             manifest.selectedExecutable);
    copyInto(out->workingDirectory, sizeof(out->workingDirectory),
             manifest.workingDirectory);
    copyInto(out->winePrefix, sizeof(out->winePrefix), manifest.winePrefix);
    copyInto(out->renderer, sizeof(out->renderer), manifest.renderer);
    out->requestedWidth = manifest.requestedWidth;
    out->requestedHeight = manifest.requestedHeight;
    out->importedAtUnixSeconds = manifest.importedAtUnixSeconds;
}

extern "C" bool BVNManifestUpdateLaunchSettings(
    const char* manifestPath, const char* selectedExecutable,
    const char* workingDirectory, const char* renderer,
    const char* const* arguments, size_t argumentCount,
    const char* const* environment, size_t environmentCount,
    uint32_t requestedWidth, uint32_t requestedHeight,
    char* error, size_t errorCapacity) {
    if (manifestPath == nullptr) {
        copyInto(error, errorCapacity, "No manifest path was given.");
        return false;
    }

    std::string text;
    std::string readError;
    if (!readTextFile(manifestPath, text, readError)) {
        copyInto(error, errorCapacity, readError);
        return false;
    }

    boxedvn::ManifestParseResult parsed = boxedvn::parseManifest(text);
    if (!parsed.ok) {
        copyInto(error, errorCapacity, parsed.error);
        return false;
    }

    boxedvn::GameManifest& manifest = parsed.manifest;
    if (selectedExecutable != nullptr) {
        manifest.selectedExecutable = selectedExecutable;
    }
    if (workingDirectory != nullptr) {
        manifest.workingDirectory = workingDirectory;
    }
    if (renderer != nullptr) {
        const std::string requested(renderer);
        manifest.renderer = requested == "wined3d" || requested == "dxvk"
            ? requested : "automatic";
    }
    manifest.arguments.clear();
    for (size_t i = 0; i < argumentCount && arguments != nullptr; ++i) {
        if (arguments[i] != nullptr) {
            manifest.arguments.emplace_back(arguments[i]);
        }
    }
    manifest.environment.clear();
    for (size_t i = 0; i < environmentCount && environment != nullptr; ++i) {
        // An entry without '=' is not an assignment and Wine would ignore it.
        // Dropping it here keeps a typo out of the manifest instead of
        // silently carrying it into every future launch.
        if (environment[i] != nullptr && strchr(environment[i], '=') != nullptr) {
            manifest.environment.emplace_back(environment[i]);
        }
    }
    manifest.requestedWidth = requestedWidth;
    manifest.requestedHeight = requestedHeight;

    std::string writeError;
    if (!writeTextFile(manifestPath, boxedvn::serialiseManifest(manifest),
                       writeError)) {
        copyInto(error, errorCapacity, writeError);
        return false;
    }
    copyInto(error, errorCapacity, "");
    return true;
}

extern "C" size_t BVNManifestCopyArgumentsJoined(const char* manifestPath,
                                                 char* out, size_t capacity) {
    if (out != nullptr && capacity > 0) {
        out[0] = '\0';
    }
    if (manifestPath == nullptr) {
        return 0;
    }
    std::string text;
    std::string error;
    if (!readTextFile(manifestPath, text, error)) {
        return 0;
    }
    const boxedvn::ManifestParseResult parsed = boxedvn::parseManifest(text);
    if (!parsed.ok) {
        return 0;
    }

    std::string joined;
    for (const std::string& argument : parsed.manifest.arguments) {
        if (!joined.empty()) {
            joined.push_back('\n');
        }
        joined += argument;
    }
    copyInto(out, capacity, joined);
    return joined.size() < capacity ? joined.size() : (capacity > 0 ? capacity - 1 : 0);
}

extern "C" size_t BVNManifestCopyEnvironmentJoined(const char* manifestPath,
                                                   char* out,
                                                   size_t capacity) {
    if (out != nullptr && capacity > 0) {
        out[0] = '\0';
    }
    if (manifestPath == nullptr) {
        return 0;
    }
    std::string text;
    std::string error;
    if (!readTextFile(manifestPath, text, error)) {
        return 0;
    }
    const boxedvn::ManifestParseResult parsed = boxedvn::parseManifest(text);
    if (!parsed.ok) {
        return 0;
    }

    std::string joined;
    for (const std::string& entry : parsed.manifest.environment) {
        if (!joined.empty()) {
            joined.push_back('\n');
        }
        joined += entry;
    }
    copyInto(out, capacity, joined);
    return joined.size() < capacity ? joined.size() : (capacity > 0 ? capacity - 1 : 0);
}

extern "C" size_t BVNCopyBackendDescriptions(BVNBackendDescription* out,
                                             size_t capacity) {
    const std::vector<boxedvn::RuntimeBackendCapabilities> backends =
        boxedvn::allBackends();
    const size_t written = backends.size() < capacity ? backends.size() : capacity;
    for (size_t i = 0; i < written && out != nullptr; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        copyInto(out[i].identifier, sizeof(out[i].identifier),
                 boxedvn::toString(backends[i].id));
        out[i].implemented = backends[i].implemented;
        out[i].requiresJIT = backends[i].requiresJIT;
        out[i].hasInterpreterFallback = backends[i].hasInterpreterFallback;
        out[i].runsX86_16 = backends[i].canExecute(boxedvn::GuestArchitecture::X86_16);
        out[i].runsX86_32 = backends[i].canExecute(boxedvn::GuestArchitecture::X86_32);
        out[i].runsX86_64 = backends[i].canExecute(boxedvn::GuestArchitecture::X86_64);
    }
    return backends.size();
}

extern "C" void BVNUnsupportedArchitectureMessage(
    BVNGuestArchitecture architecture, char* out, size_t capacity) {
    copyInto(out, capacity,
             boxedvn::unsupportedArchitectureMessage(fromC(architecture)));
}
