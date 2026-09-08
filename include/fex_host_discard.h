// Restore Linux's anonymous MADV_DONTNEED contract for the native FEX library.
// GPL-2.0-or-later.
#pragma once
#include <sys/mman.h>
#include <cstring>
#include <cstdint>
#include <cerrno>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
namespace boxedvn {
inline int discardFexHostPages(void* address, size_t length, int advice) {
#if defined(__APPLE__)
    if (advice == MADV_DONTNEED && length) {
        const auto begin = reinterpret_cast<uintptr_t>(address);
        if (begin + length < begin || begin % vm_page_size) { errno=EINVAL; return -1; }
        // Only anonymous writable data may be zeroed. File-backed code-cache
        // views and executable aliases retain the platform's ordinary hint.
        mach_vm_address_t cursor = begin;
        const mach_vm_address_t end = begin + length;
        while (cursor < end) {
            mach_vm_address_t region = cursor; mach_vm_size_t bytes = 0;
            natural_t depth = 0;
            vm_region_submap_info_data_64_t info{};
            mach_msg_type_number_t count;
            kern_return_t status;
            do {
                count=VM_REGION_SUBMAP_INFO_COUNT_64;
                status=mach_vm_region_recurse(mach_task_self(), &region, &bytes, &depth,
                    reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
                if(status!=KERN_SUCCESS) {errno=ENOMEM;return -1;}
                if(info.is_submap) ++depth;
            } while(info.is_submap);
            if(region>cursor || !bytes || region+bytes<=cursor) {errno=ENOMEM;return -1;}
            if(info.external_pager || (info.protection & (VM_PROT_WRITE|VM_PROT_EXECUTE)) != VM_PROT_WRITE)
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
