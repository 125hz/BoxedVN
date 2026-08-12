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
 *  The runtime state machine.
 *
 *  BVNGuestMain runs on the main thread for the entire life of the process,
 *  because SDL's UIKit backend requires video and event handling to be there
 *  (see SDL_uikitappdelegate.m, which calls SDL_main from postFinishLaunch).
 *  It alternates between two phases:
 *
 *    Idle    - services the main run loop so the SwiftUI library UI is live,
 *              and watches for a launch request.
 *    Session - calls Boxedwine's boxedmain() with a synthesised argv.  That
 *              call owns the main thread until the guest exits, and drives
 *              SDL's event loop itself; guest CPU threads run on pthreads.
 *
 *  The frontend never calls boxedmain() and never touches SDL.  It posts a
 *  request and polls state, which keeps the C ABI free of both.
 *  ---------------------------------------------------------------------
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include <SDL.h>

#include "BVNExecMemory.h"
#include "BVNLaunchArguments.h"
#include "boxedvn/wine_prefix.h"
#include "BVNRuntime.h"

// Boxedwine's own entry point, from source/sdl/main.cpp.  Declared with C++
// linkage to match its definition there, exactly as platform/sdl/
// knativesystem.cpp declares it.
int boxedmain(int argc, const char** argv);

extern "C" bool BVNLogStartSessionFile(void);

// Implemented in BVNAppDelegate.mm.
extern "C" void BVNFrontendShowLibrary(void);
extern "C" void BVNFrontendHideLibrary(void);
extern "C" void BVNPrepareGuestPresentation(dispatch_block_t completion);
extern "C" void BVNFinishGuestPresentation(void);

namespace {

pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
BVNRuntimeState gState = BVNRuntimeStateIdle;
bool gHasPendingLaunch = false;
BVNLaunchConfiguration gPendingLaunch;
int32_t gLastExitCode = INT32_MIN;
std::string gLastError;
bool gFrontendReady = false;
bool gShutdownRequested = false;

// Set for the lifetime of an outstanding call to BVNJITProbeExecute() and
// never cleared if that call never returns.  See probeJitWithTimeout() below
// for why a hang, not just a crash, has to be planned for.
std::atomic<bool> gJitProbeInFlight{false};

void setState(BVNRuntimeState state) {
    pthread_mutex_lock(&gMutex);
    gState = state;
    pthread_mutex_unlock(&gMutex);
    BVNLogWrite(BVNLogLevelInfo, "runtime",
                (std::string("state -> ") + BVNRuntimeStateName(state)).c_str());
}

void setLastError(const std::string& message) {
    pthread_mutex_lock(&gMutex);
    gLastError = message;
    pthread_mutex_unlock(&gMutex);
    if (!message.empty()) {
        BVNLogWrite(BVNLogLevelError, "runtime", message.c_str());
    }
}

bool directoryExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool fileExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool createDirectories(const std::string& path, std::string& error) {
    NSError* nsError = nil;
    NSString* nsPath = [NSString stringWithUTF8String:path.c_str()];
    if (![NSFileManager.defaultManager createDirectoryAtPath:nsPath
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&nsError]) {
        error = std::string("Could not create '") + path +
                "': " + nsError.localizedDescription.UTF8String;
        return false;
    }
    return true;
}

void copyString(char* buffer, size_t bufferSize, const std::string& text) {
    if (buffer == nullptr || bufferSize == 0) {
        return;
    }
    const size_t copied = std::min(text.size(), bufferSize - 1);
    memcpy(buffer, text.data(), copied);
    buffer[copied] = '\0';
}

void logLaunch(const std::vector<std::string>& argv) {
    std::string joined;
    for (const std::string& argument : argv) {
        if (!joined.empty()) {
            joined.push_back(' ');
        }
        // Quote anything with a space so the log line can be replayed.
        if (argument.find(' ') != std::string::npos) {
            joined += "\"" + argument + "\"";
        } else {
            joined += argument;
        }
    }
    BVNLogWrite(BVNLogLevelInfo, "runtime",
                (std::string("boxedmain ") + joined).c_str());
}

// Validates a request and copies it into gPendingLaunch.  Caller holds gMutex.
bool acceptLaunchLocked(const BVNLaunchRequest* request, std::string& error) {
    if (request == nullptr) {
        error = "The launch request is null.";
        return false;
    }
    if (gState == BVNRuntimeStateStarting || gState == BVNRuntimeStateRunning ||
        gState == BVNRuntimeStateStopping) {
        error = std::string("A guest session is already ") +
                BVNRuntimeStateName(gState) +
                ". BoxedVN runs one session at a time; close the running one "
                "first.";
        return false;
    }
    if (gHasPendingLaunch) {
        error = "A launch request is already queued.";
        return false;
    }

    BVNLaunchConfiguration launch;
    if (request->rootFilesystemZipPath == nullptr ||
        request->rootFilesystemZipPath[0] == '\0') {
        error = "No root filesystem archive was given. BoxedVN needs "
                "Boxedwine's Linux/Wine root filesystem to run anything; see "
                "scripts/fetch-rootfs.sh.";
        return false;
    }
    launch.rootFilesystemZipPath = request->rootFilesystemZipPath;
    if (!fileExists(launch.rootFilesystemZipPath)) {
        error = std::string("The root filesystem archive '") +
                launch.rootFilesystemZipPath + "' does not exist.";
        return false;
    }

    if (request->writableRootPath == nullptr ||
        request->writableRootPath[0] == '\0') {
        error = "No writable root path was given.";
        return false;
    }
    launch.writableRootPath = request->writableRootPath;

    if (request->executablePath == nullptr ||
        request->executablePath[0] == '\0') {
        error = "No executable path was given.";
        return false;
    }
    launch.executablePath = request->executablePath;

    if (request->gameDirectoryHostPath != nullptr &&
        request->gameDirectoryHostPath[0] != '\0') {
        launch.gameDirectoryHostPath = request->gameDirectoryHostPath;
        if (!directoryExists(launch.gameDirectoryHostPath)) {
            error = std::string("The game directory '") +
                    launch.gameDirectoryHostPath + "' does not exist.";
            return false;
        }
    }

    if (request->sharedDirectoryHostPath != nullptr &&
        request->sharedDirectoryHostPath[0] != '\0') {
        launch.sharedDirectoryHostPath = request->sharedDirectoryHostPath;
        if (!directoryExists(launch.sharedDirectoryHostPath)) {
            error = std::string("The shared directory '") +
                    launch.sharedDirectoryHostPath + "' does not exist.";
            return false;
        }
    }

    if (request->workingDirectory != nullptr) {
        launch.workingDirectory = request->workingDirectory;
    }
    for (size_t i = 0; i < request->argumentCount; ++i) {
        if (request->arguments != nullptr && request->arguments[i] != nullptr) {
            launch.arguments.emplace_back(request->arguments[i]);
        }
    }

    for (size_t i = 0; i < request->environmentCount; ++i) {
        if (request->environment != nullptr &&
            request->environment[i] != nullptr) {
            launch.environment.emplace_back(request->environment[i]);
        }
    }
    launch.width = request->width;
    launch.height = request->height;
    launch.bitsPerPixel = request->bitsPerPixel;
    launch.soundEnabled = request->soundEnabled;
    launch.runThroughWine = request->runThroughWine;
    launch.requestedWineRenderer = static_cast<int>(request->wineRenderer);
    BVNApplyDefaultRendererPolicy(launch);

    // Before the per-title profile, so a title profile can still override
    // anything decided here, and after the user's own arguments have been
    // copied in, because whether they already carry Chromium switches is what
    // decides if BoxedVN adds its own.
    const BVNEngineProfileResult engineProfile =
        BVNApplyEngineCompatibilityProfile(launch);
    if (!engineProfile.reason.empty()) {
        BVNLogWrite(BVNLogLevelInfo, "engine", engineProfile.reason.c_str());
    }

    // LAST, so a profile can override any default above it. Build 66 called
    // this before enableWineD3DVulkan was assigned, so its Grisaia profile -
    // whose whole point was to turn DXVK off - was silently overwritten two
    // lines later and the experiment never ran: the device log still shows
    // "-dxvk 1" and the same six DXVK device failures.
    //
    // Inspect both the requested executable and wrapper arguments so a profile
    // remains correct if a future launcher delegates through Wine.
    if (BVNApplyKnownCompatibilityProfile(launch)) {
        BVNLogWrite(BVNLogLevelInfo, "compatibility",
                    "A per-title compatibility profile was applied; see the "
                    "boxedmain command line below for what it changed.");
    }

    if (!launch.rendererReason.empty()) {
        BVNLogWrite(BVNLogLevelInfo, "renderer",
                    launch.rendererReason.c_str());
    }

    gPendingLaunch = std::move(launch);
    gHasPendingLaunch = true;
    gState = BVNRuntimeStateStarting;
    return true;
}

// Calls BVNJITProbeExecute() on a background thread and waits for it with a
// hard timeout, so a hang inside the probe can never freeze runSession()'s
// caller - the main thread, which is also the ONLY thread servicing UIKit's
// run loop.  Before this wrapper, a stuck probe meant a stuck app: no crash,
// no alert, nothing but a dead UI, because the call sat directly in the path
// SDL and UIKit both depend on.
//
// A hang here is a real possibility, not a defensive nicety: on iOS 26/27
// the target intentionally stops at StikDebug's universal JIT breakpoint so
// the external script can prepare the requested pages. If the debugger is
// attached but the script is absent, stopped, or incompatible, that request
// may never be serviced. The wrapper cannot repair the external session, but
// it can stop the main thread from waiting forever and report the setup that
// needs attention.
//
// The worker thread is deliberately abandoned on timeout, not joined or
// cancelled: a thread parked in a kernel trap servicing (or failing to get an
// acknowledgement for) a hardware exception generally cannot be cancelled
// from user space, and forcing the issue risks corrupting whatever state the
// kernel already associated with that fault. One leaked thread is a small
// price for the main thread never blocking indefinitely.
constexpr int64_t kJitProbeTimeoutSeconds = 6;

// Prepares the executable arena on a background thread and waits for it with
// the same hard timeout, and for the same reason: this runs on the thread
// servicing UIKit's run loop, and a debugger that stops the requesting thread
// without resuming it would otherwise freeze the app before its UI appears.
//
// A failure or a timeout is deliberately not fatal and not surfaced as an
// alert. Nothing has been asked for yet, BVNExecMemPrepareArena remembers
// only success, and the guest-launch path will ask again - so a user who
// opens BoxedVN before attaching StikDebug simply gets the old behaviour.
void prepareJitArenaWithTimeout() {
    if (gJitProbeInFlight.load()) {
        return;
    }
    gJitProbeInFlight.store(true);

    __block bool prepared = false;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    dispatch_async(
        dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            prepared = BVNExecMemPrepareArena();
            gJitProbeInFlight.store(false);
            dispatch_semaphore_signal(finished);
        });

    const long timedOut = dispatch_semaphore_wait(
        finished,
        dispatch_time(DISPATCH_TIME_NOW,
                      kJitProbeTimeoutSeconds * NSEC_PER_SEC));
    if (timedOut != 0) {
        BVNLogWrite(BVNLogLevelWarning, "jit",
                    "StikDebug did not answer the startup request for "
                    "executable memory within the timeout. Leaving it to the "
                    "guest-launch path to ask again rather than blocking the "
                    "app before its UI appears.");
        return;
    }
    BVNLogWrite(prepared ? BVNLogLevelInfo : BVNLogLevelWarning, "jit",
                prepared
                    ? "Executable memory was obtained at startup, so JIT no "
                      "longer depends on StikDebug still being attached when "
                      "a game is launched."
                    : "Executable memory could not be obtained at startup. "
                      "If StikDebug is attached later, launching a game will "
                      "ask again.");
    BVNLogWrite(BVNLogLevelInfo, "jit", BVNExecMemReport());
}

BVNJITReport probeJitWithTimeout() {
    if (gJitProbeInFlight.load()) {
        // A previous attempt is still stuck out there. BVNExecMemory's
        // bookkeeping (which strategy is selected, whether a page is
        // currently mapped) is plain global state with no locking of its
        // own - it was never meant to be entered by two threads at once -
        // so starting a second probe now would race with whatever the first
        // one is still doing, rather than merely wasting a thread.
        BVNJITReport report{};
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        report.debuggerAttached = false;
        report.jitCompiledIn = false;
        static char detail[256];
        snprintf(detail, sizeof(detail),
                 "A previous JIT probe from an earlier launch attempt never "
                 "returned and is still running. BoxedVN will not start a "
                 "second one, since the two would race on the same "
                 "bookkeeping. Force-quit and reopen the app before trying "
                 "again.");
        report.detail = detail;
        return report;
    }

    gJitProbeInFlight.store(true);

    __block BVNJITReport probeResult{};
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);

    dispatch_async(
        dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            probeResult = BVNJITProbeExecute();
            gJitProbeInFlight.store(false);
            dispatch_semaphore_signal(finished);
        });

    const long timedOut = dispatch_semaphore_wait(
        finished,
        dispatch_time(DISPATCH_TIME_NOW, kJitProbeTimeoutSeconds * NSEC_PER_SEC));

    if (timedOut != 0) {
        BVNLogWrite(BVNLogLevelError, "jit",
                    "The executable-memory probe did not return within the "
                    "timeout. Treating JIT as unavailable and abandoning the "
                    "stuck probe thread rather than blocking the main thread "
                    "indefinitely. Confirm StikDebug's universal script is "
                    "assigned to BoxedVN and still running.");
        BVNJITReport report{};
        report.status = BVNJITStatusUnavailable;
        report.executableMemoryAvailable = false;
        report.debuggerAttached = false;
        report.jitCompiledIn = false;
        static char detail[640];
        snprintf(detail, sizeof(detail),
                 "The executable-memory probe did not return within %lld "
                 "seconds. StikDebug did not service BoxedVN's universal JIT "
                 "breakpoint request. In StikDebug, enable Advanced Options, "
                 "long-press BoxedVN, assign universal.js, then launch it "
                 "with that script kept active. BoxedVN is treating JIT as "
                 "unavailable rather than let the app stay frozen.",
                 (long long)kJitProbeTimeoutSeconds);
        report.detail = detail;
        return report;
    }

    return probeResult;
}

bool configureMoltenVKForWineD3D(std::string& error) {
    // WineD3D's D3D11 compatibility path does not need the very large bindless
    // descriptor capacity supplied by Metal argument buffers. On this device,
    // the final Saya pipeline stopped forever while MoltenVK was compiling it
    // with argument buffers enabled. Use the simpler descriptor path and put
    // a hard ceiling around an internal Metal compiler failure. MoltenVK reads
    // these variables during its first Vulkan initialization, so this must run
    // before boxedmain() loads the guest Vulkan driver.
    struct EnvironmentSetting {
        const char* name;
        const char* value;
    };
    constexpr EnvironmentSetting settings[] = {
        {"MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0"},
        {"MVK_CONFIG_METAL_COMPILE_TIMEOUT", "20000000000"},
    };
    for (const EnvironmentSetting& setting : settings) {
        if (setenv(setting.name, setting.value, 1) != 0) {
            error = std::string("Could not configure MoltenVK setting ") +
                setting.name + ".";
            return false;
        }
    }
    BVNLogWrite(BVNLogLevelInfo, "vulkan",
                "MoltenVK WineD3D policy: Metal argument buffers disabled; "
                "Metal library/function/pipeline compilation bounded at 20 "
                "seconds.");
    return true;
}

// Runs one guest session on the main thread.  Returns when the guest exits.
void runSession(const BVNLaunchConfiguration& launch) {
    std::string error;
    if (!createDirectories(launch.writableRootPath, error)) {
        setLastError(error);
        setState(BVNRuntimeStateFailed);
        return;
    }

    // The base Wine prefix lives inside the read-only rootfs ZIP. Copy only
    // its registry files into this game's writable overlay, then apply the
    // iOS compatibility policy before wineserver reads them. Imported games
    // use WineD3D's Vulkan backend. That reaches Boxedwine's generated Vulkan
    // bridge and iOS MoltenVK/Metal without requiring the unavailable GLX
    // backend. Do not overlay the rootfs's upstream DXVK 2.5.2 D3D9 DLL: its
    // baseline requires geometry shaders and transform feedback, which
    // MoltenVK does not expose. The unsupported Bluetooth root device is
    // disconnected for every prefix. Winebus remains enabled: build 24 proved
    // removing it did not reduce the cold-start gap and it will be needed for
    // future controllers.
    // Setting a service to disabled alone is insufficient because Wine 10
    // auto-starts associated root PnP services regardless of Start.
    if (launch.runThroughWine) {
        const boxedvn::WineRenderer renderer =
            launch.useWineD3DVulkanRenderer
            ? boxedvn::WineRenderer::Vulkan
            : boxedvn::WineRenderer::Default;
        const boxedvn::WinePrefixPreparationResult prefix =
            boxedvn::prepareWinePrefix(launch.rootFilesystemZipPath,
                                       launch.writableRootPath,
                                       renderer);
        if (!prefix.ok) {
            setLastError("Could not prepare the writable Wine prefix: " +
                         prefix.error);
            BVNLogWrite(BVNLogLevelError, "prefix", prefix.error.c_str());
            setState(BVNRuntimeStateFailed);
            return;
        }
        const char* prefixMessage = nullptr;
        if (launch.useWineD3DVulkanRenderer) {
            prefixMessage = launch.enableWineD3DVulkan
                ? "Wine prefix ready: unsupported NDIS and Bluetooth "
                  "drivers disabled; stale GDI/OpenGL policy repaired; "
                  "imported game uses DXVK through Boxedwine and iOS "
                  "MoltenVK/Metal."
                : "Wine prefix ready: unsupported NDIS and Bluetooth "
                  "drivers disabled; stale GDI/OpenGL policy repaired; "
                  "imported game uses WineD3D's Vulkan renderer through "
                  "Boxedwine and iOS MoltenVK/Metal; DXVK is disabled.";
        } else {
            prefixMessage =
                "Wine prefix ready: unsupported NDIS and Bluetooth "
                "drivers disabled; Wine HID bus and nsiproxy available.";
        }
        BVNLogWrite(BVNLogLevelInfo, "prefix", prefixMessage);

        // Make the user's own fonts reachable by the guest. A guest that can
        // find no font at all is not a cosmetic problem: a Chromium-family
        // game stops at a CHECK in Blink's FontCache when its font collection
        // comes back empty, which ends the whole session, and a Japanese
        // title with no CJK face draws mojibake that then gets misread as a
        // renderer fault. A failure here is reported and does not stop the
        // launch - the guest may well have everything it needs already, and
        // refusing to start it would turn a possible problem into a certain
        // one.
        if (const char* fontsDirectory = BVNPathFonts()) {
            const boxedvn::GuestFontInstallResult fonts =
                boxedvn::installGuestFonts(fontsDirectory,
                                           launch.writableRootPath);
            if (!fonts.ok) {
                BVNLogWrite(BVNLogLevelError, "fonts", fonts.error.c_str());
            } else if (fonts.available == 0) {
                BVNLogWrite(BVNLogLevelInfo, "fonts",
                            "No user fonts supplied. The guest sees only what "
                            "the root filesystem provides; if a game reports "
                            "an empty font collection or draws mojibake, put "
                            ".ttf/.ttc/.otf files in Documents/Fonts through "
                            "the Files app.");
            } else {
                char message[192];
                snprintf(message, sizeof(message),
                         "User fonts: %zu supplied, %zu newly copied into "
                         "drive_c/windows/Fonts.",
                         fonts.available, fonts.installed);
                BVNLogWrite(BVNLogLevelInfo, "fonts", message);
            }
        }

        // Shadow the rootfs's stock DXVK with the MoltenVK-compatible build.
        // Upstream DXVK requires geometryShader and VK_EXT_transform_feedback
        // and therefore refuses to create a device on Metal at all; the
        // bundled modules treat those as optional. Only d3d11/dxgi/d3d10core
        // are replaced, so d3d8/d3d9 continue to come from the archive.
        if (launch.enableWineD3DVulkan) {
            const char* bundledDxvk = BVNPathBundledDxvkDirectory();
            if (bundledDxvk == nullptr) {
                BVNLogWrite(BVNLogLevelError, "prefix",
                            "This build ships no patched DXVK, so Direct3D 11 "
                            "titles cannot render. Rebuild with ios/app/Dxvk "
                            "populated.");
            } else {
                bool installed = false;
                std::string dxvkError;
                if (!boxedvn::installBundledDxvk(bundledDxvk,
                                                 launch.writableRootPath,
                                                 installed, dxvkError)) {
                    setLastError("Could not install DXVK: " + dxvkError);
                    BVNLogWrite(BVNLogLevelError, "prefix", dxvkError.c_str());
                    setState(BVNRuntimeStateFailed);
                    return;
                }
                BVNLogWrite(BVNLogLevelInfo, "prefix",
                            installed
                                ? "Installed the MoltenVK-compatible DXVK "
                                  "modules into drive_c/dxvk."
                                : "MoltenVK-compatible DXVK already present in "
                                  "drive_c/dxvk.");
            }
        }
    }

    if (launch.enableWineD3DVulkan &&
        !configureMoltenVKForWineD3D(error)) {
        setLastError(error);
        BVNLogWrite(BVNLogLevelError, "vulkan", error.c_str());
        setState(BVNRuntimeStateFailed);
        return;
    }

    // probeJitWithTimeout(), not a direct call to BVNJITProbeExecute(): this
    // is the one place in the app where actually confirming JIT works is
    // worth the risk of a crash if it does not, because the user just
    // pressed Launch/Run Wine Notepad and Boxedwine's real JIT is about to
    // hit the identical risk moments later regardless. See BVNRuntime.h.
    // The timeout wrapper exists because this call runs on the main thread -
    // the only thread servicing UIKit's run loop - and a hang inside it, not
    // just a crash, is a real possibility on iOS; see probeJitWithTimeout().
    const BVNJITReport jit = probeJitWithTimeout();
    BVNLogWrite(jit.status == BVNJITStatusAvailable ? BVNLogLevelInfo
                                                    : BVNLogLevelWarning,
                "jit", jit.detail);
    if (jit.status != BVNJITStatusAvailable) {
        setLastError(
            std::string("JIT is not available, so the guest was not started. ") +
            jit.detail);
        setState(BVNRuntimeStateFailed);
        return;
    }

    const std::vector<std::string> argumentStrings =
        BVNBuildLaunchArguments(launch);
    logLaunch(argumentStrings);

    std::vector<const char*> argv;
    argv.reserve(argumentStrings.size() + 1);
    for (const std::string& argument : argumentStrings) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    setState(BVNRuntimeStateRunning);
    BVNFrontendHideLibrary();

    // boxedmain owns the main thread until the guest exits.  It runs SDL's
    // event loop internally; guest CPU threads are pthreads created by
    // Boxedwine's KNativeThread.
    const int exitCode =
        boxedmain(static_cast<int>(argumentStrings.size()), argv.data());

    BVNFrontendShowLibrary();

    pthread_mutex_lock(&gMutex);
    gLastExitCode = exitCode;
    gShutdownRequested = false;
    pthread_mutex_unlock(&gMutex);

    BVNLogWrite(BVNLogLevelInfo, "runtime",
                (std::string("guest exited with code ") +
                 std::to_string(exitCode)).c_str());
    setState(BVNRuntimeStateStopped);
}

}  // namespace

extern "C" const char* BVNRuntimeStateName(BVNRuntimeState state) {
    switch (state) {
        case BVNRuntimeStateIdle:     return "idle";
        case BVNRuntimeStateStarting: return "starting";
        case BVNRuntimeStateRunning:  return "running";
        case BVNRuntimeStateStopping: return "stopping";
        case BVNRuntimeStateStopped:  return "stopped";
        case BVNRuntimeStateFailed:   return "failed";
    }
    return "unknown";
}

extern "C" BVNRuntimeState BVNRuntimeGetState(void) {
    pthread_mutex_lock(&gMutex);
    const BVNRuntimeState state = gState;
    pthread_mutex_unlock(&gMutex);
    return state;
}

extern "C" int32_t BVNRuntimeLastExitCode(void) {
    pthread_mutex_lock(&gMutex);
    const int32_t code = gLastExitCode;
    pthread_mutex_unlock(&gMutex);
    return code;
}

extern "C" const char* BVNRuntimeLastError(void) {
    // The string is only replaced under the mutex and never freed, so handing
    // out the pointer is safe for a reader that copies it promptly.
    static std::string snapshot;
    pthread_mutex_lock(&gMutex);
    snapshot = gLastError;
    pthread_mutex_unlock(&gMutex);
    return snapshot.c_str();
}

extern "C" void BVNRuntimeNotifyFrontendReady(void) {
    pthread_mutex_lock(&gMutex);
    gFrontendReady = true;
    pthread_mutex_unlock(&gMutex);
    BVNLogWrite(BVNLogLevelInfo, "runtime", "frontend ready");
}

extern "C" bool BVNRuntimeRequestLaunch(const BVNLaunchRequest* request,
                                        char* errorBuffer,
                                        size_t errorBufferSize) {
    std::string error;
    pthread_mutex_lock(&gMutex);
    const bool accepted = acceptLaunchLocked(request, error);
    pthread_mutex_unlock(&gMutex);

    if (!accepted) {
        copyString(errorBuffer, errorBufferSize, error);
        setLastError(error);
        return false;
    }
    copyString(errorBuffer, errorBufferSize, "");
    BVNLogWrite(BVNLogLevelInfo, "runtime", "launch request accepted");

    // The session runs on the main thread, as SDL's UIKit backend requires,
    // but it is started from a queued block rather than from BVNGuestMain's
    // old polling loop (see BVNGuestMain for why that loop is gone). Queuing
    // it also means this function returns immediately even when called from
    // the main thread inside a SwiftUI button handler - that handler unwinds
    // first, and boxedmain() then takes over the main thread cleanly.
    dispatch_async(dispatch_get_main_queue(), ^{
        BVNLaunchConfiguration launch;
        bool shouldLaunch = false;

        pthread_mutex_lock(&gMutex);
        if (gHasPendingLaunch && gFrontendReady) {
            launch = std::move(gPendingLaunch);
            gPendingLaunch = BVNLaunchConfiguration();
            gHasPendingLaunch = false;
            shouldLaunch = true;
        }
        pthread_mutex_unlock(&gMutex);

        if (!shouldLaunch) {
            return;
        }

        // Rotate the UIKit scene *before* SDL creates its UIWindow/CAMetalLayer.
        // Creating the guest while the portrait-to-landscape transition was
        // still in flight produced a portrait first drawable, then replaced
        // it underneath Boxedwine's blocking main-thread loop. The visible
        // results were a stretched loading screen, frozen input after a
        // rotation, and a missing software keyboard.
        BVNPrepareGuestPresentation(^{
            // SDL disables its UIKit event pump when the function it was given
            // (BVNGuestMain) returns - see -[SDLUIKitDelegate postFinishLaunch],
            // which brackets that call with SDL_iPhoneSetEventPump TRUE/FALSE.
            // Since BVNGuestMain now returns immediately so the real UIKit run
            // loop can service the library UI, the pump has to be turned back
            // on here or SDL_PumpEvents would be a no-op for the whole session.
            SDL_iPhoneSetEventPump(SDL_TRUE);
            runSession(launch);
            SDL_iPhoneSetEventPump(SDL_FALSE);
            BVNFinishGuestPresentation();
        });
    });
    return true;
}

extern "C" bool BVNRuntimeRequestShutdown(void) {
    pthread_mutex_lock(&gMutex);
    const bool running = (gState == BVNRuntimeStateRunning);
    if (running) {
        gState = BVNRuntimeStateStopping;
        gShutdownRequested = true;
    }
    pthread_mutex_unlock(&gMutex);

    if (!running) {
        return false;
    }

    BVNLogWrite(BVNLogLevelInfo, "runtime", "shutdown requested; posting SDL_QUIT");

    // SDL_PushEvent is documented as thread safe and is the supported way to
    // ask an SDL application to quit from another thread.  Boxedwine's event
    // loop turns SDL_QUIT into a clean guest shutdown.
    SDL_Event quit;
    SDL_zero(quit);
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    return true;
}

extern "C" int BVNGuestMain(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    BVNLogStartSessionFile();
    BVNLogWrite(BVNLogLevelInfo, "runtime",
                (std::string("BoxedVN starting; Boxedwine core ") +
                 BVNRuntimeBoxedwineVersion()).c_str());

    // Safe status check only: this runs unconditionally on every launch, and
    // BVNJITProbeExecute() can crash the process with no recovery possible -
    // see BVNRuntime.h. This is exactly the mistake that made every cold
    // launch crash before it was split out.
    const BVNJITReport startupJIT = BVNJITProbeStatus();
    BVNLogWrite(startupJIT.status == BVNJITStatusLikelyAvailable
                    ? BVNLogLevelInfo
                    : BVNLogLevelWarning,
                "jit", startupJIT.detail);

    // Take the arena now, while StikDebug is still there.
    //
    // StikDebug's script session ends on its own, and it used to be the guest
    // launch - minutes later, after the user had browsed their library -
    // that first asked for executable memory. By then the request often could
    // not be serviced, so the game would not start and the only cure was to
    // restart the app through StikDebug and launch something immediately.
    // Preparation is the half with the deadline; it executes nothing, so it
    // adds no crash path to app startup. The execution test stays at guest
    // launch, unchanged.
    //
    // Only when the process is actually flagged CS_DEBUGGED: without that
    // there is no debugger to answer, and issuing the breakpoint anyway would
    // turn "JIT is unavailable" into a SIGTRAP during startup.
    if (startupJIT.status == BVNJITStatusLikelyAvailable) {
        prepareJitArenaWithTimeout();
    }

    setState(BVNRuntimeStateIdle);

    // Return, rather than spinning a custom run loop pump forever.
    //
    // This used to be a `while (true)` loop calling
    // CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true) to keep the
    // SwiftUI library UI alive while waiting for a launch request. That
    // worked for simple SwiftUI interaction but subtly broke anything that
    // depends on run loop modes the pump never serviced - most visibly
    // UIDocumentPickerViewController (what .fileImporter presents): its file
    // list rendered correctly, but tapping a row did nothing, because
    // UIScrollView's touch delivery depends on machinery scheduled in
    // UITrackingRunLoopMode, which a default-mode-only pump never runs. SDL's
    // own UIKit_PumpEvents pumps both modes for exactly this reason (see
    // SDL_uikitevents.m, "Make sure UIScrollView objects scroll properly"),
    // and BoxedVN's pump had silently deviated from it.
    //
    // Adding the missing mode would patch that one symptom, but the whole
    // custom-pump approach is the actual liability: any UIKit facility
    // relying on a mode or source the pump doesn't happen to service breaks
    // in a way that is very hard to attribute. SDL explicitly supports
    // returning from the supplied main function - see the comment in
    // -[SDLUIKitDelegate postFinishLaunch] ("We don't actually exit to
    // support applications that do setup in their main function and then
    // allow the Cocoa event loop to run") - so returning here hands the main
    // thread back to the real, unmodified UIKit run loop. While idle BoxedVN
    // is now an entirely ordinary iOS app, and UIKit behaves accordingly.
    //
    // Guest sessions still run on the main thread, started from a main-queue
    // block by BVNRuntimeRequestLaunch; boxedmain() blocks it for the
    // session's duration and SDL's own correct event pump takes over then.
    return 0;
}
