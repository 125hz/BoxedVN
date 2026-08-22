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
#include <cstdio>
#include <cstring>
#include <sstream>
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

extern "C" BVNLowAddressProbeReport BVNLow4GiBIdentityProbe(void) {
    struct Candidate {
        uint64_t address;
        uint64_t size;
        const char* label;
    };
    constexpr uint64_t kLow4GiBEnd = 0x100000000ULL;
    constexpr Candidate candidates[] = {
        {0x00010000ULL, 0x4000ULL, "allocation-floor"},
        {0x10000000ULL, 0x4000ULL, "image-heap"},
        {0x40000000ULL, 0x4000ULL, "midpoint"},
        {0x7ffe0000ULL, 0x10000ULL, "shared-data"},
        {0xffff0000ULL, 0x10000ULL, "upper-boundary"},
    };

    static thread_local std::string detail;
    BVNLowAddressProbeReport report{};
    report.status = BVNLowAddressProbeBlocked;
    report.candidateCount = static_cast<uint32_t>(std::size(candidates));
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    report.hostPageSize = pageSize > 0 ? static_cast<uint64_t>(pageSize) : 4096;

    bool pageZeroCoversLow4GiB = false;
    vm_address_t cursor = 0;
    while (static_cast<uint64_t>(cursor) < kLow4GiBEnd) {
        vm_address_t regionStart = cursor;
        vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t objectName = MACH_PORT_NULL;
        const kern_return_t result = vm_region_64(
            mach_task_self(), &regionStart, &regionSize,
            VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &infoCount,
            &objectName);
        if (objectName != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), objectName);
        }
        if (result == KERN_INVALID_ADDRESS) {
            break;
        }
        if (result != KERN_SUCCESS || regionSize == 0) {
            report.status = BVNLowAddressProbeQueryError;
            report.machResult = static_cast<int32_t>(result);
            detail = "Could not enumerate the host address space below 4 GiB.";
            report.detail = detail.c_str();
            BVNLogWrite(BVNLogLevelError, "fex32-address", report.detail);
            return report;
        }
        ++report.regionCount;
        const uint64_t start = static_cast<uint64_t>(regionStart);
        const uint64_t size = static_cast<uint64_t>(regionSize);
        const uint64_t end = size > UINT64_MAX - start ? UINT64_MAX : start + size;
        if (start == 0 && end >= kLow4GiBEnd &&
            info.protection == VM_PROT_NONE) {
            pageZeroCoversLow4GiB = true;
            report.blockingRegionStart = start;
            report.blockingRegionSize = size;
        }
        if (end <= static_cast<uint64_t>(cursor)) {
            report.status = BVNLowAddressProbeQueryError;
            report.machResult = KERN_FAILURE;
            detail = "The host VM region query did not advance.";
            report.detail = detail.c_str();
            BVNLogWrite(BVNLogLevelError, "fex32-address", report.detail);
            return report;
        }
        cursor = static_cast<vm_address_t>(end);
    }

    bool reservationFailed = false;
    for (const Candidate& candidate : candidates) {
        const uint64_t candidateEnd = candidate.address + candidate.size;
        vm_address_t regionStart = static_cast<vm_address_t>(candidate.address);
        vm_size_t regionSize = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t objectName = MACH_PORT_NULL;
        const kern_return_t query = vm_region_64(
            mach_task_self(), &regionStart, &regionSize,
            VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &infoCount,
            &objectName);
        if (objectName != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), objectName);
        }
        if (query != KERN_SUCCESS && query != KERN_INVALID_ADDRESS) {
            report.status = BVNLowAddressProbeQueryError;
            report.machResult = static_cast<int32_t>(query);
            detail = "A low-address candidate could not be queried safely.";
            report.detail = detail.c_str();
            BVNLogWrite(BVNLogLevelError, "fex32-address", report.detail);
            return report;
        }
        const bool occupied = query == KERN_SUCCESS &&
            static_cast<uint64_t>(regionStart) < candidateEnd;
        char message[256];
        if (occupied) {
            std::snprintf(message, sizeof(message),
                          "candidate=%s address=0x%llx size=0x%llx occupied_by=[0x%llx,0x%llx)",
                          candidate.label,
                          static_cast<unsigned long long>(candidate.address),
                          static_cast<unsigned long long>(candidate.size),
                          static_cast<unsigned long long>(regionStart),
                          static_cast<unsigned long long>(
                              static_cast<uint64_t>(regionStart) + regionSize));
            BVNLogWrite(BVNLogLevelInfo, "fex32-address", message);
            if (report.blockingRegionSize == 0) {
                report.blockingRegionStart = static_cast<uint64_t>(regionStart);
                report.blockingRegionSize = static_cast<uint64_t>(regionSize);
            }
            continue;
        }

        vm_address_t reserved = static_cast<vm_address_t>(candidate.address);
        kern_return_t result = vm_allocate(
            mach_task_self(), &reserved, static_cast<vm_size_t>(candidate.size),
            VM_FLAGS_FIXED);
        if (result != KERN_SUCCESS ||
            static_cast<uint64_t>(reserved) != candidate.address) {
            if (result == KERN_SUCCESS) {
                vm_deallocate(mach_task_self(), reserved,
                              static_cast<vm_size_t>(candidate.size));
            }
            reservationFailed = true;
            report.machResult = static_cast<int32_t>(result);
            std::snprintf(message, sizeof(message),
                          "candidate=%s address=0x%llx reservation_failed=%d",
                          candidate.label,
                          static_cast<unsigned long long>(candidate.address),
                          static_cast<int>(result));
            BVNLogWrite(BVNLogLevelWarning, "fex32-address", message);
            continue;
        }

        result = vm_protect(mach_task_self(), reserved,
                            static_cast<vm_size_t>(candidate.size), FALSE,
                            VM_PROT_READ | VM_PROT_WRITE);
        bool verified = false;
        if (result == KERN_SUCCESS) {
            volatile uint64_t* first = reinterpret_cast<volatile uint64_t*>(
                static_cast<uintptr_t>(candidate.address));
            volatile uint64_t* last = reinterpret_cast<volatile uint64_t*>(
                static_cast<uintptr_t>(candidateEnd - sizeof(uint64_t)));
            constexpr uint64_t firstSentinel = 0x42564e33324c4f57ULL;
            constexpr uint64_t lastSentinel = 0x4944454e54495459ULL;
            *first = firstSentinel;
            *last = lastSentinel;
            verified = *first == firstSentinel && *last == lastSentinel;
        }
        const kern_return_t cleanup = vm_deallocate(
            mach_task_self(), reserved, static_cast<vm_size_t>(candidate.size));
        if (result == KERN_SUCCESS && verified && cleanup == KERN_SUCCESS) {
            ++report.claimedCandidateCount;
            std::snprintf(message, sizeof(message),
                          "candidate=%s address=0x%llx claim_touch_release=ok",
                          candidate.label,
                          static_cast<unsigned long long>(candidate.address));
            BVNLogWrite(BVNLogLevelInfo, "fex32-address", message);
        } else {
            reservationFailed = true;
            report.machResult = static_cast<int32_t>(
                result != KERN_SUCCESS ? result : cleanup);
            std::snprintf(message, sizeof(message),
                          "candidate=%s address=0x%llx protect=%d verified=%u cleanup=%d",
                          candidate.label,
                          static_cast<unsigned long long>(candidate.address),
                          static_cast<int>(result), verified ? 1U : 0U,
                          static_cast<int>(cleanup));
            BVNLogWrite(BVNLogLevelWarning, "fex32-address", message);
        }
    }

    std::ostringstream summary;
    summary << "Host page size " << report.hostPageSize << " bytes; "
            << report.regionCount << " low regions; claimed "
            << report.claimedCandidateCount << "/" << report.candidateCount
            << " representative low-address holes.";
    if (pageZeroCoversLow4GiB) {
        report.status = BVNLowAddressProbeBlocked;
        summary << " The app's PAGEZERO reservation covers the complete low 4 GiB, "
                   "so direct FEX32 identity mapping is unavailable.";
    } else if (report.claimedCandidateCount == report.candidateCount) {
        report.status = BVNLowAddressProbeCandidate;
        summary << " Representative identity mappings succeeded; a full address-space "
                   "prototype is still required.";
    } else if (report.claimedCandidateCount > 0) {
        report.status = BVNLowAddressProbePartial;
        summary << " Only partial holes are available, which is not sufficient proof "
                   "for a complete FEX32 address space.";
    } else if (reservationFailed) {
        report.status = BVNLowAddressProbeReservationError;
        summary << " Free-looking candidates could not be reserved safely.";
    } else {
        report.status = BVNLowAddressProbeBlocked;
        summary << " No representative low-address identity mapping was available.";
    }
    detail = summary.str();
    report.detail = detail.c_str();
    BVNLogWrite(report.status == BVNLowAddressProbeCandidate
                    ? BVNLogLevelInfo : BVNLogLevelWarning,
                "fex32-address", report.detail);
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
