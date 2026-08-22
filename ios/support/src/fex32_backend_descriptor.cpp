/*
 * BoxedVN - host-independent FEX32 backend capability contract.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#include "boxedvn/fex32_backend_descriptor.h"

#include <limits>

namespace boxedvn {

namespace {

bool isFlag(std::uint8_t value) noexcept {
    return value == 0 || value == 1;
}

bool isZeroReserved(const Fex32BackendDescriptor& descriptor) noexcept {
    return descriptor.reserved[0] == 0 && descriptor.reserved[1] == 0;
}

bool hasValidCommonFields(const Fex32BackendDescriptor& descriptor) noexcept {
    return descriptor.version == Fex32BackendDescriptor::kVersion &&
           descriptor.structSize == Fex32BackendDescriptor::kStructSize &&
           descriptor.guestPointerBits ==
               Fex32BackendDescriptor::kGuestPointerBits &&
           descriptor.syscallAbi == Fex32SyscallAbi::LinuxI386 &&
           isFlag(descriptor.directCodeAccess) &&
           isFlag(descriptor.directDataAccess) &&
           isFlag(descriptor.nativeIdentityRequired) &&
           isFlag(descriptor.lowAddressRequired) &&
           isFlag(descriptor.backendAvailable) &&
           isZeroReserved(descriptor);
}

bool hasValidAddressMode(const Fex32BackendDescriptor& descriptor) noexcept {
    return descriptor.addressMode == GuestAddressContract32::Mode::Paged ||
           descriptor.addressMode ==
               GuestAddressContract32::Mode::BiasedDirect;
}

}  // namespace

Fex32BackendStatus validateFex32BackendDescriptor(
    const Fex32BackendDescriptor& descriptor) noexcept {
    if (!hasValidCommonFields(descriptor) ||
        !hasValidAddressMode(descriptor)) {
        return Fex32BackendStatus::Invalid;
    }

    // This reuses the same checked U64 bias/window arithmetic as the address
    // conversion layer, including the 4 GiB upper bound and host overflow.
    if (!GuestAddressContract32::create(descriptor.addressMode,
                                        descriptor.hostBias,
                                        descriptor.hostWindow)
             .has_value()) {
        return Fex32BackendStatus::Invalid;
    }

    if (descriptor.addressMode == GuestAddressContract32::Mode::Paged) {
        // Paged mode has no translator-visible flat pointer window. Keeping
        // its address metadata empty prevents callers from treating a hint as
        // an actual direct mapping.
        if (descriptor.hostBias != 0 || descriptor.hostWindow != 0 ||
            descriptor.directCodeAccess != 0 ||
            descriptor.directDataAccess != 0 ||
            descriptor.nativeIdentityRequired != 0 ||
            descriptor.lowAddressRequired != 0) {
            return Fex32BackendStatus::Invalid;
        }
    } else {
        // A direct FEX32 translator must be able to fetch instructions and
        // access data through the same complete 32-bit host window.
        if (descriptor.directCodeAccess != 1 ||
            descriptor.directDataAccess != 1 ||
            descriptor.hostWindow !=
                Fex32BackendDescriptor::kFullGuestWindow ||
            descriptor.hostBias % Fex32BackendDescriptor::kFullGuestWindow !=
                0) {
            return Fex32BackendStatus::Invalid;
        }

        // Native identity and low-address requirements are stronger than a
        // general biased mapping: both require the complete window at zero.
        if ((descriptor.nativeIdentityRequired != 0 ||
             descriptor.lowAddressRequired != 0) &&
            descriptor.hostBias != 0) {
            return Fex32BackendStatus::Invalid;
        }
    }

    return descriptor.backendAvailable != 0
               ? Fex32BackendStatus::ValidAvailable
               : Fex32BackendStatus::ValidUnavailable;
}

}  // namespace boxedvn
