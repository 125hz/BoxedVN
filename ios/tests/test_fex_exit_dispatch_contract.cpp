#include "boxedvn_test.h"
#include "boxedvn/fex_exit_dispatch_contract.h"

#include <cstddef>
#include <cstdint>

using namespace boxedvn;

BOXEDVN_TEST(fex_exit_link_record_has_the_pinned_abi_layout) {
    CHECK_EQ(sizeof(FexExitFunctionLinkData), std::size_t{24});
    CHECK_EQ(offsetof(FexExitFunctionLinkData, hostCode), std::size_t{0});
    CHECK_EQ(offsetof(FexExitFunctionLinkData, guestRIP), std::size_t{8});
    CHECK_EQ(offsetof(FexExitFunctionLinkData, callerOffset), std::size_t{16});
}

BOXEDVN_TEST(fex_unlinked_exit_returns_to_the_dispatcher_at_guest_target) {
    const FexExitFunctionLinkData record {
        0x1111222233334444ULL,
        0x7e0000123456ULL,
        -20,
    };
    const auto transition =
        dispatchWithoutBlockLinking(record, 0x5555666677778888ULL);

    CHECK_EQ(transition.guestRIP, record.guestRIP);
    CHECK_EQ(transition.hostTarget, 0x5555666677778888ULL);
}
