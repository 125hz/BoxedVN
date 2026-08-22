#include "boxedvn_test.h"
#include "boxedvn/fex32_backend_admission.h"

#include <string>

using namespace boxedvn;

namespace {

Fex32BackendDescriptor availableDescriptor() {
    Fex32BackendDescriptor descriptor;
    descriptor.addressMode = GuestAddressContract32::Mode::BiasedDirect;
    descriptor.hostBias = 0x7000000000ULL;
    descriptor.hostWindow = Fex32BackendDescriptor::kFullGuestWindow;
    descriptor.directCodeAccess = 1;
    descriptor.directDataAccess = 1;
    descriptor.backendAvailable = 1;
    return descriptor;
}

}  // namespace

BOXEDVN_TEST(fex32_selection_requires_explicit_opt_in) {
    const Fex32BackendSelectionResult result =
        selectFex32Backend(availableDescriptor(), false);
    CHECK(result.status == Fex32BackendSelectionStatus::NotRequested);
    CHECK(!result.selected());
    CHECK_CONTAINS(result.reason, "not requested");
}

BOXEDVN_TEST(fex32_selection_admits_only_valid_available_descriptor) {
    const Fex32BackendSelectionResult result =
        selectFex32Backend(availableDescriptor(), true);
    CHECK(result.status == Fex32BackendSelectionStatus::Selected);
    CHECK(result.selected());
    CHECK_CONTAINS(result.reason, "admitted");
}

BOXEDVN_TEST(fex32_selection_reports_valid_but_unavailable) {
    Fex32BackendDescriptor descriptor = availableDescriptor();
    descriptor.backendAvailable = 0;

    const Fex32BackendSelectionResult result =
        selectFex32Backend(descriptor, true);
    CHECK(result.status == Fex32BackendSelectionStatus::Unavailable);
    CHECK(!result.selected());
    CHECK_CONTAINS(result.reason, "unavailable");
}

BOXEDVN_TEST(fex32_selection_rejects_unsupported_direct_mapping) {
    Fex32BackendDescriptor descriptor = availableDescriptor();
    descriptor.hostWindow--;

    const Fex32BackendSelectionResult result =
        selectFex32Backend(descriptor, true);
    CHECK(result.status == Fex32BackendSelectionStatus::InvalidDescriptor);
    CHECK(!result.selected());
    CHECK_CONTAINS(result.reason, "invalid");
}

BOXEDVN_TEST(fex32_selection_rejects_paged_direct_flags_even_when_available) {
    Fex32BackendDescriptor descriptor;
    descriptor.directCodeAccess = 1;
    descriptor.backendAvailable = 1;

    const Fex32BackendSelectionResult result =
        selectFex32Backend(descriptor, true);
    CHECK(result.status == Fex32BackendSelectionStatus::InvalidDescriptor);
    CHECK(!result.selected());
    CHECK_CONTAINS(result.reason, "address mode");
}
