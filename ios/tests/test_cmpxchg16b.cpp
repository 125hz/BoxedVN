#include "boxedvn_test.h"
#include "cmpxchg16b.h"

#include <cstdint>

using namespace boxedvn;

// Wine's ntdll reaches `lock cmpxchg16b [r8]` (f0 49 0f c7 08) and the device
// log stopped there: "CPU64: unimpl opcode at RIP=0x6fffffc9b98c bytes=f0 49
// 0f c7 08 41 0f". The bytes after it are `sete r10b; test r10b,r10b`, so ZF
// is the result the caller reads.

BOXEDVN_TEST(cmpxchg16b_swaps_when_memory_matches_rdx_rax) {
    const Cmpxchg16bResult result = evaluateCmpxchg16b(
        /*memoryLow=*/0x1111111111111111ULL,
        /*memoryHigh=*/0x2222222222222222ULL,
        /*rax=*/0x1111111111111111ULL,
        /*rdx=*/0x2222222222222222ULL,
        /*rbx=*/0xAAAAAAAAAAAAAAAAULL,
        /*rcx=*/0xBBBBBBBBBBBBBBBBULL);
    CHECK(result.succeeded);
    CHECK(result.zeroFlag);
    // RCX:RBX is stored, low half from RBX.
    CHECK(result.writeLow == 0xAAAAAAAAAAAAAAAAULL);
    CHECK(result.writeHigh == 0xBBBBBBBBBBBBBBBBULL);
    // RAX and RDX are unchanged on success.
    CHECK(result.raxAfter == 0x1111111111111111ULL);
    CHECK(result.rdxAfter == 0x2222222222222222ULL);
}

BOXEDVN_TEST(cmpxchg16b_reloads_rdx_rax_when_memory_differs) {
    const Cmpxchg16bResult result = evaluateCmpxchg16b(
        0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL,
        /*rax=*/0x1111111111111111ULL, /*rdx=*/0x2222222222222222ULL,
        /*rbx=*/0xAAAAAAAAAAAAAAAAULL, /*rcx=*/0xBBBBBBBBBBBBBBBBULL);
    CHECK(!result.succeeded);
    CHECK(!result.zeroFlag);
    // The caller is handed what memory actually held; that is what makes a
    // compare-and-swap loop terminate.
    CHECK(result.raxAfter == 0xDEADBEEFCAFEBABEULL);
    CHECK(result.rdxAfter == 0x0123456789ABCDEFULL);
}

BOXEDVN_TEST(cmpxchg16b_compares_both_halves) {
    // Half a match is a mismatch. Getting this wrong would corrupt memory a
    // competing thread had already changed.
    const Cmpxchg16bResult lowOnly = evaluateCmpxchg16b(
        0x1111111111111111ULL, 0x9999999999999999ULL,
        0x1111111111111111ULL, 0x2222222222222222ULL, 1, 2);
    CHECK(!lowOnly.succeeded);
    CHECK(lowOnly.raxAfter == 0x1111111111111111ULL);
    CHECK(lowOnly.rdxAfter == 0x9999999999999999ULL);

    const Cmpxchg16bResult highOnly = evaluateCmpxchg16b(
        0x9999999999999999ULL, 0x2222222222222222ULL,
        0x1111111111111111ULL, 0x2222222222222222ULL, 1, 2);
    CHECK(!highOnly.succeeded);
    CHECK(highOnly.raxAfter == 0x9999999999999999ULL);
    CHECK(highOnly.rdxAfter == 0x2222222222222222ULL);
}

BOXEDVN_TEST(cmpxchg16b_handles_zero_and_all_ones) {
    // A zeroed 128-bit slot is the ordinary initial state of a Wine lock-free
    // list head, and the all-ones case guards against a sign-extension slip.
    const Cmpxchg16bResult zeroed =
        evaluateCmpxchg16b(0, 0, 0, 0, 0x1234, 0x5678);
    CHECK(zeroed.succeeded);
    CHECK(zeroed.writeLow == 0x1234);
    CHECK(zeroed.writeHigh == 0x5678);

    const Cmpxchg16bResult ones = evaluateCmpxchg16b(
        ~(std::uint64_t)0, ~(std::uint64_t)0,
        ~(std::uint64_t)0, ~(std::uint64_t)0, 0, 0);
    CHECK(ones.succeeded);
    CHECK(ones.writeLow == 0);
    CHECK(ones.writeHigh == 0);
}

BOXEDVN_TEST(cmpxchg16b_failure_never_produces_a_write) {
    // The interpreter only writes when `succeeded`; a failed compare must not
    // leave a plausible-looking value behind for it to store.
    const Cmpxchg16bResult result =
        evaluateCmpxchg16b(1, 2, 3, 4, 0xAAAA, 0xBBBB);
    CHECK(!result.succeeded);
    CHECK(result.writeLow == 0);
    CHECK(result.writeHigh == 0);
}

BOXEDVN_TEST(cmpxchg16b_encoding_requires_slash_one_rexw_and_memory) {
    // The observed encoding: f0 49 0f c7 08. REX 0x49 is REX.W|REX.B, modrm
    // 0x08 is mod=00 reg=001 rm=000, so /1 with [r8].
    const std::uint8_t modrm = 0x08;
    const std::uint8_t regField = (std::uint8_t)((modrm >> 3) & 7);
    CHECK(regField == 1);
    CHECK(isCmpxchg16bEncoding(regField, /*rexW=*/true, /*registerForm=*/false));

    // Without REX.W the same encoding is CMPXCHG8B, which is not this.
    CHECK(!isCmpxchg16bEncoding(1, false, false));
    // mod==11 is an invalid encoding for /1 and must not be executed.
    CHECK(!isCmpxchg16bEncoding(1, true, true));
    // Other extensions of 0F C7 are different instructions entirely
    // (/6 RDRAND, /7 RDSEED).
    for (std::uint8_t extension : {(std::uint8_t)0, (std::uint8_t)2,
                                   (std::uint8_t)3, (std::uint8_t)4,
                                   (std::uint8_t)5, (std::uint8_t)6,
                                   (std::uint8_t)7}) {
        CHECK(!isCmpxchg16bEncoding(extension, true, false));
    }
}

BOXEDVN_TEST(cmpxchg16b_encoding_ignores_rex_r_in_the_opcode_extension) {
    // The reg field is an opcode extension, so REX.R is not part of it. A
    // decoder that folds REX.R in would produce 9 and stop matching.
    CHECK(isCmpxchg16bEncoding(1, true, false));
    CHECK(isCmpxchg16bEncoding(1 | 8, true, false));
    // But a genuinely different extension still does not match, with or
    // without REX.R.
    CHECK(!isCmpxchg16bEncoding(6, true, false));
    CHECK(!isCmpxchg16bEncoding(6 | 8, true, false));
}

BOXEDVN_TEST(cmpxchg16b_instruction_length_matches_the_observed_bytes) {
    // f0 49 0f c7 08 is five bytes: two prefix bytes before the opcode, the
    // two-byte 0F C7 opcode, and a one-byte ModRM with no SIB or displacement.
    // cpu64.cpp computes `opOff + 2 + m.length`.
    const unsigned opOff = 2;      // F0 (LOCK) and 49 (REX.W|REX.B)
    const unsigned modrmLength = 1;
    CHECK(opOff + 2 + modrmLength == 5);
    // The next instruction is `41 0f 94 c2` (sete r10b), which is where the
    // device log's byte dump continues.
    const std::uint64_t syscallSiteRip = 0x6fffffc9b98cULL;
    CHECK(syscallSiteRip + 5 == 0x6fffffc9b991ULL);
}

BOXEDVN_TEST(cmpxchg16b_result_is_a_pure_function_of_its_inputs) {
    // The interpreter holds an address lock across read, compare and write.
    // That is only sound if the decision itself carries no hidden state, so
    // repeating it under identical inputs must give an identical answer.
    for (int round = 0; round < 4; ++round) {
        const Cmpxchg16bResult a = evaluateCmpxchg16b(7, 8, 7, 8, 9, 10);
        CHECK(a.succeeded);
        CHECK(a.writeLow == 9);
        CHECK(a.writeHigh == 10);
        const Cmpxchg16bResult b = evaluateCmpxchg16b(7, 8, 1, 2, 9, 10);
        CHECK(!b.succeeded);
        CHECK(b.raxAfter == 7);
        CHECK(b.rdxAfter == 8);
    }
}
