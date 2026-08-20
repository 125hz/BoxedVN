#include "boxedvn_test.h"
#include "boxedvn/guest_address_space.h"

#include <array>
#include <cstdint>

using namespace boxedvn;

BOXEDVN_TEST(guest64_address_space_rejects_overlap_and_overflow) {
    std::array<uint8_t, 64> first {};
    std::array<uint8_t, 64> second {};
    GuestAddressSpace64 memory;
    CHECK_EQ(memory.add({0x1000, 0x40, reinterpret_cast<uintptr_t>(first.data()),
                         GuestMemoryRead}), true);
    CHECK_EQ(memory.add({0x1020, 0x40, reinterpret_cast<uintptr_t>(second.data()),
                         GuestMemoryRead}), false);
    CHECK_EQ(memory.add({UINT64_MAX - 1, 8,
                         reinterpret_cast<uintptr_t>(second.data()),
                         GuestMemoryRead}), false);
}

BOXEDVN_TEST(guest64_address_space_translates_with_permissions) {
    std::array<uint8_t, 64> storage {};
    GuestAddressSpace64 memory;
    CHECK_EQ(memory.add({0x140000000ULL, storage.size(),
                         reinterpret_cast<uintptr_t>(storage.data()),
                         GuestMemoryRead | GuestMemoryExecute}), true);
    CHECK_EQ(memory.translate(0x140000008ULL, 4, GuestMemoryRead),
             static_cast<void*>(storage.data() + 8));
    CHECK_EQ(memory.translate(0x140000008ULL, 4, GuestMemoryWrite), nullptr);
    CHECK_EQ(memory.executableRange(0x140000010ULL).has_value(), true);
}

BOXEDVN_TEST(guest64_address_space_requires_exact_remove_and_protect) {
    std::array<uint8_t, 32> storage {};
    GuestAddressSpace64 memory;
    CHECK_EQ(memory.add({0x8000, storage.size(),
                         reinterpret_cast<uintptr_t>(storage.data()),
                         GuestMemoryRead}), true);
    CHECK_EQ(memory.protect(0x8000, storage.size(),
                            GuestMemoryRead | GuestMemoryWrite), true);
    CHECK_EQ(memory.translate(0x8000, 1, GuestMemoryWrite),
             static_cast<void*>(storage.data()));
    CHECK_EQ(memory.remove(0x8000, storage.size() - 1), false);
    CHECK_EQ(memory.remove(0x8000, storage.size()), true);
}
