/*
 * BoxedVN - address metadata for a direct-mapped 64-bit guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/guest_address_space.h"

#include <algorithm>
#include <limits>

namespace boxedvn {

namespace {

bool checkedEnd(uint64_t base, uint64_t size, uint64_t& result) {
    if (size == 0 || base > std::numeric_limits<uint64_t>::max() - size) {
        return false;
    }
    result = base + size;
    return true;
}

} // namespace

uint64_t GuestMemoryRegion64::end() const {
    uint64_t value = 0;
    return checkedEnd(guestBase, size, value) ? value : 0;
}

bool GuestMemoryRegion64::contains(uint64_t address, uint64_t length) const {
    uint64_t regionEnd = 0;
    uint64_t requestedEnd = 0;
    return checkedEnd(guestBase, size, regionEnd) &&
           checkedEnd(address, length, requestedEnd) &&
           address >= guestBase && requestedEnd <= regionEnd;
}

bool GuestMemoryRegion64::isIdentityMapped() const {
    return guestBase == static_cast<uint64_t>(hostBase);
}

bool GuestAddressSpace64::add(const GuestMemoryRegion64& region) {
    uint64_t newEnd = 0;
    if (region.hostBase == 0 || !checkedEnd(region.guestBase, region.size, newEnd)) {
        return false;
    }
    const auto position = std::lower_bound(
        regions_.begin(), regions_.end(), region.guestBase,
        [](const GuestMemoryRegion64& candidate, uint64_t base) {
            return candidate.guestBase < base;
        });
    if (position != regions_.begin()) {
        const GuestMemoryRegion64& previous = *std::prev(position);
        if (previous.end() > region.guestBase) {
            return false;
        }
    }
    if (position != regions_.end() && newEnd > position->guestBase) {
        return false;
    }
    regions_.insert(position, region);
    return true;
}

bool GuestAddressSpace64::remove(uint64_t guestBase, uint64_t size) {
    const auto found = std::find_if(
        regions_.begin(), regions_.end(),
        [guestBase, size](const GuestMemoryRegion64& region) {
            return region.guestBase == guestBase && region.size == size;
        });
    if (found == regions_.end()) {
        return false;
    }
    regions_.erase(found);
    return true;
}

bool GuestAddressSpace64::protect(uint64_t guestBase, uint64_t size,
                                  uint8_t access) {
    const auto found = std::find_if(
        regions_.begin(), regions_.end(),
        [guestBase, size](const GuestMemoryRegion64& region) {
            return region.guestBase == guestBase && region.size == size;
        });
    if (found == regions_.end()) {
        return false;
    }
    found->access = access;
    return true;
}

const GuestMemoryRegion64* GuestAddressSpace64::find(
    uint64_t guestAddress, uint64_t length) const {
    const auto position = std::upper_bound(
        regions_.begin(), regions_.end(), guestAddress,
        [](uint64_t address, const GuestMemoryRegion64& candidate) {
            return address < candidate.guestBase;
        });
    if (position == regions_.begin()) {
        return nullptr;
    }
    const GuestMemoryRegion64& candidate = *std::prev(position);
    return candidate.contains(guestAddress, length) ? &candidate : nullptr;
}

void* GuestAddressSpace64::translate(uint64_t guestAddress, uint64_t length,
                                     uint8_t requiredAccess) const {
    const GuestMemoryRegion64* region = find(guestAddress, length);
    if (region == nullptr || (region->access & requiredAccess) != requiredAccess) {
        return nullptr;
    }
    const uint64_t offset = guestAddress - region->guestBase;
    if (offset > std::numeric_limits<uintptr_t>::max() - region->hostBase) {
        return nullptr;
    }
    return reinterpret_cast<void*>(region->hostBase + static_cast<uintptr_t>(offset));
}

std::optional<GuestMemoryRegion64> GuestAddressSpace64::executableRange(
    uint64_t guestAddress) const {
    const GuestMemoryRegion64* region = find(guestAddress);
    if (region == nullptr || (region->access & GuestMemoryExecute) == 0) {
        return std::nullopt;
    }
    return *region;
}

} // namespace boxedvn
