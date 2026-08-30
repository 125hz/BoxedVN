#include "boxedvn_test.h"
#include "bounded_syscall_report.h"

using namespace boxedvn;

// One unsupported syscall in a retry loop wrote 408 MB across 3,215,735
// identical lines. The semantic fix for that particular loop is the Wine NT
// redirect; this is the guard that stops the next one doing it again, so what
// matters here is that the bound holds against a pathological caller while
// first-fault evidence survives.

namespace {

using Decision = BoundedSyscallReportLimiter::Decision;

unsigned countReports(BoundedSyscallReportLimiter& limiter, uint64_t pid,
                      uint64_t tid, uint64_t number, uint64_t rip,
                      uint64_t times) {
    unsigned reports = 0;
    for (uint64_t index = 0; index < times; ++index) {
        if (limiter.record(pid, tid, number, rip).decision !=
            Decision::Silent) {
            ++reports;
        }
    }
    return reports;
}

} // namespace

BOXEDVN_TEST(bounded_syscall_report_logs_the_first_sighting_in_full) {
    BoundedSyscallReportLimiter limiter;
    const auto first = limiter.record(18, 19, 227, 0x6fffffca5642ULL);
    CHECK(first.decision == Decision::Detailed);
    CHECK(first.occurrences == 1);
}

BOXEDVN_TEST(bounded_syscall_report_bounds_one_million_identical_faults) {
    // The exact shape of the failure: one key, repeated without limit.
    BoundedSyscallReportLimiter limiter;
    const unsigned reports =
        countReports(limiter, 18, 19, 227, 0x6fffffca5642ULL, 1000000);
    CHECK(reports <= BoundedSyscallReportLimiter::kReportsPerKey);
    CHECK(reports >= 1);
    // And the very last thing it said was that it was going quiet.
    CHECK(limiter.record(18, 19, 227, 0x6fffffca5642ULL).decision ==
          Decision::Silent);
}

BOXEDVN_TEST(bounded_syscall_report_says_once_that_it_has_gone_quiet) {
    BoundedSyscallReportLimiter limiter;
    unsigned suppressions = 0;
    for (uint64_t index = 0; index < 100000; ++index) {
        if (limiter.record(18, 19, 227, 0x1000).decision ==
            Decision::Suppress) {
            ++suppressions;
        }
    }
    CHECK(suppressions == 1);
}

BOXEDVN_TEST(bounded_syscall_report_keeps_a_new_key_visible) {
    BoundedSyscallReportLimiter limiter;
    countReports(limiter, 18, 19, 227, 0x6fffffca5642ULL, 1000000);

    // A different syscall number is a different fault and must not inherit
    // the silence of the one before it.
    CHECK(limiter.record(18, 19, 154, 0x6fffffca4d22ULL).decision ==
          Decision::Detailed);
    // So is the same number from a different instruction.
    CHECK(limiter.record(18, 19, 227, 0x6fffffca9999ULL).decision ==
          Decision::Detailed);
    // So is the same fault in another thread, and in another process.
    CHECK(limiter.record(18, 21, 227, 0x6fffffca5642ULL).decision ==
          Decision::Detailed);
    CHECK(limiter.record(20, 19, 227, 0x6fffffca5642ULL).decision ==
          Decision::Detailed);
}

BOXEDVN_TEST(bounded_syscall_report_reports_early_repeats_then_powers_of_two) {
    BoundedSyscallReportLimiter limiter;
    // 1 is Detailed; 2, 3 and 4 are early repeats; then 8, 16, 32...
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Detailed);
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Repeat);
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Repeat);
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Repeat);
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Silent);
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Silent);
    CHECK(limiter.record(1, 1, 300, 0x2000).decision == Decision::Silent);
    // The eighth sighting is a power of two.
    const auto eighth = limiter.record(1, 1, 300, 0x2000);
    CHECK(eighth.occurrences == 8);
    CHECK(eighth.decision != Decision::Silent);
}

BOXEDVN_TEST(bounded_syscall_report_counts_every_occurrence) {
    BoundedSyscallReportLimiter limiter;
    for (uint64_t index = 1; index <= 5000; ++index) {
        CHECK(limiter.record(1, 1, 300, 0x2000).occurrences == index);
    }
}

BOXEDVN_TEST(bounded_syscall_report_bounds_an_unbounded_set_of_keys) {
    // A guest that manufactures a fresh key every time -- a different RIP on
    // every fault -- must not defeat the bound. This is why the per-key cap is
    // not the only one.
    BoundedSyscallReportLimiter limiter;
    unsigned reports = 0;
    for (uint64_t index = 0; index < 200000; ++index) {
        if (limiter.record(18, 19, 227, 0x40000 + index * 32).decision !=
            Decision::Silent) {
            ++reports;
        }
    }
    CHECK(reports <= BoundedSyscallReportLimiter::kTotalReports);
    CHECK(limiter.reportsEmitted() <=
          BoundedSyscallReportLimiter::kTotalReports);
}

BOXEDVN_TEST(bounded_syscall_report_bounds_many_hot_keys_sharing_slots) {
    // More distinct hot keys than there are slots, each hammered. The table is
    // fixed-size, so keys evict each other; the global cap is what keeps the
    // total bounded.
    BoundedSyscallReportLimiter limiter;
    unsigned reports = 0;
    for (uint64_t round = 0; round < 5000; ++round) {
        for (uint64_t key = 0; key < BoundedSyscallReportLimiter::kSlots * 4;
             ++key) {
            if (limiter.record(18, 19, 200 + key, 0x50000 + key * 64)
                    .decision != Decision::Silent) {
                ++reports;
            }
        }
    }
    CHECK(reports <= BoundedSyscallReportLimiter::kTotalReports);
}

BOXEDVN_TEST(bounded_syscall_report_reset_restores_first_fault_reporting) {
    BoundedSyscallReportLimiter limiter;
    countReports(limiter, 18, 19, 227, 0x1000, 100000);
    CHECK(limiter.record(18, 19, 227, 0x1000).decision == Decision::Silent);
    limiter.reset();
    CHECK(limiter.reportsEmitted() == 0);
    CHECK(limiter.record(18, 19, 227, 0x1000).decision == Decision::Detailed);
}

BOXEDVN_TEST(bounded_syscall_report_limits_are_small_enough_to_matter) {
    // The bound has to be a bound in practice, not only in principle: the
    // failure it exists to prevent was three million lines.
    CHECK(BoundedSyscallReportLimiter::kReportsPerKey <= 16);
    CHECK(BoundedSyscallReportLimiter::kTotalReports <= 1024);
    CHECK(BoundedSyscallReportLimiter::kSlots >= 8);
}
