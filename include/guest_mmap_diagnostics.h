/*
 * BoxedWine - bounded, per-address-space diagnostics for 64-bit guest mmap.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * The mmap syscall layer used one process-global ordinal to decide which
 * requests were worth a detailed line. That is exactly the wrong key. On a
 * Wine launch the ordinal budget is spent by the loader and by short-lived
 * helper processes -- wineboot, services.exe, the DLL-trace children -- long
 * before the process that matters reaches its first interesting request. The
 * device run that motivated this file shows the failure precisely: every
 * detailed line belongs to a helper or to the loader, and the 4,193,283
 * refused requests the main process issued after the Windows PE handoff are
 * represented only by a running total. The address, length, protection and
 * flags of the request that never stopped repeating were not recoverable from
 * the log at all.
 *
 * Two independent bounds fix that, and both are pure value types so they can
 * be exercised without a process or an address space:
 *
 *   - GuestMmapDetailBudget gives every (process, address-space generation)
 *     pair its own allowance of detailed lines. A helper cannot consume the
 *     main process's evidence, and an exec gets a fresh allowance because it
 *     is a fresh address space. A global cap still bounds the total.
 *
 *   - GuestMmapRepeatTracker keys on the request itself -- pid, tid, address
 *     space, address, length, protection, flags -- and reports the first
 *     sighting and then only powers of two, carrying the running count. A
 *     request repeated four million times costs a couple of dozen lines and
 *     still says what it was, how often it happened, and how it was answered.
 *
 * Storage is fixed-size and direct-mapped in both: no allocation, no growth,
 * and no container keyed on guest-controlled values. A colliding key takes
 * over the slot; the global caps are what make that safe.
 */

#ifndef BOXEDWINE_GUEST_MMAP_DIAGNOSTICS_H
#define BOXEDWINE_GUEST_MMAP_DIAGNOSTICS_H

#include <cstdint>

namespace boxedvn {

// Everything that makes two mmap requests "the same request again". The
// address space is part of the identity because the same pid before and after
// execve is not the same address space, and a repeat across that boundary is
// not a loop.
struct GuestMmapSignature {
    std::uint64_t processId = 0;
    std::uint64_t threadId = 0;
    std::uint64_t addressSpace = 0;
    std::uint64_t address = 0;
    std::uint64_t length = 0;
    std::uint32_t protection = 0;
    std::uint32_t flags = 0;
};

inline bool operator==(const GuestMmapSignature& left,
                       const GuestMmapSignature& right) noexcept {
    return left.processId == right.processId &&
           left.threadId == right.threadId &&
           left.addressSpace == right.addressSpace &&
           left.address == right.address && left.length == right.length &&
           left.protection == right.protection && left.flags == right.flags;
}

// Per-address-space allowance of detailed per-call lines.
class GuestMmapDetailBudget {
public:
    // Enough slots for the loader, wineserver, wineboot, services.exe, the
    // DLL-trace helpers and the process under test to hold one each.
    static constexpr unsigned kSlots = 16;
    // Enough to cover a Wine start-up reservation walk in full.
    static constexpr unsigned kLinesPerAddressSpace = 24;
    // The whole budget, however many address spaces appear.
    static constexpr unsigned kTotalLines = 192;

    // True when this address space may spend one detailed line.
    bool take(std::uint64_t processId,
              std::uint64_t addressSpaceGeneration) noexcept {
        if (total_ >= kTotalLines) {
            return false;
        }
        Slot& slot = slots_[slotIndex(processId, addressSpaceGeneration)];
        if (!slot.used || slot.processId != processId ||
            slot.addressSpace != addressSpaceGeneration) {
            slot = Slot {};
            slot.used = true;
            slot.processId = processId;
            slot.addressSpace = addressSpaceGeneration;
        }
        if (slot.issued >= kLinesPerAddressSpace) {
            return false;
        }
        ++slot.issued;
        ++total_;
        return true;
    }

    // Lines this budget has authorised in total, and for one address space.
    unsigned issued() const noexcept { return total_; }

    unsigned issuedFor(std::uint64_t processId,
                       std::uint64_t addressSpaceGeneration) const noexcept {
        const Slot& slot = slots_[slotIndex(processId, addressSpaceGeneration)];
        if (!slot.used || slot.processId != processId ||
            slot.addressSpace != addressSpaceGeneration) {
            return 0;
        }
        return slot.issued;
    }

    void reset() noexcept {
        for (unsigned index = 0; index < kSlots; ++index) {
            slots_[index] = Slot {};
        }
        total_ = 0;
    }

private:
    struct Slot {
        std::uint64_t processId = 0;
        std::uint64_t addressSpace = 0;
        unsigned issued = 0;
        bool used = false;
    };

    static unsigned slotIndex(std::uint64_t processId,
                              std::uint64_t addressSpaceGeneration) noexcept {
        std::uint64_t mixed = processId * 0x9E3779B97F4A7C15ULL;
        mixed ^= addressSpaceGeneration + 0x165667B19E3779F9ULL +
                 (mixed << 6) + (mixed >> 2);
        return (unsigned)((mixed >> 17) % kSlots);
    }

    Slot slots_[kSlots] {};
    unsigned total_ = 0;
};

// Bounded reporting for a request that keeps coming back.
class GuestMmapRepeatTracker {
public:
    static constexpr unsigned kSlots = 32;
    static constexpr unsigned kReportsPerSignature = 16;
    static constexpr unsigned kTotalReports = 128;

    enum class Decision {
        // Seen before, and this repetition is not one of the reported ones.
        Silent,
        // First sighting of this exact request: say everything about it.
        First,
        // A power-of-two repetition: one line carrying the running count.
        Repeat,
        // The last line this request will produce, so the silence that
        // follows is deliberate rather than mysterious.
        Suppress,
    };

    struct Outcome {
        Decision decision = Decision::Silent;
        // How many times this request has now been seen, this one included.
        std::uint64_t occurrences = 0;
    };

    Outcome record(const GuestMmapSignature& signature) noexcept {
        Outcome outcome;
        Slot& slot = slots_[slotIndex(signature)];
        if (!slot.used || !(slot.signature == signature)) {
            slot = Slot {};
            slot.used = true;
            slot.signature = signature;
        }
        outcome.occurrences = ++slot.seen;
        if (slot.silenced || !shouldReport(slot.seen)) {
            return outcome;
        }
        if (total_ >= kTotalReports) {
            // The tracker as a whole has said enough. Go quiet without
            // spending this request's own last line on saying so.
            slot.silenced = true;
            return outcome;
        }
        ++total_;
        ++slot.reported;
        if (slot.reported >= kReportsPerSignature) {
            slot.silenced = true;
            outcome.decision = Decision::Suppress;
            return outcome;
        }
        outcome.decision = slot.seen == 1 ? Decision::First : Decision::Repeat;
        return outcome;
    }

    unsigned reportsEmitted() const noexcept { return total_; }

    void reset() noexcept {
        for (unsigned index = 0; index < kSlots; ++index) {
            slots_[index] = Slot {};
        }
        total_ = 0;
    }

    // The first sighting and then every power of two. A tight retry loop
    // settles into a handful of lines whose spacing itself shows the rate.
    static bool shouldReport(std::uint64_t seen) noexcept {
        return (seen & (seen - 1)) == 0;
    }

private:
    struct Slot {
        GuestMmapSignature signature {};
        std::uint64_t seen = 0;
        unsigned reported = 0;
        bool used = false;
        bool silenced = false;
    };

    static unsigned slotIndex(const GuestMmapSignature& signature) noexcept {
        std::uint64_t mixed = signature.processId * 0x9E3779B97F4A7C15ULL;
        const std::uint64_t parts[] = {
            signature.threadId,
            signature.addressSpace,
            signature.address,
            signature.length,
            (std::uint64_t)signature.protection,
            (std::uint64_t)signature.flags,
        };
        for (std::uint64_t part : parts) {
            mixed ^= part + 0x9E3779B97F4A7C15ULL + (mixed << 6) + (mixed >> 2);
        }
        return (unsigned)((mixed >> 17) % kSlots);
    }

    Slot slots_[kSlots] {};
    unsigned total_ = 0;
};

} // namespace boxedvn

#endif // BOXEDWINE_GUEST_MMAP_DIAGNOSTICS_H
