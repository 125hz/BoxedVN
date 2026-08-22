#include "boxedvn_test.h"
#include "boxedvn/guest_address_contract32.h"

#include <cstdint>
#include <limits>
#include <type_traits>

using namespace boxedvn;

BOXEDVN_TEST(guest32_contract_has_explicit_pointer_width) {
    static_assert(GuestAddressContract32::kGuestPointerBits == 32);
    static_assert(sizeof(GuestAddress32) == 4);
    static_assert(sizeof(HostAddress64) == 8);
    static_assert(!std::is_convertible_v<GuestAddress32, void*>);
    static_assert(!std::is_convertible_v<HostAddress64, void*>);
    CHECK_EQ(GuestAddressContract32::kGuestAddressLimit,
             std::uint64_t{0x100000000});
}

BOXEDVN_TEST(guest32_contract_translates_zero_and_page_aligned_biases) {
    const auto zeroBias = GuestAddressContract32::create(
        GuestAddressContract32::Mode::BiasedDirect, 0, 0x100000000ULL);
    CHECK(zeroBias.has_value());
    CHECK_EQ(zeroBias->guestToHost(GuestAddress32(0), 1)->value, 0ULL);
    CHECK_EQ(zeroBias->hostToGuest(HostAddress64(0), 1)->value, 0U);

    constexpr std::uint64_t highBias = 0x7000000000ULL;
    const auto pageBias = GuestAddressContract32::create(
        GuestAddressContract32::Mode::BiasedDirect, highBias,
        0x100000000ULL);
    CHECK(pageBias.has_value());
    CHECK_EQ(pageBias->guestToHost(GuestAddress32(0x1000), 0x1000)->value,
             highBias + 0x1000ULL);
    CHECK_EQ(pageBias->hostToGuest(HostAddress64(highBias + 0x1000),
                                   0x1000)->value,
             0x1000U);
}

BOXEDVN_TEST(guest32_contract_accepts_the_maximum_guest_byte) {
    const auto contract = GuestAddressContract32::create(
        GuestAddressContract32::Mode::BiasedDirect, 0x400000000ULL,
        0x100000000ULL);
    CHECK(contract.has_value());
    CHECK(GuestAddressContract32::validGuestRange(
        GuestAddress32(0xffffffffU), 1));
    CHECK_EQ(contract->guestToHost(GuestAddress32(0xffffffffU), 1)->value,
             0x4ffffffffULL);
    CHECK_EQ(contract->hostToGuest(HostAddress64(0x4ffffffffULL), 1)->value,
             0xffffffffU);
    CHECK(!GuestAddressContract32::validGuestRange(
        GuestAddress32(0xffffffffU), 2));
    CHECK(!contract->guestToHost(GuestAddress32(0xffffffffU), 2).has_value());
}

BOXEDVN_TEST(guest32_contract_rejects_guest_range_overflow) {
    CHECK(!GuestAddressContract32::validGuestRange(GuestAddress32(0), 0));
    CHECK(!GuestAddressContract32::validGuestRange(
        GuestAddress32(0x1000), 0x100000000ULL));
    CHECK(!GuestAddressContract32::validGuestRange(
        GuestAddress32(0xfffffff0U), 0x20));

    const auto narrow = GuestAddressContract32::create(
        GuestAddressContract32::Mode::BiasedDirect, 0x100000000ULL, 0x2000);
    CHECK(narrow.has_value());
    CHECK(narrow->guestToHost(GuestAddress32(0x1000), 0x1000).has_value());
    CHECK(!narrow->guestToHost(GuestAddress32(0x1000), 0x1001).has_value());
}

BOXEDVN_TEST(guest32_contract_rejects_host_bias_and_window_overflow) {
    CHECK(!GuestAddressContract32::create(
               GuestAddressContract32::Mode::BiasedDirect,
               std::numeric_limits<std::uint64_t>::max(), 1)
               .has_value());
    CHECK(!GuestAddressContract32::create(
               GuestAddressContract32::Mode::BiasedDirect,
               std::numeric_limits<std::uint64_t>::max() - 3, 8)
               .has_value());
    CHECK(!GuestAddressContract32::create(
               GuestAddressContract32::Mode::BiasedDirect, 0, 0x100000001ULL)
               .has_value());

    const auto edge = GuestAddressContract32::create(
        GuestAddressContract32::Mode::BiasedDirect,
        std::numeric_limits<std::uint64_t>::max() - 0x1000, 0x1000);
    CHECK(edge.has_value());
    CHECK(edge->guestToHost(GuestAddress32(0xfff), 1).has_value());
    CHECK(!edge->guestToHost(GuestAddress32(0xfff), 2).has_value());
    CHECK(!edge->hostToGuest(
                HostAddress64(std::numeric_limits<std::uint64_t>::max()), 2)
                .has_value());
    CHECK(!edge->hostToGuest(
                HostAddress64(std::numeric_limits<std::uint64_t>::max()), 1)
                .has_value());
}

BOXEDVN_TEST(guest32_contract_keeps_paged_mode_non_direct) {
    const auto paged = GuestAddressContract32::create(
        GuestAddressContract32::Mode::Paged, 0x7000000000ULL,
        0x100000000ULL);
    CHECK(paged.has_value());
    CHECK(GuestAddressContract32::validGuestRange(GuestAddress32(0x2000),
                                                  0x1000));
    CHECK(!paged->guestToHost(GuestAddress32(0x2000), 0x1000).has_value());
    CHECK(!paged->hostToGuest(HostAddress64(0x7000002000ULL), 0x1000)
               .has_value());
}

BOXEDVN_TEST(guest32_contract_rejects_unknown_mode) {
    CHECK(!GuestAddressContract32::create(
               static_cast<GuestAddressContract32::Mode>(0xff), 0, 0)
               .has_value());
}
