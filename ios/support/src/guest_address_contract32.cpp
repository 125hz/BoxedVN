/*
 * BoxedVN - checked address contract for a 32-bit guest.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/guest_address_contract32.h"

#include <limits>

namespace boxedvn {

bool GuestAddressContract32::checkedAdd(std::uint64_t left,
                                        std::uint64_t right,
                                        std::uint64_t& result) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

std::optional<GuestAddressContract32> GuestAddressContract32::create(
    Mode mode, std::uint64_t hostBias, std::uint64_t hostWindow) noexcept {
    if (mode != Mode::Paged && mode != Mode::BiasedDirect) {
        return std::nullopt;
    }
    if (hostWindow > kGuestAddressLimit) {
        return std::nullopt;
    }

    if (hostBias > std::numeric_limits<std::uint64_t>::max() - hostWindow) {
        return std::nullopt;
    }

    return GuestAddressContract32(mode, hostBias, hostWindow);
}

bool GuestAddressContract32::validGuestRange(GuestAddress32 base,
                                             std::uint64_t length) noexcept {
    if (length == 0) {
        return false;
    }

    const std::uint64_t guestBase = base.value;
    return guestBase < kGuestAddressLimit &&
           length <= kGuestAddressLimit - guestBase;
}

std::optional<HostAddress64> GuestAddressContract32::guestToHost(
    GuestAddress32 guestAddress, std::uint64_t length) const noexcept {
    if (mode_ != Mode::BiasedDirect ||
        !validGuestRange(guestAddress, length)) {
        return std::nullopt;
    }

    const std::uint64_t guestBase = guestAddress.value;
    if (guestBase >= hostWindow_ || length > hostWindow_ - guestBase) {
        return std::nullopt;
    }

    std::uint64_t hostAddress = 0;
    if (!checkedAdd(hostBias_, guestBase, hostAddress)) {
        return std::nullopt;
    }
    return HostAddress64(hostAddress);
}

std::optional<GuestAddress32> GuestAddressContract32::hostToGuest(
    HostAddress64 hostAddress, std::uint64_t length) const noexcept {
    if (mode_ != Mode::BiasedDirect || hostAddress.value < hostBias_) {
        return std::nullopt;
    }

    const std::uint64_t guestBase = hostAddress.value - hostBias_;
    if (guestBase >= kGuestAddressLimit || guestBase >= hostWindow_ ||
        length > hostWindow_ - guestBase ||
        !validGuestRange(GuestAddress32(
                             static_cast<std::uint32_t>(guestBase)),
                         length)) {
        return std::nullopt;
    }

    return GuestAddress32(static_cast<std::uint32_t>(guestBase));
}

}  // namespace boxedvn
