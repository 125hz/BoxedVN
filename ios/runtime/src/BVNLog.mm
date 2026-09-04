#include <algorithm>
#include <cstdio>
#include <ctime>
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "BVNRuntime.h"
#include "BVNFEXBackend.h"

// Implemented in the SDL/core translation unit, which already includes
// boxedwine.h in the order its internal headers require. Pulling ksystem.h
// directly into this Objective-C++ bridge makes Foundation's NSString a
// misleading fallback for Boxedwine's not-yet-declared BString.
extern "C" void BVNGuestLogThreadSnapshot(const char* reason);

namespace {

// A bounded in-memory tail, so the log viewer can render without reading a
// file that a running guest is still appending to.
constexpr size_t kRingCapacity = 512 * 1024;

pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
std::string gRing;
std::string gLogPath;
int gLogFileDescriptor = -1;

// Session logging can be turned off from Settings.
//
// It is on by default and should stay that way: every diagnosis in this port
// so far has come from a log the player exported. But the emulator writes from
// hot paths - the Vulkan bridge, the syscall layer - and a player who is just
// trying to read a novel is paying for evidence nobody is going to look at.
std::atomic<bool> gLoggingEnabled{true};
std::atomic<uint64_t> gGeneration{0};
os_log_t gOSLog = nullptr;

// The opt-in browser boot probe normally prints every three seconds. If its
// timer stops, the renderer's JavaScript thread is no longer servicing its
// event loop even though Chromium's other processes may continue producing
// log output. Capture that moment from the emulator core instead of trying to
// infer it later from a screenshot.
std::atomic<uint64_t> gBootHeartbeatSequence{0};
std::atomic<uint64_t> gBootHeartbeatMicroseconds{0};
std::atomic<uint64_t> gBootSnapshotSequence{0};
std::atomic<bool> gBootWatchStarted{false};

uint64_t monotonicMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void observeBootHeartbeat() {
    gBootHeartbeatMicroseconds.store(monotonicMicroseconds(),
                                     std::memory_order_relaxed);
    gBootHeartbeatSequence.fetch_add(1, std::memory_order_release);
}

void startBootHeartbeatWatch() {
    if (gBootWatchStarted.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::thread([] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // Unlike Swift/UIKit timers, this watcher remains alive while
            // boxedmain owns the main thread. Sample translated execution even
            // when FEX is reusing linked blocks and emits no compile messages.
            BVNFEXBackendPollExecutionTrace();
            const uint64_t sequence =
                gBootHeartbeatSequence.load(std::memory_order_acquire);
            if (sequence == 0 || sequence ==
                    gBootSnapshotSequence.load(std::memory_order_relaxed)) {
                continue;
            }
            const uint64_t heartbeat = gBootHeartbeatMicroseconds.load(
                std::memory_order_relaxed);
            const uint64_t now = monotonicMicroseconds();
            if (now < heartbeat || now - heartbeat < 10'000'000 ||
                gBootHeartbeatSequence.load(std::memory_order_acquire) !=
                    sequence) {
                continue;
            }

            // Do not let a heartbeat from a guest that just exited produce a
            // misleading snapshot of the next library screen or session.
            if (BVNRuntimeGetState() != BVNRuntimeStateRunning) {
                gBootSnapshotSequence.store(sequence,
                                            std::memory_order_relaxed);
                continue;
            }

            gBootSnapshotSequence.store(sequence, std::memory_order_relaxed);
            BVNLogWrite(BVNLogLevelWarning, "diagnostics",
                        "Browser boot heartbeat stopped for 10 seconds; "
                        "capturing every guest thread and live EIP once.");
            BVNGuestLogThreadSnapshot(
                "browser boot heartbeat stopped for 10 seconds");
        }
    }).detach();
}

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
    if (!gLoggingEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    pthread_mutex_lock(&gMutex);
    appendToRingLocked(text, length);
    if (gLogFileDescriptor >= 0) {
        ssize_t ignored = write(gLogFileDescriptor, text, length);
        (void)ignored;
    }
    pthread_mutex_unlock(&gMutex);
}

// Flood limiter. A guest that spins through the same few lines (a thread
// re-entering one instruction, a translator message per re-entry) wrote a
// 190 MB session log in a minute. Each distinct line body gets an allowance
// per window; beyond it the line is dropped and counted, and the count is
// written once when the window rolls over. Timestamps are skipped so
// BoxedVN's own stamped lines dedupe like the emulator's unstamped ones.
struct FloodSlot {
    uint64_t hash = 0;
    uint32_t seen = 0;
    uint32_t suppressed = 0;
    char sample[100] = {0};
};
constexpr size_t kFloodSlots = 64;
constexpr uint32_t kFloodAllowance = 24;
constexpr double kFloodWindowSeconds = 2.0;
FloodSlot gFloodSlots[kFloodSlots];
double gFloodWindowStart = 0.0;
pthread_mutex_t gFloodMutex = PTHREAD_MUTEX_INITIALIZER;

double floodNow() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

uint64_t floodHash(const char* text, size_t length) {
    size_t start = 0;
    if (length > 24 && text[4] == '-' && text[7] == '-' && text[10] == ' ' &&
        text[13] == ':' && text[16] == ':') {
        start = 24; // "YYYY-MM-DD HH:MM:SS.mmm "
    }
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = start; i < length; ++i) {
        hash ^= (uint8_t)text[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Wine's relay trace is exempt from the allowance above.
//
// Turning relay on is the only way this platform can see a guest's Windows
// API calls, and those lines are repetitive by their nature: a startup check
// that loops over a table writes one near-identical line per iteration, and a
// call made with the same arguments from the same site writes a line that is
// identical byte for byte. The allowance would therefore drop exactly the run
// that was worth turning the trace on for, and would drop it silently apart
// from a count.
//
// Exempt before a slot is claimed, not after, so relay output cannot occupy
// all sixty-four slots and leave a genuine flood of something else unlimited.
//
// Recognised by the shape Wine writes and only that shape: a hexadecimal
// thread id, a colon, then "Call " or "Ret  ". Both may sit behind BoxedVN's
// timestamp or behind the "[guest fd=2 pid=N exe]" prefix sys_write64 adds,
// so the whole head of the line is searched rather than its first column.
// Nothing else in this project writes that pattern, and a line without it is
// limited exactly as before.
bool floodExemptRelay(const char* text, size_t length) {
    const size_t limit = length < 128 ? length : 128;
    for (size_t i = 1; i + 6 <= limit; ++i) {
        if (text[i] != ':' || isxdigit((unsigned char)text[i - 1]) == 0) {
            continue;
        }
        if (memcmp(text + i + 1, "Call ", 5) == 0 ||
            memcmp(text + i + 1, "Ret  ", 5) == 0) {
            return true;
        }
    }
    return false;
}

// True when the line should reach the sinks. Writes the previous window's
// summaries itself. Caller must NOT hold gMutex.
bool floodAdmit(const char* text, size_t length) {
    const uint64_t hash = floodHash(text, length);
    std::string summaries;
    pthread_mutex_lock(&gFloodMutex);
    const double now = floodNow();
    if (now - gFloodWindowStart >= kFloodWindowSeconds) {
        for (FloodSlot& slot : gFloodSlots) {
            if (slot.suppressed > 0) {
                char line[192];
                const int written = snprintf(
                    line, sizeof(line), "BOXEDVN_LOG_SUPPRESSED repeated=%u: %s\n",
                    slot.suppressed,
                    slot.sample[0] != 0 ? slot.sample : "(blank line)");
                if (written > 0) {
                    summaries.append(line, (size_t)std::min<int>(written, (int)sizeof(line) - 1));
                }
            }
            slot = FloodSlot();
        }
        gFloodWindowStart = now;
    }
    if (floodExemptRelay(text, length)) {
        pthread_mutex_unlock(&gFloodMutex);
        if (!summaries.empty()) {
            writeToSinks(summaries.data(), summaries.size());
        }
        return true;
    }
    FloodSlot* match = nullptr;
    FloodSlot* empty = nullptr;
    for (FloodSlot& slot : gFloodSlots) {
        if (slot.seen != 0 && slot.hash == hash) {
            match = &slot;
            break;
        }
        if (empty == nullptr && slot.seen == 0) {
            empty = &slot;
        }
    }
    bool admit = true;
    if (match == nullptr) {
        if (empty != nullptr) {
            empty->hash = hash;
            empty->seen = 1;
            empty->suppressed = 0;
            size_t copied = 0;
            for (size_t i = 0; i < length && copied + 1 < sizeof(empty->sample); ++i) {
                if (text[i] == '\n' || text[i] == '\r') break;
                empty->sample[copied++] = text[i];
            }
            empty->sample[copied] = 0;
        }
    } else {
        match->seen += 1;
        if (match->seen > kFloodAllowance) {
            match->suppressed += 1;
            admit = false;
        }
    }
    pthread_mutex_unlock(&gFloodMutex);
    if (!summaries.empty()) {
        writeToSinks(summaries.data(), summaries.size());
    }
    return admit;
}

int gCapturePipe[2] = {-1, -1};

void* captureThread(void* /*context*/) {
    pthread_setname_np("BoxedVN log capture");
    char buffer[4096];
    std::string pendingLine;
    while (true) {
        const ssize_t count = read(gCapturePipe[0], buffer, sizeof(buffer));
        if (count > 0) {
            pendingLine.append(buffer, static_cast<size_t>(count));
            std::size_t newline = 0;
            while ((newline = pendingLine.find('\n')) != std::string::npos) {
                // Whole lines reach the sinks, each through the flood
                // limiter; a partial line waits for its newline.
                const std::string line = pendingLine.substr(0, newline + 1);
                if (line.find("BOXEDVN boot ") != std::string::npos) {
                    observeBootHeartbeat();
                }
                if (floodAdmit(line.data(), line.size())) {
                    writeToSinks(line.data(), line.size());
                }
                pendingLine.erase(0, newline + 1);
            }
            if (pendingLine.size() > 64 * 1024) {
                // A runaway unterminated line is flushed rather than held.
                writeToSinks(pendingLine.data(), pendingLine.size());
                pendingLine.clear();
            }
            // A guest can write an arbitrarily long unterminated line. Keep
            // enough tail to recognise the marker across reads without
            // allowing diagnostics themselves to grow without bound.
            if (pendingLine.size() > 8192) {
                pendingLine.erase(0, pendingLine.size() - 8192);
            }
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
    startBootHeartbeatWatch();

    pthread_mutex_unlock(&gMutex);
    return true;
}

extern "C" void BVNLogSetEnabled(bool enabled) {
    const bool was = gLoggingEnabled.exchange(enabled,
                                              std::memory_order_relaxed);
    if (was == enabled) {
        return;
    }
    if (enabled) {
        // Say so through the sink that just came back, so a log that resumes
        // mid-session explains its own gap.
        BVNLogWrite(BVNLogLevelInfo, "app",
                    "Session logging re-enabled from Settings.");
    } else {
        BVNLogWrite(BVNLogLevelInfo, "app",
                    "Session logging disabled from Settings; nothing further "
                    "will be recorded until it is turned back on.");
        gLoggingEnabled.store(false, std::memory_order_relaxed);
    }
}

extern "C" bool BVNLogIsEnabled(void) {
    return gLoggingEnabled.load(std::memory_order_relaxed);
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
    if (floodAdmit(line.data(), line.size())) {
        writeToSinks(line.data(), line.size());
    }

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
