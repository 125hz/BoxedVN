// Restore Linux's anonymous MADV_DONTNEED contract for the native FEX library.
// GPL-2.0-or-later.
#pragma once
#include <sys/mman.h>
#include <cstring>
#include <cstdint>
#include <cerrno>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <mach/mach_vm.h>
#endif
#endif
namespace boxedvn {
inline int discardFexHostPages(void* address, size_t length, int advice) {
#if defined(__APPLE__)
    if (advice == MADV_DONTNEED && length) {
        const auto begin = reinterpret_cast<uintptr_t>(address);
        if (begin + length < begin || begin % vm_page_size) { errno=EINVAL; return -1; }
        // Only anonymous writable data may be zeroed. File-backed code-cache
        // views and executable aliases retain the platform's ordinary hint.
        static_assert(sizeof(vm_address_t) == sizeof(uintptr_t));
        vm_address_t cursor = begin;
        const vm_address_t end = begin + length;
        while (cursor < end) {
            vm_address_t region = cursor; vm_size_t bytes = 0;
            vm_prot_t protection;
            bool externalPager;
#if TARGET_OS_OSX
            mach_vm_address_t wideRegion = cursor;
            mach_vm_size_t wideBytes = 0;
            natural_t depth = 0;
            vm_region_submap_info_data_64_t info{};
            mach_msg_type_number_t count;
            kern_return_t status;
            do {
                count=VM_REGION_SUBMAP_INFO_COUNT_64;
                status=mach_vm_region_recurse(mach_task_self(), &wideRegion, &wideBytes, &depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
                if(status!=KERN_SUCCESS) {errno=ENOMEM;return -1;}
                if(info.is_submap) ++depth;
            } while(info.is_submap);
            region=wideRegion; bytes=wideBytes;
            protection=info.protection; externalPager=info.external_pager;
#else
            // iPhone SDKs expose vm_region_64, not the macOS mach_vm header.
            // Extended info supplies the backing-object check as well as access.
            vm_region_extended_info_data_t info{};
            mach_msg_type_number_t count=VM_REGION_EXTENDED_INFO_COUNT;
            mach_port_t object=MACH_PORT_NULL;
            const auto status=vm_region_64(mach_task_self(), &region, &bytes,
                VM_REGION_EXTENDED_INFO, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
            if(object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
            if(status!=KERN_SUCCESS) {errno=ENOMEM;return -1;}
            protection=info.protection; externalPager=info.external_pager;
#endif
            if(region>cursor || !bytes || region+bytes<=cursor) {errno=ENOMEM;return -1;}
            if(externalPager || (protection & (VM_PROT_WRITE|VM_PROT_EXECUTE)) != VM_PROT_WRITE)
                return ::madvise(address,length,advice);
            cursor=region+bytes;
        }
#ifdef MADV_ZERO
        if(::madvise(address,length,MADV_ZERO)==0) return 0;
#endif
        // Older kernels: establish zeros before giving away the physical pages.
        // MADV_FREE alone can retain stale cache entries until memory pressure.
        std::memset(address,0,length);
        (void)::madvise(address,length,MADV_FREE);
        return 0;
    }
#endif
    return ::madvise(address,length,advice);
}
}
