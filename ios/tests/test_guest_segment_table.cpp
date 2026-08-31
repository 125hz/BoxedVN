#include "boxedvn_test.h"
#include "guest_segment_table.h"

#include <cstdint>

using namespace boxedvn;

// The transition that never happened.
//
// A 64-bit Wine process loads selector zero for its entire life, right up to
// the moment server_init_process_done hands it its PE entry point. Then
// call_init_thunk builds an iretq frame carrying __USER_CS = 0x33 and
// __USER_DS = 0x2b, and the dispatcher return takes it.
//
// Those selectors are indices 6 and 5. The descriptor table this port gave FEX
// had room for one entry, and nothing bounds-checks the index: the processor
// does not, and neither does FEX, which reads the CS descriptor's L bit on the
// host side to decide whether the next block is decoded as 64-bit or 32-bit
// code. Index 6 of a one-entry array is whatever follows it in memory.
//
// So the arithmetic is what is tested here, against a table size, rather than
// the descriptor contents -- the defect was never a wrong descriptor, it was a
// read past the end of the array.

BOXEDVN_TEST(wine_x64_selectors_are_not_index_zero) {
    const GuestSegmentSelector code =
        decodeGuestSegmentSelector(K_WINE_X64_CODE_SELECTOR);
    CHECK_EQ(code.table, 0u);
    CHECK_EQ(code.index, 6u);

    const GuestSegmentSelector data =
        decodeGuestSegmentSelector(K_WINE_X64_DATA_SELECTOR);
    CHECK_EQ(data.table, 0u);
    CHECK_EQ(data.index, 5u);
}

BOXEDVN_TEST(a_single_entry_table_cannot_hold_wine_x64_selectors) {
    // This is the defect, stated as a test: the table that shipped had one
    // entry, and both selectors the dispatcher return loads are out of bounds
    // in it.
    CHECK(!guestSegmentSelectorFitsTable(K_WINE_X64_CODE_SELECTOR, 1));
    CHECK(!guestSegmentSelectorFitsTable(K_WINE_X64_DATA_SELECTOR, 1));

    // And why it survived every earlier instruction: selector zero fits.
    CHECK(guestSegmentSelectorFitsTable(0, 1));
}

BOXEDVN_TEST(the_shipped_table_size_holds_every_selector_wine_uses) {
    CHECK(guestSegmentSelectorFitsTable(K_WINE_X64_CODE_SELECTOR,
                                        K_GUEST_SEGMENT_TABLE_ENTRIES));
    CHECK(guestSegmentSelectorFitsTable(K_WINE_X64_DATA_SELECTOR,
                                        K_GUEST_SEGMENT_TABLE_ENTRIES));
    CHECK(guestSegmentSelectorFitsTable(0, K_GUEST_SEGMENT_TABLE_ENTRIES));

    // FEX's own Linux thread manager allocates exactly this many, so matching
    // it is not a guess about how large the table should be.
    CHECK_EQ((unsigned)K_GUEST_SEGMENT_TABLE_ENTRIES, 32u);

    // Every index the table can hold, and the first one it cannot.
    for (unsigned index = 0; index < K_GUEST_SEGMENT_TABLE_ENTRIES; ++index) {
        const std::uint16_t selector = (std::uint16_t)((index << 3) | 3u);
        CHECK(guestSegmentSelectorFitsTable(selector,
                                            K_GUEST_SEGMENT_TABLE_ENTRIES));
    }
    CHECK(!guestSegmentSelectorFitsTable(
        (std::uint16_t)(K_GUEST_SEGMENT_TABLE_ENTRIES << 3),
        K_GUEST_SEGMENT_TABLE_ENTRIES));
}

BOXEDVN_TEST(the_table_indicator_bit_selects_the_ldt) {
    // Bit 2 chooses the table. The LDT pointer was left null, so a selector
    // with this bit set dereferenced nothing at all -- which is why the two
    // arrays are now mirrored the way FEX mirrors them.
    CHECK_EQ(decodeGuestSegmentSelector(0x33).table, 0u);
    CHECK_EQ(decodeGuestSegmentSelector(0x37).table, 1u);
    CHECK_EQ(decodeGuestSegmentSelector(0x37).index, 6u);

    // The requested privilege level is not part of the index.
    for (std::uint16_t rpl = 0; rpl < 4; ++rpl) {
        const GuestSegmentSelector decoded =
            decodeGuestSegmentSelector((std::uint16_t)(0x30 | rpl));
        CHECK_EQ(decoded.index, 6u);
        CHECK_EQ(decoded.table, 0u);
    }
}
