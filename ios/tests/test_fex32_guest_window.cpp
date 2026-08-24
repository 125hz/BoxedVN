#include "boxedvn_test.h"
#include "boxedvn/fex32_guest_window.h"

#include <cstdint>
#include <optional>
#include <vector>

using namespace boxedvn;

namespace {

struct VmOperation {
    enum class Kind {
        Reserve,
        Commit,
        Protect,
        Decommit,
        Release,
    };

    Kind kind;
    std::uint64_t address;
    std::uint64_t size;
    std::uint64_t detail;
};

class FakeFex32VirtualMemory final : public Fex32VirtualMemory {
public:
    std::optional<HostAddress64> nextBase =
        HostAddress64(0x7000000000ULL);
    std::uint64_t pageSize = 0x4000;
    bool operationResult = true;
    std::vector<VmOperation> operations;

    std::uint64_t hostPageSize() const noexcept override {
        return pageSize;
    }

    std::optional<HostAddress64> reserve(
        std::uint64_t size, std::uint64_t alignment) noexcept override {
        operations.push_back(
            {VmOperation::Kind::Reserve, 0, size, alignment});
        return nextBase;
    }

    bool commit(HostAddress64 address, std::uint64_t size,
                std::uint8_t access) noexcept override {
        operations.push_back(
            {VmOperation::Kind::Commit, address.value, size, access});
        return operationResult;
    }

    bool protect(HostAddress64 address, std::uint64_t size,
                 std::uint8_t access) noexcept override {
        operations.push_back(
            {VmOperation::Kind::Protect, address.value, size, access});
        return operationResult;
    }

    bool decommit(HostAddress64 address,
                  std::uint64_t size) noexcept override {
        operations.push_back(
            {VmOperation::Kind::Decommit, address.value, size, 0});
        return operationResult;
    }

    void release(HostAddress64 address,
                 std::uint64_t size) noexcept override {
        operations.push_back(
            {VmOperation::Kind::Release, address.value, size, 0});
    }
};

}  // namespace

BOXEDVN_TEST(fex32_window_requires_a_high_aligned_complete_reservation) {
    static_assert(Fex32GuestWindow::kWindowSize == 0x100000000ULL);
    static_assert(Fex32GuestWindow::kWindowAlignment == 0x100000000ULL);
    static_assert(Fex32GuestWindow::kPageCount == 0x100000);

    FakeFex32VirtualMemory memory;
    memory.nextBase = HostAddress64(0x7000001000ULL);
    CHECK(Fex32GuestWindow::create(memory) == nullptr);
    CHECK(memory.operations.back().kind == VmOperation::Kind::Release);

    memory.operations.clear();
    memory.nextBase = HostAddress64(0);
    CHECK(Fex32GuestWindow::create(memory) == nullptr);
    CHECK(memory.operations.back().kind == VmOperation::Kind::Release);
}

BOXEDVN_TEST(fex32_window_rejects_invalid_host_page_geometry_and_reserve_failure) {
    FakeFex32VirtualMemory memory;
    memory.pageSize = 0;
    CHECK(Fex32GuestWindow::create(memory) == nullptr);
    CHECK(memory.operations.empty());

    memory.pageSize = 0x1800;
    CHECK(Fex32GuestWindow::create(memory) == nullptr);
    CHECK(memory.operations.empty());

    memory.pageSize = Fex32GuestWindow::kWindowSize * 2;
    CHECK(Fex32GuestWindow::create(memory) == nullptr);
    CHECK(memory.operations.empty());

    memory.pageSize = 0x4000;
    memory.nextBase = std::nullopt;
    CHECK(Fex32GuestWindow::create(memory) == nullptr);
    CHECK_EQ(memory.operations.size(), std::size_t{1});
    CHECK(memory.operations.front().kind == VmOperation::Kind::Reserve);
}

BOXEDVN_TEST(fex32_window_commits_and_translates_cross_page_ranges) {
    FakeFex32VirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);
    CHECK_EQ(window->hostBase().value, 0x7000000000ULL);

    CHECK(window->commitPage(1, Fex32WindowRead | Fex32WindowWrite));
    CHECK(memory.operations.back().kind == VmOperation::Kind::Commit);
    CHECK_EQ(memory.operations.back().address, 0x7000000000ULL);
    CHECK_EQ(memory.operations.back().size, 0x4000ULL);
    CHECK(window->commitPage(2, Fex32WindowRead));
    CHECK(memory.operations.back().kind == VmOperation::Kind::Protect);
    CHECK_EQ(memory.operations.back().address, 0x7000000000ULL);
    CHECK_EQ(memory.operations.back().size, 0x4000ULL);
    CHECK_EQ(memory.operations.back().detail,
             static_cast<std::uint64_t>(Fex32WindowRead |
                                        Fex32WindowWrite));
    const auto translated = window->translate(
        GuestAddress32(0x1ff0), 0x20, Fex32WindowRead);
    CHECK(translated.has_value());
    CHECK_EQ(translated->value, 0x7000001ff0ULL);
    CHECK(!window->translate(GuestAddress32(0x1ff0), 0x20,
                             Fex32WindowWrite)
               .has_value());
    CHECK(!window->translate(GuestAddress32(0x2ff0), 0x20,
                             Fex32WindowRead)
               .has_value());
}

BOXEDVN_TEST(fex32_window_decommits_only_after_the_last_host_page_sibling) {
    FakeFex32VirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);

    CHECK(window->commitPage(0, Fex32WindowRead | Fex32WindowWrite));
    CHECK(window->commitPage(1, Fex32WindowRead));
    CHECK(window->decommitPage(0));
    CHECK(memory.operations.back().kind == VmOperation::Kind::Protect);
    CHECK_EQ(memory.operations.back().address, 0x7000000000ULL);
    CHECK_EQ(memory.operations.back().size, 0x4000ULL);
    CHECK_EQ(memory.operations.back().detail,
             static_cast<std::uint64_t>(Fex32WindowRead));
    CHECK(!window->isCommitted(0));
    CHECK(window->isCommitted(1));

    CHECK(window->decommitPage(1));
    CHECK(memory.operations.back().kind == VmOperation::Kind::Decommit);
    CHECK_EQ(memory.operations.back().address, 0x7000000000ULL);
    CHECK_EQ(memory.operations.back().size, 0x4000ULL);
}

BOXEDVN_TEST(fex32_window_tracks_protection_and_decommit) {
    FakeFex32VirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);

    CHECK(window->commitPage(4, Fex32WindowRead | Fex32WindowWrite));
    CHECK(window->isCommitted(4));
    CHECK(window->protectPage(4, Fex32WindowRead));
    CHECK_EQ(window->pageAccess(4),
             static_cast<std::uint8_t>(Fex32WindowRead));
    CHECK(!window->translate(GuestAddress32(0x4000), 1,
                             Fex32WindowWrite)
               .has_value());
    CHECK(window->protectPage(4, Fex32WindowNone));
    CHECK(window->isCommitted(4));
    CHECK_EQ(window->pageAccess(4),
             static_cast<std::uint8_t>(Fex32WindowNone));
    CHECK(!window->translate(GuestAddress32(0x4000), 1,
                             Fex32WindowRead)
               .has_value());
    CHECK(window->decommitPage(4));
    CHECK(!window->isCommitted(4));
    CHECK(!window->translate(GuestAddress32(0x4000), 1,
                             Fex32WindowRead)
               .has_value());
    CHECK(!window->decommitPage(4));
}

BOXEDVN_TEST(fex32_window_covers_the_last_guest_byte_without_overflow) {
    FakeFex32VirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);

    CHECK(window->commitPage(Fex32GuestWindow::kPageCount - 1,
                             Fex32WindowRead));
    const auto last = window->translate(
        GuestAddress32(0xffffffffU), 1, Fex32WindowRead);
    CHECK(last.has_value());
    CHECK_EQ(last->value, 0x70ffffffffULL);
    CHECK_EQ(window->hostToGuest(*last)->value, 0xffffffffU);
    const auto reserved = window->hostToGuest(
        HostAddress64(0x7000003000ULL), 0x1000);
    CHECK(reserved.has_value());
    CHECK_EQ(reserved->value, 0x3000U);
    CHECK(!window->translate(GuestAddress32(0xffffffffU), 2,
                             Fex32WindowRead)
               .has_value());
    CHECK(!window->hostToGuest(HostAddress64(0x7100000000ULL), 1)
               .has_value());
}

BOXEDVN_TEST(fex32_window_rejects_invalid_pages_ranges_and_access) {
    FakeFex32VirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);

    CHECK(!window->commitPage(Fex32GuestWindow::kPageCount,
                              Fex32WindowRead));
    CHECK(!window->commitPage(0, Fex32WindowNone));
    CHECK(!window->commitPage(0, 0x80));
    CHECK(!window->translate(GuestAddress32(0), 0, Fex32WindowRead)
               .has_value());
    CHECK(!window->translate(GuestAddress32(0), 1, Fex32WindowNone)
               .has_value());
}

BOXEDVN_TEST(fex32_window_keeps_state_unchanged_when_platform_calls_fail) {
    FakeFex32VirtualMemory memory;
    auto window = Fex32GuestWindow::create(memory);
    CHECK(window != nullptr);

    memory.operationResult = false;
    CHECK(!window->commitPage(8, Fex32WindowRead));
    CHECK(!window->isCommitted(8));

    memory.operationResult = true;
    CHECK(window->commitPage(8, Fex32WindowRead | Fex32WindowWrite));
    memory.operationResult = false;
    CHECK(!window->protectPage(8, Fex32WindowRead));
    CHECK_EQ(window->pageAccess(8), static_cast<std::uint8_t>(
                                           Fex32WindowRead |
                                           Fex32WindowWrite));
    CHECK(!window->decommitPage(8));
    CHECK(window->isCommitted(8));
}

BOXEDVN_TEST(fex32_window_releases_the_complete_reservation) {
    FakeFex32VirtualMemory memory;
    {
        auto window = Fex32GuestWindow::create(memory);
        CHECK(window != nullptr);
    }
    CHECK(memory.operations.back().kind == VmOperation::Kind::Release);
    CHECK_EQ(memory.operations.back().address, 0x7000000000ULL);
    CHECK_EQ(memory.operations.back().size,
             Fex32GuestWindow::kWindowSize);
}
