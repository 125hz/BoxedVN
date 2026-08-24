#include "boxedvn_test.h"
#include "../runtime/src/BVNFEX32VirtualMemory.h"

#include <cstdint>
#include <limits>
#include <type_traits>

// The implementation is iOS-only, but the contract-facing declaration is
// intentionally safe to include in the host test binary. This keeps the
// provider API reviewable without pretending Windows can exercise Mach VM.
static_assert(std::is_base_of_v<boxedvn::Fex32VirtualMemory,
                                BVNFEX32DarwinVirtualMemory>);
static_assert(boxedvn::Fex32GuestWindow::kWindowSize == 0x100000000ULL);
static_assert(boxedvn::Fex32GuestWindow::kPageSize == 0x1000ULL);

BOXEDVN_TEST(fex32_darwin_reservation_layout_aligns_a_complete_window) {
    const auto layout = bvn_fex32::computeReservationLayout(
        0x12345000ULL, 0x100000000ULL, 0x100000000ULL);
    CHECK(layout.has_value());
    CHECK_EQ(layout->span, std::uint64_t{0x200000000ULL});
    CHECK_EQ(layout->aligned, std::uint64_t{0x100000000ULL});
    CHECK_EQ(layout->prefix, std::uint64_t{0xedcbb000ULL});
    CHECK_EQ(layout->suffix, std::uint64_t{0x12345000ULL});
}

BOXEDVN_TEST(fex32_darwin_reservation_layout_handles_an_aligned_base) {
    const auto layout = bvn_fex32::computeReservationLayout(
        0x7000000000ULL, 0x100000000ULL, 0x100000000ULL);
    CHECK(layout.has_value());
    CHECK_EQ(layout->aligned, std::uint64_t{0x7000000000ULL});
    CHECK_EQ(layout->prefix, std::uint64_t{0});
    CHECK_EQ(layout->suffix, std::uint64_t{0x100000000ULL});
}

BOXEDVN_TEST(fex32_darwin_reservation_layout_rejects_overflow) {
    CHECK(!bvn_fex32::computeReservationLayout(
               0, std::numeric_limits<std::uint64_t>::max(), 2)
               .has_value());
    CHECK(!bvn_fex32::computeReservationLayout(
               std::numeric_limits<std::uint64_t>::max() - 0x1000,
               0x1000, 0x100000000ULL)
               .has_value());
}
