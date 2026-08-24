#include "boxedvn_test.h"
#include "boxedvn/fex_guest_mode_policy.h"

#include <string>

using namespace boxedvn;

BOXEDVN_TEST(fex_guest_mode_policy_admits_only_current_64bit_bridge) {
    CHECK(fexGuestModeAdmitted(FexGuestMode::X86_64));
    CHECK(!fexGuestModeAdmitted(FexGuestMode::X86_32));
    CHECK(fexGuestModeAdmission(FexGuestMode::X86_32) ==
          FexGuestModeAdmission::RejectedNoLinux32Adapter);
}

BOXEDVN_TEST(fex_guest_mode_policy_names_modes_explicitly) {
    CHECK(std::string(fexGuestModeName(FexGuestMode::X86_32)) == "x86-32");
    CHECK(std::string(fexGuestModeName(FexGuestMode::X86_64)) == "x86-64");
}
