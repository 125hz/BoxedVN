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
 *  The complete interface between the Apple frontend and the Boxedwine
 *  emulator core.  This header is plain C with no C++ in it, so Swift can
 *  import it directly through the bridging header without ever seeing a
 *  Boxedwine or SDL type.
 *
 *  Threading contract, in one place:
 *
 *    * main() calls SDL_UIKitRunApp, so SDL's UIApplication delegate owns the
 *      application lifecycle, and BVNGuestMain runs on the MAIN THREAD.
 *    * While no guest is running, BVNGuestMain services the main run loop,
 *      which is what keeps the SwiftUI library UI alive.
 *    * BVNRuntimeRequestLaunch may be called from any thread.  It only
 *      records the request; the guest is always started from the main thread
 *      by BVNGuestMain, because SDL's UIKit video backend requires it.
 *    * Guest CPU and JIT threads are pthreads created by Boxedwine.  Audio
 *      runs on SDL's audio thread.  Frame presentation happens on the main
 *      thread inside the emulator's own loop.
 *    * BVNRuntimeRequestShutdown may be called from any thread; it posts
 *      SDL_QUIT and returns immediately.
 *    * Exactly one guest session may exist at a time.  A launch request made
 *      while a session is running is refused, not queued.
 *  ---------------------------------------------------------------------
 */

#ifndef BVN_RUNTIME_H
#define BVN_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------

typedef enum {
    // No guest session; the library UI is being serviced.
    BVNRuntimeStateIdle = 0,
    // A launch has been requested but the guest has not started yet.
    BVNRuntimeStateStarting = 1,
    // The guest is running.
    BVNRuntimeStateRunning = 2,
    // Shutdown requested; the guest is winding down.
    BVNRuntimeStateStopping = 3,
    // The last session ended normally.  See BVNRuntimeLastExitCode.
    BVNRuntimeStateStopped = 4,
    // The last session could not start.  See BVNRuntimeLastError.
    BVNRuntimeStateFailed = 5,
} BVNRuntimeState;

const char* BVNRuntimeStateName(BVNRuntimeState state);

// ---------------------------------------------------------------------------
// JIT availability
// ---------------------------------------------------------------------------

typedef enum {
    BVNJITStatusUnknown = 0,
    // An executable MAP_JIT page was mapped, written and executed.
    BVNJITStatusAvailable = 1,
    // Executable memory could not be obtained.  BoxedVN cannot enable this
    // itself; an external JIT enabler must attach a debugger first.
    BVNJITStatusUnavailable = 2,
} BVNJITStatus;

typedef struct {
    BVNJITStatus status;
    // True when this binary contains Boxedwine's ARM64 JIT backend at all.
    bool jitCompiledIn;
    // True when the kernel has flagged this process CS_DEBUGGED - i.e. a
    // debugger genuinely attached (StikDebug or equivalent).  Read directly
    // from the kernel via csops(), independent of whether the mmap probe
    // below succeeded, so a failure can be told apart: "no debugger ever
    // attached" (this is false) versus "a debugger attached but executable
    // memory still isn't available" (this is true, executableMemoryAvailable
    // is false - a signing/entitlement problem, not a StikDebug problem).
    bool debuggerAttached;
    // True when mmap(PROT_EXEC | MAP_JIT) succeeded.  This is an observation
    // of what the kernel allowed, not a reading of the signed entitlement
    // blob; on iOS the two coincide because only the dynamic-codesigning
    // entitlement makes that mapping succeed.
    bool executableMemoryAvailable;
    // Human-readable detail, including the failing syscall and errno when the
    // probe failed.  Owned by the runtime; valid until the next probe.
    const char* detail;
} BVNJITReport;

// Allocates a MAP_JIT page, writes a function that returns a known value,
// flushes the instruction cache and calls it.  This is a real end-to-end test
// of the same mechanism Boxedwine's JIT uses, not a check of a flag.
//
// Safe to call repeatedly; the result is recomputed each time because JIT can
// become available after the app launches, once a JIT enabler attaches.
BVNJITReport BVNJITProbe(void);

// ---------------------------------------------------------------------------
// Storage layout
//
// Every accessor returns an absolute path to a directory that has already been
// created, or NULL if it could not be created.  Strings are owned by the
// runtime and remain valid for the lifetime of the process.
// ---------------------------------------------------------------------------

// Application Support: root filesystem, Wine runtime, Wine prefixes.
// Excluded from iCloud/iTunes backup.
const char* BVNPathRootFilesystems(void);
const char* BVNPathWinePrefixes(void);

// Documents (visible in the Files app): imported games, saves, logs.
const char* BVNPathGames(void);
const char* BVNPathLogs(void);

// Caches: regenerable data, safe for the system to purge.
const char* BVNPathCaches(void);

// The pinned root filesystem archive that shipped in the app bundle, or NULL
// when the build was made without one (see scripts/fetch-rootfs.sh and the
// BOXEDVN_BUNDLE_ROOTFS build option).
const char* BVNPathBundledRootFilesystemZip(void);

// ---------------------------------------------------------------------------
// Launching a guest
// ---------------------------------------------------------------------------

typedef struct {
    // Absolute host path of the Boxedwine root filesystem ZIP.  Mounted
    // read-only at '/'; writes land in writableRootPath.  Required.
    const char* rootFilesystemZipPath;

    // Absolute host path of the writable overlay for this session.  Created if
    // missing.  Required.
    const char* writableRootPath;

    // Absolute host path of the imported game directory.  Mounted as drive D:
    // inside the guest.  May be NULL to launch a program that already lives in
    // the root filesystem, such as Wine's own notepad.
    const char* gameDirectoryHostPath;

    // The program to run, as a guest path.  Either a Windows path such as
    // "d:\\game.exe" or a Linux path such as "/bin/wine".  Required.
    const char* executablePath;

    // Arguments passed after executablePath.  May be NULL when count is 0.
    const char* const* arguments;
    size_t argumentCount;

    // Guest environment entries, each "NAME=VALUE".  May be NULL.
    const char* const* environment;
    size_t environmentCount;

    // Guest working directory, as a guest path.  May be NULL for the default.
    const char* workingDirectory;

    // Emulated screen size.  Zero means Boxedwine's default of 800x600.
    uint32_t width;
    uint32_t height;

    // Emulated colour depth: 16 or 32.  Zero means Boxedwine's default.
    uint32_t bitsPerPixel;

    bool soundEnabled;

    // When true, run Wine through /bin/wine rather than executing
    // executablePath directly.  Windows programs need this; Linux programs
    // such as /bin/wine itself do not.
    bool runThroughWine;
} BVNLaunchRequest;

// Records a launch request.  Returns false and fills errorBuffer when the
// request is rejected, which happens when:
//   * a session is already running,
//   * a required field is missing,
//   * the root filesystem archive does not exist,
//   * JIT is unavailable and the build requires it.
//
// Returning true means the request was accepted, not that the guest started;
// poll BVNRuntimeGetState or watch the log for that.
bool BVNRuntimeRequestLaunch(const BVNLaunchRequest* request,
                             char* errorBuffer,
                             size_t errorBufferSize);

// Asks the running guest to shut down.  Returns false when no session is
// running.  The session ends asynchronously; wait for state Stopped.
bool BVNRuntimeRequestShutdown(void);

BVNRuntimeState BVNRuntimeGetState(void);

// Exit code of the last finished session, or INT32_MIN when none has run.
int32_t BVNRuntimeLastExitCode(void);

// Reason the last session failed to start or ended abnormally.  Empty string
// when there is nothing to report.  Owned by the runtime.
const char* BVNRuntimeLastError(void);

// Version string of the embedded Boxedwine core, e.g. "26R2".
const char* BVNRuntimeBoxedwineVersion(void);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

typedef enum {
    BVNLogLevelDebug = 0,
    BVNLogLevelInfo = 1,
    BVNLogLevelWarning = 2,
    BVNLogLevelError = 3,
} BVNLogLevel;

// Writes a structured entry to the session log.  Thread safe.
void BVNLogWrite(BVNLogLevel level, const char* category, const char* message);

// Absolute path of the current session log file, or NULL before logging has
// been started.  The file lives under BVNPathLogs and is visible in Files.
const char* BVNLogCurrentFilePath(void);

// Copies the most recent log text into `buffer`, NUL terminated, and returns
// the number of bytes written excluding the terminator.  Passing NULL returns
// the number of bytes that would be needed.
size_t BVNLogCopyRecent(char* buffer, size_t bufferSize);

// A monotonically increasing counter bumped on every write, so the log viewer
// can poll cheaply instead of subscribing.
uint64_t BVNLogGeneration(void);

// ---------------------------------------------------------------------------
// Entry point.  Called by main() in BVNMain.mm; not for use by the frontend.
// ---------------------------------------------------------------------------
int BVNGuestMain(int argc, char* argv[]);

// Set by the app delegate once the SwiftUI library UI is on screen.  Until
// this is called BVNGuestMain will not service launch requests.
void BVNRuntimeNotifyFrontendReady(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_RUNTIME_H
