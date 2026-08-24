/*
 * BoxedVN - FEX exit-dispatch ABI contract.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H
#define BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H

#include <cstddef>
#include <cstdint>

namespace boxedvn {

// FEX emits this record immediately after an indirect call to the function in
// CpuStateFrame::Pointers.ExitFunctionLink. The definition is private to
// FEXCore, so the integration keeps the exact public ABI shape here.
struct FexExitFunctionLinkData final {
    std::uint64_t hostCode;
    std::uint64_t guestRIP;
    std::int64_t callerOffset;
};

static_assert(sizeof(FexExitFunctionLinkData) == 24);
static_assert(offsetof(FexExitFunctionLinkData, hostCode) == 0);
static_assert(offsetof(FexExitFunctionLinkData, guestRIP) == 8);
static_assert(offsetof(FexExitFunctionLinkData, callerOffset) == 16);

struct FexExitDispatchTransition final {
    std::uint64_t guestRIP;
    std::uint64_t hostTarget;
};

// Direct block linking is optional. An integration may instead restore the
// target guest RIP and send execution through the ordinary dispatcher cache.
constexpr FexExitDispatchTransition dispatchWithoutBlockLinking(
    const FexExitFunctionLinkData& record,
    std::uint64_t dispatcherLoopTop) noexcept {
    return {record.guestRIP, dispatcherLoopTop};
}

}  // namespace boxedvn

#endif  // BOXEDVN_FEX_EXIT_DISPATCH_CONTRACT_H
