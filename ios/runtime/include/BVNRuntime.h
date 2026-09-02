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
 *    * BVNGuestMain does startup work and RETURNS, handing the main thread
 *      back to the ordinary UIKit run loop.  While idle, BoxedVN is a normal
 *      iOS app with no custom run loop pumping - see BVNGuestMain's
 *      definition for why the previous polling loop was removed (it broke
 *      UIDocumentPickerViewController's touch handling).
 *    * BVNRuntimeRequestLaunch may be called from any thread.  It records the
 *      request and queues the session on the main queue; the guest always
 *      starts on the main thread, because SDL's UIKit video backend requires
 *      it.  boxedmain() then blocks the main thread for the session's
 *      duration and SDL's own event pump takes over.
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
    // Executable memory was mapped, written and actually called - the
    // strongest confirmation available, only produced by
    // BVNJITProbeExecute().
    BVNJITStatusAvailable = 1,
    // Confirmed unavailable, or an execute attempt failed before it could
    // report back (see BVNJITProbeExecute()'s file-header comment - a real
    // crash there is possible, and this value could theoretically be stale
    // from before one).
    BVNJITStatusUnavailable = 2,
    // The kernel has this process flagged CS_DEBUGGED, so JIT is EXPECTED to
    // work, but nothing has actually tried to execute generated code yet.
    // Only BVNJITProbeStatus() produces this - it never risks execution, so
    // it cannot promote this to BVNJITStatusAvailable on its own.
    BVNJITStatusLikelyAvailable = 3,
} BVNJITStatus;

typedef struct {
    BVNJITStatus status;
    // True when this binary contains Boxedwine's ARM64 JIT backend at all.
    bool jitCompiledIn;
    // True when the kernel has flagged this process CS_DEBUGGED - i.e. a
    // debugger genuinely attached (StikDebug or equivalent).  Read directly
    // from the kernel via csops(), which never touches guest memory and is
    // always safe to call.
    bool debuggerAttached;
    // True only after BVNJITProbeExecute() has actually mapped, written and
    // called generated code successfully.  BVNJITProbeStatus() never sets
    // this true - it does not attempt execution, so it cannot know.
    bool executableMemoryAvailable;
    // Human-readable detail, including the failing syscall and errno when a
    // probe failed.  Owned by the runtime; valid until the next probe.
    const char* detail;
} BVNJITReport;

// Safe: reads jitCompiledIn and debuggerAttached only (both via csops() /
// compile-time flags) and NEVER attempts to execute anything. Call this as
// often as you like - at startup, on a poll timer, wherever - it cannot
// crash the process.
//
// Reports at most BVNJITStatusLikelyAvailable: CS_DEBUGGED being set is
// necessary for JIT to work but is not sufficient proof, and proving it
// requires BVNJITProbeExecute()'s risk (see below).
BVNJITReport BVNJITProbeStatus(void);

// UNSAFE. Actually maps a page, writes a small function into it, flushes the
// instruction cache, and CALLS it - the same sequence Boxedwine's real JIT
// depends on. If the process cannot really execute from that page, iOS
// delivers an uncatchable SIGKILL (CODESIGNING / "Invalid Page") the instant
// the call is attempted - not a caught exception, not an error return, no
// recovery possible in-process. On some iOS versions this SIGKILL can happen
// even when mmap(PROT_EXEC) itself returned success and the process holds
// CS_DEBUGGED; the failure genuinely cannot be predicted without attempting
// it.
//
// Only call this from a context where a crash is an acceptable, explainable
// consequence of what the user just did:
//   - immediately before actually starting a guest (BVNRuntime.mm does this;
//     Boxedwine's real JIT would hit the identical risk moments later
//     regardless, so this at least ties the crash to "I pressed Launch"
//     rather than "the app just opened"), or
//   - a UI action the user explicitly chose, clearly labelled as possibly
//     crashing the app.
//
// NEVER call this from application startup, a timer, or anywhere else that
// runs without a specific, understood user action behind it - that is
// exactly the mistake that made every cold launch crash before this was
// split out.
BVNJITReport BVNJITProbeExecute(void);

// ---------------------------------------------------------------------------
// Signed memory entitlement and current process budget
// ---------------------------------------------------------------------------

typedef enum {
    BVNMemoryEntitlementUnknown = 0,
    BVNMemoryEntitlementDisabled = 1,
    BVNMemoryEntitlementEnabled = 2,
} BVNMemoryEntitlementStatus;

typedef struct {
    // Read from this running process's kernel-validated code-signature blob,
    // not from the source .entitlements file in the repository.
    BVNMemoryEntitlementStatus increasedMemoryLimit;
    // Current dirty-memory headroom reported by os_proc_available_memory().
    // It is a changing snapshot, not the device's total RAM.
    uint64_t availableBytes;
    uint64_t physicalMemoryBytes;
    // Current resident footprint of BoxedVN itself. Used by the optional
    // in-game performance overlay; unlike availableBytes this is memory
    // already committed by the process.
    uint64_t processResidentBytes;
    const char* detail;
} BVNMemoryReport;

// Safe and side-effect free. Suitable for the library screen and its timer.
BVNMemoryReport BVNMemoryProbe(void);

// ---------------------------------------------------------------------------
// Optional FEX32 low-address identity-map feasibility
// ---------------------------------------------------------------------------

typedef enum {
    BVNLowAddressProbeNotRun = 0,
    BVNLowAddressProbeBlocked = 1,
    BVNLowAddressProbePartial = 2,
    BVNLowAddressProbeCandidate = 3,
    BVNLowAddressProbeQueryError = 4,
    BVNLowAddressProbeReservationError = 5,
} BVNLowAddressProbeStatus;

typedef struct {
    BVNLowAddressProbeStatus status;
    uint64_t hostPageSize;
    uint64_t blockingRegionStart;
    uint64_t blockingRegionSize;
    uint32_t regionCount;
    uint32_t candidateCount;
    uint32_t claimedCandidateCount;
    int32_t machResult;
    const char* detail;
} BVNLowAddressProbeReport;

// Explicit, temporary, non-overwriting device probe. It only reserves holes
// that vm_region_64 first reports free, verifies read/write identity, and
// immediately releases them. It never requests executable permission and is
// never called by startup or the memory polling path.
BVNLowAddressProbeReport BVNLow4GiBIdentityProbe(void);

// Memory advertised to the 32-bit Linux/Wine guest. Boxedwine historically
// hard-coded 1 GB in both sysinfo() and /proc/meminfo; iOS can safely expose a
// larger but still 32-bit-addressable budget when the signed process has it.
uint64_t BVNGuestReportedTotalMemory(void);
uint64_t BVNGuestReportedFreeMemory(void);

// Applies the persisted whole-app orientation preference after Settings
// changes it. Values are 0 portrait, 1 landscape, 2 landscape flipped.
void BVNApplyPreferredOrientation(void);

// Maximum percentage of the guest image that fill-aspect may crop from each
// opposing edge. Zero behaves like aspect fit; 25 allows up to half of one
// guest dimension to be outside the display. The preference is persisted and
// applied to the active guest when possible.
int BVNGuestFillCropPercent(void);
void BVNGuestSetFillCropPercent(int percent);

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

// Documents/Fonts: font files the user drops in through the Files app, copied
// into every Wine prefix at launch.  BoxedVN ships no fonts of its own - the
// faces these games expect are Microsoft's - so this is the only way a guest
// that needs a font the root filesystem lacks can get one.
const char* BVNPathFonts(void);

// Caches: regenerable data, safe for the system to purge.
const char* BVNPathCaches(void);

// The pinned root filesystem archive that shipped in the app bundle, or NULL
// when the build was made without one (see scripts/fetch-rootfs.sh and the
// BOXEDVN_BUNDLE_ROOTFS build option).
const char* BVNPathBundledRootFilesystemZip(void);

// Directory holding the patched 32-bit DXVK modules shipped with the app, or
// NULL when the build did not include them.
const char* BVNPathBundledDxvkDirectory(void);

// Optional BoxedWine x86-64 runtime resources. These return NULL unless the
// validated glibc/Wine64 layers were bundled by scripts/build-ios.sh.
const char* BVNPathBundledWine64GlibcZip(void);
const char* BVNPathBundledWine64Zip(void);
const char* BVNPathBundledX64GraphicsProbe(void);
const char* BVNPathBundledDXMTDirectory(void);

// ---------------------------------------------------------------------------
// Launching a guest
// ---------------------------------------------------------------------------

typedef enum {
    BVNWineRendererAutomatic = 0,
    BVNWineRendererWineD3D = 1,
    BVNWineRendererDXVK = 2,
} BVNWineRenderer;

typedef struct {
    // Absolute host path of the Boxedwine root filesystem ZIP.  Mounted
    // read-only at '/'; writes land in writableRootPath.  Required.
    const char* rootFilesystemZipPath;

    // Additional read-only root filesystem layers. x86-64 launches mount the
    // validated glibc and Wine64 archives here after the ordinary BoxedWine
    // archive. May be NULL when count is zero.
    const char* const* rootFilesystemOverlayZipPaths;
    size_t rootFilesystemOverlayZipCount;

    // Absolute host path of the writable overlay for this session.  Created if
    // missing.  Required.
    const char* writableRootPath;

    // Absolute host path of the imported game directory.  Mounted as drive D:
    // inside the guest.  May be NULL to launch a program that already lives in
    // the root filesystem, such as Wine's own notepad.
    const char* gameDirectoryHostPath;

    // Absolute host directory containing the selected program. Compatibility
    // detection is scoped here while gameDirectoryHostPath remains the whole
    // mounted drive. NULL falls back to gameDirectoryHostPath.
    const char* compatibilityDirectoryHostPath;

    // Absolute host path mounted as drive E: for every game and built-in Wine
    // tool. May be NULL, although the app normally supplies Documents/Shared.
    const char* sharedDirectoryHostPath;

    // Absolute host directory mounted over the Wine prefix's drive_c, so the
    // prefix's C: is reachable from the Files app while the rest of the
    // writable root stays private. NULL leaves drive_c inside the writable
    // root. Applies to the prefix the launch's lane uses (.wine or .wine64).
    const char* winePrefixDriveCHostPath;

    // Guest letters used for the two host mounts. Zero retains D: for the
    // game/container files and E: for shared files.
    char gameDriveLetter;
    char sharedDriveLetter;

    // Wine compatibility version ("win10", "win7", or "winxp"). NULL or
    // empty retains the root filesystem default.
    const char* windowsVersion;

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

    // Per-game Direct3D translator choice. Automatic is the safe general
    // policy; known D3D10/11 profiles may still choose DXVK.
    BVNWineRenderer wineRenderer;

    // When true, run Wine through /bin/wine rather than executing
    // executablePath directly.  Windows programs need this; Linux programs
    // such as /bin/wine itself do not.
    bool runThroughWine;

    // Per-launch optional 64-bit CPU and renderer selection. Neither changes
    // the default IA-32 path for other sessions.
    bool useFEX64;
    bool useDXMT;
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

// Session logging is on by default.  Turning it off stops all sinks - the file
// and the in-memory tail - which matters because the emulator logs from hot
// paths.  Thread safe.
void BVNLogSetEnabled(bool enabled);
bool BVNLogIsEnabled(void);

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
// this is called, an accepted launch request will not actually start a
// session.
void BVNRuntimeNotifyFrontendReady(void);

// Thread-safe executable-memory allocation signal. It must not be presented
// as a translated-code count: one allocation can contain many translations.
void BVNGuestLoadingUpdateJITProgress(size_t allocationCount);

// Live view. The container page registers a UIView (passed as an unretained
// pointer) that hosts the guest's presentation: SDL's view, the DXMT layer
// and the touch overlay move into it and SDL's own window hides, so the
// library stays on screen around the running guest. NULL restores the
// full-screen presentation. Main thread only.
void BVNGuestPresentationSetHostView(void* uiView);

// Live-view readout: the presented frame rate and its frame time, sampled by
// the same counter that fed the in-guest overlay. Either pointer may be NULL.
void BVNGuestPerformanceSnapshot(double* framesPerSecond,
                                 double* frameMilliseconds);

// Live-view control bar. Toggle the on-screen keyboard, read and set the
// pointer mode (0 = direct tap, 1 = Wine cursor), and tap a key by its SDL
// scancode name ("Return", "Space", "Escape", "Tab", ...). Main thread only.
void BVNGuestControlsToggleKeyboard(void);
int BVNGuestControlsPointerMode(void);
void BVNGuestControlsSetPointerMode(int mode);
void BVNGuestControlsTapKeyNamed(const char* sdlScancodeName);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BVN_RUNTIME_H
