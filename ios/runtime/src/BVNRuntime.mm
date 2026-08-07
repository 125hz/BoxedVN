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

#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

#include <SDL.h>

#include "BVNRuntime.h"

// Boxedwine's own entry point, from source/sdl/main.cpp.  Declared with C++
// linkage to match its definition there, exactly as platform/sdl/
// knativesystem.cpp declares it.
int boxedmain(int argc, const char** argv);

extern "C" bool BVNLogStartSessionFile(void);

// Implemented in BVNAppDelegate.mm.
extern "C" void BVNFrontendShowLibrary(void);
extern "C" void BVNFrontendHideLibrary(void);

namespace {

// A launch request, deep-copied out of the caller's BVNLaunchRequest so the
// frontend may free its strings the moment the call returns.
struct PendingLaunch {
    std::string rootFilesystemZipPath;
    std::string writableRootPath;
    std::string gameDirectoryHostPath;
    std::string executablePath;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    std::string workingDirectory;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitsPerPixel = 0;
    bool soundEnabled = true;
    bool runThroughWine = true;
};

pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
BVNRuntimeState gState = BVNRuntimeStateIdle;
bool gHasPendingLaunch = false;
PendingLaunch gPendingLaunch;
int32_t gLastExitCode = INT32_MIN;
std::string gLastError;
bool gFrontendReady = false;
bool gShutdownRequested = false;

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

// Builds the argv Boxedwine is started with.  Documented in commandLine.txt;
// every flag here corresponds to a real emulator option.
std::vector<std::string> buildArguments(const PendingLaunch& launch,
                                        const char* logPath) {
    std::vector<std::string> argv;
    argv.push_back("boxedvn");

    // Writable overlay for the emulated Linux filesystem.
    argv.push_back("-root");
    argv.push_back(launch.writableRootPath);

    // Read-only ZIP mounted at '/'.
    argv.push_back("-zip");
    argv.push_back(launch.rootFilesystemZipPath);

    // Boxedwine otherwise scans the working directory for a *Wine*.zip.
    argv.push_back("-nozip");
    argv.push_back("1");

    if (logPath != nullptr && logPath[0] != '\0') {
        argv.push_back("-log");
        argv.push_back(logPath);
    }

    if (!launch.gameDirectoryHostPath.empty()) {
        // The imported game directory appears as drive D: inside Wine.
        argv.push_back("-mount_drive");
        argv.push_back(launch.gameDirectoryHostPath);
        argv.push_back("d");
    }

    if (launch.width > 0 && launch.height > 0) {
        argv.push_back("-resolution");
        argv.push_back(std::to_string(launch.width) + "x" +
                       std::to_string(launch.height));
    }
    if (launch.bitsPerPixel == 16 || launch.bitsPerPixel == 32) {
        argv.push_back("-bpp");
        argv.push_back(std::to_string(launch.bitsPerPixel));
    }
    if (!launch.soundEnabled) {
        argv.push_back("-nosound");
    }
    if (!launch.workingDirectory.empty()) {
        argv.push_back("-w");
        argv.push_back(launch.workingDirectory);
    }
    for (const std::string& entry : launch.environment) {
        argv.push_back("-env");
        argv.push_back(entry);
    }

    // Everything from here on is the guest program and its arguments.
    if (launch.runThroughWine) {
        argv.push_back("/bin/wine");
    }
    argv.push_back(launch.executablePath);
    for (const std::string& argument : launch.arguments) {
        argv.push_back(argument);
    }
    return argv;
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

    PendingLaunch launch;
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

    gPendingLaunch = std::move(launch);
    gHasPendingLaunch = true;
    gState = BVNRuntimeStateStarting;
    return true;
}

// Runs one guest session on the main thread.  Returns when the guest exits.
void runSession(const PendingLaunch& launch) {
    std::string error;
    if (!createDirectories(launch.writableRootPath, error)) {
        setLastError(error);
        setState(BVNRuntimeStateFailed);
        return;
    }

    const BVNJITReport jit = BVNJITProbe();
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
        buildArguments(launch, BVNLogCurrentFilePath());
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

// Services the main run loop until something happens.  This is what keeps
// SwiftUI responsive while no guest is running.
void pumpMainRunLoop(CFTimeInterval seconds) {
    @autoreleasepool {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
    }
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

    const BVNJITReport startupJIT = BVNJITProbe();
    BVNLogWrite(startupJIT.status == BVNJITStatusAvailable ? BVNLogLevelInfo
                                                           : BVNLogLevelWarning,
                "jit", startupJIT.detail);

    setState(BVNRuntimeStateIdle);

    while (true) {
        PendingLaunch launch;
        bool shouldLaunch = false;

        pthread_mutex_lock(&gMutex);
        if (gHasPendingLaunch && gFrontendReady) {
            launch = std::move(gPendingLaunch);
            gPendingLaunch = PendingLaunch();
            gHasPendingLaunch = false;
            shouldLaunch = true;
        }
        pthread_mutex_unlock(&gMutex);

        if (shouldLaunch) {
            // Leaves the state at Stopped or Failed so the frontend can see
            // how the session ended; the loop keeps servicing the library UI
            // from either state and will accept the next launch request.
            runSession(launch);
            continue;
        }

        // 20 ms keeps the SwiftUI UI responsive without spinning the CPU.
        pumpMainRunLoop(0.02);
    }
}
