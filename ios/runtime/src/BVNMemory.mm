/*
 * BoxedVN - signed increased-memory entitlement detection.
 * GPLv2; see license.txt.
 */

#import <Foundation/Foundation.h>

#include <os/proc.h>
#include <mach/mach.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "BVNRuntime.h"

extern "C" int csops(pid_t pid, unsigned int ops, void* useraddr,
                     size_t usersize);

// SecTask is exported by Security.framework on iOS but omitted from the public
// umbrella header. It asks the code-signing subsystem for the entitlement of
// the current task, so it also works when a signer emitted DER entitlements
// instead of the legacy XML blob handled by the fallback below.
typedef struct __SecTask* SecTaskRef;
extern "C" SecTaskRef SecTaskCreateFromSelf(CFAllocatorRef allocator);
extern "C" CFTypeRef SecTaskCopyValueForEntitlement(
    SecTaskRef task, CFStringRef entitlement, CFErrorRef* error);

namespace {

constexpr unsigned int kCSOpsEntitlementsBlob = 7;
constexpr uint32_t kEmbeddedEntitlementsMagic = 0xfade7171;

uint32_t readBigEndian32(const unsigned char* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

BVNMemoryEntitlementStatus signedEntitlementStatus(std::string& detail) {
    SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
    if (task != nullptr) {
        CFErrorRef error = nullptr;
        CFTypeRef value = SecTaskCopyValueForEntitlement(
            task,
            CFSTR("com.apple.developer.kernel.increased-memory-limit"),
            &error);
        CFRelease(task);

        if (value != nullptr) {
            const bool enabled = CFGetTypeID(value) == CFBooleanGetTypeID() &&
                                 CFBooleanGetValue((CFBooleanRef)value);
            CFRelease(value);
            if (error != nullptr) {
                CFRelease(error);
            }
            detail = enabled
                ? "The installed app's signed entitlement is enabled."
                : "The installed app's signed entitlement is not enabled.";
            return enabled ? BVNMemoryEntitlementEnabled
                           : BVNMemoryEntitlementDisabled;
        }

        // A successful query returning no value means the signature simply
        // does not contain the key. Only fall back to blob parsing when the
        // code-signing query itself reported an error.
        if (error == nullptr) {
            detail = "The installed app's signed entitlement is not enabled.";
            return BVNMemoryEntitlementDisabled;
        }
        CFRelease(error);
    }

    // Entitlement dictionaries are normally a few kilobytes. A generous
    // fixed buffer keeps the probe allocation-free at the kernel boundary and
    // avoids depending on private SDK headers for the Code Signing blob ABI.
    std::array<unsigned char, 256 * 1024> blob{};
    if (csops(getpid(), kCSOpsEntitlementsBlob, blob.data(), blob.size()) != 0) {
        detail = "The running signature's entitlement blob could not be read.";
        return BVNMemoryEntitlementUnknown;
    }

    const uint32_t magic = readBigEndian32(blob.data());
    const uint32_t length = readBigEndian32(blob.data() + 4);
    if (magic != kEmbeddedEntitlementsMagic || length <= 8 ||
        length > blob.size()) {
        detail = "The running signature has no readable XML entitlement blob.";
        return BVNMemoryEntitlementUnknown;
    }

    NSData* data = [NSData dataWithBytes:blob.data() + 8 length:length - 8];
    NSError* error = nil;
    id plist = [NSPropertyListSerialization propertyListWithData:data
                                                         options:0
                                                          format:nullptr
                                                           error:&error];
    if (![plist isKindOfClass:[NSDictionary class]]) {
        detail = "The running signature's entitlement dictionary is invalid.";
        return BVNMemoryEntitlementUnknown;
    }

    id value = [(NSDictionary*)plist
        objectForKey:@"com.apple.developer.kernel.increased-memory-limit"];
    if ([value respondsToSelector:@selector(boolValue)] && [value boolValue]) {
        detail = "The installed app's signed entitlement is enabled.";
        return BVNMemoryEntitlementEnabled;
    }

    detail = "The installed app's signed entitlement is not enabled.";
    return BVNMemoryEntitlementDisabled;
}

}  // namespace

static uint64_t processResidentBytes(void) {
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<uint64_t>(info.resident_size);
}

extern "C" BVNMemoryReport BVNMemoryProbe(void) {
    static thread_local std::string detail;
    BVNMemoryReport report{};
    report.increasedMemoryLimit = signedEntitlementStatus(detail);
    report.availableBytes = static_cast<uint64_t>(os_proc_available_memory());
    report.physicalMemoryBytes =
        static_cast<uint64_t>(NSProcessInfo.processInfo.physicalMemory);
    report.processResidentBytes = processResidentBytes();
    report.detail = detail.c_str();
    return report;
}

extern "C" uint64_t BVNGuestReportedTotalMemory(void) {
    constexpr uint64_t kMinimum = 512ull * 1024ull * 1024ull;
    // The guest is 32-bit. Advertising more than 3 GB invites Wine to make
    // allocations that cannot coexist in its user address space even when
    // the iOS host process has a 6 GB entitlement budget.
    constexpr uint64_t kGuestAddressableMaximum = 3ull * 1024ull * 1024ull * 1024ull;
    const BVNMemoryReport report = BVNMemoryProbe();
    const uint64_t processBudget = report.availableBytes +
                                   report.processResidentBytes;
    return std::max(kMinimum,
                    std::min(kGuestAddressableMaximum,
                             std::min(report.physicalMemoryBytes,
                                      processBudget)));
}

extern "C" uint64_t BVNGuestReportedFreeMemory(void) {
    return std::min(BVNGuestReportedTotalMemory(),
                    static_cast<uint64_t>(os_proc_available_memory()));
}
