/*
 * BoxedVN - host-independent FEX32 backend capability contract.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX32_BACKEND_DESCRIPTOR_H
#define BOXEDVN_FEX32_BACKEND_DESCRIPTOR_H

#include "boxedvn/guest_address_contract32.h"

#include <cstdint>
#include <type_traits>

namespace boxedvn {

enum class Fex32SyscallAbi : std::uint32_t {
    LinuxI386 = 1,
};

// Keep malformed descriptors separate from a descriptor that is structurally
// sound but whose translator was not compiled into this process.
enum class Fex32BackendStatus : std::uint8_t {
    Invalid,
    ValidUnavailable,
    ValidAvailable,
};

struct Fex32BackendDescriptor final {
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::uint32_t kStructSize = 40;
    static constexpr std::uint32_t kGuestPointerBits =
        GuestAddressContract32::kGuestPointerBits;
    static constexpr std::uint64_t kFullGuestWindow =
        GuestAddressContract32::kGuestAddressLimit;

    // These fields intentionally use fixed-width values: the descriptor is a
    // contract, not a host-language bool layout or a pointer-bearing object.
    std::uint32_t version = kVersion;
    std::uint32_t structSize = kStructSize;
    std::uint32_t guestPointerBits = kGuestPointerBits;
    Fex32SyscallAbi syscallAbi = Fex32SyscallAbi::LinuxI386;

    GuestAddressContract32::Mode addressMode =
        GuestAddressContract32::Mode::Paged;
    std::uint8_t directCodeAccess = 0;
    std::uint8_t directDataAccess = 0;
    std::uint8_t nativeIdentityRequired = 0;
    std::uint8_t lowAddressRequired = 0;
    std::uint8_t backendAvailable = 0;
    std::uint8_t reserved[2] = {};

    std::uint64_t hostBias = 0;
    std::uint64_t hostWindow = 0;
};

static_assert(std::is_standard_layout_v<Fex32BackendDescriptor>);
static_assert(sizeof(Fex32BackendDescriptor) ==
              Fex32BackendDescriptor::kStructSize);

Fex32BackendStatus validateFex32BackendDescriptor(
    const Fex32BackendDescriptor& descriptor) noexcept;

}  // namespace boxedvn

#endif  // BOXEDVN_FEX32_BACKEND_DESCRIPTOR_H
