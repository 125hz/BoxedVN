#include "boxedvn_test.h"
#include "guest_mmap_diagnostics.h"

#include <cstdint>

using namespace boxedvn;

// The device run this covers produced 4,194,305 mmap requests and described
// none of the ones that mattered. Every detailed line went to the loader or to
// a short-lived helper process, because the budget was one global ordinal; by
// the time the failing address space reached its first refused request the
// budget was long gone. These tests pin both halves of the fix: a helper
// cannot spend another address space's evidence, and a request repeated
// without end still says what it was, bounded.

namespace {

// The pids and address spaces the device log shows: the process that fails is
// pid 10, and it re-execs, so its post-handoff address space is a different
// generation from the one that loaded it.
constexpr std::uint64_t kMainPid = 10;
constexpr std::uint64_t kMainSpaceBeforeExec = 3;
constexpr std::uint64_t kMainSpaceAfterExec = 4;

// The helpers that drained the old global budget.
constexpr std::uint64_t kHelperPids[] = {12, 18, 20, 21, 22};

// The repeating request, as the syscall layer builds it. MAP_PRIVATE |
// MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, accessible, outside every hostable
// lane.
GuestMmapSignature repeatingRequest() {
    GuestMmapSignature signature;
    signature.processId = kMainPid;
    signature.threadId = 11;
    signature.addressSpace = kMainSpaceAfterExec;
    signature.address = 0x300000000ULL;
    signature.length = 0x10000;
    signature.protection = 0x3;
    signature.flags = 0x104022;
    return signature;
}

unsigned countReports(GuestMmapRepeatTracker& tracker,
                      const GuestMmapSignature& signature,
                      std::uint64_t times) {
    unsigned reports = 0;
    for (std::uint64_t index = 0; index < times; ++index) {
        if (tracker.record(signature).decision !=
            GuestMmapRepeatTracker::Decision::Silent) {
            ++reports;
        }
    }
    return reports;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-address-space detail budgeting.
// ---------------------------------------------------------------------------

BOXEDVN_TEST(mmap_detail_budget_gives_each_address_space_its_own_allowance) {
    GuestMmapDetailBudget budget;
    for (unsigned index = 0; index < GuestMmapDetailBudget::kLinesPerAddressSpace;
         ++index) {
        CHECK(budget.take(kMainPid, kMainSpaceAfterExec));
    }
    // Spent, and only for that address space.
    CHECK(!budget.take(kMainPid, kMainSpaceAfterExec));
    CHECK(budget.issuedFor(kMainPid, kMainSpaceAfterExec) ==
          GuestMmapDetailBudget::kLinesPerAddressSpace);
    CHECK(budget.take(kMainPid, kMainSpaceBeforeExec));
    CHECK(budget.take(kHelperPids[0], 7));
}

BOXEDVN_TEST(mmap_detail_budget_helpers_cannot_consume_the_failing_evidence) {
    // The exact failure the old global ordinal produced: the loader and five
    // helper processes issue a thousand requests between them before the
    // process under test issues its first one. Its allowance must be intact.
    GuestMmapDetailBudget budget;
    std::uint64_t space = 100;
    for (std::uint64_t pid : kHelperPids) {
        ++space;
        for (unsigned index = 0; index < 200; ++index) {
            budget.take(pid, space);
        }
    }
    CHECK(budget.issuedFor(kMainPid, kMainSpaceAfterExec) == 0);
    for (unsigned index = 0; index < GuestMmapDetailBudget::kLinesPerAddressSpace;
         ++index) {
        CHECK(budget.take(kMainPid, kMainSpaceAfterExec));
    }
}

BOXEDVN_TEST(mmap_detail_budget_treats_an_exec_as_a_fresh_address_space) {
    // execve replaces the KMemory64, so the pid before and after the Windows
    // handoff is not the same address space and does not share a budget.
    GuestMmapDetailBudget budget;
    for (unsigned index = 0; index < GuestMmapDetailBudget::kLinesPerAddressSpace;
         ++index) {
        CHECK(budget.take(kMainPid, kMainSpaceBeforeExec));
    }
    CHECK(!budget.take(kMainPid, kMainSpaceBeforeExec));
    CHECK(budget.take(kMainPid, kMainSpaceAfterExec));
    CHECK(budget.issuedFor(kMainPid, kMainSpaceAfterExec) == 1);
}

BOXEDVN_TEST(mmap_detail_budget_is_bounded_against_unbounded_address_spaces) {
    // A guest that forks without limit must not be able to make the budget
    // unbounded, whatever the per-space allowance is.
    GuestMmapDetailBudget budget;
    for (std::uint64_t space = 0; space < 100000; ++space) {
        budget.take(space % 64, space);
    }
    CHECK(budget.issued() <= GuestMmapDetailBudget::kTotalLines);
    CHECK(!budget.take(kMainPid, kMainSpaceAfterExec));
}

BOXEDVN_TEST(mmap_detail_budget_resets_cleanly) {
    GuestMmapDetailBudget budget;
    for (unsigned index = 0; index < GuestMmapDetailBudget::kLinesPerAddressSpace;
         ++index) {
        CHECK(budget.take(kMainPid, kMainSpaceAfterExec));
    }
    budget.reset();
    CHECK(budget.issued() == 0);
    CHECK(budget.issuedFor(kMainPid, kMainSpaceAfterExec) == 0);
    CHECK(budget.take(kMainPid, kMainSpaceAfterExec));
}

// ---------------------------------------------------------------------------
// The repeat tracker.
// ---------------------------------------------------------------------------

BOXEDVN_TEST(mmap_repeat_tracker_describes_the_first_sighting) {
    GuestMmapRepeatTracker tracker;
    const auto first = tracker.record(repeatingRequest());
    CHECK(first.decision == GuestMmapRepeatTracker::Decision::First);
    CHECK(first.occurrences == 1);
}

BOXEDVN_TEST(mmap_repeat_tracker_reports_at_powers_of_two) {
    GuestMmapRepeatTracker tracker;
    const GuestMmapSignature signature = repeatingRequest();
    std::uint64_t reportedAt[GuestMmapRepeatTracker::kReportsPerSignature] = {};
    unsigned reports = 0;
    for (std::uint64_t index = 0; index < 4096; ++index) {
        const auto outcome = tracker.record(signature);
        if (outcome.decision != GuestMmapRepeatTracker::Decision::Silent) {
            CHECK(reports < GuestMmapRepeatTracker::kReportsPerSignature);
            reportedAt[reports++] = outcome.occurrences;
        }
    }
    // 1, 2, 4, 8, ... and nothing in between.
    for (unsigned index = 0; index < reports; ++index) {
        const std::uint64_t seen = reportedAt[index];
        CHECK((seen & (seen - 1)) == 0);
        CHECK(seen == (std::uint64_t)1 << index);
    }
    CHECK(reports >= 8);
}

BOXEDVN_TEST(mmap_repeat_tracker_bounds_four_million_identical_requests) {
    // The device count, exactly. A bounded tracker is the whole requirement:
    // the old behaviour would have been four million lines or nothing at all.
    GuestMmapRepeatTracker tracker;
    const unsigned reports =
        countReports(tracker, repeatingRequest(), 4194305);
    CHECK(reports <= GuestMmapRepeatTracker::kReportsPerSignature);
    CHECK(reports >= 1);
    CHECK(tracker.record(repeatingRequest()).decision ==
          GuestMmapRepeatTracker::Decision::Silent);
}

BOXEDVN_TEST(mmap_repeat_tracker_says_once_that_it_has_gone_quiet) {
    GuestMmapRepeatTracker tracker;
    const GuestMmapSignature signature = repeatingRequest();
    bool suppressed = false;
    for (std::uint64_t index = 0; index < 1000000; ++index) {
        if (tracker.record(signature).decision ==
            GuestMmapRepeatTracker::Decision::Suppress) {
            CHECK(!suppressed);
            suppressed = true;
        }
    }
    CHECK(suppressed);
}

BOXEDVN_TEST(mmap_repeat_tracker_keeps_the_running_count_exact) {
    GuestMmapRepeatTracker tracker;
    const GuestMmapSignature signature = repeatingRequest();
    for (std::uint64_t index = 1; index <= 5000; ++index) {
        CHECK(tracker.record(signature).occurrences == index);
    }
}

BOXEDVN_TEST(mmap_repeat_tracker_separates_requests_that_differ) {
    // Two requests that differ in any single field are not the same request,
    // so a walk that steps the address is never mistaken for a repeat. Each
    // variant gets its own tracker: the table is direct-mapped, so a variant
    // may legitimately evict the one it collides with, and what is under test
    // here is the identity comparison, not the slot policy.
    const GuestMmapSignature base = repeatingRequest();
    GuestMmapSignature variants[6];
    for (GuestMmapSignature& variant : variants) variant = base;
    variants[0].address += 0x10000;
    variants[1].length += 0x1000;
    variants[2].protection = 0x1;
    variants[3].flags = 0x22;
    variants[4].threadId += 1;
    variants[5].processId += 1;

    for (const GuestMmapSignature& variant : variants) {
        GuestMmapRepeatTracker tracker;
        CHECK(tracker.record(base).occurrences == 1);
        // A different request starts its own count rather than continuing the
        // first one's.
        CHECK(tracker.record(variant).occurrences == 1);
        CHECK(tracker.record(variant).occurrences == 2);
    }

    // The identical request does continue its count.
    GuestMmapRepeatTracker tracker;
    CHECK(tracker.record(base).occurrences == 1);
    CHECK(tracker.record(base).occurrences == 2);
}

BOXEDVN_TEST(mmap_repeat_tracker_separates_the_same_pid_across_an_exec) {
    // pid 10 before the handoff and pid 10 after it are different address
    // spaces; a request that appears in both is not a loop.
    GuestMmapRepeatTracker tracker;
    GuestMmapSignature before = repeatingRequest();
    before.addressSpace = kMainSpaceBeforeExec;
    CHECK(tracker.record(before).occurrences == 1);
    CHECK(tracker.record(repeatingRequest()).occurrences == 1);
}

BOXEDVN_TEST(mmap_repeat_tracker_is_bounded_against_distinct_requests) {
    // A guest that manufactures an endless stream of distinct requests must
    // not be able to make the tracker unbounded either.
    GuestMmapRepeatTracker tracker;
    GuestMmapSignature signature = repeatingRequest();
    for (std::uint64_t index = 0; index < 200000; ++index) {
        signature.address = 0x300000000ULL + index * 0x10000ULL;
        tracker.record(signature);
    }
    CHECK(tracker.reportsEmitted() <= GuestMmapRepeatTracker::kTotalReports);
}

BOXEDVN_TEST(mmap_repeat_tracker_never_narrates_a_line_per_call) {
    // The bound that matters in practice: a hot loop plus helper traffic must
    // stay within the global cap, not proportional to the call count.
    GuestMmapRepeatTracker tracker;
    GuestMmapSignature signature = repeatingRequest();
    for (std::uint64_t index = 0; index < 1000000; ++index) {
        tracker.record(signature);
        if ((index % 1000) == 0) {
            GuestMmapSignature helper = signature;
            helper.processId = kHelperPids[index % 5];
            helper.addressSpace = 200 + (index % 5);
            tracker.record(helper);
        }
    }
    CHECK(tracker.reportsEmitted() <= GuestMmapRepeatTracker::kTotalReports);
}
