#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace boxedvn {
// A leaf AArch64 replacement for SWPAL Xs,Xt,[Xn]. The fast path updates an
// unaligned qword contained within one aligned 16-byte block using CASPAL.
// Other alignments execute the original instruction (and retain FEX's slow
// split-access handler). No C call, SIMD state change, or weakened atomic.
struct FexSwapThunk {
    static constexpr size_t stackBytes = 128;
    static constexpr size_t slotBytes = 512;
    std::array<uint32_t, slotBytes / 4> words{};
    size_t count = 0, load = 0, cas = 0, fallback = 0;

    static constexpr bool accepts(uint32_t op) {
        return (op & 0xffe0fc00u) == 0xf8e08000u &&
               ((op >> 5) & 31) != 31; // SP addressing is not emitted by FEX.
    }
    static constexpr uint32_t branch(uintptr_t from, uintptr_t to) {
        const auto delta = static_cast<int64_t>(to) - static_cast<int64_t>(from);
        return (delta & 3) || delta < -(1ll << 27) || delta >= (1ll << 27)
            ? 0 : 0x14000000u | (static_cast<uint32_t>(delta / 4) & 0x3ffffff);
    }
    void emit(uint32_t op) { words[count++] = op; }
    void originalRegister(unsigned dst, unsigned src) {
        if (src < 12) emit(0xf94003e0u | ((src * 8 / 8) << 10) | dst);
        else emit(0xaa0003e0u | (src << 16) | dst); // MOV (also XZR)
    }
    void restore() {
        emit(0xf94033ebu); // ldr x11,[sp,#96]
        emit(0xd51b420bu); // msr nzcv,x11
        for (unsigned r = 0; r < 12; r += 2)
            emit(0xa9400000u | (r << 15) | ((r + 1) << 10) | (31 << 5) | r);
        emit(0x910203ffu); // add sp,sp,#128
    }
    bool build(uint32_t op, uintptr_t rx, uintptr_t resume) {
        if (!accepts(op) || !branch(rx, resume)) return false;
        const unsigned rn = (op >> 5) & 31, rs = (op >> 16) & 31, rt = op & 31;
        emit(0xd10203ffu); // sub sp,sp,#128
        for (unsigned r = 0; r < 12; r += 2)
            emit(0xa9000000u | (r << 15) | ((r + 1) << 10) | (31 << 5) | r);
        emit(0xd53b420bu); // mrs x11,nzcv
        emit(0xf90033ebu); // str x11,[sp,#96]
        originalRegister(4, rn);
        originalRegister(5, rs);
        emit(0x92400c86u); // and x6,x4,#15
        const size_t aligned = count; emit(0); // cbz x6,fallback_restore
        emit(0xf1001cdfu); // cmp x6,#7
        const size_t split = count; emit(0); // b.hi fallback_restore
        emit(0x927cec84u); // and x4,x4,#~15
        emit(0xd37df0c6u); // lsl x6,x6,#3
        emit(0x92800007u); // mov x7,#-1
        emit(0x9ac620e8u); // lsl x8,x7,x6 (low mask)
        emit(0xcb0603e9u); // neg x9,x6 (64-shift modulo 64)
        emit(0x9ac924e7u); // lsr x7,x7,x9 (high mask)
        emit(0x9ac620aau); // lsl x10,x5,x6 (new low)
        emit(0x9ac924a5u); // lsr x5,x5,x9 (new high)
        load = count; emit(0xa9400480u); // ldp x0,x1,[x4]
        const size_t loop = count;
        emit(0x8a280002u); // bic x2,x0,x8
        emit(0xaa0a0042u); // orr x2,x2,x10
        emit(0x8a270023u); // bic x3,x1,x7
        emit(0xaa050063u); // orr x3,x3,x5
        emit(0xa90707e0u); // stp x0,x1,[sp,#112] (expected)
        cas = count; emit(0x4860fc82u); // caspal x0,x1,x2,x3,[x4]
        emit(0xf9403bebu); // ldr x11,[sp,#112]
        emit(0xeb0b001fu); // cmp x0,x11
        emit(0x54000001u | ((static_cast<uint32_t>(loop - count) & 0x7ffff) << 5));
        emit(0xf9403febu); // ldr x11,[sp,#120]
        emit(0xeb0b003fu); // cmp x1,x11
        emit(0x54000001u | ((static_cast<uint32_t>(loop - count) & 0x7ffff) << 5));
        emit(0x9ac62400u); // lsr x0,x0,x6
        emit(0x9ac92021u); // lsl x1,x1,x9
        emit(0xaa010000u); // orr x0,x0,x1 (old qword)
        if (rt < 12) emit(0xf90003e0u | (rt << 10)); // save returned register
        else if (rt != 31) emit(0xaa0003e0u | rt);
        restore();
        emit(branch(rx + count * 4, resume));
        const size_t slow = count;
        words[aligned] = 0xb4000006u | (static_cast<uint32_t>(slow - aligned) << 5);
        words[split] = 0x54000008u | (static_cast<uint32_t>(slow - split) << 5);
        restore();
        fallback = count; emit(op);
        emit(branch(rx + count * 4, resume));
        return true;
    }
};
}
