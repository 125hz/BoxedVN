#include "boxedvn_test.h"
#include "fex_callret_guard.h"

namespace {
constexpr std::uint64_t base = 0x7029c14000ULL;
constexpr std::uint64_t size = 0x1000000;
constexpr std::uint64_t reset = base + size / 4;
}

BOXEDVN_TEST(fex_callret_guard_recovers_device_stp_without_skipping_instruction) {
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base,
        0x7029c13ff0ULL, 0xa9bf7f3fU), reset);
    // Same pre-index instruction storing two real prediction registers.
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base,
        base - 8, 0xa9bf0f22U), reset);
}

BOXEDVN_TEST(fex_callret_guard_recovers_pop_into_upper_guard) {
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base + size,
        base + size + 8, 0xa8c10f22U), reset);
}

BOXEDVN_TEST(fex_callret_guard_rejects_guest_faults_and_wrong_access) {
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base, 0, 0xa9bf7f3fU), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base + 16, base, 0xa9bf7f3fU), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base, base - 16, 0xa9bf7f1fU), 0ULL); // x24
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base, base - 16, 0xa9017f3fU), 0ULL); // offset STP
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base + 16, base - 16, 0xa9bf7f3fU), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base + 1, base - 15, 0xa9bf7f3fU), 0ULL);
}

BOXEDVN_TEST(fex_callret_guard_checks_owned_mapping_boundaries) {
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base - 0x4000,
        base - 0x4010, 0xa9bf7f3fU), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, size, base + size + 0x4000,
        base + size + 0x4000, 0xa8c10f22U), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(0, size, base, base - 16, 0xa9bf7f3fU), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(base, ~0ULL, base, base - 16, 0xa9bf7f3fU), 0ULL);
    CHECK_EQ(boxedvn::recoverFexCallRetGuard(~0ULL - 15, size, base, base - 16, 0xa9bf7f3fU), 0ULL);
}
