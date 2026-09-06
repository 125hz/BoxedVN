// Bounded metadata-only diagnostics for application data in the guest FS.
#pragma once
#include "bounded_syscall_report.h"
#include <cstring>
#include <cctype>

namespace boxedvn {
inline bool guestDataPath(const char* path) {
    if (!path) return false;
    if (!std::strstr(path, "/mnt/") && !std::strstr(path, "/drive_c/users/") &&
        !std::strstr(path, "/dosdevices/")) return false;
    // Loader traffic must not consume the data/save budget.
    const char* extension = std::strrchr(path, '.');
    if (extension) {
        char suffix[8]{};
        size_t n = std::strlen(extension);
        if (n < sizeof(suffix)) {
            for (size_t i=0; i<n; ++i) suffix[i]=(char)std::tolower((unsigned char)extension[i]);
            if (!std::strcmp(suffix,".dll") || !std::strcmp(suffix,".exe") ||
                !std::strcmp(suffix,".so") || !std::strcmp(suffix,".reg")) return false;
        }
    }
    return !std::strstr(path, "/.boxedvn-");
}
class GuestDataFileTrace {
public:
    BoundedSyscallReportLimiter::Outcome record(const char* path, unsigned operation, int64_t result) {
        if (!guestDataPath(path)) return {};
        uint64_t key=1469598103934665603ULL;
        for (const char* c=path; *c; ++c) key=(key ^ (unsigned char)*c)*1099511628211ULL;
        // Separate failures from normal reads/EOF; a busy read stream cannot
        // consume the open/stat budget needed to explain a later missing file.
        auto& limiter=result < 0 ? failures_ : operation == 2 ? reads_ : paths_;
        return limiter.record(0, 0, key, (uint64_t(operation)<<32) | (result < 0 ? uint32_t(-result) : result == 0));
    }
private:
    BoundedSyscallReportLimiter paths_, reads_, failures_;
};
}
