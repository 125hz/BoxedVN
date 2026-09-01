/* BoxedVN - SSE4.2 packed string compare semantics. GPLv2. */

#include "boxedvn_test.h"

#include "sse42_string_compare.h"

#include <cstring>

namespace {

void fill(uint8_t* out, const char* text) {
    std::memset(out, 0, 16);
    std::memcpy(out, text, std::strlen(text));
}

// glibc __strcmp_sse42: signed bytes, equal each, negative polarity, LSB index.
constexpr uint8_t kStrcmpImm = 0x1a;
// glibc __strcspn_sse42: unsigned bytes, equal any, LSB index.
constexpr uint8_t kStrcspnImm = 0x02;
// glibc __strspn_sse42: unsigned bytes, equal any, negative polarity.
constexpr uint8_t kStrspnImm = 0x12;
// strstr-style: unsigned bytes, equal ordered, LSB index.
constexpr uint8_t kStrstrImm = 0x0c;

}  // namespace

BOXEDVN_TEST(sse42_strcmp_immediate_finds_the_first_difference) {
    uint8_t a[16], b[16];
    fill(a, "WINEPREFIX=/tmp");
    fill(b, "WINEPREFIX=/usr");
    const auto r = boxedvn::sse42StringCompare(
        a, boxedvn::sse42StringImplicitLength(a, kStrcmpImm),
        b, boxedvn::sse42StringImplicitLength(b, kStrcmpImm), kStrcmpImm);
    CHECK_EQ(r.index, 12U);
    CHECK(r.cf);
    CHECK(r.zf);  // both strings end inside the register
    CHECK(r.sf);
}

BOXEDVN_TEST(sse42_strcmp_immediate_reports_no_difference_for_equal_strings) {
    // Sixteen equal bytes, no terminator: lengths are 16, IntRes2 is zero,
    // ECX is the element count, ZF and SF are clear.
    uint8_t a[16], b[16];
    std::memcpy(a, "0123456789abcdef", 16);
    std::memcpy(b, "0123456789abcdef", 16);
    const auto r = boxedvn::sse42StringCompare(
        a, boxedvn::sse42StringImplicitLength(a, kStrcmpImm),
        b, boxedvn::sse42StringImplicitLength(b, kStrcmpImm), kStrcmpImm);
    CHECK_EQ(r.index, 16U);
    CHECK(!r.cf);
    CHECK(!r.zf);
    CHECK(!r.sf);
    CHECK(!r.of);
}

BOXEDVN_TEST(sse42_strcmp_immediate_treats_both_terminators_as_equal) {
    uint8_t a[16], b[16];
    fill(a, "abc");
    fill(b, "abc");
    const auto r = boxedvn::sse42StringCompare(
        a, boxedvn::sse42StringImplicitLength(a, kStrcmpImm),
        b, boxedvn::sse42StringImplicitLength(b, kStrcmpImm), kStrcmpImm);
    // Elements past both terminators are "both invalid", which equal-each
    // counts as equal, so negative polarity leaves nothing set.
    CHECK_EQ(r.index, 16U);
    CHECK(!r.cf);
    CHECK(r.zf);
    CHECK(r.sf);
}

BOXEDVN_TEST(sse42_strcmp_immediate_stops_where_one_string_ends) {
    uint8_t a[16], b[16];
    fill(a, "abc");
    fill(b, "abcd");
    const auto r = boxedvn::sse42StringCompare(
        a, boxedvn::sse42StringImplicitLength(a, kStrcmpImm),
        b, boxedvn::sse42StringImplicitLength(b, kStrcmpImm), kStrcmpImm);
    CHECK_EQ(r.index, 3U);
    CHECK(r.cf);
}

BOXEDVN_TEST(sse42_equal_any_matches_strcspn) {
    uint8_t set[16], text[16];
    fill(set, ":;");
    fill(text, "PATH=/bin:/usr");
    const auto r = boxedvn::sse42StringCompare(
        set, boxedvn::sse42StringImplicitLength(set, kStrcspnImm),
        text, boxedvn::sse42StringImplicitLength(text, kStrcspnImm), kStrcspnImm);
    CHECK_EQ(r.index, 9U);
    CHECK_EQ(r.mask, 1U << 9);
    CHECK(r.cf);
}

BOXEDVN_TEST(sse42_negative_polarity_matches_strspn) {
    uint8_t set[16], text[16];
    fill(set, "0123456789");
    fill(text, "1234abc");
    const auto r = boxedvn::sse42StringCompare(
        set, boxedvn::sse42StringImplicitLength(set, kStrspnImm),
        text, boxedvn::sse42StringImplicitLength(text, kStrspnImm), kStrspnImm);
    // The first non-digit is at 4; negative polarity also sets every
    // position past the end of the text, so bit 4 is the least significant.
    CHECK_EQ(r.index, 4U);
    CHECK(r.cf);
}

BOXEDVN_TEST(sse42_equal_ordered_finds_a_substring_and_its_partial_tail) {
    uint8_t needle[16], hay[16];
    fill(needle, "dll");
    fill(hay, "ntdll.dll;kernel");
    const auto r = boxedvn::sse42StringCompare(
        needle, boxedvn::sse42StringImplicitLength(needle, kStrstrImm),
        hay, boxedvn::sse42StringImplicitLength(hay, kStrstrImm), kStrstrImm);
    CHECK_EQ(r.index, 2U);
    CHECK((r.mask & (1U << 2)) != 0);
    CHECK((r.mask & (1U << 6)) != 0);
    CHECK(!r.zf);  // haystack fills the register
    CHECK(r.sf);
}

BOXEDVN_TEST(sse42_explicit_lengths_saturate_and_take_magnitudes) {
    CHECK_EQ(boxedvn::sse42StringExplicitLength(5, false, 0x00), 5U);
    CHECK_EQ(boxedvn::sse42StringExplicitLength(40, false, 0x00), 16U);
    CHECK_EQ(boxedvn::sse42StringExplicitLength(40, false, 0x01), 8U);
    CHECK_EQ(boxedvn::sse42StringExplicitLength((uint64_t)(int64_t)-3, true, 0x00), 3U);
    // Without REX.W only EDX/EAX count: the high half is ignored.
    CHECK_EQ(boxedvn::sse42StringExplicitLength(0xFFFFFFFF00000002ULL, false, 0x00), 2U);
    CHECK_EQ(boxedvn::sse42StringExplicitLength(0xFFFFFFFFULL, false, 0x00), 1U);  // -1
    CHECK_EQ(boxedvn::sse42StringExplicitLength(0x8000000000000000ULL, true, 0x00), 16U);
}

BOXEDVN_TEST(sse42_mask_form_produces_bit_and_byte_masks) {
    uint8_t set[16], text[16];
    fill(set, "a");
    fill(text, "banana");
    const auto bits = boxedvn::sse42StringCompare(
        set, 1, text, boxedvn::sse42StringImplicitLength(text, 0x00), 0x00);
    CHECK_EQ(bits.mask, (1U << 1) | (1U << 3) | (1U << 5));
    CHECK_EQ(bits.xmm0[0], 0x2AU);
    CHECK_EQ(bits.xmm0[1], 0U);
    const auto bytes = boxedvn::sse42StringCompare(
        set, 1, text, boxedvn::sse42StringImplicitLength(text, 0x40), 0x40);
    CHECK_EQ(bytes.xmm0[1], 0xFFU);
    CHECK_EQ(bytes.xmm0[2], 0U);
    CHECK_EQ(bytes.xmm0[5], 0xFFU);
    // Most-significant index with bit 6.
    CHECK_EQ(bytes.index, 5U);
}

BOXEDVN_TEST(sse42_word_elements_and_ranges) {
    // Words: range 'a'..'z' against "Az9m".
    uint8_t range[16] = {}, text[16] = {};
    range[0] = 'a'; range[2] = 'z';
    const char* t = "Az9m";
    for (int i = 0; i < 4; i++) text[i * 2] = (uint8_t)t[i];
    const uint8_t imm = 0x05;  // words, ranges
    const auto r = boxedvn::sse42StringCompare(
        range, boxedvn::sse42StringImplicitLength(range, imm),
        text, boxedvn::sse42StringImplicitLength(text, imm), imm);
    CHECK_EQ(boxedvn::sse42StringElementCount(imm), 8U);
    CHECK_EQ(r.mask, (1U << 1) | (1U << 3));
    CHECK_EQ(r.index, 1U);
    CHECK(r.zf);
}
