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
 *  platform/linux/platform.cpp routes three calls through C functions that
 *  the macOS application shell provides (platform/mac is compiled out on
 *  iOS, see cmake/BoxedwineSources.cmake).  Those functions are declared
 *  under #ifdef __MACH__, which is true on iOS too, so iOS has to supply
 *  them.  The macOS originals are in
 *  project/mac-xcode/Boxedwine/Boxedwine/MacPlatform.m.
 *  ---------------------------------------------------------------------
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <string.h>

#include "BVNRuntime.h"

extern "C" {

// macOS raises the thread priority of guest CPU threads with
// +[NSThread setThreadPriority:].  The same call exists on iOS.
void MacPlatormSetThreadPriority(void) {
    [NSThread setThreadPriority:1.0];
}

// macOS reveals a path in the Finder.  iOS has no Finder; the equivalent for
// the user is the Files app, and an app cannot ask it to reveal an arbitrary
// path.  Rather than silently doing nothing, this records the request in the
// log so a caller that depended on it is visible during bring-up.
void MacPlatformOpenFileLocation(const char* location) {
    if (location == nullptr) {
        return;
    }
    NSString* path = [NSString stringWithUTF8String:location];
    if ([path hasPrefix:@"http"]) {
        NSURL* url = [NSURL URLWithString:path];
        if (url != nil) {
            [UIApplication.sharedApplication openURL:url
                                             options:@{}
                                   completionHandler:nil];
            return;
        }
    }
    BVNLogWrite(BVNLogLevelInfo, "platform",
                [[NSString stringWithFormat:
                               @"openFileLocation(%@) has no iOS equivalent; "
                               @"the path is reachable through the Files app "
                               @"under On My iPhone > BoxedVN.",
                               path] UTF8String]);
}

// Resource lookup inside the app bundle.  Boxedwine passes a bare file name.
const char* MacPlatformGetResourcePath(const char* name) {
    // The returned pointer must outlive the call, and Boxedwine copies it
    // immediately into a BString, so one static buffer matches the macOS
    // implementation's contract exactly.
    static char buffer[1024];

    if (name == nullptr) {
        return nullptr;
    }
    NSString* requested = [NSString stringWithUTF8String:name];
    NSString* base = requested.stringByDeletingPathExtension;
    NSString* extension = requested.pathExtension;

    NSString* path = [NSBundle.mainBundle pathForResource:base
                                                   ofType:extension.length > 0
                                                              ? extension
                                                              : nil];
    if (path == nil) {
        return nullptr;
    }
    strlcpy(buffer, path.UTF8String, sizeof(buffer));
    return buffer;
}

}  // extern "C"
