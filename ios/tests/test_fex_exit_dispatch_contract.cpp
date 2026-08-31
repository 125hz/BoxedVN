#include "boxedvn_test.h"
#include "boxedvn/fex_exit_dispatch_contract.h"
#include "guest_low_alias.h"

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

// ---------------------------------------------------------------------------
// The indirect-exit lookup. A device run branched to host address zero out of
// the `leave; mov eax, edx; ret` epilogue of glibc's __libc_sigaction at guest
// 0x7a4004d4b9, on a thread whose stack was in Wine's top-down arena.
// ---------------------------------------------------------------------------

// The frame the device reported, so the arithmetic below is checked against
// real numbers rather than invented ones.
namespace {
constexpr std::uint64_t kDeviceRBP = 0x7ffffe1fe938ULL;
// The host address the faulting block's TMP4 (x3) actually held.
constexpr std::uint64_t kDeviceReturnSlotHost = 0x7ffe1fe940ULL;
constexpr std::uint64_t kDispatcherLoopTop = 0x0000000123456780ULL;
}  // namespace

BOXEDVN_TEST(fex_indirect_exit_never_branches_to_a_null_host_target) {
    // The exact failure: an empty slot, and a dynamic guest target of zero.
    // Comparing the key alone made this a hit whose host target was zero.
    const auto exit = resolveIndirectExit(emptyLookupCacheEntry(), 0,
                                          kDispatcherLoopTop);
    CHECK(exit.path == FexIndirectExitPath::DispatcherFallback);
    CHECK_EQ(exit.hostTarget, kDispatcherLoopTop);
    CHECK(exit.hostTarget != 0);
    // The target is carried through unchanged, so the dispatcher resolves the
    // address the guest actually asked for -- and a guest with no executable
    // mapping at zero faults there, as a guest fault, rather than the host
    // branching to page zero.
    CHECK_EQ(exit.guestRIP, std::uint64_t{0});
}

BOXEDVN_TEST(fex_indirect_exit_refuses_a_null_host_target_for_any_key) {
    // Not just zero: any entry whose host half is null is an empty entry, and
    // no key may be believed against one.
    for (std::uint64_t target : {std::uint64_t{0}, std::uint64_t{0x1000},
                                 std::uint64_t{0x7a4004d4b9},
                                 std::uint64_t{0x7ffffe1fe940}}) {
        const FexLookupCacheEntry entry {0, target};
        const auto exit =
            resolveIndirectExit(entry, target, kDispatcherLoopTop);
        CHECK(exit.path == FexIndirectExitPath::DispatcherFallback);
        CHECK_EQ(exit.hostTarget, kDispatcherLoopTop);
    }
}

BOXEDVN_TEST(fex_indirect_exit_takes_a_real_cached_block) {
    // The ordinary path is unchanged: a matching key with a real host pointer
    // is still a direct branch, with no dispatcher round trip.
    const FexLookupCacheEntry entry {0x00000001a45adf8cULL, 0x7a4004d4bcULL};
    const auto exit =
        resolveIndirectExit(entry, 0x7a4004d4bcULL, kDispatcherLoopTop);
    CHECK(exit.path == FexIndirectExitPath::CacheHit);
    CHECK_EQ(exit.hostTarget, entry.hostCode);
    CHECK_EQ(exit.guestRIP, entry.guestCode);
}

BOXEDVN_TEST(fex_indirect_exit_misses_on_a_key_that_does_not_match) {
    const FexLookupCacheEntry entry {0x00000001a45adf8cULL, 0x7a4004d4bcULL};
    const auto exit =
        resolveIndirectExit(entry, 0x7a4004d500ULL, kDispatcherLoopTop);
    CHECK(exit.path == FexIndirectExitPath::DispatcherFallback);
    CHECK_EQ(exit.hostTarget, kDispatcherLoopTop);
    CHECK(exit.hostTarget != 0);
}

BOXEDVN_TEST(fex_invalidated_cache_entry_keeps_no_dead_code_pointer) {
    // Invalidation used to zero only the key and deliberately leave the host
    // pointer in place. A zero key then matched a target of zero and produced
    // a branch into code that no longer existed. Both halves go.
    const FexLookupCacheEntry live {0x00000001a45adf8cULL, 0x7a4004d4bcULL};
    const auto dead = invalidateLookupCacheEntry(live, 0x7a4004d4bcULL);
    CHECK_EQ(dead.guestCode, std::uint64_t{0});
    CHECK_EQ(dead.hostCode, std::uint64_t{0});

    // And a zero target against it is a miss, not a branch.
    const auto exit = resolveIndirectExit(dead, 0, kDispatcherLoopTop);
    CHECK(exit.path == FexIndirectExitPath::DispatcherFallback);

    // An entry for a different address is left alone.
    const auto untouched = invalidateLookupCacheEntry(live, 0x7a4004d500ULL);
    CHECK_EQ(untouched.hostCode, live.hostCode);
    CHECK_EQ(untouched.guestCode, live.guestCode);
}

BOXEDVN_TEST(fex_leave_ret_frame_arithmetic_matches_the_device) {
    // The device's frame, and the host address its RET actually loaded from.
    CHECK_EQ(fexSavedFramePointerSlot(kDeviceRBP), kDeviceRBP);
    CHECK_EQ(fexReturnSlotForFrame(kDeviceRBP), 0x7ffffe1fe940ULL);
    CHECK_EQ(fexStackPointerAfterLeaveRet(kDeviceRBP), 0x7ffffe1fe948ULL);
    // Which is exactly the host address the faulting block held in TMP4, so
    // the address the RET read from was right and the value it read is the
    // remaining question.
    CHECK_EQ(guestToHostAddress(fexReturnSlotForFrame(kDeviceRBP)),
             kDeviceReturnSlotHost);
    // And the slot to re-read, given the stack pointer a RET leaves behind.
    CHECK_EQ(fexPoppedSlotForStackPointer(
                 fexStackPointerAfterLeaveRet(kDeviceRBP)),
             fexReturnSlotForFrame(kDeviceRBP));
}

BOXEDVN_TEST(fex_leave_ret_frame_arithmetic_holds_on_the_low_alias_lane) {
    // The same epilogue on a canonical low stack, which reaches memory through
    // the OR alias instead of the arena's cleared field. Both lanes have to
    // agree about where the return address lives.
    constexpr std::uint64_t lowRBP = 0x7ffc7f00ULL;
    CHECK_EQ(fexReturnSlotForFrame(lowRBP), 0x7ffc7f08ULL);
    CHECK_EQ(guestToHostAddress(fexReturnSlotForFrame(lowRBP)),
             kGuestLowAliasBase + 0x7ffc7f08ULL);
    CHECK_EQ(fexStackPointerAfterLeaveRet(lowRBP), 0x7ffc7f10ULL);
}

BOXEDVN_TEST(fex_nested_call_frames_walk_back_out_in_order) {
    // Three nested frames on the arena stack, the shape the VIXL fixture runs:
    // each LEAVE/RET pair has to hand the stack pointer back exactly where the
    // caller left it, on both lanes.
    for (std::uint64_t base : {kDeviceRBP, std::uint64_t{0x7ffc7f00ULL}}) {
        std::uint64_t rbp = base;
        std::uint64_t frames[3] = {};
        for (int level = 0; level < 3; ++level) {
            frames[level] = rbp;
            // Each callee's frame sits below its caller's: the pushed return
            // address and saved frame pointer, plus a local area.
            rbp = rbp - 0x30;
        }
        for (int level = 2; level >= 0; --level) {
            CHECK_EQ(fexStackPointerAfterLeaveRet(frames[level]),
                     frames[level] + 16);
            CHECK_EQ(guestToHostAddress(fexReturnSlotForFrame(frames[level])),
                     guestToHostAddress(frames[level] + 8));
        }
    }
}

// ---------------------------------------------------------------------------
// The CALL return-address push, and the read-back that says whether it landed.
//
// The frame the second device pair reported: RSP 0x7ffffe1fe948 after the RET,
// the consumed slot 0x7ffffe1fe940 holding zero, and the saved frame pointer
// below it at 0x7ffffe1fe938 holding 0x7ffffe1ff1b8 -- correct.
// ---------------------------------------------------------------------------

namespace {
constexpr std::uint64_t kDeviceReturnSlot = 0x7ffffe1fe940ULL;
constexpr std::uint64_t kDeviceStackAfterRet = 0x7ffffe1fe948ULL;
constexpr std::uint64_t kPlausibleReturn = 0x7a4004d4bcULL;

constexpr FexCallReturnPush devicePush(std::uint64_t readback) {
    return FexCallReturnPush {
        kPlausibleReturn,
        kDeviceReturnSlot,
        guestToHostAddress(kDeviceReturnSlot),
        readback,
    };
}
}  // namespace

BOXEDVN_TEST(fex_call_push_readback_separates_the_two_causes) {
    // A push that landed. A slot that later reads zero is then somebody else's
    // write, and the CALL is exonerated.
    CHECK(classifyCallReturnPush(devicePush(kPlausibleReturn)) ==
          FexCallPushVerdict::Stored);
    // A push that did not land at all: the exact shape the device stack shows.
    CHECK(classifyCallReturnPush(devicePush(0)) ==
          FexCallPushVerdict::NotStored);
    // A push that landed somewhere, but not with the value it named.
    CHECK(classifyCallReturnPush(devicePush(0x1122334455667788ULL)) ==
          FexCallPushVerdict::Mismatched);
}

BOXEDVN_TEST(fex_call_push_record_is_well_formed_or_is_itself_the_anomaly) {
    CHECK(fexCallReturnPushIsWellFormed(devicePush(kPlausibleReturn)));
    // A CALL return address is the instruction after the call and can never be
    // zero, so a record claiming otherwise is a defect in the witness, not
    // evidence about the stack.
    FexCallReturnPush broken = devicePush(0);
    broken.intendedReturn = 0;
    CHECK(!fexCallReturnPushIsWellFormed(broken));
    // Nor can the slot it names be null in either address space.
    broken = devicePush(0);
    broken.slotHost = 0;
    CHECK(!fexCallReturnPushIsWellFormed(broken));
}

BOXEDVN_TEST(fex_call_push_slot_matches_the_stack_pointer_after_the_push) {
    // The slot a CALL pushes to is the stack pointer it leaves behind, and the
    // slot the matching RET consumes is one qword below the pointer IT leaves
    // behind. Those have to be the same address or the witness is describing a
    // different push than the one that failed.
    CHECK_EQ(fexPoppedSlotForStackPointer(kDeviceStackAfterRet),
             kDeviceReturnSlot);
    const auto push = devicePush(kPlausibleReturn);
    CHECK_EQ(push.slotGuest, fexPoppedSlotForStackPointer(kDeviceStackAfterRet));
    CHECK_EQ(push.slotHost, guestToHostAddress(push.slotGuest));
}

BOXEDVN_TEST(fex_push_overlap_shapes_round_trip_through_the_packed_info) {
    // Every shape the Push lowering can take has to be recoverable from the
    // record, because a register-allocation shape that only occurs on device is
    // exactly what a host test cannot reproduce.
    struct Shape {
        bool valueIsAddress;
        bool valueIsResult;
        bool valueCopied;
    };
    const Shape shapes[] = {
        {false, false, false},
        {true, false, true},
        {false, true, true},
        {true, true, true},
    };
    for (const Shape& shape : shapes) {
        const std::uint64_t info = fexPushInfo(
            8, shape.valueIsAddress, shape.valueIsResult, shape.valueCopied);
        CHECK_EQ(fexPushInfoValueSize(info), std::uint64_t{8});
        CHECK(fexPushInfoHas(info, FexPushOverlap::ValueIsAddress) ==
              shape.valueIsAddress);
        CHECK(fexPushInfoHas(info, FexPushOverlap::ValueIsResult) ==
              shape.valueIsResult);
        CHECK(fexPushInfoHas(info, FexPushOverlap::ValueCopied) ==
              shape.valueCopied);
    }
    // The size field does not collide with the overlap bits.
    const std::uint64_t info = fexPushInfo(4, true, true, true);
    CHECK_EQ(fexPushInfoValueSize(info), std::uint64_t{4});
}

BOXEDVN_TEST(fex_null_guest_target_takes_the_guest_fault_path) {
    // Refusing to compile a null-page RIP is right; handing the dispatcher a
    // null host pointer to branch to instead is what turned that refusal into a
    // host crash at PC 0. The disposition is a guest fault, never a compile.
    for (std::uint64_t rip : {std::uint64_t{0}, std::uint64_t{0x8},
                              std::uint64_t{0x100}, std::uint64_t{0xfff}}) {
        CHECK(dispositionForGuestTarget(rip) ==
              FexInvalidTargetDisposition::GuestFault);
        CHECK(dispositionForGuestTarget(rip) !=
              FexInvalidTargetDisposition::Compile);
    }
    // Ordinary guest addresses are unaffected, on every lane.
    for (std::uint64_t rip : {std::uint64_t{0x1000}, std::uint64_t{0x10000},
                              std::uint64_t{0x7a4004d4bcULL},
                              std::uint64_t{0x7ffffe1fe940ULL}}) {
        CHECK(dispositionForGuestTarget(rip) ==
              FexInvalidTargetDisposition::Compile);
    }
}

BOXEDVN_TEST(fex_a_null_host_target_is_never_executable) {
    // Whatever produced it -- an empty cache slot, a refused compile, an
    // internal failure -- a null host pointer is never something to branch to.
    CHECK(!fexHostTargetIsExecutable(0));
    CHECK(fexHostTargetIsExecutable(0x00000001a45adf8cULL));
    // And the dispatcher fallback target itself must be executable, or the
    // miss path would be as bad as the hit path it replaced.
    const auto exit = resolveIndirectExit(emptyLookupCacheEntry(), 0,
                                          kDispatcherLoopTop);
    CHECK(fexHostTargetIsExecutable(exit.hostTarget));
}
