/*
 * BoxedVN - host-page-safe executable allocation layout.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 */

#ifndef BOXEDVN_FEX_CODE_BUFFER_LAYOUT_H
#define BOXEDVN_FEX_CODE_BUFFER_LAYOUT_H

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

namespace boxedvn {

struct FexCodeBufferLayout final {
    std::size_t allocationOffset;
    std::size_t guardOffset;
    std::size_t nextOffset;
};

constexpr bool isPowerOfTwo(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// FEX reserves the final 4 KiB translator page as PROT_NONE. Darwin requires
// mprotect addresses to use the host page size (16 KiB on current iOS), so the
// returned allocation is shifted until that final translator page starts on a
// host-page boundary. The complete rounded host guard page is also kept out of
// the following allocation.
constexpr std::optional<FexCodeBufferLayout> planFexCodeBufferLayout(
    std::size_t cursor, std::size_t length, std::size_t capacity,
    std::size_t hostPageSize, std::size_t translatorPageSize) noexcept {
    if (length == 0 || cursor > capacity ||
        !isPowerOfTwo(hostPageSize) ||
        !isPowerOfTwo(translatorPageSize) ||
        translatorPageSize > hostPageSize ||
        hostPageSize % translatorPageSize != 0) {
        return std::nullopt;
    }

    constexpr std::size_t maximum =
        std::numeric_limits<std::size_t>::max();
    if (cursor > maximum - (translatorPageSize - 1)) {
        return std::nullopt;
    }
    std::size_t allocation =
        (cursor + translatorPageSize - 1) & ~(translatorPageSize - 1);
    const std::size_t guardWithinAllocation =
        ((length - 1) / translatorPageSize) * translatorPageSize;

    bool aligned = false;
    const std::size_t attempts = hostPageSize / translatorPageSize;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        if (allocation <= maximum - guardWithinAllocation &&
            (allocation + guardWithinAllocation) % hostPageSize == 0) {
            aligned = true;
            break;
        }
        if (allocation > maximum - translatorPageSize) {
            return std::nullopt;
        }
        allocation += translatorPageSize;
    }
    if (!aligned || allocation > maximum - length) {
        return std::nullopt;
    }

    const std::size_t guard = allocation + guardWithinAllocation;
    if (guard > maximum - hostPageSize) {
        return std::nullopt;
    }
    const std::size_t occupiedEnd =
        std::max(allocation + length, guard + hostPageSize);
    if (occupiedEnd > maximum - (hostPageSize - 1)) {
        return std::nullopt;
    }
    const std::size_t next =
        (occupiedEnd + hostPageSize - 1) & ~(hostPageSize - 1);
    if (next > capacity) {
        return std::nullopt;
    }
    return FexCodeBufferLayout {allocation, guard, next};
}

// FEX releases a code buffer with the same pointer and length that it received
// from its executable allocator. Keep retired layouts available for an exact-
// size reuse before advancing the finite iOS executable-pool cursor. Exact
// reuse is important: it preserves the already-aligned host guard page at the
// end of the allocation without having to change protections on a neighbouring
// live buffer.
class FexCodeBufferPool final {
public:
    struct Allocation final {
        FexCodeBufferLayout layout;
        bool reused;
    };

    std::optional<Allocation> allocate(
        std::size_t length, std::size_t capacity,
        std::size_t hostPageSize, std::size_t translatorPageSize) {
        for (Reservation& reservation : reservations_) {
            if (!reservation.live && reservation.length == length) {
                reservation.live = true;
                return Allocation {reservation.layout, true};
            }
        }

        const auto layout = planFexCodeBufferLayout(
            cursor_, length, capacity, hostPageSize, translatorPageSize);
        if (!layout.has_value()) {
            return std::nullopt;
        }
        reservations_.push_back(Reservation {*layout, length, true});
        cursor_ = layout->nextOffset;
        return Allocation {*layout, false};
    }

    bool release(std::size_t allocationOffset, std::size_t length) {
        for (Reservation& reservation : reservations_) {
            if (reservation.live && reservation.length == length &&
                reservation.layout.allocationOffset == allocationOffset) {
                reservation.live = false;
                return true;
            }
        }
        return false;
    }

    std::size_t cursor() const noexcept { return cursor_; }

private:
    struct Reservation final {
        FexCodeBufferLayout layout;
        std::size_t length;
        bool live;
    };

    std::size_t cursor_ = 0;
    std::vector<Reservation> reservations_;
};

}  // namespace boxedvn

#endif  // BOXEDVN_FEX_CODE_BUFFER_LAYOUT_H
