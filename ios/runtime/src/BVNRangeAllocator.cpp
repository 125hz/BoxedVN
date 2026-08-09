/*
 * BoxedVN - small dependency-free range allocator used by the iOS JIT arena.
 * GPLv2; see license.txt.
 */

#include "BVNRangeAllocator.h"

#include <algorithm>
#include <limits>

namespace {

bool alignUp(std::size_t value, std::size_t alignment, std::size_t* result) {
    if (alignment == 0) {
        return false;
    }
    const std::size_t remainder = value % alignment;
    if (remainder == 0) {
        *result = value;
        return true;
    }
    const std::size_t padding = alignment - remainder;
    if (value > std::numeric_limits<std::size_t>::max() - padding) {
        return false;
    }
    *result = value + padding;
    return true;
}

}  // namespace

void BVNRangeAllocator::reset(std::size_t capacity) {
    capacity_ = capacity;
    available_ = capacity;
    allocations_.clear();
    freeRanges_.clear();
    if (capacity != 0) {
        freeRanges_.push_back({0, capacity});
    }
}

bool BVNRangeAllocator::allocate(std::size_t length, std::size_t alignment,
                                 std::size_t* offset) {
    if (length == 0 || offset == nullptr) {
        return false;
    }

    for (auto it = freeRanges_.begin(); it != freeRanges_.end(); ++it) {
        std::size_t aligned = 0;
        if (!alignUp(it->offset, alignment, &aligned) || aligned < it->offset) {
            continue;
        }
        const std::size_t prefix = aligned - it->offset;
        if (prefix > it->length || length > it->length - prefix) {
            continue;
        }

        const Range original = *it;
        it = freeRanges_.erase(it);
        if (prefix != 0) {
            it = freeRanges_.insert(it, {original.offset, prefix});
            ++it;
        }
        const std::size_t consumed = prefix + length;
        if (consumed < original.length) {
            freeRanges_.insert(
                it, {original.offset + consumed, original.length - consumed});
        }

        allocations_[aligned] = length;
        available_ -= length;
        *offset = aligned;
        return true;
    }
    return false;
}

bool BVNRangeAllocator::release(std::size_t offset, std::size_t length) {
    const auto found = allocations_.find(offset);
    if (found == allocations_.end() || found->second != length) {
        return false;
    }
    allocations_.erase(found);
    available_ += length;
    insertFreeRange({offset, length});
    return true;
}

bool BVNRangeAllocator::contains(std::size_t offset,
                                 std::size_t length) const {
    if (length == 0) {
        return false;
    }
    auto after = allocations_.upper_bound(offset);
    if (after == allocations_.begin()) {
        return false;
    }
    --after;
    const std::size_t allocationOffset = after->first;
    const std::size_t allocationLength = after->second;
    return offset >= allocationOffset && length <= allocationLength &&
           offset - allocationOffset <= allocationLength - length;
}

void BVNRangeAllocator::insertFreeRange(Range range) {
    auto position = std::lower_bound(
        freeRanges_.begin(), freeRanges_.end(), range.offset,
        [](const Range& existing, std::size_t offset) {
            return existing.offset < offset;
        });
    position = freeRanges_.insert(position, range);

    if (position != freeRanges_.begin()) {
        auto previous = position - 1;
        if (previous->offset + previous->length == position->offset) {
            previous->length += position->length;
            position = freeRanges_.erase(position);
            position = previous;
        }
    }

    auto next = position + 1;
    if (next != freeRanges_.end() &&
        position->offset + position->length == next->offset) {
        position->length += next->length;
        freeRanges_.erase(next);
    }
}
