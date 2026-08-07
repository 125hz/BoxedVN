/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.  See license.txt.
 *
 *  ---------------------------------------------------------------------
 *  The C face of ios/support: executable inspection, safe ZIP import and
 *  game manifests.  Every function here performs the real operation; there
 *  are no placeholders.
 *
 *  Memory: functions that return a BVN*Result fill a caller-owned struct
 *  holding fixed-size buffers, so Swift never has to free anything.  The one
 *  exception is BVNManifestSerialise, which returns heap memory the caller
 *  releases with BVNStringFree.
 *  ---------------------------------------------------------------------
 */

#ifndef BVN_IMPORT_H
#define BVN_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BVN_MAX_PATH 1024
#define BVN_MAX_DIAGNOSTIC 512
#define BVN_MAX_SHORT 64

// ---------------------------------------------------------------------------
// Executable inspection
// ---------------------------------------------------------------------------

typedef enum {
    BVNGuestArchitectureUnknown = 0,
    BVNGuestArchitectureX86_16 = 1,
    BVNGuestArchitectureX86_32 = 2,
    BVNGuestArchitectureX86_64 = 3,
} BVNGuestArchitecture;

typedef struct {
    BVNGuestArchitecture architecture;
    // Stable identifier of the container format: "pe32", "pe32+", "ne-win16",
    // "dos-mz", "le-vxd", "not-an-executable" or "unknown".
    char format[BVN_MAX_SHORT];
    // Identifier of the backend that would run it, or "none".
    char backend[BVN_MAX_SHORT];
    uint16_t coffMachine;
    uint16_t subsystem;
    bool managedDotNet;
    bool runnable;
    char diagnostic[BVN_MAX_DIAGNOSTIC];
} BVNExecutableInfo;

void BVNInspectExecutable(const char* path, BVNExecutableInfo* out);

// ---------------------------------------------------------------------------
// ZIP import
// ---------------------------------------------------------------------------

typedef struct {
    bool ok;
    uint64_t totalUncompressedBytes;
    size_t entryCount;
    size_t rejectedEntryCount;
    char redundantTopLevel[BVN_MAX_PATH];
    char error[BVN_MAX_DIAGNOSTIC];
    // The reason the first unsafe entry was rejected, so the UI can explain a
    // refusal precisely instead of saying "invalid archive".
    char firstRejection[BVN_MAX_DIAGNOSTIC];
} BVNZipListing;

// Reads the central directory and sanitises every entry name.  Writes nothing
// to disk.
void BVNZipInspect(const char* archivePath, BVNZipListing* out);

typedef struct {
    bool ok;
    size_t filesWritten;
    size_t directoriesCreated;
    size_t entriesSkipped;
    uint64_t bytesWritten;
    char error[BVN_MAX_DIAGNOSTIC];
} BVNZipExtractionResult;

// Extracts into `destinationDirectory`.  Refuses the whole archive if any
// entry would escape the destination.  Blocking; call off the main thread.
void BVNZipExtract(const char* archivePath,
                   const char* destinationDirectory,
                   bool flattenRedundantTopLevel,
                   BVNZipExtractionResult* out);

// ---------------------------------------------------------------------------
// Executable discovery
// ---------------------------------------------------------------------------

typedef struct {
    char relativePath[BVN_MAX_PATH];
    BVNExecutableInfo info;
} BVNDiscoveredExecutable;

// Scans `contentDirectory` recursively for .exe/.com/.bat/.pif files, inspects
// each, and writes up to `capacity` of them into `out`, best candidate first.
// Returns the number of executables found, which may exceed `capacity`.
size_t BVNDiscoverExecutables(const char* contentDirectory,
                              BVNDiscoveredExecutable* out,
                              size_t capacity);

// ---------------------------------------------------------------------------
// Manifests
// ---------------------------------------------------------------------------

// Derives a filesystem-safe identifier from a title.  Writes at most
// `capacity` bytes including the terminator.
void BVNMakeIdentifier(const char* title, char* out, size_t capacity);

// Writes a manifest for a freshly imported game to
// <contentDirectory>/../manifest.json.  Returns false and fills `error` on
// failure.
bool BVNManifestWriteForImport(const char* manifestPath,
                               const char* identifier,
                               const char* title,
                               const char* contentDirectory,
                               const char* selectedExecutable,
                               const BVNDiscoveredExecutable* discovered,
                               size_t discoveredCount,
                               int64_t importedAtUnixSeconds,
                               char* error,
                               size_t errorCapacity);

typedef struct {
    bool ok;
    int schemaVersion;
    char identifier[BVN_MAX_SHORT];
    char title[BVN_MAX_PATH];
    char backend[BVN_MAX_SHORT];
    char contentDirectory[BVN_MAX_PATH];
    char selectedExecutable[BVN_MAX_PATH];
    char workingDirectory[BVN_MAX_PATH];
    char winePrefix[BVN_MAX_SHORT];
    uint32_t requestedWidth;
    uint32_t requestedHeight;
    int64_t importedAtUnixSeconds;
    char error[BVN_MAX_DIAGNOSTIC];
} BVNManifestSummary;

// Reads and validates a manifest file.  A manifest written by a newer BoxedVN
// is refused with an explanation rather than partially read.
void BVNManifestRead(const char* manifestPath, BVNManifestSummary* out);

// Rewrites the mutable launch settings of an existing manifest, preserving
// every other field, including the discovered executable list.
bool BVNManifestUpdateLaunchSettings(const char* manifestPath,
                                     const char* selectedExecutable,
                                     const char* workingDirectory,
                                     const char* const* arguments,
                                     size_t argumentCount,
                                     uint32_t requestedWidth,
                                     uint32_t requestedHeight,
                                     char* error,
                                     size_t errorCapacity);

// Reads the arguments array out of a manifest as a single newline-separated
// string.  A newline is not valid inside a shell argument BoxedVN would pass,
// so the encoding is unambiguous, and it crosses the Swift boundary as one
// value instead of a 2-D C array.  Returns the number of bytes written,
// excluding the terminator.
size_t BVNManifestCopyArgumentsJoined(const char* manifestPath,
                                      char* out,
                                      size_t capacity);

// ---------------------------------------------------------------------------
// Runtime backend description
// ---------------------------------------------------------------------------

typedef struct {
    char identifier[BVN_MAX_SHORT];
    bool implemented;
    bool requiresJIT;
    bool hasInterpreterFallback;
    bool runsX86_16;
    bool runsX86_32;
    bool runsX86_64;
} BVNBackendDescription;

// Writes up to `capacity` backend descriptions and returns how many exist.
size_t BVNCopyBackendDescriptions(BVNBackendDescription* out, size_t capacity);

// The user-facing reason an architecture cannot run, or an empty string when
// it can.
void BVNUnsupportedArchitectureMessage(BVNGuestArchitecture architecture,
                                       char* out, size_t capacity);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_IMPORT_H
