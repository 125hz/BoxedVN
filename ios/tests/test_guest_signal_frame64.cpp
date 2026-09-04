#include "boxedvn_test.h"
#include "guest_signal_frame64.h"
#include "fex_x87_state.h"
#include <array>
#include <cstring>

namespace {
struct TestCpu {
    struct Fpu {
        struct Reg { uint64_t signif; uint16_t signExp; } regs[8]{};
        uint8_t tags[8]{};
        bool isRegCached[8]{};
        bool isMMXInUse{};
        uint16_t control{}, status{};
        uint16_t CW() { return control; }
        uint16_t SW() { return status; }
        void SetCW(uint16_t v) { control = v; }
        void SetSW(uint16_t v) { status = v; }
        Reg& getReg(unsigned i) { return regs[i]; }
    } fpu;
    struct Xmm { uint64_t lo, hi; } xmm[16]{};
    Xmm ymmHigh[16]{};
    bool signalXsave{};
    uint32_t mxcsr{};
};
}

BOXEDVN_TEST(signal64_handler_stack_cannot_overwrite_payload) {
    using namespace boxedvn;
    for (uint64_t skew = 0; skew < 64; ++skew) {
        const uint64_t base = signal64FrameBase(0x20000 + skew, 0, 0, false);
        const uint64_t rsp = base + signal64RestorerOffset;
        CHECK_EQ(rsp % 16, 8ull);
        CHECK_EQ((base + signal64FpOffset) % 64, 0ull);
        CHECK(base + signal64FrameSize <= 0x20000 + skew - 128);
        CHECK(base + signal64SiginfoOffset > rsp);
        CHECK_EQ(base + signal64UcontextOffset, rsp + 8);
    }
}

BOXEDVN_TEST(signal64_nested_altstack_keeps_outer_frame) {
    using namespace boxedvn;
    const auto outer = signal64FrameBase(0x10000, 0x30000, 0x10000, true);
    CHECK(outer >= 0x30000);
    const auto inner = signal64FrameBase(outer + signal64RestorerOffset - 256,
                                         0x30000, 0x10000, true);
    CHECK(inner + signal64FrameSize < outer);
}

BOXEDVN_TEST(signal64_fxsave_uses_logical_registers_and_physical_tags) {
    for (unsigned top = 0; top < 8; ++top) {
        TestCpu cpu;
        cpu.fpu.control = 0x37f;
        cpu.fpu.status = uint16_t((top << 11) | 0x4541);
        cpu.mxcsr = 0xdfc0;
        for (unsigned i = 0; i < 8; ++i) {
            cpu.fpu.regs[i] = {0x8000000000000000ull + i, uint16_t(0x3fff + i)};
            cpu.fpu.tags[i] = (i % 2) ? 3 : 0;
        }
        for (unsigned i = 0; i < 16; ++i) cpu.xmm[i] = {100 + i, 200 + i};
        auto saved = boxedvn::saveGuestFxState64(cpu);
        CHECK_EQ(saved.tag, 0x55);
        CHECK_EQ(saved.st[0].lo, cpu.fpu.regs[top].signif);
        CHECK_EQ(saved.st[7].hi, cpu.fpu.regs[(top + 7) & 7].signExp);
        CHECK_EQ(saved.xmm[15].hi, 215ull);
        CHECK_EQ(saved.padding[48], 0); // no fictitious XSAVE magic

        // Simulate handler clobbers followed by a Wine context edit.
        cpu = {};
        saved.xmm[6].lo = 0xdeadbeef;
        boxedvn::restoreGuestFxState64(cpu, saved);
        CHECK_EQ(cpu.xmm[6].lo, 0xdeadbeefull);
        CHECK_EQ(cpu.mxcsr, 0xdfc0u);
        CHECK_EQ(cpu.fpu.regs[5].signif, 0x8000000000000005ull);
        CHECK_EQ(cpu.fpu.tags[5], 3);
        CHECK_EQ(cpu.fpu.status, saved.status);
    }
}

BOXEDVN_TEST(fex_x87_top_is_one_three_bit_byte) {
    for (unsigned status = 0; status <= 0xffff; ++status) {
        std::array<uint8_t, 48> flags{};
        flags[44] = 0x5a;
        flags[45] = 0xa5;
        boxedvn::setFexX87Status(flags.data(), uint16_t(status));
        CHECK_EQ(flags[43], (status >> 11) & 7);
        CHECK_EQ(flags[44], 0x5a);
        CHECK_EQ(flags[45], 0xa5);
        CHECK_EQ(boxedvn::fexX87Status(flags.data()), status);
    }
}

BOXEDVN_TEST(signal64_reconstructs_full_x87_tags) {
    CHECK_EQ(boxedvn::guestX87Tag(0, 0x8000, true), 1); // negative zero
    CHECK_EQ(boxedvn::guestX87Tag(1, 0, true), 2); // denormal
    CHECK_EQ(boxedvn::guestX87Tag(0x8000000000000000ull, 0x7fff, true), 2);
    CHECK_EQ(boxedvn::guestX87Tag(0x8000000000000000ull, 0x3fff, true), 0);
    CHECK_EQ(boxedvn::guestX87Tag(1, 0x3fff, true), 2); // unnormal
    CHECK_EQ(boxedvn::guestX87Tag(0, 0, false), 3);
}

BOXEDVN_TEST(signal64_xsave_preserves_handler_edits_and_initial_components) {
    TestCpu cpu;
    cpu.signalXsave = true;
    cpu.mxcsr = 0x1f80;
    cpu.ymmHigh[15] = {123, 456};
    auto saved = boxedvn::saveGuestSignalFpState64(cpu);
    const auto info = boxedvn::guestSignalXstateInfo64(saved.legacy);
    CHECK_EQ(info.magic, boxedvn::signal64XstateMagic1);
    CHECK_EQ(info.extendedSize, 836u);
    CHECK_EQ(info.stateSize, 832u);
    CHECK_EQ(saved.magic2, boxedvn::signal64XstateMagic2);
    CHECK_EQ(saved.ymmHigh[15].hi, 456ull);
    saved.ymmHigh[15].lo = 789;
    cpu.ymmHigh[15] = {};
    boxedvn::restoreGuestSignalFpState64(cpu, saved, true);
    CHECK_EQ(cpu.ymmHigh[15].lo, 789ull);
    CHECK_EQ(cpu.ymmHigh[15].hi, 456ull);
    saved.features = 0;
    saved.legacy.mxcsr = 0xffff;
    saved.legacy.xmm[0] = {111, 222};
    boxedvn::restoreGuestSignalFpState64(cpu, saved, true);
    CHECK_EQ(cpu.ymmHigh[15].lo, 0ull);
    CHECK_EQ(cpu.xmm[0].lo, 0ull);
    CHECK_EQ(cpu.fpu.control, 0x37f);
    CHECK_EQ(cpu.mxcsr, 0x1f80u);
    cpu.signalXsave = false;
    saved = boxedvn::saveGuestSignalFpState64(cpu);
    CHECK_EQ(boxedvn::guestSignalXstateInfo64(saved.legacy).magic, 0u);
}

BOXEDVN_TEST(signal64_mxcsr_restores_arm_rounding_without_losing_other_bits) {
    constexpr unsigned armModes[] = {0, 2, 1, 3};
    for (unsigned rc = 0; rc < 4; ++rc) {
        const auto fpcr = boxedvn::guestMxcsrToArmFpcr(0x40000002, 0x9fc0 | (rc << 13));
        CHECK_EQ((fpcr >> 22) & 3, armModes[rc]);
        CHECK_EQ((fpcr >> 24) & 1, 1ull);
        CHECK_EQ(fpcr & 0x40000003, 0x40000003ull);
    }
    CHECK_EQ(boxedvn::guestMxcsrToArmFpcr(0, 0x1fc0) & 1, 0ull);
}
