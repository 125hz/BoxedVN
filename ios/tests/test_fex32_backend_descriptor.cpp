#include "boxedvn_test.h"
#include "boxedvn/fex32_backend_descriptor.h"

#include <cstdint>

using namespace boxedvn;

namespace {

Fex32BackendDescriptor validBiasedDescriptor() {
    Fex32BackendDescriptor descriptor;
    descriptor.addressMode = GuestAddressContract32::Mode::BiasedDirect;
    descriptor.hostBias = 0x7000000000ULL;
    descriptor.hostWindow = Fex32BackendDescriptor::kFullGuestWindow;
    descriptor.directCodeAccess = 1;
    descriptor.directDataAccess = 1;
    return descriptor;
}

}  // namespace

BOXEDVN_TEST(fex32_descriptor_has_fixed_abi_shape) {
    static_assert(Fex32BackendDescriptor::kVersion == 1);
    static_assert(Fex32BackendDescriptor::kStructSize == 40);
    static_assert(Fex32BackendDescriptor::kGuestPointerBits == 32);
    static_assert(Fex32BackendDescriptor::kFullGuestWindow == 0x100000000ULL);
    CHECK_EQ(sizeof(Fex32BackendDescriptor), std::size_t{40});
}

BOXEDVN_TEST(fex32_descriptor_distinguishes_unavailable_from_available) {
    const Fex32BackendDescriptor unavailable = validBiasedDescriptor();
    CHECK(validateFex32BackendDescriptor(unavailable) ==
          Fex32BackendStatus::ValidUnavailable);

    Fex32BackendDescriptor available = unavailable;
    available.backendAvailable = 1;
    CHECK(validateFex32BackendDescriptor(available) ==
          Fex32BackendStatus::ValidAvailable);
}

BOXEDVN_TEST(fex32_descriptor_accepts_paged_unavailable_contract) {
    Fex32BackendDescriptor descriptor;
    CHECK(descriptor.addressMode == GuestAddressContract32::Mode::Paged);
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::ValidUnavailable);
}

BOXEDVN_TEST(fex32_descriptor_rejects_direct_access_in_paged_mode) {
    Fex32BackendDescriptor descriptor;
    descriptor.directCodeAccess = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor.directCodeAccess = 0;
    descriptor.directDataAccess = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor.directDataAccess = 0;
    descriptor.nativeIdentityRequired = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor.nativeIdentityRequired = 0;
    descriptor.lowAddressRequired = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
}

BOXEDVN_TEST(fex32_descriptor_rejects_incomplete_or_misaligned_direct_window) {
    Fex32BackendDescriptor descriptor = validBiasedDescriptor();
    descriptor.hostWindow = 0x0fffff000ULL;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.hostBias += 0x1000;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.directCodeAccess = 0;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.directDataAccess = 0;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
}

BOXEDVN_TEST(fex32_descriptor_rejects_bias_and_window_overflow) {
    Fex32BackendDescriptor descriptor = validBiasedDescriptor();
    descriptor.hostBias = UINT64_MAX - 0x1000;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.hostWindow = Fex32BackendDescriptor::kFullGuestWindow - 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
}

BOXEDVN_TEST(fex32_descriptor_rejects_wrong_version_size_width_and_abi) {
    Fex32BackendDescriptor descriptor = validBiasedDescriptor();

    descriptor.version++;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
    descriptor = validBiasedDescriptor();
    descriptor.structSize--;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
    descriptor = validBiasedDescriptor();
    descriptor.guestPointerBits = 64;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
    descriptor = validBiasedDescriptor();
    descriptor.syscallAbi = static_cast<Fex32SyscallAbi>(0);
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
}

BOXEDVN_TEST(fex32_descriptor_rejects_inconsistent_requirements) {
    Fex32BackendDescriptor descriptor = validBiasedDescriptor();
    descriptor.nativeIdentityRequired = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.lowAddressRequired = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.backendAvailable = 2;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);

    descriptor = validBiasedDescriptor();
    descriptor.reserved[0] = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::Invalid);
}

BOXEDVN_TEST(fex32_descriptor_accepts_zero_identity_window) {
    Fex32BackendDescriptor descriptor;
    descriptor.addressMode = GuestAddressContract32::Mode::BiasedDirect;
    descriptor.hostWindow = Fex32BackendDescriptor::kFullGuestWindow;
    descriptor.directCodeAccess = 1;
    descriptor.directDataAccess = 1;
    descriptor.nativeIdentityRequired = 1;
    descriptor.lowAddressRequired = 1;
    CHECK(validateFex32BackendDescriptor(descriptor) ==
          Fex32BackendStatus::ValidUnavailable);
}
