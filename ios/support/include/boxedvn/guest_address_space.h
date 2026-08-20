/*
 * BoxedVN - address metadata for a direct-mapped 64-bit guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_GUEST_ADDRESS_SPACE_H
#define BOXEDVN_GUEST_ADDRESS_SPACE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace boxedvn {

enum GuestMemoryAccess : uint8_t {
    GuestMemoryNone = 0,
    GuestMemoryRead = 1u << 0,
    GuestMemoryWrite = 1u << 1,
    GuestMemoryExecute = 1u << 2,
};

struct GuestMemoryRegion64 {
    uint64_t guestBase = 0;
    uint64_t size = 0;
    uintptr_t hostBase = 0;
    uint8_t access = GuestMemoryNone;

    uint64_t end() const;
    bool contains(uint64_t address, uint64_t length = 1) const;
    bool isIdentityMapped() const;
};

// Metadata and checked translation for FEX's direct guest mappings.  The host
// mapping itself is owned by the platform layer (mmap/vm_allocate on iOS),
// while this class gives BoxedWine's syscall and loader code one authoritative
// view of ranges and permissions.  It is sparse and uses U64 guest addresses;
// the existing IA-32 KMemory remains unchanged.
class GuestAddressSpace64 {
public:
    bool add(const GuestMemoryRegion64& region);
    bool remove(uint64_t guestBase, uint64_t size);
    bool protect(uint64_t guestBase, uint64_t size, uint8_t access);

    const GuestMemoryRegion64* find(uint64_t guestAddress,
                                    uint64_t length = 1) const;
    void* translate(uint64_t guestAddress, uint64_t length,
                    uint8_t requiredAccess) const;
    std::optional<GuestMemoryRegion64> executableRange(
        uint64_t guestAddress) const;

    const std::vector<GuestMemoryRegion64>& regions() const { return regions_; }

private:
    std::vector<GuestMemoryRegion64> regions_;
};

} // namespace boxedvn

#endif
