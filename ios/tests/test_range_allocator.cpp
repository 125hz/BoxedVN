/* BoxedVN - JIT arena range-allocation regression tests. GPLv2. */

#include "boxedvn_test.h"

#include "BVNRangeAllocator.h"

BOXEDVN_TEST(range_allocator_aligns_and_reuses_released_ranges) {
    BVNRangeAllocator allocator;
    allocator.reset(16 * 1024);

    std::size_t first = 0;
    std::size_t second = 0;
    CHECK(allocator.allocate(4096, 4096, &first));
    CHECK(allocator.allocate(4096, 4096, &second));
    CHECK_EQ(first, static_cast<std::size_t>(0));
    CHECK_EQ(second, static_cast<std::size_t>(4096));
    CHECK_EQ(allocator.available(), static_cast<std::size_t>(8192));

    CHECK(allocator.release(first, 4096));
    std::size_t reused = 999;
    CHECK(allocator.allocate(4096, 4096, &reused));
    CHECK_EQ(reused, first);
}

BOXEDVN_TEST(range_allocator_coalesces_freed_arena) {
    BVNRangeAllocator allocator;
    allocator.reset(3 * 65536);

    std::size_t a = 0;
    std::size_t b = 0;
    std::size_t c = 0;
    CHECK(allocator.allocate(65536, 65536, &a));
    CHECK(allocator.allocate(65536, 65536, &b));
    CHECK(allocator.allocate(65536, 65536, &c));
    CHECK(!allocator.allocate(1, 1, &c));

    CHECK(allocator.release(b, 65536));
    CHECK(allocator.release(a, 65536));
    CHECK(allocator.release(c, 65536));
    CHECK_EQ(allocator.available(), allocator.capacity());

    std::size_t whole = 1;
    CHECK(allocator.allocate(3 * 65536, 65536, &whole));
    CHECK_EQ(whole, static_cast<std::size_t>(0));
}

BOXEDVN_TEST(range_allocator_rejects_partial_or_duplicate_release) {
    BVNRangeAllocator allocator;
    allocator.reset(65536);

    std::size_t offset = 0;
    CHECK(allocator.allocate(65536, 65536, &offset));
    CHECK(allocator.contains(offset + 128, 256));
    CHECK(!allocator.contains(offset + 65535, 2));
    CHECK(!allocator.release(offset, 32768));
    CHECK(allocator.release(offset, 65536));
    CHECK(!allocator.release(offset, 65536));
}
