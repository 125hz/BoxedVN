#include "boxedvn_test.h"
#include "boxedvn/fex_code_buffer_layout.h"
#include "boxedvn/fex_code_segments.h"

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

BOXEDVN_TEST(fex_code_segments_translate_independent_aliases_and_boundaries) {
    FexCodeSegments segments;
    CHECK_EQ(segments.writable(0x100000), std::uintptr_t{0});
    CHECK(segments.append({0x100000, 0x500000, 0x10000}));
    CHECK(segments.append({0x900000, 0x200000, 0x20000}));
    CHECK_EQ(segments.writable(0x100123), std::uintptr_t{0x500123});
    CHECK_EQ(segments.writable(0x90ffff), std::uintptr_t{0x20ffff});
    CHECK_EQ(segments.writable(0x91ffff), std::uintptr_t{0x21ffff});
    CHECK_EQ(segments.writable(0x920000), std::uintptr_t{0});
    CHECK_EQ(segments.writable(0xfffff), std::uintptr_t{0});
    CHECK_EQ(segments.writable(0x110000), std::uintptr_t{0});
    CHECK(!segments.append({0x108000, 0x600000, 0x10000}));
    CHECK_EQ(segments.size(), std::size_t{2});
    CHECK(!segments.append({~std::uintptr_t{0} - 0xff, 0x600000, 0x1000}));
    CHECK_EQ(segments.writable(0x100123), std::uintptr_t{0x500123});
}

BOXEDVN_TEST(fex_code_cache_growth_can_move_to_another_segment) {
    constexpr std::size_t mib = 1024 * 1024;
    FexCodeBufferPool first, second;
    // The observed probe, loader and replacement contexts consume three
    // 16 MiB buffers. A 32 MiB generation must use another 64 MiB segment.
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(first.allocate(0x4000, 64 * mib, 0x4000, 0x1000).has_value());
        CHECK(first.allocate(16 * mib, 64 * mib, 0x4000, 0x1000).has_value());
    }
    CHECK(!first.allocate(32 * mib, 64 * mib, 0x4000, 0x1000).has_value());
    const auto grown = second.allocate(32 * mib, 64 * mib, 0x4000, 0x1000);
    CHECK(grown.has_value());
    CHECK(second.release(grown->layout.allocationOffset, 32 * mib));
    const auto reused = second.allocate(32 * mib, 64 * mib, 0x4000, 0x1000);
    CHECK(reused.has_value());
    CHECK(reused->reused);
}
