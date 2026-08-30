/*
 * BoxedWine - bounding unsupported-syscall diagnostics.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * One unsupported syscall in a retry loop produced 408 MB of log across
 * 3,215,735 identical lines, which is worse than useless: it buries the
 * evidence that would explain the loop and it is what fills a device's disk
 * during a diagnostic run. The semantic fix for that particular loop lives
 * elsewhere; this is the generic guard, so the next one cannot do it again.
 *
 * The rules are deliberately simple and provable:
 *
 *   - The first sighting of a key always reports in full. First-fault
 *     evidence is the whole point of the diagnostic and is never suppressed.
 *   - A few early repeats report, then only powers of two, so a slow leak is
 *     still visible without being proportional to the fault rate.
 *   - Each key reports at most kReportsPerKey times and then says once that
 *     it has gone quiet, so a reader knows the silence is deliberate.
 *   - The whole limiter reports at most kTotalReports times, which bounds the
 *     output even if a guest manufactures an unbounded set of distinct keys.
 *
 * Storage is a fixed-size direct-mapped table -- no allocation, no growth, and
 * no map keyed on guest-controlled values. A key that collides with a live
 * slot takes it over; the global cap is what makes that safe, because two hot
 * keys trading one slot can still only produce kTotalReports lines between
 * them.
 */

#ifndef __BOUNDED_SYSCALL_REPORT_H__
#define __BOUNDED_SYSCALL_REPORT_H__

#include <cstdint>

#if defined(__cplusplus)

namespace boxedvn {

class BoundedSyscallReportLimiter {
public:
    // Small enough to sit inside a per-thread CPU object, large enough that
    // the handful of distinct unsupported syscalls a real guest hits each
    // keep their own slot.
    static constexpr unsigned kSlots = 32;
    static constexpr unsigned kReportsPerKey = 8;
    static constexpr unsigned kTotalReports = 256;

    enum class Decision {
        // Seen before and already reported enough. Say nothing.
        Silent,
        // First sighting of this key: log everything, including registers and
        // the bytes around the faulting instruction.
        Detailed,
        // An early or power-of-two repeat: one line with the running count.
        Repeat,
        // The last line this key will produce, so the silence that follows is
        // explained rather than mysterious.
        Suppress,
    };

    struct Outcome {
        Decision decision = Decision::Silent;
        // How many times this key has now been seen, including this one.
        uint64_t occurrences = 0;
    };

    Outcome record(uint64_t processId, uint64_t threadId, uint64_t number,
                   uint64_t address) {
        Outcome outcome;
        Slot& slot = slots_[slotIndex(processId, threadId, number, address)];
        if (!slot.used || slot.processId != processId ||
            slot.threadId != threadId || slot.number != number ||
            slot.address != address) {
            slot = Slot {};
            slot.used = true;
            slot.processId = processId;
            slot.threadId = threadId;
            slot.number = number;
            slot.address = address;
        }
        outcome.occurrences = ++slot.seen;
        if (slot.silenced) {
            return outcome;
        }
        if (!shouldReport(slot.seen)) {
            return outcome;
        }
        if (total_ >= kTotalReports) {
            // The limiter as a whole has said enough. Go quiet without
            // spending one of this key's own reports on saying so.
            slot.silenced = true;
            return outcome;
        }
        ++total_;
        ++slot.reported;
        if (slot.reported >= kReportsPerKey) {
            slot.silenced = true;
            outcome.decision = Decision::Suppress;
            return outcome;
        }
        outcome.decision =
            slot.seen == 1 ? Decision::Detailed : Decision::Repeat;
        return outcome;
    }

    // Total lines this limiter has authorised, for tests and for a caller that
    // wants to say so.
    unsigned reportsEmitted() const { return total_; }

    void reset() {
        for (unsigned index = 0; index < kSlots; ++index) {
            slots_[index] = Slot {};
        }
        total_ = 0;
    }

private:
    struct Slot {
        uint64_t processId = 0;
        uint64_t threadId = 0;
        uint64_t number = 0;
        uint64_t address = 0;
        uint64_t seen = 0;
        uint32_t reported = 0;
        bool used = false;
        bool silenced = false;
    };

    // Report on 1, 2, 3, 4 and then every power of two. A retry loop settles
    // into a handful of lines; a fault that genuinely recurs at a low rate
    // stays visible.
    static bool shouldReport(uint64_t seen) {
        if (seen <= 4) {
            return true;
        }
        return (seen & (seen - 1)) == 0;
    }

    static unsigned slotIndex(uint64_t processId, uint64_t threadId,
                              uint64_t number, uint64_t address) {
        uint64_t mixed = processId * 0x9E3779B97F4A7C15ULL;
        mixed ^= threadId + 0x165667B19E3779F9ULL + (mixed << 6) +
                 (mixed >> 2);
        mixed ^= number + 0x9E3779B97F4A7C15ULL + (mixed << 6) + (mixed >> 2);
        mixed ^= address + 0xC2B2AE3D27D4EB4FULL + (mixed << 6) + (mixed >> 2);
        return (unsigned)((mixed >> 17) % kSlots);
    }

    Slot slots_[kSlots] {};
    unsigned total_ = 0;
};

} // namespace boxedvn

#endif // __cplusplus

#endif
