#include "boxedvn_test.h"
#include "syscall_tail_ring.h"

#include <cstdint>
#include <vector>

using namespace boxedvn;

// A guest that exits non-zero after a blocked read leaves almost nothing in a
// log. The device shows Wine's main process parked in glibc read+0xf on fd 9
// with a 16-byte count -- the Wine-server reply header -- and then exiting
// status 1, with no record of what that read returned.
//
// The property that makes this ring worth having is the one tested hardest
// below: a syscall that never returned must not be reported as a completed
// zero. "read returned 0" is EOF; "read never came back" is a thread still
// parked, and they call for opposite investigations.

namespace {

std::uint64_t begin(SyscallTailRing& ring, std::uint32_t pid,
                    std::uint32_t tid, std::uint64_t number,
                    std::uint64_t rip = 0) {
    const std::uint64_t arguments[6] = {number, rip, 2, 3, 4, 5};
    return ring.begin(pid, tid, number, rip, arguments);
}

std::vector<SyscallTailRecord> collect(const SyscallTailRing& ring) {
    std::vector<SyscallTailRecord> records;
    ring.forEach([&](const SyscallTailRecord& record) {
        records.push_back(record);
    });
    return records;
}

// The device's own shape: read(fd=9, buffer, 16) that never came back.
constexpr std::uint64_t kRead = 0;
constexpr std::uint64_t kServerFd = 9;
constexpr std::uint64_t kReplyHeaderBytes = 16;

} // namespace

BOXEDVN_TEST(syscall_tail_ring_pairs_entry_with_return) {
    SyscallTailRing ring;
    const std::uint64_t token = begin(ring, 10, 11, /*number=*/1, 0x7a40123b8f);
    const std::vector<SyscallTailRecord> pending = collect(ring);
    CHECK(pending.size() == 1);
    CHECK(pending[0].state == SyscallTailState::Pending);
    CHECK(pending[0].processId == 10);
    CHECK(pending[0].threadId == 11);
    CHECK(pending[0].number == 1);
    CHECK(pending[0].syscallRip == 0x7a40123b8f);

    ring.complete(token, 16);
    const std::vector<SyscallTailRecord> done = collect(ring);
    CHECK(done.size() == 1);
    CHECK(done[0].state == SyscallTailState::Completed);
    CHECK(done[0].result == 16);
    // The same record was updated, not a second one appended.
    CHECK(ring.recorded() == 1);
}

BOXEDVN_TEST(syscall_tail_ring_keeps_a_blocked_read_pending) {
    // The exact question the device leaves open. A read that never returned
    // must not read back as a completed zero.
    SyscallTailRing ring;
    const std::uint64_t arguments[6] = {kServerFd, 0x7ffff000,
                                        kReplyHeaderBytes, 0, 0, 0};
    ring.begin(10, 11, kRead, 0x7a40123b8f, arguments);

    const std::vector<SyscallTailRecord> records = collect(ring);
    CHECK(records.size() == 1);
    CHECK(records[0].state == SyscallTailState::Pending);
    CHECK(records[0].state != SyscallTailState::Completed);
    CHECK(records[0].arguments[0] == kServerFd);
    CHECK(records[0].arguments[2] == kReplyHeaderBytes);
    // The result field is untouched, and the state is what says so.
    CHECK(records[0].result == 0);
}

BOXEDVN_TEST(syscall_tail_ring_records_a_signed_error_return) {
    // -EPIPE and 0 are different answers and must stay different.
    SyscallTailRing ring;
    const std::uint64_t eof = begin(ring, 10, 11, kRead);
    ring.complete(eof, 0);
    const std::uint64_t broken = begin(ring, 10, 11, kRead);
    ring.complete(broken, -32);

    const std::vector<SyscallTailRecord> records = collect(ring);
    CHECK(records.size() == 2);
    CHECK(records[0].result == 0);
    CHECK(records[0].state == SyscallTailState::Completed);
    CHECK(records[1].result == -32);
    CHECK(records[1].state == SyscallTailState::Completed);
}

BOXEDVN_TEST(syscall_tail_ring_marks_a_restarted_syscall) {
    // A parked futex or a re-executed syscall produced no result either.
    SyscallTailRing ring;
    const std::uint64_t token = begin(ring, 10, 11, 202);
    ring.restart(token);
    const std::vector<SyscallTailRecord> records = collect(ring);
    CHECK(records.size() == 1);
    CHECK(records[0].state == SyscallTailState::Restarted);
    CHECK(records[0].state != SyscallTailState::Completed);
}

BOXEDVN_TEST(syscall_tail_ring_wraps_and_stays_chronological) {
    SyscallTailRing ring;
    const unsigned total = SyscallTailRing::kCapacity * 3 + 7;
    for (unsigned index = 0; index < total; ++index) {
        const std::uint64_t token = begin(ring, 10, 11, index);
        ring.complete(token, (std::int64_t)index);
    }
    const std::vector<SyscallTailRecord> records = collect(ring);
    // Exactly the capacity, and the newest ones.
    CHECK(records.size() == SyscallTailRing::kCapacity);
    CHECK(ring.recorded() == total);
    // Oldest first, contiguous, ending at the last syscall made.
    for (std::size_t index = 1; index < records.size(); ++index) {
        CHECK(records[index].sequence == records[index - 1].sequence + 1);
        CHECK(records[index].number == records[index - 1].number + 1);
    }
    CHECK(records.back().number == total - 1);
    CHECK(records.back().result == (std::int64_t)(total - 1));
    CHECK(records.front().number == total - SyscallTailRing::kCapacity);
}

BOXEDVN_TEST(syscall_tail_ring_will_not_close_a_recycled_slot) {
    // A syscall that blocks while the ring wraps past its slot must not write
    // its result over the newer syscall that now owns it. This is what makes
    // the tail trustworthy when a thread is parked for a long time.
    SyscallTailRing ring;
    const std::uint64_t stale = begin(ring, 10, 11, kRead);
    for (unsigned index = 0; index < SyscallTailRing::kCapacity + 4; ++index) {
        const std::uint64_t token = begin(ring, 10, 11, 100 + index);
        ring.complete(token, 1);
    }
    ring.complete(stale, 4242);

    for (const SyscallTailRecord& record : collect(ring)) {
        CHECK(record.result != 4242);
        CHECK(record.number != kRead);
    }
}

BOXEDVN_TEST(syscall_tail_ring_is_isolated_per_process) {
    // One ring per process: a child's syscalls must never appear in its
    // parent's tail, or the exit diagnostic would blame the wrong process.
    SyscallTailRing parent;
    SyscallTailRing child;
    const std::uint64_t parentToken = begin(parent, 10, 11, kRead);
    parent.complete(parentToken, 16);
    const std::uint64_t childToken = begin(child, 14, 15, 60);
    child.complete(childToken, 0);

    const std::vector<SyscallTailRecord> parentRecords = collect(parent);
    const std::vector<SyscallTailRecord> childRecords = collect(child);
    CHECK(parentRecords.size() == 1);
    CHECK(childRecords.size() == 1);
    CHECK(parentRecords[0].processId == 10);
    CHECK(childRecords[0].processId == 14);
    CHECK(parent.recorded() == 1);
    CHECK(child.recorded() == 1);
}

BOXEDVN_TEST(syscall_tail_ring_dumps_at_most_once) {
    // exit and exit_group both reach the same place, and a process with
    // several threads can reach it more than once. The tail is printed once.
    SyscallTailRing ring;
    CHECK(ring.claimDump());
    CHECK(!ring.dumped() == false);
    CHECK(!ring.claimDump());
    CHECK(!ring.claimDump());
}

BOXEDVN_TEST(syscall_tail_ring_is_silent_until_claimed) {
    // A process that exits cleanly never claims the dump, so nothing is
    // printed for it however many syscalls it made.
    SyscallTailRing ring;
    for (unsigned index = 0; index < 10; ++index) {
        ring.complete(begin(ring, 10, 11, index), 0);
    }
    CHECK(!ring.dumped());
    CHECK(ring.recorded() == 10);
}

BOXEDVN_TEST(syscall_tail_ring_capacity_is_bounded_and_allocation_free) {
    // The point of a ring: a guest making millions of syscalls costs a fixed
    // amount of memory and produces a fixed number of lines.
    CHECK(SyscallTailRing::kCapacity >= 64);
    CHECK(SyscallTailRing::kCapacity <= 128);
    SyscallTailRing ring;
    for (unsigned index = 0; index < 100000; ++index) {
        ring.complete(begin(ring, 10, 11, index), 0);
    }
    CHECK(collect(ring).size() == SyscallTailRing::kCapacity);
    CHECK(ring.recorded() == 100000);
}

BOXEDVN_TEST(syscall_tail_ring_reset_clears_everything) {
    SyscallTailRing ring;
    ring.complete(begin(ring, 10, 11, kRead), 16);
    CHECK(ring.claimDump());
    ring.reset();
    CHECK(ring.recorded() == 0);
    CHECK(collect(ring).empty());
    // A reset ring can be dumped again; nothing carries over.
    CHECK(ring.claimDump());
}

BOXEDVN_TEST(syscall_tail_ring_rejects_a_token_it_never_issued) {
    SyscallTailRing ring;
    ring.complete(begin(ring, 10, 11, kRead), 16);
    // Zero means "no record", and a token from the future cannot exist.
    ring.complete(0, 99);
    ring.complete(1000, 99);
    ring.restart(0);
    const std::vector<SyscallTailRecord> records = collect(ring);
    CHECK(records.size() == 1);
    CHECK(records[0].result == 16);
    CHECK(records[0].state == SyscallTailState::Completed);
}

BOXEDVN_TEST(syscall_tail_ring_holds_the_device_shape) {
    // The tail as it would look at the failing exit: a run of completed
    // syscalls and then the read that never returned, last.
    SyscallTailRing ring;
    for (unsigned index = 0; index < 20; ++index) {
        ring.complete(begin(ring, 10, 11, 1 + index), 8);
    }
    const std::uint64_t arguments[6] = {kServerFd, 0x7ffff000,
                                        kReplyHeaderBytes, 0, 0, 0};
    ring.begin(10, 11, kRead, 0x7a40123b8f, arguments);

    const std::vector<SyscallTailRecord> records = collect(ring);
    CHECK(records.size() == 21);
    const SyscallTailRecord& last = records.back();
    CHECK(last.number == kRead);
    CHECK(last.syscallRip == 0x7a40123b8f);
    CHECK(last.arguments[0] == kServerFd);
    CHECK(last.arguments[2] == kReplyHeaderBytes);
    CHECK(last.state == SyscallTailState::Pending);
    // Everything before it completed, so the tail names one suspect.
    for (std::size_t index = 0; index + 1 < records.size(); ++index) {
        CHECK(records[index].state == SyscallTailState::Completed);
    }
}
