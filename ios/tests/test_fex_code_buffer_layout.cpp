#include "boxedvn_test.h"
#include "boxedvn/fex_code_buffer_layout.h"

#include <cstddef>

using namespace boxedvn;

BOXEDVN_TEST(fex_code_buffer_guard_uses_an_ios_host_page_boundary) {
    const auto layout = planFexCodeBufferLayout(
        0, 16u * 1024u * 1024u, 64u * 1024u * 1024u, 0x4000, 0x1000);
    CHECK(layout.has_value());
    CHECK_EQ(layout->allocationOffset, std::size_t{0x1000});
    CHECK_EQ(layout->guardOffset, std::size_t{0x1000000});
    CHECK_EQ(layout->nextOffset, std::size_t{0x1004000});
    CHECK_EQ(layout->guardOffset % 0x4000, std::size_t{0});
}

BOXEDVN_TEST(fex_code_buffer_layout_keeps_the_rounded_guard_out_of_next_carve) {
    const auto first = planFexCodeBufferLayout(
        0, 0x4000, 0x20000, 0x4000, 0x1000);
    CHECK(first.has_value());
    CHECK_EQ(first->allocationOffset, std::size_t{0x1000});
    CHECK_EQ(first->guardOffset, std::size_t{0x4000});
    CHECK_EQ(first->nextOffset, std::size_t{0x8000});

    const auto second = planFexCodeBufferLayout(
        first->nextOffset, 0x8000, 0x20000, 0x4000, 0x1000);
    CHECK(second.has_value());
    CHECK(second->allocationOffset >= first->nextOffset);
    CHECK_EQ(second->guardOffset % 0x4000, std::size_t{0});
    CHECK(second->allocationOffset >= first->guardOffset + 0x4000);
}

BOXEDVN_TEST(fex_code_buffer_layout_rejects_invalid_or_exhausted_requests) {
    CHECK(!planFexCodeBufferLayout(0, 0, 0x10000, 0x4000, 0x1000)
               .has_value());
    CHECK(!planFexCodeBufferLayout(0, 0x4000, 0x10000, 0x3000, 0x1000)
               .has_value());
    CHECK(!planFexCodeBufferLayout(0, 0x4000, 0x10000, 0x1000, 0x4000)
               .has_value());
    CHECK(!planFexCodeBufferLayout(0, 0x10000, 0x10000, 0x4000, 0x1000)
               .has_value());
}

BOXEDVN_TEST(fex_code_buffer_pool_reuses_only_matching_retired_layouts) {
    FexCodeBufferPool pool;
    const auto small = pool.allocate(0x4000, 0x4000000, 0x4000, 0x1000);
    const auto large = pool.allocate(0x1000000, 0x4000000, 0x4000, 0x1000);
    CHECK(small.has_value());
    CHECK(large.has_value());
    CHECK(!small->reused);
    CHECK(!large->reused);
    const auto cursor = pool.cursor();

    CHECK(pool.release(large->layout.allocationOffset, 0x1000000));
    CHECK(!pool.release(large->layout.allocationOffset, 0x1000000));
    const auto different = pool.allocate(0x800000, 0x4000000, 0x4000, 0x1000);
    CHECK(different.has_value());
    CHECK(!different->reused);
    CHECK(pool.cursor() > cursor);

    const auto beforeReuse = pool.cursor();
    const auto reused = pool.allocate(0x1000000, 0x4000000, 0x4000, 0x1000);
    CHECK(reused.has_value());
    CHECK(reused->reused);
    CHECK_EQ(reused->layout.allocationOffset,
             large->layout.allocationOffset);
    CHECK_EQ(pool.cursor(), beforeReuse);
}
