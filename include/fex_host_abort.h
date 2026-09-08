#pragma once
#include <cstdint>

namespace boxedvn {
// ESR_EL1 as saved by Darwin in arm_exception_state64_t. si_code alone does
// not distinguish a data-abort permission failure from an alignment fault.
constexpr bool armDataAbort(uint32_t esr) {
    return (esr >> 26) == 0x24 || (esr >> 26) == 0x25;
}
constexpr bool armWriteAbort(uint32_t esr) {
    return armDataAbort(esr) && (esr & (1u << 6));
}
constexpr bool armAlignmentAbort(uint32_t esr) {
    return armDataAbort(esr) && (esr & 0x3f) == 0x21;
}
constexpr uint32_t guestPageFaultError(bool mapped, bool write) {
    return 4u | (mapped ? 1u : 0u) | (write ? 2u : 0u);
}
}
