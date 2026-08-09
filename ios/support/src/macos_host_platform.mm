/*
 * BoxedVN - Cocoa helpers for the development-only macOS emulator host.
 * GPLv2; see license.txt.
 *
 * platform/linux/platform.cpp calls these three functions under __MACH__,
 * which is true on macOS and iOS alike. Upstream defines them in its Xcode
 * project, but that file imports an Xcode-generated Swift bridging header;
 * ios/runtime/src/BVNMacPlatformShim.mm supplies them for the device but is
 * built against UIKit. Neither is usable from a plain CMake macOS build, so
 * the host gets its own minimal versions.
 *
 * Note the upstream spelling "MacPlatormSetThreadPriority" - the typo is part
 * of the ABI and must be matched exactly.
 */

#import <Foundation/Foundation.h>

#include <pthread.h>
#include <string>

extern "C" void MacPlatformOpenFileLocation(const char* location);
extern "C" const char* MacPlatformGetResourcePath(const char* name);
extern "C" void MacPlatormSetThreadPriority(void);

void MacPlatformOpenFileLocation(const char* location) {
    // Revealing a path in Finder is a desktop convenience the headless
    // debugging host has no use for. Deliberately inert rather than launching
    // an application from emulator code.
    (void)location;
}

const char* MacPlatformGetResourcePath(const char* name) {
    // The host has no application bundle; Boxedwine is given every path it
    // needs on the command line. Returning a stable empty string keeps callers
    // that expect a non-null result safe.
    (void)name;
    static const char* empty = "";
    return empty;
}

void MacPlatormSetThreadPriority(void) {
    // Match the device's intent - raise the calling thread - without assuming
    // a particular scheduling class is available.
    struct sched_param parameters;
    int policy = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &parameters) == 0) {
        const int maximum = sched_get_priority_max(policy);
        if (maximum > 0) {
            parameters.sched_priority = maximum;
            pthread_setschedparam(pthread_self(), policy, &parameters);
        }
    }
}
