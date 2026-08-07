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
 *  Session logging.
 *
 *  Boxedwine writes diagnostics with klog()/kwarn()/kpanic(), which go to
 *  stdout and stderr, and additionally to KSystem::logFile when the emulator
 *  is started with -log.  On iOS neither stdout nor stderr goes anywhere a
 *  user can reach.
 *
 *  So BoxedVN redirects the process's stdout and stderr into a pipe and reads
 *  that pipe on a dedicated thread, appending everything to both the session
 *  log file and an in-memory tail that the log viewer renders.  That captures
 *  real emulator output - including whatever a kpanic prints on its way out -
 *  rather than a summary of it.  BVNLogWrite adds BoxedVN's own structured
 *  entries to the same two sinks, so the frontend and the emulator appear
 *  interleaved in one ordered stream.
 *
 *  Bytes still in flight in the pipe when the process exits are lost.  Every
 *  write to the log file is unbuffered, so the window is one read() wide.
 *  ---------------------------------------------------------------------
 */

#import <Foundation/Foundation.h>
#import <os/log.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <string>

#include "BVNRuntime.h"

namespace {

// A bounded in-memory tail, so the log viewer can render without reading a
// file that a running guest is still appending to.
constexpr size_t kRingCapacity = 512 * 1024;

pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
std::string gRing;
std::string gLogPath;
int gLogFileDescriptor = -1;
std::atomic<uint64_t> gGeneration{0};
os_log_t gOSLog = nullptr;

const char* levelName(BVNLogLevel level) {
    switch (level) {
        case BVNLogLevelDebug:   return "DEBUG";
        case BVNLogLevelInfo:    return "INFO ";
        case BVNLogLevelWarning: return "WARN ";
        case BVNLogLevelError:   return "ERROR";
    }
    return "INFO ";
}

// Caller must hold gMutex.
void appendToRingLocked(const char* text, size_t length) {
    gRing.append(text, length);
    if (gRing.size() > kRingCapacity) {
        // Drop from the front, but only on a line boundary so the viewer never
        // shows a half line.
        const size_t excess = gRing.size() - kRingCapacity;
        size_t cut = gRing.find('\n', excess);
        cut = (cut == std::string::npos) ? excess : cut + 1;
        gRing.erase(0, cut);
    }
    gGeneration.fetch_add(1, std::memory_order_relaxed);
}

// Caller must NOT hold gMutex.
void writeToSinks(const char* text, size_t length) {
    pthread_mutex_lock(&gMutex);
    appendToRingLocked(text, length);
    if (gLogFileDescriptor >= 0) {
        ssize_t ignored = write(gLogFileDescriptor, text, length);
        (void)ignored;
    }
    pthread_mutex_unlock(&gMutex);
}

int gCapturePipe[2] = {-1, -1};

void* captureThread(void* /*context*/) {
    pthread_setname_np("BoxedVN log capture");
    char buffer[4096];
    while (true) {
        const ssize_t count = read(gCapturePipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            writeToSinks(buffer, static_cast<size_t>(count));
        } else if (count == 0) {
            break;  // write end closed
        } else if (errno != EINTR) {
            break;
        }
    }
    return nullptr;
}

std::string timestamp() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm parts;
    localtime_r(&ts.tv_sec, &parts);
    char buffer[32];
    const size_t used =
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
    snprintf(buffer + used, sizeof(buffer) - used, ".%03d",
             static_cast<int>(ts.tv_nsec / 1000000));
    return std::string(buffer);
}

}  // namespace

// Opens the session log and redirects stdout/stderr into it.  Called once, by
// BVNGuestMain, before anything else logs.
extern "C" bool BVNLogStartSessionFile(void) {
    pthread_mutex_lock(&gMutex);
    if (gLogFileDescriptor >= 0) {
        pthread_mutex_unlock(&gMutex);
        return true;
    }

    if (gOSLog == nullptr) {
        gOSLog = os_log_create("org.boxedwine.boxedvn", "runtime");
    }

    const char* logsDirectory = BVNPathLogs();
    if (logsDirectory == nullptr) {
        pthread_mutex_unlock(&gMutex);
        return false;
    }

    char name[64];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm parts;
    localtime_r(&ts.tv_sec, &parts);
    strftime(name, sizeof(name), "boxedvn-%Y%m%d-%H%M%S.log", &parts);

    gLogPath = std::string(logsDirectory) + "/" + name;

    gLogFileDescriptor =
        open(gLogPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (gLogFileDescriptor < 0) {
        const int openErrno = errno;
        os_log_error(gOSLog, "BoxedVN could not open the session log %{public}s: %{public}s",
                     gLogPath.c_str(), strerror(openErrno));
        gLogPath.clear();
        pthread_mutex_unlock(&gMutex);
        return false;
    }

    // Route stdout and stderr through a pipe so the capture thread can fan
    // the emulator's output out to both the file and the in-memory tail.
    if (pipe(gCapturePipe) != 0) {
        const int pipeErrno = errno;
        os_log_error(gOSLog,
                     "BoxedVN could not create the log capture pipe: %{public}s",
                     strerror(pipeErrno));
        // The file is still usable; only the live viewer loses emulator output.
        dup2(gLogFileDescriptor, STDOUT_FILENO);
        dup2(gLogFileDescriptor, STDERR_FILENO);
    } else {
        dup2(gCapturePipe[1], STDOUT_FILENO);
        dup2(gCapturePipe[1], STDERR_FILENO);
        pthread_t thread;
        if (pthread_create(&thread, nullptr, captureThread, nullptr) == 0) {
            pthread_detach(thread);
        }
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    pthread_mutex_unlock(&gMutex);
    return true;
}

extern "C" void BVNLogWrite(BVNLogLevel level, const char* category,
                            const char* message) {
    if (message == nullptr) {
        message = "";
    }
    if (category == nullptr) {
        category = "runtime";
    }

    const std::string line =
        timestamp() + " [" + levelName(level) + "] " + category + ": " +
        message + "\n";

    // Written to the sinks directly rather than through stdout, so BoxedVN's
    // own entries do not depend on the capture pipe being healthy.
    writeToSinks(line.data(), line.size());

    pthread_mutex_lock(&gMutex);
    os_log_t osLog = gOSLog;
    pthread_mutex_unlock(&gMutex);

    if (osLog != nullptr) {
        switch (level) {
            case BVNLogLevelError:
                os_log_error(osLog, "%{public}s: %{public}s", category, message);
                break;
            case BVNLogLevelWarning:
                os_log(osLog, "%{public}s: %{public}s", category, message);
                break;
            default:
                os_log_debug(osLog, "%{public}s: %{public}s", category, message);
                break;
        }
    }
}

extern "C" const char* BVNLogCurrentFilePath(void) {
    pthread_mutex_lock(&gMutex);
    const char* result = gLogPath.empty() ? nullptr : gLogPath.c_str();
    pthread_mutex_unlock(&gMutex);
    return result;
}

extern "C" size_t BVNLogCopyRecent(char* buffer, size_t bufferSize) {
    pthread_mutex_lock(&gMutex);
    const size_t available = gRing.size();
    if (buffer == nullptr || bufferSize == 0) {
        pthread_mutex_unlock(&gMutex);
        return available;
    }
    const size_t copied =
        (available < bufferSize - 1) ? available : bufferSize - 1;
    // Keep the newest text when the caller's buffer is too small.
    memcpy(buffer, gRing.data() + (available - copied), copied);
    buffer[copied] = '\0';
    pthread_mutex_unlock(&gMutex);
    return copied;
}

extern "C" uint64_t BVNLogGeneration(void) {
    return gGeneration.load(std::memory_order_relaxed);
}
