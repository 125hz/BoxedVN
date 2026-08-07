/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  ZIP import built on minizip, which already ships in lib/zlib/contrib and is
 *  the same reader Boxedwine uses for its ZIP-mounted root filesystem.
 */

#include "boxedvn/zip_import.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "boxedvn/path_safety.h"

extern "C" {
#include "unzip.h"
}

namespace fs = std::filesystem;

namespace boxedvn {
namespace {

constexpr size_t kCopyBufferSize = 64 * 1024;

// minizip's maximum filename length for a single entry read.
constexpr size_t kMaxEntryNameLength = 8192;

std::string minizipError(int code) {
    switch (code) {
        case UNZ_OK:             return "ok";
        case UNZ_END_OF_LIST_OF_FILE: return "end of central directory";
        case UNZ_ERRNO:          return std::strerror(errno);
        case UNZ_PARAMERROR:     return "invalid parameter";
        case UNZ_BADZIPFILE:     return "the file is not a valid ZIP archive";
        case UNZ_INTERNALERROR:  return "internal minizip error";
        case UNZ_CRCERROR:       return "CRC mismatch (the archive is corrupt)";
        default:                 return "minizip error " + std::to_string(code);
    }
}

struct UnzFileCloser {
    unzFile handle = nullptr;
    ~UnzFileCloser() {
        if (handle != nullptr) {
            unzClose(handle);
        }
    }
};

}  // namespace

ZipListing listZipArchive(const std::string& archivePath) {
    ZipListing listing;

    UnzFileCloser zip;
    zip.handle = unzOpen64(archivePath.c_str());
    if (zip.handle == nullptr) {
        listing.error = "Could not open '" + archivePath +
                        "' as a ZIP archive. It may be corrupt, truncated, or "
                        "not a ZIP file at all.";
        return listing;
    }

    int status = unzGoToFirstFile(zip.handle);
    if (status == UNZ_END_OF_LIST_OF_FILE) {
        listing.ok = true;
        listing.error = "The archive contains no entries.";
        return listing;
    }
    if (status != UNZ_OK) {
        listing.error = "Could not read the ZIP central directory: " +
                        minizipError(status);
        return listing;
    }

    std::vector<std::string> acceptedNames;

    while (status == UNZ_OK) {
        unz_file_info64 info;
        std::memset(&info, 0, sizeof(info));
        std::vector<char> nameBuffer(kMaxEntryNameLength, '\0');

        status = unzGetCurrentFileInfo64(zip.handle, &info, nameBuffer.data(),
                                         static_cast<uLong>(nameBuffer.size() - 1),
                                         nullptr, 0, nullptr, 0);
        if (status != UNZ_OK) {
            listing.error = "Could not read a ZIP entry header: " +
                            minizipError(status);
            return listing;
        }

        ZipEntrySummary summary;
        summary.rawName.assign(nameBuffer.data());
        summary.uncompressedSize = info.uncompressed_size;

        const SanitisedPath sanitised = sanitiseArchiveEntryName(summary.rawName);
        if (sanitised.accepted) {
            summary.accepted = true;
            summary.normalisedName = sanitised.normalised;
            // A trailing separator in the stored name is the only reliable
            // marker of a directory entry; a zero-length entry without one is
            // an empty file and must stay a file.
            summary.isDirectory = sanitised.isDirectory;
            if (!summary.isDirectory) {
                acceptedNames.push_back(sanitised.normalised);
                listing.totalUncompressedBytes += info.uncompressed_size;
            }
        } else {
            summary.accepted = false;
            summary.rejectionReason = sanitised.diagnostic;
            listing.rejectedEntryCount++;
        }

        listing.entries.push_back(std::move(summary));
        status = unzGoToNextFile(zip.handle);
    }

    if (status != UNZ_END_OF_LIST_OF_FILE) {
        listing.error = "Could not walk the ZIP central directory: " +
                        minizipError(status);
        return listing;
    }

    listing.redundantTopLevel = redundantTopLevelDirectory(acceptedNames);
    listing.ok = true;
    return listing;
}

ZipExtractionResult extractZipArchive(const std::string& archivePath,
                                      const std::string& destinationDirectory,
                                      const ZipExtractionOptions& options) {
    ZipExtractionResult result;

    const ZipListing listing = listZipArchive(archivePath);
    if (!listing.ok) {
        result.error = listing.error;
        return result;
    }
    if (listing.entries.empty()) {
        result.error = "The archive contains no entries.";
        return result;
    }
    if (options.failOnRejectedEntry && listing.rejectedEntryCount > 0) {
        for (const ZipEntrySummary& entry : listing.entries) {
            if (!entry.accepted) {
                result.error = "Refusing to extract '" + archivePath +
                               "': " + entry.rejectionReason;
                return result;
            }
        }
    }
    if (options.maxTotalUncompressedBytes != 0 &&
        listing.totalUncompressedBytes > options.maxTotalUncompressedBytes) {
        result.error =
            "Refusing to extract '" + archivePath + "': it expands to " +
            std::to_string(listing.totalUncompressedBytes) +
            " bytes, above the " +
            std::to_string(options.maxTotalUncompressedBytes) + " byte limit.";
        return result;
    }

    const std::string topLevel =
        options.flattenRedundantTopLevel ? listing.redundantTopLevel : std::string();

    std::error_code ec;
    const fs::path destination = fs::path(destinationDirectory);
    fs::create_directories(destination, ec);
    if (ec) {
        result.error = "Could not create '" + destinationDirectory +
                       "': " + ec.message();
        return result;
    }
    const fs::path canonicalDestination = fs::weakly_canonical(destination, ec);
    if (ec) {
        result.error = "Could not resolve '" + destinationDirectory +
                       "': " + ec.message();
        return result;
    }

    UnzFileCloser zip;
    zip.handle = unzOpen64(archivePath.c_str());
    if (zip.handle == nullptr) {
        result.error = "Could not reopen '" + archivePath + "' for extraction.";
        return result;
    }

    std::vector<char> buffer(kCopyBufferSize);

    for (const ZipEntrySummary& entry : listing.entries) {
        if (!entry.accepted) {
            result.entriesSkipped++;
            result.skippedEntries.push_back(entry.rawName + ": " +
                                            entry.rejectionReason);
            continue;
        }

        const std::string relative =
            topLevel.empty() ? entry.normalisedName
                             : stripTopLevelDirectory(entry.normalisedName, topLevel);
        if (relative.empty()) {
            // This was the redundant top-level directory itself.
            continue;
        }

        const fs::path target = destination / fs::path(relative);

        // Belt and braces: even though the name was sanitised, verify the
        // resolved path still lies under the destination before writing.
        const fs::path resolved = fs::weakly_canonical(target, ec);
        ec.clear();
        const std::string resolvedStr = resolved.string();
        const std::string rootStr = canonicalDestination.string();
        if (resolvedStr.compare(0, rootStr.size(), rootStr) != 0) {
            result.error = "Refusing to write '" + entry.rawName +
                           "': the resolved path escapes the destination "
                           "directory.";
            return result;
        }

        if (entry.isDirectory) {
            fs::create_directories(target, ec);
            if (ec) {
                result.error = "Could not create directory '" + target.string() +
                               "': " + ec.message();
                return result;
            }
            result.directoriesCreated++;
            continue;
        }

        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            result.error = "Could not create directory '" +
                           target.parent_path().string() + "': " + ec.message();
            return result;
        }

        if (unzLocateFile(zip.handle, entry.rawName.c_str(), 0) != UNZ_OK) {
            result.error = "Could not locate '" + entry.rawName +
                           "' in the archive during extraction.";
            return result;
        }
        int status = unzOpenCurrentFile(zip.handle);
        if (status != UNZ_OK) {
            result.error = "Could not open '" + entry.rawName +
                           "' in the archive: " + minizipError(status);
            return result;
        }

        std::FILE* out = std::fopen(target.string().c_str(), "wb");
        if (out == nullptr) {
            unzCloseCurrentFile(zip.handle);
            result.error = "Could not create '" + target.string() + "': " +
                           std::strerror(errno);
            return result;
        }

        bool writeFailed = false;
        int read = 0;
        while ((read = unzReadCurrentFile(zip.handle, buffer.data(),
                                          static_cast<unsigned>(buffer.size()))) > 0) {
            if (std::fwrite(buffer.data(), 1, static_cast<size_t>(read), out) !=
                static_cast<size_t>(read)) {
                writeFailed = true;
                break;
            }
            result.bytesWritten += static_cast<uint64_t>(read);
            if (options.progress) {
                options.progress(result.bytesWritten,
                                 listing.totalUncompressedBytes,
                                 entry.normalisedName);
            }
        }
        std::fclose(out);

        if (writeFailed) {
            unzCloseCurrentFile(zip.handle);
            result.error = "Could not write '" + target.string() +
                           "'. The device may be out of space.";
            return result;
        }
        if (read < 0) {
            unzCloseCurrentFile(zip.handle);
            result.error = "Could not decompress '" + entry.rawName + "': " +
                           minizipError(read);
            return result;
        }

        // unzCloseCurrentFile verifies the entry's CRC.
        status = unzCloseCurrentFile(zip.handle);
        if (status != UNZ_OK) {
            result.error = "'" + entry.rawName + "' failed its integrity check: " +
                           minizipError(status);
            return result;
        }

        result.filesWritten++;
    }

    result.ok = true;
    return result;
}

std::vector<std::string> listRegularFilesRecursively(const std::string& directory) {
    std::vector<std::string> files;
    std::error_code ec;
    const fs::path root(directory);

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return files;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const fs::path relative = fs::relative(it->path(), root, ec);
        if (ec) {
            ec.clear();
            continue;
        }
        std::string generic = relative.generic_string();
        if (!generic.empty()) {
            files.push_back(std::move(generic));
        }
    }
    return files;
}

}  // namespace boxedvn
