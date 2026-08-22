/*
 *  BoxedVN - iOS/iPadOS port of Boxedwine
 *  Copyright (C) 2026  The BoxedWine Team
 *  GPLv2; see license.txt.
 */

#include "boxedvn_test.h"
#include "krandom.h"

#include <array>
#include <algorithm>
#include <cstdint>

BOXEDVN_TEST(kernel_random_rejects_invalid_destination) {
    CHECK(KRandom::fill(nullptr, 1) == KRandom::Source::Invalid);
    CHECK(KRandom::fill(nullptr, 0) == KRandom::Source::System);
}

BOXEDVN_TEST(kernel_random_produces_nonzero_bytes_repeatedly) {
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 32> second{};

    CHECK(KRandom::fill(first.data(), first.size()) != KRandom::Source::Invalid);
    CHECK(KRandom::fill(second.data(), second.size()) != KRandom::Source::Invalid);
    CHECK(std::any_of(first.begin(), first.end(), [](std::uint8_t value) {
        return value != 0;
    }));
    CHECK(std::any_of(second.begin(), second.end(), [](std::uint8_t value) {
        return value != 0;
    }));
}
