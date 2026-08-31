/*
 * BoxedWine - the last syscalls a 64-bit guest process made before it exited.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * A guest that exits non-zero after a blocked read leaves almost nothing in a
 * log. The device shows Wine's main process parked in glibc read+0xf on
 * fd 9 with a 16-byte count -- the Wine-server reply header -- and then
 * exiting status 1, with no indication of what that read returned or what it
 * did next. Tracing every syscall live is not an option: the first sixty-four
 * are already traced and the interesting ones are thousands in.
 *
 * So the tail is collected in memory and printed only when a process exits
 * non-zero. A record is opened before the syscall runs and closed when it
 * returns, which is what distinguishes "read returned 0" from "read never
 * returned at all" -- the difference between EOF and a thread still parked.
 *
 * Fixed capacity, no allocation, no locking beyond the caller's own: this
 * lives on the process and is written by its own threads.
 */

#ifndef __SYSCALL_TAIL_RING_H__
#define __SYSCALL_TAIL_RING_H__

#include <cstdint>

#if defined(__cplusplus)

namespace boxedvn {

enum class SyscallTailState : std::uint8_t {
    // Nothing was ever written here.
    Unused = 0,
    // Entered and never came back: still blocked, parked, or the process
    // exited inside it. Reporting this as a zero result would invent an EOF
    // that never happened.
    Pending = 1,
    // Returned a value.
    Completed = 2,
    // Parked and will re-run the same instruction when rescheduled.
    Restarted = 3,
};

struct SyscallTailRecord {
    std::uint64_t sequence = 0;
    std::uint32_t processId = 0;
    std::uint32_t threadId = 0;
    std::uint64_t number = 0;
    std::uint64_t syscallRip = 0;
    std::uint64_t arguments[6] = {};
    std::int64_t result = 0;
    SyscallTailState state = SyscallTailState::Unused;
};

class SyscallTailRing {
public:
    static constexpr unsigned kCapacity = 96;

    // Opens a record and returns a token for closing it. Sequence numbers
    // start at 1 so zero can mean "no record".
    std::uint64_t begin(std::uint32_t processId, std::uint32_t threadId,
                        std::uint64_t number, std::uint64_t syscallRip,
                        const std::uint64_t* arguments) {
        const std::uint64_t sequence = ++sequence_;
        SyscallTailRecord& record = slots_[(sequence - 1) % kCapacity];
        record.sequence = sequence;
        record.processId = processId;
        record.threadId = threadId;
        record.number = number;
        record.syscallRip = syscallRip;
        for (unsigned index = 0; index < 6; ++index) {
            record.arguments[index] = arguments ? arguments[index] : 0;
        }
        record.result = 0;
        record.state = SyscallTailState::Pending;
        return sequence;
    }

    void complete(std::uint64_t token, std::int64_t result) {
        close(token, result, SyscallTailState::Completed);
    }

    void restart(std::uint64_t token) {
        close(token, 0, SyscallTailState::Restarted);
    }

    // Oldest first. A slot whose sequence has been overwritten is simply the
    // newer record; iterating by sequence keeps the order right across a wrap.
    template <typename Visitor>
    void forEach(Visitor&& visit) const {
        const std::uint64_t total = sequence_;
        const std::uint64_t first =
            total > kCapacity ? total - kCapacity : 0;
        for (std::uint64_t sequence = first + 1; sequence <= total;
             ++sequence) {
            const SyscallTailRecord& record = slots_[(sequence - 1) % kCapacity];
            if (record.state == SyscallTailState::Unused ||
                record.sequence != sequence) {
                continue;
            }
            visit(record);
        }
    }

    std::uint64_t recorded() const { return sequence_; }

    // True the first time only, so a process cannot print its tail twice --
    // exit and exit_group both reach the same place.
    bool claimDump() {
        if (dumped_) {
            return false;
        }
        dumped_ = true;
        return true;
    }

    bool dumped() const { return dumped_; }

    void reset() {
        for (unsigned index = 0; index < kCapacity; ++index) {
            slots_[index] = SyscallTailRecord {};
        }
        sequence_ = 0;
        dumped_ = false;
    }

private:
    // A token only closes the record it opened. If the ring wrapped past it
    // while the syscall was blocked, the slot now belongs to a newer syscall
    // and must not be overwritten with this one's result.
    void close(std::uint64_t token, std::int64_t result,
               SyscallTailState state) {
        if (token == 0 || token > sequence_) {
            return;
        }
        SyscallTailRecord& record = slots_[(token - 1) % kCapacity];
        if (record.sequence != token) {
            return;
        }
        record.result = result;
        record.state = state;
    }

    SyscallTailRecord slots_[kCapacity] {};
    std::uint64_t sequence_ = 0;
    bool dumped_ = false;
};

} // namespace boxedvn

#endif // __cplusplus

#endif
