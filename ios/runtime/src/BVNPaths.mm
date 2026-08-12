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
 *  Storage layout.
 *
 *  Application Support  (not visible in Files, excluded from backup)
 *      rootfs/          the Boxedwine root filesystem archive and its
 *                       writable overlay - large, regenerable from the
 *                       bundled or re-downloaded archive
 *      prefixes/<id>/   one Wine prefix per game
 *
 *  Documents            (visible in Files, included in backup)
 *      Games/<id>/      imported game content and its manifest
 *      Logs/            session logs, exportable through the share sheet
 *      Fonts/           font files the user supplies, copied into every
 *                       prefix at launch
 *
 *  Caches               (visible to nobody, purgeable by the system)
 *
 *  Wine prefixes live in Application Support rather than Documents because
 *  they are large, contain thousands of small files, and are rebuildable.
 *  Saves that a user would want to keep are inside the prefix, so
 *  KNOWN_LIMITATIONS_IOS.md records this as a gap to close by exporting
 *  saves into Documents.
 *  ---------------------------------------------------------------------
 */

#import <Foundation/Foundation.h>

#include <string>

#include "BVNRuntime.h"

namespace {

// Each accessor caches its result, so the returned pointers stay valid for the
// lifetime of the process as the header promises.
struct CachedPath {
    std::string value;
    bool resolved = false;
};

NSString* firstDirectory(NSSearchPathDirectory directory) {
    NSArray<NSString*>* paths =
        NSSearchPathForDirectoriesInDomains(directory, NSUserDomainMask, YES);
    return paths.firstObject;
}

// Creates `path` if needed and returns it, or nil on failure.
NSString* ensureDirectory(NSString* path, bool excludeFromBackup) {
    if (path == nil) {
        return nil;
    }
    NSFileManager* manager = NSFileManager.defaultManager;
    NSError* error = nil;
    if (![manager createDirectoryAtPath:path
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error]) {
        BVNLogWrite(BVNLogLevelError, "paths",
                    [[NSString stringWithFormat:@"Could not create %@: %@", path,
                                                error.localizedDescription]
                        UTF8String]);
        return nil;
    }
    if (excludeFromBackup) {
        NSURL* url = [NSURL fileURLWithPath:path isDirectory:YES];
        NSError* excludeError = nil;
        if (![url setResourceValue:@YES
                            forKey:NSURLIsExcludedFromBackupKey
                             error:&excludeError]) {
            BVNLogWrite(BVNLogLevelWarning, "paths",
                        [[NSString stringWithFormat:
                                       @"Could not exclude %@ from backup: %@",
                                       path, excludeError.localizedDescription]
                            UTF8String]);
        }
    }
    return path;
}

const char* cachedSubdirectory(CachedPath& cache,
                               NSSearchPathDirectory searchPath,
                               NSString* subdirectory,
                               bool excludeFromBackup) {
    if (cache.resolved) {
        return cache.value.empty() ? nullptr : cache.value.c_str();
    }
    cache.resolved = true;

    NSString* base = firstDirectory(searchPath);
    if (base == nil) {
        return nullptr;
    }
    NSString* full = subdirectory.length > 0
                         ? [base stringByAppendingPathComponent:subdirectory]
                         : base;
    NSString* created = ensureDirectory(full, excludeFromBackup);
    if (created == nil) {
        return nullptr;
    }
    cache.value = created.UTF8String;
    return cache.value.c_str();
}

CachedPath gRootFilesystems;
CachedPath gWinePrefixes;
CachedPath gGames;
CachedPath gLogs;
CachedPath gFonts;
CachedPath gCaches;
CachedPath gBundledRootfs;

}  // namespace

extern "C" const char* BVNPathRootFilesystems(void) {
    return cachedSubdirectory(gRootFilesystems, NSApplicationSupportDirectory,
                              @"rootfs", /*excludeFromBackup=*/true);
}

extern "C" const char* BVNPathWinePrefixes(void) {
    return cachedSubdirectory(gWinePrefixes, NSApplicationSupportDirectory,
                              @"prefixes", /*excludeFromBackup=*/true);
}

extern "C" const char* BVNPathGames(void) {
    return cachedSubdirectory(gGames, NSDocumentDirectory, @"Games",
                              /*excludeFromBackup=*/false);
}

extern "C" const char* BVNPathLogs(void) {
    return cachedSubdirectory(gLogs, NSDocumentDirectory, @"Logs",
                              /*excludeFromBackup=*/false);
}

extern "C" const char* BVNPathFonts(void) {
    return cachedSubdirectory(gFonts, NSDocumentDirectory, @"Fonts",
                              /*excludeFromBackup=*/false);
}

extern "C" const char* BVNPathCaches(void) {
    return cachedSubdirectory(gCaches, NSCachesDirectory, @"BoxedVN",
                              /*excludeFromBackup=*/false);
}

extern "C" const char* BVNPathBundledRootFilesystemZip(void) {
    if (gBundledRootfs.resolved) {
        return gBundledRootfs.value.empty() ? nullptr
                                            : gBundledRootfs.value.c_str();
    }
    gBundledRootfs.resolved = true;

    // scripts/fetch-rootfs.sh places the pinned archive here when the build
    // was made with BOXEDVN_BUNDLE_ROOTFS=ON.  A development build without it
    // returns NULL, and the frontend then asks the user to import one.
    NSString* path = [NSBundle.mainBundle pathForResource:@"boxedwine"
                                                   ofType:@"zip"
                                              inDirectory:@"rootfs"];
    if (path == nil) {
        path = [NSBundle.mainBundle pathForResource:@"boxedwine" ofType:@"zip"];
    }
    if (path == nil) {
        return nullptr;
    }
    gBundledRootfs.value = path.UTF8String;
    return gBundledRootfs.value.c_str();
}

extern "C" const char* BVNPathBundledDxvkDirectory(void) {
    static CachedPath gBundledDxvk;
    if (gBundledDxvk.resolved) {
        return gBundledDxvk.value.empty() ? nullptr
                                          : gBundledDxvk.value.c_str();
    }
    gBundledDxvk.resolved = true;

    // ios/app/Dxvk is a folder reference, so the DLLs keep their subdirectory
    // inside the bundle. Resolve the directory rather than a single file: the
    // prefix installer copies whatever is present, so adding another DXVK
    // module later needs no code change here.
    NSString* path = [NSBundle.mainBundle.resourcePath
                      stringByAppendingPathComponent:@"Dxvk"];
    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:path
                                           isDirectory:&isDirectory]
        || !isDirectory) {
        return nullptr;
    }
    gBundledDxvk.value = path.UTF8String;
    return gBundledDxvk.value.c_str();
}
