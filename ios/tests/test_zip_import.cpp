/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  GPLv2; see license.txt.
 *
 *  These tests build real ZIP archives with minizip - including hostile ones -
 *  and extract them to a temporary directory, so the traversal defence is
 *  exercised end to end rather than only at the string level.
 */

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "boxedvn/zip_import.h"
#include "boxedvn_test.h"

extern "C" {
#include "zip.h"
}

namespace fs = std::filesystem;
using namespace boxedvn;

namespace {

struct TemporaryDirectory {
    fs::path path;

    explicit TemporaryDirectory(const std::string& name) {
        path = fs::temp_directory_path() /
               ("boxedvn-test-" + name + "-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct ArchiveEntry {
    std::string name;
    std::string contents;
};

// Writes a ZIP archive with the given entries, storing names verbatim so that
// hostile names survive into the archive exactly as an attacker would craft
// them.
bool writeArchive(const fs::path& archivePath,
                  const std::vector<ArchiveEntry>& entries) {
    zipFile zip = zipOpen64(archivePath.string().c_str(), APPEND_STATUS_CREATE);
    if (zip == nullptr) {
        return false;
    }
    zip_fileinfo info;
    std::memset(&info, 0, sizeof(info));

    for (const ArchiveEntry& entry : entries) {
        if (zipOpenNewFileInZip64(zip, entry.name.c_str(), &info, nullptr, 0,
                                  nullptr, 0, nullptr, Z_DEFLATED,
                                  Z_DEFAULT_COMPRESSION, 0) != ZIP_OK) {
            zipClose(zip, nullptr);
            return false;
        }
        if (!entry.contents.empty()) {
            zipWriteInFileInZip(zip, entry.contents.data(),
                                static_cast<unsigned>(entry.contents.size()));
        }
        zipCloseFileInZip(zip);
    }
    zipClose(zip, nullptr);
    return true;
}

std::string readFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

BOXEDVN_TEST(zip_listing_reports_entries_and_sizes) {
    TemporaryDirectory temp("listing");
    const fs::path archive = temp.path / "game.zip";
    CHECK(writeArchive(archive, {
                                    {"Game/game.exe", "MZ-not-really"},
                                    {"Game/data/a.dat", "aaaa"},
                                }));

    const ZipListing listing = listZipArchive(archive.string());
    CHECK_EQ(listing.ok, true);
    CHECK_EQ(listing.entries.size(), size_t(2));
    CHECK_EQ(listing.rejectedEntryCount, size_t(0));
    CHECK_EQ(listing.redundantTopLevel, std::string("Game"));
    CHECK_EQ(listing.totalUncompressedBytes, uint64_t(17));
}

BOXEDVN_TEST(zip_listing_flags_a_traversal_entry_without_extracting) {
    TemporaryDirectory temp("traversal-list");
    const fs::path archive = temp.path / "evil.zip";
    CHECK(writeArchive(archive, {
                                    {"Game/game.exe", "ok"},
                                    {"../../../../tmp/pwned", "owned"},
                                }));

    const ZipListing listing = listZipArchive(archive.string());
    CHECK_EQ(listing.ok, true);
    CHECK_EQ(listing.rejectedEntryCount, size_t(1));

    bool sawRejection = false;
    for (const ZipEntrySummary& entry : listing.entries) {
        if (!entry.accepted) {
            sawRejection = true;
            CHECK_CONTAINS(entry.rejectionReason, "escape the destination");
        }
    }
    CHECK_EQ(sawRejection, true);
}

BOXEDVN_TEST(extraction_refuses_an_archive_containing_a_traversal_entry) {
    TemporaryDirectory temp("traversal-extract");
    const fs::path archive = temp.path / "evil.zip";
    const fs::path destination = temp.path / "out";
    CHECK(writeArchive(archive, {
                                    {"good.txt", "ok"},
                                    {"../escaped.txt", "owned"},
                                }));

    ZipExtractionOptions options;
    options.failOnRejectedEntry = true;
    const ZipExtractionResult result =
        extractZipArchive(archive.string(), destination.string(), options);

    CHECK_EQ(result.ok, false);
    CHECK_CONTAINS(result.error, "Refusing to extract");
    // Nothing outside the destination may exist.
    CHECK_EQ(fs::exists(temp.path / "escaped.txt"), false);
}

BOXEDVN_TEST(extraction_can_skip_unsafe_entries_and_keep_the_rest) {
    TemporaryDirectory temp("traversal-skip");
    const fs::path archive = temp.path / "mixed.zip";
    const fs::path destination = temp.path / "out";
    CHECK(writeArchive(archive, {
                                    {"good.txt", "ok"},
                                    {"/absolute.txt", "no"},
                                }));

    ZipExtractionOptions options;
    options.failOnRejectedEntry = false;
    options.flattenRedundantTopLevel = false;
    const ZipExtractionResult result =
        extractZipArchive(archive.string(), destination.string(), options);

    CHECK_EQ(result.ok, true);
    CHECK_EQ(result.filesWritten, size_t(1));
    CHECK_EQ(result.entriesSkipped, size_t(1));
    CHECK_EQ(readFile(destination / "good.txt"), std::string("ok"));
    CHECK_EQ(fs::exists(destination / "absolute.txt"), false);
}

BOXEDVN_TEST(extraction_flattens_a_redundant_top_level_directory) {
    TemporaryDirectory temp("flatten");
    const fs::path archive = temp.path / "game.zip";
    const fs::path destination = temp.path / "out";
    CHECK(writeArchive(archive, {
                                    {"Kanon/game.exe", "MZ"},
                                    {"Kanon/data/script.ks", "text"},
                                }));

    ZipExtractionOptions options;
    options.flattenRedundantTopLevel = true;
    const ZipExtractionResult result =
        extractZipArchive(archive.string(), destination.string(), options);

    CHECK_EQ(result.ok, true);
    CHECK_EQ(result.error, std::string());
    CHECK_EQ(fs::exists(destination / "game.exe"), true);
    CHECK_EQ(fs::exists(destination / "data" / "script.ks"), true);
    CHECK_EQ(fs::exists(destination / "Kanon"), false);
}

BOXEDVN_TEST(extraction_preserves_the_layout_when_flattening_is_off) {
    TemporaryDirectory temp("no-flatten");
    const fs::path archive = temp.path / "game.zip";
    const fs::path destination = temp.path / "out";
    CHECK(writeArchive(archive, {
                                    {"Kanon/game.exe", "MZ"},
                                    {"Kanon/data/script.ks", "text"},
                                }));

    ZipExtractionOptions options;
    options.flattenRedundantTopLevel = false;
    const ZipExtractionResult result =
        extractZipArchive(archive.string(), destination.string(), options);

    CHECK_EQ(result.ok, true);
    CHECK_EQ(fs::exists(destination / "Kanon" / "game.exe"), true);
}

BOXEDVN_TEST(extraction_enforces_the_uncompressed_size_ceiling) {
    TemporaryDirectory temp("bomb");
    const fs::path archive = temp.path / "big.zip";
    const fs::path destination = temp.path / "out";
    CHECK(writeArchive(archive, {{"big.dat", std::string(4096, 'a')}}));

    ZipExtractionOptions options;
    options.maxTotalUncompressedBytes = 1024;
    const ZipExtractionResult result =
        extractZipArchive(archive.string(), destination.string(), options);

    CHECK_EQ(result.ok, false);
    CHECK_CONTAINS(result.error, "above the");
    CHECK_EQ(fs::exists(destination / "big.dat"), false);
}

BOXEDVN_TEST(opening_a_non_zip_file_reports_a_specific_error) {
    TemporaryDirectory temp("not-a-zip");
    const fs::path fake = temp.path / "notazip.zip";
    {
        std::ofstream stream(fake, std::ios::binary);
        stream << "this is definitely not a zip archive";
    }

    const ZipListing listing = listZipArchive(fake.string());
    CHECK_EQ(listing.ok, false);
    CHECK_CONTAINS(listing.error, "not a ZIP file at all");
}

BOXEDVN_TEST(recursive_file_listing_returns_relative_forward_slash_paths) {
    TemporaryDirectory temp("listfiles");
    fs::create_directories(temp.path / "a" / "b");
    {
        std::ofstream(temp.path / "root.txt") << "x";
        std::ofstream(temp.path / "a" / "b" / "deep.txt") << "y";
    }

    std::vector<std::string> files = listRegularFilesRecursively(temp.path.string());
    std::sort(files.begin(), files.end());
    CHECK_EQ(files.size(), size_t(2));
    CHECK_EQ(files[0], std::string("a/b/deep.txt"));
    CHECK_EQ(files[1], std::string("root.txt"));
}
