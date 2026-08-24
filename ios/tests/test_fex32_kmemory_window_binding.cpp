#include "boxedvn_test.h"
#include "boxedvn/fex32_kmemory_window_binding.h"

#include <cstdint>
#include <optional>
#include <vector>

using namespace boxedvn;

namespace {

struct BindingVmOperation {
    enum class Kind { Reserve, Commit, Protect, Decommit, Release };
    Kind kind;
    std::uint64_t address;
    std::uint8_t access;
};

class BindingVirtualMemory final : public Fex32VirtualMemory {
public:
    std::uint64_t pageSize = 0x4000;
    std::size_t failOperation = 0;
    std::vector<BindingVmOperation> operations;

    std::uint64_t hostPageSize() const noexcept override { return pageSize; }

    std::optional<HostAddress64> reserve(
        std::uint64_t, std::uint64_t) noexcept override {
        operations.push_back(
            {BindingVmOperation::Kind::Reserve, 0, Fex32WindowNone});
        return HostAddress64(0x7000000000ULL);
    }

    bool commit(HostAddress64 address, std::uint64_t,
                std::uint8_t access) noexcept override {
        return record(BindingVmOperation::Kind::Commit, address, access);
    }

    bool protect(HostAddress64 address, std::uint64_t,
                 std::uint8_t access) noexcept override {
        return record(BindingVmOperation::Kind::Protect, address, access);
    }

    bool decommit(HostAddress64 address, std::uint64_t) noexcept override {
        return record(BindingVmOperation::Kind::Decommit, address,
                      Fex32WindowNone);
    }

    void release(HostAddress64 address, std::uint64_t) noexcept override {
        operations.push_back(
            {BindingVmOperation::Kind::Release, address.value,
             Fex32WindowNone});
    }

private:
    bool record(BindingVmOperation::Kind kind, HostAddress64 address,
                std::uint8_t access) noexcept {
        operations.push_back({kind, address.value, access});
        return failOperation == 0 || operations.size() != failOperation;
    }
};

}  // namespace

BOXEDVN_TEST(fex32_kmemory_binding_tracks_uncommitted_prot_none_pages) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    const std::size_t operationsBefore = memory.operations.size();
    CHECK(binding.mapAnonymous(16, 2, Fex32KMemoryNone));
    CHECK_EQ(memory.operations.size(), operationsBefore);
    CHECK(binding.isMapped(16));
    CHECK(!binding.isCommitted(16));
    CHECK(!binding.hostPageAddress(16).has_value());

    CHECK(binding.protectAnonymous(
        16, 2, Fex32KMemoryRead | Fex32KMemoryWrite));
    CHECK(binding.isCommitted(16));
    CHECK_EQ(binding.pageAccess(17), static_cast<std::uint8_t>(
                                         Fex32KMemoryRead |
                                         Fex32KMemoryWrite));
    CHECK_EQ(*binding.hostPageAddress(16),
             static_cast<std::uintptr_t>(0x7000010000ULL));

    CHECK(binding.protectAnonymous(16, 2, Fex32KMemoryNone));
    CHECK(binding.isMapped(16));
    CHECK(!binding.isCommitted(16));
    CHECK_EQ(binding.pageAccess(16),
             static_cast<std::uint8_t>(Fex32KMemoryNone));
    CHECK(!binding.hostPageAddress(16).has_value());
}

BOXEDVN_TEST(fex32_kmemory_binding_shares_host_permissions_across_16k) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    CHECK(binding.mapAnonymous(1, 1, Fex32KMemoryRead));
    CHECK(binding.mapAnonymous(2, 1,
                               Fex32KMemoryRead | Fex32KMemoryWrite));
    CHECK(memory.operations.back().kind ==
          BindingVmOperation::Kind::Protect);
    CHECK_EQ(memory.operations.back().address, 0x7000000000ULL);
    CHECK_EQ(memory.operations.back().access,
             static_cast<std::uint8_t>(Fex32WindowRead |
                                       Fex32WindowWrite));
}

BOXEDVN_TEST(fex32_kmemory_binding_rolls_back_partial_map_failure) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    // Reserve is operation 1; fail the second page's grouped protect.
    memory.failOperation = 3;
    CHECK(!binding.mapAnonymous(4, 3, Fex32KMemoryRead));
    CHECK(binding.healthy());
    CHECK(!binding.isMapped(4));
    CHECK(!window->isCommitted(4));
    CHECK(!window->isCommitted(5));
}

BOXEDVN_TEST(fex32_kmemory_binding_rolls_back_protection_failure) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    CHECK(binding.mapAnonymous(
        8, 2, Fex32KMemoryRead | Fex32KMemoryWrite));
    const std::uint8_t oldAccess = binding.pageAccess(8);
    memory.failOperation = memory.operations.size() + 2;
    CHECK(!binding.protectAnonymous(8, 2, Fex32KMemoryRead));
    CHECK(binding.healthy());
    CHECK_EQ(binding.pageAccess(8), oldAccess);
    CHECK_EQ(binding.pageAccess(9), oldAccess);
}

BOXEDVN_TEST(fex32_kmemory_binding_preserves_state_when_unmap_fails) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    CHECK(binding.mapAnonymous(20, 2, Fex32KMemoryRead));
    memory.failOperation = memory.operations.size() + 2;
    CHECK(!binding.unmapAnonymous(20, 2));
    CHECK(binding.healthy());
    CHECK(binding.isMapped(20));
    CHECK(binding.isCommitted(20));
    CHECK(binding.isMapped(21));
    CHECK(binding.isCommitted(21));
}

BOXEDVN_TEST(fex32_kmemory_binding_accepts_munmap_holes_and_clear) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    CHECK(binding.mapAnonymous(32, 1, Fex32KMemoryRead));
    CHECK(binding.mapAnonymous(34, 1, Fex32KMemoryExecute));
    CHECK(binding.unmapAnonymous(31, 2));
    CHECK(!binding.isMapped(32));
    CHECK(binding.isMapped(34));
    CHECK(binding.clear());
    CHECK(!binding.isMapped(34));
}

BOXEDVN_TEST(fex32_kmemory_binding_rejects_overlaps_ranges_and_access) {
    BindingVirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    Fex32WindowKMemoryBinding binding(*window);

    CHECK(!binding.mapAnonymous(0, 0, Fex32KMemoryRead));
    CHECK(!binding.mapAnonymous(Fex32GuestWindow::kPageCount, 1,
                                Fex32KMemoryRead));
    CHECK(!binding.mapAnonymous(0, 1, 0x80));
    CHECK(binding.mapAnonymous(40, 2, Fex32KMemoryRead));
    CHECK(!binding.mapAnonymous(41, 1, Fex32KMemoryRead));
    CHECK(!binding.protectAnonymous(42, 1, Fex32KMemoryWrite));
}
