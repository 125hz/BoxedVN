/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 */

#ifndef BOXEDVN_ZIP_IMPORT_H
#define BOXEDVN_ZIP_IMPORT_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace boxedvn {

struct ZipEntrySummary {
    std::string rawName;         // exactly as stored in the archive
    std::string normalisedName;  // sanitised relative path, empty when rejected
    bool accepted = false;
    bool isDirectory = false;
    uint64_t uncompressedSize = 0;
    std::string rejectionReason;  // empty when accepted
};

struct ZipListing {
    bool ok = false;
    std::string error;
    std::vector<ZipEntrySummary> entries;

    // Non-empty when every entry lives under one shared top-level directory.
    std::string redundantTopLevel;

    uint64_t totalUncompressedBytes = 0;
    size_t rejectedEntryCount = 0;
};

// Reads the central directory without extracting anything.  Safe to run on
// untrusted input: every name is passed through sanitiseArchiveEntryName and
// nothing is written to disk.
ZipListing listZipArchive(const std::string& archivePath);

struct ZipExtractionOptions {
    // Drop a single shared top-level directory, so "Game.zip -> Game/data/..."
    // extracts as "data/...".  Only applied when listZipArchive found one.
    bool flattenRedundantTopLevel = true;

    // Refuse the whole archive if any entry is unsafe.  When false, unsafe
    // entries are skipped and reported, and the rest are extracted.
    bool failOnRejectedEntry = true;

    // Hard ceiling on the total uncompressed size, to bound zip-bomb damage.
    // Zero disables the check.
    uint64_t maxTotalUncompressedBytes = 16ull * 1024 * 1024 * 1024;

    // Invoked as extraction progresses.  May be null.
    std::function<void(uint64_t bytesWritten, uint64_t totalBytes,
                       const std::string& currentEntry)> progress;
};

struct ZipExtractionResult {
    bool ok = false;
    std::string error;
    size_t filesWritten = 0;
    size_t directoriesCreated = 0;
    size_t entriesSkipped = 0;
    uint64_t bytesWritten = 0;
    std::vector<std::string> skippedEntries;
};

// Extracts `archivePath` into `destinationDirectory`, which is created if
// missing.  Every entry path is sanitised before use; the extractor never
// follows a path outside the destination.
ZipExtractionResult extractZipArchive(const std::string& archivePath,
                                      const std::string& destinationDirectory,
                                      const ZipExtractionOptions& options);

// Recursively lists regular files under `directory`, returning paths relative
// to it, using '/' separators.  Used after extraction and for folder imports.
std::vector<std::string> listRegularFilesRecursively(const std::string& directory);

}  // namespace boxedvn

#endif  // BOXEDVN_ZIP_IMPORT_H
