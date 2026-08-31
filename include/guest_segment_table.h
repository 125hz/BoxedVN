/*
 * BoxedWine - the descriptor table a 64-bit guest's selectors index into.
 * Copyright (C) 2026 The BoxedWine Team. GPLv2; see license.txt.
 *
 * A segment selector is not an index on its own. The processor splits it:
 *
 *   bits 15..3  index into the table
 *   bit 2       table indicator, 0 = GDT, 1 = LDT
 *   bits 1..0   requested privilege level, ignored here
 *
 * FEX keeps one descriptor array per thread and reaches it exactly this way,
 * so a host that hands FEX a shorter array does not get a smaller table -- it
 * gets an out-of-bounds read of whatever follows the array, and the value it
 * finds there decides whether the next block is decoded as 64-bit or 32-bit
 * code.
 *
 * Wine's x86-64 Unix side loads __USER_CS = 0x33 and __USER_DS = 0x2b through
 * the iretq that enters Windows code. Those are indices 6 and 5, which is why
 * a table with room for one entry survives every earlier instruction and
 * fails at exactly that transition.
 *
 * The arithmetic lives here, apart from FEX's types, so it can be checked on
 * the host build.
 */

#ifndef __GUEST_SEGMENT_TABLE_H__
#define __GUEST_SEGMENT_TABLE_H__

#include <cstdint>

// FEX allocates gdt[32] per thread and mirrors the LDT onto it. Anything
// smaller cannot hold the selectors below.
#define K_GUEST_SEGMENT_TABLE_ENTRIES 32

// The selectors Wine's x86-64 dispatcher return loads. Fixed values in
// Wine's unix signal_x86_64.c, not something a prefix or a build option
// varies.
#define K_WINE_X64_CODE_SELECTOR 0x33
#define K_WINE_X64_DATA_SELECTOR 0x2b

#if defined(__cplusplus)

namespace boxedvn {

struct GuestSegmentSelector {
    // 0 selects the GDT, 1 the LDT, matching the selector's table indicator.
    unsigned table = 0;
    unsigned index = 0;
};

inline GuestSegmentSelector decodeGuestSegmentSelector(
    std::uint16_t selector) noexcept {
    GuestSegmentSelector decoded;
    decoded.table = (unsigned)((selector >> 2) & 1u);
    decoded.index = (unsigned)(selector >> 3);
    return decoded;
}

// True when a table of `entries` descriptors can be indexed by this selector.
// A false here is an out-of-bounds read, not a rejected selector: nothing in
// the processor or in FEX bounds-checks it.
inline bool guestSegmentSelectorFitsTable(std::uint16_t selector,
                                          unsigned entries) noexcept {
    return decodeGuestSegmentSelector(selector).index < entries;
}

} // namespace boxedvn

#endif // __cplusplus

#endif
