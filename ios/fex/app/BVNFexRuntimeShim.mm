/*
 *  BoxedVN fex64 - the small part of the runtime that BVNExecMemory needs.
 *  Copyright (C) 2026  The BoxedWine Team.  GPLv2; see license.txt.
 *
 *  BVNExecMemory is shared with the emulator application unchanged, and it
 *  calls three things from the wider runtime: a log sink, a memory report, and
 *  a progress callback for the loading screen. Pulling in the real
 *  implementations would drag in the emulator's paths, settings and UI, none
 *  of which exist on this branch.
 *
 *  So they are provided here instead. Copying BVNExecMemory itself was the
 *  alternative, and a second copy of the one piece of this project that is
 *  genuinely delicate - and genuinely device-proven - is not worth saving
 *  forty lines.
 */

#import <Foundation/Foundation.h>

#include "BVNRuntime.h"

#include <mach/mach.h>
#include <os/log.h>
#include <os/proc.h>

#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex g_logMutex;
std::vector<std::string> g_log;

const char* levelName(BVNLogLevel level) {
    switch (level) {
        case BVNLogLevelDebug: return "debug";
        case BVNLogLevelInfo: return "info";
        case BVNLogLevelWarning: return "warn";
        case BVNLogLevelError: return "error";
    }
    return "?";
}
} // namespace

extern "C" void BVNLogWrite(BVNLogLevel level, const char* category,
                            const char* message) {
    if (message == nullptr) {
        return;
    }
    const char* safeCategory = category != nullptr ? category : "-";

    os_log(OS_LOG_DEFAULT, "BOXEDVN [%{public}s] %{public}s: %{public}s",
           levelName(level), safeCategory, message);

    std::lock_guard<std::mutex> guard(g_logMutex);
    g_log.push_back(std::string("[") + safeCategory + "] " + message);
    // The arena writes a line per prepared segment, so this is bounded rather
    // than trusted to stay small.
    if (g_log.size() > 2000) {
        g_log.erase(g_log.begin(), g_log.begin() + 1000);
    }
}

extern "C" const char* BVNFexRuntimeLogText(void) {
    static std::string joined;
    std::lock_guard<std::mutex> guard(g_logMutex);
    joined.clear();
    for (const auto& line : g_log) {
        joined.append(line);
        joined.append("\n");
    }
    return joined.c_str();
}

extern "C" BVNMemoryReport BVNMemoryProbe(void) {
    BVNMemoryReport report {};

    task_vm_info_data_t info {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        report.processResidentBytes = info.phys_footprint;
    }
    report.physicalMemoryBytes = [NSProcessInfo processInfo].physicalMemory;
    report.availableBytes = os_proc_available_memory();
    report.increasedMemoryLimit = BVNMemoryEntitlementUnknown;
    report.detail = "fex64 shim";
    return report;
}

extern "C" void BVNGuestLoadingUpdateJITProgress(size_t allocationCount) {
    // No loading screen on this branch yet. The arena reports its own totals
    // through BVNExecMemArenaStatus, which is what the interface reads.
    (void)allocationCount;
}
