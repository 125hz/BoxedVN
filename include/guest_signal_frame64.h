/* BoxedWine Linux x86-64 signal/FXSAVE layout. GPLv2. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace boxedvn {
// The handler grows DOWN from restorer. All kernel-owned payloads must be
// above it. The frame base is 64-aligned; SysV function entry RSP is 8 mod 16.
constexpr uint64_t signal64RestorerOffset = 8;
constexpr uint64_t signal64UcontextOffset = 16;
constexpr uint64_t signal64SiginfoOffset = 448;
constexpr uint64_t signal64FpOffset = 576;
constexpr uint64_t signal64FrameSize = 1472;

inline uint64_t signal64FrameBase(uint64_t rsp, uint64_t alt, uint64_t size,
                                  bool useAlt) {
    const bool onAlt = size && rsp >= alt && rsp - alt < size;
    const uint64_t top = useAlt && alt && size && !onAlt ? alt + size : rsp - 128;
    return (top - signal64FrameSize) & ~uint64_t{63};
}

struct GuestFxState64 {
    uint16_t control, status;
    uint8_t tag, reserved;
    uint16_t opcode;
    uint64_t ip, dp;
    uint32_t mxcsr, mxcsrMask;
    struct Slot { uint64_t lo, hi; } st[8], xmm[16];
    uint8_t padding[96];
};
static_assert(sizeof(GuestFxState64) == 512);
static_assert(offsetof(GuestFxState64, st) == 32);
static_assert(offsetof(GuestFxState64, xmm) == 160);
static_assert(signal64UcontextOffset + 296 + 128 <= signal64SiginfoOffset);
static_assert(signal64SiginfoOffset + 128 <= signal64FpOffset);
static_assert(signal64FpOffset + sizeof(GuestFxState64) <= signal64FrameSize);

// CPU64 and its test fixture expose the same architectural state. FXSAVE
// stores ST(i) in logical order, but its abridged tag bits name physical R(i).
template<class Cpu> GuestFxState64 saveGuestFxState64(Cpu& cpu) {
    GuestFxState64 out{};
    out.control = static_cast<uint16_t>(cpu.fpu.CW());
    out.status = static_cast<uint16_t>(cpu.fpu.SW());
    out.mxcsr = cpu.mxcsr;
    out.mxcsrMask = 0xffff;
    const unsigned top = (out.status >> 11) & 7;
    for (unsigned i = 0; i < 8; ++i) {
        if (cpu.fpu.tags[i] != 3) out.tag |= uint8_t(1u << i);
        const unsigned physical = (top + i) & 7;
        const auto& reg = cpu.fpu.getReg(physical);
        out.st[i] = {reg.signif, reg.signExp};
    }
    for (unsigned i = 0; i < 16; ++i) out.xmm[i] = {cpu.xmm[i].lo, cpu.xmm[i].hi};
    return out;
}

inline uint8_t guestX87Tag(uint64_t significand, uint16_t signExp, bool present) {
    if (!present) return 3;
    const auto exponent = signExp & 0x7fff;
    if (exponent == 0 && significand == 0) return 1;
    if (exponent == 0 || exponent == 0x7fff || !(significand >> 63)) return 2;
    return 0;
}

template<class Cpu> void restoreGuestFxState64(Cpu& cpu, const GuestFxState64& in) {
    cpu.fpu.SetCW(in.control);
    cpu.fpu.SetSW(in.status);
    cpu.mxcsr = in.mxcsr & 0xffff;
    const unsigned top = (in.status >> 11) & 7;
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned physical = (top + i) & 7;
        cpu.fpu.regs[physical].signif = in.st[i].lo;
        cpu.fpu.regs[physical].signExp = static_cast<uint16_t>(in.st[i].hi);
        cpu.fpu.tags[physical] = guestX87Tag(in.st[i].lo,
            static_cast<uint16_t>(in.st[i].hi), (in.tag & (1u << physical)) != 0);
        cpu.fpu.isRegCached[physical] = false;
    }
    cpu.fpu.isMMXInUse = false;
    for (unsigned i = 0; i < 16; ++i) {
        cpu.xmm[i].lo = in.xmm[i].lo;
        cpu.xmm[i].hi = in.xmm[i].hi;
    }
}

inline uint64_t guestMxcsrToArmFpcr(uint64_t fpcr, uint32_t mxcsr) {
    // ARM RC: nearest,+inf,-inf,zero; x86 RC: nearest,-inf,+inf,zero.
    fpcr &= ~(uint64_t{7} << 22);
    fpcr |= uint64_t((mxcsr >> 14) & 1) << 22;
    fpcr |= uint64_t((mxcsr >> 13) & 1) << 23;
    fpcr |= uint64_t((mxcsr >> 15) & 1) << 24;
    // FEX enables AH only on hosts with AFP; FIZ is reserved otherwise.
    if (fpcr & 2) fpcr = (fpcr & ~uint64_t{1}) | ((mxcsr >> 6) & 1);
    return fpcr;
}

// Linux x86 signal ABI: a standard (non-compacted) XSAVE area followed by
// MAGIC2. Bytes 464..511 of the legacy area describe this extension to Wine.
constexpr uint32_t signal64XstateMagic1 = 0x46505853;
constexpr uint32_t signal64XstateMagic2 = 0x46505845;
struct GuestXstateInfo64 {
    uint32_t magic, extendedSize;
    uint64_t features;
    uint32_t stateSize, reserved[7];
};
struct GuestSignalFpState64 {
    GuestFxState64 legacy;
    uint64_t features, compaction, reserved[6];
    GuestFxState64::Slot ymmHigh[16];
    uint32_t magic2;
};
static_assert(sizeof(GuestXstateInfo64) == 48);
static_assert(offsetof(GuestSignalFpState64, features) == 512);
static_assert(offsetof(GuestSignalFpState64, ymmHigh) == 576);
static_assert(offsetof(GuestSignalFpState64, magic2) == 832);
static_assert(signal64FpOffset + sizeof(GuestSignalFpState64) <= signal64FrameSize);

inline GuestXstateInfo64 guestSignalXstateInfo64(const GuestFxState64& fp) {
    GuestXstateInfo64 info{};
    std::memcpy(&info, fp.padding + 48, sizeof(info));
    return info;
}

template<class Cpu> GuestSignalFpState64 saveGuestSignalFpState64(Cpu& cpu) {
    GuestSignalFpState64 out{};
    out.legacy = saveGuestFxState64(cpu);
    if (cpu.signalXsave) {
        const GuestXstateInfo64 info{signal64XstateMagic1, 836, 7, 832, {}};
        std::memcpy(out.legacy.padding + 48, &info, sizeof(info));
        out.features = 7; // x87, SSE, AVX; never advertise unsupported state.
        out.magic2 = signal64XstateMagic2;
        for (unsigned i = 0; i < 16; ++i)
            out.ymmHigh[i] = {cpu.ymmHigh[i].lo, cpu.ymmHigh[i].hi};
    }
    return out;
}

template<class Cpu> void restoreGuestSignalFpState64(
    Cpu& cpu, GuestSignalFpState64 in, bool extended) {
    if (extended) {
        // An absent XSAVE component denotes its architectural initial state.
        if (!(in.features & 1)) {
            in.legacy.control = 0x37f;
            in.legacy.status = 0;
            in.legacy.tag = 0;
            for (auto& reg : in.legacy.st) reg = {};
        }
        if (!(in.features & 2)) {
            in.legacy.mxcsr = 0x1f80;
            for (auto& reg : in.legacy.xmm) reg = {};
        }
    }
    restoreGuestFxState64(cpu, in.legacy);
    for (unsigned i = 0; i < 16; ++i) {
        const auto value = extended && (in.features & 4)
            ? in.ymmHigh[i] : GuestFxState64::Slot{};
        cpu.ymmHigh[i].lo = value.lo;
        cpu.ymmHigh[i].hi = value.hi;
    }
}
} // namespace boxedvn
