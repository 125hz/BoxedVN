/*
 * BoxedVN - small dependency-free range allocator used by the iOS JIT arena.
 * GPLv2; see license.txt.
 */

#ifndef BVN_RANGE_ALLOCATOR_H
#define BVN_RANGE_ALLOCATOR_H

#include <cstddef>
#include <map>
#include <vector>

class BVNRangeAllocator {
public:
    void reset(std::size_t capacity);

    bool allocate(std::size_t length, std::size_t alignment,
                  std::size_t* offset);
    bool release(std::size_t offset, std::size_t length);
    bool contains(std::size_t offset, std::size_t length) const;

    std::size_t capacity() const { return capacity_; }
    std::size_t available() const { return available_; }

private:
    struct Range {
        std::size_t offset;
        std::size_t length;
    };

    void insertFreeRange(Range range);

    std::size_t capacity_ = 0;
    std::size_t available_ = 0;
    std::vector<Range> freeRanges_;
    std::map<std::size_t, std::size_t> allocations_;
};

#endif  // BVN_RANGE_ALLOCATOR_H
