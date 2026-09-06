#pragma once

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>

namespace boxedvn {
// Own a stable, non-executable host page independently of every guest view.
// mapAt replaces only a host page that the caller has already reserved.
class NativeSharedAlias {
public:
    static size_t pageSize() {
#ifdef BOXEDVN_NATIVE_ALIAS_TEST_GRANULE
        return BOXEDVN_NATIVE_ALIAS_TEST_GRANULE;
#else
        static const size_t value = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        return value;
#endif
    }
    NativeSharedAlias() {
        void* p = mmap(nullptr, pageSize(), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) backing = static_cast<uint8_t*>(p);
    }
    ~NativeSharedAlias() { if (backing) munmap(backing, pageSize()); }
    NativeSharedAlias(const NativeSharedAlias&) = delete;
    NativeSharedAlias& operator=(const NativeSharedAlias&) = delete;
    uint8_t* data() const { return backing; }
    bool mapAt(uintptr_t destination) const {
        return remapAt(backing,destination,false);
    }
    // A replacement anonymous view must stop sharing before it is cleared.
    // Copy-on-write also preserves untouched bytes in its enclosing host page.
    static bool copyAt(const uint8_t* source, uintptr_t destination) {
        return remapAt(source,destination,true);
    }
private:
    static bool remapAt(const uint8_t* source, uintptr_t destination, bool copy) {
        if (!source || !pageSize() || destination % pageSize()) return false;
        vm_address_t target = static_cast<vm_address_t>(destination);
        vm_prot_t current = 0, maximum = 0;
        const auto result = vm_remap(mach_task_self(), &target, pageSize(), 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
            reinterpret_cast<vm_address_t>(source), copy ? TRUE : FALSE,
            &current, &maximum, VM_INHERIT_SHARE);
        return result == KERN_SUCCESS && target == destination;
    }
    uint8_t* backing = nullptr;
};
}
#endif
